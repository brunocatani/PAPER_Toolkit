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
    inline constexpr std::uint32_t kFormatVersion = 2;

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

    /*
     * Format-v2 authoritative reload profile.
     *
     * A profile is intentionally different from a collection of learned
     * paths. It binds one verified live reload clip to explicit interaction
     * groups and contiguous stage windows. While the profile is active the
     * engine remains the pose oracle for the complete weapon rig; PAPER only
     * controls clip time and which concrete ROCK collider may manipulate the
     * current stage. This preserves inherited scene-graph motion and avoids
     * independently driving a parent and its descendants.
     *
     * These are equip-scoped values loaded from disk, never per-frame
     * allocations. Fixed runtime limits are validated by the parser before a
     * profile can become authoritative.
     */
    inline constexpr std::uint32_t kAuthoritativeProfileVersion = 1;
    inline constexpr std::size_t kMaxAuthoritativeGroups = 8;
    inline constexpr std::size_t kMaxAuthoritativeGripsPerGroup = 32;
    inline constexpr std::size_t kMaxAuthoritativeDriversPerGroup = 16;
    inline constexpr std::size_t kMaxAuthoritativeFollowersPerDriver = 64;
    inline constexpr std::size_t kMaxAuthoritativeStages = 16;
    inline constexpr std::size_t kMaxAuthoritativeEvents = 64;

    struct AuthoritativeGripSource
    {
        std::string sourceName;
        FormRef omod;
        // Informational only; identity remains the FormRef above.
        std::string omodName;
        std::string role;
    };

    struct AuthoritativeDriver
    {
        // Independently animated rig node driven by the live clip.
        std::string node;
        std::string role;
        // Nodes that must inherit this driver's motion and therefore must
        // never be independently driven by PAPER.
        std::vector<std::string> inheritedFollowers;
    };

    struct AuthoritativeInteractionGroup
    {
        std::string id;
        std::string role;
        std::string notes;
        std::vector<AuthoritativeGripSource> grips;
        std::vector<AuthoritativeDriver> drivers;
    };

    struct AuthoritativeStage
    {
        std::string id;
        std::string groupId;
        float startSeconds{ 0.0f };
        float endSeconds{ 0.0f };
        std::string notes;
    };

    enum class AuthoritativeEventKind : std::uint8_t
    {
        Sound = 0,
        Visibility = 1,
        Gameplay = 2,
    };

    struct AuthoritativeTimelineEvent
    {
        std::string id;
        float timeSeconds{ 0.0f };
        AuthoritativeEventKind kind{ AuthoritativeEventKind::Sound };
        // Exact annotation payload sent to the player's animation graph.
        std::string animationEvent;
        std::string notes;
    };

    struct AuthoritativeReloadProfile
    {
        bool used{ false };
        std::uint32_t profileVersion{ kAuthoritativeProfileVersion };
        std::string archetype;
        std::string sourceCapture;
        std::string notes;
        std::string clipNameContains;
        float expectedDurationSeconds{ 0.0f };
        float durationToleranceSeconds{ 0.05f };
        // Mode-2 control ends here; the engine resumes natively afterward.
        float releaseSeconds{ 0.0f };
        std::vector<AuthoritativeInteractionGroup> groups;
        std::vector<AuthoritativeStage> stages;
        std::vector<AuthoritativeTimelineEvent> events;
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
        // When present, this profile supersedes Hybrid/Authored/Learned/
        // Scrub selection for this weapon only.
        AuthoritativeReloadProfile authoritativeReload{};
    };

    [[nodiscard]] std::string serialize(const WeaponLibrary& library);

    /*
     * Fail-closed parse: returns false when the document structure or format
     * version is unusable. Individual malformed legacy motion parts/stages
     * (hand-edit typos) are SKIPPED, not fatal. An authoritative profile is
     * an all-or-nothing runtime contract, so any malformed field in it is
     * fatal. The first problem is described in outError so the runtime can
     * log it.
     */
    [[nodiscard]] bool parse(std::string_view jsonText, WeaponLibrary& out, std::string* outError);
}
