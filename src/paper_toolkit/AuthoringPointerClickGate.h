#pragma once

#include <cstdint>

namespace paper_toolkit::authoring_pointer_gate
{
    struct State
    {
        bool leaseAccepted{ false };
        bool neutralObserved{ false };
        std::uint64_t firstAcceptedFrame{ 0 };
    };

    struct Result
    {
        bool forwardPrimaryDown{ false };
        bool clearLease{ false };
    };

    [[nodiscard]] inline bool reset(State& state) noexcept
    {
        const bool hadLease = state.leaseAccepted;
        state = {};
        return hadLease;
    }

    // ROCK consumes suppression requests after native input for the current
    // provider frame. The accepted lease must survive a later frame and see a
    // physical neutral sample before the workstation forwards a press.
    [[nodiscard]] inline Result advance(
        State& state,
        std::uint64_t frameIndex,
        bool routed,
        bool rawAvailable,
        bool primaryDown,
        bool leaseRequestAccepted) noexcept
    {
        if (!routed || !rawAvailable || !leaseRequestAccepted) {
            return { false, reset(state) };
        }
        if (!state.leaseAccepted || frameIndex < state.firstAcceptedFrame) {
            state.leaseAccepted = true;
            state.neutralObserved = false;
            state.firstAcceptedFrame = frameIndex;
        }
        const bool leaseMature = frameIndex > state.firstAcceptedFrame;
        if (leaseMature && !primaryDown) {
            state.neutralObserved = true;
        }
        return {
            leaseMature && state.neutralObserved && primaryDown,
            false,
        };
    }
}
