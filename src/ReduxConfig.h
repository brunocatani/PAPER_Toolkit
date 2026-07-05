#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "redux/MotionPathMode.h"
#include "redux/WeaponPartEligibility.h"

namespace filewatch
{
    template <class T>
    class FileWatch;
}

namespace redux
{
    /*
     * INI-backed configuration, hot-reloadable like ROCK's: a FileWatch on
     * the INI sets a pending flag from its watcher thread; the frame thread
     * applies it via processPendingReload() (debounced, so editors that
     * fire several change events per save reload once). All value reads
     * happen on the frame thread only.
     *
     * Location follows ROCK's convention — outside the game's Data tree so
     * MO2's VFS cannot interfere with file watching:
     *   Documents\My Games\Fallout4VR\PAPERRedux_Config\PAPERRedux.ini
     * A missing file is created with the compiled defaults on first load so
     * there is always a live-editable file.
     */
    struct ReduxConfig
    {
        bool enabled = true;
        MotionPathMode motionPathMode = MotionPathMode::Hybrid;
        /*
         * Trigger selects the grip type on eligible parts, PER HAND: grab
         * alone is a normal ROCK authority/carry grab; that hand's trigger
         * (before or during the hold — including the free firing hand in
         * part-carry) makes the part attach-only, sticky until released.
         * A hand's trigger does not arm while it owns the firing grip, so
         * firing never flips the other hand's grabs. The per-part provider
         * targets only exist while armed, so ROCK itself resolves grips
         * into the right mode. Off = eligible parts are always attach-only
         * (scrub on grab, the pre-selection behavior).
         */
        bool requireTriggerUnlock = true;
        // spdlog level: 0=trace 1=debug 2=info 3=warn 4=error 5=critical 6=off.
        int logLevel = 2;
        /*
         * AttachOnly allowlist, one INI boolean per part class
         * (bAttachOnly<Class>, see kAttachOnlyPartKeys). Composed into
         * attachOnlyParts on every (re)load. The masks are only the class
         * half of the gate — the runtime additionally requires a motion
         * path per part ("must move") before whitelisting it with ROCK.
         */
        std::array<bool, std::size(kAttachOnlyPartKeys)> attachOnlyPartEnabled{};
        AttachOnlyAllowList attachOnlyParts = defaultAttachOnlyAllowList();
        /*
         * Learner grouping/staging (mapper-side, hot-reloadable; applies to
         * strokes learned AFTER the change):
         *  - co-timed followers: non-rigid parts temporally contained in
         *    the leader's stroke (P320 barrel tilt during the slide travel)
         *    ride the scrub alongside the rigid tier;
         *  - stage transitions: a stroke chaining onto the primary's end
         *    (mag-in after mag-out) is kept as a return stage, and the
         *    scrub hands over between stages at the path extremes so each
         *    direction keeps its own path and min/max.
         */
        bool coTimedFollowers = true;
        float coTimedMinOverlap = 0.70f;
        float coTimedMaxArcRatio = 1.5f;
        bool stageTransitions = true;
        float stageEndEpsilonArcUnits = 0.35f;
        float stageChainToleranceGameUnits = 2.0f;
        /*
         * Bumped whenever a value that changes the eligible-part set changed
         * (mode or allowlist); the runtime recomputes per-part targets when
         * this moves. Starts at 1 so a zero-initialized consumer always
         * recomputes once.
         */
        std::uint64_t targetPolicyRevision = 1;

        // First load: resolve the path, create the default INI if missing,
        // parse, apply the log level, start the file watch.
        void load();

        // Frame-thread poll; applies a debounced pending file change.
        // Returns true when a reload was applied this call.
        bool processPendingReload();

        // Out-of-line where FileWatch is complete: the unique_ptr member
        // makes the implicitly generated ctor/dtor require the full type.
        ReduxConfig();
        ~ReduxConfig();

    private:
        void parseIni(bool logDiff);
        void writeDefaultIniIfMissing();
        void startFileWatch();

        std::string _iniFilePath;
        std::unique_ptr<filewatch::FileWatch<std::string>> _fileWatch;
        std::atomic<bool> _reloadPending{ false };
        std::atomic<std::int64_t> _lastChangeEventMs{ 0 };
    };

    inline ReduxConfig g_reduxConfig{};
}
