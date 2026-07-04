#pragma once

#include <array>
#include <cstdint>

#include "api/ROCKProviderApi.h"
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

    private:
        struct DrivePartCacheEntry
        {
            std::uint32_t bodyId{ 0x7FFF'FFFFu };
            RE::NiAVObject* node{ nullptr };
            std::array<char, WeaponPartMotionLearner::kMaxSourceName> sourceName{};
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

        WeaponPartMotionLearner _learner{};
        WeaponPartDriveSandbox _sandbox{};
        DrivePartCache _drivePartCache{};
        std::array<DrivenPartLease, WeaponPartDriveSandbox::kMaxSentDrives> _drivenPartLeases{};
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
