#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "redux/WeaponPartMotionPathPolicy.h"

/*
 * Pure policy that turns baked animation-clip track samples into authored
 * stroke groups. Input is engine-free: per-track pose arrays sampled at
 * uniform clip times (the harvest hook produces them by calling the engine's
 * own track sampler). Output is one stroke group per moving "leader" track:
 * the leader's rest→peak stroke resampled exactly like learner paths (so the
 * existing scrub consumes it unchanged), plus followers — tracks that move
 * RIGIDLY with the leader (constant pairwise distance, the learner's
 * co-movement criterion), driven off one scrub parameter. This is what
 * runtime observation could not give us: per-bone isolated tracks.
 */
namespace redux::weapon_clip_stroke
{
    using weapon_part_motion_path::MotionPath;
    using weapon_part_motion_path::PoseSample;
    using weapon_part_motion_path::kMinPathExcursionGameUnits;
    using weapon_part_motion_path::kResampledKeyCount;
    using weapon_part_motion_path::lerpPose;
    using weapon_part_motion_path::poseDistance;

    // Legacy live-activation capture remains fixed at 64 samples so its graph-
    // thread hook stays bounded. Exact off-screen preharvest may accumulate up
    // to the learner's 720-sample ceiling incrementally across frames.
    inline constexpr std::uint32_t kClipSampleCount = 64;
    inline constexpr std::uint32_t kMaxClipSampleCount = 720;
    // Legacy live capture remains bounded independently from exact preharvest;
    // its graph-thread packets must not inherit the 720-sample offline buffer.
    inline constexpr std::uint32_t kMaxTracksPerClip = 64;
    // Matches PAPER's full observation/evidence ceiling for complex weapon
    // assemblies. Exact storage is process-lifetime heap/member memory.
    inline constexpr std::uint32_t kMaxExactTracksPerClip = 128;
    // Sized for real assemblies: a magazine leads its whole bullet stack,
    // a slide carries its sights and decorative riders.
    inline constexpr std::uint32_t kMaxFollowers = 10;
    inline constexpr std::uint32_t kMaxGroupsPerClip = kMaxExactTracksPerClip;
    inline constexpr std::size_t kMaxBoneName = 64;
    // Followers move less than leaders (an ejector nudge vs the slide stroke);
    // anything below this is sampling noise and stays undriven.
    inline constexpr float kFollowerMinExcursionGameUnits = 0.15f;
    // A follower's distance to its leader may drift at most this much over
    // the stroke and still count as rigid co-movement. Shared with the
    // learner's observed co-movement grouping so authored and learned
    // groups mean the same thing by "follower".
    inline constexpr float kRigidFollowerDistanceToleranceGameUnits = 0.6f;
    // Offline exact clips must be reduced to the same unit of evidence the
    // learner records: one stable pose -> motion -> first stable pose (or the
    // first unambiguous reversal). This prevents later reload carry motion and
    // repeated automatic-fire cycles from becoming one enormous guide.
    // Eight transitions at the exact sampler's 120 Hz is the learner's
    // roughly 67 ms stable-pose interval. Compare the whole window against a
    // tight exact-data radius; adjacent-frame stillness would misclassify a
    // slow but continuous authored motion as a dwell.
    inline constexpr std::uint32_t kStageStableTransitions = 8;
    inline constexpr float kStageDwellPoseDistanceGameUnits = 0.05f;
    inline constexpr std::uint32_t kStageRetreatConfirmSamples = 2;
    inline constexpr float kStageRetreatMinimumGameUnits = 0.15f;
    inline constexpr float kStageRetreatFraction = 0.08f;

    template <std::uint32_t SampleCapacity>
    struct BasicTrackSamples
    {
        std::array<char, kMaxBoneName> boneName{};
        std::uint32_t sampleCount{ 0 };
        std::array<PoseSample, SampleCapacity> samples{};
        // Exact authored hkQs scale vector. Serving paths currently preserve
        // scene-observed scalar rest scale, but the evidence archive retains
        // all three clip-authored components for offline analysis.
        std::array<weapon_part_motion_path::Vec3, SampleCapacity> scales{};
    };

    using TrackSamples = BasicTrackSamples<kClipSampleCount>;
    using ExactTrackSamples = BasicTrackSamples<kMaxClipSampleCount>;

    enum class AuthoredClipSource : std::uint8_t
    {
        // A binding found loaded on a graph without proof that it belongs to
        // the exact equipped weapon configuration.
        LoadedGraphFallback = 0,
        // Legacy evidence captured when the live graph activated the clip.
        ActivatedClip = 1,
        // Exact AnimationFileData path from the equipped weapon's off-screen
        // first-person subgraph, loaded and sampled without playback.
        ExactWeaponPreharvest = 2,
    };

    enum class AuthoredTrackSpace : std::uint8_t
    {
        // Legacy clip tracks expressed in the old rig-bone frame and requiring
        // PAPER's historical calibrated basis conversion.
        RigBoneLocal = 0,
        // Bone hierarchy reconstructed relative to the animation Weapon bone.
        // This is the native preharvest contract and needs no learned basis.
        WeaponRootLocal = 1,
    };

    [[nodiscard]] inline constexpr bool isWeaponSpecificSource(const AuthoredClipSource source)
    {
        return source != AuthoredClipSource::LoadedGraphFallback;
    }

    [[nodiscard]] inline constexpr bool isExactWeaponPreharvest(const AuthoredClipSource source)
    {
        return source == AuthoredClipSource::ExactWeaponPreharvest;
    }

    struct AuthoredFollower
    {
        std::array<char, kMaxBoneName> boneName{};
        std::array<PoseSample, kResampledKeyCount> keys{};
        // Weapon-root-local rest scale of the follower node, captured at
        // attribution time; strokes do not animate scale, drives need one.
        float restScale{ 1.0f };
    };

    struct AuthoredStrokeGroup
    {
        std::array<char, kMaxBoneName> leaderBoneName{};
        AuthoredClipSource source{ AuthoredClipSource::LoadedGraphFallback };
        AuthoredTrackSpace trackSpace{ AuthoredTrackSpace::RigBoneLocal };
        // Animation name from the activating hkbClipGenerator (empty on
        // the walk path or when the read fails); diagnostics/provenance.
        std::array<char, kMaxBoneName> clipAnimationName{};
        std::uint32_t sourceSampleStart{ 0 };
        std::uint32_t sourceSamplePeak{ 0 };
        std::uint32_t sourceSampleEnd{ 0 };
        MotionPath leaderPath{};
        std::uint32_t followerCount{ 0 };
        std::array<AuthoredFollower, kMaxFollowers> followers{};
    };

    struct StrokeSampleWindow
    {
        bool valid{ false };
        std::uint32_t firstSample{ 0 };
        std::uint32_t lastSample{ 0 };
    };

    template <std::uint32_t SampleCapacity>
    [[nodiscard]] inline bool stageWindowIsStill(
        const BasicTrackSamples<SampleCapacity>& track,
        const std::uint32_t firstSample,
        const std::uint32_t lastSample)
    {
        if (firstSample >= track.sampleCount ||
            lastSample >= track.sampleCount ||
            firstSample > lastSample) {
            return false;
        }
        const auto& anchor = track.samples[firstSample];
        for (std::uint32_t sample = firstSample + 1;
             sample <= lastSample;
             ++sample) {
            if (poseDistance(track.samples[sample], anchor) >
                kStageDwellPoseDistanceGameUnits) {
                return false;
            }
        }
        return true;
    }

    /*
     * Find the first bounded manipulation stage in a uniformly sampled part
     * track. Sample zero is the clip's authored rest/reference. Once the part
     * leaves that pose, the stage ends at the first stable dwell or at the
     * first confirmed retreat from an excursion peak. The latter is what
     * turns an automatic-fire clip into one bolt stroke even when the bolt has
     * no long dwell between cycles.
     */
    template <std::uint32_t SampleCapacity>
    [[nodiscard]] inline StrokeSampleWindow findFirstMotionStage(
        const BasicTrackSamples<SampleCapacity>& track)
    {
        if (track.sampleCount < 2 || track.sampleCount > SampleCapacity) {
            return {};
        }

        const auto& rest = track.samples[0];
        std::uint32_t motionStart = 0;
        for (std::uint32_t sample = 1; sample < track.sampleCount; ++sample) {
            if (poseDistance(track.samples[sample], rest) >=
                kMinPathExcursionGameUnits) {
                motionStart = sample;
                break;
            }
        }
        if (motionStart == 0) {
            return {};
        }

        float peakExcursion = 0.0f;
        std::uint32_t peakSample = motionStart;
        std::uint32_t retreatSamples = 0;
        for (std::uint32_t sample = motionStart;
             sample < track.sampleCount;
             ++sample) {
            const float excursion = poseDistance(track.samples[sample], rest);
            if (excursion > peakExcursion) {
                peakExcursion = excursion;
                peakSample = sample;
                retreatSamples = 0;
            }

            if (peakExcursion >= kMinPathExcursionGameUnits &&
                sample >= motionStart + kStageStableTransitions) {
                const auto stableStart = sample - kStageStableTransitions;
                if (stageWindowIsStill(track, stableStart, sample)) {
                    return StrokeSampleWindow{
                        .valid = true,
                        .firstSample = 0,
                        .lastSample = stableStart,
                    };
                }
            }

            const float retreatThreshold = (std::max)(
                kStageRetreatMinimumGameUnits,
                peakExcursion * kStageRetreatFraction);
            if (peakExcursion >= kMinPathExcursionGameUnits &&
                excursion <= peakExcursion - retreatThreshold) {
                ++retreatSamples;
                if (retreatSamples >= kStageRetreatConfirmSamples) {
                    return StrokeSampleWindow{
                        .valid = true,
                        .firstSample = 0,
                        .lastSample = peakSample,
                    };
                }
            } else if (sample != peakSample) {
                retreatSamples = 0;
            }
        }

        return peakExcursion >= kMinPathExcursionGameUnits
            ? StrokeSampleWindow{
                  .valid = true,
                  .firstSample = 0,
                  .lastSample = track.sampleCount - 1,
              }
            : StrokeSampleWindow{};
    }

    template <std::uint32_t SampleCapacity>
    inline float trackExcursion(const BasicTrackSamples<SampleCapacity>& track)
    {
        float excursion = 0.0f;
        for (std::uint32_t i = 1; i < track.sampleCount; ++i) {
            const float candidate = poseDistance(track.samples[i], track.samples[0]);
            if (candidate > excursion) {
                excursion = candidate;
            }
        }
        return excursion;
    }

    template <std::uint32_t SampleCapacity>
    inline PoseSample poseAtSamplePosition(
        const BasicTrackSamples<SampleCapacity>& track,
        float samplePosition)
    {
        if (track.sampleCount == 0) {
            return {};
        }
        if (samplePosition <= 0.0f) {
            return track.samples[0];
        }
        const auto lastIndex = track.sampleCount - 1;
        if (samplePosition >= static_cast<float>(lastIndex)) {
            return track.samples[lastIndex];
        }
        const auto index = static_cast<std::uint32_t>(samplePosition);
        return lerpPose(track.samples[index], track.samples[index + 1], samplePosition - static_cast<float>(index));
    }

    /*
     * Leader path build: identical stroke semantics to the learner's
     * buildPathFromRecording (rest→peak, arc-uniform keys) but also reports
     * each key's fractional source-sample position so followers can be
     * sampled at the same clip times.
     */
    template <std::uint32_t SampleCapacity>
    inline bool buildLeaderPath(
        const BasicTrackSamples<SampleCapacity>& track,
        MotionPath& outPath,
        std::array<float, kResampledKeyCount>& outKeySamplePositions,
        StrokeSampleWindow* outWindow = nullptr,
        std::uint32_t* outPeakSample = nullptr)
    {
        outPath = MotionPath{};
        outKeySamplePositions = {};
        if (outWindow) {
            *outWindow = {};
        }
        if (outPeakSample) {
            *outPeakSample = 0;
        }

        const auto window = findFirstMotionStage(track);
        if (!window.valid || window.lastSample <= window.firstSample) {
            return false;
        }

        std::array<float, kResampledKeyCount> windowKeyPositions{};
        std::uint32_t windowPeak = 0;
        const auto windowSampleCount =
            window.lastSample - window.firstSample + 1;
        if (!weapon_part_motion_path::buildPathFromRecording(
                track.samples.data() + window.firstSample,
                windowSampleCount,
                outPath,
                windowKeyPositions.data(),
                &windowPeak)) {
            return false;
        }

        for (std::uint32_t key = 0; key < kResampledKeyCount; ++key) {
            outKeySamplePositions[key] =
                static_cast<float>(window.firstSample) +
                windowKeyPositions[key];
        }
        if (outWindow) {
            *outWindow = window;
        }
        if (outPeakSample) {
            *outPeakSample = window.firstSample + windowPeak;
        }
        return true;
    }

    /*
     * Build one stroke group per moving track (leaders). A follower is a
     * track that moves RIGIDLY with the leader over the leader's stroke —
     * pairwise distance held constant (the learner's co-movement criterion),
     * not merely co-timed. A reload clip moves the mag and the bolt in
     * overlapping windows, but they are separate strokes: attaching one as
     * the other's follower replayed the whole clip off a single grab
     * (in-game 2026-07-04: pulling the AK bolt drove the mag and bullets
     * down and out of the weapon).
     */
    template <std::uint32_t SampleCapacity>
    inline std::uint32_t buildAuthoredGroups(
        const BasicTrackSamples<SampleCapacity>* tracks,
        std::uint32_t trackCount,
        AuthoredStrokeGroup* outGroups,
        std::uint32_t maxGroups)
    {
        if (!tracks || !outGroups || maxGroups == 0) {
            return 0;
        }

        std::array<float, kMaxExactTracksPerClip> excursions{};
        const auto boundedTrackCount =
            (std::min)(trackCount, kMaxExactTracksPerClip);
        for (std::uint32_t i = 0; i < boundedTrackCount; ++i) {
            excursions[i] = trackExcursion(tracks[i]);
        }

        std::uint32_t groupCount = 0;
        for (std::uint32_t leader = 0; leader < boundedTrackCount && groupCount < maxGroups && groupCount < kMaxGroupsPerClip; ++leader) {
            if (excursions[leader] < kMinPathExcursionGameUnits) {
                continue;
            }
            AuthoredStrokeGroup group{};
            std::array<float, kResampledKeyCount> keyPositions{};
            StrokeSampleWindow stageWindow{};
            if (!buildLeaderPath(
                    tracks[leader],
                    group.leaderPath,
                    keyPositions,
                    &stageWindow,
                    &group.sourceSamplePeak)) {
                continue;
            }
            group.leaderBoneName = tracks[leader].boneName;
            group.sourceSampleStart = stageWindow.firstSample;
            group.sourceSampleEnd = stageWindow.lastSample;

            for (std::uint32_t follower = 0; follower < boundedTrackCount && group.followerCount < kMaxFollowers; ++follower) {
                if (follower == leader || excursions[follower] < kFollowerMinExcursionGameUnits) {
                    continue;
                }
                auto& slot = group.followers[group.followerCount];
                slot.boneName = tracks[follower].boneName;
                bool followerMoves = false;
                float minLeaderDistance = 0.0f;
                float maxLeaderDistance = 0.0f;
                for (std::uint32_t key = 0; key < kResampledKeyCount; ++key) {
                    slot.keys[key] = poseAtSamplePosition(tracks[follower], keyPositions[key]);
                    if (key > 0 && !followerMoves &&
                        poseDistance(slot.keys[key], slot.keys[0]) >= kFollowerMinExcursionGameUnits) {
                        followerMoves = true;
                    }
                    const float leaderDistance = weapon_part_motion_path::length(weapon_part_motion_path::sub(
                        group.leaderPath.keys[key].translate,
                        slot.keys[key].translate));
                    if (key == 0) {
                        minLeaderDistance = leaderDistance;
                        maxLeaderDistance = leaderDistance;
                    } else {
                        minLeaderDistance = (std::min)(minLeaderDistance, leaderDistance);
                        maxLeaderDistance = (std::max)(maxLeaderDistance, leaderDistance);
                    }
                }
                // A track can move in the clip but be still during the
                // leader's stroke window (e.g. hammer only moves at fire);
                // such a follower would just pin its node — drop it. A track
                // that moves but drifts relative to the leader is its own
                // stroke, not a rider — drop it too.
                if (followerMoves &&
                    maxLeaderDistance - minLeaderDistance <= kRigidFollowerDistanceToleranceGameUnits) {
                    ++group.followerCount;
                }
            }

            outGroups[groupCount++] = group;
        }
        return groupCount;
    }

    // Fractional key index for a scrub arc position; mirrors the scrub
    // policy's uniform-arc key spacing so follower poses interpolate at the
    // same place the leader pose was produced.
    inline float keyPositionForArc(const MotionPath& path, float arcPosition)
    {
        const float keySpacing = path.totalArcLength / static_cast<float>(kResampledKeyCount - 1);
        if (!(keySpacing > 0.0f)) {
            return 0.0f;
        }
        const float clamped = (std::min)(path.totalArcLength, (std::max)(0.0f, arcPosition));
        return clamped / keySpacing;
    }

    inline PoseSample followerPoseAtKeyPosition(const AuthoredFollower& follower, float keyPosition)
    {
        if (keyPosition <= 0.0f) {
            return follower.keys[0];
        }
        if (keyPosition >= static_cast<float>(kResampledKeyCount - 1)) {
            return follower.keys[kResampledKeyCount - 1];
        }
        const auto index = static_cast<std::uint32_t>(keyPosition);
        return lerpPose(follower.keys[index], follower.keys[index + 1], keyPosition - static_cast<float>(index));
    }
}
