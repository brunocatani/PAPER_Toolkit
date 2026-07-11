#include "redux/MotionLibraryFormat.h"
#include "redux/RichMotionCaptureFormat.h"
#include "redux/SpatialReloadController.h"
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

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

    struct CapturedRawSummary
    {
        std::uint32_t callbackCount{ 0 };
        redux::rich_capture::StrokeTermination termination{};
        redux::rich_capture::ServingDecision decision{};
        std::uint32_t sampleCount{ 0 };
        std::uint64_t firstFrame{ 0 };
        std::uint64_t lastFrame{ 0 };
        float firstScale{ 0.0f };
        float lastScale{ 0.0f };
        bool terminalPresent{ false };
        bool terminalTrusted{ true };
        std::uint32_t peakIndex{ 0 };
        float peakExcursion{ 0.0f };
        bool candidateValid{ false };
    };

    void captureRawSummary(const redux::WeaponPartMotionLearner::RawCaptureView& capture, void* context)
    {
        auto& summary = *static_cast<CapturedRawSummary*>(context);
        ++summary.callbackCount;
        summary.termination = capture.termination;
        summary.decision = capture.servingDecision;
        summary.sampleCount = capture.sampleCount;
        summary.firstFrame = capture.rockFrameIndices && capture.sampleCount > 0 ? capture.rockFrameIndices[0] : 0;
        summary.lastFrame = capture.rockFrameIndices && capture.sampleCount > 0
            ? capture.rockFrameIndices[capture.sampleCount - 1]
            : 0;
        summary.firstScale = capture.scales && capture.sampleCount > 0 ? capture.scales[0] : 0.0f;
        summary.lastScale = capture.scales && capture.sampleCount > 0 ? capture.scales[capture.sampleCount - 1] : 0.0f;
        summary.terminalPresent = capture.terminalSamplePresent;
        summary.terminalTrusted = capture.terminalSample.trusted;
        summary.peakIndex = capture.peakSampleIndex;
        summary.peakExcursion = capture.peakExcursion;
        summary.candidateValid = capture.candidatePath.valid;
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

        // Stored rotation is playback data, not a hand-input axis. A path
        // with no translatable segment must remain parked.
        MotionPath rotationOnly{};
        rotationOnly.valid = true;
        constexpr float kQuarterTurn = 1.57079632679f;
        rotationOnly.totalArcLength = kQuarterTurn * kRotationArcRadiusGameUnits;
        for (std::uint32_t key = 0; key < kResampledKeyCount; ++key) {
            const float angle = kQuarterTurn * static_cast<float>(key) /
                static_cast<float>(kResampledKeyCount - 1);
            rotationOnly.keys[key].rotate = Quat{
                std::cos(angle * 0.5f), 0.0f, 0.0f, std::sin(angle * 0.5f)
            };
        }
        ok &= expectTrue("learner scrub remains parked on pure rotation",
            scrub(rotationOnly, 0.0f, Vec3{}).arcPosition < 0.001f);
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
        // The shipped F4NV-AMR file is the known-good learner-motion fixture.
        // Its original parts remain present byte-for-value, while the curated
        // profile reorganizes those exact recorded paths into explicit groups
        // and position-driven cycles. Only sound crossings are executable.
        using namespace redux::motion_library;
        using redux::spatial_reload::Controller;

        const auto profilePath = std::filesystem::path(PAPER_REDUX_TEST_SOURCE_DIR) /
            "data" / "MotionLibrary" / "F4NV-AMR.esp_00000F99.json";
        std::ifstream profileStream(profilePath, std::ios::binary);
        ok &= expectTrue("bundled F4NV-AMR profile opens", static_cast<bool>(profileStream));
        std::ostringstream profileBuffer;
        profileBuffer << profileStream.rdbuf();
        WeaponLibrary f4nv;
        std::string profileError;
        ok &= expectTrue("bundled F4NV-AMR spatial profile parses",
            parse(profileBuffer.str(), f4nv, &profileError));
        ok &= expectTrue("bundled F4NV-AMR spatial parse reports no error", profileError.empty());
        ok &= expectTrue("F4NV-AMR profile is curated learner movement format 2",
            f4nv.formatVersion == 2 && f4nv.curated && f4nv.parts.size() == 23 &&
                f4nv.spatialReload.used &&
                f4nv.spatialReload.runtimeMode ==
                    SpatialReloadRuntimeMode::LearnerMovementPreview);
        ok &= expectEqual("F4NV-AMR spatial group count", f4nv.spatialReload.groups.size(), std::size_t{ 2 });
        ok &= expectEqual("F4NV-AMR physical stage count", f4nv.spatialReload.stages.size(), std::size_t{ 4 });
        ok &= expectEqual("F4NV-AMR mapped finding count", f4nv.spatialReload.mappedEvents.size(), std::size_t{ 12 });
        ok &= expectTrue("source clip is provenance only",
            f4nv.spatialReload.sourceClip == "Animations\\44Pistol\\WPNReload.hkt");
        ok &= expectTrue("F4NV magazine group uses unique physical mesh grips",
            f4nv.spatialReload.groups.size() == 2 &&
                f4nv.spatialReload.groups[1].grips.size() == 3 &&
                f4nv.spatialReload.groups[1].grips[0].sourceName == "Object01" &&
                f4nv.spatialReload.groups[1].grips[1].sourceName == "Object02" &&
                f4nv.spatialReload.groups[1].grips[2].sourceName ==
                    "308MagSmalBullets:0" &&
                f4nv.spatialReload.groups[1].grips[0].omod.empty() &&
                f4nv.spatialReload.groups[1].grips[1].omod.empty() &&
                f4nv.spatialReload.groups[1].grips[2].omod.empty());
        ok &= expectTrue("P-Mag is connector evidence rather than a grip or driver",
                f4nv.spatialReload.groups[1].connectorEvidence.size() == 1 &&
                f4nv.spatialReload.groups[1].connectorEvidence[0] == "P-Mag" &&
                f4nv.spatialReload.groups[1].drivers[0].node == "WeaponMagazine");
        ok &= expectTrue("exact learner driver retains all 24 recorded keys",
            f4nv.spatialReload.stages[0].driverTracks[0].keys.size() == 24 &&
                f4nv.spatialReload.stages[0].driverTracks[0].keys[1].pathDistance > 0.0f);
        ok &= expectTrue("bolt stages cycle only at captured endpoints",
            std::abs(f4nv.spatialReload.stages[0].transitionFraction - 1.0f) < 0.0001f &&
                f4nv.spatialReload.stages[0].nextStageId == "bolt_close" &&
                std::abs(f4nv.spatialReload.stages[1].transitionFraction - 1.0f) < 0.0001f &&
                f4nv.spatialReload.stages[1].nextStageId == "bolt_open");
        ok &= expectTrue("magazine exchanges at 20 percent and enters at 80 percent",
            std::abs(f4nv.spatialReload.stages[2].transitionFraction - 0.20f) < 0.0001f &&
                f4nv.spatialReload.stages[2].nextStageId == "magazine_insert" &&
                std::abs(f4nv.spatialReload.stages[2].nextStageEntryFraction - 0.80f) < 0.0001f &&
                std::abs(f4nv.spatialReload.stages[3].entryFraction - 0.80f) < 0.0001f &&
                f4nv.spatialReload.stages[3].nextStageId == "magazine_remove");

        std::size_t soundFindingCount = 0;
        std::size_t visibilityFindingCount = 0;
        std::size_t gameplayFindingCount = 0;
        for (const auto& event : f4nv.spatialReload.mappedEvents) {
            soundFindingCount += event.kind == SpatialReloadMappedEventKind::Sound ? 1u : 0u;
            visibilityFindingCount += event.kind == SpatialReloadMappedEventKind::Visibility ? 1u : 0u;
            gameplayFindingCount += event.kind == SpatialReloadMappedEventKind::Gameplay ? 1u : 0u;
        }
        ok &= expectEqual("all seven mapped preview sounds retained", soundFindingCount, std::size_t{ 7 });
        ok &= expectEqual("both visibility findings retained", visibilityFindingCount, std::size_t{ 2 });
        ok &= expectEqual("all three gameplay findings retained inertly", gameplayFindingCount, std::size_t{ 3 });

        const auto containsTemporalField = [](const auto& self, const nlohmann::json& value) -> bool {
            static constexpr std::array<std::string_view, 9> blocked{
                "timeSeconds", "durationSeconds", "durationToleranceSeconds",
                "startSeconds", "endSeconds", "releaseSeconds", "fraction",
                "normalizedTime", "clipTime"
            };
            if (value.is_object()) {
                for (const auto& [name, child] : value.items()) {
                    for (const auto field : blocked) {
                        if (name == field) {
                            return true;
                        }
                    }
                    if (self(self, child)) {
                        return true;
                    }
                }
            } else if (value.is_array()) {
                for (const auto& child : value) {
                    if (self(self, child)) {
                        return true;
                    }
                }
            }
            return false;
        };
        const auto shippedJson = nlohmann::json::parse(profileBuffer.str());
        ok &= expectFalse("shipped spatial profile contains no temporal authority fields",
            containsTemporalField(containsTemporalField, shippedJson["spatialReload"]));
        ok &= expectTrue("movement-only contract is explicit in JSON",
            shippedJson["spatialReload"]["runtimeMode"] == "learnerMovementPreview" &&
                shippedJson["spatialReload"]["coordinateConventions"]["handProjection"] ==
                    "weapon-root-local-translation" &&
                shippedJson["spatialReload"].contains("sourceClip") &&
                shippedJson["spatialReload"].contains("mappedEvents") &&
                !shippedJson["spatialReload"].contains("activationClip") &&
                !shippedJson["spatialReload"].contains("events"));

        WeaponLibrary f4nvRoundTrip;
        profileError.clear();
        const auto roundTripText = serialize(f4nv);
        ok &= expectTrue("spatial profile serialize/parse round-trips",
            parse(roundTripText, f4nvRoundTrip, &profileError));
        ok &= expectTrue("spatial round-trip reports no error", profileError.empty());
        ok &= expectEqual("spatial round-trip stage count",
            f4nvRoundTrip.spatialReload.stages.size(), std::size_t{ 4 });
        ok &= expectEqual("spatial round-trip inherited follower count",
            f4nvRoundTrip.spatialReload.groups[1].drivers[0].inheritedFollowers.size(),
            std::size_t{ 8 });
        ok &= expectFalse("serialized spatial profile remains clock-free",
            containsTemporalField(
                containsTemporalField,
                nlohmann::json::parse(roundTripText)["spatialReload"]));

        Controller controller;
        const auto& profile = f4nv.spatialReload;
        using redux::weapon_part_motion_path::PoseSample;
        const auto makeInput = [&](bool grip, std::uint32_t groupIndex,
                                   std::uint64_t gripSequence,
                                   const PoseSample& handPose,
                                   bool triggerHeld = true) {
            Controller::FrameInput input{};
            input.hands[0] = Controller::HandInput{
                .gripActive = grip,
                .triggerHeld = triggerHeld,
                .groupIndex = groupIndex,
                .gripSequence = gripSequence,
                .posesValid = grip,
                .partPose = {},
                .handPose = handPose,
            };
            return input;
        };
        const auto stageIndex = [&](std::string_view id) {
            for (std::uint32_t i = 0; i < profile.stages.size(); ++i) {
                if (profile.stages[i].id == id) {
                    return i;
                }
            }
            return Controller::kInvalidIndex;
        };
        const auto anchoredTarget = [&](const SpatialReloadStage& stage,
                                        float entryFraction,
                                        const PoseSample& currentPart,
                                        float targetFraction) {
            const auto entry = redux::weapon_part_motion_scrub::poseAtArcPosition(
                stage.controlPath, entryFraction * stage.controlPath.totalArcLength);
            const auto target = redux::weapon_part_motion_scrub::poseAtArcPosition(
                stage.controlPath, targetFraction * stage.controlPath.totalArcLength);
            const auto rotationDelta = redux::weapon_part_motion_path::quatMultiply(
                target.rotate,
                redux::weapon_part_motion_path::quatConjugate(entry.rotate));
            return PoseSample{
                .translate = redux::weapon_part_motion_path::add(
                    currentPart.translate,
                    redux::weapon_part_motion_path::sub(
                        target.translate, entry.translate)),
                .rotate = redux::weapon_part_motion_path::quatMultiply(
                    rotationDelta, currentPart.rotate),
            };
        };
        const auto trackPoseAt = [](const SpatialReloadDriverTrack& track, float distance) {
            if (distance <= track.keys.front().pathDistance) {
                return track.keys.front().pose;
            }
            for (std::size_t i = 1; i < track.keys.size(); ++i) {
                if (distance <= track.keys[i].pathDistance) {
                    const float span = track.keys[i].pathDistance -
                        track.keys[i - 1].pathDistance;
                    const float t = span > 0.0f
                        ? (distance - track.keys[i - 1].pathDistance) / span
                        : 1.0f;
                    return redux::weapon_part_motion_path::lerpPose(
                        track.keys[i - 1].pose, track.keys[i].pose, t);
                }
            }
            return track.keys.back().pose;
        };

        std::map<std::string, std::uint32_t> emittedSoundCounts;
        const auto collectSounds = [&](const Controller::FrameOutput& output) {
            for (std::uint32_t i = 0; i < output.soundEventCount; ++i) {
                const auto index = output.soundEventIndices[i];
                ok &= expectTrue("controller sound index is in range",
                    index < profile.mappedEvents.size());
                if (index < profile.mappedEvents.size()) {
                    ok &= expectEqual("controller emits only sound findings",
                        profile.mappedEvents[index].kind,
                        SpatialReloadMappedEventKind::Sound);
                    ++emittedSoundCounts[profile.mappedEvents[index].id];
                }
            }
        };
        const auto expectTransitionContinuity = [&](const char* label,
                                                    const SpatialReloadStage& outgoing,
                                                    const Controller::FrameOutput& output) {
            bool continuous = output.driverCount == outgoing.driverTracks.size();
            const float gateDistance = outgoing.transitionFraction *
                outgoing.controlPath.totalArcLength;
            for (std::uint32_t i = 0; i < output.driverCount && continuous; ++i) {
                const std::string_view node(output.drivers[i].node.data());
                const SpatialReloadDriverTrack* oldTrack = nullptr;
                for (const auto& track : outgoing.driverTracks) {
                    if (track.node == node) {
                        oldTrack = &track;
                        break;
                    }
                }
                continuous = oldTrack &&
                    redux::weapon_part_motion_path::poseDistance(
                        trackPoseAt(*oldTrack, gateDistance),
                        output.drivers[i].target) < 0.002f;
            }
            ok &= expectTrue(label, continuous);
        };

        const auto boltOpenIndex = stageIndex("bolt_open");
        const auto boltCloseIndex = stageIndex("bolt_close");
        const auto magRemoveIndex = stageIndex("magazine_remove");
        const auto magInsertIndex = stageIndex("magazine_insert");
        ok &= expectTrue("all named F4NV-AMR stages resolve",
            boltOpenIndex != Controller::kInvalidIndex &&
                boltCloseIndex != Controller::kInvalidIndex &&
                magRemoveIndex != Controller::kInvalidIndex &&
                magInsertIndex != Controller::kInvalidIndex);

        auto output = controller.update(
            &profile, makeInput(true, 0, 1, {}));
        collectSounds(output);
        ok &= expectTrue("bolt preview begins from a physical grip without a clip",
            output.newGrip && output.active && output.groupIndex == 0 &&
                output.stageIndex == boltOpenIndex && output.driverCount == 3);

        PoseSample rotationOnlyHand{};
        rotationOnlyHand.rotate = redux::weapon_part_motion_path::Quat{
            0.70710677f, 0.0f, 0.70710677f, 0.0f
        };
        output = controller.update(
            &profile, makeInput(true, 0, 1, rotationOnlyHand, false));
        collectSounds(output);
        ok &= expectTrue("wrist rotation alone cannot advance learner movement",
            output.stageIndex == boltOpenIndex &&
                std::abs(output.pathFraction) < 0.0001f && !output.stageChanged);

        const auto boltOpenGate = anchoredTarget(
            profile.stages[boltOpenIndex], 0.0f, {}, 1.0f);
        bool changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 0, 1, boltOpenGate, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        ok &= expectTrue("bolt outgoing switches at its physical maximum",
            changed && output.stageIndex == boltCloseIndex &&
                std::abs(output.pathFraction) < 0.0001f &&
                std::abs(output.outwardFraction - 1.0f) < 0.0001f);
        expectTransitionContinuity(
            "bolt open-to-close handoff preserves driver poses",
            profile.stages[boltOpenIndex], output);

        const auto boltCloseGate = anchoredTarget(
            profile.stages[boltCloseIndex],
            profile.stages[boltCloseIndex].entryFraction,
            boltOpenGate,
            profile.stages[boltCloseIndex].transitionFraction);
        changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 0, 1, boltCloseGate, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        ok &= expectTrue("bolt incoming switches back only at seated minimum",
            changed && output.stageIndex == boltOpenIndex &&
                std::abs(output.pathFraction) < 0.0001f &&
                std::abs(output.outwardFraction) < 0.0001f);

        // A second full cycle under the same pinned grip must re-arm every
        // stage-local sound. This guards against one-shot-per-session latches.
        const auto secondBoltOpenGate = anchoredTarget(
            profile.stages[boltOpenIndex], 0.0f, boltCloseGate, 1.0f);
        changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 0, 1, secondBoltOpenGate, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        const auto secondBoltCloseGate = anchoredTarget(
            profile.stages[boltCloseIndex], 0.0f, secondBoltOpenGate, 1.0f);
        changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 0, 1, secondBoltCloseGate, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        ok &= expectTrue("reverse/end sounds re-arm on a continuous second cycle",
            changed && output.stageIndex == boltOpenIndex &&
                emittedSoundCounts["bolt_open"] >= 2 &&
                emittedSoundCounts["bolt_close"] >= 2 &&
                emittedSoundCounts["reload_end"] >= 2);

        output = controller.update(
            &profile, makeInput(false, Controller::kInvalidIndex, 0, {}));
        ok &= expectTrue("release ends preview drives without gameplay completion",
            !output.active && output.driverCount == 0 &&
                output.soundEventCount == 0);

        output = controller.update(
            &profile, makeInput(true, 1, 2, {}));
        collectSounds(output);
        ok &= expectTrue("magazine preview begins on outgoing group stage",
            output.newGrip && output.active && output.groupIndex == 1 &&
                output.stageIndex == magRemoveIndex && output.driverCount == 1);

        const auto& magRemove = profile.stages[magRemoveIndex];
        const auto magExchangePose = anchoredTarget(
            magRemove, magRemove.entryFraction, {}, magRemove.transitionFraction);
        changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 1, 2, magExchangePose, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        ok &= expectTrue("magazine exchanges at 20 percent of outward travel",
            changed && output.stageIndex == magInsertIndex &&
                std::abs(output.pathFraction - 0.80f) < 0.0002f &&
                std::abs(output.outwardFraction - 0.20f) < 0.0002f);
        expectTransitionContinuity(
            "20-percent magazine handoff has no driver teleport",
            magRemove, output);

        const auto& magInsert = profile.stages[magInsertIndex];
        const auto magSeatedPose = anchoredTarget(
            magInsert,
            magInsert.entryFraction,
            magExchangePose,
            magInsert.transitionFraction);
        changed = false;
        for (std::uint32_t step = 0; step < 128 && !changed; ++step) {
            output = controller.update(
                &profile, makeInput(true, 1, 2, magSeatedPose, false));
            collectSounds(output);
            changed = output.stageChanged;
        }
        ok &= expectTrue("seated magazine resets to outgoing stage at outward zero",
            changed && output.stageIndex == magRemoveIndex &&
                std::abs(output.pathFraction) < 0.0001f &&
                std::abs(output.outwardFraction) < 0.0001f);

        output = controller.update(
            &profile, makeInput(false, Controller::kInvalidIndex, 0, {}));
        output = controller.update(
            &profile, makeInput(true, 1, 3, {}));
        collectSounds(output);
        ok &= expectTrue("release resets magazine session to clean outgoing baseline",
            output.newGrip && output.stageIndex == magRemoveIndex &&
                std::abs(output.pathFraction) < 0.0001f);

        for (const auto* expected : {
                 "reload_start", "bolt_open", "bolt_close", "reload_end",
                 "mag_release", "mag_out", "mag_in" }) {
            ok &= expectTrue("reachable preview sound emitted",
                emittedSoundCounts[expected] > 0);
        }

        WeaponLibrary invalidProfile;
        auto autoWritable = shippedJson;
        autoWritable["curated"] = false;
        ok &= expectFalse("spatial profile cannot be auto-overwritable",
            parse(autoWritable.dump(), invalidProfile, nullptr));
        ok &= expectFalse("removed time-driven profile is rejected explicitly",
            parse(R"({"format":2,"weapon":{"plugin":"a.esp","id":"0x1"},"curated":true,"parts":[],"authoritativeReload":{}})",
                invalidProfile, nullptr));
        ok &= expectFalse("hand-edited wrong JSON field types fail closed without escaping",
            parse(R"({"format":"two","weapon":{"plugin":"a.esp","id":"0x1"},"parts":[]})",
                invalidProfile, nullptr));

        auto invalidOmod = nlohmann::json::parse(roundTripText);
        invalidOmod["spatialReload"]["groups"][0]["grips"][0]["omod"] = {
            { "plugin", "" }, { "id", "0x00000001" }
        };
        ok &= expectFalse("partial spatial OMOD identity fails closed",
            parse(invalidOmod.dump(), invalidProfile, nullptr));

        auto connectorGrip = nlohmann::json::parse(roundTripText);
        connectorGrip["spatialReload"]["groups"][1]["grips"][0]["source"] =
            "P-Mag";
        ok &= expectFalse("P-* connection point cannot become a physical grip",
            parse(connectorGrip.dump(), invalidProfile, nullptr));

        auto temporalInjection = nlohmann::json::parse(roundTripText);
        temporalInjection["spatialReload"]["stages"][0]["startSeconds"] = 0.0;
        ok &= expectFalse("any temporal field invalidates the spatial profile",
            parse(temporalInjection.dump(), invalidProfile, nullptr));

        auto wrongRuntimeMode = nlohmann::json::parse(roundTripText);
        wrongRuntimeMode["spatialReload"]["runtimeMode"] = "reloadReplacement";
        ok &= expectFalse("reload-replacement runtime mode is rejected",
            parse(wrongRuntimeMode.dump(), invalidProfile, nullptr));

        auto supersededOzzyMode = nlohmann::json::parse(roundTripText);
        supersededOzzyMode["spatialReload"]["runtimeMode"] = "movementPreview";
        supersededOzzyMode["spatialReload"]["authority"] = "curatedSpatialMovement";
        ok &= expectFalse("superseded full-pose Ozzy profile signature is rejected",
            parse(supersededOzzyMode.dump(), invalidProfile, nullptr));

        auto removedClipAuthority = nlohmann::json::parse(roundTripText);
        removedClipAuthority["spatialReload"]["activationClip"] =
            "Animations\\44Pistol\\WPNReload.hkt";
        ok &= expectFalse("removed activation clip authority is rejected",
            parse(removedClipAuthority.dump(), invalidProfile, nullptr));

        auto removedExecutableEvents = nlohmann::json::parse(roundTripText);
        removedExecutableEvents["spatialReload"]["events"] = nlohmann::json::array();
        ok &= expectFalse("removed executable event surface is rejected",
            parse(removedExecutableEvents.dump(), invalidProfile, nullptr));

        auto brokenCycle = nlohmann::json::parse(roundTripText);
        brokenCycle["spatialReload"]["stages"][1]["nextStage"] = "bolt_close";
        ok &= expectFalse("stage cycle cannot cross interaction groups",
            parse(brokenCycle.dump(), invalidProfile, nullptr));

        auto wrongEventPose = nlohmann::json::parse(roundTripText);
        for (auto& encodedEvent : wrongEventPose["spatialReload"]["mappedEvents"]) {
            if (encodedEvent["trigger"] == "pathPosition") {
                encodedEvent["targetPose"][0] = 999.0;
                break;
            }
        }
        ok &= expectFalse("mapped event pose must agree with its physical path fraction",
            parse(wrongEventPose.dump(), invalidProfile, nullptr));
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

    {
        // Rich learner capture is an evidence plane, not a winner-only view:
        // exact frame/scale samples survive completion, untrusted discard,
        // and a weapon-change flush.
        using namespace redux;
        using namespace redux::weapon_part_motion_path;

        static WeaponPartMotionLearner learner{};
        learner.reset();
        CapturedRawSummary captured{};
        learner.setRawCaptureSink(&captureRawSummary, &captured);
        std::uint64_t frame = 1000;
        const auto observe = [&](const PoseSample& pose, float scale, bool trusted) {
            learner.beginObservationFrame();
            learner.observe(WeaponPartMotionLearner::Observation{
                .weaponFormId = 0x01020304,
                .omodFormId = 0x05060708,
                .sourceName = "WeaponBolt",
                .pose = pose,
                .scale = scale,
                .trusted = trusted,
                .rockFrameIndex = frame++,
                .weaponGenerationKey = 0xABCDEF,
                .catalogPartId = 7,
                .bodyId = 12,
                .nodePath = "/[2]WeaponBolt",
            });
        };

        PoseSample rest{};
        for (std::uint32_t i = 0; i <= kRestStableFramesToArm; ++i) {
            observe(rest, 0.75f, true);
        }
        PoseSample moving = rest;
        for (std::uint32_t i = 1; i <= 8; ++i) {
            moving.translate.y = static_cast<float>(i);
            observe(moving, 0.75f + static_cast<float>(i) * 0.01f, true);
        }
        for (std::uint32_t i = 0; i <= kRestReturnFramesToComplete && captured.callbackCount == 0; ++i) {
            observe(moving, 0.83f, true);
        }
        ok &= expectEqual("rich capture receives completed recording", captured.callbackCount, 1u);
        ok &= expectEqual("rich completion termination retained", captured.termination,
            rich_capture::StrokeTermination::Settled);
        ok &= expectEqual("rich completion serving decision retained", captured.decision,
            rich_capture::ServingDecision::StoredPrimary);
        ok &= expectTrue("rich completion keeps the full raw cycle", captured.sampleCount > 8);
        ok &= expectEqual("rich completion keeps rest frame", captured.firstFrame, 1008ull);
        ok &= expectTrue("rich completion keeps exact last frame", captured.lastFrame > captured.firstFrame);
        ok &= expectTrue("rich completion keeps scale", std::abs(captured.firstScale - 0.75f) < 0.001f &&
            std::abs(captured.lastScale - 0.83f) < 0.001f);
        ok &= expectTrue("rich completion keeps derived candidate", captured.candidateValid && captured.peakExcursion > 7.5f);

        // Re-arm at rest, begin another stroke, then feed an explicitly
        // untrusted PAPER-driven sample. The preceding evidence and terminal
        // sample must both survive even though serving stores nothing.
        for (std::uint32_t i = 0; i <= kRestStableFramesToArm + 1; ++i) {
            observe(rest, 1.25f, true);
        }
        moving = rest;
        moving.translate.x = 2.0f;
        observe(moving, 1.25f, true);
        moving.translate.x = 3.0f;
        observe(moving, 1.25f, true);
        observe(moving, 1.25f, false);
        ok &= expectEqual("rich capture receives untrusted discard", captured.callbackCount, 2u);
        ok &= expectEqual("untrusted termination retained", captured.termination,
            rich_capture::StrokeTermination::UntrustedDrive);
        ok &= expectEqual("discard is never evaluated for serving", captured.decision,
            rich_capture::ServingDecision::NotEvaluated);
        ok &= expectTrue("untrusted terminal sample retained",
            captured.terminalPresent && !captured.terminalTrusted && captured.sampleCount >= 3);

        for (std::uint32_t i = 0; i <= kRestStableFramesToArm; ++i) {
            observe(rest, 1.0f, true);
        }
        moving = rest;
        moving.translate.z = 1.0f;
        observe(moving, 1.0f, true);
        learner.resetRecorders(rich_capture::StrokeTermination::WeaponChanged);
        ok &= expectEqual("partial stroke is flushed on weapon change", captured.callbackCount, 3u);
        ok &= expectEqual("weapon-change termination retained", captured.termination,
            rich_capture::StrokeTermination::WeaponChanged);

        // Capacity termination is evidence too: all 720 buffered samples
        // survive, and the next trusted sample is retained as the terminal
        // observation rather than silently resetting the recorder.
        for (std::uint32_t i = 0; i <= kRestStableFramesToArm; ++i) {
            observe(rest, 1.0f, true);
        }
        for (std::uint32_t i = 1; i <= kMaxRecordingSamples; ++i) {
            moving = rest;
            moving.translate.x = static_cast<float>(i) * 0.2f;
            observe(moving, 1.0f, true);
        }
        ok &= expectEqual("capacity discard is captured", captured.callbackCount, 4u);
        ok &= expectEqual("capacity termination retained", captured.termination,
            rich_capture::StrokeTermination::SampleCapacity);
        ok &= expectEqual("all bounded samples survive capacity termination",
            captured.sampleCount, kMaxRecordingSamples);
        ok &= expectTrue("capacity terminal observation is retained and trusted",
            captured.terminalPresent && captured.terminalTrusted);
        learner.setRawCaptureSink(nullptr, nullptr);
    }

    {
        // JSONL evidence schema: 64-bit values are strings, identities are
        // load-order independent, role names accompany raw values, and raw
        // samples use the versioned compact layout.
        using namespace redux;
        using namespace redux::rich_capture;

        WeaponSnapshotEvent snapshot{};
        snapshot.context.sessionId = "session-test";
        snapshot.context.sequence = 9'007'199'254'740'993ull;
        snapshot.context.capturedAtUnixMs = 123456789;
        snapshot.context.rockFrameIndex = 9'007'199'254'740'994ull;
        snapshot.context.weaponGenerationKey = 0x123456789ABCDEF0ull;
        snapshot.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        snapshot.context.weapon.runtimeFormId = 0xFE001234;
        snapshot.context.paperVersion = "0.1.0";
        snapshot.context.rockApiVersion = 1;
        snapshot.classification.available = true;
        snapshot.classification.valid = true;
        snapshot.classification.keywordFlags = 1ull << 2;
        snapshot.classification.sizeClass = 2;
        snapshot.nodes.push_back(NodeSnapshot{ .id = 0, .parentId = -1, .name = "Weapon", .rootRelativePath = "/" });
        EvidenceSnapshot evidence{};
        evidence.id = 4;
        evidence.providerSourceName = "WeaponMagazine#0";
        evidence.sourceNodeId = 0;
        evidence.sourceNodePath = "/[4]WeaponMagazine";
        evidence.partKind = 7;
        evidence.reloadRole = 1;
        evidence.omod.ref = { "ExampleParts.esp", 0x77 };
        evidence.attachPoint.ref = { "Fallout4.esm", 0x123 };
        evidence.providerPointCount = 5;
        evidence.queriedPointCount = 5;
        evidence.scheduledPointCount = 3;
        evidence.geometryDelivery = GeometryDelivery::Chunks;
        evidence.pointCloudTruncated = true;
        snapshot.evidence.push_back(std::move(evidence));

        const auto snapshotText = serializeLine(Event{ std::move(snapshot) });
        const auto snapshotJson = nlohmann::json::parse(snapshotText, nullptr, false);
        ok &= expectFalse("rich snapshot JSON parses", snapshotJson.is_discarded());
        if (!snapshotJson.is_discarded()) {
            ok &= expectTrue("rich schema and event are named",
                snapshotJson["schema"] == "paper-redux-motion-capture" &&
                    snapshotJson["event"] == "weaponSnapshot");
            ok &= expectTrue("64-bit sequence is lossless string",
                snapshotJson["sequence"] == "9007199254740993" &&
                    snapshotJson["rockFrame"] == "9007199254740994");
            ok &= expectTrue("weapon form ref is plugin-local",
                snapshotJson["weapon"]["ref"]["plugin"] == "ExampleWeapon.esp" &&
                    snapshotJson["weapon"]["ref"]["id"] == "0x00001234");
            ok &= expectTrue("raw role and informative name coexist",
                snapshotJson["evidence"][0]["partKind"] == 7 &&
                    snapshotJson["evidence"][0]["partKindName"] == "Magazine" &&
                    snapshotJson["evidence"][0]["reloadRoleName"] == "MagazineBody");
            ok &= expectTrue("snapshot schedules deferred point geometry",
                snapshotJson["evidence"][0]["scheduledPointCount"] == 3 &&
                    snapshotJson["evidence"][0]["geometryDelivery"] == "chunks" &&
                    snapshotJson["evidence"][0]["geometryDeferred"] == true &&
                    snapshotJson["evidence"][0]["pointsWeaponLocalGame"].empty());
        }

        GeometryChunkEvent firstChunk{};
        firstChunk.context.sessionId = "session-test";
        firstChunk.context.sequence = 9'007'199'254'740'995ull;
        firstChunk.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        firstChunk.snapshotSequence = 9'007'199'254'740'993ull;
        firstChunk.evidenceId = 4;
        firstChunk.bodyId = 27;
        firstChunk.pointOffset = 0;
        firstChunk.totalPointCount = 3;
        firstChunk.chunkIndex = 0;
        firstChunk.pointsWeaponLocalGame = {
            { 1.0f, 2.0f, 3.0f },
            { 4.0f, 5.0f, 6.0f },
        };
        const auto firstChunkJson = nlohmann::json::parse(
            serializeLine(Event{ std::move(firstChunk) }), nullptr, false);
        ok &= expectFalse("first geometry chunk JSON parses", firstChunkJson.is_discarded());
        if (!firstChunkJson.is_discarded()) {
            ok &= expectTrue("first geometry chunk links exactly to snapshot",
                firstChunkJson["event"] == "geometryChunk" &&
                    firstChunkJson["snapshotSequence"] == "9007199254740993" &&
                    firstChunkJson["evidenceId"] == 4 && firstChunkJson["bodyIdDiagnostic"] == 27);
            ok &= expectTrue("first geometry chunk preserves range and point order",
                firstChunkJson["pointOffset"] == 0 && firstChunkJson["totalPointCount"] == 3 &&
                    firstChunkJson["chunkIndex"] == 0 && firstChunkJson["pointCount"] == 2 &&
                    firstChunkJson["pointsWeaponLocalGame"][0] == nlohmann::json::array({ 1.0f, 2.0f, 3.0f }) &&
                    firstChunkJson["pointsWeaponLocalGame"][1] == nlohmann::json::array({ 4.0f, 5.0f, 6.0f }));
            ok &= expectTrue("non-final geometry flags stay false",
                firstChunkJson["finalChunk"] == false && firstChunkJson["sourceComplete"] == false &&
                    firstChunkJson["evidenceComplete"] == false && firstChunkJson["snapshotComplete"] == false);
        }

        GeometryChunkEvent finalChunk{};
        finalChunk.context.sessionId = "session-test";
        finalChunk.context.sequence = 9'007'199'254'740'996ull;
        finalChunk.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        finalChunk.snapshotSequence = 9'007'199'254'740'993ull;
        finalChunk.evidenceId = 4;
        finalChunk.bodyId = 27;
        finalChunk.pointOffset = 2;
        finalChunk.totalPointCount = 3;
        finalChunk.chunkIndex = 1;
        finalChunk.finalChunk = true;
        // Three scheduled points were delivered, but the provider reported
        // five. Final delivery and source fidelity must remain independent.
        finalChunk.sourceComplete = false;
        finalChunk.snapshotComplete = true;
        finalChunk.pointsWeaponLocalGame = { { 7.0f, 8.0f, 9.0f } };
        const auto finalChunkJson = nlohmann::json::parse(
            serializeLine(Event{ std::move(finalChunk) }), nullptr, false);
        ok &= expectFalse("final geometry chunk JSON parses", finalChunkJson.is_discarded());
        if (!finalChunkJson.is_discarded()) {
            ok &= expectTrue("final geometry chunk continues exact source order",
                finalChunkJson["pointOffset"] == 2 && finalChunkJson["chunkIndex"] == 1 &&
                    finalChunkJson["pointCount"] == 1 &&
                    finalChunkJson["pointsWeaponLocalGame"][0] == nlohmann::json::array({ 7.0f, 8.0f, 9.0f }));
            ok &= expectTrue("final geometry flags are independent and explicit",
                finalChunkJson["finalChunk"] == true && finalChunkJson["sourceComplete"] == false &&
                    finalChunkJson["evidenceComplete"] == false && finalChunkJson["snapshotComplete"] == true);
        }

        RawStrokeEvent stroke{};
        stroke.context.sessionId = "session-test";
        stroke.context.sequence = 2;
        stroke.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        stroke.sourceName = "WeaponBolt";
        stroke.termination = StrokeTermination::UntrustedDrive;
        stroke.servingDecision = ServingDecision::NotEvaluated;
        stroke.samples.push_back(RawSample{
            .rockFrameIndex = 42,
            .scale = 0.5f,
            .clip = ClipSampleContext{
                .activityId = 77,
                .scrubSessionId = 88,
                .concurrentActivityCount = 2,
                .fraction = 0.25f,
                .localTimeSeconds = 0.5f,
            },
        });
        stroke.terminalSamplePresent = true;
        stroke.terminalSample.trusted = false;
        const auto strokeJson = nlohmann::json::parse(serializeLine(Event{ std::move(stroke) }), nullptr, false);
        ok &= expectFalse("rich stroke JSON parses", strokeJson.is_discarded());
        if (!strokeJson.is_discarded()) {
            ok &= expectTrue("raw sample layout is explicit and compact",
                strokeJson["rawSampleLayout"].size() == 15 && strokeJson["samples"][0].size() == 15);
            ok &= expectTrue("clip definition, scrub, and layering identities remain distinct",
                strokeJson["samples"][0][10] == "77" && strokeJson["samples"][0][11] == "88" &&
                    strokeJson["samples"][0][12] == 2);
            ok &= expectTrue("termination and terminal trust survive",
                strokeJson["termination"] == "untrustedDrive" && strokeJson["terminalSample"][9] == 0);
        }

        AuthoredClipEvent clip{};
        clip.context.sessionId = "session-test";
        clip.context.sequence = 7;
        clip.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        clip.activityId = 9'007'199'254'740'997ull;
        clip.activatedClip = true;
        clip.animationName = "ReloadMagazine";
        clip.durationSeconds = 1.5f;
        clip.rawTransformTrackCount = 22;
        clip.capturedWeaponTrackCount = 1;
        ClipTrack clipTrack{};
        clipTrack.boneName = "WeaponMagazine";
        weapon_part_motion_path::PoseSample clipPose{};
        clipPose.translate = { 10.0f, 20.0f, 30.0f };
        clipTrack.samples.push_back(clipPose);
        clipTrack.scaleSamples.push_back({ 1.0f, 1.1f, 1.2f });
        clip.weaponTracks.push_back(std::move(clipTrack));
        clip.annotations.push_back({ 0.25f, "Reload", "magazine_release" });
        clip.triggers.push_back({ 0.75f, 18, "reloadComplete" });
        const auto clipJson = nlohmann::json::parse(serializeLine(Event{ std::move(clip) }), nullptr, false);
        ok &= expectFalse("rich authored clip JSON parses", clipJson.is_discarded());
        if (!clipJson.is_discarded()) {
            ok &= expectTrue("authored clip activity correlation is lossless",
                clipJson["event"] == "authoredClip" && clipJson["activityId"] == "9007199254740997");
            ok &= expectTrue("authored clip track and scale survive",
                clipJson["weaponTracks"][0]["bone"] == "WeaponMagazine" &&
                    clipJson["weaponTracks"][0]["samples"][0] ==
                        nlohmann::json::array({ 10.0f, 20.0f, 30.0f, 1.0f, 0.0f, 0.0f, 0.0f }) &&
                    clipJson["weaponTracks"][0]["scaleSamples"][0] ==
                        nlohmann::json::array({ 1.0f, 1.1f, 1.2f }));
            ok &= expectTrue("authored clip annotations and triggers survive",
                clipJson["annotations"][0]["track"] == "Reload" &&
                    clipJson["annotations"][0]["text"] == "magazine_release" &&
                    clipJson["triggers"][0]["eventId"] == 18 &&
                    clipJson["triggers"][0]["eventName"] == "reloadComplete");
        }

        CaptureGapEvent gap{};
        gap.context.sessionId = "session-test";
        gap.context.sequence = 8;
        gap.context.weapon.ref = { "ExampleWeapon.esp", 0x1234 };
        gap.relatedSnapshotSequence = 9'007'199'254'740'997ull;
        gap.firstDroppedSequence = 9'007'199'254'740'998ull;
        gap.lastDroppedSequence = 9'007'199'254'740'999ull;
        gap.droppedEventCount = 2;
        gap.reason = "invalid ";
        gap.reason.push_back(static_cast<char>(0xC3));
        gap.reason.push_back('(');
        const auto gapJson = nlohmann::json::parse(serializeLine(Event{ std::move(gap) }), nullptr, false);
        ok &= expectFalse("capture gap with invalid UTF-8 still serializes", gapJson.is_discarded());
        if (!gapJson.is_discarded()) {
            const auto reason = gapJson["reason"].get<std::string>();
            ok &= expectTrue("capture gap range remains lossless",
                gapJson["relatedSnapshotSequence"] == "9007199254740997" &&
                gapJson["firstDroppedSequence"] == "9007199254740998" &&
                    gapJson["lastDroppedSequence"] == "9007199254740999" &&
                    gapJson["droppedEventCount"] == 2);
            ok &= expectTrue("invalid UTF-8 is replaced instead of dropping the event",
                reason.find("\xEF\xBF\xBD") != std::string::npos && reason.back() == '(');
        }

        GeometryChunkEvent sizedChunk{};
        sizedChunk.context.sessionId = "sized";
        sizedChunk.pointsWeaponLocalGame.reserve(128);
        Event emptyChunkEvent{ GeometryChunkEvent{} };
        Event sizedChunkEvent{ std::move(sizedChunk) };
        ok &= expectTrue("owned-byte estimate accounts for deferred geometry capacity",
            estimateOwnedBytes(sizedChunkEvent) >=
                estimateOwnedBytes(emptyChunkEvent) + 128 * sizeof(Point3));
    }

    return ok ? 0 : 1;
}
