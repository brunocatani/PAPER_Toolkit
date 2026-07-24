#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace paper_toolkit::weapon_animation_preharvest_policy
{
    inline constexpr std::size_t kFirstPersonGraphIndex = 1;
    inline constexpr std::uint32_t kAnimationResourceStateMask = 0x70000000u;
    inline constexpr unsigned kAnimationResourceStateShift = 28;
    inline constexpr float kSamplesPerSecond = 120.0f;
    inline constexpr std::uint32_t kMinimumClipSamples = 24;
    inline constexpr std::uint32_t kMaximumClipSamples = 720;

    struct FirstPersonSelection
    {
        bool valid{ false };
        std::size_t graphIndex{ 0 };
    };

    [[nodiscard]] constexpr FirstPersonSelection selectFirstPersonGraph(
        const std::size_t graphCount,
        const std::size_t identifierCount) noexcept
    {
        if (graphCount <= kFirstPersonGraphIndex || identifierCount <= kFirstPersonGraphIndex) {
            return {};
        }
        return FirstPersonSelection{
            .valid = true,
            .graphIndex = kFirstPersonGraphIndex,
        };
    }

    [[nodiscard]] constexpr std::uint32_t animationResourceState(const std::uint32_t flags) noexcept
    {
        return (flags & kAnimationResourceStateMask) >> kAnimationResourceStateShift;
    }

    [[nodiscard]] constexpr bool animationResourceCanExposeData(const std::uint32_t flags) noexcept
    {
        const auto state = animationResourceState(flags);
        return state == 3u || state == 4u;
    }

    [[nodiscard]] inline std::uint32_t clipSampleCount(const float durationSeconds) noexcept
    {
        if (!std::isfinite(durationSeconds) || !(durationSeconds > 0.0f)) {
            return 0;
        }
        const auto requested = static_cast<std::uint32_t>(
            std::ceil(durationSeconds * kSamplesPerSecond)) + 1u;
        return (std::min)(kMaximumClipSamples,
            (std::max)(kMinimumClipSamples, requested));
    }

    [[nodiscard]] constexpr float clipSampleTime(
        const float durationSeconds,
        const std::uint32_t sampleIndex,
        const std::uint32_t sampleCount) noexcept
    {
        if (!(durationSeconds > 0.0f) || sampleCount < 2) {
            return 0.0f;
        }
        const auto boundedIndex = (std::min)(sampleIndex, sampleCount - 1);
        return durationSeconds * static_cast<float>(boundedIndex) /
               static_cast<float>(sampleCount - 1);
    }

    [[nodiscard]] constexpr char asciiLower(const char value) noexcept
    {
        return value >= 'A' && value <= 'Z'
            ? static_cast<char>(value + ('a' - 'A'))
            : value;
    }

    [[nodiscard]] constexpr std::string_view withoutSceneInstanceSuffix(
        const std::string_view name) noexcept
    {
        const auto colon = name.rfind(':');
        if (colon == std::string_view::npos || colon + 1 >= name.size()) {
            return name;
        }
        for (std::size_t index = colon + 1; index < name.size(); ++index) {
            if (name[index] < '0' || name[index] > '9') {
                return name;
            }
        }
        return name.substr(0, colon);
    }

    [[nodiscard]] constexpr bool boneNameMatchesSceneNode(
        const std::string_view boneName,
        const std::string_view sceneNodeName) noexcept
    {
        const auto normalizedSceneName = withoutSceneInstanceSuffix(sceneNodeName);
        if (boneName.size() != normalizedSceneName.size()) {
            return false;
        }
        for (std::size_t index = 0; index < boneName.size(); ++index) {
            if (asciiLower(boneName[index]) != asciiLower(normalizedSceneName[index])) {
                return false;
            }
        }
        return true;
    }

    struct SkeletonPartition
    {
        std::int16_t startBone{ 0 };
        std::int16_t boneCount{ 0 };
    };

    /*
     * Resolve hkaAnimationBinding's three legal mapping forms into one
     * track->bone table: explicit shorts, selected skeleton partitions, or
     * identity when both arrays are empty. Any truncated/out-of-range layout
     * fails as a unit; partial maps must never silently drive the wrong bone.
     */
    [[nodiscard]] constexpr bool buildTrackToBoneMap(
        const std::uint32_t transformTrackCount,
        const std::uint32_t skeletonBoneCount,
        const std::span<const std::int16_t> explicitTrackToBone,
        const std::span<const std::uint16_t> partitionIndices,
        const std::span<const SkeletonPartition> skeletonPartitions,
        const std::span<std::int16_t> outTrackToBone) noexcept
    {
        if (transformTrackCount == 0 || skeletonBoneCount == 0 ||
            outTrackToBone.size() < transformTrackCount) {
            return false;
        }

        if (!explicitTrackToBone.empty()) {
            if (explicitTrackToBone.size() < transformTrackCount) {
                return false;
            }
            for (std::uint32_t track = 0; track < transformTrackCount; ++track) {
                const auto bone = explicitTrackToBone[track];
                if (bone < 0 || static_cast<std::uint32_t>(bone) >= skeletonBoneCount) {
                    return false;
                }
                outTrackToBone[track] = bone;
            }
            return true;
        }

        if (!partitionIndices.empty()) {
            std::uint32_t track = 0;
            for (const auto partitionIndex : partitionIndices) {
                if (partitionIndex >= skeletonPartitions.size()) {
                    return false;
                }
                const auto partition = skeletonPartitions[partitionIndex];
                if (partition.startBone < 0 || partition.boneCount <= 0 ||
                    static_cast<std::uint32_t>(partition.startBone) +
                            static_cast<std::uint32_t>(partition.boneCount) >
                        skeletonBoneCount) {
                    return false;
                }
                for (std::int32_t offset = 0; offset < partition.boneCount; ++offset) {
                    if (track >= transformTrackCount) {
                        return false;
                    }
                    outTrackToBone[track++] = static_cast<std::int16_t>(
                        partition.startBone + offset);
                }
            }
            return track == transformTrackCount;
        }

        // With no explicit/partition map, identity is only valid when the
        // animation covers the complete skeleton. A prefix-sized animation
        // does not prove track N belongs to bone N.
        if (transformTrackCount != skeletonBoneCount) {
            return false;
        }
        for (std::uint32_t track = 0; track < transformTrackCount; ++track) {
            outTrackToBone[track] = static_cast<std::int16_t>(track);
        }
        return true;
    }

    /*
     * Build the local-transform chain strictly below `ancestorBone` in
     * ancestor->target order. The ancestor itself is excluded so composing
     * the result yields the target transform in the ancestor's coordinate
     * frame. Cycles, excessive depth, and unrelated bones fail closed.
     */
    [[nodiscard]] constexpr std::uint32_t buildBoneChainBelowAncestor(
        const std::int16_t targetBone,
        const std::int16_t ancestorBone,
        const std::span<const std::int16_t> parents,
        const std::span<std::int16_t> outChain) noexcept
    {
        if (targetBone < 0 || ancestorBone < 0 || targetBone == ancestorBone ||
            static_cast<std::size_t>(targetBone) >= parents.size() || outChain.empty()) {
            return 0;
        }

        std::uint32_t count = 0;
        auto current = targetBone;
        while (current != ancestorBone) {
            if (current < 0 || static_cast<std::size_t>(current) >= parents.size() ||
                count >= outChain.size()) {
                return 0;
            }
            outChain[count++] = current;
            const auto parent = parents[static_cast<std::size_t>(current)];
            if (parent == current) {
                return 0;
            }
            current = parent;
        }

        for (std::uint32_t left = 0, right = count - 1; left < right; ++left, --right) {
            const auto value = outChain[left];
            outChain[left] = outChain[right];
            outChain[right] = value;
        }
        return count;
    }
}
