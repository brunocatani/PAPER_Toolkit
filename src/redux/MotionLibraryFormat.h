#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartMotionPathPolicy.h"

/*
 * Pure on-disk format for the per-weapon motion library (phase 2 of the
 * scalability plan, Bruno 2026-07-05): one human-editable JSON per weapon
 * holding every part's learned/authored motion records so learning survives
 * game restarts and the files become the future fine-tuning surface.
 *
 * No engine types and no I/O here — serialize/parse work on strings, so the
 * format is policy-testable. Identity is load-order independent: forms are
 * (plugin name, plugin-local id) pairs; the store layer converts to/from
 * runtime formIDs at the disk boundary ONLY.
 *
 * Allocation note: these are event-scoped value types (weapon equip / save
 * debounce), never per-frame — std::string/std::vector are intentional.
 */
namespace redux::motion_library
{
    inline constexpr std::uint32_t kFormatVersion = 1;

    // Load-order-independent form identity; empty = none (base-weapon part).
    struct FormRef
    {
        std::string plugin;
        std::uint32_t localFormId{ 0 };
        [[nodiscard]] bool empty() const { return plugin.empty() && localFormId == 0; }
    };

    struct StageData
    {
        bool used{ false };
        weapon_part_motion_path::MotionPath path{};
        std::uint32_t followerCount{ 0 };
        std::array<weapon_clip_stroke::AuthoredFollower, weapon_clip_stroke::kMaxFollowers> followers{};
        /*
         * Curation surface (phase 4): free-text labels round-tripped
         * verbatim, so hand edits survive runtime re-saves. The runtime
         * never invents values here.
         */
        std::string stageName;
        std::string notes;
    };

    struct PartRecord
    {
        FormRef omod;
        std::string sourceName;
        StageData learnedPrimary{};
        StageData learnedReturn{};
        StageData authored{};
        bool authoredFallback{ false };
    };

    struct WeaponLibrary
    {
        std::uint32_t formatVersion{ kFormatVersion };
        FormRef weapon;
        // Display name, informational only (never used as identity).
        std::string weaponName;
        /*
         * Hand-tuned file: the runtime loads it but NEVER overwrites or
         * deletes it (re-record wipes skip it too). Set by hand in the file.
         */
        bool curated{ false };
        std::vector<PartRecord> parts;
    };

    [[nodiscard]] std::string serialize(const WeaponLibrary& library);

    /*
     * Fail-closed parse: returns false when the document structure or format
     * version is unusable. Individual malformed parts/stages (hand-edit
     * typos) are SKIPPED, not fatal — the first such problem is described in
     * outError (also set on fatal failures) so the runtime can log it.
     */
    [[nodiscard]] bool parse(std::string_view jsonText, WeaponLibrary& out, std::string* outError);
}
