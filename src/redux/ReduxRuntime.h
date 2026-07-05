#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "api/ROCKProviderApi.h"
#include "redux/MotionLibraryStore.h"
#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartDriveSandbox.h"
#include "redux/WeaponPartMotionLearner.h"

namespace RE
{
    class NiAVObject;
    class NiNode;
}

namespace redux
{
    /*
     * Per-frame owner of the weapon-part reload runtime, driven entirely by
     * ROCK's provider frame callback (main thread, end of ROCK's update).
     * This is the glue PhysicsInteraction provided when this stack lived
     * inside ROCK:
     *
     *  - drive-part cache: one snapshot of ALL weapon evidence parts per
     *    weapon generation (unfiltered — the learner observes everything;
     *    grabbing alone is gated by the grip filter and the provider
     *    whitelist installed by the sandbox);
     *  - motion observation: samples every cached part's weapon-root-local
     *    pose each frame and feeds the learner; parts this runtime drove
     *    within the last drive lease arrive untrusted so the learner never
     *    records our own authority as animation evidence;
     *  - clip harvest walk + drain: walks the weapon's animation graph
     *    bindings, keeps the clip-activation hook targeted, and converts
     *    drained rig-space stroke groups into weapon-root-local paths
     *    (basis rotation, rotation-delta conjugation, provenance tiers);
     *  - drive sandbox input: assembles grip/hand/part state from ROCK's
     *    grip-state API for the scrub-and-drive loop.
     *
     * Engine access: snapshot.weaponNode and evidence sourceRoot pointers are
     * non-owning engine pointers valid only while the snapshot's
     * weaponGenerationKey matches — every use is generation-gated and
     * subtree-checked, exactly as ROCK's internal implementation did.
     * Main-thread only; no allocation after construction on the per-frame
     * paths (the evidence copy is a fixed array refreshed once per weapon
     * generation).
     */
    class ReduxRuntime
    {
    public:
        void onFrame(const rock::provider::RockProviderFrameSnapshot& snapshot);

        // Full teardown: unregister the consumer, drop learner/harvest state.
        // Used when the config disables the runtime and on session resets.
        void shutdown();

        // Re-record mode (bResetLearnedPaths): wipe all learner-held motion
        // data so reloads re-record under the current grouping settings.
        // Frame thread only; the revision bump re-resolves eligible parts.
        void wipeLearnedPaths();

    private:
        struct DrivePartCacheEntry
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            RE::NiAVObject* node{ nullptr };
            // Raw V1 classification values from the evidence detail; used
            // against the INI allowlist when resolving attach-only targets.
            std::uint32_t partKind{ 0 };
            std::uint32_t actionRole{ 0 };
            // ROCK record identity: the installed OMOD occupying this part's
            // slot (0 for base/unpaired, and always 0 on a pre-record-
            // identity ROCK). Part of the learner key, so a workbench part
            // swap can never serve a lookalike's motion data.
            std::uint32_t omodFormId{ 0 };
            /*
             * Full-subtree observation (phase 3): named weapon nodes with no
             * collider evidence — bullets riding a mag, small linkages. They
             * feed the learner (and the library) but are never grip-eligible
             * and never install provider targets.
             */
            bool observationOnly{ false };
            std::array<char, WeaponPartMotionLearner::kMaxSourceName> sourceName{};
            /*
             * Rest-pose capture (weapon-local): the pose the part settles at
             * while ungripped and undriven for ~1s is its authored rest
             * (bolts close, slides return to battery). Latest stationary
             * pose wins, so a mid-animation pause that latches a wrong rest
             * is overwritten the next time the part truly idles. Reference
             * for the delta curve anchoring max-travel and stage triggers.
             */
            bool restPoseValid{ false };
            weapon_part_motion_path::PoseSample restPose{};
            bool hasLastObserved{ false };
            weapon_part_motion_path::PoseSample lastObserved{};
            std::uint32_t stationaryFrames{ 0 };
        };

        struct DrivePartCache
        {
            std::uint64_t generationKey{ 0 };
            std::uint32_t count{ 0 };
            std::array<DrivePartCacheEntry, WeaponPartMotionLearner::kMaxActiveRecorders> entries{};
        };

        // Parts driven by our own drive targets within the last lease window;
        // their observations are untrusted (leaders match by bodyId,
        // followers by source name).
        struct DrivenPartLease
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::array<char, WeaponPartDriveSandbox::kMaxSourceName> sourceName{};
            std::uint32_t framesRemaining{ 0 };
        };

        void refreshDrivePartCache(RE::NiNode* weaponNode, std::uint64_t generationKey);
        /*
         * Resolve which concrete parts of the current weapon may be
         * AttachOnly: allowlisted class (INI) AND a motion path exists under
         * the active mode ("must move" — an unmapped part keeps its normal
         * grip). Recomputed only when the cache generation, the learner
         * revision, or the config target policy moved; logs the set and the
         * allowlisted-but-unmapped exclusions whenever it actually changes.
         */
        void refreshEligibleParts(std::uint32_t weaponFormId);
        void observeWeaponPartMotion(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        void updateWeaponClipHarvestWalk(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        void drainWeaponClipHarvest(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        // Returns true when the drained batch filled the buffer (more queued).
        bool drainWeaponClipHarvestBatch(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        void updateWeaponPartDriveSandbox(
            RE::NiNode* weaponNode,
            std::uint64_t generationKey,
            std::uint32_t weaponFormId,
            const rock::provider::RockProviderFrameSnapshot& snapshot);
        [[nodiscard]] bool partRecentlyDriven(const DrivePartCacheEntry& entry) const;
        void ageDrivenPartLeases();

        /*
         * Motion library (phase 2): equip-time import ("disk seeds, live
         * learning wins"), debounced background save when the learner
         * revision settles, flush on weapon switch and shutdown. Curated
         * files and unresolvable identities are never written.
         */
        void updateMotionLibrary(std::uint32_t weaponFormId);
        void loadMotionLibraryForWeapon(std::uint32_t weaponFormId);
        void flushMotionLibrarySave();

        WeaponPartMotionLearner _learner{};
        WeaponPartDriveSandbox _sandbox{};
        DrivePartCache _drivePartCache{};
        // Resolved attach-only set + the state it was computed from.
        std::uint32_t _eligiblePartCount{ 0 };
        std::array<WeaponPartDriveSandbox::EligiblePart, WeaponPartDriveSandbox::kMaxEligibleParts> _eligibleParts{};
        std::uint64_t _eligibleCacheGeneration{ 0 };
        std::uint64_t _eligibleLearnerRevision{ 0 };
        std::uint64_t _eligibleConfigRevision{ 0 };
        bool _eligibleResolvedOnce{ false };
        // Trigger-unlock support probe (once per session): raw wand button
        // reads and the pipboy-suppression query need a current ROCK; if the
        // loaded ROCK predates them, unlock falls back to grab-only with a
        // one-time warning instead of dead-locking every grip.
        bool _rawWandSupportChecked{ false };
        bool _rawWandButtonsAvailable{ false };
        bool _pipboySuppressionAvailable{ false };
        // Last attach-arming state, for transition logging only.
        bool _lastAttachModeArmed{ false };
        // Parts held by a hand last frame (any grip kind): a held part is
        // not at rest, so its rest-pose capture pauses. One-frame lag is
        // absorbed by the stationary-frame requirement.
        std::array<std::uint32_t, 2> _grippedBodyIds{ 0x7FFF'FFFFu, 0x7FFF'FFFFu };
        std::array<DrivenPartLease, WeaponPartDriveSandbox::kMaxSentDrives> _drivenPartLeases{};

        motion_library::MotionLibraryStore _libraryStore{};
        // The loaded file is kept for the curation-text merge on save:
        // stageName/notes live only in the files, and a runtime save must
        // never drop hand edits. Heap-held; replaced per weapon.
        std::unique_ptr<motion_library::WeaponLibrary> _libraryLoaded{};
        std::uint32_t _libraryWeaponFormId{ 0 };
        motion_library::FormRef _libraryWeaponRef{};
        // Learner revision already persisted/imported; differing revision
        // marks the library dirty.
        std::uint64_t _librarySyncedRevision{ 0 };
        std::uint64_t _libraryLastRevision{ 0 };
        std::uint32_t _libraryStableFrames{ 0 };
        // Scratch for the per-frame harvest drain; member storage because one
        // full batch of stroke groups is far too large for the stack.
        std::array<weapon_clip_stroke::AuthoredStrokeGroup, weapon_clip_stroke::kMaxGroupsPerClip> _clipHarvestDrainGroups{};

        bool _active{ false };
        std::uint32_t _lastClipHarvestWeaponFormId{ 0 };
        std::uint64_t _clipHarvestWalkGenerationKey{ 0 };
        std::uint32_t _clipHarvestWalkAttempts{ 0 };
        bool _clipHarvestWalkCompleted{ false };
        bool _clipHarvestWalkGaveUp{ false };
        bool _clipHarvestWalkHolderSeen{ false };
        bool _clipHarvestWalkCandidateLogged{ false };
        bool _clipHarvestRewalkActive{ false };
        std::uint32_t _clipHarvestRewalkCooldownFrames{ 0 };
    };
}
