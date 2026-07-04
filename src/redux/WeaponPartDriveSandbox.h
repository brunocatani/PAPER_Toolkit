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
     * provider consumer, installs a NonExclusive AttachOnly whitelist for
     * every drive-eligible part class (so normal support grips stay
     * untouched), and drives a gripped part along its learned/authored
     * motion path with setWeaponPartDriveTargetsV1. Engine access stays in
     * ReduxRuntime: this class receives plain weapon-root-local data per
     * frame and talks only to ROCK's provider API.
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
            // Attach-only grip on a Bolt-classified part owned by this
            // sandbox's whitelist; false ends any session for the hand.
            bool gripActive{ false };
            std::uint64_t gripSequence{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::string_view sourceName{};
            // All positions weapon-root-local, valid only when resolvable.
            bool transformsValid{ false };
            weapon_part_motion_path::Vec3 partTranslate{};
            float partScale{ 1.0f };
            weapon_part_motion_path::Vec3 handTranslate{};
        };

        struct FrameInput
        {
            std::uint32_t weaponFormId{ 0 };
            std::uint64_t weaponGenerationKey{ 0 };
            // INI-selected path source. Pinned into each session at grip
            // start so a hot-reload mode switch never swaps the path under a
            // hand mid-scrub; it applies to the next grip.
            MotionPathMode motionPathMode{ MotionPathMode::Hybrid };
            // Indexed [0]=right, [1]=left to match hand-state conventions.
            std::array<HandInput, 2> hands{};
        };

        // Returns the number of drives sent this update, written to
        // outSentDrives (capacity kMaxSentDrives).
        std::uint32_t update(
            const FrameInput& input,
            const WeaponPartMotionLearner& learner,
            SentDrive* outSentDrives);
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
        void endSession(HandSession& session);

        std::uint64_t _ownerToken{ 0 };
        bool _whitelistInstalled{ false };
        bool _sentDrivesLastUpdate{ false };
        std::uint32_t _registrationRetryCooldownFrames{ 0 };
        bool _registrationWarned{ false };
        std::array<HandSession, 2> _sessions{};
        // Rate-limits the "no learned path yet" hint to once per fresh grip.
        std::array<std::uint64_t, 2> _lastNoPathGripSequence{};
    };
}
