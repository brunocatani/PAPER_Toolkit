#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "paper_toolkit/MotionPathMode.h"
#include "paper_toolkit/WeaponPartEligibility.h"

namespace filewatch
{
    template <class T>
    class FileWatch;
}

namespace paper_toolkit
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
     *   Documents\My Games\Fallout4VR\PAPER_Toolkit_Config\PAPER_Toolkit.ini
     * A missing file is created with the compiled defaults on first load so
     * there is always a live-editable file.
     */
    struct PaperToolkitConfig
    {
        bool enabled = true;
        MotionPathMode motionPathMode = MotionPathMode::AuthoredOnly;
        /*
         * Trigger selects the grip type on eligible parts, PER HAND: grab
         * alone is a normal ROCK authority/carry grab; that hand's trigger
         * (before or during the hold — including the free firing hand in
         * part-carry) makes the part attach-only, sticky until released.
         * A hand's trigger does not arm while it owns the firing grip, so
         * firing never flips the other hand's grabs. The per-part provider
         * targets only exist while armed, so ROCK itself resolves grips
         * into the right mode. Off = eligible parts are always attach-only
         * (path guidance on grab).
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
         *    follow the projected leader path alongside the rigid tier;
         *  - stage transitions: a stroke chaining onto the primary's end
         *    (mag-in after mag-out) is kept as a return stage, and the
         *    drive hands over between stages at the path extremes so each
         *    direction keeps its own path and min/max.
         */
        bool coTimedFollowers = true;
        float coTimedMinOverlap = 0.70f;
        float coTimedMaxArcRatio = 1.5f;
        bool stageTransitions = true;
        float stageChainToleranceGameUnits = 2.0f;
        /*
         * Max/min trigger zone as a FRACTION of each part's full travel
         * (the delta-curve height). Percentage-based on purpose: an
         * absolute arc-unit epsilon meant a pistol slide's zone and a bolt
         * pull's zone were different fractions of their strokes, failing
         * short-travel weapons. Drives max-travel events AND stage-
         * transition triggers.
         */
        float travelExtremeTolerance = 0.10f;
        /*
         * Shell-eject test (2026-07-05): reaching max travel on a driven
         * bolt/slide-class part fires the engine's own shell-casing ejection
         * for the equipped weapon (the P-Casing spawn a fired shot uses).
         * One eject per full stroke; weapons without a casing model no-op
         * inside the engine.
         */
        bool shellEjectOnMaxTravel = true;
        /*
         * Diagnostic-only substring filter for verbose live-clip track-name
         * logs. Structured evidence capture remains unfiltered and passive.
         */
        std::string clipTelemetryFilter = "Reload";
        /*
         * Re-record mode: while true, EVERY config (re)load wipes all
         * learner-held motion data (learned strokes AND drained authored
         * strokes; authored preharvest rebuilds on the next equipped-weapon pass),
         * so reloads re-record from scratch under the current grouping
         * settings. With the motion library on, the wipe also deletes the
         * on-disk library files (curated files are kept). Leave true during
         * a re-record session, set false when done. The runtime performs
         * the wipe on the frame thread.
         */
        bool resetMotionData = false;
        /*
         * Motion library (phase 2): one human-editable JSON per weapon under
         * PAPER_Toolkit_Config\MotionLibrary — learning survives restarts, and
         * the files are the fine-tuning surface. Loaded on weapon equip
         * ("disk seeds, live learning wins"), saved by a background writer
         * when learning settles. ReadOnly: load but never write (global
         * curated mode); per-file "curated": true does the same for one file.
         */
        bool motionLibrary = true;
        bool motionLibraryReadOnly = false;
        /*
         * Append-only mapping evidence beside each serving JSON. Captures a
         * complete ROCK/OMOD/node inventory, geometry clouds, every raw
         * learned recording (including rejected/replaced/interrupted data),
         * and raw passive clip tracks/markers. The .capture.jsonl archive
         * is never loaded by gameplay and is not erased by re-record wipes.
         */
        bool richMotionCapture = true;
        /*
         * Full-subtree observation (phase 3): the learner watches EVERY
         * named node under the weapon root, not only collider-evidence
         * parts, so purely visual movers (bullets in a mag, small linkages)
         * are recorded and persisted too. Observation-only parts are never
         * grip-eligible.
         */
        bool fullSubtreeObservation = true;
        /*
         * Standalone Prisma authoring workstation. These controls intentionally
         * live in this Toolkit INI rather than ROCK Reload Prober's config: the
         * two applications have independent ownership and lifecycles.
         */
        bool authoringPanelEnabled = true;
        float authoringPanelScale = 3.0f;
        float authoringPanelPositionX = 7.75f;
        float authoringPanelPositionY = -29.0f;
        float authoringPanelPositionZ = -16.5f;
        float authoringPanelRotationXDegrees = 0.0f;
        float authoringPanelRotationYDegrees = 90.0f;
        float authoringPanelRotationZDegrees = 6.0f;
        bool authoringPanelFlipRotationX = true;
        bool authoringPanelFlipRotationY = true;
        bool authoringPanelFlipRotationZ = false;
        // Moves whenever panel geometry or enablement changes. The authoring
        // runtime clears pointer/preview leases before adopting a new pose.
        std::uint64_t authoringPanelRevision = 1;
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
        PaperToolkitConfig();
        ~PaperToolkitConfig();

    private:
        void parseIni(bool logDiff);
        void writeDefaultIniIfMissing();
        void startFileWatch();

        std::string _iniFilePath;
        std::unique_ptr<filewatch::FileWatch<std::string>> _fileWatch;
        std::atomic<bool> _reloadPending{ false };
        std::atomic<std::int64_t> _lastChangeEventMs{ 0 };
    };

    inline PaperToolkitConfig g_paperToolkitConfig{};
}
