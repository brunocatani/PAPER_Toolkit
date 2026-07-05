#include "ReduxConfig.h"

#include "ReduxLog.h"

#include <SimpleIni.h>
#include <thomasmonkman-filewatch/FileWatch.hpp>

#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace redux
{
    namespace
    {
        constexpr const char* kSection = "PAPERRedux";
        // Editors fire several change events per save; the reload applies
        // once this long after the last event (frame-thread wait, no sleeps).
        constexpr std::int64_t kReloadDebounceMs = 200;

        constexpr const char* kDefaultIniContent =
            "[PAPERRedux]\n"
            "; Master switch for the weapon-part reload runtime (clip harvest, motion\n"
            "; learner, and part-drive scrubbing through ROCK's provider API).\n"
            "; Hot-reloadable: toggling while in game shuts the runtime down/brings it back.\n"
            "bEnabled = true\n"
            "\n"
            "; Which motion-path source may DRIVE a grabbed part. Data collection is\n"
            "; always on in every mode (the learner keeps recording, the harvest keeps\n"
            "; extracting clips), so switching modes applies instantly to NEW grips with\n"
            "; whatever both sources have accumulated.\n"
            ";   hybrid   - learned paths outrank authored clip strokes; authored\n"
            ";              bootstraps parts not taught yet (original behavior)\n"
            ";   authored - only clip-harvested strokes drive parts\n"
            ";   learned  - only runtime-learned paths drive parts\n"
            "sMotionPathMode = hybrid\n"
            "\n"
            "; Log verbosity: 0=trace 1=debug 2=info 3=warn 4=error 5=critical 6=off.\n"
            "; Hot-reloadable; drop to 1 or 0 when collecting harvest/scrub diagnostics.\n"
            "iLogLevel = 2\n"
            "\n"
            "; Trigger selects the grip type on eligible parts, PER HAND: grab alone =\n"
            "; normal authority/carry grab; that hand's trigger (before or during the\n"
            "; hold, part-carry included) = attach-only manipulation, sticky until the\n"
            "; part is released. A hand's trigger does nothing here while it owns the\n"
            "; firing grip, so firing never flips the other hand's grabs.\n"
            "; false = eligible parts are always attach-only (scrub on grab).\n"
            "bRequireTriggerUnlock = true\n"
            "\n"
            "; Learner grouping (mapper side, hot-reloadable; applies to strokes learned\n"
            "; AFTER a change). Co-timed followers: parts that move NON-rigidly but only\n"
            "; during the leader's stroke window ride the scrub (P320 barrel tilting\n"
            "; while the slide travels, a bullet advancing during the bolt pull).\n"
            "; fCoTimedMinOverlap = fraction of the part's total motion that must fall\n"
            "; inside the leader's window (keeps separate reload phases apart);\n"
            "; fCoTimedMaxArcRatio = max follower motion relative to the leader stroke.\n"
            "bCoTimedFollowers = true\n"
            "fCoTimedMinOverlap = 0.70\n"
            "fCoTimedMaxArcRatio = 1.50\n"
            "\n"
            "; Stage transitions: a learned stroke that starts where the primary stroke\n"
            "; ends (mag-in after mag-out) is kept as a RETURN stage, and the scrub\n"
            "; hands over between stages at the physical travel extremes — each\n"
            "; direction keeps its own path and min/max. Chain tolerance = how close\n"
            "; the return stroke's start must be to the primary's end to count chained.\n"
            "bStageTransitions = true\n"
            "fStageChainToleranceGameUnits = 2.0\n"
            "\n"
            "; Max/min trigger zone as a FRACTION of each part's full travel: the part\n"
            "; counts as at-max / at-rest when its displacement from rest is within\n"
            "; this percentage of the extreme. Percentage-based so short pistol slides\n"
            "; and long bolt pulls trigger identically. Drives max-travel events\n"
            "; (shell eject) AND stage-transition triggers. 0.02 - 0.45.\n"
            "fTravelExtremeTolerance = 0.10\n"
            "\n"
            "; Shell-eject test: reaching max travel on a scrubbed bolt/slide-class part\n"
            "; (bolt, slide, charging handle, pump) fires the engine's own shell-casing\n"
            "; ejection for the equipped weapon — the same P-Casing debris spawn used\n"
            "; when firing. One eject per full stroke (re-arms once the part comes\n"
            "; halfway back toward rest); weapons without a casing model do nothing.\n"
            "bShellEjectOnMaxTravel = true\n"
            "\n"
            "; Re-record mode: while true, EVERY save of this INI (and every game\n"
            "; start) wipes all learned motion data so reloads re-record from scratch\n"
            "; under the current grouping settings (drained authored strokes are wiped\n"
            "; too and re-harvest on the next equip / clip playback). With the motion\n"
            "; library on, the wipe also deletes the on-disk library files (files\n"
            "; marked \"curated\": true are kept). Leave true during a re-record\n"
            "; session, set false when done.\n"
            "bResetLearnedPaths = false\n"
            "\n"
            "; Motion library: one human-editable JSON per weapon in\n"
            "; PAPERRedux_Config\\MotionLibrary — mapped reloads survive game restarts\n"
            "; and the files are the fine-tuning surface (stageName/notes fields are\n"
            "; yours; they round-trip untouched). Loaded on weapon equip; disk data\n"
            "; seeds the learner, anything learned live always wins. Saves happen on a\n"
            "; background writer once learning settles. ReadOnly = load but never\n"
            "; write; a per-file \"curated\": true protects a single hand-tuned file\n"
            "; from being overwritten or wiped.\n"
            "bMotionLibrary = true\n"
            "bMotionLibraryReadOnly = false\n"
            "\n"
            "; Full-subtree observation: the mapper watches EVERY named node under\n"
            "; the weapon root, not only parts with colliders — bullets riding a mag,\n"
            "; small linkages — so the full animation gets mapped and persisted.\n"
            "; Observation-only parts are never grabbable.\n"
            "bFullSubtreeObservation = true\n"
            "\n"
            "; AttachOnly allowlist — which part classes MAY become attach-only grips.\n"
            "; Hot-reloadable. The class switch is only half the gate: a part must ALSO\n"
            "; have a motion path (clip-harvested or learned, under the active mode) to\n"
            "; actually be attach-only. A part that does not move keeps its normal grip\n"
            "; no matter what is enabled here — e.g. with bAttachOnlyReceiver=true, a\n"
            "; receiver nif that the animation moves glues to the hand, a static one on\n"
            "; the same weapon stays a regular support grip.\n"
            "bAttachOnlyBolt = true\n"
            "bAttachOnlySlide = true\n"
            "bAttachOnlyChargingHandle = true\n"
            "bAttachOnlyPump = true\n"
            "bAttachOnlyBreakAction = true\n"
            "bAttachOnlyCylinder = true\n"
            "bAttachOnlyLever = true\n"
            "bAttachOnlyLatch = false\n"
            "bAttachOnlyMagazine = true\n"
            "bAttachOnlyMagwell = true\n"
            "bAttachOnlyChamber = true\n"
            "bAttachOnlyShell = true\n"
            "bAttachOnlyRound = true\n"
            "bAttachOnlyLaserCell = true\n"
            "bAttachOnlyCosmeticAmmo = true\n"
            "bAttachOnlyReceiver = false\n"
            "bAttachOnlyBarrel = false\n"
            "bAttachOnlyHandguard = false\n"
            "bAttachOnlyForegrip = false\n"
            "bAttachOnlyStock = false\n"
            "bAttachOnlyGrip = false\n"
            "bAttachOnlySight = false\n"
            "bAttachOnlyAccessory = false\n"
            "bAttachOnlyOther = false\n";

        std::string resolveIniPath()
        {
            char documents[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, documents))) {
                return std::string(documents) + R"(\My Games\Fallout4VR\PAPERRedux_Config\PAPERRedux.ini)";
            }

            RDX_LOG_WARN(Config, "SHGetFolderPath failed — using fallback PAPERRedux.ini path");
            return R"(Data\F4SE\Plugins\PAPERRedux.ini)";
        }

        [[nodiscard]] std::int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    ReduxConfig::ReduxConfig()
    {
        for (std::size_t i = 0; i < std::size(kAttachOnlyPartKeys); ++i) {
            attachOnlyPartEnabled[i] = kAttachOnlyPartKeys[i].defaultOn;
        }
    }

    ReduxConfig::~ReduxConfig() = default;

    void ReduxConfig::load()
    {
        _iniFilePath = resolveIniPath();
        writeDefaultIniIfMissing();
        parseIni(false);
        RDX_LOG_INFO(Config,
            "Config loaded from '{}': enabled={} motionPathMode={} logLevel={} (hot reload active)",
            _iniFilePath,
            enabled,
            motionPathModeName(motionPathMode),
            logLevel);
        startFileWatch();
    }

    bool ReduxConfig::processPendingReload()
    {
        if (!_reloadPending.load(std::memory_order_acquire)) {
            return false;
        }
        // Debounce on the frame thread: wait out the editor's event burst.
        if (nowMs() - _lastChangeEventMs.load(std::memory_order_acquire) < kReloadDebounceMs) {
            return false;
        }
        _reloadPending.store(false, std::memory_order_release);

        RDX_LOG_INFO(Config, "PAPERRedux.ini change detected, reloading on frame thread...");
        parseIni(true);
        return true;
    }

    void ReduxConfig::parseIni(bool logDiff)
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        const auto rc = ini.LoadFile(_iniFilePath.c_str());
        if (rc < 0) {
            RDX_LOG_WARN(Config, "Failed to read '{}' (rc={}); keeping current values (enabled={} mode={} logLevel={})",
                _iniFilePath,
                static_cast<int>(rc),
                enabled,
                motionPathModeName(motionPathMode),
                logLevel);
            return;
        }

        const bool previousEnabled = enabled;
        const auto previousMode = motionPathMode;
        const int previousLogLevel = logLevel;
        const auto previousAllowList = attachOnlyParts;
        const auto previousPartEnabled = attachOnlyPartEnabled;
        const bool previousCoTimed = coTimedFollowers;
        const float previousCoTimedOverlap = coTimedMinOverlap;
        const float previousCoTimedRatio = coTimedMaxArcRatio;
        const bool previousStageTransitions = stageTransitions;
        const float previousExtremeTolerance = travelExtremeTolerance;
        const float previousChainTolerance = stageChainToleranceGameUnits;
        const bool previousShellEject = shellEjectOnMaxTravel;
        const bool previousResetLearned = resetLearnedPaths;
        const bool previousMotionLibrary = motionLibrary;
        const bool previousLibraryReadOnly = motionLibraryReadOnly;
        const bool previousFullSubtree = fullSubtreeObservation;

        const bool previousRequireTriggerUnlock = requireTriggerUnlock;
        enabled = ini.GetBoolValue(kSection, "bEnabled", enabled);
        requireTriggerUnlock = ini.GetBoolValue(kSection, "bRequireTriggerUnlock", requireTriggerUnlock);

        const char* modeText = ini.GetValue(kSection, "sMotionPathMode", motionPathModeName(motionPathMode));
        MotionPathMode parsedMode = motionPathMode;
        if (modeText && parseMotionPathMode(modeText, parsedMode)) {
            motionPathMode = parsedMode;
        } else {
            RDX_LOG_WARN(Config, "Invalid sMotionPathMode='{}' — keeping '{}' (valid: hybrid, authored, learned)",
                modeText ? modeText : "",
                motionPathModeName(motionPathMode));
        }

        const int parsedLogLevel = static_cast<int>(ini.GetLongValue(kSection, "iLogLevel", logLevel));
        if (parsedLogLevel >= 0 && parsedLogLevel <= 6) {
            logLevel = parsedLogLevel;
        } else {
            RDX_LOG_WARN(Config, "Invalid iLogLevel={} — keeping {}", parsedLogLevel, logLevel);
        }
        logger::setLogLevelAndPattern(logLevel, "");

        // Learner grouping/staging tuning; clamped to sane ranges so a typo
        // degrades into a bound, never into NaN-shaped grouping.
        const auto readClampedFloat = [&](const char* key, float current, float minValue, float maxValue) {
            const auto value = static_cast<float>(ini.GetDoubleValue(kSection, key, current));
            if (!std::isfinite(value)) {
                RDX_LOG_WARN(Config, "Invalid {}={} — keeping {:.2f}", key, value, current);
                return current;
            }
            return std::clamp(value, minValue, maxValue);
        };
        coTimedFollowers = ini.GetBoolValue(kSection, "bCoTimedFollowers", coTimedFollowers);
        coTimedMinOverlap = readClampedFloat("fCoTimedMinOverlap", coTimedMinOverlap, 0.05f, 1.0f);
        coTimedMaxArcRatio = readClampedFloat("fCoTimedMaxArcRatio", coTimedMaxArcRatio, 0.1f, 10.0f);
        stageTransitions = ini.GetBoolValue(kSection, "bStageTransitions", stageTransitions);
        stageChainToleranceGameUnits = readClampedFloat("fStageChainToleranceGameUnits", stageChainToleranceGameUnits, 0.25f, 10.0f);
        // Cap keeps the max/rest zones clear of the 50%-of-travel re-arm point.
        travelExtremeTolerance = readClampedFloat("fTravelExtremeTolerance", travelExtremeTolerance, 0.02f, 0.45f);
        shellEjectOnMaxTravel = ini.GetBoolValue(kSection, "bShellEjectOnMaxTravel", shellEjectOnMaxTravel);
        resetLearnedPaths = ini.GetBoolValue(kSection, "bResetLearnedPaths", resetLearnedPaths);
        motionLibrary = ini.GetBoolValue(kSection, "bMotionLibrary", motionLibrary);
        motionLibraryReadOnly = ini.GetBoolValue(kSection, "bMotionLibraryReadOnly", motionLibraryReadOnly);
        fullSubtreeObservation = ini.GetBoolValue(kSection, "bFullSubtreeObservation", fullSubtreeObservation);

        // AttachOnly allowlist booleans, composed into the class masks.
        for (std::size_t i = 0; i < std::size(kAttachOnlyPartKeys); ++i) {
            attachOnlyPartEnabled[i] =
                ini.GetBoolValue(kSection, kAttachOnlyPartKeys[i].iniKey, attachOnlyPartEnabled[i]);
        }
        attachOnlyParts = AttachOnlyAllowList{};
        for (std::size_t i = 0; i < std::size(kAttachOnlyPartKeys); ++i) {
            applyAttachOnlyPartKey(kAttachOnlyPartKeys[i], attachOnlyPartEnabled[i], attachOnlyParts);
        }

        // The eligible-part set depends on the mode (which source must hold
        // a path) and the allowlist; a change re-resolves per-part targets.
        const bool targetPolicyChanged = motionPathMode != previousMode || !(attachOnlyParts == previousAllowList);
        if (targetPolicyChanged) {
            ++targetPolicyRevision;
        }

        if (logDiff) {
            if (enabled != previousEnabled) {
                RDX_LOG_INFO(Config, "bEnabled: {} -> {}", previousEnabled, enabled);
            }
            if (motionPathMode != previousMode) {
                RDX_LOG_INFO(Config,
                    "sMotionPathMode: {} -> {} (applies to NEW grips; active scrub sessions keep their source)",
                    motionPathModeName(previousMode),
                    motionPathModeName(motionPathMode));
            }
            if (logLevel != previousLogLevel) {
                RDX_LOG_INFO(Config, "iLogLevel: {} -> {}", previousLogLevel, logLevel);
            }
            if (requireTriggerUnlock != previousRequireTriggerUnlock) {
                RDX_LOG_INFO(Config,
                    "bRequireTriggerUnlock: {} -> {} (applies to new grips)",
                    previousRequireTriggerUnlock,
                    requireTriggerUnlock);
            }
            if (shellEjectOnMaxTravel != previousShellEject) {
                RDX_LOG_INFO(Config,
                    "bShellEjectOnMaxTravel: {} -> {}",
                    previousShellEject,
                    shellEjectOnMaxTravel);
            }
            if (resetLearnedPaths != previousResetLearned) {
                RDX_LOG_INFO(Config,
                    "bResetLearnedPaths: {} -> {} (while true, every reload wipes learned motion data)",
                    previousResetLearned,
                    resetLearnedPaths);
            }
            if (motionLibrary != previousMotionLibrary || motionLibraryReadOnly != previousLibraryReadOnly) {
                RDX_LOG_INFO(Config,
                    "Motion library: enabled={} readOnly={} (loads apply on the next weapon equip)",
                    motionLibrary,
                    motionLibraryReadOnly);
            }
            if (fullSubtreeObservation != previousFullSubtree) {
                RDX_LOG_INFO(Config,
                    "bFullSubtreeObservation: {} -> {} (applies on the next weapon generation)",
                    previousFullSubtree,
                    fullSubtreeObservation);
            }
            const bool groupingChanged = coTimedFollowers != previousCoTimed || coTimedMinOverlap != previousCoTimedOverlap ||
                coTimedMaxArcRatio != previousCoTimedRatio || stageTransitions != previousStageTransitions ||
                travelExtremeTolerance != previousExtremeTolerance || stageChainToleranceGameUnits != previousChainTolerance;
            if (groupingChanged) {
                RDX_LOG_INFO(Config,
                    "Grouping tuning: coTimed={} overlap={:.2f} arcRatio={:.2f} stages={} extremeTol={:.2f} chainTol={:.2f} (applies to strokes learned from now on; extremeTol applies immediately)",
                    coTimedFollowers,
                    coTimedMinOverlap,
                    coTimedMaxArcRatio,
                    stageTransitions,
                    travelExtremeTolerance,
                    stageChainToleranceGameUnits);
            }
            bool allowListChangedKeys = false;
            for (std::size_t i = 0; i < std::size(kAttachOnlyPartKeys); ++i) {
                if (attachOnlyPartEnabled[i] != previousPartEnabled[i]) {
                    allowListChangedKeys = true;
                    RDX_LOG_INFO(Config,
                        "{}: {} -> {} (mapped moving parts of this class {} attach-only on next grip)",
                        kAttachOnlyPartKeys[i].iniKey,
                        previousPartEnabled[i],
                        attachOnlyPartEnabled[i],
                        attachOnlyPartEnabled[i] ? "become" : "stop being");
                }
            }
            if (enabled == previousEnabled && motionPathMode == previousMode && logLevel == previousLogLevel &&
                requireTriggerUnlock == previousRequireTriggerUnlock && !allowListChangedKeys && !groupingChanged &&
                shellEjectOnMaxTravel == previousShellEject && resetLearnedPaths == previousResetLearned &&
                motionLibrary == previousMotionLibrary && motionLibraryReadOnly == previousLibraryReadOnly &&
                fullSubtreeObservation == previousFullSubtree) {
                RDX_LOG_INFO(Config, "Reload applied, no value changes (enabled={} mode={} logLevel={})",
                    enabled,
                    motionPathModeName(motionPathMode),
                    logLevel);
            }
        }
    }

    void ReduxConfig::writeDefaultIniIfMissing()
    {
        std::error_code ec;
        if (std::filesystem::exists(_iniFilePath, ec)) {
            return;
        }
        const auto directory = std::filesystem::path(_iniFilePath).parent_path();
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            RDX_LOG_WARN(Config, "Could not create config directory '{}': {}", directory.string(), ec.message());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadData(kDefaultIniContent) != SI_OK || ini.SaveFile(_iniFilePath.c_str(), false) < 0) {
            RDX_LOG_WARN(Config, "Could not write default INI to '{}'; running on compiled defaults", _iniFilePath);
            return;
        }
        RDX_LOG_INFO(Config, "Created default INI at '{}'", _iniFilePath);
    }

    void ReduxConfig::startFileWatch()
    {
        if (_fileWatch || _iniFilePath.empty()) {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(_iniFilePath, ec)) {
            RDX_LOG_WARN(Config, "No INI on disk — hot reload disabled for this session");
            return;
        }
        try {
            _fileWatch = std::make_unique<filewatch::FileWatch<std::string>>(
                _iniFilePath,
                [this](const std::string&, const filewatch::Event changeType) {
                    if (changeType != filewatch::Event::modified &&
                        changeType != filewatch::Event::added &&
                        changeType != filewatch::Event::renamed_new) {
                        return;
                    }
                    // Watcher thread: record the event and raise the flag;
                    // the frame thread debounces and reloads.
                    _lastChangeEventMs.store(nowMs(), std::memory_order_release);
                    _reloadPending.store(true, std::memory_order_release);
                });
            RDX_LOG_DEBUG(Config, "File watch started on '{}'", _iniFilePath);
        } catch (const std::exception& e) {
            RDX_LOG_WARN(Config, "File watch failed to start ('{}'); hot reload disabled: {}", _iniFilePath, e.what());
        }
    }
}
