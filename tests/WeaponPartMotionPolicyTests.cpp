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
        learner.storeAuthoredGroup(kWeapon, kPart, fallbackGroup, true);

        ok &= expectTrue("authored-only serves the fallback stroke",
            learner.findPath(kWeapon, kPart, MotionPathMode::AuthoredOnly) != nullptr);
        ok &= expectTrue("hybrid serves authored while nothing is learned",
            learner.findGroup(kWeapon, kPart, MotionPathMode::Hybrid).authored);
        ok &= expectTrue("hybrid reports the fallback tier",
            learner.findGroup(kWeapon, kPart, MotionPathMode::Hybrid).fallbackSource);
        ok &= expectTrue("learned-only has nothing before learning",
            learner.findPath(kWeapon, kPart, MotionPathMode::LearnedOnly) == nullptr);

        // Activated-clip stroke outranks the fallback even when shorter...
        weapon_clip_stroke::AuthoredStrokeGroup activatedGroup = fallbackGroup;
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            activatedGroup.leaderPath.keys[key].translate.y *= 0.75f;
        }
        activatedGroup.leaderPath.totalArcLength = 3.0f;
        activatedGroup.activatedClip = true;
        learner.storeAuthoredGroup(kWeapon, kPart, activatedGroup, false);
        {
            const auto view = learner.findGroup(kWeapon, kPart, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("activated stroke replaces the fallback tier", view.leaderPath && !view.fallbackSource);
            ok &= expectTrue("activated stroke arc stored",
                view.leaderPath && std::abs(view.leaderPath->totalArcLength - 3.0f) < 0.01f);
        }
        // ...and a later fallback store never demotes it.
        learner.storeAuthoredGroup(kWeapon, kPart, fallbackGroup, true);
        ok &= expectFalse("fallback store cannot demote an activated stroke",
            learner.findGroup(kWeapon, kPart, MotionPathMode::AuthoredOnly).fallbackSource);

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

        const auto* learnedPath = learner.findPath(kWeapon, kPart, MotionPathMode::LearnedOnly);
        ok &= expectTrue("observed stroke lands in the learned record", learnedPath != nullptr);
        ok &= expectTrue("learned stroke arc covers the observed pull",
            learnedPath && learnedPath->totalArcLength > 5.5f && learnedPath->totalArcLength < 6.5f);

        // The regression this redesign prevents: learning must NOT destroy
        // the authored record, and each mode serves its own source.
        {
            const auto authoredView = learner.findGroup(kWeapon, kPart, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("authored record survives learning", authoredView.leaderPath != nullptr);
            ok &= expectTrue("authored-only still serves the authored stroke",
                authoredView.leaderPath && std::abs(authoredView.leaderPath->totalArcLength - 3.0f) < 0.01f);
            const auto hybridView = learner.findGroup(kWeapon, kPart, MotionPathMode::Hybrid);
            ok &= expectTrue("hybrid now serves the learned stroke", hybridView.leaderPath && !hybridView.authored);
            const auto availability = learner.sourceAvailability(kWeapon, kPart);
            ok &= expectTrue("availability reports both sources", availability.learned && availability.authored);
        }

        // And the reverse: a fresh authored store must not touch the learned
        // record.
        weapon_clip_stroke::AuthoredStrokeGroup biggerActivated = activatedGroup;
        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
            biggerActivated.leaderPath.keys[key].translate.y *= 2.0f;
        }
        biggerActivated.leaderPath.totalArcLength = 6.0f;
        learner.storeAuthoredGroup(kWeapon, kPart, biggerActivated, false);
        ok &= expectTrue("learned record survives an authored update",
            learner.findPath(kWeapon, kPart, MotionPathMode::LearnedOnly) != nullptr);
        {
            const auto authoredView = learner.findGroup(kWeapon, kPart, MotionPathMode::AuthoredOnly);
            ok &= expectTrue("larger same-tier authored stroke replaced the stored one",
                authoredView.leaderPath && std::abs(authoredView.leaderPath->totalArcLength - 6.0f) < 0.01f);
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

    return ok ? 0 : 1;
}
