#include "redux/AuthoritativeReloadProfileFormat.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace redux::motion_library::authoritative_profile_format
{
    namespace
    {
        using nlohmann::json;

        [[nodiscard]] std::string formIdToHex(std::uint32_t id)
        {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08X", id);
            return buffer;
        }

        [[nodiscard]] bool hexToFormId(std::string_view text, std::uint32_t& out)
        {
            if (text.size() > 2 && (text[1] == 'x' || text[1] == 'X') && text[0] == '0') {
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

        [[nodiscard]] const char* eventKindName(AuthoritativeEventKind kind)
        {
            switch (kind) {
            case AuthoritativeEventKind::Sound:
                return "sound";
            case AuthoritativeEventKind::Visibility:
                return "visibility";
            case AuthoritativeEventKind::Gameplay:
                return "gameplay";
            }
            return "sound";
        }

        [[nodiscard]] bool eventKindFromJson(const json& value, AuthoritativeEventKind& out)
        {
            if (!value.is_string()) {
                return false;
            }
            const auto& name = value.get_ref<const std::string&>();
            if (name == "sound") {
                out = AuthoritativeEventKind::Sound;
                return true;
            }
            if (name == "visibility") {
                out = AuthoritativeEventKind::Visibility;
                return true;
            }
            if (name == "gameplay") {
                out = AuthoritativeEventKind::Gameplay;
                return true;
            }
            return false;
        }

        [[nodiscard]] bool fail(std::string* outError, std::string message)
        {
            if (outError) {
                *outError = "authoritativeReload: " + std::move(message);
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

        [[nodiscard]] const AuthoritativeInteractionGroup* findGroup(
            const AuthoritativeReloadProfile& profile,
            std::string_view id)
        {
            for (const auto& group : profile.groups) {
                if (group.id == id) {
                    return &group;
                }
            }
            return nullptr;
        }
    }

    nlohmann::json toJson(const AuthoritativeReloadProfile& profile)
    {
        json out;
        out["profileVersion"] = profile.profileVersion;
        out["archetype"] = profile.archetype;
        if (!profile.sourceCapture.empty()) {
            out["sourceCapture"] = profile.sourceCapture;
        }
        if (!profile.notes.empty()) {
            out["notes"] = profile.notes;
        }
        out["clip"] = {
            { "nameContains", profile.clipNameContains },
            { "durationSeconds", profile.expectedDurationSeconds },
            { "durationToleranceSeconds", profile.durationToleranceSeconds },
            { "releaseSeconds", profile.releaseSeconds },
        };

        json groups = json::array();
        for (const auto& group : profile.groups) {
            json encoded;
            encoded["id"] = group.id;
            encoded["role"] = group.role;
            if (!group.notes.empty()) {
                encoded["notes"] = group.notes;
            }
            json grips = json::array();
            for (const auto& grip : group.grips) {
                json encodedGrip;
                encodedGrip["source"] = grip.sourceName;
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
                json encodedDriver;
                encodedDriver["node"] = driver.node;
                if (!driver.role.empty()) {
                    encodedDriver["role"] = driver.role;
                }
                encodedDriver["inheritedFollowers"] = driver.inheritedFollowers;
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
                { "startSeconds", stage.startSeconds },
                { "endSeconds", stage.endSeconds },
            };
            if (!stage.notes.empty()) {
                encoded["notes"] = stage.notes;
            }
            stages.push_back(std::move(encoded));
        }
        out["stages"] = std::move(stages);

        json events = json::array();
        for (const auto& event : profile.events) {
            json encoded{
                { "id", event.id },
                { "timeSeconds", event.timeSeconds },
                { "kind", eventKindName(event.kind) },
                { "animationEvent", event.animationEvent },
            };
            if (!event.notes.empty()) {
                encoded["notes"] = event.notes;
            }
            events.push_back(std::move(encoded));
        }
        out["events"] = std::move(events);
        return out;
    }

    bool fromJson(const nlohmann::json& value, AuthoritativeReloadProfile& out, std::string* outError)
    {
        out = {};
        if (!value.is_object()) {
            return fail(outError, "must be an object");
        }
        if (!value.contains("profileVersion") || !value["profileVersion"].is_number_unsigned()) {
            return fail(outError, "missing profileVersion");
        }
        out.profileVersion = value["profileVersion"].get<std::uint32_t>();
        if (out.profileVersion != kAuthoritativeProfileVersion) {
            return fail(outError, "unsupported profileVersion");
        }
        if (!value.contains("archetype") || !value["archetype"].is_string() ||
            value["archetype"].get_ref<const std::string&>().empty()) {
            return fail(outError, "missing archetype");
        }
        out.archetype = value["archetype"].get<std::string>();
        if (value.contains("sourceCapture")) {
            if (!value["sourceCapture"].is_string()) {
                return fail(outError, "sourceCapture must be a string");
            }
            out.sourceCapture = value["sourceCapture"].get<std::string>();
        }
        if (value.contains("notes")) {
            if (!value["notes"].is_string()) {
                return fail(outError, "notes must be a string");
            }
            out.notes = value["notes"].get<std::string>();
        }

        if (!value.contains("clip") || !value["clip"].is_object()) {
            return fail(outError, "missing clip object");
        }
        const auto& clip = value["clip"];
        if (!clip.contains("nameContains") || !clip["nameContains"].is_string() ||
            clip["nameContains"].get_ref<const std::string&>().empty() ||
            !clip.contains("durationSeconds") || !clip["durationSeconds"].is_number() ||
            !clip.contains("durationToleranceSeconds") || !clip["durationToleranceSeconds"].is_number() ||
            !clip.contains("releaseSeconds") || !clip["releaseSeconds"].is_number()) {
            return fail(outError,
                "clip needs nameContains/durationSeconds/durationToleranceSeconds/releaseSeconds");
        }
        out.clipNameContains = clip["nameContains"].get<std::string>();
        out.expectedDurationSeconds = clip["durationSeconds"].get<float>();
        out.durationToleranceSeconds = clip["durationToleranceSeconds"].get<float>();
        out.releaseSeconds = clip["releaseSeconds"].get<float>();
        if (!std::isfinite(out.expectedDurationSeconds) || out.expectedDurationSeconds <= 0.0f ||
            !std::isfinite(out.durationToleranceSeconds) || out.durationToleranceSeconds <= 0.0f ||
            out.durationToleranceSeconds > 1.0f || !std::isfinite(out.releaseSeconds) ||
            out.releaseSeconds <= 0.0f || out.releaseSeconds > out.expectedDurationSeconds) {
            return fail(outError, "clip timing values are out of range");
        }

        if (!value.contains("groups") || !value["groups"].is_array() || value["groups"].empty() ||
            value["groups"].size() > kMaxAuthoritativeGroups) {
            return fail(outError, "groups count is outside 1..8");
        }
        out.groups.reserve(value["groups"].size());
        for (const auto& encoded : value["groups"]) {
            if (!encoded.is_object() || !encoded.contains("id") || !encoded["id"].is_string() ||
                encoded["id"].get_ref<const std::string&>().empty() || !encoded.contains("role") ||
                !encoded["role"].is_string() || !encoded.contains("grips") ||
                !encoded["grips"].is_array() || encoded["grips"].empty() ||
                encoded["grips"].size() > kMaxAuthoritativeGripsPerGroup ||
                !encoded.contains("drivers") || !encoded["drivers"].is_array() ||
                encoded["drivers"].empty() || encoded["drivers"].size() > kMaxAuthoritativeDriversPerGroup) {
                return fail(outError,
                    "every group needs id/role and bounded non-empty grips/drivers");
            }
            AuthoritativeInteractionGroup group;
            group.id = encoded["id"].get<std::string>();
            group.role = encoded["role"].get<std::string>();
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "group notes must be a string");
                }
                group.notes = encoded["notes"].get<std::string>();
            }
            for (const auto& encodedGrip : encoded["grips"]) {
                if (!encodedGrip.is_object() || !encodedGrip.contains("source") ||
                    !encodedGrip["source"].is_string()) {
                    return fail(outError, "group grip missing source");
                }
                AuthoritativeGripSource grip;
                grip.sourceName = encodedGrip["source"].get<std::string>();
                if (!validProviderName(grip.sourceName)) {
                    return fail(outError, "grip source must fit ROCK's 63-byte identity");
                }
                if (isConnectionPointName(grip.sourceName)) {
                    return fail(outError,
                        "P-* connection points cannot be physical grip sources");
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
                return fail(outError, "group contains duplicate grip sources");
            }
            for (const auto& encodedDriver : encoded["drivers"]) {
                if (!encodedDriver.is_object() || !encodedDriver.contains("node") ||
                    !encodedDriver["node"].is_string() ||
                    !validProviderName(encodedDriver["node"].get_ref<const std::string&>()) ||
                    !encodedDriver.contains("inheritedFollowers") ||
                    !encodedDriver["inheritedFollowers"].is_array() ||
                    encodedDriver["inheritedFollowers"].size() >
                        kMaxAuthoritativeFollowersPerDriver) {
                    return fail(outError,
                        "driver needs a bounded node and inheritedFollowers array");
                }
                AuthoritativeDriver driver;
                driver.node = encodedDriver["node"].get<std::string>();
                if (encodedDriver.contains("role")) {
                    if (!encodedDriver["role"].is_string()) {
                        return fail(outError, "driver role must be a string");
                    }
                    driver.role = encodedDriver["role"].get<std::string>();
                }
                for (const auto& follower : encodedDriver["inheritedFollowers"]) {
                    if (!follower.is_string() ||
                        !validProviderName(follower.get_ref<const std::string&>())) {
                        return fail(outError,
                            "inherited follower must fit ROCK's 63-byte identity");
                    }
                    driver.inheritedFollowers.push_back(follower.get<std::string>());
                }
                if (hasDuplicateName(driver.inheritedFollowers,
                        [](const auto& name) -> const std::string& { return name; })) {
                    return fail(outError, "driver contains duplicate inherited followers");
                }
                group.drivers.push_back(std::move(driver));
            }
            if (hasDuplicateName(group.drivers,
                    [](const auto& driver) -> const std::string& { return driver.node; })) {
                return fail(outError, "group contains duplicate driver nodes");
            }
            out.groups.push_back(std::move(group));
        }
        if (hasDuplicateName(out.groups,
                [](const auto& group) -> const std::string& { return group.id; })) {
            return fail(outError, "group ids must be unique");
        }

        if (!value.contains("stages") || !value["stages"].is_array() ||
            value["stages"].empty() || value["stages"].size() > kMaxAuthoritativeStages) {
            return fail(outError, "stages count is outside 1..16");
        }
        out.stages.reserve(value["stages"].size());
        float priorEnd = 0.0f;
        for (const auto& encoded : value["stages"]) {
            if (!encoded.is_object() || !encoded.contains("id") ||
                !encoded["id"].is_string() || encoded["id"].get_ref<const std::string&>().empty() ||
                !encoded.contains("group") || !encoded["group"].is_string() ||
                !encoded.contains("startSeconds") || !encoded["startSeconds"].is_number() ||
                !encoded.contains("endSeconds") || !encoded["endSeconds"].is_number()) {
                return fail(outError, "stage needs id/group/startSeconds/endSeconds");
            }
            AuthoritativeStage stage;
            stage.id = encoded["id"].get<std::string>();
            stage.groupId = encoded["group"].get<std::string>();
            stage.startSeconds = encoded["startSeconds"].get<float>();
            stage.endSeconds = encoded["endSeconds"].get<float>();
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "stage notes must be a string");
                }
                stage.notes = encoded["notes"].get<std::string>();
            }
            if (!findGroup(out, stage.groupId)) {
                return fail(outError, "stage references an unknown group");
            }
            if (!std::isfinite(stage.startSeconds) || !std::isfinite(stage.endSeconds) ||
                stage.startSeconds < 0.0f || stage.endSeconds <= stage.startSeconds ||
                stage.endSeconds > out.releaseSeconds + 0.001f) {
                return fail(outError, "stage timing is out of range");
            }
            // V1 profiles deliberately cover the complete interactive
            // interval. Skipping gaps would collapse authored sound spacing
            // and visibility events into one frame.
            if (std::fabs(stage.startSeconds - priorEnd) > 0.001f) {
                return fail(outError,
                    "stages must be ordered and contiguous from time zero");
            }
            priorEnd = stage.endSeconds;
            out.stages.push_back(std::move(stage));
        }
        if (hasDuplicateName(out.stages,
                [](const auto& stage) -> const std::string& { return stage.id; })) {
            return fail(outError, "stage ids must be unique");
        }
        if (std::fabs(priorEnd - out.releaseSeconds) > 0.001f) {
            return fail(outError, "releaseSeconds must equal the final stage end");
        }

        if (!value.contains("events") || !value["events"].is_array() ||
            value["events"].size() > kMaxAuthoritativeEvents) {
            return fail(outError, "events must be an array with at most 64 entries");
        }
        out.events.reserve(value["events"].size());
        float priorEventTime = -1.0f;
        for (const auto& encoded : value["events"]) {
            if (!encoded.is_object() || !encoded.contains("id") ||
                !encoded["id"].is_string() || encoded["id"].get_ref<const std::string&>().empty() ||
                !encoded.contains("timeSeconds") || !encoded["timeSeconds"].is_number() ||
                !encoded.contains("kind") || !encoded.contains("animationEvent") ||
                !encoded["animationEvent"].is_string() ||
                encoded["animationEvent"].get_ref<const std::string&>().empty() ||
                encoded["animationEvent"].get_ref<const std::string&>().size() >= 96) {
                return fail(outError,
                    "event needs bounded id/timeSeconds/kind/animationEvent");
            }
            AuthoritativeTimelineEvent event;
            event.id = encoded["id"].get<std::string>();
            event.timeSeconds = encoded["timeSeconds"].get<float>();
            event.animationEvent = encoded["animationEvent"].get<std::string>();
            if (!eventKindFromJson(encoded["kind"], event.kind)) {
                return fail(outError,
                    "event kind must be sound, visibility, or gameplay");
            }
            if (encoded.contains("notes")) {
                if (!encoded["notes"].is_string()) {
                    return fail(outError, "event notes must be a string");
                }
                event.notes = encoded["notes"].get<std::string>();
            }
            if (!std::isfinite(event.timeSeconds) || event.timeSeconds < 0.0f ||
                event.timeSeconds > out.releaseSeconds + 0.001f ||
                event.timeSeconds < priorEventTime) {
                return fail(outError,
                    "events must be time-ordered inside the interactive interval");
            }
            priorEventTime = event.timeSeconds;
            out.events.push_back(std::move(event));
        }
        if (hasDuplicateName(out.events,
                [](const auto& event) -> const std::string& { return event.id; })) {
            return fail(outError, "event ids must be unique");
        }

        out.used = true;
        return true;
    }
}
