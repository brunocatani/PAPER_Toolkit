#include "redux/SpatialReloadController.h"

#include "redux/WeaponPartMotionScrubPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace redux::spatial_reload
{
    namespace
    {
        using weapon_part_motion_path::MotionPath;
        using weapon_part_motion_path::PoseSample;
        using weapon_part_motion_path::Quat;
        using weapon_part_motion_path::Vec3;

        constexpr float kFractionEpsilon = 1.0e-4f;
        constexpr float kSoundCrossingTolerance = 0.001f;

        [[nodiscard]] std::string_view fixedName(
            const std::array<char, Controller::kMaxDriverName>& name)
        {
            std::size_t length = 0;
            while (length < name.size() && name[length] != '\0') {
                ++length;
            }
            return std::string_view(name.data(), length);
        }

        void copyName(
            std::array<char, Controller::kMaxDriverName>& out,
            std::string_view name)
        {
            out = {};
            std::memcpy(
                out.data(), name.data(), (std::min)(name.size(), out.size() - 1));
        }

        [[nodiscard]] PoseSample rigidHandDesiredPose(
            const PoseSample& handStart,
            const PoseSample& handNow,
            const PoseSample& partStart)
        {
            const Quat rotationDelta = weapon_part_motion_path::quatMultiply(
                handNow.rotate,
                weapon_part_motion_path::quatConjugate(handStart.rotate));
            const Vec3 handToPart = weapon_part_motion_path::sub(
                partStart.translate, handStart.translate);
            return PoseSample{
                .translate = weapon_part_motion_path::add(
                    handNow.translate,
                    weapon_part_motion_path::quatRotate(rotationDelta, handToPart)),
                .rotate = weapon_part_motion_path::quatMultiply(
                    rotationDelta, partStart.rotate),
            };
        }

        void anchorControlPath(
            const MotionPath& source,
            float sourcePathDistance,
            const PoseSample& currentPart,
            MotionPath& out)
        {
            out = source;
            const auto sourceAnchor = weapon_part_motion_scrub::poseAtArcPosition(
                source, sourcePathDistance);
            for (std::size_t i = 0; i < out.keys.size(); ++i) {
                const auto rotationDelta = weapon_part_motion_path::quatMultiply(
                    source.keys[i].rotate,
                    weapon_part_motion_path::quatConjugate(sourceAnchor.rotate));
                out.keys[i].translate = weapon_part_motion_path::add(
                    currentPart.translate,
                    weapon_part_motion_path::sub(
                        source.keys[i].translate, sourceAnchor.translate));
                out.keys[i].rotate = weapon_part_motion_path::quatMultiply(
                    rotationDelta, currentPart.rotate);
            }
        }

        [[nodiscard]] PoseSample trackPoseAtDistance(
            const motion_library::SpatialReloadDriverTrack& track,
            float pathDistance)
        {
            if (track.keys.empty()) {
                return {};
            }
            if (pathDistance <= track.keys.front().pathDistance) {
                return track.keys.front().pose;
            }
            for (std::size_t i = 1; i < track.keys.size(); ++i) {
                if (pathDistance <= track.keys[i].pathDistance) {
                    const float span = track.keys[i].pathDistance -
                        track.keys[i - 1].pathDistance;
                    const float t = span > 0.0f
                        ? std::clamp(
                              (pathDistance - track.keys[i - 1].pathDistance) /
                                  span,
                              0.0f,
                              1.0f)
                        : 1.0f;
                    return weapon_part_motion_path::lerpPose(
                        track.keys[i - 1].pose, track.keys[i].pose, t);
                }
            }
            return track.keys.back().pose;
        }

        [[nodiscard]] float stageDistance(
            const motion_library::SpatialReloadStage& stage,
            float fraction)
        {
            return std::clamp(fraction, 0.0f, 1.0f) *
                stage.controlPath.totalArcLength;
        }

        [[nodiscard]] Vec3 scaled(const Vec3& value, float scale)
        {
            return Vec3{ value.x * scale, value.y * scale, value.z * scale };
        }
    }

    void Controller::reset()
    {
        _profileIdentity = nullptr;
        _profileValid = false;
        _grip = {};
        _groups = {};
        _firstStageByGroup.fill(-1);
        _driverCount = 0;
        _drivers = {};
        for (auto& stage : _stageDriverMap) {
            stage.fill(-1);
        }
        _firedSoundEvents = {};
    }

    std::uint32_t Controller::groupIndexForStage(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t stageIndex)
    {
        if (stageIndex >= profile.stages.size()) {
            return kInvalidIndex;
        }
        const auto& groupId = profile.stages[stageIndex].groupId;
        for (std::uint32_t i = 0; i < profile.groups.size(); ++i) {
            if (profile.groups[i].id == groupId) {
                return i;
            }
        }
        return kInvalidIndex;
    }

    bool Controller::initializeProfile(
        const motion_library::SpatialReloadProfile& profile)
    {
        _profileValid = false;
        _grip = {};
        _groups = {};
        _firstStageByGroup.fill(-1);
        _driverCount = 0;
        _drivers = {};
        for (auto& stage : _stageDriverMap) {
            stage.fill(-1);
        }
        _firedSoundEvents = {};

        if (profile.runtimeMode !=
                motion_library::SpatialReloadRuntimeMode::MovementPreview ||
            profile.groups.empty() || profile.stages.empty() ||
            profile.groups.size() > _groups.size() ||
            profile.stages.size() > _stageDriverMap.size()) {
            return false;
        }

        for (const auto& group : profile.groups) {
            for (const auto& driver : group.drivers) {
                if (_driverCount >= _drivers.size()) {
                    return false;
                }
                auto& state = _drivers[_driverCount++];
                state.used = true;
                copyName(state.node, driver.node);
            }
        }

        for (std::uint32_t stageIndex = 0;
             stageIndex < profile.stages.size();
             ++stageIndex) {
            const auto& stage = profile.stages[stageIndex];
            const auto groupIndex = groupIndexForStage(profile, stageIndex);
            if (groupIndex == kInvalidIndex || groupIndex >= _firstStageByGroup.size() ||
                stage.nextStageIndex >= profile.stages.size() ||
                groupIndexForStage(profile, stage.nextStageIndex) != groupIndex) {
                return false;
            }
            if (_firstStageByGroup[groupIndex] < 0) {
                _firstStageByGroup[groupIndex] =
                    static_cast<std::int8_t>(stageIndex);
            }

            for (std::uint32_t trackIndex = 0;
                 trackIndex < stage.driverTracks.size() &&
                 trackIndex < _stageDriverMap[stageIndex].size();
                 ++trackIndex) {
                const auto& track = stage.driverTracks[trackIndex];
                std::int8_t stateIndex = -1;
                for (std::uint32_t driverIndex = 0;
                     driverIndex < _driverCount;
                     ++driverIndex) {
                    if (fixedName(_drivers[driverIndex].node) == track.node) {
                        stateIndex = static_cast<std::int8_t>(driverIndex);
                        break;
                    }
                }
                if (stateIndex < 0) {
                    return false;
                }
                _stageDriverMap[stageIndex][trackIndex] = stateIndex;
            }
        }

        for (std::uint32_t groupIndex = 0;
             groupIndex < profile.groups.size();
             ++groupIndex) {
            if (_firstStageByGroup[groupIndex] < 0) {
                return false;
            }
        }

        _profileValid = true;
        return true;
    }

    void Controller::initializeGroupState(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t groupIndex)
    {
        if (groupIndex >= profile.groups.size() || groupIndex >= _groups.size()) {
            return;
        }
        auto& state = _groups[groupIndex];
        if (state.initialized) {
            return;
        }
        const auto first = _firstStageByGroup[groupIndex];
        if (first < 0 || static_cast<std::size_t>(first) >= profile.stages.size()) {
            return;
        }
        state.initialized = true;
        state.stageAnnounced = false;
        state.stageIndex = static_cast<std::uint32_t>(first);
        const auto& stage = profile.stages[state.stageIndex];
        state.pathDistance = stageDistance(stage, stage.entryFraction);
    }

    void Controller::queueMappedSounds(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t groupIndex,
        std::uint32_t stageIndex,
        motion_library::SpatialReloadMappedEventTrigger trigger,
        float priorPathDistance,
        float pathDistance,
        FrameOutput& output)
    {
        if (groupIndex >= _firedSoundEvents.size()) {
            return;
        }
        auto& fired = _firedSoundEvents[groupIndex];
        for (std::uint32_t eventIndex = 0;
             eventIndex < profile.mappedEvents.size() && eventIndex < fired.size();
             ++eventIndex) {
            const auto& event = profile.mappedEvents[eventIndex];
            if (event.kind != motion_library::SpatialReloadMappedEventKind::Sound ||
                event.stageIndex != stageIndex || event.trigger != trigger ||
                fired[eventIndex]) {
                continue;
            }
            if (trigger == motion_library::SpatialReloadMappedEventTrigger::PathPosition &&
                !(event.pathDistance > priorPathDistance + kSoundCrossingTolerance &&
                    event.pathDistance <= pathDistance + kSoundCrossingTolerance)) {
                continue;
            }
            if (output.soundEventCount < output.soundEventIndices.size()) {
                output.soundEventIndices[output.soundEventCount++] = eventIndex;
                fired[eventIndex] = true;
            }
        }
    }

    void Controller::beginGrip(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t handIndex,
        const HandInput& hand,
        FrameOutput& output)
    {
        initializeGroupState(profile, hand.groupIndex);
        if (hand.groupIndex >= _groups.size() ||
            !_groups[hand.groupIndex].initialized) {
            return;
        }
        auto& groupState = _groups[hand.groupIndex];
        if (groupState.stageIndex >= profile.stages.size()) {
            return;
        }
        const auto& stage = profile.stages[groupState.stageIndex];
        _grip = {};
        _grip.active = true;
        _grip.handIndex = handIndex;
        _grip.groupIndex = hand.groupIndex;
        _grip.gripSequence = hand.gripSequence;
        _grip.handStart = hand.handPose;
        _grip.partStart = hand.partPose;
        anchorControlPath(
            stage.controlPath,
            groupState.pathDistance,
            hand.partPose,
            _grip.anchoredControlPath);
        // A release lets the engine restore baseline. A fresh preview grip
        // therefore starts from the curated first-stage poses, never from a
        // correction retained by the previous continuous cycle.
        for (std::uint32_t trackIndex = 0;
             trackIndex < stage.driverTracks.size() &&
             trackIndex < _stageDriverMap[groupState.stageIndex].size();
             ++trackIndex) {
            const auto stateIndex =
                _stageDriverMap[groupState.stageIndex][trackIndex];
            if (stateIndex >= 0 &&
                static_cast<std::size_t>(stateIndex) < _drivers.size()) {
                _drivers[static_cast<std::size_t>(stateIndex)].alignmentActive = false;
            }
        }
        updateCurrentStageDrivers(
            profile, groupState.stageIndex, groupState.pathDistance);

        if (!groupState.stageAnnounced) {
            queueMappedSounds(
                profile,
                hand.groupIndex,
                groupState.stageIndex,
                motion_library::SpatialReloadMappedEventTrigger::StageEnter,
                groupState.pathDistance,
                groupState.pathDistance,
                output);
            groupState.stageAnnounced = true;
        }
        // Grip-start sounds belong to physical grips and may repeat after a
        // release/re-grip even while the stage position is retained.
        for (std::uint32_t eventIndex = 0;
             eventIndex < profile.mappedEvents.size() &&
             eventIndex < _firedSoundEvents[hand.groupIndex].size();
             ++eventIndex) {
            const auto& event = profile.mappedEvents[eventIndex];
            if (event.stageIndex == groupState.stageIndex &&
                event.trigger ==
                    motion_library::SpatialReloadMappedEventTrigger::GripStart) {
                _firedSoundEvents[hand.groupIndex][eventIndex] = false;
            }
        }
        queueMappedSounds(
            profile,
            hand.groupIndex,
            groupState.stageIndex,
            motion_library::SpatialReloadMappedEventTrigger::GripStart,
            groupState.pathDistance,
            groupState.pathDistance,
            output);
    }

    void Controller::updateCurrentStageDrivers(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t stageIndex,
        float pathDistance)
    {
        for (auto& state : _drivers) {
            state.initialized = false;
        }
        if (stageIndex >= profile.stages.size()) {
            return;
        }
        const auto& stage = profile.stages[stageIndex];
        const float entryDistance = stageDistance(stage, stage.entryFraction);
        const float transitionDistance =
            stageDistance(stage, stage.transitionFraction);
        const float span = transitionDistance - entryDistance;
        const float progress = span > 0.0f
            ? std::clamp((pathDistance - entryDistance) / span, 0.0f, 1.0f)
            : 1.0f;
        const float alignmentWeight = 1.0f - progress;

        for (std::uint32_t trackIndex = 0;
             trackIndex < stage.driverTracks.size() &&
             trackIndex < _stageDriverMap[stageIndex].size();
             ++trackIndex) {
            const auto stateIndex = _stageDriverMap[stageIndex][trackIndex];
            if (stateIndex < 0 ||
                static_cast<std::size_t>(stateIndex) >= _drivers.size()) {
                continue;
            }
            auto& state = _drivers[static_cast<std::size_t>(stateIndex)];
            const auto& track = stage.driverTracks[trackIndex];
            const auto raw = trackPoseAtDistance(track, pathDistance);
            state.initialized = true;
            if (state.alignmentActive && alignmentWeight > kFractionEpsilon) {
                state.target.translate = weapon_part_motion_path::add(
                    raw.translate,
                    scaled(state.alignmentTranslate, alignmentWeight));
                const auto weightedCorrection = weapon_part_motion_path::quatNlerp(
                    Quat{}, state.alignmentRotate, alignmentWeight);
                state.target.rotate = weapon_part_motion_path::quatMultiply(
                    weightedCorrection, raw.rotate);
                state.scale = track.scale + state.alignmentScale * alignmentWeight;
            } else {
                state.target = raw;
                state.scale = track.scale;
                if (alignmentWeight <= kFractionEpsilon) {
                    state.alignmentActive = false;
                }
            }
        }
    }

    void Controller::transitionStage(
        const motion_library::SpatialReloadProfile& profile,
        GroupState& groupState,
        const HandInput& hand,
        FrameOutput& output)
    {
        if (groupState.stageIndex >= profile.stages.size()) {
            return;
        }
        const auto oldStageIndex = groupState.stageIndex;
        const auto& oldStage = profile.stages[oldStageIndex];
        const auto groupIndex = groupIndexForStage(profile, oldStageIndex);
        if (groupIndex == kInvalidIndex || oldStage.nextStageIndex >= profile.stages.size()) {
            return;
        }

        const float oldGateDistance =
            stageDistance(oldStage, oldStage.transitionFraction);
        groupState.pathDistance = oldGateDistance;
        updateCurrentStageDrivers(profile, oldStageIndex, oldGateDistance);
        queueMappedSounds(
            profile,
            groupIndex,
            oldStageIndex,
            motion_library::SpatialReloadMappedEventTrigger::StageComplete,
            oldGateDistance,
            oldGateDistance,
            output);

        std::array<PoseSample, motion_library::kMaxSpatialReloadDrivers>
            priorTargets{};
        std::array<float, motion_library::kMaxSpatialReloadDrivers> priorScales{};
        std::array<bool, motion_library::kMaxSpatialReloadDrivers> priorUsed{};
        for (std::uint32_t driverIndex = 0;
             driverIndex < _driverCount;
             ++driverIndex) {
            priorUsed[driverIndex] = _drivers[driverIndex].initialized;
            priorTargets[driverIndex] = _drivers[driverIndex].target;
            priorScales[driverIndex] = _drivers[driverIndex].scale;
        }

        const auto currentPart = weapon_part_motion_scrub::poseAtArcPosition(
            _grip.anchoredControlPath, oldGateDistance);
        groupState.stageIndex = oldStage.nextStageIndex;
        groupState.stageAnnounced = false;
        const auto& nextStage = profile.stages[groupState.stageIndex];
        groupState.pathDistance =
            stageDistance(nextStage, oldStage.nextStageEntryFraction);

        for (std::uint32_t trackIndex = 0;
             trackIndex < nextStage.driverTracks.size() &&
             trackIndex < _stageDriverMap[groupState.stageIndex].size();
             ++trackIndex) {
            const auto stateIndex =
                _stageDriverMap[groupState.stageIndex][trackIndex];
            if (stateIndex < 0 ||
                static_cast<std::size_t>(stateIndex) >= _drivers.size()) {
                continue;
            }
            auto& state = _drivers[static_cast<std::size_t>(stateIndex)];
            const auto& track = nextStage.driverTracks[trackIndex];
            const auto rawEntry =
                trackPoseAtDistance(track, groupState.pathDistance);
            const auto index = static_cast<std::size_t>(stateIndex);
            if (priorUsed[index]) {
                state.alignmentActive = true;
                state.alignmentTranslate = weapon_part_motion_path::sub(
                    priorTargets[index].translate, rawEntry.translate);
                state.alignmentRotate = weapon_part_motion_path::quatMultiply(
                    priorTargets[index].rotate,
                    weapon_part_motion_path::quatConjugate(rawEntry.rotate));
                state.alignmentScale = priorScales[index] - track.scale;
            } else {
                state.alignmentActive = false;
            }
        }

        _grip.handStart = hand.handPose;
        _grip.partStart = currentPart;
        anchorControlPath(
            nextStage.controlPath,
            groupState.pathDistance,
            currentPart,
            _grip.anchoredControlPath);
        updateCurrentStageDrivers(
            profile, groupState.stageIndex, groupState.pathDistance);

        _firedSoundEvents[groupIndex].fill(false);
        queueMappedSounds(
            profile,
            groupIndex,
            groupState.stageIndex,
            motion_library::SpatialReloadMappedEventTrigger::StageEnter,
            groupState.pathDistance,
            groupState.pathDistance,
            output);
        groupState.stageAnnounced = true;
        output.stageChanged = true;
    }

    void Controller::writeDriverOutput(
        const motion_library::SpatialReloadProfile& profile,
        std::uint32_t stageIndex,
        FrameOutput& output) const
    {
        output.driverCount = 0;
        if (stageIndex >= profile.stages.size()) {
            return;
        }
        const auto& stage = profile.stages[stageIndex];
        for (std::uint32_t trackIndex = 0;
             trackIndex < stage.driverTracks.size() &&
             trackIndex < _stageDriverMap[stageIndex].size() &&
             output.driverCount < output.drivers.size();
             ++trackIndex) {
            const auto stateIndex = _stageDriverMap[stageIndex][trackIndex];
            if (stateIndex < 0 ||
                static_cast<std::size_t>(stateIndex) >= _drivers.size()) {
                continue;
            }
            const auto& state = _drivers[static_cast<std::size_t>(stateIndex)];
            if (!state.used || !state.initialized) {
                continue;
            }
            auto& drive = output.drivers[output.driverCount++];
            drive.node = state.node;
            drive.target = state.target;
            drive.scale = state.scale;
        }
    }

    float Controller::outwardFraction(
        const motion_library::SpatialReloadStage& stage,
        float pathDistance) const
    {
        const float entryDistance = stageDistance(stage, stage.entryFraction);
        const float transitionDistance =
            stageDistance(stage, stage.transitionFraction);
        const float span = transitionDistance - entryDistance;
        const float progress = span > 0.0f
            ? std::clamp((pathDistance - entryDistance) / span, 0.0f, 1.0f)
            : 1.0f;
        return stage.outwardFractionAtEntry +
            (stage.outwardFractionAtTransition -
                stage.outwardFractionAtEntry) *
                progress;
    }

    Controller::FrameOutput Controller::update(
        const motion_library::SpatialReloadProfile* profile,
        const FrameInput& input)
    {
        FrameOutput output{};
        if (!profile || !profile->used) {
            if (_profileIdentity) {
                reset();
            }
            return output;
        }
        output.profileAvailable = true;
        if (_profileIdentity != profile) {
            reset();
            _profileIdentity = profile;
            _profileValid = initializeProfile(*profile);
        }
        output.profileValid = _profileValid;
        if (!_profileValid) {
            return output;
        }

        std::int32_t activeHand = -1;
        if (_grip.active && _grip.handIndex < input.hands.size()) {
            const auto& pinned = input.hands[_grip.handIndex];
            if (pinned.gripActive && pinned.posesValid &&
                pinned.gripSequence == _grip.gripSequence &&
                pinned.groupIndex == _grip.groupIndex) {
                activeHand = static_cast<std::int32_t>(_grip.handIndex);
            }
        }
        if (activeHand < 0) {
            if (_grip.active && _grip.groupIndex < _groups.size()) {
                _groups[_grip.groupIndex] = {};
                _firedSoundEvents[_grip.groupIndex].fill(false);
            }
            _grip = {};
            for (std::uint32_t handIndex = 0;
                 handIndex < input.hands.size();
                 ++handIndex) {
                const auto& hand = input.hands[handIndex];
                if (hand.gripActive && hand.triggerHeld && hand.posesValid &&
                    hand.groupIndex < profile->groups.size()) {
                    activeHand = static_cast<std::int32_t>(handIndex);
                    beginGrip(*profile, handIndex, hand, output);
                    output.newGrip = _grip.active;
                    break;
                }
            }
        }
        if (activeHand < 0 || !_grip.active ||
            _grip.groupIndex >= _groups.size()) {
            return output;
        }

        const auto& hand = input.hands[static_cast<std::size_t>(activeHand)];
        auto& groupState = _groups[_grip.groupIndex];
        if (!groupState.initialized ||
            groupState.stageIndex >= profile->stages.size()) {
            _grip = {};
            return output;
        }

        output.active = true;
        const auto oldStageIndex = groupState.stageIndex;
        const auto& stage = profile->stages[oldStageIndex];
        const float entryDistance = stageDistance(stage, stage.entryFraction);
        const float gateDistance =
            stageDistance(stage, stage.transitionFraction);
        const auto desired = rigidHandDesiredPose(
            _grip.handStart, hand.handPose, _grip.partStart);
        const auto scrubbed = weapon_part_motion_scrub::scrubPose(
            _grip.anchoredControlPath, groupState.pathDistance, desired);
        if (scrubbed.valid) {
            const float priorDistance = groupState.pathDistance;
            groupState.pathDistance = std::clamp(
                scrubbed.arcPosition, entryDistance, gateDistance);
            queueMappedSounds(
                *profile,
                _grip.groupIndex,
                oldStageIndex,
                motion_library::SpatialReloadMappedEventTrigger::PathPosition,
                priorDistance,
                groupState.pathDistance,
                output);

            const bool endpointGate =
                stage.transitionFraction >= 1.0f - kFractionEpsilon;
            const bool gateReached = endpointGate
                ? groupState.pathDistance >=
                      gateDistance - stage.endpointTolerance
                : groupState.pathDistance >= gateDistance - kFractionEpsilon;
            if (gateReached) {
                const float beforeSnap = groupState.pathDistance;
                groupState.pathDistance = gateDistance;
                queueMappedSounds(
                    *profile,
                    _grip.groupIndex,
                    oldStageIndex,
                    motion_library::SpatialReloadMappedEventTrigger::PathPosition,
                    beforeSnap,
                    gateDistance,
                    output);
                transitionStage(*profile, groupState, hand, output);
            } else {
                updateCurrentStageDrivers(
                    *profile, oldStageIndex, groupState.pathDistance);
            }
        }

        output.groupIndex = _grip.groupIndex;
        output.stageIndex = groupState.stageIndex;
        if (groupState.stageIndex < profile->stages.size()) {
            const auto& currentStage = profile->stages[groupState.stageIndex];
            output.pathDistance = groupState.pathDistance;
            output.pathLength = currentStage.controlPath.totalArcLength;
            output.pathFraction = output.pathLength > 0.0f
                ? output.pathDistance / output.pathLength
                : 0.0f;
            output.outwardFraction =
                outwardFraction(currentStage, groupState.pathDistance);
            writeDriverOutput(*profile, groupState.stageIndex, output);
        }
        return output;
    }
}
