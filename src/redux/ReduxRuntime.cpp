#include "redux/ReduxRuntime.h"

#include "ReduxConfig.h"
#include "ReduxLog.h"
#include "redux/TransformMath.h"
#include "redux/WeaponClipMotionHarvest.h"
#include "redux/WeaponPartEligibility.h"
#include "redux/WeaponPartMotionScrubPolicy.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace redux
{
    namespace
    {
        // One extra frame past the 2-frame drive lease: ROCK restores the
        // node's baseline on the frame the lease expires, and that restore
        // write is not animation evidence either.
        constexpr std::uint32_t kDrivenPartUntrustedFrames = 3;

        [[nodiscard]] std::string_view providerFixedStringView(const char* value, std::size_t capacity)
        {
            if (!value) {
                return {};
            }
            for (std::size_t i = 0; i < capacity; ++i) {
                if (value[i] == '\0') {
                    return std::string_view(value, i);
                }
            }
            return std::string_view(value, capacity);
        }

        [[nodiscard]] bool finiteNiTransform(const RE::NiTransform& transform)
        {
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (!std::isfinite(transform.rotate.entry[row][column])) {
                        return false;
                    }
                }
            }
            return std::isfinite(transform.translate.x) &&
                   std::isfinite(transform.translate.y) &&
                   std::isfinite(transform.translate.z) &&
                   std::isfinite(transform.scale) &&
                   std::abs(transform.scale) > 0.0001f;
        }

        [[nodiscard]] bool nodeContainsNode(RE::NiAVObject* root, RE::NiAVObject* target, int maxDepth)
        {
            if (!root || !target || maxDepth < 0) {
                return false;
            }
            if (root == target) {
                return true;
            }
            auto* node = root->IsNode();
            if (!node) {
                return false;
            }
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t i = 0; i < children.size(); ++i) {
                if (nodeContainsNode(children[i].get(), target, maxDepth - 1)) {
                    return true;
                }
            }
            return false;
        }

        // Animation rig bones carry plain names ('Bolt_Carrier') while the
        // assembled scene tree may decorate instanced nodes with a ':N'
        // suffix ('Bolt_Carrier:0'); a bone matches its node exactly or with
        // that suffix.
        [[nodiscard]] bool nodeNameMatchesBoneName(const RE::NiAVObject* node, std::string_view boneName)
        {
            if (!node || boneName.empty()) {
                return false;
            }
            const char* nodeName = node->name.c_str();
            if (!nodeName) {
                return false;
            }
            const std::string_view nodeView{ nodeName };
            if (nodeView == boneName) {
                return true;
            }
            return nodeView.size() > boneName.size() &&
                   nodeView.compare(0, boneName.size(), boneName) == 0 &&
                   nodeView[boneName.size()] == ':';
        }

        [[nodiscard]] RE::NiAVObject* findWeaponNodeByBoneName(RE::NiAVObject* root, std::string_view boneName, int maxDepth)
        {
            if (!root || boneName.empty() || maxDepth < 0) {
                return nullptr;
            }
            if (nodeNameMatchesBoneName(root, boneName)) {
                return root;
            }
            auto* node = root->IsNode();
            if (!node) {
                return nullptr;
            }
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t i = 0; i < children.size(); ++i) {
                if (auto* found = findWeaponNodeByBoneName(children[i].get(), boneName, maxDepth - 1)) {
                    return found;
                }
            }
            return nullptr;
        }

        /*
         * Player graph-manager access, raw-disassembly verified on the FO4VR
         * binary (2026-07-04); CommonLib headers are deliberately not trusted
         * for any of these:
         *  - TESObjectREFR's IAnimationGraphManagerHolder subobject sits at
         *    +0x48 — the refr graph bootstrap (0x140419030) passes refr+0x48
         *    to every holder helper it calls;
         *  - holder vtable slot 4 (+0x20) is
         *    GetAnimationGraphManagerImpl(out&), writing an add-ref'd
         *    BSAnimationGraphManager* into the caller's pointer slot (engine
         *    helper 0x14080d5e0; graph-swap code 0x14080dfd0 uses slots
         *    +0x20/+0x28/+0x68 as Get/Set/PostChange);
         *  - the reference is released by decrementing the refcount dword at
         *    object+0x8, destroying via vtable slot 0 with argument 1 when it
         *    reaches zero (0x14080d5e0 epilogue; matching add-ref at
         *    0x14080e20e).
         * The returned pointer is accepted only when its vtable equals the
         * module's BSAnimationGraphManager vtable (+0x2E00550 — confirmed
         * in-game by the chain diagnostics); anything else fails closed.
         */
        constexpr std::uintptr_t kRefrGraphHolderInterfaceOffset = 0x48;
        constexpr std::uintptr_t kGetGraphManagerVtableSlotOffset = 0x20;
        constexpr std::uintptr_t kGraphManagerRefCountOffset = 0x8;
        constexpr std::uintptr_t kGraphManagerVtableModuleOffset = 0x2E00550;

        // Vtables and virtual functions must live inside the loaded module
        // image; anything else is a wrong-offset read and fails closed.
        [[nodiscard]] bool pointerInModuleImage(std::uintptr_t value)
        {
            const auto base = REL::Module::get().base();
            return value > base && value - base < 0x800'0000ull;
        }

        using GetGraphManagerFn = bool (*)(void*, void**);
        using DestroyGraphManagerFn = void* (*)(void*, std::uint32_t);

        // Add-ref'd BSAnimationGraphManager reference obtained through the
        // refr's holder interface; releases on scope exit mirroring the
        // engine's own release path. manager() is null (fail closed) when
        // the interface, the call, or the runtime type gate fails.
        class AcquiredGraphManager
        {
        public:
            explicit AcquiredGraphManager(RE::TESObjectREFR* refr)
            {
                if (!refr) {
                    return;
                }
                auto* holderInterface =
                    reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(refr) + kRefrGraphHolderInterfaceOffset);
                const auto vtable = *reinterpret_cast<const std::uintptr_t*>(holderInterface);
                if (!pointerInModuleImage(vtable)) {
                    return;
                }
                const auto getManager = *reinterpret_cast<const std::uintptr_t*>(vtable + kGetGraphManagerVtableSlotOffset);
                if (!pointerInModuleImage(getManager)) {
                    return;
                }
                void* raw = nullptr;
                reinterpret_cast<GetGraphManagerFn>(getManager)(holderInterface, &raw);
                // The reference must be released whether or not the type gate
                // passes below.
                _reference = raw;
                if (!raw) {
                    return;
                }
                const auto managerVtable = *reinterpret_cast<const std::uintptr_t*>(raw);
                if (managerVtable == REL::Module::get().base() + kGraphManagerVtableModuleOffset) {
                    _manager = raw;
                }
            }

            AcquiredGraphManager(const AcquiredGraphManager&) = delete;
            AcquiredGraphManager& operator=(const AcquiredGraphManager&) = delete;

            ~AcquiredGraphManager()
            {
                if (!_reference) {
                    return;
                }
                auto* refCount = reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uintptr_t>(_reference) + kGraphManagerRefCountOffset);
                if (std::atomic_ref<std::uint32_t>{ *refCount }.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(_reference);
                    if (pointerInModuleImage(vtable)) {
                        const auto destroy = *reinterpret_cast<const std::uintptr_t*>(vtable);
                        if (pointerInModuleImage(destroy)) {
                            reinterpret_cast<DestroyGraphManagerFn>(destroy)(_reference, 1);
                        }
                    }
                }
            }

            [[nodiscard]] const void* manager() const { return _manager; }

        private:
            void*       _reference{ nullptr };
            const void* _manager{ nullptr };
        };

        // Collect the names of every node strictly under `root` (the root
        // itself carries whole-weapon motion and is excluded). Non-owning
        // pointers into the nodes' names; valid only within the frame.
        void collectSubtreeNodeNames(
            RE::NiAVObject* object,
            const char** outNames,
            std::uint32_t maxNames,
            std::uint32_t& count,
            int maxDepth)
        {
            if (!object || count >= maxNames || maxDepth < 0) {
                return;
            }
            if (const char* name = object->name.c_str(); name && name[0] != '\0') {
                outNames[count++] = name;
            }
            auto* node = object->IsNode();
            if (!node) {
                return;
            }
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t i = 0; i < children.size() && count < maxNames; ++i) {
                collectSubtreeNodeNames(children[i].get(), outNames, maxNames, count, maxDepth - 1);
            }
        }
    }

    void ReduxRuntime::onFrame(const rock::provider::RockProviderFrameSnapshot& snapshot)
    {
        if (!_active) {
            _active = true;
            RDX_LOG_INFO(Runtime, "ReduxRuntime active on ROCK frame callbacks (frame {})", snapshot.frameIndex);
        }
        ageDrivenPartLeases();

        auto* weaponNode = reinterpret_cast<RE::NiNode*>(snapshot.weaponNode);
        const auto generationKey = snapshot.weaponGenerationKey;
        const auto weaponFormId = snapshot.weaponFormId;

        /*
         * Same call order as the stack had inside ROCK's update: observe the
         * engine-animated poses first, then adopt/drain harvested strokes,
         * then step the graph walk, then run the drive loop off this frame's
         * fresh grip reports. Every step fails closed on a null weapon node
         * or zero generation, so holstered/transition frames only end the
         * drive sessions.
         */
        observeWeaponPartMotion(weaponNode, generationKey, weaponFormId);
        drainWeaponClipHarvest(weaponNode, generationKey, weaponFormId);
        updateWeaponClipHarvestWalk(weaponNode, generationKey, weaponFormId);
        updateWeaponPartDriveSandbox(weaponNode, generationKey, weaponFormId, snapshot);
    }

    void ReduxRuntime::shutdown()
    {
        _sandbox.shutdown();
        _learner.reset();
        _drivePartCache = {};
        _drivenPartLeases = {};
        weapon_clip_motion_harvest::clearPending();
        weapon_clip_motion_harvest::resetWalk();
        weapon_clip_motion_harvest::clearClipActivationTargets();
        _lastClipHarvestWeaponFormId = 0;
        _clipHarvestWalkGenerationKey = 0;
        _clipHarvestWalkAttempts = 0;
        _clipHarvestWalkCompleted = false;
        _clipHarvestWalkGaveUp = false;
        _clipHarvestWalkHolderSeen = false;
        _clipHarvestWalkCandidateLogged = false;
        _clipHarvestRewalkActive = false;
        _clipHarvestRewalkCooldownFrames = 0;
        _active = false;
    }

    void ReduxRuntime::ageDrivenPartLeases()
    {
        for (auto& lease : _drivenPartLeases) {
            if (lease.framesRemaining > 0) {
                --lease.framesRemaining;
                if (lease.framesRemaining == 0) {
                    lease = {};
                }
            }
        }
    }

    bool ReduxRuntime::partRecentlyDriven(const DrivePartCacheEntry& entry) const
    {
        const auto entryName = providerFixedStringView(entry.sourceName.data(), entry.sourceName.size());
        for (const auto& lease : _drivenPartLeases) {
            if (lease.framesRemaining == 0) {
                continue;
            }
            if (lease.bodyId != 0x7FFF'FFFFu && lease.bodyId == entry.bodyId) {
                return true;
            }
            const auto leaseName = providerFixedStringView(lease.sourceName.data(), lease.sourceName.size());
            if (!leaseName.empty() && leaseName == entryName) {
                return true;
            }
        }
        return false;
    }

    void ReduxRuntime::refreshDrivePartCache(RE::NiNode* weaponNode, std::uint64_t generationKey)
    {
        if (_drivePartCache.generationKey == generationKey) {
            return;
        }
        _drivePartCache = {};
        if (!weaponNode || generationKey == 0) {
            return;
        }
        const auto* api = ::rock::provider::RockProviderApi::inst;
        if (!api || !api->copyWeaponEvidenceDetailsV1) {
            return;
        }

        /*
         * One evidence copy per weapon generation, never per frame. The cache
         * key is only committed once a detail for the current generation is
         * seen, so an early call before ROCK publishes the evidence snapshot
         * retries next frame instead of caching emptiness. Unfiltered on
         * purpose: the learner sees and groups EVERY evidence part — bullets
         * riding a mag, parts that only move in a later reload phase — and
         * authored strokes map to every part. Only GRABBING stays restricted,
         * at the grip filter and the provider whitelist.
         */
        std::array<::rock::provider::RockProviderWeaponEvidenceDetailV1, WeaponPartMotionLearner::kMaxActiveRecorders> details{};
        const auto detailCount = api->copyWeaponEvidenceDetailsV1(details.data(), static_cast<std::uint32_t>(details.size()));
        bool sawCurrentGeneration = false;
        for (std::uint32_t i = 0; i < detailCount && i < details.size(); ++i) {
            const auto& detail = details[i];
            if (detail.weaponGenerationKey != generationKey) {
                continue;
            }
            sawCurrentGeneration = true;
            auto* node = reinterpret_cast<RE::NiAVObject*>(detail.sourceRoot);
            const auto sourceName = providerFixedStringView(detail.sourceName, ::rock::provider::ROCK_PROVIDER_MAX_EVIDENCE_NAME);
            if (!node || sourceName.empty() || _drivePartCache.count >= _drivePartCache.entries.size()) {
                continue;
            }
            auto& entry = _drivePartCache.entries[_drivePartCache.count++];
            entry.bodyId = detail.bodyId;
            entry.node = node;
            entry.sourceName = {};
            std::memcpy(
                entry.sourceName.data(),
                sourceName.data(),
                (std::min)(sourceName.size(), entry.sourceName.size() - 1));
        }
        if (sawCurrentGeneration) {
            _drivePartCache.generationKey = generationKey;
        }
    }

    void ReduxRuntime::observeWeaponPartMotion(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId)
    {
        if (!weaponNode || generationKey == 0 || weaponFormId == 0) {
            return;
        }
        refreshDrivePartCache(weaponNode, generationKey);
        if (_drivePartCache.generationKey != generationKey || _drivePartCache.count == 0) {
            return;
        }

        const RE::NiTransform weaponWorldInverse = transform_math::invertTransform(weaponNode->world);
        // Frame-align every recorder before this frame's observations so
        // concurrent recordings can be compared for co-movement grouping.
        _learner.beginObservationFrame();
        for (std::uint32_t i = 0; i < _drivePartCache.count; ++i) {
            const auto& entry = _drivePartCache.entries[i];
            if (!entry.node || !nodeContainsNode(weaponNode, entry.node, 64)) {
                continue;
            }
            // Parts we drove within the lease window arrive untrusted: the
            // pose is our own drive authority (or ROCK's baseline restore),
            // not animation evidence.
            const bool driven = partRecentlyDriven(entry);
            const RE::NiTransform partWeaponLocal = transform_math::composeTransforms(weaponWorldInverse, entry.node->world);
            if (!finiteNiTransform(partWeaponLocal)) {
                continue;
            }
            weapon_part_motion_path::PoseSample pose{};
            pose.translate = weapon_part_motion_path::Vec3{
                partWeaponLocal.translate.x,
                partWeaponLocal.translate.y,
                partWeaponLocal.translate.z,
            };
            float quaternion[4]{};
            transform_math::niRowsToHavokQuaternion(partWeaponLocal.rotate, quaternion);
            pose.rotate = weapon_part_motion_path::Quat{ quaternion[3], quaternion[0], quaternion[1], quaternion[2] };
            _learner.observe(WeaponPartMotionLearner::Observation{
                .weaponFormId = weaponFormId,
                .sourceName = providerFixedStringView(entry.sourceName.data(), entry.sourceName.size()),
                .pose = pose,
                .trusted = !driven,
            });
        }
    }

    void ReduxRuntime::updateWeaponClipHarvestWalk(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId)
    {
        // Bounded retry window (frames with colliders ready) while the weapon
        // graph's bindings finish loading; weapons without a behavior graph
        // (some melee) give up quietly after this.
        static constexpr std::uint32_t kClipHarvestWalkMaxAttempts = 900;
        // Completed walks re-run at this cadence while the weapon stays
        // equipped: clip spline payloads stream in only while a clip plays
        // (in-game confirmed 2026-07-04 — headers resident, payload pointers
        // null), so a reload performed in-hand makes its clips harvestable
        // on the next pass. A pass is a few pointer-gated reads per binding;
        // sampling happens only for clips that became resident.
        static constexpr std::uint32_t kClipHarvestRewalkIntervalFrames = 180;

        if (!weaponNode || generationKey == 0 || weaponFormId == 0) {
            return;
        }
        if (_clipHarvestWalkGenerationKey != generationKey) {
            _clipHarvestWalkGenerationKey = generationKey;
            _clipHarvestWalkAttempts = 0;
            _clipHarvestWalkCompleted = false;
            _clipHarvestWalkGaveUp = false;
            _clipHarvestWalkHolderSeen = false;
            _clipHarvestWalkCandidateLogged = false;
            _clipHarvestRewalkActive = false;
            _clipHarvestRewalkCooldownFrames = 0;
        }
        if (_clipHarvestWalkGaveUp) {
            return;
        }
        if (_clipHarvestWalkCompleted && !_clipHarvestRewalkActive) {
            if (++_clipHarvestRewalkCooldownFrames < kClipHarvestRewalkIntervalFrames) {
                return;
            }
            _clipHarvestRewalkCooldownFrames = 0;
            _clipHarvestRewalkActive = true;
            weapon_clip_motion_harvest::restartWalkPass();
        }

        // The walk starts only after the weapon's colliders finished creation
        // (evidence snapshot committed for the current generation) so the
        // drain always attributes strokes against the final part set.
        refreshDrivePartCache(weaponNode, generationKey);
        if (_drivePartCache.generationKey != generationKey) {
            return;
        }

        // On the give-up frame the holder is still resolved below so the
        // one-shot chain diagnostics can dump the live pointers of the
        // failing hop before the walk closes out. Re-walk passes never give
        // up — the first pass already proved bindings exist.
        const bool givingUp = !_clipHarvestWalkCompleted &&
                              ++_clipHarvestWalkAttempts > kClipHarvestWalkMaxAttempts;

        /*
         * Weapon clips can live on several graph managers, and the copies
         * are not equivalent: in-game chain diagnostics (2026-07-04) showed
         * BOTH biped-slot weapon holders running a shared one-bone dummy rig
         * ('x_bone01') with an empty binding set, while weapon subgraphs are
         * activated on the ACTOR's manager at equip (BSSubGraphActivationUpdate
         * path). Every candidate manager is therefore collected — weapon
         * holders first (a weapon with a real own graph wins), the player's
         * manager last — and the first whose active graph has a non-empty
         * binding set is walked. The clip-track name filter (weapon subtree
         * node names) keeps the actor-graph walk from harvesting body clips.
         * Non-owning pointers, used only within this call; the actor manager
         * reference is scoped by AcquiredGraphManager.
         */
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        std::array<const void*, 5> candidateManagers{};
        std::array<const char*, 5> candidateLabels{};
        std::uint32_t candidateCount = 0;
        bool weaponHolderFound = false;
        const auto firstWeaponSlot = static_cast<std::uint32_t>(std::to_underlying(RE::BIPED_OBJECT::kWeaponHand));
        const auto totalSlots = static_cast<std::uint32_t>(std::to_underlying(RE::BIPED_OBJECT::kTotal));
        for (const bool firstPerson : { true, false }) {
            auto* biped = player->GetBiped(firstPerson).get();
            if (!biped) {
                continue;
            }
            for (std::uint32_t slot = firstWeaponSlot;
                 slot < totalSlots && candidateCount + 1 < candidateManagers.size();
                 ++slot) {
                auto& bipObject = biped->object[slot];
                const auto* itemForm = bipObject.parent.object;
                if (!itemForm || itemForm->GetFormID() != weaponFormId) {
                    continue;
                }
                const void* holder = bipObject.objectGraphManager.get();
                if (!holder) {
                    continue;
                }
                weaponHolderFound = true;
                if (const void* manager = weapon_clip_motion_harvest::managerFromWeaponHolder(holder)) {
                    candidateManagers[candidateCount] = manager;
                    candidateLabels[candidateCount] = firstPerson ? "weapon-holder-1st" : "weapon-holder-3rd";
                    ++candidateCount;
                }
            }
        }
        AcquiredGraphManager actorManager{ player };
        if (actorManager.manager()) {
            candidateManagers[candidateCount] = actorManager.manager();
            candidateLabels[candidateCount] = "actor";
            ++candidateCount;
        }
        if (candidateCount > 0) {
            _clipHarvestWalkHolderSeen = true;
        }

        const void* chosenManager = nullptr;
        const char* chosenLabel = "?";
        for (std::uint32_t i = 0; i < candidateCount; ++i) {
            if (weapon_clip_motion_harvest::probeBindings(candidateManagers[i])) {
                chosenManager = candidateManagers[i];
                chosenLabel = candidateLabels[i];
                break;
            }
        }
        // One-shot chain dump of the candidate the walk locks onto, so the
        // walked skeleton and binding-set contents are visible on success
        // paths too (not only at give-up).
        if (chosenManager && !_clipHarvestWalkCandidateLogged) {
            _clipHarvestWalkCandidateLogged = true;
            RDX_LOG_INFO(Weapon,
                "WeaponClipMotionHarvest: weapon {:08X} walking candidate [{}]",
                weaponFormId,
                chosenLabel);
            weapon_clip_motion_harvest::logResolveDiagnostics(chosenManager, chosenLabel);
        }

        if (!chosenManager) {
            // No candidate exposes bindings yet (graphs still loading, or
            // only dummy-rig copies); retry until the attempt budget runs
            // out, then dump the chain of every candidate.
            if (givingUp) {
                _clipHarvestWalkGaveUp = true;
                RDX_LOG_WARN(Weapon,
                    "WeaponClipMotionHarvest: weapon {:08X} graph bindings never became available (holderSeen={} candidates={} lastStage={}); no authored strokes for this weapon",
                    weaponFormId,
                    _clipHarvestWalkHolderSeen,
                    candidateCount,
                    weapon_clip_motion_harvest::lastResolveStage());
                for (std::uint32_t i = 0; i < candidateCount; ++i) {
                    weapon_clip_motion_harvest::logResolveDiagnostics(candidateManagers[i], candidateLabels[i]);
                }
                if (!weaponHolderFound) {
                    // One-shot slot dump so a weapon whose slot never matches
                    // the equipped form ID becomes diagnosable from the log.
                    for (const bool firstPerson : { true, false }) {
                        auto* biped = player->GetBiped(firstPerson).get();
                        if (!biped) {
                            continue;
                        }
                        for (std::uint32_t slot = firstWeaponSlot; slot < totalSlots; ++slot) {
                            auto& bipObject = biped->object[slot];
                            const auto* itemForm = bipObject.parent.object;
                            if (!itemForm) {
                                continue;
                            }
                            RDX_LOG_WARN(Weapon,
                                "WeaponClipMotionHarvest diagnostics: biped {} slot {} form {:08X} holder={}",
                                firstPerson ? "1st" : "3rd",
                                slot,
                                itemForm->GetFormID(),
                                bipObject.objectGraphManager ? "yes" : "no");
                        }
                    }
                }
            }
            return;
        }

        // Clip tracks are matched against the weapon's scene-node names, so
        // an actor-graph walk only ever harvests this weapon's part clips.
        std::array<const char*, 128> allowedNodeNames{};
        std::uint32_t allowedNodeNameCount = 0;
        auto& weaponChildren = weaponNode->GetRuntimeData().children;
        for (std::uint16_t i = 0; i < weaponChildren.size(); ++i) {
            collectSubtreeNodeNames(
                weaponChildren[i].get(),
                allowedNodeNames.data(),
                static_cast<std::uint32_t>(allowedNodeNames.size()),
                allowedNodeNameCount,
                32);
        }

        // Streamed clips never land in the walked binding sets, so the
        // activation hook harvests them the moment their loaded binding is
        // installed on a clip generator of one of these graphs (a reload
        // performed in-hand teaches the weapon its authored curves).
        weapon_clip_motion_harvest::ensureClipActivationHookInstalled();
        weapon_clip_motion_harvest::setClipActivationTargets(
            candidateManagers.data(),
            candidateCount,
            allowedNodeNames.data(),
            allowedNodeNameCount);

        if (weapon_clip_motion_harvest::stepHarvest(
                chosenManager,
                weaponFormId,
                generationKey,
                allowedNodeNames.data(),
                allowedNodeNameCount) == weapon_clip_motion_harvest::StepResult::Completed) {
            _clipHarvestWalkCompleted = true;
            _clipHarvestRewalkActive = false;
        }
    }

    void ReduxRuntime::drainWeaponClipHarvest(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId)
    {
        /*
         * Empty the queue every frame (bounded): an equip-time hook/walk
         * burst queues far more than one batch, and a full queue makes the
         * harvest drop groups (in-game 2026-07-04: one whole clip's groups —
         * the activated-tier upgrade — lost per equip).
         */
        for (std::uint32_t pass = 0; pass < 8; ++pass) {
            if (!drainWeaponClipHarvestBatch(weaponNode, generationKey, weaponFormId)) {
                break;
            }
        }
    }

    bool ReduxRuntime::drainWeaponClipHarvestBatch(RE::NiNode* weaponNode, std::uint64_t generationKey, std::uint32_t weaponFormId)
    {
        if (!weaponNode || generationKey == 0 || weaponFormId == 0) {
            return false;
        }
        if (weaponFormId != _lastClipHarvestWeaponFormId) {
            // Strokes still queued belong to the previous weapon's graph;
            // shared bone names would misattribute them.
            weapon_clip_motion_harvest::clearPending();
            _lastClipHarvestWeaponFormId = weaponFormId;
            // Once per weapon swap: cumulative harvest counters distinguish
            // graph-never-walked (seen=0) from track-filter rejection
            // (seen>0, harvested=0, nonSpline=0) from compression gaps
            // (nonSpline>0) without any per-binding hot-path logging.
            const auto stats = weapon_clip_motion_harvest::snapshotStats();
            RDX_LOG_INFO(Weapon,
                "WeaponClipMotionHarvest: stats at weapon {:08X} equip: bindingsSeen={} harvested={} noTargets={} groupsQueued={} groupsDropped={} skippedNonSpline={} walksCompleted={} hookFires={} hookActivations={} bail=[anim={} clip={} splineData={} map={} bone={} sampler={}]",
                weaponFormId,
                stats.bindingsSeen,
                stats.bindingsHarvested,
                stats.bindingsNoTargets,
                stats.groupsQueued,
                stats.groupsDropped,
                stats.skippedNonSpline,
                stats.walksCompleted,
                stats.hookFires,
                stats.hookActivations,
                stats.bailAnimationPtr,
                stats.bailClipParams,
                stats.bailSplineData,
                stats.bailTrackMap,
                stats.bailBoneCount,
                stats.bailSampler);
            return false;
        }

        auto& drainedGroups = _clipHarvestDrainGroups;
        const auto drainedCount = weapon_clip_motion_harvest::drainGroups(
            drainedGroups.data(),
            static_cast<std::uint32_t>(drainedGroups.size()));
        if (drainedCount == 0) {
            return false;
        }

        const auto niToPose = [](const RE::NiTransform& transform) {
            weapon_part_motion_path::PoseSample pose{};
            float quaternion[4]{};
            transform_math::niRowsToHavokQuaternion(transform.rotate, quaternion);
            pose.rotate = weapon_part_motion_path::Quat{ quaternion[3], quaternion[0], quaternion[1], quaternion[2] };
            pose.translate = weapon_part_motion_path::Vec3{ transform.translate.x, transform.translate.y, transform.translate.z };
            return pose;
        };
        /*
         * Clip keys are RIG-bone-local under the rig 'Weapon' bone: their
         * rest value differs from the scene node's (in-game A/B 2026-07-04:
         * authored bolt paths started at the rig rest (0, 4.44, 0) instead
         * of the part's weapon-local rest — grabbing teleported the part).
         * Paths are therefore REBASED onto the node's weapon-local rest:
         * translation rest + R·(t_i - t_0), rotation as the track's key
         * delta relative to key 0 conjugated into the scene frame and
         * applied about the bone origin (evidence nodes offset inside the
         * bone orbit it, which is the true motion — the earlier "orbit"
         * bug was this lever under a WRONG basis).
         *
         * R is the fixed rig-Weapon-root → scene-weapon-node-local basis
         * rotation, calibrated 2026-07-04 from learner ground truth: the
         * template bolt track's rig stroke (-8.05, 0, -3.88) is a straight
         * back pull (0,-1,0) on five learned weapons (AK/hunting rifle/
         * P320/10mm/handmade AR), which fixes R up to a spin about the
         * pull axis; requiring the mag track (-6.61, 0, 3.88) to exit
         * DOWNWARD (-Z) pins the spin uniquely (det +1, angle-preservation
         * then forces the mag's 34-degree back-tilt — rock-and-lock; the
         * in-game 2026-07-04 retest confirmed both predictions). The
         * rig-Y ↦ scene-X consequence also matches: the template mag
         * track's 45-degree key rotation about rig Y becomes a rock about
         * the weapon's lateral axis.
         * Result: scene = (rig.y, c*rig.x + s*rig.z, s*rig.x - c*rig.z)
         * with (c, s) = normalized bolt-track direction.
         */
        constexpr float kRigBasisC = 0.900823f;  // 8.05 / |(-8.05, 0, -3.88)|
        constexpr float kRigBasisS = 0.434185f;  // 3.88 / |(-8.05, 0, -3.88)|
        const auto rigDeltaToScene = [](float dx, float dy, float dz) {
            return RE::NiPoint3{
                dy,
                kRigBasisC * dx + kRigBasisS * dz,
                kRigBasisS * dx - kRigBasisC * dz
            };
        };
        /*
         * The same basis as a raw column-vector matrix (v_scene = B·v_rig).
         * Two matrix conventions meet here and MUST NOT be mixed (first
         * rotation attempt mixed them — every authored rotation came out
         * wrong): havokQuaternionToNiRows yields the standard COLUMN-vector
         * matrix of the quaternion (M·v rotates v), while engine node
         * matrices composed by transform_math are the TRANSPOSE of that
         * (composeTransforms/localPointToWorld compute Mᵀ·v). All delta
         * math below stays in column form; the transpose happens exactly
         * once, where a delta composes onto an engine rest matrix.
         */
        RE::NiMatrix3 rigBasis{};
        rigBasis.entry[0][1] = 1.0f;
        rigBasis.entry[1][0] = kRigBasisC;
        rigBasis.entry[1][2] = kRigBasisS;
        rigBasis.entry[2][0] = kRigBasisS;
        rigBasis.entry[2][2] = -kRigBasisC;
        const RE::NiMatrix3 rigBasisTransposed = transform_math::transposeRotation(rigBasis);
        const auto quatToColumnRotate = [](const weapon_part_motion_path::Quat& q) {
            const float quaternion[4]{ q.x, q.y, q.z, q.w };
            return transform_math::havokQuaternionToNiRows<RE::NiMatrix3>(quaternion);
        };
        // Rotation of a track key relative to the track's key 0, mapped into
        // the scene weapon-node frame; column-vector form. The delta must be
        // the PARENT-frame (left) delta key·key0ᵀ — the basis conjugation
        // maps parent-frame rotations — not the body-frame key0ᵀ·key (key 0
        // sits 135° from rest on the template rig, so the wrong frame is
        // wrong by a lot, not subtly).
        const auto sceneRotationDelta = [&](const weapon_part_motion_path::PoseSample& key0,
                                            const weapon_part_motion_path::PoseSample& keyN) {
            const auto rigDelta = transform_math::multiplyStoredRotations(
                quatToColumnRotate(keyN.rotate),
                transform_math::transposeRotation(quatToColumnRotate(key0.rotate)));
            return transform_math::multiplyStoredRotations(
                transform_math::multiplyStoredRotations(rigBasis, rigDelta),
                rigBasisTransposed);
        };
        // Column-form delta applied to an engine-convention rest matrix:
        // true result = delta·restᵀ, stored back as the transpose —
        // rest_s·deltaᵀ.
        const auto applyDeltaToRest = [](const RE::NiMatrix3& restStored, const RE::NiMatrix3& deltaColumn) {
            return transform_math::multiplyStoredRotations(
                restStored,
                transform_math::transposeRotation(deltaColumn));
        };
        /*
         * Rotation is honored only for ACTIVATED-clip strokes — the weapon's
         * own animation (bolt-action rotate-then-pull) — and learned paths.
         * Fallback strokes keep translation only: fallback is another clip's
         * motion, and while its translation direction provably generalizes
         * (basis calibrated against five learned weapons), its rotation
         * provably does not (in-game 2026-07-04 twice: 45° pitch on
         * straight-pull slides, then zero-translation optics/trigger tracks
         * whose 90° diagonal-axis spins became "valid" strokes purely via
         * rotation arc and drove parts around random points).
         */
        const auto convertLeaderPath = [&](const weapon_clip_stroke::AuthoredStrokeGroup& source,
                                           const RE::NiTransform& leaderRestWeaponLocal,
                                           const RE::NiTransform* tail,
                                           weapon_part_motion_path::MotionPath& outPath) {
            const bool applyRotation = source.activatedClip;
            outPath = weapon_part_motion_path::MotionPath{};
            RE::NiTransform anchor = leaderRestWeaponLocal;
            if (tail) {
                anchor = transform_math::composeTransforms(anchor, *tail);
            }
            // The track rotates its bone about the bone origin; an evidence
            // node offset inside the bone (tail) orbits that origin.
            const RE::NiPoint3 lever{
                anchor.translate.x - leaderRestWeaponLocal.translate.x,
                anchor.translate.y - leaderRestWeaponLocal.translate.y,
                anchor.translate.z - leaderRestWeaponLocal.translate.z
            };
            const auto& firstKey = source.leaderPath.keys[0];
            float arc = 0.0f;
            for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
                const auto& clipKey = source.leaderPath.keys[key];
                const auto sceneDelta = rigDeltaToScene(
                    clipKey.translate.x - firstKey.translate.x,
                    clipKey.translate.y - firstKey.translate.y,
                    clipKey.translate.z - firstKey.translate.z);
                RE::NiTransform keyTransform = anchor;
                if (applyRotation) {
                    const auto rotationDelta = sceneRotationDelta(firstKey, clipKey);
                    // Column form: M·v — rotateWorldVectorToLocal computes
                    // exactly that on the raw entries.
                    const auto rotatedLever =
                        transform_math::rotateWorldVectorToLocal<RE::NiMatrix3, RE::NiPoint3>(rotationDelta, lever);
                    keyTransform.translate = leaderRestWeaponLocal.translate + sceneDelta + rotatedLever;
                    keyTransform.rotate = applyDeltaToRest(anchor.rotate, rotationDelta);
                } else {
                    keyTransform.translate += sceneDelta;
                }
                outPath.keys[key] = niToPose(keyTransform);
                if (key > 0) {
                    arc += weapon_part_motion_path::poseDistance(outPath.keys[key], outPath.keys[key - 1]);
                }
            }
            outPath.totalArcLength = arc;
            outPath.valid = arc > 0.0f;
            return outPath.valid;
        };

        const RE::NiTransform weaponWorldInverse = transform_math::invertTransform(weaponNode->world);
        for (std::uint32_t groupIndex = 0; groupIndex < drainedCount; ++groupIndex) {
            const auto& group = drainedGroups[groupIndex];
            const auto leaderName = providerFixedStringView(group.leaderBoneName.data(), group.leaderBoneName.size());
            auto* leaderNode = findWeaponNodeByBoneName(weaponNode, leaderName, 32);
            if (!leaderNode || !leaderNode->parent || !group.leaderPath.valid) {
                // Clip does not belong to this weapon (or the rig bone is not
                // in the assembled tree) — normal for NPC/other-race clips.
                continue;
            }
            /*
             * Authored tier is CLIP PROVENANCE, not bone name (Bruno,
             * 2026-07-04): modded weapons animate template-named bones with
             * their OWN clips, so the bone-name heuristic misjudged real
             * data as generic and let a merely-loaded shared clip drive
             * modded parts. A stroke from a clip the weapon actually
             * ACTIVATED is the weapon's animation; loaded-set walk strokes
             * are fallback only, replaced the moment real data arrives.
             */
            const bool fallbackSource = !group.activatedClip;
            /*
             * Fallback plausibility cap (in-game 2026-07-04): the 23-unit
             * 'WeaponExtra2' carry track from the loaded shared clip mapped
             * onto slide stops and drove them sideways across the weapon.
             * No real reciprocating part travels that far; a fallback
             * stroke past this cap is helper/carry animation, not part
             * motion. Activated clips are the weapon's own data and are
             * not second-guessed.
             */
            constexpr float kMaxFallbackStrokeArcGameUnits = 15.0f;
            if (fallbackSource && group.leaderPath.totalArcLength > kMaxFallbackStrokeArcGameUnits) {
                RDX_LOG_INFO(Weapon,
                    "WeaponClipHarvest: dropped fallback stroke '{}' (arc {:.1f} > {:.1f} cap)",
                    leaderName,
                    group.leaderPath.totalArcLength,
                    kMaxFallbackStrokeArcGameUnits);
                continue;
            }
            const RE::NiTransform leaderRestWeaponLocal =
                transform_math::composeTransforms(weaponWorldInverse, leaderNode->world);

            // Basis evidence: raw rig-frame stroke vs the basis-corrected
            // scene-frame stroke actually stored. sceneDeltaT for bolt/slide
            // tracks must read as a straight -Y back pull, mags as -Z-biased
            // down-and-back; any other shape means the calibration is off
            // for this rig family.
            {
                const auto restPose = niToPose(leaderRestWeaponLocal);
                const auto& key0 = group.leaderPath.keys[0];
                const auto& keyLast = group.leaderPath.keys[weapon_part_motion_path::kResampledKeyCount - 1];
                const auto sceneDelta = rigDeltaToScene(
                    keyLast.translate.x - key0.translate.x,
                    keyLast.translate.y - key0.translate.y,
                    keyLast.translate.z - key0.translate.z);
                // True scene-frame rotation delta of the stroke (column
                // matrix → standard quat extraction).
                float sceneRotQuat[4]{};
                transform_math::niRowsToHavokQuaternion(sceneRotationDelta(key0, keyLast), sceneRotQuat);
                RDX_LOG_INFO(Weapon,
                    "WeaponClipHarvest basis: leader '{}' src={} clip='{}' restT=({:.2f},{:.2f},{:.2f}) restQ=({:.3f},{:.3f},{:.3f},{:.3f}) key0Q=({:.3f},{:.3f},{:.3f},{:.3f}) keyLastQ=({:.3f},{:.3f},{:.3f},{:.3f}) rigDeltaT=({:.2f},{:.2f},{:.2f}) sceneDeltaT=({:.2f},{:.2f},{:.2f}) sceneDeltaQ=(w{:.3f},{:.3f},{:.3f},{:.3f})",
                    leaderName,
                    group.activatedClip ? "activated" : "loaded",
                    group.clipAnimationName.data(),
                    restPose.translate.x,
                    restPose.translate.y,
                    restPose.translate.z,
                    restPose.rotate.w,
                    restPose.rotate.x,
                    restPose.rotate.y,
                    restPose.rotate.z,
                    key0.rotate.w,
                    key0.rotate.x,
                    key0.rotate.y,
                    key0.rotate.z,
                    keyLast.rotate.w,
                    keyLast.rotate.x,
                    keyLast.rotate.y,
                    keyLast.rotate.z,
                    keyLast.translate.x - key0.translate.x,
                    keyLast.translate.y - key0.translate.y,
                    keyLast.translate.z - key0.translate.z,
                    sceneDelta.x,
                    sceneDelta.y,
                    sceneDelta.z,
                    sceneRotQuat[3],
                    sceneRotQuat[0],
                    sceneRotQuat[1],
                    sceneRotQuat[2]);
            }

            // Followers convert once (leader-tail-independent): each follower
            // stroke drives its own node in weapon-root-local space, rebased
            // onto that node's rest pose the same way as the leader.
            weapon_clip_stroke::AuthoredStrokeGroup converted{};
            converted.leaderBoneName = group.leaderBoneName;
            for (std::uint32_t follower = 0; follower < group.followerCount && follower < group.followers.size(); ++follower) {
                const auto followerName = providerFixedStringView(
                    group.followers[follower].boneName.data(),
                    group.followers[follower].boneName.size());
                auto* followerNode = findWeaponNodeByBoneName(weaponNode, followerName, 32);
                if (!followerNode || !followerNode->parent || followerNode == leaderNode) {
                    continue;
                }
                const RE::NiTransform followerRestWeaponLocal =
                    transform_math::composeTransforms(weaponWorldInverse, followerNode->world);
                const auto& followerFirstKey = group.followers[follower].keys[0];
                auto& slot = converted.followers[converted.followerCount];
                slot.boneName = group.followers[follower].boneName;
                for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
                    const auto& clipKey = group.followers[follower].keys[key];
                    const auto sceneDelta = rigDeltaToScene(
                        clipKey.translate.x - followerFirstKey.translate.x,
                        clipKey.translate.y - followerFirstKey.translate.y,
                        clipKey.translate.z - followerFirstKey.translate.z);
                    RE::NiTransform keyTransform = followerRestWeaponLocal;
                    keyTransform.translate += sceneDelta;
                    if (group.activatedClip) {
                        const auto rotationDelta = sceneRotationDelta(followerFirstKey, clipKey);
                        keyTransform.rotate = applyDeltaToRest(followerRestWeaponLocal.rotate, rotationDelta);
                    }
                    slot.keys[key] = niToPose(keyTransform);
                }
                slot.restScale = followerRestWeaponLocal.scale;
                ++converted.followerCount;
            }

            /*
             * The leader stroke is stored once per matching evidence part so a
             * grip on the authored rig bone or on any collider node beneath it
             * scrubs the same authored stroke; tail carries the evidence
             * node's static offset inside the leader bone's frame.
             */
            bool storedForEvidence = false;
            if (_drivePartCache.generationKey == generationKey) {
                for (std::uint32_t entryIndex = 0; entryIndex < _drivePartCache.count; ++entryIndex) {
                    const auto& entry = _drivePartCache.entries[entryIndex];
                    if (!entry.node ||
                        (entry.node != leaderNode && !nodeContainsNode(leaderNode, entry.node, 16))) {
                        continue;
                    }
                    const RE::NiTransform tail = transform_math::composeTransforms(
                        transform_math::invertTransform(leaderNode->world),
                        entry.node->world);
                    const RE::NiTransform* tailPtr = entry.node != leaderNode ? &tail : nullptr;
                    if (!convertLeaderPath(group, leaderRestWeaponLocal, tailPtr, converted.leaderPath)) {
                        continue;
                    }
                    /*
                     * Sibling evidence parts under the same rig bone are one
                     * rigid body — a single track animates the whole subtree
                     * (P320: slide, slide top, rear/front sights all map to
                     * the bolt bone) — so each sibling rides this group as a
                     * follower: the leader's key deltas rebased onto the
                     * sibling node's own rest. Grabbing the slide then
                     * carries its sights before the part is ever learned,
                     * matching what the learner observes.
                     */
                    auto groupForEntry = converted;
                    for (std::uint32_t otherIndex = 0;
                         otherIndex < _drivePartCache.count &&
                         groupForEntry.followerCount < groupForEntry.followers.size();
                         ++otherIndex) {
                        const auto& other = _drivePartCache.entries[otherIndex];
                        if (otherIndex == entryIndex || !other.node || other.node == entry.node ||
                            (other.node != leaderNode &&
                                !nodeContainsNode(leaderNode, other.node, 16))) {
                            continue;
                        }
                        const RE::NiTransform otherRestWeaponLocal =
                            transform_math::composeTransforms(weaponWorldInverse, other.node->world);
                        // Rigid with the leader bone: the sibling orbits the
                        // bone origin under the leader's rotation delta.
                        const RE::NiPoint3 siblingLever{
                            otherRestWeaponLocal.translate.x - leaderRestWeaponLocal.translate.x,
                            otherRestWeaponLocal.translate.y - leaderRestWeaponLocal.translate.y,
                            otherRestWeaponLocal.translate.z - leaderRestWeaponLocal.translate.z
                        };
                        auto& slot = groupForEntry.followers[groupForEntry.followerCount];
                        slot = weapon_clip_stroke::AuthoredFollower{};
                        std::memcpy(
                            slot.boneName.data(),
                            other.sourceName.data(),
                            (std::min)(slot.boneName.size() - 1, other.sourceName.size()));
                        for (std::uint32_t key = 0; key < weapon_part_motion_path::kResampledKeyCount; ++key) {
                            const auto& clipKey = group.leaderPath.keys[key];
                            const auto sceneDelta = rigDeltaToScene(
                                clipKey.translate.x - group.leaderPath.keys[0].translate.x,
                                clipKey.translate.y - group.leaderPath.keys[0].translate.y,
                                clipKey.translate.z - group.leaderPath.keys[0].translate.z);
                            RE::NiTransform keyTransform = otherRestWeaponLocal;
                            if (group.activatedClip) {
                                const auto rotationDelta =
                                    sceneRotationDelta(group.leaderPath.keys[0], clipKey);
                                const auto rotatedLever = transform_math::rotateWorldVectorToLocal<RE::NiMatrix3, RE::NiPoint3>(
                                    rotationDelta, siblingLever);
                                keyTransform.translate =
                                    leaderRestWeaponLocal.translate + sceneDelta + rotatedLever;
                                keyTransform.rotate = applyDeltaToRest(otherRestWeaponLocal.rotate, rotationDelta);
                            } else {
                                keyTransform.translate += sceneDelta;
                            }
                            slot.keys[key] = niToPose(keyTransform);
                        }
                        slot.restScale = otherRestWeaponLocal.scale;
                        ++groupForEntry.followerCount;
                    }
                    _learner.storeAuthoredGroup(
                        weaponFormId,
                        providerFixedStringView(entry.sourceName.data(), entry.sourceName.size()),
                        groupForEntry,
                        fallbackSource);
                    storedForEvidence = true;
                }
            }

            if (!storedForEvidence) {
                // No collider evidence under this bone yet; keep the stroke
                // under the rig-bone name so future parts can find it.
                if (convertLeaderPath(group, leaderRestWeaponLocal, nullptr, converted.leaderPath)) {
                    _learner.storeAuthoredGroup(weaponFormId, leaderName, converted, fallbackSource);
                }
            }
        }
        // A full batch may leave more groups queued; tell the caller to
        // drain again this frame.
        return drainedCount == static_cast<std::uint32_t>(drainedGroups.size());
    }

    void ReduxRuntime::updateWeaponPartDriveSandbox(
        RE::NiNode* weaponNode,
        std::uint64_t generationKey,
        std::uint32_t weaponFormId,
        const rock::provider::RockProviderFrameSnapshot& snapshot)
    {
        WeaponPartDriveSandbox::FrameInput input{};
        input.weaponGenerationKey = generationKey;
        input.weaponFormId = weaponNode && generationKey != 0 ? weaponFormId : 0;
        input.motionPathMode = g_reduxConfig.motionPathMode;

        RE::NiTransform weaponWorldInverse{};
        bool hasWeaponInverse = false;
        if (weaponNode && input.weaponFormId != 0) {
            weaponWorldInverse = transform_math::invertTransform(weaponNode->world);
            hasWeaponInverse = true;
        }

        const auto* api = ::rock::provider::RockProviderApi::inst;
        for (const bool isLeft : { false, true }) {
            auto& handInput = input.hands[isLeft ? 1u : 0u];
            ::rock::provider::RockProviderWeaponPartGripStateV1 report{};
            if (!api || !api->getWeaponPartGripStateV1 ||
                !api->getWeaponPartGripStateV1(
                    isLeft ? ::rock::provider::RockProviderHand::Left : ::rock::provider::RockProviderHand::Right,
                    &report)) {
                continue;
            }
            if (report.active == 0 || report.attachOnly == 0 ||
                !weaponPartDriveEligible(
                    static_cast<::rock::provider::RockProviderWeaponActionRoleV1>(report.actionRole),
                    static_cast<::rock::provider::RockProviderWeaponPartKindV1>(report.partKind)) ||
                report.providerOwnerToken != _sandbox.ownerToken() ||
                report.weaponGenerationKey != generationKey) {
                continue;
            }

            handInput.gripActive = true;
            handInput.gripSequence = report.gripSequence;
            handInput.bodyId = report.bodyId;

            // Names and nodes come from the member cache (stable storage) so
            // the string_views handed to the sandbox outlive this scope.
            RE::NiAVObject* node = nullptr;
            if (_drivePartCache.generationKey == generationKey) {
                for (std::uint32_t i = 0; i < _drivePartCache.count; ++i) {
                    if (_drivePartCache.entries[i].bodyId == report.bodyId) {
                        node = _drivePartCache.entries[i].node;
                        handInput.sourceName = providerFixedStringView(
                            _drivePartCache.entries[i].sourceName.data(),
                            _drivePartCache.entries[i].sourceName.size());
                        break;
                    }
                }
            }
            if (hasWeaponInverse && node && nodeContainsNode(weaponNode, node, 64)) {
                const RE::NiTransform partWeaponLocal = transform_math::composeTransforms(weaponWorldInverse, node->world);
                // Hand anchor: the provider hand frame. Only hand DISPLACEMENT
                // from grip start feeds the scrub, so any hand-rigid point is
                // equivalent to the grab anchor ROCK used internally.
                const auto& handTransform = isLeft ? snapshot.leftHandTransform : snapshot.rightHandTransform;
                const RE::NiPoint3 handWorld{
                    handTransform.translate[0],
                    handTransform.translate[1],
                    handTransform.translate[2],
                };
                const RE::NiPoint3 handWeaponLocal = transform_math::worldPointToLocal(weaponNode->world, handWorld);
                if (finiteNiTransform(partWeaponLocal)) {
                    handInput.partTranslate = weapon_part_motion_path::Vec3{
                        partWeaponLocal.translate.x,
                        partWeaponLocal.translate.y,
                        partWeaponLocal.translate.z,
                    };
                    handInput.partScale = partWeaponLocal.scale;
                    handInput.handTranslate = weapon_part_motion_path::Vec3{ handWeaponLocal.x, handWeaponLocal.y, handWeaponLocal.z };
                    handInput.transformsValid = true;
                }
            }
        }

        std::array<WeaponPartDriveSandbox::SentDrive, WeaponPartDriveSandbox::kMaxSentDrives> sentDrives{};
        const auto sentCount = _sandbox.update(input, _learner, sentDrives.data());
        // Refresh the untrusted-observation leases for everything we drove
        // this frame; the lease outlives the drive by the restore frame.
        for (std::uint32_t i = 0; i < sentCount; ++i) {
            const auto sentName = providerFixedStringView(sentDrives[i].sourceName.data(), sentDrives[i].sourceName.size());
            DrivenPartLease* slot = nullptr;
            for (auto& lease : _drivenPartLeases) {
                const auto leaseName = providerFixedStringView(lease.sourceName.data(), lease.sourceName.size());
                if (lease.framesRemaining > 0 && lease.bodyId == sentDrives[i].bodyId && leaseName == sentName) {
                    slot = &lease;
                    break;
                }
            }
            if (!slot) {
                for (auto& lease : _drivenPartLeases) {
                    if (lease.framesRemaining == 0) {
                        slot = &lease;
                        break;
                    }
                }
            }
            if (!slot) {
                break;
            }
            slot->bodyId = sentDrives[i].bodyId;
            slot->sourceName = sentDrives[i].sourceName;
            slot->framesRemaining = kDrivenPartUntrustedFrames;
        }
    }
}
