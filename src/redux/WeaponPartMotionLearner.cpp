#include "redux/WeaponPartMotionLearner.h"

#include "ReduxLog.h"

#include <algorithm>
#include <cstring>

namespace redux
{
    namespace
    {
        std::string_view slotName(const std::array<char, WeaponPartMotionLearner::kMaxSourceName>& name)
        {
            std::size_t length = 0;
            while (length < name.size() && name[length] != '\0') {
                ++length;
            }
            return std::string_view(name.data(), length);
        }

        void copySlotName(std::array<char, WeaponPartMotionLearner::kMaxSourceName>& target, std::string_view source)
        {
            target = {};
            const auto count = (std::min)(source.size(), target.size() - 1);
            std::memcpy(target.data(), source.data(), count);
        }

        bool slotMatches(
            std::uint32_t slotFormId,
            const std::array<char, WeaponPartMotionLearner::kMaxSourceName>& name,
            std::uint32_t weaponFormId,
            std::string_view sourceName)
        {
            return slotFormId == weaponFormId && slotName(name) == sourceName;
        }
    }

    void WeaponPartMotionLearner::beginObservationFrame()
    {
        ++_frameCounter;
    }

    void WeaponPartMotionLearner::observe(const Observation& observation)
    {
        if (observation.weaponFormId == 0 || observation.sourceName.empty()) {
            return;
        }
        ++_observationCounter;

        auto* recorder = acquireRecorderSlot(observation.weaponFormId, observation.sourceName);
        if (!recorder) {
            return;
        }
        recorder->lastSeenCounter = _observationCounter;
        recorder->lastScale = observation.scale;

        const bool wasRecording = recorder->state.phase == weapon_part_motion_path::RecorderPhase::Recording;
        const auto result = weapon_part_motion_path::step(
            recorder->state,
            recorder->buffer.data(),
            observation.pose,
            observation.trusted);
        if (!wasRecording && recorder->state.phase == weapon_part_motion_path::RecorderPhase::Recording) {
            // buffer[1] was written this frame; buffer[0] is the pre-motion
            // rest pose.
            recorder->startFrame = _frameCounter;
        }
        if (result == weapon_part_motion_path::StepResult::RecordingComplete) {
            storeCompletedPath(*recorder);
        }
    }

    const WeaponPartMotionLearner::StrokeGroup* WeaponPartMotionLearner::selectPrimary(
        const PathSlot& slot,
        MotionPathMode mode)
    {
        switch (mode) {
        case MotionPathMode::AuthoredOnly:
            return slot.authored.used ? &slot.authored : nullptr;
        case MotionPathMode::LearnedOnly:
            return slot.learnedPrimary.used ? &slot.learnedPrimary : nullptr;
        case MotionPathMode::Hybrid:
        default:
            // Learner priority (Bruno, 2026-07-04): a real observed stroke
            // outranks authored clip data; authored fills the gap until the
            // part is taught.
            if (slot.learnedPrimary.used) {
                return &slot.learnedPrimary;
            }
            return slot.authored.used ? &slot.authored : nullptr;
        }
    }

    const WeaponPartMotionLearner::PathSlot* WeaponPartMotionLearner::findSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName) const
    {
        for (const auto& slot : _paths) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
        }
        return nullptr;
    }

    const weapon_part_motion_path::MotionPath* WeaponPartMotionLearner::findPath(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        MotionPathMode mode) const
    {
        // lastUseCounter is an eviction hint, not behavior; keeping this
        // accessor const outweighs refreshing it on reads.
        const auto* slot = findSlot(weaponFormId, sourceName);
        const auto* primary = slot ? selectPrimary(*slot, mode) : nullptr;
        return primary ? &primary->path : nullptr;
    }

    WeaponPartMotionLearner::GroupView WeaponPartMotionLearner::findGroup(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        MotionPathMode mode) const
    {
        const auto* slot = findSlot(weaponFormId, sourceName);
        const auto* primary = slot ? selectPrimary(*slot, mode) : nullptr;
        if (!primary) {
            return {};
        }
        GroupView view{
            .leaderPath = &primary->path,
            .followers = primary->followers.data(),
            .followerCount = primary->followerCount,
            .authored = primary == &slot->authored,
            .fallbackSource = primary == &slot->authored && slot->authoredFallback,
        };
        // The return stage is learned data chained onto the learned primary;
        // it only serves when the learned primary is the serving stage.
        if (!view.authored && slot->learnedReturn.used) {
            view.returnPath = &slot->learnedReturn.path;
            view.returnFollowers = slot->learnedReturn.followers.data();
            view.returnFollowerCount = slot->learnedReturn.followerCount;
        }
        return view;
    }

    WeaponPartMotionLearner::SourceAvailability WeaponPartMotionLearner::sourceAvailability(
        std::uint32_t weaponFormId,
        std::string_view sourceName) const
    {
        const auto* slot = findSlot(weaponFormId, sourceName);
        if (!slot) {
            return {};
        }
        return SourceAvailability{
            .learned = slot->learnedPrimary.used,
            .authored = slot->authored.used,
            .authoredFallback = slot->authored.used && slot->authoredFallback,
        };
    }

    WeaponPartMotionLearner::PathSlot* WeaponPartMotionLearner::findOrClaimSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        bool preferSlotsWithoutLearnedData)
    {
        PathSlot* freeSlot = nullptr;
        PathSlot* preferredEviction = nullptr;
        PathSlot* anyEviction = nullptr;
        for (auto& slot : _paths) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
            if (!slot.used) {
                freeSlot = freeSlot ? freeSlot : &slot;
                continue;
            }
            if (!anyEviction || slot.lastUseCounter < anyEviction->lastUseCounter) {
                anyEviction = &slot;
            }
            if (preferSlotsWithoutLearnedData && slot.learnedPrimary.used) {
                continue;
            }
            if (!preferredEviction || slot.lastUseCounter < preferredEviction->lastUseCounter) {
                preferredEviction = &slot;
            }
        }

        auto* claimed = freeSlot ? freeSlot : (preferredEviction ? preferredEviction : anyEviction);
        if (!claimed) {
            return nullptr;
        }
        if (claimed->used) {
            RDX_LOG_DEBUG(Weapon,
                "WeaponPartMotionLearner: evicting path slot for part '{}' on weapon {:08X} (learned={} authored={})",
                slotName(claimed->sourceName),
                claimed->weaponFormId,
                claimed->learnedPrimary.used,
                claimed->authored.used);
        }
        *claimed = {};
        claimed->used = true;
        claimed->weaponFormId = weaponFormId;
        copySlotName(claimed->sourceName, sourceName);
        return claimed;
    }

    void WeaponPartMotionLearner::storeAuthoredGroup(
        std::uint32_t weaponFormId,
        std::string_view sourceName,
        const weapon_clip_stroke::AuthoredStrokeGroup& group,
        bool fallbackSource)
    {
        if (weaponFormId == 0 || sourceName.empty() || !group.leaderPath.valid) {
            return;
        }
        ++_observationCounter;

        auto* target = findOrClaimSlot(weaponFormId, sourceName, true);
        if (!target) {
            return;
        }
        auto& record = target->authored;
        if (record.used) {
            // SOURCE TIER (Bruno, 2026-07-04): the data must be what THIS
            // weapon actually plays — a stroke from a clip the weapon
            // ACTIVATED always beats a merely-loaded fallback stroke for the
            // same part; fallback data only stands while nothing else
            // exists. Within the same tier the largest leader stroke wins
            // (a reload stroke beats a fire nudge).
            if (target->authoredFallback != fallbackSource) {
                if (fallbackSource) {
                    return;
                }
            } else if (!weapon_part_motion_path::shouldReplacePath(record.path, group.leaderPath)) {
                return;
            }
        }

        const bool replaced = record.used;
        record.used = true;
        target->authoredFallback = fallbackSource;
        record.path = group.leaderPath;
        record.followerCount = (std::min)(group.followerCount, static_cast<std::uint32_t>(record.followers.size()));
        record.followers = group.followers;
        target->lastUseCounter = _observationCounter;
        ++_revision;

        // Path endpoints in the stored frame: comparing these against the
        // learner's endpoints for the same part exposes any frame mismatch
        // between authored (rig-derived) and learned (scene-observed) data.
        const auto& firstKey = group.leaderPath.keys.front();
        const auto& lastKey = group.leaderPath.keys.back();
        RDX_LOG_INFO(Weapon,
            "WeaponPartMotionLearner: {} AUTHORED [{}] stroke group for part '{}' on weapon {:08X} (leader arc {:.2f} game units, {} followers, learned record {}) start=({:.2f},{:.2f},{:.2f}) end=({:.2f},{:.2f},{:.2f})",
            replaced ? "updated" : "stored",
            fallbackSource ? "fallback-loaded" : "weapon-clip",
            sourceName,
            weaponFormId,
            group.leaderPath.totalArcLength,
            record.followerCount,
            target->learnedPrimary.used ? "also present" : "absent",
            firstKey.translate.x,
            firstKey.translate.y,
            firstKey.translate.z,
            lastKey.translate.x,
            lastKey.translate.y,
            lastKey.translate.z);
    }

    void WeaponPartMotionLearner::reset()
    {
        /*
         * Slot-by-slot on purpose: `_paths = {}` / `_recorders = {}`
         * materialize full temporary ARRAYS on the stack (megabytes), and
         * both temporaries share one frame — this overflowed the game main
         * thread's stack in the load-game reset (in-game 2026-07-04,
         * EXCEPTION_STACK_OVERFLOW in __chkstk). Per-slot temporaries keep
         * the frame at one slot.
         */
        for (auto& slot : _paths) {
            slot = {};
        }
        for (auto& slot : _recorders) {
            slot = {};
        }
        _observationCounter = 0;
        ++_revision;
    }

    WeaponPartMotionLearner::RecorderSlot* WeaponPartMotionLearner::acquireRecorderSlot(
        std::uint32_t weaponFormId,
        std::string_view sourceName)
    {
        RecorderSlot* freeSlot = nullptr;
        RecorderSlot* staleSlot = nullptr;
        for (auto& slot : _recorders) {
            if (slot.used && slotMatches(slot.weaponFormId, slot.sourceName, weaponFormId, sourceName)) {
                return &slot;
            }
            if (!slot.used) {
                freeSlot = freeSlot ? freeSlot : &slot;
            } else if (_observationCounter - slot.lastSeenCounter > kRecorderStaleObservationAge &&
                       (!staleSlot || slot.lastSeenCounter < staleSlot->lastSeenCounter)) {
                staleSlot = &slot;
            }
        }

        auto* claimed = freeSlot ? freeSlot : staleSlot;
        if (!claimed) {
            return nullptr;
        }
        claimed->used = true;
        claimed->weaponFormId = weaponFormId;
        copySlotName(claimed->sourceName, sourceName);
        claimed->lastSeenCounter = _observationCounter;
        claimed->state = {};
        return claimed;
    }

    void WeaponPartMotionLearner::storeCompletedPath(const RecorderSlot& recorder)
    {
        weapon_part_motion_path::MotionPath candidate{};
        std::array<float, weapon_part_motion_path::kResampledKeyCount> keyPositions{};
        if (!weapon_part_motion_path::buildPathFromRecording(
                recorder.buffer.data(), recorder.state.sampleCount, candidate, keyPositions.data())) {
            return;
        }

        /*
         * Stage assignment (Bruno, 2026-07-05): a stroke whose START pose
         * chains onto the stored primary's END pose is the part's RETURN
         * stage (mag-in observed after mag-out) and gets its own path and
         * min/max instead of competing with — and losing to — the larger
         * primary. Which motion becomes "primary" is simply whichever was
         * learned first; the scrub stage machine is symmetric.
         */
        auto* target = findOrClaimSlot(recorder.weaponFormId, slotName(recorder.sourceName), false);
        if (!target) {
            return;
        }
        bool isReturnStage = false;
        if (_tuning.stageCapture && target->learnedPrimary.used) {
            const auto& primaryPath = target->learnedPrimary.path;
            const float startToPrimaryEnd = weapon_part_motion_path::poseDistance(
                candidate.keys.front(),
                primaryPath.keys.back());
            const float startToPrimaryStart = weapon_part_motion_path::poseDistance(
                candidate.keys.front(),
                primaryPath.keys.front());
            isReturnStage = startToPrimaryEnd <= _tuning.stageChainToleranceGameUnits &&
                startToPrimaryEnd < startToPrimaryStart;
        }
        auto& record = isReturnStage ? target->learnedReturn : target->learnedPrimary;
        // Within a stage the larger stroke wins; the other stage and the
        // authored record are untouched either way (dual storage).
        if (record.used && !weapon_part_motion_path::shouldReplacePath(record.path, candidate)) {
            return;
        }

        /*
         * Follower recovery from frame-aligned concurrent recordings, two
         * tiers (see header): RIGID (constant leader distance) and CO-TIMED
         * (non-rigid but temporally contained in the leader's window — the
         * P320 barrel tilting while the slide travels). Containment is the
         * gate that keeps separate phases of one animation apart: a mag
         * moving in another phase shares no window with this stroke, and a
         * part whose motion mostly happens OUTSIDE the window (a carry
         * mover spanning phases) fails the overlap fraction.
         */
        struct FollowerCandidate
        {
            const RecorderSlot* recorder{ nullptr };
            std::int64_t frameOffset{ 0 };
            bool rigid{ false };
        };
        std::array<FollowerCandidate, weapon_clip_stroke::kMaxFollowers> followerCandidates{};
        std::uint32_t followerCandidateCount = 0;
        std::uint32_t rigidCount = 0;
        std::uint32_t coTimedCount = 0;
        constexpr float kRigidDistanceToleranceGameUnits =
            weapon_clip_stroke::kRigidFollowerDistanceToleranceGameUnits;
        constexpr std::int64_t kMinOverlapSamples = 8;
        for (const auto& other : _recorders) {
            if (&other == &recorder || !other.used || other.weaponFormId != recorder.weaponFormId ||
                other.state.sampleCount < 2 ||
                followerCandidateCount >= followerCandidates.size()) {
                continue;
            }
            // Frame f holds leader sample f-startFrame+1 and other sample
            // f-other.startFrame+1 (index 0 is each recording's rest pose).
            const auto offset =
                static_cast<std::int64_t>(recorder.startFrame) - static_cast<std::int64_t>(other.startFrame);
            float minDistance = 0.0f;
            float maxDistance = 0.0f;
            float insideArc = 0.0f;
            std::int64_t overlap = 0;
            std::int64_t firstOtherIndex = -1;
            std::int64_t lastOtherIndex = -1;
            for (std::uint32_t i = 1; i < recorder.state.sampleCount; ++i) {
                const auto otherIndex = static_cast<std::int64_t>(i) + offset;
                if (otherIndex < 1 || otherIndex >= static_cast<std::int64_t>(other.state.sampleCount)) {
                    continue;
                }
                const float distance = weapon_part_motion_path::length(weapon_part_motion_path::sub(
                    recorder.buffer[i].translate,
                    other.buffer[static_cast<std::size_t>(otherIndex)].translate));
                if (overlap == 0) {
                    minDistance = distance;
                    maxDistance = distance;
                    firstOtherIndex = otherIndex;
                } else {
                    minDistance = (std::min)(minDistance, distance);
                    maxDistance = (std::max)(maxDistance, distance);
                    // Overlapped indices advance by one per leader sample, so
                    // consecutive terms measure the follower's motion INSIDE
                    // the leader's window.
                    insideArc += weapon_part_motion_path::poseDistance(
                        other.buffer[static_cast<std::size_t>(otherIndex)],
                        other.buffer[static_cast<std::size_t>(otherIndex - 1)]);
                }
                lastOtherIndex = otherIndex;
                ++overlap;
            }
            if (overlap < kMinOverlapSamples) {
                continue;
            }
            // Constant distance to a purely ROTATING leader does not prove
            // co-movement; the follower itself must have traveled.
            const bool followerMovedInWindow = weapon_part_motion_path::poseDistance(
                    other.buffer[static_cast<std::size_t>(firstOtherIndex)],
                    other.buffer[static_cast<std::size_t>(lastOtherIndex)]) >=
                weapon_clip_stroke::kFollowerMinExcursionGameUnits;

            const bool rigid = followerMovedInWindow &&
                maxDistance - minDistance <= kRigidDistanceToleranceGameUnits;
            bool coTimed = false;
            if (!rigid && _tuning.coTimedFollowers && followerMovedInWindow &&
                insideArc >= weapon_clip_stroke::kFollowerMinExcursionGameUnits &&
                insideArc <= _tuning.coTimedMaxArcRatio * candidate.totalArcLength) {
                float totalArc = 0.0f;
                for (std::uint32_t s = 1; s < other.state.sampleCount; ++s) {
                    totalArc += weapon_part_motion_path::poseDistance(other.buffer[s], other.buffer[s - 1]);
                }
                coTimed = totalArc > 0.0f && insideArc / totalArc >= _tuning.coTimedMinOverlapFraction;
            }
            if (!rigid && !coTimed) {
                continue;
            }
            followerCandidates[followerCandidateCount++] = FollowerCandidate{ &other, offset, rigid };
        }

        const bool replaced = record.used;
        record.used = true;
        record.path = candidate;
        target->lastUseCounter = _observationCounter;

        // Followers resampled at the leader's key positions (frame-aligned)
        // so they replay time-locked to the stroke; rigid tier first — it is
        // the stronger evidence when capacity runs out.
        record.followerCount = 0;
        for (const bool rigidPass : { true, false }) {
            for (std::uint32_t c = 0; c < followerCandidateCount &&
                 record.followerCount < static_cast<std::uint32_t>(record.followers.size());
                 ++c) {
                if (followerCandidates[c].rigid != rigidPass) {
                    continue;
                }
                const auto& followerRecorder = *followerCandidates[c].recorder;
                const auto offset = followerCandidates[c].frameOffset;
                auto& slot = record.followers[record.followerCount];
                slot = {};
                slot.boneName = followerRecorder.sourceName;
                // The follower's OBSERVED weapon-local scale, not 1: drives
                // restate scale, and forcing 1 rescaled modder meshes.
                slot.restScale = followerRecorder.lastScale;
                const auto lastIndex = static_cast<float>(followerRecorder.state.sampleCount - 1);
                for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
                    const float position = (std::min)(
                        lastIndex,
                        (std::max)(0.0f, keyPositions[key] + static_cast<float>(offset)));
                    const auto base = static_cast<std::size_t>(position);
                    const float t = position - static_cast<float>(base);
                    const auto next = (std::min)(base + 1, static_cast<std::size_t>(lastIndex));
                    slot.keys[key] = weapon_part_motion_path::lerpPose(
                        followerRecorder.buffer[base],
                        followerRecorder.buffer[next],
                        t);
                }
                ++record.followerCount;
                if (followerCandidates[c].rigid) {
                    ++rigidCount;
                } else {
                    ++coTimedCount;
                }
            }
        }

        /*
         * A replaced PRIMARY may break the chain to the stored return stage
         * (different rest, different end); a return stage that no longer
         * starts at the primary's end is stale and dropped, never served.
         */
        if (!isReturnStage && target->learnedReturn.used) {
            const float chainGap = weapon_part_motion_path::poseDistance(
                target->learnedReturn.path.keys.front(),
                candidate.keys.back());
            if (chainGap > _tuning.stageChainToleranceGameUnits) {
                target->learnedReturn = {};
                RDX_LOG_INFO(Weapon,
                    "WeaponPartMotionLearner: dropped return stage for part '{}' on weapon {:08X} — replaced primary broke the chain (gap {:.2f})",
                    slotName(recorder.sourceName),
                    recorder.weaponFormId,
                    chainGap);
            }
        }
        ++_revision;

        // Once per completed stroke, never per frame. Path endpoints in the
        // stored frame — the learned counterpart of the AUTHORED endpoint
        // log, for frame-mismatch comparison between the two sources.
        const auto& firstKey = candidate.keys.front();
        const auto& lastKey = candidate.keys.back();
        RDX_LOG_INFO(Weapon,
            "WeaponPartMotionLearner: {} LEARNED {} motion path for part '{}' on weapon {:08X} (stroke arc {:.2f} game units, {} raw samples, followers: {} rigid + {} co-timed, authored record {}) start=({:.2f},{:.2f},{:.2f}) end=({:.2f},{:.2f},{:.2f})",
            replaced ? "updated" : "learned",
            isReturnStage ? "RETURN-stage" : "primary",
            slotName(recorder.sourceName),
            recorder.weaponFormId,
            candidate.totalArcLength,
            recorder.state.sampleCount,
            rigidCount,
            coTimedCount,
            target->authored.used ? "also present" : "absent",
            firstKey.translate.x,
            firstKey.translate.y,
            firstKey.translate.z,
            lastKey.translate.x,
            lastKey.translate.y,
            lastKey.translate.z);
    }
}
