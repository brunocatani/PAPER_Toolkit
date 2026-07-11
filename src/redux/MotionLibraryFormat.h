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
     * Format-v2 spatial movement-preview profile.
     *
     * This is curated movement data, not a reload/clip authority. A physical
     * grip selects one interaction group, and that group cycles between its
     * explicitly linked primary/return-style stages. Each stage owns an exact
     * learner-recorded delta-pose control path and exact weapon-root-local
     * driver poses. Hand progress uses the learner's translation projection;
     * path rotations remain playback data, never controller input. No clip
     * interception, clip identity check, clock, gameplay event,
     * or reload-completion contract exists here. sourceClip is provenance;
     * mapped visibility/gameplay stays inert, while sound-kind findings may
     * request direct audio without graph notification.
     *
     * Values are equip-scoped and parsed off the hot path. The controller
     * copies only fixed-capacity state for per-frame work.
     */
    inline constexpr std::uint32_t kSpatialReloadProfileVersion = 1;
    inline constexpr std::size_t kMaxSpatialReloadGroups = 8;
    inline constexpr std::size_t kMaxSpatialReloadGripsPerGroup = 32;
    inline constexpr std::size_t kMaxSpatialReloadDrivers = 16;
    inline constexpr std::size_t kMaxSpatialReloadDriverKeys = 64;
    inline constexpr std::size_t kMaxSpatialReloadFollowersPerDriver = 64;
    inline constexpr std::size_t kMaxSpatialReloadConnectorsPerGroup = 16;
    inline constexpr std::size_t kMaxSpatialReloadStages = 16;
    inline constexpr std::size_t kMaxSpatialReloadEvents = 64;

    enum class SpatialReloadRuntimeMode : std::uint8_t
    {
        /*
         * Curated profiles may reorganize exact learner-recorded paths, but
         * they must retain the learner's proven translation-only hand
         * projection. Stored path rotations still play back; controller
         * rotation never becomes an input axis.
         */
        LearnerMovementPreview = 0,
    };

    struct SpatialReloadGripSource
    {
        std::string sourceName;
        FormRef omod;
        // Informational only; identity remains the FormRef above.
        std::string omodName;
        std::string role;
    };

    struct SpatialReloadDriver
    {
        // Independently animated rig node driven by PAPER.
        std::string node;
        std::string role;
        // Descendants carried by this driver. They are executable hierarchy
        // evidence and are never independently driven.
        std::vector<std::string> inheritedFollowers;
    };

    struct SpatialReloadInteractionGroup
    {
        std::string id;
        std::string role;
        std::string notes;
        // P-* nodes are connection subnodes only. They may prove the captured
        // hierarchy here, but can never be grips or physical driver identity.
        std::vector<std::string> connectorEvidence;
        // Alternative physical identities for supported assembly variants.
        // Every concrete match is exposed; the group fails closed only when
        // none bind. OMOD-qualified generic names may coexist with stable
        // mesh fallbacks without making either variant mandatory.
        std::vector<SpatialReloadGripSource> grips;
        std::vector<SpatialReloadDriver> drivers;
    };

    struct SpatialReloadDriverKey
    {
        float pathDistance{ 0.0f };
        weapon_part_motion_path::PoseSample pose{};
    };

    struct SpatialReloadDriverTrack
    {
        std::string node;
        float scale{ 1.0f };
        // Weapon-root-local absolute poses keyed explicitly by physical
        // distance along the stage control path. Variable keys preserve an
        // auxiliary latch/linkage motion even when the gripped part barely
        // moves; the parser bounds this to 64 keys per driver.
        std::vector<SpatialReloadDriverKey> keys;
    };

    struct SpatialReloadStage
    {
        std::string id;
        std::string groupId;
        // Stage-local normalized positions. The controller only scrubs from
        // entryFraction toward transitionFraction; both refer to the full
        // controlPath/driverTracks retained below.
        float entryFraction{ 0.0f };
        float transitionFraction{ 1.0f };
        std::string nextStageId;
        // Resolved by the parser; never serialized as identity.
        std::uint32_t nextStageIndex{ 0 };
        // Explicit handoff point on nextStage. This must agree with that
        // stage's entryFraction, making every cycle edge self-contained.
        float nextStageEntryFraction{ 0.0f };
        // Conceptual outward travel for diagnostics/integration. It is
        // independent of each captured stage's geometric arc length.
        float outwardFractionAtEntry{ 0.0f };
        float outwardFractionAtTransition{ 1.0f };
        // Grip-relative delta pose path. Key zero is identity; arc length is
        // translation plus rotation at the standard 3-unit lever radius.
        weapon_part_motion_path::MotionPath controlPath{};
        // Applied only when transitionFraction is the physical path endpoint.
        // Interior fractional gates (for example magazine exchange at 0.20)
        // transition on crossing, never early by this tolerance.
        float endpointTolerance{ 0.35f };
        std::vector<SpatialReloadDriverTrack> driverTracks;
        std::string notes;
    };

    enum class SpatialReloadMappedEventKind : std::uint8_t
    {
        Sound = 0,
        Visibility = 1,
        Gameplay = 2,
    };

    enum class SpatialReloadMappedEventTrigger : std::uint8_t
    {
        StageEnter = 0,
        GripStart = 1,
        PathPosition = 2,
        StageComplete = 3,
    };

    struct SpatialReloadMappedEvent
    {
        std::string id;
        std::string stageId;
        // Resolved by the parser. Visibility/gameplay stay inert; sound-kind
        // entries may be surfaced as edge-latched direct-audio requests.
        std::uint32_t stageIndex{ 0 };
        SpatialReloadMappedEventKind kind{ SpatialReloadMappedEventKind::Sound };
        SpatialReloadMappedEventTrigger trigger{ SpatialReloadMappedEventTrigger::PathPosition };
        // PathPosition metadata uses normalized spatial position in the full
        // stage, plus derived absolute distance and matching delta pose.
        float pathFraction{ 0.0f };
        float pathDistance{ 0.0f };
        bool targetPoseUsed{ false };
        weapon_part_motion_path::PoseSample targetPose{};
        // Captured graph annotation retained as metadata. The movement
        // controller never dispatches it and cannot complete gameplay. For
        // Sound only, integration may play the payload directly after
        // stripping the captured "Soundplay." prefix.
        std::string sourceEvent;
        std::string notes;
    };

    struct SpatialReloadProfile
    {
        bool used{ false };
        std::uint32_t profileVersion{ kSpatialReloadProfileVersion };
        SpatialReloadRuntimeMode runtimeMode{ SpatialReloadRuntimeMode::LearnerMovementPreview };
        std::string archetype;
        std::string sourceCapture;
        std::string sourceActivityId;
        // Offline provenance only; never matched, intercepted, frozen, or
        // sampled by the movement-preview controller.
        std::string sourceClip;
        std::string notes;
        std::vector<SpatialReloadInteractionGroup> groups;
        std::vector<SpatialReloadStage> stages;
        std::vector<SpatialReloadMappedEvent> mappedEvents;
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
        // Curated movement-preview data for physical part grips.
        SpatialReloadProfile spatialReload{};
    };

    [[nodiscard]] std::string serialize(const WeaponLibrary& library);

    /*
     * Fail-closed parse: returns false when the document structure or format
     * version is unusable. Individual malformed legacy motion parts/stages
     * (hand-edit typos) are SKIPPED, not fatal. A spatial preview profile is
     * an all-or-nothing movement contract, so any malformed field in it is
     * fatal. The first problem is described in outError so the runtime can
     * log it.
     */
    [[nodiscard]] bool parse(std::string_view jsonText, WeaponLibrary& out, std::string* outError);
}
