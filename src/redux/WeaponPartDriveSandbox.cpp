#include "redux/WeaponPartDriveSandbox.h"

#include "api/ROCKProviderApi.h"
#include "ReduxLog.h"
#include "redux/WeaponPartMotionLearner.h"
#include "redux/WeaponPartMotionScrubPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace redux
{
    namespace
    {
        constexpr char kSandboxConsumerName[] = "PAPER_Redux";
        constexpr std::uint32_t kRegistrationRetryFrames = 300;
        constexpr std::uint32_t kDriveLeaseFrames = 2;
        constexpr std::uint32_t kDrivePriority = 10;

        /*
         * Clip-scrub pursuit controller tuning (weapon-root-local units,
         * per-frame steps at the provider frame rate). Deliberately
         * file-local constants, not INI — promote whichever ones Bruno's
         * A/B feel testing actually needs.
         */
        // Fraction correction per unit of projected part error, after slope
        // normalization (1.0 would try to close the whole error in one frame).
        constexpr float kScrubPursuitGain = 0.35f;
        // Hard per-frame fraction step cap: full clip in >= ~0.4s at 90fps.
        constexpr float kScrubMaxFractionPerFrame = 0.03f;
        // Below this observed travel per unit fraction the direction
        // estimate is degenerate (dwell window / bootstrap).
        constexpr float kScrubMinSlopeUnitsPerFraction = 0.05f;
        // Dwell crawl: slow forward advance while the estimate is
        // degenerate AND the hand has really pulled away from its anchor.
        constexpr float kScrubCrawlFractionPerFrame = 0.005f;
        constexpr float kScrubCrawlHandDeadzoneUnits = 1.0f;
        // Projected-error deadzone so hand tremor cannot dither the time.
        constexpr float kScrubErrorDeadzoneUnits = 0.15f;
        // Minimum observed deltas for a trustworthy slope sample.
        constexpr float kScrubMinFractionDelta = 1.0e-4f;
        constexpr float kScrubMinTravelDelta = 1.0e-3f;

        // Magazine free-movement: periodic free-state diagnostic cadence
        // (frames) — logs part delta / path distance / arming so a failing
        // detach or capture geometry is readable from one session log.
        constexpr std::uint32_t kMagFreeDiagnosticFrames = 90;

        [[nodiscard]] weapon_part_motion_path::Vec3 vecSub(
            const weapon_part_motion_path::Vec3& a, const weapon_part_motion_path::Vec3& b)
        {
            return weapon_part_motion_path::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
        }

        [[nodiscard]] float vecDot(
            const weapon_part_motion_path::Vec3& a, const weapon_part_motion_path::Vec3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        [[nodiscard]] float vecLength(const weapon_part_motion_path::Vec3& v)
        {
            return std::sqrt(vecDot(v, v));
        }

        // Quat is {w, x, y, z}; helpers for the hand-local free attachment.
        [[nodiscard]] weapon_part_motion_path::Quat quatMul(
            const weapon_part_motion_path::Quat& a, const weapon_part_motion_path::Quat& b)
        {
            return weapon_part_motion_path::Quat{
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            };
        }

        [[nodiscard]] weapon_part_motion_path::Quat quatConjugate(const weapon_part_motion_path::Quat& q)
        {
            return weapon_part_motion_path::Quat{ q.w, -q.x, -q.y, -q.z };
        }

        [[nodiscard]] weapon_part_motion_path::Vec3 quatRotateVec(
            const weapon_part_motion_path::Quat& q, const weapon_part_motion_path::Vec3& v)
        {
            // v' = v + 2*u x (u x v + w*v), u = (x,y,z).
            const weapon_part_motion_path::Vec3 u{ q.x, q.y, q.z };
            const weapon_part_motion_path::Vec3 t{
                2.0f * (u.y * v.z - u.z * v.y),
                2.0f * (u.z * v.x - u.x * v.z),
                2.0f * (u.x * v.y - u.y * v.x),
            };
            return weapon_part_motion_path::Vec3{
                v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
                v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
                v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
            };
        }

        // Row-major 3x3 from a unit quaternion {w,x,y,z}; matches the
        // fillProviderTransform flattening consumed by providerTransformToNi.
        void quatToRotateRowMajor(const weapon_part_motion_path::Quat& q, float outRotate[9])
        {
            const auto n = weapon_part_motion_path::quatNormalizeOrIdentity(q);
            const float xx = n.x * n.x, yy = n.y * n.y, zz = n.z * n.z;
            const float xy = n.x * n.y, xz = n.x * n.z, yz = n.y * n.z;
            const float wx = n.w * n.x, wy = n.w * n.y, wz = n.w * n.z;
            outRotate[0] = 1.0f - 2.0f * (yy + zz);
            outRotate[1] = 2.0f * (xy - wz);
            outRotate[2] = 2.0f * (xz + wy);
            outRotate[3] = 2.0f * (xy + wz);
            outRotate[4] = 1.0f - 2.0f * (xx + zz);
            outRotate[5] = 2.0f * (yz - wx);
            outRotate[6] = 2.0f * (xz - wy);
            outRotate[7] = 2.0f * (yz + wx);
            outRotate[8] = 1.0f - 2.0f * (xx + yy);
        }

        std::string_view sessionName(const std::array<char, WeaponPartDriveSandbox::kMaxSourceName>& name)
        {
            std::size_t length = 0;
            while (length < name.size() && name[length] != '\0') {
                ++length;
            }
            return std::string_view(name.data(), length);
        }

        [[nodiscard]] std::int32_t findChainLink(
            const WeaponPartDriveSandbox::FrameInput& input,
            std::string_view name)
        {
            for (std::uint32_t i = 0; i < input.chainLinkCount; ++i) {
                if (sessionName(input.chainLinks[i].name) == name) {
                    return static_cast<std::int32_t>(i);
                }
            }
            return -1;
        }

        // True when ancestorName is a scene-graph ancestor of nodeName per
        // the chain table (walks nearest-cached-ancestor links; bounded by
        // the table size, so a malformed cycle terminates).
        [[nodiscard]] bool chainIsAncestor(
            const WeaponPartDriveSandbox::FrameInput& input,
            std::string_view ancestorName,
            std::string_view nodeName)
        {
            if (ancestorName.empty() || nodeName.empty() || ancestorName == nodeName) {
                return false;
            }
            auto index = findChainLink(input, nodeName);
            for (std::uint32_t hops = 0; index >= 0 && hops < input.chainLinkCount; ++hops) {
                const auto parent = sessionName(input.chainLinks[index].parentName);
                if (parent.empty()) {
                    return false;
                }
                if (parent == ancestorName) {
                    return true;
                }
                index = findChainLink(input, parent);
            }
            return false;
        }

        /*
         * Drive-time chain filter (Bruno's 2x bug, 2026-07-05): decide which
         * of the session's followers may be DRIVEN alongside the leader.
         * Children ride their parents natively, so driving two nodes of one
         * parent chain stacks displacement on the descendant — the gripped
         * part traveled 2x once full-subtree observation adopted hierarchy
         * nodes as rigid followers. Rules: any follower that is an ancestor
         * OR descendant of the leader is suppressed (the leader always
         * wins), and among the rest only chain-topmost nodes are kept.
         * Filtering here (not at learn/import time) covers every source —
         * fresh learning, authored strokes, and already-saved library data.
         * Unknown names (no chain entry) pass through untouched.
         */
        std::uint32_t chainFilterFollowers(
            const WeaponPartDriveSandbox::FrameInput& input,
            std::string_view leaderName,
            const weapon_clip_stroke::AuthoredFollower* followers,
            std::uint32_t followerCount,
            std::array<bool, weapon_clip_stroke::kMaxFollowers>& outKeep)
        {
            std::uint32_t kept = 0;
            std::array<std::string_view, weapon_clip_stroke::kMaxFollowers> names{};
            for (std::uint32_t i = 0; i < followerCount && i < outKeep.size(); ++i) {
                names[i] = sessionName(followers[i].boneName);
                outKeep[i] = !names[i].empty() &&
                    !chainIsAncestor(input, names[i], leaderName) &&
                    !chainIsAncestor(input, leaderName, names[i]);
            }
            for (std::uint32_t i = 0; i < followerCount && i < outKeep.size(); ++i) {
                if (!outKeep[i]) {
                    continue;
                }
                for (std::uint32_t j = 0; j < followerCount && j < outKeep.size(); ++j) {
                    // An in-set ancestor carries this follower already; the
                    // ancestor stays (topmost wins), the descendant drops.
                    if (i != j && outKeep[j] && chainIsAncestor(input, names[j], names[i])) {
                        outKeep[i] = false;
                        break;
                    }
                }
                if (outKeep[i]) {
                    ++kept;
                }
            }
            return kept;
        }
    }

    bool WeaponPartDriveSandbox::ensureRegistered()
    {
        if (_ownerToken != 0) {
            return true;
        }
        if (_registrationRetryCooldownFrames > 0) {
            --_registrationRetryCooldownFrames;
            return false;
        }

        // External consumer: the table pointer comes from
        // RockProviderApi::initialize() (GetProcAddress on ROCK.dll), never
        // from the dllimport symbol — PAPER_Redux does not link ROCK.lib.
        const auto* api = ::rock::provider::RockProviderApi::inst;
        if (!api || !api->registerConsumerV1 || !api->setWeaponPartTargetsV1) {
            return false;
        }

        ::rock::provider::RockProviderConsumerRegistrationV1 registration{};
        std::memcpy(registration.modName, kSandboxConsumerName, sizeof(kSandboxConsumerName));
        registration.requestedCapabilities =
            static_cast<std::uint32_t>(::rock::provider::RockProviderConsumerCapabilityV1::WeaponPartInteraction);
        ::rock::provider::RockProviderConsumerHandleV1 handle{};
        const auto result = api->registerConsumerV1(&registration, &handle);
        if (result != ::rock::provider::RockProviderResultV1::Ok || handle.ownerToken == 0) {
            if (!_registrationWarned) {
                RDX_LOG_WARN(Weapon, "WeaponPartDriveSandbox: consumer registration failed result={}", static_cast<std::uint32_t>(result));
                _registrationWarned = true;
            }
            _registrationRetryCooldownFrames = kRegistrationRetryFrames;
            return false;
        }
        _ownerToken = handle.ownerToken;
        _installedTargets = {};
        RDX_LOG_INFO(Weapon,
            "WeaponPartDriveSandbox: consumer registered (token={}); per-part AttachOnly targets follow the resolved eligible set",
            _ownerToken);
        return true;
    }

    void WeaponPartDriveSandbox::ensureTargetsInstalled(const FrameInput& input)
    {
        const auto count = (std::min)(input.eligiblePartCount, static_cast<std::uint32_t>(kMaxEligibleParts));
        // Change detection: same generation and same bodyId set means the
        // installed targets are already correct (the common per-frame case).
        if (_installedTargets.any &&
            _installedTargets.weaponGenerationKey == input.weaponGenerationKey &&
            _installedTargets.count == count) {
            bool same = true;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (_installedTargets.bodyIds[i] != input.eligibleParts[i].bodyId) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return;
            }
        }
        if (!_installedTargets.any && count == 0) {
            return;
        }

        const auto* api = ::rock::provider::RockProviderApi::inst;
        if (!api || !api->setWeaponPartTargetsV1 || !api->clearWeaponPartTargetsV1) {
            return;
        }

        if (count == 0) {
            (void)api->clearWeaponPartTargetsV1(_ownerToken);
            _installedTargets = {};
            RDX_LOG_INFO(Weapon, "WeaponPartDriveSandbox: attach-only targets cleared (no eligible moving parts)");
            return;
        }

        /*
         * Per-part whitelist: one NonExclusive AttachOnly target per
         * eligible part, matched by bodyId and pinned to the weapon
         * generation. Parts of the same weapon that are NOT in this set —
         * allowlisted classes without motion data included — never match a
         * target and keep their normal grip behavior; a weapon swap
         * invalidates everything through the generation key until the
         * runtime resolves the new weapon's set.
         */
        std::array<::rock::provider::RockProviderWeaponPartTargetV1, kMaxEligibleParts> targets{};
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& target = targets[i];
            target.flags = static_cast<std::uint32_t>(::rock::provider::RockProviderWeaponPartTargetFlagV1::NonExclusive) |
                           static_cast<std::uint32_t>(::rock::provider::RockProviderWeaponPartTargetFlagV1::MatchBodyId);
            target.grabMode = ::rock::provider::RockProviderWeaponPartGrabModeV1::AttachOnly;
            target.weaponGenerationKey = input.weaponGenerationKey;
            target.bodyId = input.eligibleParts[i].bodyId;
            target.groupId = 1;
            target.priority = kDrivePriority;
        }
        const auto targetResult = api->setWeaponPartTargetsV1(_ownerToken, targets.data(), count);
        if (targetResult != ::rock::provider::RockProviderResultV1::Ok) {
            if (targetResult == ::rock::provider::RockProviderResultV1::OwnerNotRegistered) {
                // Provider dropped this owner (ROCK reset); re-register from
                // scratch on the next update instead of retrying a dead token.
                _ownerToken = 0;
            }
            _installedTargets = {};
            RDX_LOG_WARN(Weapon,
                "WeaponPartDriveSandbox: per-part target install failed result={} (count={})",
                static_cast<std::uint32_t>(targetResult),
                count);
            return;
        }

        _installedTargets.any = true;
        _installedTargets.weaponGenerationKey = input.weaponGenerationKey;
        _installedTargets.count = count;
        for (std::uint32_t i = 0; i < count; ++i) {
            _installedTargets.bodyIds[i] = input.eligibleParts[i].bodyId;
        }

        // Compact one-line set dump — this is the live answer to "why is
        // this part (not) attach-only": exactly these parts glue.
        std::array<char, 512> names{};
        std::size_t nameLength = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto sourceName = sessionName(input.eligibleParts[i].sourceName);
            const std::size_t needed = sourceName.size() + 1;
            if (nameLength + needed >= names.size()) {
                break;
            }
            if (nameLength > 0) {
                names[nameLength++] = ' ';
            }
            std::memcpy(names.data() + nameLength, sourceName.data(), sourceName.size());
            nameLength += sourceName.size();
        }
        RDX_LOG_INFO(Weapon,
            "WeaponPartDriveSandbox: attach-only targets installed for {} moving part(s) gen={:#x}: [{}]",
            count,
            input.weaponGenerationKey,
            std::string_view(names.data(), nameLength));
    }

    void WeaponPartDriveSandbox::endSession(HandSession& session)
    {
        session = {};
    }

    std::uint32_t WeaponPartDriveSandbox::update(
        const FrameInput& input,
        const WeaponPartMotionLearner& learner,
        SentDrive* outSentDrives,
        MaxTravelEvent* outMaxTravelEvents,
        std::uint32_t* outMaxTravelEventCount,
        float* outClipScrubFraction,
        bool* outClipScrubFractionValid)
    {
        if (outMaxTravelEventCount) {
            *outMaxTravelEventCount = 0;
        }
        if (outClipScrubFractionValid) {
            *outClipScrubFractionValid = false;
        }
        if (!ensureRegistered()) {
            _sessions = {};
            _sentDrivesLastUpdate = false;
            return 0;
        }
        ensureTargetsInstalled(input);

        // Per hand: the gripped leader plus up to kMaxFollowers assembly parts.
        std::array<::rock::provider::RockProviderWeaponPartDriveTargetV1, 2 * (1 + weapon_clip_stroke::kMaxFollowers)> drives{};
        std::uint32_t driveCount = 0;

        for (std::size_t handIndex = 0; handIndex < 2; ++handIndex) {
            const auto& hand = input.hands[handIndex];
            auto& session = _sessions[handIndex];

            if (!hand.gripActive || input.weaponFormId == 0 || input.weaponGenerationKey == 0) {
                /*
                 * Park-at-rest on release (Bruno 2026-07-06: parts were
                 * staying at their extreme positions): one explicit final
                 * drive at the pinned rest pose instead of relying on the
                 * provider's baseline restore — deterministic regardless of
                 * what baseline ROCK captured, and it covers the free-moving
                 * state where the part may be far off its path. The short
                 * lease expires on its own, so an engine animation reasserts
                 * right after. Skipped for clip-scrub sessions (the engine
                 * owns the pose) and when the weapon generation moved (the
                 * target would be stale).
                 */
                if (session.active && !session.clipScrub && session.restPoseValid &&
                    session.weaponGenerationKey == input.weaponGenerationKey &&
                    input.weaponFormId != 0 && driveCount < drives.size()) {
                    bool parkDuplicate = false;
                    for (std::uint32_t i = 0; i < driveCount; ++i) {
                        if (drives[i].bodyId == session.bodyId) {
                            parkDuplicate = true;
                            break;
                        }
                    }
                    if (!parkDuplicate) {
                        auto& drive = drives[driveCount++];
                        drive.flags = static_cast<std::uint32_t>(
                            ::rock::provider::RockProviderWeaponPartTargetFlagV1::MatchBodyId);
                        drive.driveSpace = ::rock::provider::RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
                        drive.weaponGenerationKey = session.weaponGenerationKey;
                        drive.bodyId = session.bodyId;
                        drive.groupId = static_cast<std::uint32_t>(handIndex + 1);
                        drive.priority = kDrivePriority;
                        drive.leaseFrames = kDriveLeaseFrames;
                        quatToRotateRowMajor(session.restPose.rotate, drive.targetTransform.rotate);
                        drive.targetTransform.translate[0] = session.restPose.translate.x;
                        drive.targetTransform.translate[1] = session.restPose.translate.y;
                        drive.targetTransform.translate[2] = session.restPose.translate.z;
                        drive.targetTransform.scale = session.partScale;
                        RDX_LOG_INFO(Weapon,
                            "WeaponPartDriveSandbox: release-park part='{}' hand={} -> rest pose ({} state)",
                            sessionName(session.sourceName),
                            handIndex == 1 ? "left" : "right",
                            session.freeMoving ? "free" : "guided");
                    }
                }
                endSession(session);
                continue;
            }

            const bool freshGrip = !session.active ||
                session.gripSequence != hand.gripSequence ||
                session.bodyId != hand.bodyId ||
                session.weaponGenerationKey != input.weaponGenerationKey;
            if (freshGrip) {
                endSession(session);
                if (!hand.transformsValid || hand.sourceName.empty()) {
                    continue;
                }
                /*
                 * Trigger arming: the hand is already glued to the part
                 * (ROCK's AttachOnly grip), but the part does not scrub
                 * until the trigger unlocks it. Level semantics — a trigger
                 * already held at grab time unlocks immediately, a later
                 * press unlocks then; the session then seeds from the part
                 * and hand poses AT UNLOCK, so displacement is measured from
                 * the moment manipulation actually starts.
                 */
                if (!hand.triggerHeld) {
                    if (_lastAwaitingUnlockGripSequence[handIndex] != hand.gripSequence) {
                        _lastAwaitingUnlockGripSequence[handIndex] = hand.gripSequence;
                        RDX_LOG_INFO(Weapon,
                            "WeaponPartDriveSandbox: hand={} glued to part '{}' — awaiting trigger unlock",
                            handIndex == 1 ? "left" : "right",
                            hand.sourceName);
                    }
                    continue;
                }
                /*
                 * ClipScrub session start: no path lookup at all — the hand
                 * drives the captured clip's time and the engine poses the
                 * parts. Requires a live captured clip; a grip without one
                 * glues (ROCK's AttachOnly) but cannot drive, and says so
                 * once per grip.
                 */
                if (input.motionPathMode == MotionPathMode::ClipScrub) {
                    if (!input.clipScrubSessionActive) {
                        if (_lastNoPathGripSequence[handIndex] != hand.gripSequence) {
                            _lastNoPathGripSequence[handIndex] = hand.gripSequence;
                            RDX_LOG_INFO(Weapon,
                                "WeaponPartDriveSandbox: scrub grip on '{}' but no reload clip is captured — trigger a reload to scrub",
                                hand.sourceName);
                        }
                        continue;
                    }
                    session.active = true;
                    session.clipScrub = true;
                    session.clipScrubSessionId = input.clipScrubSessionId;
                    session.gripSequence = hand.gripSequence;
                    session.bodyId = hand.bodyId;
                    session.weaponGenerationKey = input.weaponGenerationKey;
                    session.weaponFormId = input.weaponFormId;
                    session.omodFormId = hand.omodFormId;
                    session.mode = input.motionPathMode;
                    session.sourceName = {};
                    std::memcpy(session.sourceName.data(), hand.sourceName.data(),
                        (std::min)(hand.sourceName.size(), session.sourceName.size() - 1));
                    session.handStartTranslate = hand.handTranslate;
                    session.scrubPartStartTranslate = hand.partTranslate;
                    session.scrubPrevPartTranslate = hand.partTranslate;
                    session.scrubPrevFraction = input.clipScrubFraction;
                    session.scrubDirectionValid = false;
                    session.scrubSlope = 0.0f;
                    session.scrubLastLogDecile =
                        static_cast<std::uint32_t>(input.clipScrubFraction * 10.0f);
                    RDX_LOG_INFO(Weapon,
                        "WeaponPartDriveSandbox: CLIP-SCRUB grip hand={} part='{}' fraction={:.3f} (pursuit controller live)",
                        handIndex == 1 ? "left" : "right",
                        hand.sourceName,
                        input.clipScrubFraction);
                    continue;
                }
                const auto group = learner.findGroup(
                    WeaponPartMotionLearner::PartKey{ input.weaponFormId, hand.omodFormId, hand.sourceName },
                    input.motionPathMode);
                if (!group.leaderPath) {
                    if (_lastNoPathGripSequence[handIndex] != hand.gripSequence) {
                        _lastNoPathGripSequence[handIndex] = hand.gripSequence;
                        // Say what DOES exist so a mode mismatch is readable
                        // straight from the log ("authored data present but
                        // mode=learned").
                        const auto available = learner.sourceAvailability(
                            WeaponPartMotionLearner::PartKey{ input.weaponFormId, hand.omodFormId, hand.sourceName });
                        RDX_LOG_INFO(Weapon,
                            "WeaponPartDriveSandbox: no motion path for part '{}' on weapon {:08X} under mode={} (available: learned={} authored={}{})",
                            hand.sourceName,
                            input.weaponFormId,
                            motionPathModeName(input.motionPathMode),
                            available.learned,
                            available.authored,
                            available.authoredFallback ? " [fallback-tier]" : "");
                    }
                    continue;
                }
                const auto seeded = weapon_part_motion_scrub::initialScrubPosition(*group.leaderPath, hand.partTranslate);
                if (!seeded.valid) {
                    continue;
                }
                session.active = true;
                session.gripSequence = hand.gripSequence;
                session.bodyId = hand.bodyId;
                session.weaponGenerationKey = input.weaponGenerationKey;
                session.weaponFormId = input.weaponFormId;
                session.omodFormId = hand.omodFormId;
                session.mode = input.motionPathMode;
                session.sourceName = {};
                std::memcpy(session.sourceName.data(), hand.sourceName.data(), (std::min)(hand.sourceName.size(), session.sourceName.size() - 1));
                session.arcPosition = seeded.arcPosition;
                // Seeding inside the max-travel zone latches WITHOUT
                // emitting — a grab of an already-out part is not a stroke.
                const auto& seedRestReference = hand.restPoseValid ? hand.restPose : group.leaderPath->keys[0];
                // Pinned rest pose: anchors the magazine free-movement
                // radius and the park-at-rest drive emitted on release.
                session.restPoseValid = true;
                session.restPose = seedRestReference;
                const auto seedExtreme =
                    weapon_part_motion_path::travelExtremeFromRest(*group.leaderPath, seedRestReference);
                session.maxTravelLatched = false;
                if (seedExtreme.valid) {
                    const float seedDelta = weapon_part_motion_path::poseDistance(seeded.target, seedRestReference);
                    const float seedRange = seedExtreme.peakDelta - seedExtreme.restDelta;
                    session.maxTravelLatched = seedDelta >=
                        seedExtreme.restDelta + (1.0f - input.travelExtremeToleranceFraction) * seedRange;
                }
                session.handStartTranslate = hand.handTranslate;
                session.pathAnchorTranslate = seeded.target.translate;
                session.partScale = hand.partScale;
                session.followerCount = 0;
                // Learned groups carry co-observed followers now, not only
                // authored clips — drive whichever source provided them.
                if (group.followers) {
                    session.followerCount = (std::min)(group.followerCount, static_cast<std::uint32_t>(session.followers.size()));
                    for (std::uint32_t i = 0; i < session.followerCount; ++i) {
                        session.followers[i] = group.followers[i];
                    }
                }
                std::array<bool, weapon_clip_stroke::kMaxFollowers> keptAtStart{};
                const auto keptCount = chainFilterFollowers(
                    input, hand.sourceName, session.followers.data(), session.followerCount, keptAtStart);
                RDX_LOG_INFO(Weapon,
                    "WeaponPartDriveSandbox: scrub session started hand={} part='{}' arc={:.2f}/{:.2f} mode={} source={} followers={} ({} chain-suppressed) returnStage={} maxTravelArc={:.2f} restArc={:.2f} peakDelta={:.2f} restRef={}",
                    handIndex == 1 ? "left" : "right",
                    hand.sourceName,
                    seeded.arcPosition,
                    group.leaderPath->totalArcLength,
                    motionPathModeName(session.mode),
                    !group.authored ? "runtime-learned" : (group.fallbackSource ? "authored-fallback" : "authored-clip"),
                    session.followerCount,
                    session.followerCount - keptCount,
                    group.returnPath != nullptr,
                    seedExtreme.valid ? seedExtreme.arcPosition : -1.0f,
                    seedExtreme.valid ? seedExtreme.restArcPosition : -1.0f,
                    seedExtreme.valid ? seedExtreme.peakDelta : 0.0f,
                    hand.restPoseValid ? "live-rest" : "path-start");
            }

            if (!session.active || !hand.transformsValid) {
                continue;
            }
            if (session.clipScrub) {
                // The captured clip is the session's substrate: gone (or a
                // NEW capture) means this grip's time authority is over.
                if (!input.clipScrubSessionActive ||
                    input.clipScrubSessionId != session.clipScrubSessionId) {
                    RDX_LOG_INFO(Weapon,
                        "WeaponPartDriveSandbox: CLIP-SCRUB grip ended hand={} part='{}' (clip session gone)",
                        handIndex == 1 ? "left" : "right",
                        sessionName(session.sourceName));
                    endSession(session);
                    continue;
                }
                // One hand owns time per frame (first session wins).
                if (outClipScrubFractionValid && *outClipScrubFractionValid) {
                    continue;
                }
                const float fraction = input.clipScrubFraction;

                /*
                 * Slope/direction estimate from what the engine-posed part
                 * actually did since the last sample: dP/df observed in the
                 * scene graph — the engine is the curve oracle, so the
                 * broken authored basis conversion is never involved. The
                 * direction carries the fraction sign, so a positive
                 * projected error always means "advance time".
                 */
                const auto partDelta = vecSub(hand.partTranslate, session.scrubPrevPartTranslate);
                const float fractionDelta = fraction - session.scrubPrevFraction;
                const float travelDelta = vecLength(partDelta);
                if (std::fabs(fractionDelta) > kScrubMinFractionDelta && travelDelta > kScrubMinTravelDelta) {
                    const float slope = travelDelta / std::fabs(fractionDelta);
                    if (slope > kScrubMinSlopeUnitsPerFraction) {
                        const float sign = fractionDelta > 0.0f ? 1.0f : -1.0f;
                        session.scrubDirection = weapon_part_motion_path::Vec3{
                            partDelta.x / travelDelta * sign,
                            partDelta.y / travelDelta * sign,
                            partDelta.z / travelDelta * sign,
                        };
                        session.scrubSlope = slope;
                        session.scrubDirectionValid = true;
                    }
                }
                session.scrubPrevPartTranslate = hand.partTranslate;
                session.scrubPrevFraction = fraction;

                // The part chases the hand's displaced target, exactly the
                // attach fantasy the learned scrub delivers by driving the
                // node — here delivered by steering time instead.
                const auto handDisplacement = vecSub(hand.handTranslate, session.handStartTranslate);
                const weapon_part_motion_path::Vec3 targetPart{
                    session.scrubPartStartTranslate.x + handDisplacement.x,
                    session.scrubPartStartTranslate.y + handDisplacement.y,
                    session.scrubPartStartTranslate.z + handDisplacement.z,
                };
                const auto error = vecSub(targetPart, hand.partTranslate);

                float fractionStep = 0.0f;
                if (session.scrubDirectionValid && session.scrubSlope > kScrubMinSlopeUnitsPerFraction) {
                    const float projectedError = vecDot(error, session.scrubDirection);
                    if (std::fabs(projectedError) > kScrubErrorDeadzoneUnits) {
                        fractionStep = kScrubPursuitGain * projectedError / session.scrubSlope;
                    }
                } else if (vecLength(handDisplacement) > kScrubCrawlHandDeadzoneUnits) {
                    // Bootstrap / dwell crawl: no usable direction yet, but
                    // the hand is really pulling — creep time forward until
                    // the part responds and the estimator takes over.
                    fractionStep = kScrubCrawlFractionPerFrame;
                }
                fractionStep = std::clamp(fractionStep, -kScrubMaxFractionPerFrame, kScrubMaxFractionPerFrame);
                const float desired = std::clamp(fraction + fractionStep, 0.0f, 1.0f);
                if (outClipScrubFraction && outClipScrubFractionValid) {
                    *outClipScrubFraction = desired;
                    *outClipScrubFractionValid = true;
                }
                const auto decile = static_cast<std::uint32_t>(fraction * 10.0f);
                if (decile != session.scrubLastLogDecile) {
                    session.scrubLastLogDecile = decile;
                    RDX_LOG_INFO(Weapon,
                        "CLIP-SCRUB drive part='{}' fraction={:.3f} slope={:.1f}u/f dirValid={} err={:.2f}u",
                        sessionName(session.sourceName),
                        fraction,
                        session.scrubSlope,
                        session.scrubDirectionValid,
                        vecLength(error));
                }
                continue;
            }
            // Session-pinned mode: a hot-reload switch never swaps the path
            // under a hand mid-scrub.
            const auto liveGroup = learner.findGroup(
                WeaponPartMotionLearner::PartKey{ session.weaponFormId, session.omodFormId, sessionName(session.sourceName) },
                session.mode);
            const weapon_part_motion_path::MotionPath* path =
                session.onReturnStage ? liveGroup.returnPath : liveGroup.leaderPath;
            if (!path) {
                endSession(session);
                continue;
            }

            weapon_part_motion_path::Vec3 desired{
                session.pathAnchorTranslate.x + (hand.handTranslate.x - session.handStartTranslate.x),
                session.pathAnchorTranslate.y + (hand.handTranslate.y - session.handStartTranslate.y),
                session.pathAnchorTranslate.z + (hand.handTranslate.z - session.handStartTranslate.z),
            };

            /*
             * Delta-gated magazine freedom (reload-template step 0, Bruno
             * 2026-07-06 final form): the part rides its animation path
             * until its DELTA DISTANCE from rest reaches the detach
             * fraction of its full travel (the same delta-curve measure
             * the max-travel events trust) — i.e. the mag is guided until
             * it is OUT. From there it is a free carried part: the stored
             * HAND-LOCAL offset is sent in ROCK's HandLocal drive space,
             * composed against the LIVE hand transform at apply time, so
             * weapon/other-hand motion is fully ignored while free (a
             * weapon-root-local free drive visibly dragged the part with
             * the right hand — first in-game round). Re-capture arms once
             * the part first leaves the capture distance, then fires when
             * it comes back within it of the nearest path point: the part
             * regains authority and the glued hand follows. Followers are
             * not driven while free — acceptable for this test.
             */
            if (input.magazineFreeMovement && hand.freeMovementEligible) {
                const auto& restPoseReference = session.restPoseValid ? session.restPose : path->keys[0];
                const auto handRotation = hand.handRotateValid
                    ? weapon_part_motion_path::quatNormalizeOrIdentity(hand.handRotate)
                    : weapon_part_motion_path::Quat{ 1.0f, 0.0f, 0.0f, 0.0f };
                if (session.freeMoving) {
                    const weapon_part_motion_path::Vec3 offsetWorld = quatRotateVec(handRotation, session.freeOffsetTranslate);
                    const weapon_part_motion_path::Vec3 freeTarget{
                        hand.handTranslate.x + offsetWorld.x,
                        hand.handTranslate.y + offsetWorld.y,
                        hand.handTranslate.z + offsetWorld.z,
                    };
                    const auto seeded = weapon_part_motion_scrub::initialScrubPosition(*path, freeTarget);
                    const float pathDistance = seeded.valid
                        ? vecLength(vecSub(freeTarget, seeded.target.translate))
                        : input.magazineFreeCaptureDistanceUnits * 10.0f;
                    if (!session.freeCaptureArmed &&
                        pathDistance > input.magazineFreeCaptureDistanceUnits) {
                        session.freeCaptureArmed = true;
                    }
                    if (++session.freeFrames % kMagFreeDiagnosticFrames == 0) {
                        RDX_LOG_INFO(Weapon,
                            "MAG-FREE state part='{}' pathDist={:.1f}u capture={:.1f}u armed={}",
                            sessionName(session.sourceName),
                            pathDistance,
                            input.magazineFreeCaptureDistanceUnits,
                            session.freeCaptureArmed);
                    }
                    if (session.freeCaptureArmed && seeded.valid &&
                        pathDistance <= input.magazineFreeCaptureDistanceUnits) {
                        // Re-capture at the path point nearest the part's
                        // free pose; re-anchor exactly like a fresh grip
                        // (this frame's desired IS that point).
                        session.freeMoving = false;
                        session.freeCaptureArmed = false;
                        session.arcPosition = seeded.arcPosition;
                        session.pathAnchorTranslate = seeded.target.translate;
                        session.handStartTranslate = hand.handTranslate;
                        session.maxTravelLatched = false;
                        desired = session.pathAnchorTranslate;
                        RDX_LOG_INFO(Weapon,
                            "MAG-FREE recapture hand={} part='{}' arc={:.2f}/{:.2f} pathDist={:.1f}u",
                            handIndex == 1 ? "left" : "right",
                            sessionName(session.sourceName),
                            seeded.arcPosition,
                            path->totalArcLength,
                            pathDistance);
                    } else {
                        // Free carried-part drive: constant hand-local
                        // offset, composed by ROCK against the live hand
                        // frame. One drive per node still holds.
                        bool freeDuplicate = false;
                        for (std::uint32_t i = 0; i < driveCount; ++i) {
                            if (drives[i].bodyId == session.bodyId) {
                                freeDuplicate = true;
                                break;
                            }
                        }
                        if (!freeDuplicate && driveCount < drives.size()) {
                            auto& drive = drives[driveCount++];
                            drive.flags = static_cast<std::uint32_t>(
                                ::rock::provider::RockProviderWeaponPartTargetFlagV1::MatchBodyId);
                            drive.driveSpace = ::rock::provider::RockProviderWeaponPartDriveSpaceV1::HandLocal;
                            drive.driveHand = handIndex == 1 ? 1u : 0u;
                            drive.weaponGenerationKey = session.weaponGenerationKey;
                            drive.bodyId = session.bodyId;
                            drive.groupId = static_cast<std::uint32_t>(handIndex + 1);
                            drive.priority = kDrivePriority;
                            drive.leaseFrames = kDriveLeaseFrames;
                            quatToRotateRowMajor(session.freeOffsetRotate, drive.targetTransform.rotate);
                            drive.targetTransform.translate[0] = session.freeOffsetTranslate.x;
                            drive.targetTransform.translate[1] = session.freeOffsetTranslate.y;
                            drive.targetTransform.translate[2] = session.freeOffsetTranslate.z;
                            drive.targetTransform.scale = session.partScale;
                        }
                        continue;
                    }
                } else {
                    // Detach test on the GUIDED pose: part delta distance
                    // from rest vs the delta-curve travel range.
                    const auto probe = weapon_part_motion_scrub::scrub(*path, session.arcPosition, desired);
                    const auto extreme = weapon_part_motion_path::travelExtremeFromRest(*path, restPoseReference);
                    if (probe.valid && extreme.valid) {
                        const float partDelta = weapon_part_motion_path::poseDistance(probe.target, restPoseReference);
                        const float travelRange = extreme.peakDelta - extreme.restDelta;
                        if (travelRange > 0.0f &&
                            partDelta >= extreme.restDelta + input.magazineFreeDetachTravelFraction * travelRange) {
                            /*
                             * Detach from the part's LAST GUIDED pose so the
                             * frame is continuous; the offset is captured in
                             * the HAND's frame so the part rides position
                             * and wrist rotation from here on.
                             */
                            const auto handInverse = quatConjugate(handRotation);
                            session.freeMoving = true;
                            session.freeCaptureArmed = false;
                            session.freeFrames = 0;
                            session.freeHandRotateValid = hand.handRotateValid;
                            session.freeOffsetTranslate =
                                quatRotateVec(handInverse, vecSub(probe.target.translate, hand.handTranslate));
                            session.freeOffsetRotate = weapon_part_motion_path::quatNormalizeOrIdentity(
                                quatMul(handInverse, probe.target.rotate));
                            RDX_LOG_INFO(Weapon,
                                "MAG-FREE detach hand={} part='{}' delta={:.1f}u range={:.1f}u fraction={:.2f} arc={:.2f}/{:.2f} handRotate={}",
                                handIndex == 1 ? "left" : "right",
                                sessionName(session.sourceName),
                                partDelta,
                                travelRange,
                                input.magazineFreeDetachTravelFraction,
                                probe.arcPosition,
                                path->totalArcLength,
                                hand.handRotateValid);
                            continue;
                        }
                    }
                }
            }

            auto scrubbed = weapon_part_motion_scrub::scrub(*path, session.arcPosition, desired);
            if (!scrubbed.valid) {
                continue;
            }
            session.arcPosition = scrubbed.arcPosition;

            /*
             * DELTA CURVE anchor (Bruno, 2026-07-05): max-travel events and
             * stage-transition triggers both anchor on the path's physical
             * travel extremes — displacement from the part's REST pose
             * graphed along the path — never on key order. Learned strokes
             * record in whichever direction the animation happened to run
             * (a recorder armed while a bolt idled open stores the closing
             * stroke), so "last key" can be the rest pose; the delta curve
             * is direction-agnostic. Falls back to the first key as the
             * rest reference when no live rest pose is captured yet.
             */
            const auto& restReference = hand.restPoseValid ? hand.restPose : path->keys[0];
            const auto extreme = weapon_part_motion_path::travelExtremeFromRest(*path, restReference);
            /*
             * Percentage zones on the delta curve (Bruno, 2026-07-05): the
             * scrub is "at max" / "at rest" when its displacement from rest
             * is within a travel FRACTION of the respective extreme, so the
             * zones scale with each part's own stroke — a 4-unit pistol
             * slide and a 10-unit bolt pull behave identically.
             */
            const float scrubDelta = weapon_part_motion_path::poseDistance(scrubbed.target, restReference);
            const float travelRange = extreme.valid ? extreme.peakDelta - extreme.restDelta : 0.0f;
            const bool atMaxTravel = extreme.valid &&
                scrubDelta >= extreme.restDelta + (1.0f - input.travelExtremeToleranceFraction) * travelRange;
            const bool atRestPoint = extreme.valid &&
                scrubDelta <= extreme.restDelta + input.travelExtremeToleranceFraction * travelRange;

            /*
             * Max-travel event (shell-eject test), checked BEFORE the stage
             * handoff so the emission belongs to the extreme just reached,
             * not to the next stage's seed. Latched: one event on entering
             * the max-travel zone, re-armed only after the part comes at
             * least halfway back down the delta curve toward rest.
             */
            if (!session.onReturnStage && extreme.valid) {
                if (atMaxTravel && !session.maxTravelLatched) {
                    session.maxTravelLatched = true;
                    if (outMaxTravelEvents && outMaxTravelEventCount &&
                        *outMaxTravelEventCount < kMaxMaxTravelEvents) {
                        auto& event = outMaxTravelEvents[(*outMaxTravelEventCount)++];
                        event.bodyId = session.bodyId;
                        event.sourceName = session.sourceName;
                        event.arcPosition = scrubbed.arcPosition;
                        event.extremeArcPosition = extreme.arcPosition;
                        event.peakDelta = extreme.peakDelta;
                    }
                } else if (!atMaxTravel && session.maxTravelLatched &&
                           scrubDelta <= extreme.restDelta + 0.5f * travelRange) {
                    session.maxTravelLatched = false;
                }
            }

            /*
             * Stage handoff at the extremes (Bruno, 2026-07-05): reaching
             * the end of the active stage hands the session to the OTHER
             * learned stage — mag pulled fully out continues onto the
             * insertion path with its own min/max, and completing the
             * insertion re-arms the extraction. The handoff re-seeds the
             * path anchor and the hand baseline, so displacement measures
             * from the handoff pose; the seed must land near the next
             * stage's BEGINNING or the handoff is refused (a stage whose
             * geometry doesn't start here would be skipped end-to-end in
             * one frame otherwise). Grabbing a part already resting at the
             * primary's end hands off on the first update, so an out-mag
             * grab starts directly on the insertion stage.
             */
            // Transition trigger on the SAME delta-curve zones: a stage
            // hands over at either physical travel extreme — the far point
            // (mag fully out -> insertion path) or the rest point (seated ->
            // extraction re-arms) — not at the recorded key order's end. The
            // seed-near-next-start check below still decides whether a
            // chained stage actually begins at the reached extreme. Old
            // arc-end trigger remains only as the no-delta-curve fallback.
            const bool atTransitionPoint = extreme.valid
                ? (atMaxTravel || atRestPoint)
                : scrubbed.arcPosition >= path->totalArcLength * (1.0f - input.travelExtremeToleranceFraction);
            if (input.stageTransitionsEnabled && atTransitionPoint) {
                const auto* nextPath = session.onReturnStage ? liveGroup.leaderPath : liveGroup.returnPath;
                const auto* nextFollowers = session.onReturnStage ? liveGroup.followers : liveGroup.returnFollowers;
                const auto nextFollowerCount = session.onReturnStage ? liveGroup.followerCount : liveGroup.returnFollowerCount;
                if (nextPath && nextPath->valid) {
                    const auto seeded = weapon_part_motion_scrub::initialScrubPosition(*nextPath, scrubbed.target.translate);
                    // Accept only near-start seeds (chained stages start at
                    // the previous stage's end by construction).
                    constexpr float kStageHandoffMaxSeedArcFraction = 0.25f;
                    if (seeded.valid &&
                        seeded.arcPosition <= kStageHandoffMaxSeedArcFraction * nextPath->totalArcLength) {
                        session.onReturnStage = !session.onReturnStage;
                        // The handoff seed lands at a stage START, so the
                        // max-travel latch re-arms with the new stage.
                        session.maxTravelLatched = false;
                        session.arcPosition = seeded.arcPosition;
                        session.pathAnchorTranslate = seeded.target.translate;
                        session.handStartTranslate = hand.handTranslate;
                        session.followerCount = 0;
                        if (nextFollowers) {
                            session.followerCount = (std::min)(nextFollowerCount, static_cast<std::uint32_t>(session.followers.size()));
                            for (std::uint32_t i = 0; i < session.followerCount; ++i) {
                                session.followers[i] = nextFollowers[i];
                            }
                        }
                        path = nextPath;
                        scrubbed = seeded;
                        RDX_LOG_INFO(Weapon,
                            "WeaponPartDriveSandbox: stage handoff hand={} part='{}' -> {} stage (arc {:.2f}/{:.2f}, {} followers)",
                            handIndex == 1 ? "left" : "right",
                            sessionName(session.sourceName),
                            session.onReturnStage ? "return" : "primary",
                            seeded.arcPosition,
                            nextPath->totalArcLength,
                            session.followerCount);
                    }
                }
            }

            // One drive per node: if both hands somehow grip the same body,
            // the first (right) hand keeps authority for this frame.
            bool duplicateBody = false;
            for (std::uint32_t i = 0; i < driveCount; ++i) {
                if (drives[i].bodyId == session.bodyId) {
                    duplicateBody = true;
                    break;
                }
            }
            if (duplicateBody) {
                continue;
            }

            auto& drive = drives[driveCount++];
            drive.flags = static_cast<std::uint32_t>(::rock::provider::RockProviderWeaponPartTargetFlagV1::MatchBodyId);
            drive.driveSpace = ::rock::provider::RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
            drive.weaponGenerationKey = session.weaponGenerationKey;
            drive.bodyId = session.bodyId;
            drive.groupId = static_cast<std::uint32_t>(handIndex + 1);
            drive.priority = kDrivePriority;
            drive.leaseFrames = kDriveLeaseFrames;
            quatToRotateRowMajor(scrubbed.target.rotate, drive.targetTransform.rotate);
            drive.targetTransform.translate[0] = scrubbed.target.translate.x;
            drive.targetTransform.translate[1] = scrubbed.target.translate.y;
            drive.targetTransform.translate[2] = scrubbed.target.translate.z;
            drive.targetTransform.scale = session.partScale;

            // Authored assembly followers move at the same stroke progress —
            // driven by source name so they need no collider evidence. The
            // chain filter drives at most one node per parent chain (see
            // chainFilterFollowers).
            std::array<bool, weapon_clip_stroke::kMaxFollowers> followerKept{};
            (void)chainFilterFollowers(
                input, sessionName(session.sourceName), session.followers.data(), session.followerCount, followerKept);
            const float keyPosition = weapon_clip_stroke::keyPositionForArc(*path, session.arcPosition);
            for (std::uint32_t i = 0; i < session.followerCount && driveCount < drives.size(); ++i) {
                const auto& follower = session.followers[i];
                if (follower.boneName[0] == '\0' || !followerKept[i]) {
                    continue;
                }
                const auto followerPose = weapon_clip_stroke::followerPoseAtKeyPosition(follower, keyPosition);
                auto& followerDrive = drives[driveCount++];
                followerDrive.flags = static_cast<std::uint32_t>(::rock::provider::RockProviderWeaponPartTargetFlagV1::MatchSourceName);
                followerDrive.driveSpace = ::rock::provider::RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
                followerDrive.weaponGenerationKey = session.weaponGenerationKey;
                followerDrive.groupId = static_cast<std::uint32_t>(handIndex + 1);
                followerDrive.priority = kDrivePriority;
                followerDrive.leaseFrames = kDriveLeaseFrames;
                std::memcpy(
                    followerDrive.sourceName,
                    follower.boneName.data(),
                    (std::min)(follower.boneName.size(), sizeof(followerDrive.sourceName) - 1));
                quatToRotateRowMajor(followerPose.rotate, followerDrive.targetTransform.rotate);
                followerDrive.targetTransform.translate[0] = followerPose.translate.x;
                followerDrive.targetTransform.translate[1] = followerPose.translate.y;
                followerDrive.targetTransform.translate[2] = followerPose.translate.z;
                followerDrive.targetTransform.scale = follower.restScale;
            }
        }

        const auto* api = ::rock::provider::RockProviderApi::inst;
        if (!api || !api->setWeaponPartDriveTargetsV1 || !api->clearWeaponPartDriveTargetsV1) {
            return 0;
        }
        std::uint32_t sentCount = 0;
        if (driveCount > 0) {
            const auto result = api->setWeaponPartDriveTargetsV1(_ownerToken, drives.data(), driveCount);
            _sentDrivesLastUpdate = result == ::rock::provider::RockProviderResultV1::Ok;
            if (result == ::rock::provider::RockProviderResultV1::OwnerNotRegistered) {
                // Provider dropped this owner; re-register lazily and end the
                // sessions — their grip reports carry a dead owner token too.
                _ownerToken = 0;
                _installedTargets = {};
                _sessions = {};
            }
            if (_sentDrivesLastUpdate && outSentDrives) {
                for (std::uint32_t i = 0; i < driveCount && sentCount < kMaxSentDrives; ++i) {
                    auto& sent = outSentDrives[sentCount++];
                    sent = SentDrive{};
                    sent.bodyId = drives[i].bodyId;
                    std::memcpy(
                        sent.sourceName.data(),
                        drives[i].sourceName,
                        (std::min)(sent.sourceName.size() - 1, sizeof(drives[i].sourceName)));
                }
            }
        } else if (_sentDrivesLastUpdate) {
            (void)api->clearWeaponPartDriveTargetsV1(_ownerToken);
            _sentDrivesLastUpdate = false;
        }
        return sentCount;
    }

    void WeaponPartDriveSandbox::shutdown()
    {
        if (_ownerToken != 0) {
            const auto* api = ::rock::provider::RockProviderApi::inst;
            if (api && api->unregisterConsumerV1) {
                // Unregistration also clears this owner's whitelist targets
                // and drive leases inside the provider store.
                (void)api->unregisterConsumerV1(_ownerToken);
            }
        }
        _ownerToken = 0;
        _installedTargets = {};
        _sentDrivesLastUpdate = false;
        _registrationRetryCooldownFrames = 0;
        _registrationWarned = false;
        _sessions = {};
        _lastNoPathGripSequence = {};
        _lastAwaitingUnlockGripSequence = {};
    }
}
