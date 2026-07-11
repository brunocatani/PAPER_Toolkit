#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "redux/MotionLibraryFormat.h"

namespace redux::authoritative_reload
{
    /*
     * Pure, engine-free state machine for a format-v2 authoritative reload.
     * Runtime engine access (clip capture, grip reports, graph events) stays
     * in ReduxRuntime; this class only owns deterministic stage progression,
     * clip-window clamping, one-shot event crossings, and release policy.
     */
    class Controller
    {
    public:
        enum class RejectionReason : std::uint8_t
        {
            None = 0,
            ClipNameMismatch,
            DurationMismatch,
            CropMismatch,
        };

        struct ClipState
        {
            bool active{ false };
            std::uint64_t sessionId{ 0 };
            std::string_view clipName{};
            float durationSeconds{ 0.0f };
            float cropStartSeconds{ 0.0f };
            float croppedDurationSeconds{ 0.0f };
            float fraction{ 0.0f };
        };

        struct FrameInput
        {
            ClipState clip{};
            // True only for a live PAPER AttachOnly grip whose bound body is
            // one of the current profile group's explicit grip sources.
            bool currentGroupGripActive{ false };
        };

        struct FrameOutput
        {
            bool profileAvailable{ false };
            bool ownsClipSession{ false };
            bool newSession{ false };
            bool stageChanged{ false };
            bool awaitingRelease{ false };
            bool desiredFractionValid{ false };
            float desiredFraction{ 0.0f };
            bool releaseClip{ false };
            RejectionReason rejection{ RejectionReason::None };
            std::uint32_t stageIndex{ 0 };
            std::uint32_t groupIndex{ 0 };
            std::uint32_t eventCount{ 0 };
            std::array<std::uint32_t, motion_library::kMaxAuthoritativeEvents> eventIndices{};
        };

        [[nodiscard]] FrameOutput update(const motion_library::AuthoritativeReloadProfile* profile, const FrameInput& input);

        void reset();

        // Preview used to interpret this frame's grip reports before update.
        // A new clip session always begins at stage zero.
        [[nodiscard]] std::uint32_t previewStageIndex(std::uint64_t incomingSessionId) const;

    private:
        enum class Phase : std::uint8_t
        {
            Inactive = 0,
            Running,
            AwaitingGripRelease,
            Rejected,
            Completed,
        };

        [[nodiscard]] static std::uint32_t groupIndexForStage(const motion_library::AuthoritativeReloadProfile& profile, std::uint32_t stageIndex);
        static void queueEventsThrough(const motion_library::AuthoritativeReloadProfile& profile, float observedSeconds, FrameOutput& output,
            std::array<bool, motion_library::kMaxAuthoritativeEvents>& firedEvents, float& furthestObservedSeconds);

        std::uint64_t _sessionId{ 0 };
        Phase _phase{ Phase::Inactive };
        RejectionReason _rejection{ RejectionReason::None };
        std::uint32_t _stageIndex{ 0 };
        float _furthestObservedSeconds{ -0.001f };
        std::array<bool, motion_library::kMaxAuthoritativeEvents> _firedEvents{};
    };

    [[nodiscard]] const char* rejectionReasonName(Controller::RejectionReason reason);
}
