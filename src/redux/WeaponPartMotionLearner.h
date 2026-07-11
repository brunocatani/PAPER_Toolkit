#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "redux/MotionPathMode.h"
#include "redux/RichMotionCaptureFormat.h"
#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartMotionPathPolicy.h"

namespace redux
{
    /*
     * Passive runtime that learns weapon-part motion paths from engine
     * animation. ReduxRuntime feeds it one observation per evidence part
     * per frame, sampled in weapon-root-local space; observations on frames
     * where our drive owned the node arrive with trusted=false so the
     * recorder never learns our own authority back.
     *
     * Storage keeps the LEARNED and AUTHORED sources SIDE BY SIDE per
     * (weapon form ID, part source name) key: collection is unrestricted in
     * every mode, and the INI-selected MotionPathMode picks the serving
     * source at lookup time only — so a hot-reload mode switch applies
     * instantly with whatever data both sources have accumulated, and
     * neither source can destroy the other's data.
     *
     * The learned source additionally keeps a RETURN STAGE per part: a
     * completed stroke whose start pose chains onto the primary stroke's
     * end pose (mag-in observed after mag-out). The scrub consumer flips
     * between the stages at the path extremes, so extraction and insertion
     * keep their own paths and their own min/max instead of the insertion
     * stroke being discarded by the largest-stroke rule.
     *
     * Followers come in two tiers, both recovered from frame-aligned
     * concurrent recordings:
     *  - RIGID: constant leader distance over the stroke (a slide carrying
     *    its sights, a mag carrying its bullets) — the original criterion.
     *  - CO-TIMED (staged, tunable/gated): moves non-rigidly but
     *    temporally CONTAINED in the leader's stroke window — the P320
     *    barrel tilting while the slide travels, a bullet advancing during
     *    the bolt pull. Containment (fraction of the follower's total
     *    motion inside the window) is what keeps separate reload phases
     *    out: a mag dropped in another phase of the same animation shares
     *    no window with the bolt stroke and never groups.
     *
     * Fixed-capacity with oldest-use eviction; no allocation after
     * construction and no I/O. Main-thread only (ROCK frame callback).
     */
    class WeaponPartMotionLearner
    {
    public:
        // A rich modded weapon can expose close to ROCK's 100-body budget;
        // keep enough serving records for several currently encountered
        // weapons without evicting data before its weapon-switch flush.
        static constexpr std::size_t kMaxStoredPaths = 256;
        // One recorder per evidence part — the learner observes EVERY part,
        // unfiltered by grab eligibility, so every mover of a reload records
        // simultaneously: co-movement grouping needs concurrent recordings
        // to compare, and later-phase motions need parts that are still at
        // grab time to be watched too.
        static constexpr std::size_t kMaxActiveRecorders = 128;
        static constexpr std::size_t kMaxSourceName = 64;
        static constexpr std::size_t kMaxCaptureNodePath = 256;
        static constexpr std::uint32_t kMinimumFollowerOverlapSamples = 8;
        // A recorder whose part was not observed for this many observations is
        // stale (weapon switched / part removed) and may be reclaimed.
        static constexpr std::uint64_t kRecorderStaleObservationAge = 300;

        /*
         * Identity of one concrete part (Bruno, 2026-07-05 long-term
         * scalability): the weapon form, the installed OMOD occupying the
         * part's slot (ROCK record identity; 0 for base-weapon/unpaired
         * parts), and the nif node name. The OMOD component is what makes
         * learned data workbench-safe — two mods sharing a node name (two
         * receivers both named "Receiver") keep separate records, so a part
         * swap can only ever find ITS OWN data or none, never a lookalike's.
         * With a pre-record-identity ROCK every omodFormId arrives 0 and
         * keying degrades to the old (weapon, name) behavior. Runtime
         * formIDs only — persistence (phase 2) normalizes to plugin-local
         * ids at the disk boundary, never here.
         */
        struct PartKey
        {
            std::uint32_t weaponFormId{ 0 };
            std::uint32_t omodFormId{ 0 };
            std::string_view sourceName{};
        };

        struct Observation
        {
            std::uint32_t weaponFormId{ 0 };
            // Installed OMOD occupying this part's slot; see PartKey.
            std::uint32_t omodFormId{ 0 };
            std::string_view sourceName{};
            weapon_part_motion_path::PoseSample pose{};
            /*
             * Weapon-root-local scale of the node this frame. Paths do not
             * animate scale, but DRIVES must restate it — modders author
             * part nodes at non-1 scales, and a drive that omits the real
             * scale rescales the mesh (in-game 2026-07-04: learned
             * followers driven at scale 1 made a Glock slide piece and a
             * hunting-rifle bullet comically giant).
             */
            float scale{ 1.0f };
            // False when a provider drive owned the node this frame.
            bool trusted{ true };
            // Rich-capture diagnostics/identity. None of these affect the
            // serving key or learner policy.
            std::uint64_t rockFrameIndex{ 0 };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t catalogPartId{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::string_view nodePath{};
            bool nodePathTruncated{ false };
            std::uint64_t clipActivityId{ 0 };
            std::uint64_t clipScrubSessionId{ 0 };
            std::uint32_t clipConcurrentActivityCount{ 0 };
            float clipFraction{ 0.0f };
            float clipLocalTimeSeconds{ 0.0f };
            std::string_view clipName{};
            float clipDurationSeconds{ 0.0f };
            float clipCroppedDurationSeconds{ 0.0f };
        };

        /*
         * Grouping/staging tuning, INI-backed and hot-reloadable; applies to
         * FUTURE completed recordings (stored groups are not regrouped).
         */
        struct GroupingTuning
        {
            // Enable the co-timed (non-rigid) follower tier.
            bool coTimedFollowers{ true };
            // Fraction of the follower's total recorded motion that must
            // fall inside the leader's stroke window.
            float coTimedMinOverlapFraction{ 0.7f };
            // Follower motion inside the window may be at most this many
            // times the leader's stroke arc (blocks unrelated big movers).
            float coTimedMaxArcRatio{ 1.5f };
            // Enable return-stage capture (mag-in chaining onto mag-out).
            bool stageCapture{ true };
            // Pose distance within which a stroke's start "chains" onto the
            // primary stroke's end.
            float stageChainToleranceGameUnits{ 2.0f };
        };
        void setGroupingTuning(const GroupingTuning& tuning) { _tuning = tuning; }

        // Call once per frame before the frame's observe() calls: recorder
        // samples are frame-aligned through this counter so concurrent
        // recordings can be compared sample-for-sample.
        void beginObservationFrame();

        void observe(const Observation& observation);

        /*
         * Rich raw-capture sink. The view and every pointed-to array remain
         * valid only during the callback. The runtime immediately copies it
         * into an owned, event-scoped value and the disk writer serializes it
         * off-thread. No callback means the original learner hot path.
         */
        struct RawCaptureView
        {
            struct SelectedFollowerView
            {
                std::uint32_t omodFormId{ 0 };
                std::uint32_t catalogPartId{ 0 };
                std::uint32_t bodyId{ 0x7FFF'FFFFu };
                std::string_view sourceName{};
                bool rigid{ false };
            };
            std::uint32_t weaponFormId{ 0 };
            std::uint32_t omodFormId{ 0 };
            std::string_view sourceName{};
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t catalogPartId{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::string_view nodePath{};
            bool nodePathTruncated{ false };
            rich_capture::StrokeTermination termination{ rich_capture::StrokeTermination::Settled };
            rich_capture::ServingDecision servingDecision{ rich_capture::ServingDecision::NotEvaluated };
            std::uint64_t learnerStartFrame{ 0 };
            const weapon_part_motion_path::PoseSample* samples{ nullptr };
            const std::uint64_t* rockFrameIndices{ nullptr };
            const float* scales{ nullptr };
            const std::uint64_t* clipActivityIds{ nullptr };
            const std::uint64_t* clipScrubSessionIds{ nullptr };
            const std::uint32_t* clipConcurrentActivityCounts{ nullptr };
            const float* clipFractions{ nullptr };
            const float* clipLocalTimesSeconds{ nullptr };
            std::uint32_t sampleCount{ 0 };
            bool terminalSamplePresent{ false };
            Observation terminalSample{};
            std::uint32_t peakSampleIndex{ 0 };
            float peakExcursion{ 0.0f };
            float fullRecordingArcLength{ 0.0f };
            weapon_part_motion_path::MotionPath candidatePath{};
            bool classifiedAsReturnStage{ false };
            bool replacedServingPath{ false };
            std::uint32_t selectedRigidFollowerCount{ 0 };
            std::uint32_t selectedCoTimedFollowerCount{ 0 };
            const SelectedFollowerView* selectedFollowers{ nullptr };
            std::uint32_t selectedFollowerCount{ 0 };
            GroupingTuning tuning{};
            std::string_view clipName{};
            float clipDurationSeconds{ 0.0f };
            float clipCroppedDurationSeconds{ 0.0f };
        };
        using RawCaptureSink = void (*)(const RawCaptureView&, void* context);
        void setRawCaptureSink(RawCaptureSink sink, void* context)
        {
            _rawCaptureSink = sink;
            _rawCaptureContext = context;
        }

        // End and preserve every in-flight recorder before a weapon switch,
        // reset, capture-disable transition, or shutdown. Serving paths are
        // untouched; recorder slots become immediately reusable.
        void resetRecorders(rich_capture::StrokeTermination termination);

        [[nodiscard]] const weapon_part_motion_path::MotionPath* findPath(
            const PartKey& key,
            MotionPathMode mode) const;

        /*
         * Full stroke-group view for a part under the given mode: the leader
         * path plus its followers, and — learned source only — the return
         * stage the scrub consumer flips to at the path extremes. Hybrid
         * serves learned over authored.
         */
        struct GroupView
        {
            const weapon_part_motion_path::MotionPath* leaderPath{ nullptr };
            const weapon_clip_stroke::AuthoredFollower* followers{ nullptr };
            std::uint32_t followerCount{ 0 };
            const weapon_part_motion_path::MotionPath* returnPath{ nullptr };
            const weapon_clip_stroke::AuthoredFollower* returnFollowers{ nullptr };
            std::uint32_t returnFollowerCount{ 0 };
            bool authored{ false };
            // Authored-only: stroke came from a merely-loaded (fallback)
            // clip rather than one the weapon activated.
            bool fallbackSource{ false };
        };
        [[nodiscard]] GroupView findGroup(const PartKey& key, MotionPathMode mode) const;

        // Which sources hold data for a part; for source-selection logging
        // ("mode=authored but only learned data exists").
        struct SourceAvailability
        {
            bool learned{ false };
            bool authored{ false };
            bool authoredFallback{ false };
        };
        [[nodiscard]] SourceAvailability sourceAvailability(const PartKey& key) const;

        /*
         * Store a clip-harvested stroke group (already converted to
         * weapon-root-local and mapped to the evidence source name) into the
         * AUTHORED record. `fallbackSource` marks strokes from clips merely
         * found LOADED on the graph (shared/template data): a stroke from a
         * clip the weapon ACTIVATED always beats a fallback stroke for the
         * same part, and fallback data is used only when it is all that
         * exists. Within the same tier the largest leader stroke wins.
         */
        void storeAuthoredGroup(
            const PartKey& key,
            const weapon_clip_stroke::AuthoredStrokeGroup& group,
            bool fallbackSource);

        /*
         * Motion-library bridge (phase 2). StageView pointers are non-owning
         * views into learner storage (export) or caller storage (import),
         * valid only for the duration of the call chain — never retained.
         */
        struct StageView
        {
            const weapon_part_motion_path::MotionPath* path{ nullptr };
            const weapon_clip_stroke::AuthoredFollower* followers{ nullptr };
            std::uint32_t followerCount{ 0 };
        };
        struct RecordView
        {
            std::uint32_t omodFormId{ 0 };
            std::string_view sourceName{};
            StageView learnedPrimary{};
            StageView learnedReturn{};
            StageView authored{};
            bool authoredFallback{ false };
        };

        // Read-only visit of every stored record for a weapon; returns the
        // number written to out (capacity max). Used by the save path.
        std::uint32_t exportWeaponRecords(std::uint32_t weaponFormId, RecordView* out, std::uint32_t max) const;

        /*
         * Seed a record from the disk library. DISK SEEDS, LIVE LEARNING
         * WINS: each stage is applied only when the in-RAM record for that
         * stage is unused, so a fresher stroke learned this session is never
         * clobbered by an equip re-import. Returns true when anything was
         * applied (revision bumps once).
         */
        bool importRecord(const PartKey& key, const RecordView& record);

        // Bumped on every stored/replaced path (either source) and on
        // reset; consumers re-resolve path-dependent state (per-part
        // attach-only targets) when it moves.
        [[nodiscard]] std::uint64_t revision() const { return _revision; }

        void reset();

    private:
        // One stage of one source: leader path plus followers.
        struct StrokeGroup
        {
            bool used{ false };
            weapon_part_motion_path::MotionPath path{};
            std::uint32_t followerCount{ 0 };
            std::array<weapon_clip_stroke::AuthoredFollower, weapon_clip_stroke::kMaxFollowers> followers{};
        };

        struct PathSlot
        {
            bool used{ false };
            std::uint32_t weaponFormId{ 0 };
            std::uint32_t omodFormId{ 0 };
            std::array<char, kMaxSourceName> sourceName{};
            std::uint64_t lastUseCounter{ 0 };
            StrokeGroup learnedPrimary{};
            // Chains off learnedPrimary's END pose (insertion after
            // extraction); dropped when a replaced primary breaks the chain.
            StrokeGroup learnedReturn{};
            StrokeGroup authored{};
            // Authored stroke came from a merely-loaded (fallback) clip;
            // outranked by the weapon's own activated-clip strokes.
            bool authoredFallback{ false };
        };

        struct RecorderSlot
        {
            bool used{ false };
            std::uint32_t weaponFormId{ 0 };
            std::uint32_t omodFormId{ 0 };
            std::array<char, kMaxSourceName> sourceName{};
            std::uint64_t lastSeenCounter{ 0 };
            // Last observed weapon-root-local scale of the part; stamped on
            // learned followers so drives restore the authored mesh scale.
            float lastScale{ 1.0f };
            std::uint64_t lastRockFrameIndex{ 0 };
            std::uint64_t lastClipActivityId{ 0 };
            std::uint64_t lastClipScrubSessionId{ 0 };
            std::uint32_t lastClipConcurrentActivityCount{ 0 };
            float lastClipFraction{ 0.0f };
            float lastClipLocalTimeSeconds{ 0.0f };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t catalogPartId{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::array<char, kMaxCaptureNodePath> nodePath{};
            bool nodePathTruncated{ false };
            std::array<char, kMaxSourceName> clipName{};
            float clipDurationSeconds{ 0.0f };
            float clipCroppedDurationSeconds{ 0.0f };
            // Frame index of buffer[1] (buffer[0] is the pre-motion rest
            // pose); aligns concurrent recordings for co-movement checks.
            std::uint64_t startFrame{ 0 };
            weapon_part_motion_path::RecorderState state{};
            std::array<weapon_part_motion_path::PoseSample, weapon_part_motion_path::kMaxRecordingSamples> buffer{};
            std::array<std::uint64_t, weapon_part_motion_path::kMaxRecordingSamples> rockFrameIndices{};
            std::array<float, weapon_part_motion_path::kMaxRecordingSamples> scales{};
            std::array<std::uint64_t, weapon_part_motion_path::kMaxRecordingSamples> clipActivityIds{};
            std::array<std::uint64_t, weapon_part_motion_path::kMaxRecordingSamples> clipScrubSessionIds{};
            std::array<std::uint32_t, weapon_part_motion_path::kMaxRecordingSamples>
                clipConcurrentActivityCounts{};
            std::array<float, weapon_part_motion_path::kMaxRecordingSamples> clipFractions{};
            std::array<float, weapon_part_motion_path::kMaxRecordingSamples> clipLocalTimesSeconds{};
        };

        struct StoreCompletedResult
        {
            rich_capture::ServingDecision decision{ rich_capture::ServingDecision::NotEvaluated };
            weapon_part_motion_path::MotionPath candidate{};
            std::uint32_t peakSampleIndex{ 0 };
            float peakExcursion{ 0.0f };
            bool isReturnStage{ false };
            bool replaced{ false };
            std::uint32_t rigidFollowerCount{ 0 };
            std::uint32_t coTimedFollowerCount{ 0 };
            std::array<RawCaptureView::SelectedFollowerView, weapon_clip_stroke::kMaxFollowers> selectedFollowers{};
            std::uint32_t selectedFollowerCount{ 0 };
        };

        [[nodiscard]] static const StrokeGroup* selectPrimary(const PathSlot& slot, MotionPathMode mode);
        [[nodiscard]] const PathSlot* findSlot(const PartKey& key) const;
        // preferSlotsWithoutLearnedData: authored stores must not evict a
        // slot holding a learned stroke while a purely-authored slot exists.
        PathSlot* findOrClaimSlot(const PartKey& key, bool preferSlotsWithoutLearnedData);
        RecorderSlot* acquireRecorderSlot(
            const PartKey& key,
            std::uint64_t weaponGenerationKey,
            std::uint32_t catalogPartId);
        StoreCompletedResult storeCompletedPath(const RecorderSlot& recorder);
        void emitRawCapture(
            const RecorderSlot& recorder,
            std::uint32_t sampleCount,
            rich_capture::StrokeTermination termination,
            const StoreCompletedResult* storeResult,
            const Observation* terminalSample);

        std::array<PathSlot, kMaxStoredPaths> _paths{};
        std::array<RecorderSlot, kMaxActiveRecorders> _recorders{};
        GroupingTuning _tuning{};
        std::uint64_t _observationCounter{ 0 };
        std::uint64_t _frameCounter{ 0 };
        std::uint64_t _revision{ 0 };
        RawCaptureSink _rawCaptureSink{ nullptr };
        void* _rawCaptureContext{ nullptr };
    };
}
