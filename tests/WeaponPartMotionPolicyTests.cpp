#include "redux/MotionLibraryFormat.h"
#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartEligibility.h"
#include "redux/WeaponPartMotionLearner.h"
#include "redux/WeaponPartMotionPathPolicy.h"
#include "redux/WeaponPartMotionScrubPolicy.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace
{
    bool expectTrue(const char* label, bool value)
    {
        if (value) {
            return true;
        }

        std::printf("%s expected true\n", label);
        return false;
    }

    bool expectFalse(const char* label, bool value)
    {
        if (!value) {
            return true;
        }

        std::printf("%s expected false\n", label);
        return false;
    }

    template <class T>
    bool expectEqual(const char* label, T actual, T expected)
    {
        if (actual == expected) {
            return true;
        }

        std::printf("%s expected %llu got %llu\n", label, static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        return false;
    }
}

int main()
{
    bool ok = true;

    {
        using namespace redux::weapon_part_motion_path;

        // A synthetic bolt stroke: rest, pull back 6 units along +Y in steps,
        // hold at peak, return to rest, hold still until completion.
        RecorderState recorder{};
        std::array<PoseSample, kMaxRecordingSamples> buffer{};
        PoseSample rest{};
        rest.translate = Vec3{ 1.0f, 2.0f, 3.0f };

        StepResult lastResult = StepResult::Idle;
        for (std::uint32_t i = 0; i <= kRestStableFramesToArm; ++i) {
            lastResult = step(recorder, buffer.data(), rest, true);
        }
        ok &= expectEqual("motion recorder arms after stable rest", lastResult, StepResult::Armed);

        for (int i = 1; i <= 12; ++i) {
            PoseSample moving = rest;
            moving.translate.y = rest.translate.y + 0.5f * static_cast<float>(i);
            lastResult = step(recorder, buffer.data(), moving, true);
            ok &= expectEqual("motion recorder records the stroke", lastResult, StepResult::RecordingActive);
        }
        PoseSample peak = rest;
        peak.translate.y = rest.translate.y + 6.0f;
        for (std::uint32_t i = 0; i < 3; ++i) {
            lastResult = step(recorder, buffer.data(), peak, true);
        }
        PoseSample returned = rest;
        for (int i = 11; i >= 0; --i) {
            returned.translate.y = rest.translate.y + 0.5f * static_cast<float>(i);
            lastResult = step(recorder, buffer.data(), returned, true);
        }
        for (std::uint32_t i = 0; i < kRestReturnFramesToComplete && lastResult != StepResult::RecordingComplete; ++i) {
            lastResult = step(recorder, buffer.data(), rest, true);
        }
        ok &= expectEqual("motion recorder completes when the part is still again", lastResult, StepResult::RecordingComplete);

        MotionPath path{};
        ok &= expectTrue("completed stroke builds a motion path", buildPathFromRecording(buffer.data(), recorder.sampleCount, path));
        ok &= expectTrue("motion path is valid", path.valid);
        ok &= expectTrue("motion path arc covers the stroke", path.totalArcLength > 5.5f && path.totalArcLength < 6.5f);
        ok &= expectTrue("motion path starts at rest",
            std::abs(path.keys[0].translate.y - rest.translate.y) < 0.05f);
        ok &= expectTrue("motion path is truncated at peak excursion, not the return",
            std::abs(path.keys[kResampledKeyCount - 1].translate.y - peak.translate.y) < 0.30f);

        // Delta-curve travel extremes: max travel is the key farthest from
        // the REST reference, wherever it lies on the path — never "the
        // last key". A forward-recorded stroke has it at the far end...
        {
            const auto extreme = travelExtremeFromRest(path, rest);
            ok &= expectTrue("forward path: delta extreme valid", extreme.valid);
            ok &= expectTrue("forward path: max travel at the far end",
                extreme.arcPosition > path.totalArcLength * 0.9f);
            ok &= expectTrue("forward path: rest point at the start",
                extreme.restArcPosition < path.totalArcLength * 0.1f);

            // ...and a stroke recorded in the CLOSING direction (recorder
            // armed while the part idled open, so the rest pose is the path
            // END) still puts max travel at the physically far point, which
            // is now the path START.
            MotionPath reversed{};
            reversed.valid = true;
            reversed.totalArcLength = path.totalArcLength;
            for (std::uint32_t i = 0; i < kResampledKeyCount; ++i) {
                reversed.keys[i] = path.keys[kResampledKeyCount - 1 - i];
            }
            const auto reversedExtreme = travelExtremeFromRest(reversed, rest);
            ok &= expectTrue("reversed path: delta extreme valid", reversedExtreme.valid);
            ok &= expectTrue("reversed path: max travel at the path START",
                reversedExtreme.arcPosition < reversed.totalArcLength * 0.1f);
            ok &= expectTrue("reversed path: rest point at the path END",
                reversedExtreme.restArcPosition > reversed.totalArcLength * 0.9f);

            // A rest reference the path never leaves names no extreme.
            MotionPath flat{};
            flat.valid = true;
            flat.totalArcLength = 1.0f;
            for (auto& key : flat.keys) {
                key = rest;
            }
            ok &= expectFalse("flat delta curve names no extreme", travelExtremeFromRest(flat, rest).valid);
        }

        // Untrusted (driven) frames discard an in-flight recording.
        RecorderState drivenRecorder{};
        for (std::uint32_t i = 0; i <= kRestStableFramesToArm; ++i) {
            (void)step(drivenRecorder, buffer.data(), rest, true);
        }
        PoseSample drivenMove = rest;
        drivenMove.translate.y += 1.0f;
        (void)step(drivenRecorder, buffer.data(), drivenMove, true);
        const auto drivenResult = step(drivenRecorder, buffer.data(), drivenMove, false);
        ok &= expectEqual("driven frame discards in-flight recording", drivenResult, StepResult::RecordingDiscarded);

        // Micro-jitter strokes never become paths.
        std::array<PoseSample, 4> jitter{};
        jitter[0] = rest;
        jitter[1] = rest;
        jitter[1].translate.y += 0.15f;
        jitter[2] = rest;
        jitter[3] = rest;
        MotionPath jitterPath{};
        ok &= expectFalse("stroke below the noise floor builds no path",
            buildPathFromRecording(jitter.data(), static_cast<std::uint32_t>(jitter.size()), jitterPath));

        MotionPath shorterPath = path;
        shorterPath.totalArcLength = path.totalArcLength * 0.5f;
        ok &= expectFalse("shorter stroke does not replace a longer stored path", shouldReplacePath(path, shorterPath));
        MotionPath longerPath = path;
        longerPath.totalArcLength = path.totalArcLength * 1.5f;
        ok &= expectTrue("longer stroke replaces the stored path", shouldReplacePath(path, longerPath));
        ok &= expectTrue("any valid stroke replaces an empty slot", shouldReplacePath(MotionPath{}, path));

        using namespace redux::weapon_part_motion_scrub;
        const auto seededAtRest = initialScrubPosition(path, rest.translate);
        ok &= expectTrue("scrub seeds from the part pose", seededAtRest.valid);
        ok &= expectTrue("scrub seeded at rest starts near arc zero", seededAtRest.arcPosition < 0.5f);
        const auto seededAtPeak = initialScrubPosition(path, peak.translate);
        ok &= expectTrue("scrub seeded at peak lands near full arc",
            seededAtPeak.valid && seededAtPeak.arcPosition > path.totalArcLength - 0.5f);

        // Pulling the hand along the stroke advances the scrub and the target
        // follows the path; the per-frame clamp bounds each step.
        float arc = seededAtRest.arcPosition;
        Vec3 desired = rest.translate;
        desired.y += 3.0f;
        for (int i = 0; i < 8; ++i) {
            const auto result = scrub(path, arc, desired);
            ok &= expectTrue("scrub result stays valid", result.valid);
            ok &= expectTrue("scrub advance respects the per-frame clamp",
                result.arcPosition - arc <= kMaxScrubAdvancePerFrame + 0.001f);
            arc = result.arcPosition;
        }
        ok &= expectTrue("scrub converges to the hand's point on the stroke", std::abs(arc - 3.0f) < 0.35f);
        const auto midTarget = scrub(path, arc, desired);
        ok &= expectTrue("scrub target tracks the path translation",
            std::abs(midTarget.target.translate.y - desired.y) < 0.35f &&
            std::abs(midTarget.target.translate.x - rest.translate.x) < 0.10f);

        // Overshooting the stroke clamps at the path end.
        Vec3 beyond = peak.translate;
        beyond.y += 10.0f;
        for (int i = 0; i < 16; ++i) {
            arc = scrub(path, arc, beyond).arcPosition;
        }
        ok &= expectTrue("scrub clamps at the end of the stroke", arc <= path.totalArcLength + 0.001f);
        ok &= expectTrue("scrub reaches the end of the stroke", arc > path.totalArcLength - 0.35f);
    }

    {
        using namespace redux::weapon_clip_stroke;
        using redux::weapon_part_motion_path::PoseSample;

        // Synthetic clip: bolt pulls 5 units along +Y (samples 8..40) and
        // returns; a handle rides it rigidly (same translation from an
        // offset rest — constant distance to the bolt); the magazine drops
        // 3 units along -Z in the SAME window (co-timed but drifting, so
        // NOT a follower); an ejector barely trembles (below the follower
        // excursion floor).
        std::array<TrackSamples, 4> clipTracks{};
        auto& boltTrack = clipTracks[0];
        std::memcpy(boltTrack.boneName.data(), "WeaponBolt", 10);
        auto& handleTrack = clipTracks[1];
        std::memcpy(handleTrack.boneName.data(), "WeaponBoltHandle", 16);
        auto& magTrack = clipTracks[2];
        std::memcpy(magTrack.boneName.data(), "WeaponMagazine", 14);
        auto& ejectorTrack = clipTracks[3];
        std::memcpy(ejectorTrack.boneName.data(), "WeaponEjector", 13);
        for (std::uint32_t i = 0; i < kClipSampleCount; ++i) {
            float phase = 0.0f;
            if (i >= 8 && i <= 40) {
                phase = static_cast<float>(i - 8) / 32.0f;
            } else if (i > 40) {
                phase = (std::max)(0.0f, 1.0f - static_cast<float>(i - 40) / 12.0f);
            }
            boltTrack.samples[i].translate.y = 5.0f * phase;
            handleTrack.samples[i].translate.z = 3.0f;
            handleTrack.samples[i].translate.y = 5.0f * phase;
            magTrack.samples[i].translate.z = -3.0f * phase;
            ejectorTrack.samples[i].translate.x = 0.05f * phase;
        }
        boltTrack.sampleCount = kClipSampleCount;
        handleTrack.sampleCount = kClipSampleCount;
        magTrack.sampleCount = kClipSampleCount;
        ejectorTrack.sampleCount = kClipSampleCount;

        std::array<AuthoredStrokeGroup, kMaxGroupsPerClip> groups{};
        const auto groupCount = buildAuthoredGroups(
            clipTracks.data(),
            static_cast<std::uint32_t>(clipTracks.size()),
            groups.data(),
            static_cast<std::uint32_t>(groups.size()));
        ok &= expectEqual("all three moving tracks become stroke-group leaders", groupCount, 3u);

        const AuthoredStrokeGroup* boltGroup = nullptr;
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            if (std::strcmp(groups[i].leaderBoneName.data(), "WeaponBolt") == 0) {
                boltGroup = &groups[i];
            }
        }
        ok &= expectTrue("bolt leads its own stroke group", boltGroup != nullptr);
        if (boltGroup) {
            ok &= expectTrue("bolt leader path is valid", boltGroup->leaderPath.valid);
            ok &= expectTrue("bolt stroke arc covers the pull",
                boltGroup->leaderPath.totalArcLength > 4.5f && boltGroup->leaderPath.totalArcLength < 5.5f);
            ok &= expectEqual("bolt group carries only the rigid handle — not the drifting mag or still ejector",
                boltGroup->followerCount, 1u);
            ok &= expectTrue("bolt group follower is the handle",
                std::strcmp(boltGroup->followers[0].boneName.data(), "WeaponBoltHandle") == 0);
            const auto& follower = boltGroup->followers[0];
            ok &= expectTrue("follower starts at rest", std::abs(follower.keys[0].translate.y) < 0.05f);
            ok &= expectTrue("follower reaches its stroke end with the leader",
                std::abs(follower.keys[redux::weapon_part_motion_path::kResampledKeyCount - 1].translate.y - 5.0f) < 0.15f);

            // Half-way along the leader stroke the follower is half-way too:
            // the whole assembly moves off one scrub parameter.
            const float midArc = boltGroup->leaderPath.totalArcLength * 0.5f;
            const float keyPosition = keyPositionForArc(boltGroup->leaderPath, midArc);
            const auto midPose = followerPoseAtKeyPosition(follower, keyPosition);
            ok &= expectTrue("follower tracks leader progress at mid-stroke",
                std::abs(midPose.translate.y - 2.5f) < 0.25f);
        }
    }

    {
        // Dual-source storage + mode selection: authored and learned records
        // must coexist per part, with the MotionPathMode picking the serving
        // source at lookup time only.
        using namespace redux;
        using redux::weapon_part_motion_path::PoseSample;
        using redux::weapon_part_motion_path::Vec3;

        constexpr std::uint32_t kWeapon = 0x0001ABCD;
        constexpr const char* kPart = "WeaponBolt";
        // ~2MB of recorder/path storage — far too large for the stack.
        static WeaponPartMotionLearner learner{};

        // Authored fallback stroke first (loaded shared clip).
        weapon_clip_stroke::AuthoredStrokeGroup fallbackGroup{};
        std::memcpy(fallbackGroup.leaderBoneName.data(), kPart, std::strlen(kPart));
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            fallbackGroup.leaderPath.keys[key].translate.y =
                4.0f * static_cast<float>(key) / static_cast<float>(weapon_part_motion_path::kResampledKeyCount - 1);
        }
        fallbackGroup.leaderPath.totalArcLength = 4.0f;
        fallbackGroup.leaderPath.valid = true;
        learner.storeAuthoredGroup({ kWeapon, 0, kPart }, fallbackGroup, true);

        ok &= expectTrue("authored-only serves the fallback stroke",
            learner.findPath({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly) != nullptr);
        ok &= expectTrue("hybrid serves authored while nothing is learned",
            learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::Hybrid).authored);
        ok &= expectTrue("hybrid reports the fallback tier",
            learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::Hybrid).fallbackSource);
        ok &= expectTrue("learned-only has nothing before learning",
            learner.findPath({ kWeapon, 0, kPart }, MotionPathMode::LearnedOnly) == nullptr);

        // Activated-clip stroke outranks the fallback even when shorter...
        weapon_clip_stroke::AuthoredStrokeGroup activatedGroup = fallbackGroup;
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            activatedGroup.leaderPath.keys[key].translate.y *= 0.75f;
        }
        activatedGroup.leaderPath.totalArcLength = 3.0f;
        activatedGroup.activatedClip = true;
        learner.storeAuthoredGroup({ kWeapon, 0, kPart }, activatedGroup, false);
        {
            const auto view = learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("activated stroke replaces the fallback tier", view.leaderPath && !view.fallbackSource);
            ok &= expectTrue("activated stroke arc stored",
                view.leaderPath && std::abs(view.leaderPath->totalArcLength - 3.0f) < 0.01f);
        }
        // ...and a later fallback store never demotes it.
        learner.storeAuthoredGroup({ kWeapon, 0, kPart }, fallbackGroup, true);
        ok &= expectFalse("fallback store cannot demote an activated stroke",
            learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly).fallbackSource);

        // OMOD identity isolation: two parts sharing a node name but owned
        // by different OMODs (two receiver mods both named "WeaponBolt")
        // keep separate records — a workbench part swap can only ever find
        // ITS OWN data or none, never a lookalike's.
        constexpr std::uint32_t kOmodA = 0x00112233;
        constexpr std::uint32_t kOmodB = 0x00445566;
        weapon_clip_stroke::AuthoredStrokeGroup omodAGroup = fallbackGroup;
        omodAGroup.leaderPath.totalArcLength = 6.0f;
        learner.storeAuthoredGroup({ kWeapon, kOmodA, kPart }, omodAGroup, false);
        ok &= expectTrue("omod-keyed record found under its own key",
            learner.findPath({ kWeapon, kOmodA, kPart }, MotionPathMode::AuthoredOnly) != nullptr);
        ok &= expectTrue("different omod, same name: no data served",
            learner.findPath({ kWeapon, kOmodB, kPart }, MotionPathMode::AuthoredOnly) == nullptr);
        {
            // The omod-A store must not have touched the base (omod 0) record.
            const auto baseView = learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("base-key record untouched by omod-keyed store",
                baseView.leaderPath && std::abs(baseView.leaderPath->totalArcLength - 3.0f) < 0.01f);
        }

        // Teach a learned stroke through real observations.
        PoseSample rest{};
        rest.translate = Vec3{ 0.0f, 10.0f, 0.0f };
        auto feed = [&](const PoseSample& pose) {
            learner.beginObservationFrame();
            learner.observe(WeaponPartMotionLearner::Observation{
                .weaponFormId = kWeapon,
                .sourceName = kPart,
                .pose = pose,
                .trusted = true,
            });
        };
        for (std::uint32_t i = 0; i <= weapon_part_motion_path::kRestStableFramesToArm; ++i) {
            feed(rest);
        }
        for (int i = 1; i <= 12; ++i) {
            PoseSample moving = rest;
            moving.translate.y = rest.translate.y - 0.5f * static_cast<float>(i);
            feed(moving);
        }
        for (int i = 11; i >= 0; --i) {
            PoseSample returning = rest;
            returning.translate.y = rest.translate.y - 0.5f * static_cast<float>(i);
            feed(returning);
        }
        for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 2; ++i) {
            feed(rest);
        }

        const auto* learnedPath = learner.findPath({ kWeapon, 0, kPart }, MotionPathMode::LearnedOnly);
        ok &= expectTrue("observed stroke lands in the learned record", learnedPath != nullptr);
        ok &= expectTrue("learned stroke arc covers the observed pull",
            learnedPath && learnedPath->totalArcLength > 5.5f && learnedPath->totalArcLength < 6.5f);

        // The regression this redesign prevents: learning must NOT destroy
        // the authored record, and each mode serves its own source.
        {
            const auto authoredView = learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("authored record survives learning", authoredView.leaderPath != nullptr);
            ok &= expectTrue("authored-only still serves the authored stroke",
                authoredView.leaderPath && std::abs(authoredView.leaderPath->totalArcLength - 3.0f) < 0.01f);
            const auto hybridView = learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::Hybrid);
            ok &= expectTrue("hybrid now serves the learned stroke", hybridView.leaderPath && !hybridView.authored);
            const auto availability = learner.sourceAvailability({ kWeapon, 0, kPart });
            ok &= expectTrue("availability reports both sources", availability.learned && availability.authored);
        }

        // And the reverse: a fresh authored store must not touch the learned
        // record.
        weapon_clip_stroke::AuthoredStrokeGroup biggerActivated = activatedGroup;
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            biggerActivated.leaderPath.keys[key].translate.y *= 2.0f;
        }
        biggerActivated.leaderPath.totalArcLength = 6.0f;
        learner.storeAuthoredGroup({ kWeapon, 0, kPart }, biggerActivated, false);
        ok &= expectTrue("learned record survives an authored update",
            learner.findPath({ kWeapon, 0, kPart }, MotionPathMode::LearnedOnly) != nullptr);
        {
            const auto authoredView = learner.findGroup({ kWeapon, 0, kPart }, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("larger same-tier authored stroke replaced the stored one",
                authoredView.leaderPath && std::abs(authoredView.leaderPath->totalArcLength - 6.0f) < 0.01f);
        }

        // Learned co-movement followers must carry the follower's OBSERVED
        // weapon-local scale: drives restate scale, and a hard-coded 1
        // rescaled modder meshes authored at non-1 node scales (giant Glock
        // slide piece / hunting-rifle bullet, in-game 2026-07-04).
        {
            constexpr std::uint32_t kWeapon2 = 0x0002BEEF;
            constexpr const char* kLeader = "WeaponBolt2";
            constexpr const char* kShell = "WeaponShell2";
            constexpr float kShellScale = 0.1f;

            PoseSample leaderRest{};
            leaderRest.translate = Vec3{ 0.0f, 5.0f, 0.0f };
            PoseSample shellRest{};
            shellRest.translate = Vec3{ 0.0f, 5.0f, -2.0f };

            auto feedPair = [&](float strokeOffsetY) {
                learner.beginObservationFrame();
                PoseSample leaderPose = leaderRest;
                leaderPose.translate.y += strokeOffsetY;
                learner.observe(WeaponPartMotionLearner::Observation{
                    .weaponFormId = kWeapon2,
                    .sourceName = kLeader,
                    .pose = leaderPose,
                    .scale = 1.0f,
                    .trusted = true,
                });
                PoseSample shellPose = shellRest;
                shellPose.translate.y += strokeOffsetY;
                learner.observe(WeaponPartMotionLearner::Observation{
                    .weaponFormId = kWeapon2,
                    .sourceName = kShell,
                    .pose = shellPose,
                    .scale = kShellScale,
                    .trusted = true,
                });
            };
            for (std::uint32_t i = 0; i <= weapon_part_motion_path::kRestStableFramesToArm; ++i) {
                feedPair(0.0f);
            }
            for (int i = 1; i <= 12; ++i) {
                feedPair(-0.5f * static_cast<float>(i));
            }
            for (int i = 11; i >= 0; --i) {
                feedPair(-0.5f * static_cast<float>(i));
            }
            for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 2; ++i) {
                feedPair(0.0f);
            }

            const auto group = learner.findGroup({ kWeapon2, 0, kLeader }, MotionPathMode::LearnedOnly);
            ok &= expectTrue("co-moved pair learns a leader group", group.leaderPath != nullptr);
            ok &= expectTrue("rigid co-mover rides as a learned follower", group.followerCount >= 1);
            if (group.followerCount >= 1) {
                ok &= expectTrue("learned follower keeps the co-mover's name",
                    std::strcmp(group.followers[0].boneName.data(), kShell) == 0);
                ok &= expectTrue("learned follower carries the OBSERVED scale, not 1",
                    std::abs(group.followers[0].restScale - kShellScale) < 0.001f);
            }
        }

        // Co-timed (non-rigid) follower tier: a part moving on its own axis
        // but only INSIDE the leader's stroke window rides along (P320
        // barrel tilt during the slide travel); a part whose motion happened
        // in a different phase never groups.
        {
            constexpr std::uint32_t kWeapon3 = 0x0003CAFE;
            constexpr const char* kSlide = "P320Slide";
            constexpr const char* kBarrel = "P320Barrel";
            constexpr const char* kEarlyMover = "P320Mag";

            PoseSample slideRest{};
            slideRest.translate = Vec3{ 0.0f, 8.0f, 0.0f };
            PoseSample barrelRest{};
            barrelRest.translate = Vec3{ 0.0f, 6.0f, 1.0f };
            PoseSample earlyRest{};
            earlyRest.translate = Vec3{ 0.0f, 4.0f, -3.0f };

            auto feed3 = [&](const char* part, const PoseSample& pose) {
                learner.observe(WeaponPartMotionLearner::Observation{
                    .weaponFormId = kWeapon3,
                    .sourceName = part,
                    .pose = pose,
                    .scale = 1.0f,
                    .trusted = true,
                });
            };
            auto frame3 = [&](float slideOffsetY, float barrelOffsetZ, float earlyOffsetZ) {
                learner.beginObservationFrame();
                PoseSample slidePose = slideRest;
                slidePose.translate.y += slideOffsetY;
                feed3(kSlide, slidePose);
                PoseSample barrelPose = barrelRest;
                barrelPose.translate.z += barrelOffsetZ;
                feed3(kBarrel, barrelPose);
                PoseSample earlyPose = earlyRest;
                earlyPose.translate.z += earlyOffsetZ;
                feed3(kEarlyMover, earlyPose);
            };

            // Phase 0: everything at rest (arm all recorders).
            for (std::uint32_t i = 0; i <= weapon_part_motion_path::kRestStableFramesToArm; ++i) {
                frame3(0.0f, 0.0f, 0.0f);
            }
            // Phase 1: the early mover does its own thing while the slide is
            // still at rest, then settles well before the slide stroke.
            for (int i = 1; i <= 10; ++i) {
                frame3(0.0f, 0.0f, -0.4f * static_cast<float>(i));
            }
            for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 10; ++i) {
                frame3(0.0f, 0.0f, -4.0f);
            }
            // Phase 2: slide stroke; the barrel tilts only during the middle
            // of the stroke (non-rigid: different axis, varying distance).
            for (int i = 1; i <= 16; ++i) {
                const float barrelOffset = (i >= 5 && i <= 12) ? 0.25f * static_cast<float>(i - 4) : (i > 12 ? 2.0f : 0.0f);
                frame3(-0.5f * static_cast<float>(i), barrelOffset, -4.0f);
            }
            // Hold peak, then settle so the slide recording completes.
            for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 2; ++i) {
                frame3(-8.0f, 2.0f, -4.0f);
            }

            const auto slideGroup = learner.findGroup({ kWeapon3, 0, kSlide }, MotionPathMode::LearnedOnly);
            ok &= expectTrue("slide stroke learned", slideGroup.leaderPath != nullptr);
            bool barrelIsFollower = false;
            bool earlyMoverIsFollower = false;
            for (std::uint32_t i = 0; i < slideGroup.followerCount; ++i) {
                if (std::strcmp(slideGroup.followers[i].boneName.data(), kBarrel) == 0) {
                    barrelIsFollower = true;
                }
                if (std::strcmp(slideGroup.followers[i].boneName.data(), kEarlyMover) == 0) {
                    earlyMoverIsFollower = true;
                }
            }
            ok &= expectTrue("non-rigid co-timed barrel rides the slide stroke", barrelIsFollower);
            ok &= expectFalse("different-phase mover never groups with the slide", earlyMoverIsFollower);
        }

        // Stage chaining: a stroke starting where the primary ends becomes
        // the RETURN stage (mag-in after mag-out) instead of losing to the
        // largest-stroke rule; both stages stay available.
        {
            constexpr std::uint32_t kWeapon4 = 0x0004F00D;
            constexpr const char* kMag = "GlockMag";

            PoseSample seated{};
            seated.translate = Vec3{ 0.0f, 2.0f, -1.0f };
            PoseSample out{};
            out.translate = Vec3{ 0.0f, -1.0f, -7.0f };

            auto feed4 = [&](const PoseSample& pose) {
                learner.beginObservationFrame();
                learner.observe(WeaponPartMotionLearner::Observation{
                    .weaponFormId = kWeapon4,
                    .sourceName = kMag,
                    .pose = pose,
                    .scale = 1.0f,
                    .trusted = true,
                });
            };
            auto lerpPose4 = [&](const PoseSample& a, const PoseSample& b, float t) {
                return weapon_part_motion_path::lerpPose(a, b, t);
            };

            // Extraction: seated -> out, settle at OUT so the recording
            // completes with the out pose as its peak.
            for (std::uint32_t i = 0; i <= weapon_part_motion_path::kRestStableFramesToArm; ++i) {
                feed4(seated);
            }
            for (int i = 1; i <= 12; ++i) {
                feed4(lerpPose4(seated, out, static_cast<float>(i) / 12.0f));
            }
            for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 2; ++i) {
                feed4(out);
            }
            const auto* primaryPath = learner.findPath({ kWeapon4, 0, kMag }, MotionPathMode::LearnedOnly);
            ok &= expectTrue("extraction stroke learned as primary", primaryPath != nullptr);
            ok &= expectTrue("no return stage before the insertion is observed",
                learner.findGroup({ kWeapon4, 0, kMag }, MotionPathMode::LearnedOnly).returnPath == nullptr);

            // Insertion: re-arm at OUT, push back to seated, settle.
            for (std::uint32_t i = 0; i <= weapon_part_motion_path::kRestStableFramesToArm; ++i) {
                feed4(out);
            }
            for (int i = 1; i <= 12; ++i) {
                feed4(lerpPose4(out, seated, static_cast<float>(i) / 12.0f));
            }
            for (std::uint32_t i = 0; i < weapon_part_motion_path::kRestReturnFramesToComplete + 2; ++i) {
                feed4(seated);
            }

            const auto magGroup = learner.findGroup({ kWeapon4, 0, kMag }, MotionPathMode::LearnedOnly);
            ok &= expectTrue("primary stage survives the insertion", magGroup.leaderPath != nullptr);
            ok &= expectTrue("insertion stroke stored as the return stage", magGroup.returnPath != nullptr);
            if (magGroup.leaderPath && magGroup.returnPath) {
                ok &= expectTrue("return stage starts at the primary's end",
                    weapon_part_motion_path::poseDistance(
                        magGroup.returnPath->keys.front(),
                        magGroup.leaderPath->keys.back()) < 2.0f);
                ok &= expectTrue("return stage ends near the primary's start",
                    weapon_part_motion_path::poseDistance(
                        magGroup.returnPath->keys.back(),
                        magGroup.leaderPath->keys.front()) < 2.0f);
            }
        }
    }

    {
        // AttachOnly allowlist booleans: defaults reproduce the action-role
        // + feed-chain set; per-key toggles change exactly their class.
        using namespace redux;
        using ActionRole = rock::provider::RockProviderWeaponActionRoleV1;
        using PartKind = rock::provider::RockProviderWeaponPartKindV1;

        const auto defaults = defaultAttachOnlyAllowList();
        ok &= expectTrue("default allowlist admits action-role bolt",
            defaults.allows(ActionRole::Bolt, PartKind::Other));
        ok &= expectTrue("default allowlist admits magazine part kind",
            defaults.allows(ActionRole::None, PartKind::Magazine));
        ok &= expectFalse("default allowlist rejects stock",
            defaults.allows(ActionRole::None, PartKind::Stock));
        ok &= expectFalse("default allowlist rejects latch+receiver",
            defaults.allows(ActionRole::Latch, PartKind::Receiver));

        // Rebuild with stock ON and magazine OFF via the key table, exactly
        // as the config does from the INI booleans.
        AttachOnlyAllowList custom{};
        for (const auto& key : kAttachOnlyPartKeys) {
            bool enabled = key.defaultOn;
            if (std::strcmp(key.name, "stock") == 0) {
                enabled = true;
            } else if (std::strcmp(key.name, "magazine") == 0) {
                enabled = false;
            }
            applyAttachOnlyPartKey(key, enabled, custom);
        }
        ok &= expectTrue("toggled-on stock is admitted", custom.allows(ActionRole::None, PartKind::Stock));
        ok &= expectFalse("toggled-off magazine is rejected", custom.allows(ActionRole::None, PartKind::Magazine));
        ok &= expectTrue("unrelated bolt class unaffected by toggles",
            custom.allows(ActionRole::Bolt, PartKind::Other));
        ok &= expectTrue("bolt key also enables the bolt part kind",
            custom.allows(ActionRole::None, PartKind::Bolt));
    }

    {
        // Motion-library format: serialize -> parse must round-trip identity,
        // curation text, paths, and followers; garbage fails closed.
        using namespace redux;
        using namespace redux::motion_library;

        WeaponLibrary library;
        library.weapon = FormRef{ "Fallout4.esm", 0x0004822D };
        library.weaponName = "Hunting Rifle";
        library.curated = true;
        PartRecord part;
        part.omod = FormRef{ "SomeMod.esp", 0x000123 };
        part.sourceName = "WeaponBolt";
        part.learnedPrimary.used = true;
        part.learnedPrimary.path.valid = true;
        part.learnedPrimary.path.totalArcLength = 6.5f;
        part.learnedPrimary.stageName = "boltPull";
        part.learnedPrimary.notes = "hand tuned";
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            part.learnedPrimary.path.keys[key].translate.y = static_cast<float>(key) * 0.25f;
        }
        part.learnedPrimary.followerCount = 1;
        auto& follower = part.learnedPrimary.followers[0];
        std::memcpy(follower.boneName.data(), "Bullet01", 8);
        follower.restScale = 0.5f;
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            follower.keys[key].translate.z = static_cast<float>(key) * 0.1f;
        }
        library.parts.push_back(part);

        const auto text = serialize(library);
        WeaponLibrary parsed;
        std::string error;
        ok &= expectTrue("library JSON parses back", parse(text, parsed, &error));
        ok &= expectTrue("library parse reports no error", error.empty());
        ok &= expectTrue("weapon identity round-trips",
            parsed.weapon.plugin == "Fallout4.esm" && parsed.weapon.localFormId == 0x0004822D);
        ok &= expectTrue("curated flag round-trips", parsed.curated);
        ok &= expectEqual("part count round-trips", parsed.parts.size(), std::size_t{ 1 });
        if (parsed.parts.size() == 1) {
            const auto& p = parsed.parts[0];
            ok &= expectTrue("omod ref round-trips",
                p.omod.plugin == "SomeMod.esp" && p.omod.localFormId == 0x000123u);
            ok &= expectTrue("stage flags round-trip", p.learnedPrimary.used && !p.learnedReturn.used && !p.authored.used);
            ok &= expectTrue("stage curation text round-trips",
                p.learnedPrimary.stageName == "boltPull" && p.learnedPrimary.notes == "hand tuned");
            ok &= expectTrue("path arc round-trips",
                p.learnedPrimary.path.valid &&
                    std::abs(p.learnedPrimary.path.totalArcLength - 6.5f) < 0.001f);
            ok &= expectTrue("path keys round-trip",
                std::abs(p.learnedPrimary.path.keys[23].translate.y - 23.0f * 0.25f) < 0.001f);
            ok &= expectEqual("follower count round-trips", p.learnedPrimary.followerCount, 1u);
            ok &= expectTrue("follower data round-trips",
                std::strncmp(p.learnedPrimary.followers[0].boneName.data(), "Bullet01", 8) == 0 &&
                    std::abs(p.learnedPrimary.followers[0].restScale - 0.5f) < 0.001f);
        }

        WeaponLibrary garbage;
        ok &= expectFalse("garbage text fails closed", parse("not json at all {", garbage, nullptr));
        ok &= expectFalse("future format version fails closed",
            parse(R"({"format": 999, "weapon": {"plugin": "a.esp", "id": "0x1"}, "parts": []})", garbage, nullptr));
    }

    {
        // Learner export/import: records round-trip through the view API,
        // and import NEVER overwrites live in-RAM data ("disk seeds, live
        // learning wins").
        using namespace redux;

        constexpr std::uint32_t kWeapon = 0x0002BEEF;
        constexpr std::uint32_t kOmod = 0x00777777;
        constexpr const char* kPart = "WeaponSlide";
        static WeaponPartMotionLearner source{};
        static WeaponPartMotionLearner destination{};

        weapon_clip_stroke::AuthoredStrokeGroup group{};
        std::memcpy(group.leaderBoneName.data(), kPart, std::strlen(kPart));
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            group.leaderPath.keys[key].translate.x = static_cast<float>(key) * 0.2f;
        }
        group.leaderPath.totalArcLength = 4.6f;
        group.leaderPath.valid = true;
        group.activatedClip = true;
        source.storeAuthoredGroup({ kWeapon, kOmod, kPart }, group, false);

        std::array<WeaponPartMotionLearner::RecordView, WeaponPartMotionLearner::kMaxStoredPaths> records{};
        const auto exported = source.exportWeaponRecords(kWeapon, records.data(),
            static_cast<std::uint32_t>(records.size()));
        ok &= expectEqual("one record exported for the weapon", exported, 1u);
        if (exported == 1) {
            ok &= expectTrue("exported record carries the omod key",
                records[0].omodFormId == kOmod && records[0].sourceName == kPart);
            ok &= expectTrue("exported record carries the authored stage",
                records[0].authored.path != nullptr && records[0].learnedPrimary.path == nullptr);

            ok &= expectTrue("import seeds an empty learner",
                destination.importRecord({ kWeapon, kOmod, kPart }, records[0]));
            const auto* imported = destination.findPath({ kWeapon, kOmod, kPart }, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("imported path serves under the same key",
                imported && std::abs(imported->totalArcLength - 4.6f) < 0.001f);

            // A second import against now-populated records applies nothing.
            ok &= expectFalse("import never overwrites live data",
                destination.importRecord({ kWeapon, kOmod, kPart }, records[0]));
        }
    }

    return ok ? 0 : 1;
}
