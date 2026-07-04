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
     * instantly with whatever both sources have accumulated, and neither
     * source can destroy the other's data (previously a learned stroke
     * overwrote the authored record in place, which would starve
     * authored-only mode the moment learning kicked in).
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
        // simultaneously: co-movement grouping (a mag pulls its bullets, a
        // slide carries its sights) needs concurrent recordings to compare,
        // and later-phase motions (mag insertion, lever-action feeding) need
        // parts that are still at grab time to be watched too.
        static constexpr std::size_t kMaxActiveRecorders = 48;
        static constexpr std::size_t kMaxSourceName = 64;
        // A recorder whose part was not observed for this many observations is
        // stale (weapon switched / part removed) and may be reclaimed.
        static constexpr std::uint64_t kRecorderStaleObservationAge = 300;

        struct Observation
        {
            std::uint32_t weaponFormId{ 0 };
            std::string_view sourceName{};
            weapon_part_motion_path::PoseSample pose{};
            // False when a provider drive owned the node this frame.
            bool trusted{ true };
        };

        // Call once per frame before the frame's observe() calls: recorder
        // samples are frame-aligned through this counter so concurrent
        // recordings can be compared sample-for-sample.
        void beginObservationFrame();

        void observe(const Observation& observation);

        [[nodiscard]] const weapon_part_motion_path::MotionPath* findPath(
            std::uint32_t weaponFormId,
            std::string_view sourceName,
            MotionPathMode mode) const;

        /*
         * Full stroke-group view for a part under the given mode: the leader
         * path plus any followers — authored (assembly parts the clip moves
         * with it) or learned (parts observed moving rigidly with the leader
         * during the same stroke). Hybrid serves learned over authored.
         */
        struct GroupView
        {
            const weapon_part_motion_path::MotionPath* leaderPath{ nullptr };
            const weapon_clip_stroke::AuthoredFollower* followers{ nullptr };
            std::uint32_t followerCount{ 0 };
            bool authored{ false };
            // Authored-only: stroke came from a merely-loaded (fallback)
            // clip rather than one the weapon activated.
            bool fallbackSource{ false };
        };
        [[nodiscard]] GroupView findGroup(std::uint32_t weaponFormId, std::string_view sourceName, MotionPathMode mode) const;

        // Which sources hold data for a part; for source-selection logging
        // ("mode=authored but only learned data exists").
        struct SourceAvailability
        {
            bool learned{ false };
            bool authored{ false };
            bool authoredFallback{ false };
        };
        [[nodiscard]] SourceAvailability sourceAvailability(std::uint32_t weaponFormId, std::string_view sourceName) const;

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
            std::uint32_t weaponFormId,
            std::string_view sourceName,
            const weapon_clip_stroke::AuthoredStrokeGroup& group,
            bool fallbackSource);

        // Bumped on every stored/replaced path (either source) and on
        // reset; consumers re-resolve path-dependent state (per-part
        // attach-only targets) when it moves.
        [[nodiscard]] std::uint64_t revision() const { return _revision; }

        void reset();

    private:
        // One source's stroke for a part: leader path plus followers.
        struct PathRecord
        {
            bool used{ false };
            // Authored-only: stroke from a merely-loaded (fallback) clip;
            // outranked by the weapon's own activated-clip strokes.
            bool fallbackSource{ false };
            weapon_part_motion_path::MotionPath path{};
            std::uint32_t followerCount{ 0 };
            std::array<weapon_clip_stroke::AuthoredFollower, weapon_clip_stroke::kMaxFollowers> followers{};
        };

        struct PathSlot
        {
            bool used{ false };
            std::uint32_t weaponFormId{ 0 };
            std::array<char, kMaxSourceName> sourceName{};
            std::uint64_t lastUseCounter{ 0 };
            PathRecord learned{};
            PathRecord authored{};
        };

        struct RecorderSlot
        {
            bool used{ false };
            std::uint32_t weaponFormId{ 0 };
            std::array<char, kMaxSourceName> sourceName{};
            std::uint64_t lastSeenCounter{ 0 };
            // Frame index of buffer[1] (buffer[0] is the pre-motion rest
            // pose); aligns concurrent recordings for co-movement checks.
            std::uint64_t startFrame{ 0 };
            weapon_part_motion_path::RecorderState state{};
            std::array<weapon_part_motion_path::PoseSample, weapon_part_motion_path::kMaxRecordingSamples> buffer{};
        };

        [[nodiscard]] static const PathRecord* selectRecord(const PathSlot& slot, MotionPathMode mode);
        [[nodiscard]] const PathSlot* findSlot(std::uint32_t weaponFormId, std::string_view sourceName) const;
        // preferSlotsWithoutLearnedData: authored stores must not evict a
        // slot holding a learned stroke while a purely-authored slot exists.
        PathSlot* findOrClaimSlot(std::uint32_t weaponFormId, std::string_view sourceName, bool preferSlotsWithoutLearnedData);
        RecorderSlot* acquireRecorderSlot(std::uint32_t weaponFormId, std::string_view sourceName);
        void storeCompletedPath(const RecorderSlot& recorder);

        std::array<PathSlot, kMaxStoredPaths> _paths{};
        std::array<RecorderSlot, kMaxActiveRecorders> _recorders{};
        std::uint64_t _observationCounter{ 0 };
        std::uint64_t _frameCounter{ 0 };
        std::uint64_t _revision{ 0 };
    };
}
