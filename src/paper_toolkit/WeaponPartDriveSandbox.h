#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "paper_toolkit/MotionPathMode.h"
#include "paper_toolkit/WeaponClipStrokePolicy.h"
#include "paper_toolkit/WeaponPartMotionPathPolicy.h"

namespace paper_toolkit
{
    class WeaponPartMotionLearner;

    /*
     * The drive loop of the reload runtime: registers PAPER_Toolkit as a ROCK
     * provider consumer, installs a NonExclusive AttachOnly target set of
     * PER-PART targets (only the parts the runtime resolved as allowlisted
     * AND moving — so unmapped parts and every other grip surface keep
     * their normal behavior), and drives a gripped part along its
     * learned/authored motion path with setWeaponPartDriveTargetsV1.
     * Engine access stays in PaperToolkitRuntime: this class receives plain
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
            // Installed OMOD occupying the gripped part's slot (ROCK record
            // identity, 0 for base/unpaired) — part of the learner key.
            std::uint32_t omodFormId{ 0 };
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
        // generation-pinned scene-source AttachOnly provider targets.
        struct EligiblePart
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            // Stable scene-source identity for this weapon generation. ROCK
            // can publish multiple collision bodies for one driven node;
            // targeting therefore matches this pointer, while the body
            // captured by the grip remains the drive matcher.
            std::uintptr_t sourceRoot{ 0 };
            std::array<char, kMaxSourceName> sourceName{};
        };
        // Matches the runtime's drive-part cache capacity.
        static constexpr std::size_t kMaxEligibleParts = 48;

        /*
         * Scene-graph chain table (Bruno's 2x bug, 2026-07-05): one entry
         * per cached weapon part, naming its nearest OTHER cached part that
         * is a scene-graph ANCESTOR (empty = chain-top). Full-subtree
         * observation records hierarchy nodes ('parent', attach points) as
         * rigid followers of the parts they carry; DRIVING a node and its
         * ancestor with absolute targets stacks the displacement on the
         * descendant (the gripped part traveled 2x). The drive loop uses
         * this table to drive at most one node per parent chain.
         */
        struct PartChainLink
        {
            std::array<char, kMaxSourceName> name{};
            std::array<char, kMaxSourceName> parentName{};
        };
        // Must cover the complete observation cache: a follower beyond the
        // grip-eligible prefix can still share an ancestor with a driven
        // leader and needs the same double-transform protection.
        static constexpr std::size_t kMaxChainLinks = 128;

        struct FrameInput
        {
            std::uint32_t weaponFormId{ 0 };
            std::uint64_t weaponGenerationKey{ 0 };
            // INI-selected path source. Pinned into each session at grip
            // start so a hot-reload mode switch never swaps the path under a
            // hand mid-scrub; it applies to the next grip.
            MotionPathMode motionPathMode{ MotionPathMode::AuthoredOnly };
            /*
             * Stage handoff at the physical travel extremes (delta-curve
             * max/min): reaching either extreme of the active stage hands
             * the session over to the chained stage (mag-out end -> mag-in
             * path, and back), each stage with its own min/max.
             */
            bool stageTransitionsEnabled{ true };
            /*
             * Max/min trigger zone as a FRACTION of the part's full travel
             * (the delta-curve height): the scrub counts as at-max / at-rest
             * when its displacement from rest is within this fraction of the
             * extreme. Percentage-based so a short pistol slide and a long
             * bolt pull trigger identically (absolute arc units did not
             * scale — Bruno, 2026-07-05).
             */
            float travelExtremeToleranceFraction{ 0.10f };
            /*
             * Clip-scrub session feed (mode == ClipScrub): the harvest
             * module's captured-and-frozen reload clip, when one is live.
             * `clipScrubFraction` is the ENGINE-observed time fraction —
             * the pursuit controller's feedback signal. No session means
             * scrub grips glue but cannot drive (hint logged once per grip).
             */
            bool clipScrubSessionActive{ false };
            std::uint64_t clipScrubSessionId{ 0 };
            float clipScrubFraction{ 0.0f };
            // Per-part attach-only whitelist for the current weapon
            // generation; targets reinstall only when this set changes.
            std::uint32_t eligiblePartCount{ 0 };
            std::array<EligiblePart, kMaxEligibleParts> eligibleParts{};
            // Scene-graph chain relations for the drive-time chain filter.
            std::uint32_t chainLinkCount{ 0 };
            std::array<PartChainLink, kMaxChainLinks> chainLinks{};
            // Indexed [0]=right, [1]=left to match hand-state conventions.
            std::array<HandInput, 2> hands{};
        };

        // Returns the number of drives sent this update, written to
        // outSentDrives (capacity kMaxSentDrives). Max-travel events land in
        // outMaxTravelEvents (capacity kMaxMaxTravelEvents) when provided.
        // In ClipScrub mode a gripped session emits a desired clip-time
        // fraction instead of drives: written to outClipScrubFraction with
        // outClipScrubFractionValid set (one hand owns time per frame).
        std::uint32_t update(
            const FrameInput& input,
            const WeaponPartMotionLearner& learner,
            SentDrive* outSentDrives,
            MaxTravelEvent* outMaxTravelEvents = nullptr,
            std::uint32_t* outMaxTravelEventCount = nullptr,
            float* outClipScrubFraction = nullptr,
            bool* outClipScrubFractionValid = nullptr);
        void shutdown();

        [[nodiscard]] std::uint64_t ownerToken() const { return _ownerToken; }
        [[nodiscard]] bool hasInstalledTarget(
            std::uint64_t weaponGenerationKey,
            std::uintptr_t sourceRoot) const
        {
            if (!_installedTargets.any ||
                _installedTargets.weaponGenerationKey != weaponGenerationKey ||
                sourceRoot == 0) {
                return false;
            }
            for (std::uint32_t i = 0; i < _installedTargets.count; ++i) {
                if (_installedTargets.sourceRoots[i] == sourceRoot) {
                    return true;
                }
            }
            return false;
        }

    private:
        struct HandSession
        {
            bool active{ false };
            std::uint64_t gripSequence{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t weaponFormId{ 0 };
            // Learner-key OMOD identity pinned at grip start (see HandInput).
            std::uint32_t omodFormId{ 0 };
            // Path source pinned at grip start (see FrameInput).
            MotionPathMode mode{ MotionPathMode::AuthoredOnly };
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
            /*
             * Clip-scrub session (mode == ClipScrub): the hand drives the
             * captured clip's TIME through a pursuit controller — no path
             * lookup, no drives. The controller learns the part's local
             * motion direction from what the engine-posed part actually did
             * per unit fraction (the engine is the curve oracle), then
             * steers the fraction so the part chases the hand's displaced
             * target. Degenerate stretches (part not responding — stage
             * dwells, bootstrap) fall back to a slow forward crawl gated on
             * real hand pull, which carries the session across windows
             * where the gripped part is authored to rest.
             */
            bool clipScrub{ false };
            std::uint64_t clipScrubSessionId{ 0 };
            weapon_part_motion_path::Vec3 scrubPartStartTranslate{};
            weapon_part_motion_path::Vec3 scrubPrevPartTranslate{};
            float scrubPrevFraction{ 0.0f };
            bool scrubDirectionValid{ false };
            weapon_part_motion_path::Vec3 scrubDirection{};
            // Game units of part travel per unit fraction (local slope).
            float scrubSlope{ 0.0f };
            std::uint32_t scrubLastLogDecile{ 0 };
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
            std::array<std::uintptr_t, kMaxEligibleParts> sourceRoots{};
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
