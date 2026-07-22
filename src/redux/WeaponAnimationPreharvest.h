#pragma once

#include "redux/WeaponClipStrokePolicy.h"

#include <cstdint>

namespace redux::weapon_animation_preharvest
{
    enum class State : std::uint8_t
    {
        Idle,
        LoadingBaseGraphs,
        LoadingWeaponSubgraph,
        LoadingClip,
        SamplingClip,
        Completed,
        Failed,
    };

    struct StepResult
    {
        State state{ State::Idle };
        std::uint32_t groupsProduced{ 0 };
    };

    /*
     * Advance one equipped-weapon preharvest step on the ROCK frame thread.
     * The implementation owns one off-screen Bethesda graph/resource job and
     * never plays or time-controls a live clip. `allowedNodeNames` are borrowed
     * for this call only. Any produced groups are exact weapon-root-local
     * authored paths and must be adopted before the next call.
     */
    [[nodiscard]] StepResult step(
        std::uint32_t weaponFormId,
        std::uint64_t weaponGenerationKey,
        const char* const* allowedNodeNames,
        std::uint32_t allowedNodeNameCount,
        weapon_clip_stroke::AuthoredStrokeGroup* outGroups,
        std::uint32_t maxGroups) noexcept;

    // Release the retained clip handle and off-screen graph in Bethesda's
    // required order. Main/frame thread only.
    void reset() noexcept;

    struct Stats
    {
        std::uint32_t animationFileCount{ 0 };
        std::uint32_t clipsSampled{ 0 };
        std::uint32_t clipsWithPartMotion{ 0 };
        std::uint32_t groupsProduced{ 0 };
        std::uint32_t clipsRejected{ 0 };
        std::uint32_t targetBonesTruncated{ 0 };
    };

    [[nodiscard]] Stats snapshotStats() noexcept;
}
