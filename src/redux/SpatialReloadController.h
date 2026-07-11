#pragma once

#include <array>
#include <cstdint>

#include "redux/MotionLibraryFormat.h"

namespace redux::spatial_reload
{
    /*
     * Engine-free, allocation-free movement preview for curated spatial
     * profiles. A physical grip selects an interaction group directly; there
     * is no native reload clip/session, clock, or gameplay completion path.
     * A continuous grip may cycle its group's primary/return-style stages.
     * Releasing resets that group to its initial outgoing/rest stage before
     * another grip, matching learned scrub-session lifetime and avoiding a
     * raw-target jump after the engine restores its baseline.
     */
    class Controller
    {
    public:
        static constexpr std::size_t kMaxDriverName = 64;
        static constexpr std::uint32_t kInvalidIndex = 0xFFFF'FFFFu;

        struct HandInput
        {
            bool gripActive{ false };
            // Required only to begin a fresh preview grip. Once pinned, the
            // physical grip remains active until release even if the trigger
            // is no longer held.
            bool triggerHeld{ false };
            // Resolved by the runtime's concrete-grip binding. P-* connector
            // evidence never produces a group index.
            std::uint32_t groupIndex{ kInvalidIndex };
            std::uint64_t gripSequence{ 0 };
            bool posesValid{ false };
            weapon_part_motion_path::PoseSample partPose{};
            weapon_part_motion_path::PoseSample handPose{};
        };

        struct FrameInput
        {
            std::array<HandInput, 2> hands{};
        };

        struct DriverOutput
        {
            std::array<char, kMaxDriverName> node{};
            weapon_part_motion_path::PoseSample target{};
            float scale{ 1.0f };
        };

        struct FrameOutput
        {
            bool profileAvailable{ false };
            bool profileValid{ false };
            bool active{ false };
            bool newGrip{ false };
            bool stageChanged{ false };
            std::uint32_t stageIndex{ kInvalidIndex };
            std::uint32_t groupIndex{ kInvalidIndex };
            // Stage-local geometric position and its normalized equivalent.
            float pathDistance{ 0.0f };
            float pathLength{ 0.0f };
            float pathFraction{ 0.0f };
            // Data-driven conceptual outward travel (0=seated/rest).
            float outwardFraction{ 0.0f };
            std::uint32_t driverCount{ 0 };
            std::array<DriverOutput, motion_library::kMaxSpatialReloadDrivers> drivers{};
            // Sound-only executable surface. Visibility/gameplay mapped
            // events remain inert profile metadata and never appear here.
            std::uint32_t soundEventCount{ 0 };
            std::array<std::uint32_t, motion_library::kMaxSpatialReloadEvents>
                soundEventIndices{};
        };

        [[nodiscard]] FrameOutput update(
            const motion_library::SpatialReloadProfile* profile,
            const FrameInput& input);

        void reset();

    private:
        [[nodiscard]] static std::uint32_t groupIndexForStage(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t stageIndex);

        struct GripSession
        {
            bool active{ false };
            std::uint32_t handIndex{ 0 };
            std::uint32_t groupIndex{ kInvalidIndex };
            std::uint64_t gripSequence{ 0 };
            weapon_part_motion_path::PoseSample handStart{};
            weapon_part_motion_path::PoseSample partStart{};
            weapon_part_motion_path::MotionPath anchoredControlPath{};
        };

        struct GroupState
        {
            bool initialized{ false };
            bool stageAnnounced{ false };
            std::uint32_t stageIndex{ kInvalidIndex };
            float pathDistance{ 0.0f };
        };

        struct DriverState
        {
            bool used{ false };
            bool initialized{ false };
            std::array<char, kMaxDriverName> node{};
            weapon_part_motion_path::PoseSample target{};
            float scale{ 1.0f };
            // At a stage handoff, align the new track to the exact outgoing
            // target. This correction fades to identity by the next gate, so
            // curated absolute poses are recovered without a handoff teleport.
            bool alignmentActive{ false };
            weapon_part_motion_path::Vec3 alignmentTranslate{};
            weapon_part_motion_path::Quat alignmentRotate{};
            float alignmentScale{ 0.0f };
        };

        [[nodiscard]] bool initializeProfile(
            const motion_library::SpatialReloadProfile& profile);
        void initializeGroupState(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t groupIndex);
        void beginGrip(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t handIndex,
            const HandInput& hand,
            FrameOutput& output);
        void transitionStage(
            const motion_library::SpatialReloadProfile& profile,
            GroupState& groupState,
            const HandInput& hand,
            FrameOutput& output);
        void updateCurrentStageDrivers(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t stageIndex,
            float pathDistance);
        void writeDriverOutput(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t stageIndex,
            FrameOutput& output) const;
        void queueMappedSounds(
            const motion_library::SpatialReloadProfile& profile,
            std::uint32_t groupIndex,
            std::uint32_t stageIndex,
            motion_library::SpatialReloadMappedEventTrigger trigger,
            float priorPathDistance,
            float pathDistance,
            FrameOutput& output);
        [[nodiscard]] float outwardFraction(
            const motion_library::SpatialReloadStage& stage,
            float pathDistance) const;

        // Identity comparison only; the pointer is never dereferenced outside
        // update() and is reset when the equip-scoped profile object changes.
        const motion_library::SpatialReloadProfile* _profileIdentity{ nullptr };
        bool _profileValid{ false };
        GripSession _grip{};
        std::array<GroupState, motion_library::kMaxSpatialReloadGroups> _groups{};
        std::array<std::int8_t, motion_library::kMaxSpatialReloadGroups> _firstStageByGroup{};
        std::uint32_t _driverCount{ 0 };
        std::array<DriverState, motion_library::kMaxSpatialReloadDrivers> _drivers{};
        std::array<std::array<std::int8_t, motion_library::kMaxSpatialReloadDrivers>,
            motion_library::kMaxSpatialReloadStages>
            _stageDriverMap{};
        std::array<std::array<bool, motion_library::kMaxSpatialReloadEvents>,
            motion_library::kMaxSpatialReloadGroups>
            _firedSoundEvents{};
    };
}
