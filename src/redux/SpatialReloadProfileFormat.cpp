#include "redux/SpatialReloadProfileFormat.h"

#include "redux/WeaponPartMotionScrubPolicy.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace redux::motion_library::spatial_profile_format
{
    namespace
    {
        using nlohmann::json;
        using weapon_part_motion_path::MotionPath;
        using weapon_part_motion_path::PoseSample;

        [[nodiscard]] std::string formIdToHex(std::uint32_t id)
        {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08X", id);
            return buffer;
        }

        [[nodiscard]] bool hexToFormId(std::string_view text, std::uint32_t& out)
        {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                text.remove_prefix(2);
            }
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        [[nodiscard]] json formRefToJson(const FormRef& ref)
        {
            return json{ { "plugin", ref.plugin }, { "id", formIdToHex(ref.localFormId) } };
        }

        [[nodiscard]] bool formRefFromJson(const json& value, FormRef& out)
        {
            if (!value.is_object() || !value.contains("plugin") || !value["plugin"].is_string() ||
                !value.contains("id") || !value["id"].is_string()) {
                return false;
            }
            out.plugin = value["plugin"].get<std::string>();
            return hexToFormId(value["id"].get_ref<const std::string&>(), out.localFormId);
        }

        [[nodiscard]] json poseToJson(const PoseSample& pose)
        {
            return json::array({ pose.translate.x, pose.translate.y, pose.translate.z,
                pose.rotate.w, pose.rotate.x, pose.rotate.y, pose.rotate.z });
        }

        [[nodiscard]] bool finitePose(const PoseSample& pose)
        {
            const float qLengthSquared = pose.rotate.w * pose.rotate.w +
                pose.rotate.x * pose.rotate.x + pose.rotate.y * pose.rotate.y +
                pose.rotate.z * pose.rotate.z;
            return std::isfinite(pose.translate.x) && std::isfinite(pose.translate.y) &&
                std::isfinite(pose.translate.z) && std::isfinite(qLengthSquared) &&
                qLengthSquared >= 0.90f && qLengthSquared <= 1.10f;
        }

        [[nodiscard]] bool poseFromJson(const json& value, PoseSample& out)
        {
            if (!value.is_array() || value.size() != 7) {
                return false;
            }
            for (const auto& component : value) {
                if (!component.is_number()) {
                    return false;
                }
            }
            out.translate = {
                value[0].get<float>(), value[1].get<float>(), value[2].get<float>()
            };
            const weapon_part_motion_path::Quat encodedRotation{
                value[3].get<float>(), value[4].get<float>(), value[5].get<float>(),
                value[6].get<float>()
            };
            const float qLengthSquared = encodedRotation.w * encodedRotation.w +
                encodedRotation.x * encodedRotation.x +
                encodedRotation.y * encodedRotation.y +
                encodedRotation.z * encodedRotation.z;
            if (!std::isfinite(qLengthSquared) ||
                qLengthSquared < 0.90f || qLengthSquared > 1.10f) {
                return false;
            }
            out.rotate = weapon_part_motion_path::quatNormalizeOrIdentity(encodedRotation);
            return finitePose(out);
        }

        template <std::size_t N>
        [[nodiscard]] json keysToJson(const std::array<PoseSample, N>& keys)
        {
            json out = json::array();
            for (const auto& key : keys) {
                out.push_back(poseToJson(key));
            }
            return out;
        }

        template <std::size_t N>
        [[nodiscard]] bool keysFromJson(const json& value, std::array<PoseSample, N>& out)
        {
            if (!value.is_array() || value.size() != N) {
                return false;
            }
            for (std::size_t i = 0; i < N; ++i) {
                if (!poseFromJson(value[i], out[i])) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] json pathToJson(const MotionPath& path)
        {
            return json{
                { "arcLength", path.totalArcLength },
                { "keys", keysToJson(path.keys) },
            };
        }

        [[nodiscard]] bool pathFromJson(const json& value, MotionPath& out)
        {
            out = {};
            if (!value.is_object() || !value.contains("arcLength") ||
                !value["arcLength"].is_number() || !value.contains("keys") ||
                !keysFromJson(value["keys"], out.keys)) {
                return false;
            }
            out.totalArcLength = value["arcLength"].get<float>();
            if (!std::isfinite(out.totalArcLength) || out.totalArcLength <= 0.0f) {
                return false;
            }
            float measuredArc = 0.0f;
            for (std::size_t i = 1; i < out.keys.size(); ++i) {
                measuredArc += weapon_part_motion_path::poseDistance(out.keys[i - 1], out.keys[i]);
            }
            const float tolerance = (std::max)(0.02f, out.totalArcLength * 0.01f);
            if (std::fabs(measuredArc - out.totalArcLength) > tolerance) {
                return false;
            }
            out.valid = true;
            return true;
        }

        [[nodiscard]] const char* eventKindName(SpatialReloadMappedEventKind kind)
        {
            switch (kind) {
            case SpatialReloadMappedEventKind::Sound:
                return "sound";
            case SpatialReloadMappedEventKind::Visibility:
                return "visibility";
            case SpatialReloadMappedEventKind::Gameplay:
                return "gameplay";
            }
            return "sound";
        }

        [[nodiscard]] bool eventKindFromJson(
            const json& value,
            SpatialReloadMappedEventKind& out)
        {
            if (!value.is_string()) {
                return false;
            }
            const auto& name = value.get_ref<const std::string&>();
            if (name == "sound") {
                out = SpatialReloadMappedEventKind::Sound;
            } else if (name == "visibility") {
                out = SpatialReloadMappedEventKind::Visibility;
            } else if (name == "gameplay") {
                out = SpatialReloadMappedEventKind::Gameplay;
            } else {
                return false;
            }
            return true;
        }

        [[nodiscard]] const char* triggerName(SpatialReloadMappedEventTrigger trigger)
        {
            switch (trigger) {
            case SpatialReloadMappedEventTrigger::StageEnter:
                return "stageEnter";
            case SpatialReloadMappedEventTrigger::GripStart:
                return "gripStart";
            case SpatialReloadMappedEventTrigger::PathPosition:
                return "pathPosition";
            case SpatialReloadMappedEventTrigger::StageComplete:
                return "stageComplete";
            }
            return "pathPosition";
        }

        [[nodiscard]] bool triggerFromJson(
            const json& value,
            SpatialReloadMappedEventTrigger& out)
        {
            if (!value.is_string()) {
                return false;
            }
            const auto& name = value.get_ref<const std::string&>();
            if (name == "stageEnter") {
                out = SpatialReloadMappedEventTrigger::StageEnter;
            } else if (name == "gripStart") {
                out = SpatialReloadMappedEventTrigger::GripStart;
            } else if (name == "pathPosition") {
                out = SpatialReloadMappedEventTrigger::PathPosition;
            } else if (name == "stageComplete") {
                out = SpatialReloadMappedEventTrigger::StageComplete;
            } else {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool fail(std::string* outError, std::string message)
        {
            if (outError) {
                *outError = "spatialReload: " + std::move(message);
            }
            return false;
        }

        [[nodiscard]] bool validProviderName(std::string_view name)
        {
            return !name.empty() && name.size() < 64;
        }

        [[nodiscard]] bool isConnectionPointName(std::string_view name)
        {
            return name.size() >= 2 && (name[0] == 'P' || name[0] == 'p') &&
                name[1] == '-';
        }

        template <class Range, class GetName>
        [[nodiscard]] bool hasDuplicateName(const Range& range, GetName&& getName)
        {
            for (std::size_t i = 0; i < range.size(); ++i) {
                for (std::size_t j = i + 1; j < range.size(); ++j) {
                    if (getName(range[i]) == getName(range[j])) {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] std::int32_t findGroupIndex(
            const SpatialReloadProfile& profile, std::string_view id)
        {
            for (std::size_t i = 0; i < profile.groups.size(); ++i) {
                if (profile.groups[i].id == id) {
                    return static_cast<std::int32_t>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] std::int32_t findStageIndex(
            const SpatialReloadProfile& profile, std::string_view id)
        {
            for (std::size_t i = 0; i < profile.stages.size(); ++i) {
                if (profile.stages[i].id == id) {
                    return static_cast<std::int32_t>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] bool containsForbiddenTemporalField(const json& value)
        {
            static constexpr std::array<std::string_view, 9> forbidden{
                "timeSeconds", "durationSeconds", "durationToleranceSeconds",
                "startSeconds", "endSeconds", "releaseSeconds", "fraction",
                "normalizedTime", "clipTime"
            };
            if (value.is_object()) {
                for (const auto& [name, child] : value.items()) {
                    for (const auto blocked : forbidden) {
                        if (name == blocked) {
                            return true;
                        }
                    }
                    if (containsForbiddenTemporalField(child)) {
                        return true;
                    }
                }
            } else if (value.is_array()) {
                for (const auto& child : value) {
                    if (containsForbiddenTemporalField(child)) {
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool containsRemovedRuntimeControlField(const json& value)
        {
            static constexpr std::array<std::string_view, 7> forbidden{
                "activationClip", "clip", "clipName", "positionHold",
                "positionHoldSessionId", "sessionId", "events"
            };
            if (value.is_object()) {
                for (const auto& [name, child] : value.items()) {
                    for (const auto blocked : forbidden) {
                        if (name == blocked) {
                            return true;
                        }
                    }
                    if (containsRemovedRuntimeControlField(child)) {
                        return true;
                    }
                }
            } else if (value.is_array()) {
                for (const auto& child : value) {
                    if (containsRemovedRuntimeControlField(child)) {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    nlohmann::json toJson(const SpatialReloadProfile& profile)
    {
        json out;
        out["profileVersion"] = profile.profileVersion;
        out["runtimeMode"] = "learnerMovementPreview";
        out["authority"] = "curatedRecordedMovement";
        out["coordinateConventions"] = {
            { "controlPaths", "recorded-learner-delta-pose" },
            { "driverTracks", "weapon-root-local" },
            { "quaternionOrder", "w,x,y,z" },
            { "pathDistance", "recorded-learner-pose-arc" },
            { "handProjection", "weapon-root-local-translation" },
        };
        out["archetype"] = profile.archetype;
        if (!profile.sourceClip.empty()) {
            out["sourceClip"] = profile.sourceClip;
        }
        if (!profile.sourceCapture.empty()) {
            out["sourceCapture"] = profile.sourceCapture;
        }
        if (!profile.sourceActivityId.empty()) {
            out["sourceActivityId"] = profile.sourceActivityId;
        }
        if (!profile.notes.empty()) {
            out["notes"] = profile.notes;
        }

        json groups = json::array();
        for (const auto& group : profile.groups) {
            json encoded{
                { "id", group.id },
                { "role", group.role },
                { "connectorEvidence", group.connectorEvidence },
            };
            if (!group.notes.empty()) {
                encoded["notes"] = group.notes;
            }
            json grips = json::array();
            for (const auto& grip : group.grips) {
                json encodedGrip{ { "source", grip.sourceName } };
                if (!grip.omod.empty()) {
                    encodedGrip["omod"] = formRefToJson(grip.omod);
                }
                if (!grip.omodName.empty()) {
                    encodedGrip["omodName"] = grip.omodName;
                }
                if (!grip.role.empty()) {
                    encodedGrip["role"] = grip.role;
                }
                grips.push_back(std::move(encodedGrip));
            }
            encoded["grips"] = std::move(grips);

            json drivers = json::array();
            for (const auto& driver : group.drivers) {
                json encodedDriver{
                    { "node", driver.node },
                    { "inheritedFollowers", driver.inheritedFollowers },
                };
                if (!driver.role.empty()) {
                    encodedDriver["role"] = driver.role;
                }
                drivers.push_back(std::move(encodedDriver));
            }
            encoded["drivers"] = std::move(drivers);
            groups.push_back(std::move(encoded));
        }
        out["groups"] = std::move(groups);

        json stages = json::array();
        for (const auto& stage : profile.stages) {
            json encoded{
                { "id", stage.id },
                { "group", stage.groupId },
                { "entryFraction", stage.entryFraction },
                { "transitionFraction", stage.transitionFraction },
                { "nextStage", stage.nextStageId },
                { "nextStageEntryFraction", stage.nextStageEntryFraction },
                { "outwardFractionAtEntry", stage.outwardFractionAtEntry },
                { "outwardFractionAtTransition", stage.outwardFractionAtTransition },
                { "endpointTolerance", stage.endpointTolerance },
                { "controlPath", pathToJson(stage.controlPath) },
            };
            json tracks = json::array();
            for (const auto& track : stage.driverTracks) {
                json trackKeys = json::array();
                for (const auto& key : track.keys) {
                    trackKeys.push_back(json{
                        { "pathDistance", key.pathDistance },
                        { "pose", poseToJson(key.pose) },
                    });
                }
                tracks.push_back(json{
                    { "node", track.node },
                    { "scale", track.scale },
                    { "keys", std::move(trackKeys) },
                });
            }
            encoded["driverTracks"] = std::move(tracks);
            if (!stage.notes.empty()) {
                encoded["notes"] = stage.notes;
            }
            stages.push_back(std::move(encoded));
        }
        out["stages"] = std::move(stages);

        json events = json::array();
        for (const auto& event : profile.mappedEvents) {
            json encoded{
                { "id", event.id },
                { "stage", event.stageId },
                { "trigger", triggerName(event.trigger) },
                { "kind", eventKindName(event.kind) },
                { "sourceEvent", event.sourceEvent },
            };
            if (event.trigger == SpatialReloadMappedEventTrigger::PathPosition) {
                encoded["pathFraction"] = event.pathFraction;
                encoded["targetPose"] = poseToJson(event.targetPose);
            }
            if (!event.notes.empty()) {
                encoded["notes"] = event.notes;
            }
            events.push_back(std::move(encoded));
        }
        out["mappedEvents"] = std::move(events);
        return out;
    }

    bool fromJson(
        const nlohmann::json& value,
        SpatialReloadProfile& out,
        std::string* outError)
    {
        out = {};
        if (!value.is_object()) {
            return fail(outError, "must be an object");
        }
        if (containsForbiddenTemporalField(value)) {
            return fail(outError, "temporal fields are forbidden; authority must be physical pose/path distance");
        }
        if (containsRemovedRuntimeControlField(value)) {
            return fail(outError, "clip/session authority and the old graph-event surface are removed from learnerMovementPreview");
        }
        if (!value.contains("profileVersion") || !value["profileVersion"].is_number_unsigned() ||
            value["profileVersion"].get<std::uint32_t>() != kSpatialReloadProfileVersion) {
            return fail(outError, "missing or unsupported profileVersion");
        }
        out.profileVersion = value["profileVersion"].get<std::uint32_t>();
        if (value.value("runtimeMode", std::string{}) != "learnerMovementPreview") {
            return fail(outError, "runtimeMode must be learnerMovementPreview");
        }
        out.runtimeMode = SpatialReloadRuntimeMode::LearnerMovementPreview;
        if (value.value("authority", std::string{}) != "curatedRecordedMovement") {
            return fail(outError, "authority must be curatedRecordedMovement");
        }
        if (!value.contains("coordinateConventions") ||
            !value["coordinateConventions"].is_object()) {
            return fail(outError, "missing coordinateConventions");
        }
        const auto& conventions = value["coordinateConventions"];
        if (conventions.value("controlPaths", std::string{}) != "recorded-learner-delta-pose" ||
            conventions.value("driverTracks", std::string{}) != "weapon-root-local" ||
            conventions.value("quaternionOrder", std::string{}) != "w,x,y,z" ||
            conventions.value("pathDistance", std::string{}) !=
                "recorded-learner-pose-arc" ||
            conventions.value("handProjection", std::string{}) !=
                "weapon-root-local-translation") {
            return fail(outError, "unsupported coordinate convention");
        }
        if (!value.contains("archetype") || !value["archetype"].is_string() ||
            value["archetype"].get_ref<const std::string&>().empty() ||
            !value.contains("sourceClip") || !value["sourceClip"].is_string() ||
            value["sourceClip"].get_ref<const std::string&>().empty() ||
            value["sourceClip"].get_ref<const std::string&>().size() >= 256) {
            return fail(outError, "archetype and bounded provenance-only sourceClip are required");
        }
        out.archetype = value["archetype"].get<std::string>();
        out.sourceClip = value["sourceClip"].get<std::string>();
        if (value.contains("sourceCapture")) {
            if (!value["sourceCapture"].is_string()) {
                return fail(outError, "sourceCapture must be a string");
            }
            out.sourceCapture = value["sourceCapture"].get<std::string>();
        }
        if (value.contains("sourceActivityId")) {
            if (!value["sourceActivityId"].is_string()) {
                return fail(outError, "sourceActivityId must be a string");
            }
            out.sourceActivityId = value["sourceActivityId"].get<std::string>();
        }
        if (value.contains("notes")) {
            if (!value["notes"].is_string()) {
                return fail(outError, "notes must be a string");
            }
            out.notes = value["notes"].get<std::string>();
        }

        if (!value.contains("groups") || !value["groups"].is_array() ||
            value["groups"].empty() || value["groups"].size() > kMaxSpatialReloadGroups) {
            return fail(outError, "groups count is outside 1..8");
        }
        std::size_t totalDrivers = 0;
        out.groups.reserve(value["groups"].size());
        for (const auto& encoded : value["groups"]) {
            if (!encoded.is_object() || !encoded.contains("id") || !encoded["id"].is_string() ||
                encoded["id"].get_ref<const std::string&>().empty() ||
                !encoded.contains("role") || !encoded["role"].is_string() ||
                !encoded.contains("connectorEvidence") ||
                !encoded["connectorEvidence"].is_array() ||
                encoded["connectorEvidence"].size() > kMaxSpatialReloadConnectorsPerGroup ||
                !encoded.contains("grips") || !encoded["grips"].is_array() ||
                encoded["grips"].empty() ||
                encoded["grips"].size() > kMaxSpatialReloadGripsPerGroup ||
                !encoded.contains("drivers") || !encoded["drivers"].is_array() ||
                encoded["drivers"].empty()) {
                return fail(outError, "every group needs id/role and bounded connectors/grips/drivers");
            }
            SpatialReloadInteractionGroup group;
            group.id = encoded["id"].get<std::string>();
            group.role = encoded["role"].get<std::string>();
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "group notes must be a string");
                }
                group.notes = encoded["notes"].get<std::string>();
            }
            for (const auto& connector : encoded["connectorEvidence"]) {
                if (!connector.is_string() ||
                    !validProviderName(connector.get_ref<const std::string&>()) ||
                    !isConnectionPointName(connector.get_ref<const std::string&>())) {
                    return fail(outError, "connectorEvidence entries must be bounded P-* node names");
                }
                group.connectorEvidence.push_back(connector.get<std::string>());
            }
            if (hasDuplicateName(group.connectorEvidence,
                    [](const auto& name) -> const std::string& { return name; })) {
                return fail(outError, "group contains duplicate connector evidence");
            }
            for (const auto& encodedGrip : encoded["grips"]) {
                if (!encodedGrip.is_object() || !encodedGrip.contains("source") ||
                    !encodedGrip["source"].is_string()) {
                    return fail(outError, "group grip missing source");
                }
                SpatialReloadGripSource grip;
                grip.sourceName = encodedGrip["source"].get<std::string>();
                if (!validProviderName(grip.sourceName) || isConnectionPointName(grip.sourceName)) {
                    return fail(outError, "P-* connectors cannot be physical grips");
                }
                if (encodedGrip.contains("omod")) {
                    if (!formRefFromJson(encodedGrip["omod"], grip.omod) ||
                        grip.omod.plugin.empty() || grip.omod.localFormId == 0) {
                        return fail(outError, "grip has invalid omod identity");
                    }
                }
                if (encodedGrip.contains("omodName")) {
                    if (!encodedGrip["omodName"].is_string()) {
                        return fail(outError, "grip omodName must be a string");
                    }
                    grip.omodName = encodedGrip["omodName"].get<std::string>();
                }
                if (encodedGrip.contains("role")) {
                    if (!encodedGrip["role"].is_string()) {
                        return fail(outError, "grip role must be a string");
                    }
                    grip.role = encodedGrip["role"].get<std::string>();
                }
                group.grips.push_back(std::move(grip));
            }
            if (hasDuplicateName(group.grips,
                    [](const auto& grip) -> const std::string& { return grip.sourceName; })) {
                return fail(outError, "group contains duplicate grips");
            }
            for (const auto& encodedDriver : encoded["drivers"]) {
                if (!encodedDriver.is_object() || !encodedDriver.contains("node") ||
                    !encodedDriver["node"].is_string() ||
                    !validProviderName(encodedDriver["node"].get_ref<const std::string&>()) ||
                    isConnectionPointName(encodedDriver["node"].get_ref<const std::string&>()) ||
                    !encodedDriver.contains("inheritedFollowers") ||
                    !encodedDriver["inheritedFollowers"].is_array() ||
                    encodedDriver["inheritedFollowers"].size() >
                        kMaxSpatialReloadFollowersPerDriver) {
                    return fail(outError, "driver needs a non-P-* node and bounded followers");
                }
                SpatialReloadDriver driver;
                driver.node = encodedDriver["node"].get<std::string>();
                if (encodedDriver.contains("role")) {
                    if (!encodedDriver["role"].is_string()) {
                        return fail(outError, "driver role must be a string");
                    }
                    driver.role = encodedDriver["role"].get<std::string>();
                }
                for (const auto& follower : encodedDriver["inheritedFollowers"]) {
                    if (!follower.is_string() ||
                        !validProviderName(follower.get_ref<const std::string&>()) ||
                        isConnectionPointName(follower.get_ref<const std::string&>())) {
                        return fail(outError, "P-* connectors may appear only in connectorEvidence");
                    }
                    driver.inheritedFollowers.push_back(follower.get<std::string>());
                }
                if (hasDuplicateName(driver.inheritedFollowers,
                        [](const auto& name) -> const std::string& { return name; })) {
                    return fail(outError, "driver contains duplicate inherited followers");
                }
                group.drivers.push_back(std::move(driver));
            }
            totalDrivers += group.drivers.size();
            if (totalDrivers > kMaxSpatialReloadDrivers ||
                hasDuplicateName(group.drivers,
                    [](const auto& driver) -> const std::string& { return driver.node; })) {
                return fail(outError, "driver identities are duplicate or exceed profile capacity");
            }
            out.groups.push_back(std::move(group));
        }
        if (hasDuplicateName(out.groups,
                [](const auto& group) -> const std::string& { return group.id; })) {
            return fail(outError, "group ids must be unique");
        }
        for (std::size_t a = 0; a < out.groups.size(); ++a) {
            for (std::size_t b = a + 1; b < out.groups.size(); ++b) {
                for (const auto& left : out.groups[a].drivers) {
                    for (const auto& right : out.groups[b].drivers) {
                        if (left.node == right.node) {
                            return fail(outError, "a driver node belongs to more than one group");
                        }
                    }
                }
            }
        }

        if (!value.contains("stages") || !value["stages"].is_array() ||
            value["stages"].empty() || value["stages"].size() > kMaxSpatialReloadStages) {
            return fail(outError, "stages count is outside 1..16");
        }
        out.stages.reserve(value["stages"].size());
        for (const auto& encoded : value["stages"]) {
            if (!encoded.is_object() || !encoded.contains("id") || !encoded["id"].is_string() ||
                encoded["id"].get_ref<const std::string&>().empty() ||
                !encoded.contains("group") || !encoded["group"].is_string() ||
                !encoded.contains("entryFraction") ||
                !encoded["entryFraction"].is_number() ||
                !encoded.contains("transitionFraction") ||
                !encoded["transitionFraction"].is_number() ||
                !encoded.contains("nextStage") || !encoded["nextStage"].is_string() ||
                encoded["nextStage"].get_ref<const std::string&>().empty() ||
                !encoded.contains("nextStageEntryFraction") ||
                !encoded["nextStageEntryFraction"].is_number() ||
                !encoded.contains("outwardFractionAtEntry") ||
                !encoded["outwardFractionAtEntry"].is_number() ||
                !encoded.contains("outwardFractionAtTransition") ||
                !encoded["outwardFractionAtTransition"].is_number() ||
                !encoded.contains("endpointTolerance") ||
                !encoded["endpointTolerance"].is_number() ||
                !encoded.contains("controlPath") || !encoded.contains("driverTracks") ||
                !encoded["driverTracks"].is_array()) {
                return fail(outError, "stage needs a complete movement-preview transition contract");
            }
            SpatialReloadStage stage;
            stage.id = encoded["id"].get<std::string>();
            stage.groupId = encoded["group"].get<std::string>();
            stage.entryFraction = encoded["entryFraction"].get<float>();
            stage.transitionFraction = encoded["transitionFraction"].get<float>();
            stage.nextStageId = encoded["nextStage"].get<std::string>();
            stage.nextStageEntryFraction =
                encoded["nextStageEntryFraction"].get<float>();
            stage.outwardFractionAtEntry =
                encoded["outwardFractionAtEntry"].get<float>();
            stage.outwardFractionAtTransition =
                encoded["outwardFractionAtTransition"].get<float>();
            if (!std::isfinite(stage.entryFraction) ||
                !std::isfinite(stage.transitionFraction) ||
                !std::isfinite(stage.nextStageEntryFraction) ||
                !std::isfinite(stage.outwardFractionAtEntry) ||
                !std::isfinite(stage.outwardFractionAtTransition) ||
                stage.entryFraction < 0.0f || stage.entryFraction >= 1.0f ||
                stage.transitionFraction <= stage.entryFraction ||
                stage.transitionFraction > 1.0f ||
                stage.nextStageEntryFraction < 0.0f ||
                stage.nextStageEntryFraction >= 1.0f ||
                stage.outwardFractionAtEntry < 0.0f ||
                stage.outwardFractionAtEntry > 1.0f ||
                stage.outwardFractionAtTransition < 0.0f ||
                stage.outwardFractionAtTransition > 1.0f) {
                return fail(outError, "stage fractions are outside their spatial ranges");
            }
            const auto groupIndex = findGroupIndex(out, stage.groupId);
            if (groupIndex < 0) {
                return fail(outError, "stage references an unknown group");
            }
            if (!pathFromJson(encoded["controlPath"], stage.controlPath)) {
                return fail(outError, "stage controlPath is malformed or its arc is inconsistent");
            }
            const PoseSample identity{};
            if (weapon_part_motion_path::poseDistance(stage.controlPath.keys[0], identity) > 0.02f) {
                return fail(outError, "stage controlPath key zero must be the identity delta pose");
            }
            stage.endpointTolerance = encoded["endpointTolerance"].get<float>();
            const float activeSegmentLength =
                (stage.transitionFraction - stage.entryFraction) *
                stage.controlPath.totalArcLength;
            if (!std::isfinite(stage.endpointTolerance) ||
                stage.endpointTolerance <= 0.0f ||
                stage.endpointTolerance >= activeSegmentLength) {
                return fail(outError, "stage endpointTolerance is outside its active segment");
            }
            const auto& group = out.groups[static_cast<std::size_t>(groupIndex)];
            if (encoded["driverTracks"].size() != group.drivers.size()) {
                return fail(outError, "stage driverTracks must exactly cover its group drivers");
            }
            for (const auto& encodedTrack : encoded["driverTracks"]) {
                if (!encodedTrack.is_object() || !encodedTrack.contains("node") ||
                    !encodedTrack["node"].is_string() || !encodedTrack.contains("scale") ||
                    !encodedTrack["scale"].is_number() || !encodedTrack.contains("keys")) {
                    return fail(outError, "driver track is malformed");
                }
                SpatialReloadDriverTrack track;
                track.node = encodedTrack["node"].get<std::string>();
                bool groupDriver = false;
                for (const auto& driver : group.drivers) {
                    groupDriver |= driver.node == track.node;
                }
                track.scale = encodedTrack["scale"].get<float>();
                if (!groupDriver || !std::isfinite(track.scale) ||
                    std::fabs(track.scale) < 0.0001f ||
                    !encodedTrack["keys"].is_array() ||
                    encodedTrack["keys"].size() < 2 ||
                    encodedTrack["keys"].size() > kMaxSpatialReloadDriverKeys) {
                    return fail(outError, "driver track node/scale/keys are invalid");
                }
                float priorDistance = -1.0f;
                for (const auto& encodedKey : encodedTrack["keys"]) {
                    if (!encodedKey.is_object() ||
                        !encodedKey.contains("pathDistance") ||
                        !encodedKey["pathDistance"].is_number() ||
                        !encodedKey.contains("pose")) {
                        return fail(outError, "driver key needs pathDistance and pose");
                    }
                    SpatialReloadDriverKey key;
                    key.pathDistance = encodedKey["pathDistance"].get<float>();
                    if (!std::isfinite(key.pathDistance) ||
                        key.pathDistance <= priorDistance ||
                        key.pathDistance < 0.0f ||
                        key.pathDistance > stage.controlPath.totalArcLength ||
                        !poseFromJson(encodedKey["pose"], key.pose)) {
                        return fail(outError, "driver key path order or pose is invalid");
                    }
                    priorDistance = key.pathDistance;
                    track.keys.push_back(std::move(key));
                }
                const float boundaryTolerance = (std::max)(
                    0.002f, stage.controlPath.totalArcLength * 0.0001f);
                if (std::fabs(track.keys.front().pathDistance) > boundaryTolerance ||
                    std::fabs(track.keys.back().pathDistance -
                        stage.controlPath.totalArcLength) > boundaryTolerance) {
                    return fail(outError, "driver keys must cover both physical path endpoints");
                }
                track.keys.front().pathDistance = 0.0f;
                track.keys.back().pathDistance = stage.controlPath.totalArcLength;
                stage.driverTracks.push_back(std::move(track));
            }
            if (hasDuplicateName(stage.driverTracks,
                    [](const auto& track) -> const std::string& { return track.node; })) {
                return fail(outError, "stage contains duplicate driver tracks");
            }
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "stage notes must be a string");
                }
                stage.notes = encoded["notes"].get<std::string>();
            }
            out.stages.push_back(std::move(stage));
        }
        if (hasDuplicateName(out.stages,
                [](const auto& stage) -> const std::string& { return stage.id; })) {
            return fail(outError, "stage ids must be unique");
        }
        // Resolve and validate the data-driven primary/return cycle edges.
        // Driver poses at an edge may differ: the controller aligns the new
        // entry to the outgoing target and fades that correction to zero by
        // the next gate, avoiding a teleport without discarding full poses.
        for (auto& stage : out.stages) {
            const auto nextIndex = findStageIndex(out, stage.nextStageId);
            if (nextIndex < 0) {
                return fail(outError, "stage references an unknown nextStage");
            }
            stage.nextStageIndex = static_cast<std::uint32_t>(nextIndex);
            const auto& next = out.stages[stage.nextStageIndex];
            if (next.groupId != stage.groupId) {
                return fail(outError, "nextStage must remain inside its interaction group");
            }
            if (std::fabs(next.entryFraction - stage.nextStageEntryFraction) > 0.0001f) {
                return fail(outError, "nextStageEntryFraction disagrees with the target stage entry");
            }
            if (std::fabs(next.outwardFractionAtEntry -
                    stage.outwardFractionAtTransition) > 0.001f) {
                return fail(outError, "nextStage changes conceptual outward position at the handoff");
            }
        }
        for (const auto& group : out.groups) {
            std::uint32_t groupStageCount = 0;
            std::uint32_t firstStage = 0;
            for (std::uint32_t i = 0; i < out.stages.size(); ++i) {
                if (out.stages[i].groupId == group.id) {
                    if (groupStageCount == 0) {
                        firstStage = i;
                    }
                    ++groupStageCount;
                }
            }
            if (groupStageCount == 0) {
                return fail(outError, "every interaction group needs at least one stage");
            }
            std::array<bool, kMaxSpatialReloadStages> visited{};
            auto current = firstStage;
            for (std::uint32_t step = 0; step < groupStageCount; ++step) {
                if (current >= out.stages.size() || visited[current] ||
                    out.stages[current].groupId != group.id) {
                    return fail(outError, "group stages do not form one closed cycle");
                }
                visited[current] = true;
                current = out.stages[current].nextStageIndex;
            }
            if (current != firstStage) {
                return fail(outError, "group stage cycle does not return to its initial outgoing stage");
            }
            for (std::uint32_t i = 0; i < out.stages.size(); ++i) {
                if (out.stages[i].groupId == group.id && !visited[i]) {
                    return fail(outError, "group has an unreachable stage outside its cycle");
                }
            }
        }

        if (!value.contains("mappedEvents") || !value["mappedEvents"].is_array() ||
            value["mappedEvents"].size() > kMaxSpatialReloadEvents) {
            return fail(outError, "mappedEvents must be an array with at most 64 entries");
        }
        out.mappedEvents.reserve(value["mappedEvents"].size());
        for (const auto& encoded : value["mappedEvents"]) {
            if (!encoded.is_object() || !encoded.contains("id") ||
                !encoded["id"].is_string() ||
                encoded["id"].get_ref<const std::string&>().empty() ||
                !encoded.contains("stage") || !encoded["stage"].is_string() ||
                !encoded.contains("trigger") || !encoded.contains("kind") ||
                !encoded.contains("sourceEvent") ||
                !encoded["sourceEvent"].is_string() ||
                encoded["sourceEvent"].get_ref<const std::string&>().empty() ||
                encoded["sourceEvent"].get_ref<const std::string&>().size() >= 96) {
                return fail(outError, "mapped event needs id/stage/trigger/kind/bounded sourceEvent");
            }
            SpatialReloadMappedEvent event;
            event.id = encoded["id"].get<std::string>();
            event.stageId = encoded["stage"].get<std::string>();
            const auto stageIndex = findStageIndex(out, event.stageId);
            if (stageIndex < 0) {
                return fail(outError, "event references an unknown stage");
            }
            event.stageIndex = static_cast<std::uint32_t>(stageIndex);
            if (!triggerFromJson(encoded["trigger"], event.trigger) ||
                !eventKindFromJson(encoded["kind"], event.kind)) {
                return fail(outError, "event trigger or kind is unsupported");
            }
            event.sourceEvent = encoded["sourceEvent"].get<std::string>();
            if (event.trigger == SpatialReloadMappedEventTrigger::PathPosition) {
                if (!encoded.contains("pathFraction") ||
                    !encoded["pathFraction"].is_number() ||
                    !encoded.contains("targetPose") ||
                    !poseFromJson(encoded["targetPose"], event.targetPose)) {
                    return fail(outError, "pathPosition metadata requires pathFraction and targetPose");
                }
                event.pathFraction = encoded["pathFraction"].get<float>();
                event.targetPoseUsed = true;
                const auto& stage = out.stages[event.stageIndex];
                if (!std::isfinite(event.pathFraction) || event.pathFraction < 0.0f ||
                    event.pathFraction > 1.0f) {
                    return fail(outError, "mapped event pathFraction is outside its stage");
                }
                event.pathDistance = event.pathFraction * stage.controlPath.totalArcLength;
                const auto sampled = weapon_part_motion_scrub::poseAtArcPosition(
                    stage.controlPath, event.pathDistance);
                if (weapon_part_motion_path::poseDistance(sampled, event.targetPose) > 0.15f) {
                    return fail(outError, "mapped event targetPose does not match its pathFraction");
                }
            } else if (encoded.contains("pathFraction") || encoded.contains("targetPose")) {
                return fail(outError, "only pathPosition metadata may carry pathFraction/targetPose");
            }
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "event notes must be a string");
                }
                event.notes = encoded["notes"].get<std::string>();
            }
            out.mappedEvents.push_back(std::move(event));
        }
        if (hasDuplicateName(out.mappedEvents,
                [](const auto& event) -> const std::string& { return event.id; })) {
            return fail(outError, "mapped event ids must be unique");
        }

        out.used = true;
        return true;
    }
}
