#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "redux/MotionPathMode.h"
#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartMotionPathPolicy.h"

namespace redux
{
    class WeaponPartMotionLearner;

    /*
     * The drive loop of the reload runtime: registers PAPER_Redux as a ROCK
     * provider consumer, installs a NonExclusive AttachOnly whitelist of
     * PER-PART targets (only the parts the runtime resolved as allowlisted
     * AND moving — so unmapped parts and every other grip surface keep
     * their normal behavior), and drives a gripped part along its
     * learned/authored motion path with setWeaponPartDriveTargetsV1.
     * Engine access stays in ReduxRuntime: this class receives plain
     * weapon-root-local data per frame and talks only to ROCK's provider
     * API.
     *
     * Ownership/lifetime: registration is lazy on the first enabled update and
     * torn down by shutdown() (drive + whitelist cleared, consumer
     * unregistered). If ROCK drops this owner's registration (provider reset),
     * the OwnerNotRegistered result re-arms lazy registration. Main-thread
     * only (ROCK frame callback). Drive leases are 2 frames, so a lost frame
     * fails closed into ROCK's baseline-restore path.
     */
    class WeaponPartDriveSandbox
    {
    public:
        static constexpr std::size_t kMaxSourceName = 64;

        /*
         * What this update sent a drive for; the runtime marks these parts'
         * observations untrusted so the learner never records our own
         * authority back as animation evidence. Leaders identify by bodyId,
         * followers by source name (they are driven without collider
         * evidence).
         */
        struct SentDrive
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::array<char, kMaxSourceName> sourceName{};
        };
        static constexpr std::size_t kMaxSentDrives = 2 * (1 + weapon_clip_stroke::kMaxFollowers);

        struct HandInput
        {
            // Attach-only grip on a part owned by this sandbox's whitelist;
            // false ends any session for the hand.
            bool gripActive{ false };
            /*
             * Trigger arming: true when this hand's trigger is currently
             * held (or trigger selection is disabled/unavailable). A scrub
             * session only STARTS while this is true; once started it runs
             * until the grip ends — the trigger is never rechecked, so
             * "press or hold to unlock, stays unlocked until the part is
             * released".
             */
            bool triggerHeld{ false };
            std::uint64_t gripSequence{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::string_view sourceName{};
            // All positions weapon-root-local, valid only when resolvable.
            bool transformsValid{ false };
            weapon_part_motion_path::Vec3 partTranslate{};
            float partScale{ 1.0f };
            weapon_part_motion_path::Vec3 handTranslate{};
            /*
             * Weapon-local pose the part settles at when idle (runtime
             * rest-pose capture): the reference for the delta curve that
             * anchors max-travel events and stage-transition triggers.
             * When unavailable the path's first key stands in — correct for
             * forward-recorded strokes, which is all the old behavior was.
             */
            bool restPoseValid{ false };
            weapon_part_motion_path::PoseSample restPose{};
        };

        /*
         * Emitted when a scrub session first reaches the far end of its
         * PRIMARY stage ("max travel" — slide fully back, bolt fully open);
         * re-armed once the scrub retreats below half the path, so racking
         * repeatedly emits once per full stroke while end-zone jitter cannot
         * spam. Sessions that START inside the end zone latch silently
         * (grabbing an already-out part is not a stroke), and the return
         * stage never emits — its far end is the rest pose. The runtime
         * turns these into engine shell-eject calls (test feature).
         */
        struct MaxTravelEvent
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::array<char, kMaxSourceName> sourceName{};
            // Diagnostics for tuning: where the scrub was, where the delta
            // curve put the extreme, and how tall the curve is.
            float arcPosition{ 0.0f };
            float extremeArcPosition{ 0.0f };
            float peakDelta{ 0.0f };
        };
        // One per hand at most per update.
        static constexpr std::size_t kMaxMaxTravelEvents = 2;

        // One whitelist-eligible part of the current weapon: allowlisted
        // class AND a motion path exists under the active mode ("must
        // move"). The runtime computes these; this class encodes them as
        // per-bodyId NonExclusive AttachOnly provider targets.
        struct EligiblePart
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::array<char, kMaxSourceName> sourceName{};
        };
        // Matches the runtime's drive-part cache capacity.
        static constexpr std::size_t kMaxEligibleParts = 48;

        struct FrameInput
        {
            std::uint32_t weaponFormId{ 0 };
            std::uint64_t weaponGenerationKey{ 0 };
            // INI-selected path source. Pinned into each session at grip
            // start so a hot-reload mode switch never swaps the path under a
            // hand mid-scrub; it applies to the next grip.
            MotionPathMode motionPathMode{ MotionPathMode::Hybrid };
            /*
             * Stage handoff at the path extremes: reaching the end of the
             * active stage hands the session over to the learned return
             * stage (mag-out end -> mag-in path, and back), each stage with
             * its own min/max. Epsilon is how close to the end (in arc
             * units) the scrub must reach to hand off.
             */
            bool stageTransitionsEnabled{ true };
            float stageEndEpsilonArcUnits{ 0.35f };
            // Per-part attach-only whitelist for the current weapon
            // generation; targets reinstall only when this set changes.
            std::uint32_t eligiblePartCount{ 0 };
            std::array<EligiblePart, kMaxEligibleParts> eligibleParts{};
            // Indexed [0]=right, [1]=left to match hand-state conventions.
            std::array<HandInput, 2> hands{};
        };

        // Returns the number of drives sent this update, written to
        // outSentDrives (capacity kMaxSentDrives). Max-travel events land in
        // outMaxTravelEvents (capacity kMaxMaxTravelEvents) when provided.
        std::uint32_t update(
            const FrameInput& input,
            const WeaponPartMotionLearner& learner,
            SentDrive* outSentDrives,
            MaxTravelEvent* outMaxTravelEvents = nullptr,
            std::uint32_t* outMaxTravelEventCount = nullptr);
        void shutdown();

        [[nodiscard]] std::uint64_t ownerToken() const { return _ownerToken; }

    private:
        struct HandSession
        {
            bool active{ false };
            std::uint64_t gripSequence{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t weaponFormId{ 0 };
            // Path source pinned at grip start (see FrameInput).
            MotionPathMode mode{ MotionPathMode::Hybrid };
            // Which learned stage the session is scrubbing; flips at the
            // stage ends when a chained return stage exists.
            bool onReturnStage{ false };
            // Max-travel event latch (see MaxTravelEvent): true while inside
            // the primary stage's end zone or until the re-arm point.
            bool maxTravelLatched{ false };
            std::array<char, kMaxSourceName> sourceName{};
            float arcPosition{ 0.0f };
            weapon_part_motion_path::Vec3 handStartTranslate{};
            // On-path point nearest the part at grip start; hand displacement
            // is applied relative to it so the desired point stays anchored to
            // the path's own geometry.
            weapon_part_motion_path::Vec3 pathAnchorTranslate{};
            float partScale{ 1.0f };
            /*
             * Authored assembly followers copied at grip start (the learner
             * slot can be replaced mid-session by a fresh harvest, so the
             * session owns its data). Each follower node is driven at the
             * same stroke progress as the leader.
             */
            std::uint32_t followerCount{ 0 };
            std::array<weapon_clip_stroke::AuthoredFollower, weapon_clip_stroke::kMaxFollowers> followers{};
        };

        bool ensureRegistered();
        // Install/refresh the per-part targets when the eligible set or the
        // weapon generation changed; cheap no-op otherwise.
        void ensureTargetsInstalled(const FrameInput& input);
        void endSession(HandSession& session);

        // What is currently installed with ROCK, for change detection.
        struct InstalledTargets
        {
            bool any{ false };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t count{ 0 };
            std::array<std::uint32_t, kMaxEligibleParts> bodyIds{};
        };

        std::uint64_t _ownerToken{ 0 };
        InstalledTargets _installedTargets{};
        bool _sentDrivesLastUpdate{ false };
        std::uint32_t _registrationRetryCooldownFrames{ 0 };
        bool _registrationWarned{ false };
        std::array<HandSession, 2> _sessions{};
        // Rate-limits the "no learned path yet" hint to once per fresh grip.
        std::array<std::uint64_t, 2> _lastNoPathGripSequence{};
        // Rate-limits the "awaiting trigger unlock" hint to once per grip.
        std::array<std::uint64_t, 2> _lastAwaitingUnlockGripSequence{};
    };
}
