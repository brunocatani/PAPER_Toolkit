#include "redux/MotionLibraryFormat.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdio>
#include <cstring>

namespace redux::motion_library
{
    namespace
    {
        using nlohmann::json;
        using weapon_part_motion_path::PoseSample;

        // Ids serialize as hex strings ("0x0004822D"): human-readable and
        // immune to any JSON integer-precision surprises in external tools.
        std::string formIdToHex(std::uint32_t id)
        {
            char buffer[16]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08X", id);
            return buffer;
        }

        bool hexToFormId(std::string_view text, std::uint32_t& out)
        {
            if (text.size() > 2 && (text[1] == 'x' || text[1] == 'X') && text[0] == '0') {
                text.remove_prefix(2);
            }
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        json formRefToJson(const FormRef& ref)
        {
            return json{ { "plugin", ref.plugin }, { "id", formIdToHex(ref.localFormId) } };
        }

        bool formRefFromJson(const json& j, FormRef& out)
        {
            if (!j.is_object() || !j.contains("plugin") || !j["plugin"].is_string() ||
                !j.contains("id") || !j["id"].is_string()) {
                return false;
            }
            out.plugin = j["plugin"].get<std::string>();
            const auto idText = j["id"].get<std::string>();
            return hexToFormId(idText, out.localFormId);
        }

        // One pose = [tx, ty, tz, rw, rx, ry, rz].
        json poseToJson(const PoseSample& pose)
        {
            return json::array({ pose.translate.x, pose.translate.y, pose.translate.z,
                pose.rotate.w, pose.rotate.x, pose.rotate.y, pose.rotate.z });
        }

        bool poseFromJson(const json& j, PoseSample& out)
        {
            if (!j.is_array() || j.size() != 7) {
                return false;
            }
            for (const auto& v : j) {
                if (!v.is_number()) {
                    return false;
                }
            }
            out.translate = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
            out.rotate = { j[3].get<float>(), j[4].get<float>(), j[5].get<float>(), j[6].get<float>() };
            return true;
        }

        template <std::size_t N>
        bool keysFromJson(const json& j, std::array<PoseSample, N>& out)
        {
            if (!j.is_array() || j.size() != N) {
                return false;
            }
            for (std::size_t i = 0; i < N; ++i) {
                if (!poseFromJson(j[i], out[i])) {
                    return false;
                }
            }
            return true;
        }

        template <std::size_t N>
        json keysToJson(const std::array<PoseSample, N>& keys)
        {
            json out = json::array();
            for (const auto& key : keys) {
                out.push_back(poseToJson(key));
            }
            return out;
        }

        json stageToJson(const StageData& stage)
        {
            json j;
            j["arcLength"] = stage.path.totalArcLength;
            j["keys"] = keysToJson(stage.path.keys);
            if (!stage.stageName.empty()) {
                j["stageName"] = stage.stageName;
            }
            if (!stage.notes.empty()) {
                j["notes"] = stage.notes;
            }
            if (stage.followerCount > 0) {
                json followers = json::array();
                for (std::uint32_t i = 0; i < stage.followerCount && i < stage.followers.size(); ++i) {
                    const auto& follower = stage.followers[i];
                    json f;
                    f["bone"] = std::string(follower.boneName.data(),
                        strnlen(follower.boneName.data(), follower.boneName.size()));
                    f["restScale"] = follower.restScale;
                    f["keys"] = keysToJson(follower.keys);
                    followers.push_back(std::move(f));
                }
                j["followers"] = std::move(followers);
            }
            return j;
        }

        bool stageFromJson(const json& j, StageData& out, std::string* outError)
        {
            out = StageData{};
            if (!j.is_object() || !j.contains("keys") || !j.contains("arcLength") ||
                !j["arcLength"].is_number()) {
                if (outError && outError->empty()) {
                    *outError = "stage missing keys/arcLength";
                }
                return false;
            }
            if (!keysFromJson(j["keys"], out.path.keys)) {
                if (outError && outError->empty()) {
                    *outError = "stage keys malformed (need exactly 24 poses of 7 numbers)";
                }
                return false;
            }
            out.path.totalArcLength = j["arcLength"].get<float>();
            if (!(out.path.totalArcLength > 0.0f)) {
                if (outError && outError->empty()) {
                    *outError = "stage arcLength must be > 0";
                }
                return false;
            }
            out.path.valid = true;
            out.stageName = j.value("stageName", std::string{});
            out.notes = j.value("notes", std::string{});
            if (j.contains("followers") && j["followers"].is_array()) {
                for (const auto& f : j["followers"]) {
                    if (out.followerCount >= out.followers.size()) {
                        break;
                    }
                    if (!f.is_object() || !f.contains("bone") || !f["bone"].is_string() ||
                        !f.contains("keys")) {
                        continue;  // skip a malformed follower, keep the stage
                    }
                    auto& slot = out.followers[out.followerCount];
                    slot = {};
                    const auto bone = f["bone"].get<std::string>();
                    std::memcpy(slot.boneName.data(), bone.data(),
                        (std::min)(bone.size(), slot.boneName.size() - 1));
                    slot.restScale = f.value("restScale", 1.0f);
                    if (!keysFromJson(f["keys"], slot.keys)) {
                        continue;
                    }
                    ++out.followerCount;
                }
            }
            out.used = true;
            return true;
        }
    }

    std::string serialize(const WeaponLibrary& library)
    {
        json j;
        j["format"] = library.formatVersion;
        j["weapon"] = formRefToJson(library.weapon);
        j["weaponName"] = library.weaponName;
        j["curated"] = library.curated;
        json parts = json::array();
        for (const auto& part : library.parts) {
            json p;
            p["source"] = part.sourceName;
            if (!part.omod.empty()) {
                p["omod"] = formRefToJson(part.omod);
            }
            if (part.authored.used) {
                p["authoredFallback"] = part.authoredFallback;
                p["authored"] = stageToJson(part.authored);
            }
            if (part.learnedPrimary.used) {
                p["learnedPrimary"] = stageToJson(part.learnedPrimary);
            }
            if (part.learnedReturn.used) {
                p["learnedReturn"] = stageToJson(part.learnedReturn);
            }
            parts.push_back(std::move(p));
        }
        j["parts"] = std::move(parts);
        // 2-space indent: these files are the hand-tuning surface.
        return j.dump(2);
    }

    bool parse(std::string_view jsonText, WeaponLibrary& out, std::string* outError)
    {
        out = WeaponLibrary{};
        if (outError) {
            outError->clear();
        }
        const json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) {
            if (outError) {
                *outError = "not valid JSON";
            }
            return false;
        }
        out.formatVersion = j.value("format", 0u);
        if (out.formatVersion == 0 || out.formatVersion > kFormatVersion) {
            if (outError) {
                *outError = "unsupported format version";
            }
            return false;
        }
        if (!j.contains("weapon") || !formRefFromJson(j["weapon"], out.weapon) || out.weapon.empty()) {
            if (outError) {
                *outError = "missing/invalid weapon identity";
            }
            return false;
        }
        out.weaponName = j.value("weaponName", std::string{});
        out.curated = j.value("curated", false);
        if (!j.contains("parts") || !j["parts"].is_array()) {
            if (outError) {
                *outError = "missing parts array";
            }
            return false;
        }
        out.parts.reserve(j["parts"].size());
        for (const auto& p : j["parts"]) {
            if (!p.is_object() || !p.contains("source") || !p["source"].is_string()) {
                if (outError && outError->empty()) {
                    *outError = "part missing source name (skipped)";
                }
                continue;
            }
            PartRecord record;
            record.sourceName = p["source"].get<std::string>();
            if (record.sourceName.empty()) {
                continue;
            }
            if (p.contains("omod") && !formRefFromJson(p["omod"], record.omod)) {
                if (outError && outError->empty()) {
                    *outError = "part '" + record.sourceName + "' has invalid omod ref (skipped)";
                }
                continue;
            }
            record.authoredFallback = p.value("authoredFallback", false);
            if (p.contains("learnedPrimary")) {
                (void)stageFromJson(p["learnedPrimary"], record.learnedPrimary, outError);
            }
            if (p.contains("learnedReturn")) {
                (void)stageFromJson(p["learnedReturn"], record.learnedReturn, outError);
            }
            if (p.contains("authored")) {
                (void)stageFromJson(p["authored"], record.authored, outError);
            }
            // A return stage without a primary can never serve; drop it so
            // the learner is never seeded with an unreachable stage.
            if (!record.learnedPrimary.used) {
                record.learnedReturn = StageData{};
            }
            if (record.learnedPrimary.used || record.authored.used) {
                out.parts.push_back(std::move(record));
            }
        }
        return true;
    }
}
