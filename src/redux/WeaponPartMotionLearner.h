#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "redux/MotionPathMode.h"
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
        static constexpr std::size_t kMaxStoredPaths = 64;
        // One recorder per evidence part — the learner observes EVERY part,
        // unfiltered by grab eligibility, so every mover of a reload records
        // simultaneously: co-movement grouping needs concurrent recordings
        // to compare, and later-phase motions need parts that are still at
        // grab time to be watched too.
        static constexpr std::size_t kMaxActiveRecorders = 48;
        static constexpr std::size_t kMaxSourceName = 64;
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
            // Frame index of buffer[1] (buffer[0] is the pre-motion rest
            // pose); aligns concurrent recordings for co-movement checks.
            std::uint64_t startFrame{ 0 };
            weapon_part_motion_path::RecorderState state{};
            std::array<weapon_part_motion_path::PoseSample, weapon_part_motion_path::kMaxRecordingSamples> buffer{};
        };

        [[nodiscard]] static const StrokeGroup* selectPrimary(const PathSlot& slot, MotionPathMode mode);
        [[nodiscard]] const PathSlot* findSlot(const PartKey& key) const;
        // preferSlotsWithoutLearnedData: authored stores must not evict a
        // slot holding a learned stroke while a purely-authored slot exists.
        PathSlot* findOrClaimSlot(const PartKey& key, bool preferSlotsWithoutLearnedData);
        RecorderSlot* acquireRecorderSlot(const PartKey& key);
        void storeCompletedPath(const RecorderSlot& recorder);

        std::array<PathSlot, kMaxStoredPaths> _paths{};
        std::array<RecorderSlot, kMaxActiveRecorders> _recorders{};
        GroupingTuning _tuning{};
        std::uint64_t _observationCounter{ 0 };
        std::uint64_t _frameCounter{ 0 };
        std::uint64_t _revision{ 0 };
    };
}
