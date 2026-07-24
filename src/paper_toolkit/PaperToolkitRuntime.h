#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/ROCKProviderApi.h"
#include "paper_toolkit/MotionLibraryStore.h"
#include "paper_toolkit/WeaponAnimationPreharvest.h"
#include "paper_toolkit/WeaponClipMotionHarvest.h"
#include "paper_toolkit/WeaponClipStrokePolicy.h"
#include "paper_toolkit/WeaponPartDriveSandbox.h"
#include "paper_toolkit/WeaponPartMotionLearner.h"

namespace RE
{
    class NiAVObject;
    class NiNode;
}

namespace paper_toolkit
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
     *  - independent authored lane: loads the equipped instance's exact
     *    first-person AnimationFileData off-screen, samples full clips, and
     *    reconstructs Weapon-relative part paths without playback or hooks;
     *  - learned lane: when selected, samples every cached part's live
     *    weapon-root-local pose; our own recent drives arrive untrusted;
     *  - legacy live clip harvest/scrub remains isolated to its explicit
     *    non-authored modes and can never serve AuthoredOnly;
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
    class PaperToolkitRuntime
    {
    public:
        void onFrame(const rock::provider::RockProviderFrameSnapshot& snapshot);

        // Full teardown: unregister the consumer, drop learner/harvest state.
        // Used when the config disables the runtime and on session resets.
        void shutdown();

        // Re-record mode (bResetLearnedPaths): wipe all learner-held motion
        // data; exact authored preharvest restarts for the equipped weapon.
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
            // Rich-capture snapshot-local identities. They never participate
            // in serving lookup; node path distinguishes duplicate names.
            std::uint32_t catalogPartId{ 0 };
            std::int32_t catalogNodeId{ -1 };
            std::array<char, WeaponPartMotionLearner::kMaxCaptureNodePath> nodePath{};
            bool nodePathTruncated{ false };
            /*
             * Full-subtree observation (phase 3): named weapon nodes with no
             * collider evidence — bullets riding a mag, small linkages. They
             * feed the learner (and the library) but are never grip-eligible
             * and never install provider targets.
             */
            bool observationOnly{ false };
            /*
             * Nearest OTHER cache entry that is a scene-graph ANCESTOR of
             * this entry's node (-1 = chain-top). Feeds the sandbox's
             * drive-time chain filter: driving a node and its ancestor with
             * absolute targets stacks displacement on the descendant.
             */
            std::int32_t chainParentIndex{ -1 };
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
            float restScale{ 1.0f };
            // Deterministic once-per-generation fallback captured when ROCK
            // commits the assembled part set. A later stationary rest pose
            // supersedes it; clip completion time never becomes the anchor.
            bool generationAnchorValid{ false };
            weapon_part_motion_path::PoseSample generationAnchorPose{};
            float generationAnchorScale{ 1.0f };
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
        void updateAuthoredAnimationPreharvest(
            RE::NiNode* weaponNode,
            std::uint64_t generationKey,
            std::uint32_t weaponFormId);
        void updateWeaponClipHarvestWalk(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        void drainWeaponClipHarvest(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        // Returns true when the drained batch filled the buffer (more queued).
        bool drainWeaponClipHarvestBatch(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId);
        void adoptWeaponClipHarvestBatch(
            RE::NiNode* weaponNode,
            std::uint64_t generationKey,
            std::uint32_t weaponFormId,
            std::uint32_t groupCount);
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

        // Append-only rich evidence plane (one .capture.jsonl per weapon).
        void updateRichCaptureState(const rock::provider::RockProviderFrameSnapshot& snapshot);
        void captureRichWeaponSnapshot(
            RE::NiNode* weaponNode,
            std::uint64_t generationKey,
            std::uint32_t weaponFormId,
            std::uint64_t rockFrameIndex);
        void advanceRichWeaponGeometry(
            std::uint64_t generationKey,
            std::uint32_t weaponFormId,
            std::uint64_t rockFrameIndex);
        void cancelPendingRichGeometryCapture(const char* reason, std::uint64_t rockFrameIndex);
        void drainRichClipCaptures(
            std::uint64_t generationKey,
            std::uint32_t weaponFormId,
            std::uint64_t rockFrameIndex);
        static void rawCaptureSink(const WeaponPartMotionLearner::RawCaptureView& capture, void* context);
        void captureRawStroke(const WeaponPartMotionLearner::RawCaptureView& capture);
        [[nodiscard]] rich_capture::EventContext makeCaptureContext(
            std::uint32_t weaponFormId,
            std::uint64_t generationKey,
            std::uint64_t rockFrameIndex);
        [[nodiscard]] rich_capture::CaptureSettings captureSettings() const;
        [[nodiscard]] static rich_capture::FormInfo describeForm(std::uint32_t runtimeFormId);
        bool enqueueRichCapture(rich_capture::Event event);

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
        // Once-per-grip proof that ROCK reported a normal grip even though a
        // PAPER scene-source target was already installed for that part.
        std::array<std::uint64_t, 2> _lastUnmatchedEligibleGripSequence{};
        // Clip-scrub session bookkeeping: frames with a live frozen clip
        // but no hand driving it (idle release), and the last session id
        // seen (resets the idle counter on capture turnover).
        std::uint32_t _scrubIdleFrames{ 0 };
        std::uint64_t _scrubLastSessionId{ 0 };
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
        // One packet is ~100 KiB; member storage avoids the main-thread stack.
        std::array<weapon_clip_motion_harvest::RichClipCapturePacket, 8> _richClipDrainPackets{};

        bool _richCaptureActive{ false };
        std::uint32_t _recorderWeaponFormId{ 0 };
        std::uint64_t _recorderGenerationKey{ 0 };
        std::uint32_t _richCaptureWeaponFormId{ 0 };
        std::uint64_t _richCaptureGenerationKey{ 0 };
        std::uint64_t _richSnapshotGenerationKey{ 0 };
        static constexpr std::uint32_t kMaxRichGeometryTargets =
            ::rock::provider::ROCK_PROVIDER_MAX_WEAPON_BODIES;
        static constexpr std::uint32_t kRichGeometryChunkPointCount = 4096;
        struct RichGeometryTarget
        {
            std::uint32_t evidenceId{ 0 };
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            std::uint32_t providerPointCount{ 0 };
            std::uint32_t scheduledPointCount{ 0 };
        };
        struct PendingRichGeometryCapture
        {
            bool active{ false };
            std::uint32_t weaponFormId{ 0 };
            std::uint64_t generationKey{ 0 };
            std::uint64_t snapshotSequence{ 0 };
            std::array<RichGeometryTarget, kMaxRichGeometryTargets> targets{};
            std::uint32_t targetCount{ 0 };
            std::uint32_t targetIndex{ 0 };
            std::uint32_t pointOffset{ 0 };
            std::uint32_t chunkIndex{ 0 };
            std::uint32_t shortCopyAttempts{ 0 };
            bool currentPointsLoaded{ false };
            bool currentSourceComplete{ false };
            // Legacy ROCK V1 can only copy a cloud prefix in one call. Keep
            // that bounded provider buffer and convert only one JSON chunk
            // per later frame, avoiding a second full-cloud copy in PAPER.
            std::vector<::rock::provider::RockProviderPoint3> currentProviderPoints;
        };
        PendingRichGeometryCapture _pendingRichGeometry{};
        std::uint64_t _lastRockFrameIndex{ 0 };
        std::string _richCaptureSessionId;
        std::uint64_t _richCaptureSequence{ 0 };
        struct PendingCaptureGap
        {
            bool used{ false };
            rich_capture::EventContext context{};
            std::uint64_t firstSequence{ 0 };
            std::uint64_t lastSequence{ 0 };
            std::uint32_t count{ 0 };
        };
        std::array<PendingCaptureGap, 16> _pendingCaptureGaps{};

        bool _active{ false };
        // True only while AuthoredOnly owns the independent off-screen
        // preharvest lane. Used to make mode transitions one-shot cleanup,
        // never a per-frame hook/queue operation.
        bool _authoredPreharvestModeActive{ false };
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
