#include "redux/AuthoritativeReloadController.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace redux::authoritative_reload
{
    namespace
    {
        [[nodiscard]] bool containsNoCase(std::string_view value, std::string_view needle)
        {
            if (needle.empty() || needle.size() > value.size()) {
                return false;
            }
            for (std::size_t start = 0; start + needle.size() <= value.size(); ++start) {
                bool matches = true;
                for (std::size_t i = 0; i < needle.size(); ++i) {
                    const auto left = static_cast<unsigned char>(value[start + i]);
                    const auto right = static_cast<unsigned char>(needle[i]);
                    if (std::tolower(left) != std::tolower(right)) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] float fractionForSeconds(float seconds, const Controller::ClipState& clip)
        {
            if (!std::isfinite(seconds) || !std::isfinite(clip.cropStartSeconds) || !std::isfinite(clip.croppedDurationSeconds) || clip.croppedDurationSeconds <= 0.0f) {
                return 0.0f;
            }
            return std::clamp((seconds - clip.cropStartSeconds) / clip.croppedDurationSeconds, 0.0f, 1.0f);
        }

        [[nodiscard]] float observedSeconds(const Controller::ClipState& clip)
        {
            return clip.cropStartSeconds + std::clamp(clip.fraction, 0.0f, 1.0f) * clip.croppedDurationSeconds;
        }
    }

    void Controller::reset()
    {
        _sessionId = 0;
        _phase = Phase::Inactive;
        _rejection = RejectionReason::None;
        _stageIndex = 0;
        _furthestObservedSeconds = -0.001f;
        _firedEvents = {};
    }

    std::uint32_t Controller::previewStageIndex(std::uint64_t incomingSessionId) const { return incomingSessionId != 0 && incomingSessionId == _sessionId ? _stageIndex : 0; }

    std::uint32_t Controller::groupIndexForStage(const motion_library::AuthoritativeReloadProfile& profile, std::uint32_t stageIndex)
    {
        if (stageIndex >= profile.stages.size()) {
            return 0;
        }
        const auto& groupId = profile.stages[stageIndex].groupId;
        for (std::uint32_t i = 0; i < profile.groups.size(); ++i) {
            if (profile.groups[i].id == groupId) {
                return i;
            }
        }
        return 0; // parser guarantees every stage reference resolves
    }

    void Controller::queueEventsThrough(const motion_library::AuthoritativeReloadProfile& profile, float observedTime, FrameOutput& output,
        std::array<bool, motion_library::kMaxAuthoritativeEvents>& firedEvents, float& furthestObservedTime)
    {
        if (!std::isfinite(observedTime) || observedTime <= furthestObservedTime) {
            return;
        }
        constexpr float kCrossingToleranceSeconds = 0.001f;
        for (std::uint32_t i = 0; i < profile.events.size() && i < firedEvents.size(); ++i) {
            if (firedEvents[i]) {
                continue;
            }
            const auto eventTime = profile.events[i].timeSeconds;
            if (eventTime <= observedTime + kCrossingToleranceSeconds) {
                if (output.eventCount < output.eventIndices.size()) {
                    output.eventIndices[output.eventCount++] = i;
                    firedEvents[i] = true;
                }
            }
        }
        furthestObservedTime = observedTime;
    }

    Controller::FrameOutput Controller::update(const motion_library::AuthoritativeReloadProfile* profile, const FrameInput& input)
    {
        FrameOutput output{};
        if (!profile || !profile->used || profile->stages.empty() || profile->groups.empty()) {
            reset();
            return output;
        }
        output.profileAvailable = true;

        if (!input.clip.active) {
            if (_sessionId != 0) {
                reset();
            }
            output.stageIndex = 0;
            output.groupIndex = groupIndexForStage(*profile, 0);
            return output;
        }

        if (input.clip.sessionId == 0) {
            output.releaseClip = true;
            output.rejection = RejectionReason::DurationMismatch;
            return output;
        }

        if (_sessionId != input.clip.sessionId) {
            _sessionId = input.clip.sessionId;
            _phase = Phase::Running;
            _rejection = RejectionReason::None;
            _stageIndex = 0;
            _furthestObservedSeconds = -0.001f;
            _firedEvents = {};
            output.newSession = true;

            if (!containsNoCase(input.clip.clipName, profile->clipNameContains)) {
                _phase = Phase::Rejected;
                _rejection = RejectionReason::ClipNameMismatch;
            } else if (!std::isfinite(input.clip.durationSeconds) || std::fabs(input.clip.durationSeconds - profile->expectedDurationSeconds) > profile->durationToleranceSeconds) {
                _phase = Phase::Rejected;
                _rejection = RejectionReason::DurationMismatch;
            } else if (!std::isfinite(input.clip.cropStartSeconds) || !std::isfinite(input.clip.croppedDurationSeconds) ||
                std::fabs(input.clip.cropStartSeconds) > profile->durationToleranceSeconds ||
                std::fabs(input.clip.croppedDurationSeconds - profile->expectedDurationSeconds) > profile->durationToleranceSeconds) {
                _phase = Phase::Rejected;
                _rejection = RejectionReason::CropMismatch;
            }
        }

        output.stageIndex = _stageIndex;
        output.groupIndex = groupIndexForStage(*profile, _stageIndex);
        if (_phase == Phase::Rejected) {
            output.rejection = _rejection;
            output.releaseClip = true;
            return output;
        }
        if (_phase == Phase::Completed) {
            output.releaseClip = true;
            return output;
        }

        output.ownsClipSession = true;
        const float observedTime = observedSeconds(input.clip);
        queueEventsThrough(*profile, observedTime, output, _firedEvents, _furthestObservedSeconds);

        constexpr float kStageEndToleranceSeconds = 0.002f;
        const auto& currentStage = profile->stages[_stageIndex];
        if (_phase == Phase::Running) {
            if (observedTime < currentStage.startSeconds - kStageEndToleranceSeconds) {
                output.desiredFractionValid = true;
                output.desiredFraction = fractionForSeconds(currentStage.startSeconds, input.clip);
            } else if (observedTime >= currentStage.endSeconds - kStageEndToleranceSeconds) {
                _phase = Phase::AwaitingGripRelease;
            }
        }

        if (_phase == Phase::AwaitingGripRelease) {
            output.awaitingRelease = true;
            output.desiredFractionValid = true;
            output.desiredFraction = fractionForSeconds(currentStage.endSeconds, input.clip);
            if (!input.currentGroupGripActive) {
                if (_stageIndex + 1 < profile->stages.size()) {
                    ++_stageIndex;
                    _phase = Phase::Running;
                    output.stageChanged = true;
                    output.awaitingRelease = false;
                    output.stageIndex = _stageIndex;
                    output.groupIndex = groupIndexForStage(*profile, _stageIndex);
                    output.desiredFraction = fractionForSeconds(profile->stages[_stageIndex].startSeconds, input.clip);
                } else {
                    _phase = Phase::Completed;
                    output.releaseClip = true;
                    output.ownsClipSession = false;
                }
            }
        }

        return output;
    }

    const char* rejectionReasonName(Controller::RejectionReason reason)
    {
        switch (reason) {
        case Controller::RejectionReason::None:
            return "none";
        case Controller::RejectionReason::ClipNameMismatch:
            return "clip-name-mismatch";
        case Controller::RejectionReason::DurationMismatch:
            return "duration-mismatch";
        case Controller::RejectionReason::CropMismatch:
            return "crop-mismatch";
        }
        return "unknown";
    }
}
