#include "redux/WeaponAnimationPreharvest.h"

#include "ReduxLog.h"
#include "redux/NativeMemory.h"
#include "redux/TransformMath.h"
#include "redux/WeaponAnimationPreharvestPolicy.h"

#include "RE/Bethesda/Actor.h"
#include "RE/Bethesda/BSAnimationGraph.h"
#include "RE/Bethesda/BSFixedString.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/PowerArmor.h"
#include "RE/Bethesda/TESBoundObjects.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace redux::weapon_animation_preharvest
{
    namespace
    {
        /*
         * These FO4VR 1.2.72 entries and layouts are the production ROCK
         * NativeIdleGripPreharvest boundary. PAPER keeps the identical live-
         * byte gates and uses the same ownership order; the only extension is
         * sampling every exact AnimationFileData clip over its full duration.
         */
        constexpr std::uintptr_t kSimpleAnimationGraphManagerHolderCtor = 0x0811F10;
        constexpr std::uintptr_t kSimpleAnimationGraphManagerHolderDtor = 0x0811F50;
        constexpr std::uintptr_t kCreateBackgroundSimpleManager = 0x0811FE0;
        constexpr std::uintptr_t kIsAnimationLoadingComplete = 0x08122C0;
        constexpr std::uintptr_t kRequestAnimationSubGraph = 0x10162B0;
        constexpr std::uintptr_t kIsAnimationSubGraphLoaded = 0x07F4320;
        constexpr std::uintptr_t kReleaseAnimationSubGraph = 0x07F43C0;
        constexpr std::uintptr_t kAddItemToTargetKeywords = 0x0EDA2F0;
        constexpr std::uintptr_t kGetAnimationFilesForSubgraph = 0x1769140;
        constexpr std::uintptr_t kLoadAnimationResource = 0x1728BA0;
        constexpr std::uintptr_t kMoveAnimationResourceHandle = 0x172AB40;
        constexpr std::uintptr_t kIsHkxDerivativeDbData = 0x152C0D0;
        constexpr std::uintptr_t kRetrieveBindingFromContainer = 0x17865C0;
        constexpr std::uintptr_t kFindBoneWithName = 0x190A580;
        constexpr std::uintptr_t kAnimationFileLookupSingleton = 0x5B64318;

        constexpr std::size_t kSimpleAnimationGraphManagerHolderSize = 0x18;
        static_assert(kSimpleAnimationGraphManagerHolderSize == sizeof(RE::SimpleAnimationGraphManagerHolder));
        constexpr std::uint32_t kSubgraphOutputInlineCapacity = 2;
        constexpr std::size_t kSmallArrayInlineStorageOffset = 0x8;
        static_assert(sizeof(RE::BSTSmallArray<RE::SubgraphHandle, kSubgraphOutputInlineCapacity>) == 0x20);
        static_assert(sizeof(RE::BSTSmallArray<RE::SubgraphIdentifier, kSubgraphOutputInlineCapacity>) == 0x20);

        constexpr std::ptrdiff_t kGraphSkeletonOwnerOffset = 0x240;
        constexpr std::ptrdiff_t kSkeletonFromOwnerOffset = 0x20;
        constexpr std::ptrdiff_t kSkeletonParentIndicesOffset = 0x18;
        constexpr std::ptrdiff_t kSkeletonParentCountOffset = 0x20;
        constexpr std::ptrdiff_t kSkeletonBonesOffset = 0x28;
        constexpr std::ptrdiff_t kSkeletonBoneCountOffset = 0x30;
        constexpr std::ptrdiff_t kSkeletonReferencePoseOffset = 0x38;
        constexpr std::ptrdiff_t kSkeletonReferencePoseCountOffset = 0x40;
        constexpr std::ptrdiff_t kSkeletonPartitionsOffset = 0x78;
        constexpr std::ptrdiff_t kSkeletonPartitionCountOffset = 0x80;
        constexpr std::size_t kSkeletonBoneStride = 0x10;
        constexpr std::size_t kSkeletonPartitionStride = 0x10;
        constexpr std::ptrdiff_t kPartitionStartBoneOffset = 0x08;
        constexpr std::ptrdiff_t kPartitionBoneCountOffset = 0x0A;

        constexpr std::ptrdiff_t kAnimationFromBindingOffset = 0x18;
        constexpr std::ptrdiff_t kTrackToBoneMappingOffset = 0x20;
        constexpr std::ptrdiff_t kTrackToBoneMappingCountOffset = 0x28;
        constexpr std::ptrdiff_t kPartitionIndicesOffset = 0x40;
        constexpr std::ptrdiff_t kPartitionIndicesCountOffset = 0x48;
        constexpr std::ptrdiff_t kAnimationTypeOffset = 0x10;
        constexpr std::ptrdiff_t kAnimationDurationOffset = 0x14;
        constexpr std::ptrdiff_t kAnimationTransformTrackCountOffset = 0x18;
        constexpr std::ptrdiff_t kAnimationFloatTrackCountOffset = 0x1C;
        constexpr std::ptrdiff_t kAnimationResourceFlagsOffset = 0x0C;
        constexpr std::ptrdiff_t kAnimationResourceDataOffset = 0x20;
        constexpr std::ptrdiff_t kRootContainerFromAnimationDataOffset = 0x08;
        constexpr std::size_t kSampleTracksVtableSlot = 5;

        constexpr std::size_t kMaxBonesAndTracks = 768;
        constexpr std::size_t kMaxSkeletonPartitions = 256;
        constexpr std::size_t kMaxBoneChainDepth = 96;
        constexpr std::size_t kMaxAnimationFiles = 512;
        constexpr std::size_t kAnimationPathCapacity = 260;
        constexpr std::uint32_t kSamplesPerFrame = 24;
        constexpr std::uint32_t kWeaponAnimationRole = 1;
        constexpr std::int32_t kIoTaskPriority = 3;
        constexpr ULONGLONG kGraphLoadTimeoutMilliseconds = 30000;
        constexpr ULONGLONG kClipLoadTimeoutMilliseconds = 15000;
        constexpr ULONGLONG kLongLoadLogDelayMilliseconds = 5000;

        struct alignas(16) HkQsTransform
        {
            float translation[4]{};
            float rotation[4]{};
            float scale[4]{};
        };
        static_assert(sizeof(HkQsTransform) == 0x30);

        struct AnimationResourceHandle
        {
            void* entry{ nullptr };
        };
        static_assert(sizeof(AnimationResourceHandle) == sizeof(void*));

        using GraphHolderCtorFn = void* (*)(void*);
        using GraphHolderDtorFn = void (*)(void*);
        using CreateBackgroundSimpleManagerFn = bool (*)(void*, RE::BSScrapArray<RE::BSStaticStringT<260>>*, std::int32_t);
        using IsAnimationLoadingCompleteFn = bool (*)(void*);
        using AddItemToTargetKeywordsFn = void (*)(RE::BGSObjectInstance*, RE::BSScrapArray<RE::IKeywordFormBase*>*);
        using RequestAnimationSubGraphFn = void (*)(RE::Actor*, RE::BSAnimationGraphManager*, std::int32_t*, RE::BSScrapArray<RE::IKeywordFormBase*>*, std::int32_t*,
            RE::BSTSmallArray<RE::SubgraphHandle, 2>*, RE::BSTSmallArray<RE::SubgraphIdentifier, 2>*);
        using IsAnimationSubGraphLoadedFn = bool (*)(RE::BSTSmartPointer<RE::BSAnimationGraphManager>*, RE::BSTSmallArray<RE::SubgraphHandle, 2>*, std::int32_t*);
        using ReleaseAnimationSubGraphFn = void (*)(RE::BSTSmartPointer<RE::BSAnimationGraphManager>*, RE::BSTSmallArray<RE::SubgraphHandle, 2>*);
        using GetAnimationFilesForSubgraphFn = const RE::BSTArray<RE::BSFixedString>* (*)(const std::uint64_t*);
        using LoadAnimationResourceFn = bool (*)(RE::BSFixedString*, AnimationResourceHandle*);
        using MoveAnimationResourceHandleFn = AnimationResourceHandle* (*)(AnimationResourceHandle*, AnimationResourceHandle*);
        using IsHkxDerivativeDbDataFn = bool (*)(void*);
        using RetrieveBindingFromContainerFn = void (*)(void*, void**, char*);
        using FindBoneWithNameFn = std::uint64_t (*)(void*, const char*, void*);
        using SampleAnimationTracksFn = void (*)(void*, float, int, HkQsTransform*, int, float*);

        struct NativeFunctions
        {
            GraphHolderCtorFn graphHolderCtor{ nullptr };
            GraphHolderDtorFn graphHolderDtor{ nullptr };
            CreateBackgroundSimpleManagerFn createBackgroundSimpleManager{ nullptr };
            IsAnimationLoadingCompleteFn isAnimationLoadingComplete{ nullptr };
            AddItemToTargetKeywordsFn addItemToTargetKeywords{ nullptr };
            RequestAnimationSubGraphFn requestAnimationSubGraph{ nullptr };
            IsAnimationSubGraphLoadedFn isAnimationSubGraphLoaded{ nullptr };
            ReleaseAnimationSubGraphFn releaseAnimationSubGraph{ nullptr };
            GetAnimationFilesForSubgraphFn getAnimationFilesForSubgraph{ nullptr };
            LoadAnimationResourceFn loadAnimationResource{ nullptr };
            MoveAnimationResourceHandleFn moveAnimationResourceHandle{ nullptr };
            IsHkxDerivativeDbDataFn isHkxDerivativeDbData{ nullptr };
            RetrieveBindingFromContainerFn retrieveBindingFromContainer{ nullptr };
            FindBoneWithNameFn findBoneWithName{ nullptr };
        };

        struct BoneTarget
        {
            std::array<char, weapon_clip_stroke::kMaxBoneName> name{};
            std::int16_t boneIndex{ -1 };
            std::uint16_t chainCount{ 0 };
            std::array<std::int16_t, kMaxBoneChainDepth> chain{};
        };

        struct ClipWork
        {
            float durationSeconds{ 0.0f };
            std::uint32_t sampleCount{ 0 };
            std::uint32_t nextSample{ 0 };
            std::uint32_t targetCount{ 0 };
            std::int32_t animationType{ -1 };
            std::int32_t floatTrackCount{ 0 };
            std::int32_t transformTrackCount{ 0 };
            std::int32_t boneCount{ 0 };
            std::int16_t weaponBoneIndex{ -1 };
            std::array<std::int16_t, kMaxBonesAndTracks> trackToBone{};
            std::array<std::int16_t, kMaxBonesAndTracks> parentIndices{};
            std::array<std::uint8_t, kMaxBonesAndTracks> requiredBones{};
            std::array<RE::NiTransform, kMaxBonesAndTracks> referenceLocals{};
            std::array<BoneTarget, weapon_clip_stroke::kMaxExactTracksPerClip> targets{};
            std::array<weapon_clip_stroke::ExactTrackSamples,
                weapon_clip_stroke::kMaxExactTracksPerClip>
                tracks{};
        };

        struct Job
        {
            alignas(16) std::array<std::byte, kSimpleAnimationGraphManagerHolderSize> graphHolderStorage{};
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData{};
            RE::BSTSmallArray<RE::SubgraphHandle, 2> subgraphHandles{};
            RE::BSTSmallArray<RE::SubgraphIdentifier, 2> subgraphIdentifiers{};
            AnimationResourceHandle clipResource{};
            std::array<std::array<char, kAnimationPathCapacity>, kMaxAnimationFiles> animationPaths{};
            std::array<char, kAnimationPathCapacity> currentClipPath{};
            RE::TESObjectWEAP* weapon{ nullptr };  // Loaded-form identity; never owns the form.
            RE::TESRace* race{ nullptr };          // Loaded-form identity; never owns the form.
            std::uint32_t weaponFormId{ 0 };
            std::uint64_t weaponGenerationKey{ 0 };
            std::uint32_t animationPathCount{ 0 };
            std::uint32_t animationPathIndex{ 0 };
            std::uint64_t subgraphIdentifier{ 0 };
            ULONGLONG phaseStartedAtMilliseconds{ 0 };
            State phase{ State::Idle };
            bool inPowerArmor{ false };
            bool graphHolderConstructed{ false };
            bool longLoadLogged{ false };
        };

        struct Runtime
        {
            NativeFunctions native{};
            Job job{};
            ClipWork clip{};
            Stats stats{};
            alignas(16) std::array<HkQsTransform, kMaxBonesAndTracks> sampledTracks{};
            std::array<RE::NiTransform, kMaxBonesAndTracks> sampledLocals{};
            std::atomic<DWORD> ownerThreadId{ 0 };
            // F4SE session messages can request teardown off the ROCK frame
            // thread. Only the established owner may touch retained Bethesda
            // graph/resource state; other callers publish a bounded request
            // that the owner consumes before its next job step.
            std::atomic_bool resetRequested{ false };
            bool nativeValidationAttempted{ false };
            bool nativeValidated{ false };
            std::atomic_bool threadMismatchLogged{ false };
        };

        [[nodiscard]] Runtime& runtime()
        {
            // Process-lifetime allocation avoids a static teardown racing a
            // retained Bethesda IO task. Explicit reset handles gameplay
            // disable/session boundaries on the owning frame thread.
            static Runtime* instance = new Runtime();
            return *instance;
        }

        [[nodiscard]] bool claimOrValidateThread(Runtime& state)
        {
            const DWORD currentThreadId = GetCurrentThreadId();
            DWORD ownerThreadId = state.ownerThreadId.load(
                std::memory_order_acquire);
            if (ownerThreadId == 0) {
                DWORD expected = 0;
                if (state.ownerThreadId.compare_exchange_strong(
                        expected,
                        currentThreadId,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return true;
                }
                ownerThreadId = expected;
            }
            if (ownerThreadId == currentThreadId) {
                return true;
            }
            if (!state.threadMismatchLogged.exchange(
                    true, std::memory_order_relaxed)) {
                RDX_LOG_ERROR(Animation,
                    "Authored animation preharvest rejected non-owner thread owner={} caller={}",
                    ownerThreadId,
                    currentThreadId);
            }
            return false;
        }

        [[nodiscard]] bool addressIsInGameText(const std::uintptr_t address)
        {
            const auto text = REL::Module::get().segment(REL::Segment::text);
            return address >= text.address() && address < text.address() + text.size();
        }

        template <std::size_t N>
        [[nodiscard]] bool validateNativeEntry(
            const char* label,
            const std::uintptr_t offset,
            const std::array<std::uint8_t, N>& expected)
        {
            const auto address = REL::Offset(offset).address();
            std::array<std::uint8_t, N> actual{};
            if (!addressIsInGameText(address) ||
                !native_memory::guardedCopyFromMemory(
                    reinterpret_cast<const void*>(address), actual.data(), actual.size()) ||
                actual != expected) {
                RDX_LOG_ERROR(Init,
                    "Authored animation preharvest validation failed for {} at 0x{:X}",
                    label,
                    address);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool resolveNativeFunctions(Runtime& state)
        {
            if (state.nativeValidationAttempted) {
                return state.nativeValidated;
            }
            state.nativeValidationAttempted = true;
            if (!REL::Module::IsVR() ||
                REL::Module::get().version() != F4SE::RUNTIME_VR_1_2_72) {
                RDX_LOG_ERROR(Init,
                    "Authored animation preharvest requires verified Fallout4VR.exe 1.2.72 layouts");
                return false;
            }

            const bool entriesMatch = validateNativeEntry(
                                          "SimpleAnimationGraphManagerHolder::ctor",
                                          kSimpleAnimationGraphManagerHolderCtor,
                                          std::array<std::uint8_t, 6>{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 }) &&
                validateNativeEntry(
                    "SimpleAnimationGraphManagerHolder::dtor",
                    kSimpleAnimationGraphManagerHolderDtor,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x10 }) &&
                validateNativeEntry(
                    "SimpleAnimationGraphManagerHolder::CreateBackgroundSimpleManager",
                    kCreateBackgroundSimpleManager,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x18 }) &&
                validateNativeEntry(
                    "SimpleAnimationGraphManagerHolder::IsAnimationLoadingComplete",
                    kIsAnimationLoadingComplete,
                    std::array<std::uint8_t, 9>{ 0x48, 0x8B, 0x41, 0x10, 0x48, 0x85, 0xC0, 0x74, 0x0C }) &&
                validateNativeEntry(
                    "RequestAnimationSubGraph",
                    kRequestAnimationSubGraph,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x08 }) &&
                validateNativeEntry(
                    "IsAnimationSubGraphLoaded",
                    kIsAnimationSubGraphLoaded,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x08 }) &&
                validateNativeEntry(
                    "ReleaseAnimationSubGraph",
                    kReleaseAnimationSubGraph,
                    std::array<std::uint8_t, 7>{ 0x48, 0x83, 0xEC, 0x28, 0x83, 0x7A, 0x18 }) &&
                validateNativeEntry(
                    "AddItemToTargetKeywords",
                    kAddItemToTargetKeywords,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x08 }) &&
                validateNativeEntry(
                    "AnimationFileData numeric lookup",
                    kGetAnimationFilesForSubgraph,
                    std::array<std::uint8_t, 6>{ 0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D }) &&
                validateNativeEntry(
                    "LoadIdle animation resource",
                    kLoadAnimationResource,
                    std::array<std::uint8_t, 9>{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56 }) &&
                validateNativeEntry(
                    "BShkbHkxDB resource-handle move assignment",
                    kMoveAnimationResourceHandle,
                    std::array<std::uint8_t, 10>{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 }) &&
                validateNativeEntry(
                    "BShkbHkxDBUtils::IsHkxDerivativeDBData",
                    kIsHkxDerivativeDbData,
                    std::array<std::uint8_t, 10>{ 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08 }) &&
                validateNativeEntry(
                    "BShkbUtils::RetrieveBindingFromContainer",
                    kRetrieveBindingFromContainer,
                    std::array<std::uint8_t, 10>{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 }) &&
                validateNativeEntry(
                    "hkaSkeletonUtils::findBoneWithName",
                    kFindBoneWithName,
                    std::array<std::uint8_t, 5>{ 0x48, 0x89, 0x5C, 0x24, 0x08 });
            if (!entriesMatch) {
                return false;
            }

            state.native.graphHolderCtor = reinterpret_cast<GraphHolderCtorFn>(
                REL::Offset(kSimpleAnimationGraphManagerHolderCtor).address());
            state.native.graphHolderDtor = reinterpret_cast<GraphHolderDtorFn>(
                REL::Offset(kSimpleAnimationGraphManagerHolderDtor).address());
            state.native.createBackgroundSimpleManager = reinterpret_cast<CreateBackgroundSimpleManagerFn>(
                REL::Offset(kCreateBackgroundSimpleManager).address());
            state.native.isAnimationLoadingComplete = reinterpret_cast<IsAnimationLoadingCompleteFn>(
                REL::Offset(kIsAnimationLoadingComplete).address());
            state.native.requestAnimationSubGraph = reinterpret_cast<RequestAnimationSubGraphFn>(
                REL::Offset(kRequestAnimationSubGraph).address());
            state.native.isAnimationSubGraphLoaded = reinterpret_cast<IsAnimationSubGraphLoadedFn>(
                REL::Offset(kIsAnimationSubGraphLoaded).address());
            state.native.releaseAnimationSubGraph = reinterpret_cast<ReleaseAnimationSubGraphFn>(
                REL::Offset(kReleaseAnimationSubGraph).address());
            state.native.addItemToTargetKeywords = reinterpret_cast<AddItemToTargetKeywordsFn>(
                REL::Offset(kAddItemToTargetKeywords).address());
            state.native.getAnimationFilesForSubgraph = reinterpret_cast<GetAnimationFilesForSubgraphFn>(
                REL::Offset(kGetAnimationFilesForSubgraph).address());
            state.native.loadAnimationResource = reinterpret_cast<LoadAnimationResourceFn>(
                REL::Offset(kLoadAnimationResource).address());
            state.native.moveAnimationResourceHandle = reinterpret_cast<MoveAnimationResourceHandleFn>(
                REL::Offset(kMoveAnimationResourceHandle).address());
            state.native.isHkxDerivativeDbData = reinterpret_cast<IsHkxDerivativeDbDataFn>(
                REL::Offset(kIsHkxDerivativeDbData).address());
            state.native.retrieveBindingFromContainer = reinterpret_cast<RetrieveBindingFromContainerFn>(
                REL::Offset(kRetrieveBindingFromContainer).address());
            state.native.findBoneWithName = reinterpret_cast<FindBoneWithNameFn>(
                REL::Offset(kFindBoneWithName).address());
            state.nativeValidated = true;
            RDX_LOG_INFO(Init,
                "Authored animation preharvest validated: exact off-screen weapon subgraph and direct full-clip sampler ready");
            return true;
        }

        [[nodiscard]] RE::SimpleAnimationGraphManagerHolder* graphHolder(Job& job)
        {
            return job.graphHolderConstructed
                ? reinterpret_cast<RE::SimpleAnimationGraphManagerHolder*>(job.graphHolderStorage.data())
                : nullptr;
        }

        template <class T>
        [[nodiscard]] bool prepareNativeSubgraphOutput(
            RE::BSTSmallArray<T, kSubgraphOutputInlineCapacity>& output)
        {
            static_assert(sizeof(T) == sizeof(std::uint64_t));
            if (!output.empty()) {
                return false;
            }
            output.reserve(kSubgraphOutputInlineCapacity);
            const auto* expectedInlineData =
                reinterpret_cast<const std::byte*>(&output) + kSmallArrayInlineStorageOffset;
            return output.capacity() == kSubgraphOutputInlineCapacity &&
                   static_cast<const void*>(output.data()) ==
                       static_cast<const void*>(expectedInlineData);
        }

        void clearClipWork(ClipWork& clip) noexcept
        {
            static_assert(std::is_trivially_copyable_v<ClipWork>);
            // ClipWork is roughly two MiB. Aggregate assignment from `{}` may
            // materialize that temporary on Fallout's main-thread stack;
            // clear the process-lifetime heap object in place instead.
            std::memset(&clip, 0, sizeof(clip));
        }

        void clearJob(Job& job)
        {
            // Field-wise teardown avoids a ~130 KiB aggregate temporary and
            // still honors the smart-pointer/small-array member semantics.
            job.graphHolderStorage.fill(std::byte{});
            job.instanceData = nullptr;
            job.subgraphHandles.clear();
            job.subgraphIdentifiers.clear();
            job.clipResource = {};
            job.currentClipPath.fill('\0');
            job.weapon = nullptr;
            job.race = nullptr;
            job.weaponFormId = 0;
            job.weaponGenerationKey = 0;
            job.animationPathCount = 0;
            job.animationPathIndex = 0;
            job.subgraphIdentifier = 0;
            job.phaseStartedAtMilliseconds = 0;
            job.phase = State::Idle;
            job.inPowerArmor = false;
            job.graphHolderConstructed = false;
            job.longLoadLogged = false;
        }

        void releaseClipResource(Runtime& state)
        {
            if (!state.job.clipResource.entry || !state.native.moveAnimationResourceHandle) {
                state.job.clipResource = {};
                return;
            }
            AnimationResourceHandle empty{};
            state.native.moveAnimationResourceHandle(&state.job.clipResource, &empty);
            state.job.clipResource = {};
        }

        void releaseNativeOwnership(Runtime& state)
        {
            releaseClipResource(state);
            auto& job = state.job;
            if (job.graphHolderConstructed) {
                auto* holder = graphHolder(job);
                if (holder && holder->animationGraphManager &&
                    !job.subgraphHandles.empty() && state.native.releaseAnimationSubGraph) {
                    state.native.releaseAnimationSubGraph(
                        &holder->animationGraphManager,
                        &job.subgraphHandles);
                }
                job.subgraphHandles.clear();
                job.subgraphIdentifiers.clear();
                if (state.native.graphHolderDtor) {
                    state.native.graphHolderDtor(job.graphHolderStorage.data());
                }
                job.graphHolderConstructed = false;
            }
            clearClipWork(state.clip);
        }

        void resetRuntimeJob(Runtime& state)
        {
            releaseNativeOwnership(state);
            clearJob(state.job);
            clearClipWork(state.clip);
            state.stats = {};
        }

        void finishTerminal(Runtime& state, const State terminalState, const char* reason)
        {
            const auto formId = state.job.weaponFormId;
            const auto generation = state.job.weaponGenerationKey;
            const auto stats = state.stats;
            releaseNativeOwnership(state);
            clearJob(state.job);
            state.job.weaponFormId = formId;
            state.job.weaponGenerationKey = generation;
            state.job.phase = terminalState;
            state.stats = stats;

            if (terminalState == State::Completed) {
                RDX_LOG_INFO(Animation,
                    "Authored animation preharvest complete weapon={:08X} generation={:016X} files={} sampled={} movingClips={} groups={} rejected={}",
                    formId,
                    generation,
                    stats.animationFileCount,
                    stats.clipsSampled,
                    stats.clipsWithPartMotion,
                    stats.groupsProduced,
                    stats.clipsRejected);
            } else {
                RDX_LOG_WARN(Animation,
                    "Authored animation preharvest failed weapon={:08X} generation={:016X} phase={} reason={} files={} sampled={} rejected={}",
                    formId,
                    generation,
                    static_cast<unsigned>(terminalState),
                    reason ? reason : "unknown",
                    stats.animationFileCount,
                    stats.clipsSampled,
                    stats.clipsRejected);
            }
        }

        [[nodiscard]] bool isFiniteTransform(const RE::NiTransform& transform)
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
                   std::abs(transform.scale) > 0.000001f;
        }

        [[nodiscard]] bool convertHavokLocalTransform(
            const HkQsTransform& source,
            RE::NiTransform& outTransform)
        {
            for (const auto value : source.translation) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            float quaternionLengthSquared = 0.0f;
            for (const auto value : source.rotation) {
                if (!std::isfinite(value)) {
                    return false;
                }
                quaternionLengthSquared += value * value;
            }
            if (quaternionLengthSquared < 0.000001f) {
                return false;
            }
            for (const auto value : source.scale) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            if (std::abs(source.scale[0]) < 0.000001f ||
                std::abs(source.scale[0] - source.scale[1]) > 0.001f ||
                std::abs(source.scale[0] - source.scale[2]) > 0.001f) {
                return false;
            }

            outTransform.translate = RE::NiPoint3{
                source.translation[0], source.translation[1], source.translation[2]
            };
            // This transpose is the measured ROCK boundary between the hka
            // quaternion's stored axes and NiTransform relationship storage.
            outTransform.rotate = transform_math::transposeRotation(
                transform_math::havokQuaternionToNiRows<RE::NiMatrix3>(source.rotation));
            outTransform.scale = source.scale[0];
            return isFiniteTransform(outTransform);
        }

        [[nodiscard]] weapon_part_motion_path::PoseSample poseFromNiTransform(
            const RE::NiTransform& transform)
        {
            float quaternion[4]{};
            transform_math::niRowsToHavokQuaternion(transform.rotate, quaternion);
            return weapon_part_motion_path::PoseSample{
                .translate = {
                    transform.translate.x,
                    transform.translate.y,
                    transform.translate.z,
                },
                .rotate = {
                    quaternion[3], quaternion[0], quaternion[1], quaternion[2]
                },
            };
        }

        [[nodiscard]] bool addressIsExecutable(const void* address)
        {
            if (!address) {
                return false;
            }
            MEMORY_BASIC_INFORMATION memoryInfo{};
            if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) == 0 ||
                memoryInfo.State != MEM_COMMIT ||
                (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }
            const DWORD protection = memoryInfo.Protect & 0xFF;
            return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                   protection == PAGE_EXECUTE_READWRITE ||
                   protection == PAGE_EXECUTE_WRITECOPY;
        }

        [[nodiscard]] bool guardedSampleTracks(
            const SampleAnimationTracksFn sampler,
            void* animation,
            const float timeSeconds,
            const int transformTrackCount,
            HkQsTransform* output) noexcept
        {
#if defined(_MSC_VER)
            __try {
                sampler(animation, timeSeconds, transformTrackCount, output, 0, nullptr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#else
            sampler(animation, timeSeconds, transformTrackCount, output, 0, nullptr);
            return true;
#endif
        }

        [[nodiscard]] bool guardedIsHkxDerivativeDbData(
            const IsHkxDerivativeDbDataFn function,
            void* animationData,
            bool& outIsDerivative) noexcept
        {
#if defined(_MSC_VER)
            __try {
                outIsDerivative = function(animationData);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#else
            outIsDerivative = function(animationData);
            return true;
#endif
        }

        [[nodiscard]] bool guardedRetrieveBindingFromContainer(
            const RetrieveBindingFromContainerFn function,
            void* rootContainer,
            void*& outBinding,
            char* clipPath) noexcept
        {
#if defined(_MSC_VER)
            __try {
                function(rootContainer, &outBinding, clipPath);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#else
            function(rootContainer, &outBinding, clipPath);
            return true;
#endif
        }

        [[nodiscard]] bool guardedFindBoneWithName(
            const FindBoneWithNameFn function,
            void* skeleton,
            const char* name,
            std::uint64_t& outBone) noexcept
        {
#if defined(_MSC_VER)
            __try {
                outBone = function(skeleton, name, nullptr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#else
            outBone = function(skeleton, name, nullptr);
            return true;
#endif
        }

        [[nodiscard]] bool copyCString(
            const char* source,
            char* destination,
            const std::size_t capacity)
        {
            if (!source || !destination || capacity < 2) {
                return false;
            }
            destination[0] = '\0';
            for (std::size_t index = 0; index < capacity - 1; ++index) {
                char value = '\0';
                if (!native_memory::guardedCopyFromMemory(source + index, &value, sizeof(value))) {
                    destination[0] = '\0';
                    return false;
                }
                destination[index] = value;
                if (value == '\0') {
                    return index > 0;
                }
            }
            destination[capacity - 1] = '\0';
            return false;
        }

        [[nodiscard]] bool skeletonBoneName(
            const void* bones,
            const std::int32_t boneIndex,
            std::array<char, weapon_clip_stroke::kMaxBoneName>& outName)
        {
            outName = {};
            if (!bones || boneIndex < 0) {
                return false;
            }
            const auto* entry = reinterpret_cast<const std::byte*>(bones) +
                static_cast<std::size_t>(boneIndex) * kSkeletonBoneStride;
            std::uintptr_t flaggedName = 0;
            if (!native_memory::tryReadValue(
                    reinterpret_cast<const std::uintptr_t*>(entry), flaggedName)) {
                return false;
            }
            const auto* name = reinterpret_cast<const char*>(flaggedName & ~std::uintptr_t{ 1 });
            return copyCString(name, outName.data(), outName.size());
        }

        [[nodiscard]] bool nameIsAllowed(
            const std::string_view boneName,
            const char* const* allowedNodeNames,
            const std::uint32_t allowedNodeNameCount)
        {
            if (!allowedNodeNames || boneName.empty()) {
                return false;
            }
            for (std::uint32_t index = 0; index < allowedNodeNameCount; ++index) {
                const char* candidate = allowedNodeNames[index];
                if (candidate &&
                    weapon_animation_preharvest_policy::boneNameMatchesSceneNode(
                        boneName, std::string_view{ candidate })) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] RE::BShkbAnimationGraph* firstPersonGraph(Runtime& state)
        {
            auto* holder = graphHolder(state.job);
            auto* manager = holder ? holder->animationGraphManager.get() : nullptr;
            if (!manager) {
                return nullptr;
            }
            const auto selection =
                weapon_animation_preharvest_policy::selectFirstPersonGraph(
                    manager->graph.size(), state.job.subgraphIdentifiers.size());
            if (!selection.valid) {
                return nullptr;
            }
            const auto index = static_cast<decltype(manager->graph)::size_type>(
                selection.graphIndex);
            return manager->graph[index].get();
        }

        [[nodiscard]] bool resolveRetainedClip(
            Runtime& state,
            RE::BShkbAnimationGraph*& outGraph,
            void*& outSkeleton,
            void*& outBinding,
            void*& outAnimation)
        {
            outGraph = nullptr;
            outSkeleton = nullptr;
            outBinding = nullptr;
            outAnimation = nullptr;
            auto& job = state.job;
            if (!job.clipResource.entry || job.currentClipPath[0] == '\0') {
                return false;
            }

            void* animationData = nullptr;
            if (!native_memory::tryReadField(
                    job.clipResource.entry, kAnimationResourceDataOffset, animationData) ||
                !animationData) {
                return false;
            }
            bool isDerivative = false;
            if (!guardedIsHkxDerivativeDbData(
                    state.native.isHkxDerivativeDbData, animationData, isDerivative) ||
                !isDerivative) {
                return false;
            }
            void* rootContainer = nullptr;
            if (!native_memory::tryReadField(
                    animationData, kRootContainerFromAnimationDataOffset, rootContainer) ||
                !rootContainer ||
                !guardedRetrieveBindingFromContainer(
                    state.native.retrieveBindingFromContainer,
                    rootContainer,
                    outBinding,
                    job.currentClipPath.data()) ||
                !outBinding ||
                !native_memory::tryReadField(
                    outBinding, kAnimationFromBindingOffset, outAnimation) ||
                !outAnimation) {
                return false;
            }

            outGraph = firstPersonGraph(state);
            void* skeletonOwner = nullptr;
            return outGraph &&
                   native_memory::tryReadField(
                       outGraph, kGraphSkeletonOwnerOffset, skeletonOwner) &&
                   skeletonOwner &&
                   native_memory::tryReadField(
                       skeletonOwner, kSkeletonFromOwnerOffset, outSkeleton) &&
                   outSkeleton;
        }

        [[nodiscard]] bool buildTrackMapping(
            void* binding,
            void* skeleton,
            const std::uint32_t transformTrackCount,
            const std::uint32_t boneCount,
            std::array<std::int16_t, kMaxBonesAndTracks>& outTrackToBone)
        {
            const std::int16_t* explicitMapping = nullptr;
            std::int32_t explicitMappingCount = 0;
            if (!native_memory::tryReadField(
                    binding, kTrackToBoneMappingOffset, explicitMapping) ||
                !native_memory::tryReadField(
                    binding, kTrackToBoneMappingCountOffset, explicitMappingCount) ||
                explicitMappingCount < 0 ||
                explicitMappingCount > static_cast<std::int32_t>(kMaxBonesAndTracks)) {
                return false;
            }

            std::array<std::int16_t, kMaxBonesAndTracks> explicitBuffer{};
            std::span<const std::int16_t> explicitSpan{};
            if (explicitMappingCount > 0) {
                if (!explicitMapping ||
                    !native_memory::guardedCopyFromMemory(
                        explicitMapping,
                        explicitBuffer.data(),
                        static_cast<std::size_t>(explicitMappingCount) * sizeof(std::int16_t))) {
                    return false;
                }
                explicitSpan = {
                    explicitBuffer.data(), static_cast<std::size_t>(explicitMappingCount)
                };
            }

            const std::uint16_t* partitionIndices = nullptr;
            std::int32_t partitionIndexCount = 0;
            const void* skeletonPartitions = nullptr;
            std::int32_t skeletonPartitionCount = 0;
            if (!native_memory::tryReadField(
                    binding, kPartitionIndicesOffset, partitionIndices) ||
                !native_memory::tryReadField(
                    binding, kPartitionIndicesCountOffset, partitionIndexCount) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonPartitionsOffset, skeletonPartitions) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonPartitionCountOffset, skeletonPartitionCount) ||
                partitionIndexCount < 0 ||
                skeletonPartitionCount < 0 ||
                partitionIndexCount > static_cast<std::int32_t>(kMaxSkeletonPartitions) ||
                skeletonPartitionCount > static_cast<std::int32_t>(kMaxSkeletonPartitions)) {
                return false;
            }

            std::array<std::uint16_t, kMaxSkeletonPartitions> partitionIndexBuffer{};
            std::span<const std::uint16_t> partitionIndexSpan{};
            if (partitionIndexCount > 0) {
                if (!partitionIndices ||
                    !native_memory::guardedCopyFromMemory(
                        partitionIndices,
                        partitionIndexBuffer.data(),
                        static_cast<std::size_t>(partitionIndexCount) * sizeof(std::uint16_t))) {
                    return false;
                }
                partitionIndexSpan = {
                    partitionIndexBuffer.data(), static_cast<std::size_t>(partitionIndexCount)
                };
            }

            std::array<weapon_animation_preharvest_policy::SkeletonPartition,
                kMaxSkeletonPartitions>
                partitionBuffer{};
            if (skeletonPartitionCount > 0) {
                if (!skeletonPartitions) {
                    return false;
                }
                for (std::int32_t index = 0; index < skeletonPartitionCount; ++index) {
                    const auto* entry = reinterpret_cast<const std::byte*>(skeletonPartitions) +
                        static_cast<std::size_t>(index) * kSkeletonPartitionStride;
                    if (!native_memory::tryReadField(
                            entry,
                            kPartitionStartBoneOffset,
                            partitionBuffer[static_cast<std::size_t>(index)].startBone) ||
                        !native_memory::tryReadField(
                            entry,
                            kPartitionBoneCountOffset,
                            partitionBuffer[static_cast<std::size_t>(index)].boneCount)) {
                        return false;
                    }
                }
            }

            return weapon_animation_preharvest_policy::buildTrackToBoneMap(
                transformTrackCount,
                boneCount,
                explicitSpan,
                partitionIndexSpan,
                std::span<const weapon_animation_preharvest_policy::SkeletonPartition>{
                    partitionBuffer.data(), static_cast<std::size_t>(skeletonPartitionCount)
                },
                std::span<std::int16_t>{ outTrackToBone.data(), transformTrackCount });
        }

        [[nodiscard]] bool prepareClipWork(
            Runtime& state,
            const char* const* allowedNodeNames,
            const std::uint32_t allowedNodeNameCount)
        {
            RE::BShkbAnimationGraph* graph = nullptr;
            void* skeleton = nullptr;
            void* binding = nullptr;
            void* animation = nullptr;
            if (!resolveRetainedClip(state, graph, skeleton, binding, animation)) {
                return false;
            }

            clearClipWork(state.clip);
            auto& prepared = state.clip;
            if (!native_memory::tryReadField(
                    animation, kAnimationTypeOffset, prepared.animationType) ||
                !native_memory::tryReadField(
                    animation, kAnimationDurationOffset, prepared.durationSeconds) ||
                !native_memory::tryReadField(
                    animation,
                    kAnimationTransformTrackCountOffset,
                    prepared.transformTrackCount) ||
                !native_memory::tryReadField(
                    animation, kAnimationFloatTrackCountOffset, prepared.floatTrackCount) ||
                !std::isfinite(prepared.durationSeconds) ||
                !(prepared.durationSeconds > 0.0f) ||
                prepared.transformTrackCount <= 0 ||
                prepared.transformTrackCount > static_cast<std::int32_t>(kMaxBonesAndTracks)) {
                return false;
            }
            prepared.sampleCount =
                weapon_animation_preharvest_policy::clipSampleCount(
                    prepared.durationSeconds);
            if (prepared.sampleCount < 2 ||
                prepared.sampleCount > weapon_clip_stroke::kMaxClipSampleCount) {
                return false;
            }

            const std::int16_t* parents = nullptr;
            std::int32_t parentCount = 0;
            const void* bones = nullptr;
            if (!native_memory::tryReadField(
                    skeleton, kSkeletonBoneCountOffset, prepared.boneCount) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonParentIndicesOffset, parents) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonParentCountOffset, parentCount) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonBonesOffset, bones) ||
                prepared.boneCount <= 0 ||
                prepared.boneCount > static_cast<std::int32_t>(kMaxBonesAndTracks) ||
                parentCount < prepared.boneCount || !parents || !bones ||
                !native_memory::guardedCopyFromMemory(
                    parents,
                    prepared.parentIndices.data(),
                    static_cast<std::size_t>(prepared.boneCount) * sizeof(std::int16_t)) ||
                !buildTrackMapping(
                    binding,
                    skeleton,
                    static_cast<std::uint32_t>(prepared.transformTrackCount),
                    static_cast<std::uint32_t>(prepared.boneCount),
                    prepared.trackToBone)) {
                return false;
            }

            std::uint64_t weaponBoneRaw = 0xFFFFFFFFull;
            if (!guardedFindBoneWithName(
                    state.native.findBoneWithName,
                    skeleton,
                    "Weapon",
                    weaponBoneRaw) ||
                weaponBoneRaw == 0xFFFFFFFFull ||
                weaponBoneRaw >= static_cast<std::uint64_t>(prepared.boneCount)) {
                return false;
            }
            prepared.weaponBoneIndex = static_cast<std::int16_t>(weaponBoneRaw);

            const HkQsTransform* referencePose = nullptr;
            std::int32_t referencePoseCount = 0;
            std::array<HkQsTransform, kMaxBonesAndTracks> referenceBuffer{};
            if (!native_memory::tryReadField(
                    skeleton, kSkeletonReferencePoseOffset, referencePose) ||
                !native_memory::tryReadField(
                    skeleton, kSkeletonReferencePoseCountOffset, referencePoseCount) ||
                !referencePose || referencePoseCount < prepared.boneCount ||
                referencePoseCount > static_cast<std::int32_t>(kMaxBonesAndTracks) ||
                !native_memory::guardedCopyFromMemory(
                    referencePose,
                    referenceBuffer.data(),
                    static_cast<std::size_t>(prepared.boneCount) * sizeof(HkQsTransform))) {
                return false;
            }
            std::uint32_t matchingBoneCount = 0;
            for (std::int32_t bone = 0; bone < prepared.boneCount; ++bone) {
                std::array<char, weapon_clip_stroke::kMaxBoneName> name{};
                if (!skeletonBoneName(bones, bone, name)) {
                    continue;
                }
                const std::string_view nameView{ name.data() };
                if (!nameIsAllowed(nameView, allowedNodeNames, allowedNodeNameCount)) {
                    continue;
                }
                std::array<std::int16_t, kMaxBoneChainDepth> chain{};
                const auto chainCount =
                    weapon_animation_preharvest_policy::buildBoneChainBelowAncestor(
                        static_cast<std::int16_t>(bone),
                        prepared.weaponBoneIndex,
                        std::span<const std::int16_t>{
                            prepared.parentIndices.data(),
                            static_cast<std::size_t>(prepared.boneCount)
                        },
                        chain);
                if (chainCount == 0) {
                    continue;
                }
                ++matchingBoneCount;
                if (prepared.targetCount >= prepared.targets.size()) {
                    continue;
                }
                auto& target = prepared.targets[prepared.targetCount];
                target.name = name;
                target.boneIndex = static_cast<std::int16_t>(bone);
                target.chainCount = static_cast<std::uint16_t>(chainCount);
                target.chain = chain;
                auto& track = prepared.tracks[prepared.targetCount];
                track.sampleCount = 0;
                track.boneName = name;
                ++prepared.targetCount;
            }
            if (matchingBoneCount > prepared.targets.size()) {
                ++state.stats.targetBonesTruncated;
                RDX_LOG_ERROR(Animation,
                    "Authored animation preharvest rejected clip '{}' because {} matching Weapon-descendant bones exceed capacity {}",
                    state.job.currentClipPath.data(),
                    matchingBoneCount,
                    prepared.targets.size());
                return false;
            }

            // The first-person graph skeleton contains body/arm bones that
            // are irrelevant to weapon-part reconstruction and may use
            // transforms NiTransform cannot represent. Validate only the
            // sampled Weapon transform and exact descendant chains we will
            // compose; sampled tracks overwrite their corresponding
            // reference locals each step. Ancestors above Weapon are not
            // needed: Weapon and every target are reconstructed in Weapon's
            // parent frame, so those ancestors cancel by construction.
            prepared.requiredBones[static_cast<std::size_t>(
                prepared.weaponBoneIndex)] = 1;
            for (std::uint32_t targetIndex = 0;
                 targetIndex < prepared.targetCount;
                 ++targetIndex) {
                const auto& target = prepared.targets[targetIndex];
                for (std::uint16_t chainIndex = 0;
                     chainIndex < target.chainCount;
                     ++chainIndex) {
                    const auto bone = target.chain[chainIndex];
                    if (bone < 0 || bone >= prepared.boneCount) {
                        return false;
                    }
                    prepared.requiredBones[static_cast<std::size_t>(bone)] = 1;
                }
            }
            for (std::int32_t bone = 0; bone < prepared.boneCount; ++bone) {
                if (prepared.requiredBones[static_cast<std::size_t>(bone)] == 0) {
                    continue;
                }
                if (!convertHavokLocalTransform(
                        referenceBuffer[static_cast<std::size_t>(bone)],
                        prepared.referenceLocals[static_cast<std::size_t>(bone)])) {
                    return false;
                }
            }

            return true;
        }

        [[nodiscard]] bool samplerForAnimation(
            void* animation,
            SampleAnimationTracksFn& outSampler)
        {
            outSampler = nullptr;
            void** vtable = nullptr;
            return native_memory::tryReadValue(
                       reinterpret_cast<void***>(animation), vtable) &&
                   vtable &&
                   native_memory::tryReadValue(
                       reinterpret_cast<SampleAnimationTracksFn*>(vtable + kSampleTracksVtableSlot),
                       outSampler) &&
                   addressIsExecutable(reinterpret_cast<const void*>(outSampler));
        }

        [[nodiscard]] bool sampleClipBatch(Runtime& state)
        {
            auto& clip = state.clip;
            RE::BShkbAnimationGraph* graph = nullptr;
            void* skeleton = nullptr;
            void* binding = nullptr;
            void* animation = nullptr;
            if (!resolveRetainedClip(state, graph, skeleton, binding, animation)) {
                return false;
            }

            float durationSeconds = 0.0f;
            std::int32_t transformTrackCount = 0;
            if (!native_memory::tryReadField(
                    animation, kAnimationDurationOffset, durationSeconds) ||
                !native_memory::tryReadField(
                    animation,
                    kAnimationTransformTrackCountOffset,
                    transformTrackCount) ||
                std::abs(durationSeconds - clip.durationSeconds) > 0.0001f ||
                transformTrackCount != clip.transformTrackCount) {
                return false;
            }
            SampleAnimationTracksFn sampler = nullptr;
            if (!samplerForAnimation(animation, sampler)) {
                return false;
            }

            const auto endSample = (std::min)(
                clip.sampleCount, clip.nextSample + kSamplesPerFrame);
            for (; clip.nextSample < endSample; ++clip.nextSample) {
                const float timeSeconds =
                    weapon_animation_preharvest_policy::clipSampleTime(
                        clip.durationSeconds,
                        clip.nextSample,
                        clip.sampleCount);
                if (!guardedSampleTracks(
                        sampler,
                        animation,
                        timeSeconds,
                        clip.transformTrackCount,
                        state.sampledTracks.data())) {
                    return false;
                }

                for (std::int32_t bone = 0; bone < clip.boneCount; ++bone) {
                    if (clip.requiredBones[static_cast<std::size_t>(bone)] != 0) {
                        state.sampledLocals[static_cast<std::size_t>(bone)] =
                            clip.referenceLocals[static_cast<std::size_t>(bone)];
                    }
                }
                for (std::int32_t track = 0; track < clip.transformTrackCount; ++track) {
                    const auto bone = clip.trackToBone[static_cast<std::size_t>(track)];
                    if (bone < 0 || bone >= clip.boneCount) {
                        return false;
                    }
                    if (clip.requiredBones[static_cast<std::size_t>(bone)] == 0) {
                        continue;
                    }
                    if (!convertHavokLocalTransform(
                            state.sampledTracks[static_cast<std::size_t>(track)],
                            state.sampledLocals[static_cast<std::size_t>(bone)])) {
                        return false;
                    }
                }

                const auto& weaponInParent = state.sampledLocals[
                    static_cast<std::size_t>(clip.weaponBoneIndex)];
                if (!isFiniteTransform(weaponInParent)) {
                    return false;
                }

                for (std::uint32_t targetIndex = 0;
                     targetIndex < clip.targetCount;
                     ++targetIndex) {
                    const auto& target = clip.targets[targetIndex];
                    RE::NiTransform partInWeaponParent = weaponInParent;
                    for (std::uint16_t chainIndex = 0;
                         chainIndex < target.chainCount;
                         ++chainIndex) {
                        const auto bone = target.chain[chainIndex];
                        if (bone < 0 || bone >= clip.boneCount) {
                            return false;
                        }
                        partInWeaponParent = transform_math::composeTransforms(
                            partInWeaponParent,
                            state.sampledLocals[static_cast<std::size_t>(bone)]);
                    }
                    const auto weaponLocal = transform_math::relativeTransform(
                        weaponInParent, partInWeaponParent);
                    if (!isFiniteTransform(weaponLocal)) {
                        return false;
                    }
                    auto& track = clip.tracks[targetIndex];
                    track.samples[clip.nextSample] = poseFromNiTransform(weaponLocal);
                    track.scales[clip.nextSample] = {
                        weaponLocal.scale, weaponLocal.scale, weaponLocal.scale
                    };
                }
            }
            return true;
        }

        [[nodiscard]] std::string_view clipFileName(const std::string_view path)
        {
            const auto separator = path.find_last_of("/\\");
            return separator == std::string_view::npos
                ? path
                : path.substr(separator + 1);
        }

        [[nodiscard]] std::uint32_t finishSampledClip(
            Runtime& state,
            weapon_clip_stroke::AuthoredStrokeGroup* outGroups,
            const std::uint32_t maxGroups)
        {
            auto& clip = state.clip;
            for (std::uint32_t index = 0; index < clip.targetCount; ++index) {
                clip.tracks[index].sampleCount = clip.sampleCount;
            }
            const auto groupCount = weapon_clip_stroke::buildAuthoredGroups(
                clip.tracks.data(),
                clip.targetCount,
                outGroups,
                maxGroups);
            const auto fileName = clipFileName(state.job.currentClipPath.data());
            for (std::uint32_t index = 0; index < groupCount; ++index) {
                auto& group = outGroups[index];
                group.source =
                    weapon_clip_stroke::AuthoredClipSource::ExactWeaponPreharvest;
                group.trackSpace =
                    weapon_clip_stroke::AuthoredTrackSpace::WeaponRootLocal;
                const auto copyCount = (std::min)(
                    fileName.size(), group.clipAnimationName.size() - 1);
                std::memcpy(
                    group.clipAnimationName.data(), fileName.data(), copyCount);
                RDX_LOG_INFO(Animation,
                    "Authored preharvest stage weapon={:08X} clip='{}' part='{}' samples={} window={}->{} peak={} restT=({:.3f},{:.3f},{:.3f}) extremeT=({:.3f},{:.3f},{:.3f}) arc={:.3f}",
                    state.job.weaponFormId,
                    fileName,
                    group.leaderBoneName.data(),
                    clip.sampleCount,
                    group.sourceSampleStart,
                    group.sourceSampleEnd,
                    group.sourceSamplePeak,
                    group.leaderPath.keys.front().translate.x,
                    group.leaderPath.keys.front().translate.y,
                    group.leaderPath.keys.front().translate.z,
                    group.leaderPath.keys.back().translate.x,
                    group.leaderPath.keys.back().translate.y,
                    group.leaderPath.keys.back().translate.z,
                    group.leaderPath.totalArcLength);
            }

            ++state.stats.clipsSampled;
            state.stats.groupsProduced += groupCount;
            if (groupCount > 0) {
                ++state.stats.clipsWithPartMotion;
            }
            RDX_LOG_INFO(Animation,
                "Authored animation sampled weapon={:08X} clip={}/{} '{}' duration={:.3f}s samples={} targetBones={} movingGroups={}",
                state.job.weaponFormId,
                state.job.animationPathIndex + 1,
                state.job.animationPathCount,
                fileName,
                clip.durationSeconds,
                clip.sampleCount,
                clip.targetCount,
                groupCount);
            return groupCount;
        }

        void advancePastCurrentClip(Runtime& state)
        {
            releaseClipResource(state);
            clearClipWork(state.clip);
            state.job.currentClipPath = {};
            ++state.job.animationPathIndex;
            state.job.phase = State::LoadingClip;
            state.job.phaseStartedAtMilliseconds = GetTickCount64();
            state.job.longLoadLogged = false;
        }

        void rejectCurrentClip(Runtime& state, const char* reason)
        {
            ++state.stats.clipsRejected;
            RDX_LOG_WARN(Animation,
                "Authored animation preharvest skipped weapon={:08X} clip={}/{} '{}' reason={}",
                state.job.weaponFormId,
                state.job.animationPathIndex + 1,
                state.job.animationPathCount,
                state.job.currentClipPath[0] != '\0'
                    ? state.job.currentClipPath.data()
                    : state.job.animationPaths[state.job.animationPathIndex].data(),
                reason ? reason : "unknown");
            advancePastCurrentClip(state);
        }

        [[nodiscard]] bool currentEquippedWeapon(
            const std::uint32_t expectedFormId,
            RE::TESObjectWEAP*& outWeapon,
            RE::TBO_InstanceData*& outInstanceData)
        {
            outWeapon = nullptr;
            outInstanceData = nullptr;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* middleHigh =
                player && player->currentProcess ? player->currentProcess->middleHigh : nullptr;
            if (!middleHigh || middleHigh->equippedItems.empty()) {
                return false;
            }
            auto& equipped = middleHigh->equippedItems[0];
            auto* object = equipped.item.object;
            auto* weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon || weapon->GetFormID() != expectedFormId) {
                return false;
            }
            outWeapon = weapon;
            outInstanceData = equipped.item.instanceData.get();
            return true;
        }

        [[nodiscard]] bool startJob(
            Runtime& state,
            const std::uint32_t weaponFormId,
            const std::uint64_t weaponGenerationKey)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            RE::TESObjectWEAP* weapon = nullptr;
            RE::TBO_InstanceData* instanceData = nullptr;
            if (!player || !player->race ||
                !currentEquippedWeapon(weaponFormId, weapon, instanceData)) {
                return false;
            }

            clearJob(state.job);
            state.job.weapon = weapon;
            state.job.instanceData = RE::BSTSmartPointer<RE::TBO_InstanceData>(instanceData);
            state.job.race = player->race;
            state.job.weaponFormId = weaponFormId;
            state.job.weaponGenerationKey = weaponGenerationKey;
            state.job.inPowerArmor = RE::PowerArmor::PlayerInPowerArmor();
            state.job.phase = State::LoadingBaseGraphs;
            state.job.phaseStartedAtMilliseconds = GetTickCount64();
            clearClipWork(state.clip);
            state.stats = {};

            if (!prepareNativeSubgraphOutput(state.job.subgraphHandles) ||
                !prepareNativeSubgraphOutput(state.job.subgraphIdentifiers)) {
                finishTerminal(state, State::Failed, "nativeSubgraphOutputStorageUnavailable");
                return false;
            }
            void* constructed =
                state.native.graphHolderCtor(state.job.graphHolderStorage.data());
            if (constructed != state.job.graphHolderStorage.data()) {
                finishTerminal(state, State::Failed, "simpleGraphHolderConstructionFailed");
                return false;
            }
            state.job.graphHolderConstructed = true;

            auto* playerRoot = player->Get3D();
            RE::BSScrapArray<RE::BSStaticStringT<260>> graphProjects{};
            if (!playerRoot ||
                !player->PopulateGraphProjectsToLoad(playerRoot, graphProjects) ||
                graphProjects.size() < 2 ||
                !state.native.createBackgroundSimpleManager(
                    state.job.graphHolderStorage.data(),
                    &graphProjects,
                    kIoTaskPriority)) {
                finishTerminal(state, State::Failed, "backgroundGraphLoadRequestRejected");
                return false;
            }

            RDX_LOG_INFO(Animation,
                "Authored animation preharvest started weapon={:08X} generation={:016X} instance=0x{:X} baseGraph='{}' firstPersonGraph='{}'",
                weaponFormId,
                weaponGenerationKey,
                reinterpret_cast<std::uintptr_t>(instanceData),
                graphProjects[0].c_str() ? graphProjects[0].c_str() : "<null>",
                graphProjects[1].c_str() ? graphProjects[1].c_str() : "<null>");
            return true;
        }

        [[nodiscard]] bool copyExactAnimationPaths(Runtime& state)
        {
            auto* holder = graphHolder(state.job);
            auto* manager = holder ? holder->animationGraphManager.get() : nullptr;
            if (!manager) {
                return false;
            }
            const auto selection =
                weapon_animation_preharvest_policy::selectFirstPersonGraph(
                    manager->graph.size(), state.job.subgraphIdentifiers.size());
            if (!selection.valid) {
                return false;
            }
            const auto identifierIndex =
                static_cast<decltype(state.job.subgraphIdentifiers)::size_type>(
                    selection.graphIndex);
            state.job.subgraphIdentifier =
                state.job.subgraphIdentifiers[identifierIndex].identifier;
            if (state.job.subgraphIdentifier == 0) {
                return false;
            }

            void* lookupSingleton = nullptr;
            const auto singletonAddress =
                REL::Offset(kAnimationFileLookupSingleton).address();
            if (!native_memory::tryReadValue(
                    reinterpret_cast<void* const*>(singletonAddress), lookupSingleton) ||
                !lookupSingleton) {
                return false;
            }
            const auto* files = state.native.getAnimationFilesForSubgraph(
                &state.job.subgraphIdentifier);
            if (!files || files->empty() || files->size() > kMaxAnimationFiles) {
                return false;
            }

            state.job.animationPathCount = 0;
            for (const auto& file : *files) {
                const char* path = file.c_str();
                if (!path || path[0] == '\0') {
                    continue;
                }
                const std::string_view pathView{ path };
                if (pathView.size() >= kAnimationPathCapacity) {
                    ++state.stats.clipsRejected;
                    RDX_LOG_WARN(Animation,
                        "Authored animation preharvest rejected overlong AnimationFileData path ({} bytes)",
                        pathView.size());
                    continue;
                }
                bool duplicate = false;
                for (std::uint32_t existing = 0;
                     existing < state.job.animationPathCount;
                     ++existing) {
                    const std::string_view existingPath{
                        state.job.animationPaths[existing].data()
                    };
                    if (existingPath.size() != pathView.size()) {
                        continue;
                    }
                    duplicate = true;
                    for (std::size_t character = 0;
                         character < pathView.size();
                         ++character) {
                        if (weapon_animation_preharvest_policy::asciiLower(
                                existingPath[character]) !=
                            weapon_animation_preharvest_policy::asciiLower(
                                pathView[character])) {
                            duplicate = false;
                            break;
                        }
                    }
                    if (duplicate) {
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }
                auto& destination =
                    state.job.animationPaths[state.job.animationPathCount++];
                destination.fill('\0');
                std::memcpy(destination.data(), pathView.data(), pathView.size());
            }
            state.stats.animationFileCount = state.job.animationPathCount;
            return state.job.animationPathCount > 0;
        }

        [[nodiscard]] bool playerContextStillMatches(const Job& job)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            RE::TESObjectWEAP* equippedWeapon = nullptr;
            RE::TBO_InstanceData* equippedInstanceData = nullptr;
            return player && player->race == job.race &&
                   RE::PowerArmor::PlayerInPowerArmor() == job.inPowerArmor &&
                   currentEquippedWeapon(
                       job.weaponFormId, equippedWeapon, equippedInstanceData) &&
                   equippedWeapon == job.weapon &&
                   equippedInstanceData == job.instanceData.get();
        }

        [[nodiscard]] StepResult progressJob(
            Runtime& state,
            const char* const* allowedNodeNames,
            const std::uint32_t allowedNodeNameCount,
            weapon_clip_stroke::AuthoredStrokeGroup* outGroups,
            const std::uint32_t maxGroups)
        {
            auto& job = state.job;
            if (job.phase == State::Idle || job.phase == State::Completed ||
                job.phase == State::Failed) {
                return StepResult{ .state = job.phase };
            }
            if (!playerContextStillMatches(job)) {
                RDX_LOG_INFO(Animation,
                    "Authored animation preharvest restarting weapon={:08X}: equipped weapon/player graph context changed during load",
                    job.weaponFormId);
                resetRuntimeJob(state);
                return StepResult{ .state = State::Idle };
            }

            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsed = now - job.phaseStartedAtMilliseconds;
            if (!job.longLoadLogged && elapsed >= kLongLoadLogDelayMilliseconds) {
                RDX_LOG_INFO(Animation,
                    "Authored animation preharvest still loading weapon={:08X} phase={} clip={}/{} elapsedMs={}",
                    job.weaponFormId,
                    static_cast<unsigned>(job.phase),
                    job.animationPathIndex + 1,
                    job.animationPathCount,
                    elapsed);
                job.longLoadLogged = true;
            }

            if (job.phase == State::LoadingBaseGraphs) {
                if (elapsed >= kGraphLoadTimeoutMilliseconds) {
                    finishTerminal(state, State::Failed, "baseGraphLoadTimedOut");
                    return StepResult{ .state = State::Failed };
                }
                auto* holder = graphHolder(job);
                if (!holder || !state.native.isAnimationLoadingComplete(holder)) {
                    return StepResult{ .state = job.phase };
                }
                auto* manager = holder->animationGraphManager.get();
                if (!manager) {
                    finishTerminal(state, State::Failed, "backgroundManagerUnavailable");
                    return StepResult{ .state = State::Failed };
                }

                RE::BGSObjectInstance objectInstance(job.weapon, job.instanceData.get());
                RE::BSScrapArray<RE::IKeywordFormBase*> targetKeywords{};
                state.native.addItemToTargetKeywords(&objectInstance, &targetKeywords);
                if (targetKeywords.empty()) {
                    finishTerminal(state, State::Failed, "weaponInstanceProducedNoTargetKeywords");
                    return StepResult{ .state = State::Failed };
                }
                std::int32_t role = kWeaponAnimationRole;
                std::int32_t priority = kIoTaskPriority;
                state.native.requestAnimationSubGraph(
                    RE::PlayerCharacter::GetSingleton(),
                    manager,
                    &role,
                    &targetKeywords,
                    &priority,
                    &job.subgraphHandles,
                    &job.subgraphIdentifiers);
                if (job.subgraphHandles.empty() || job.subgraphIdentifiers.empty()) {
                    finishTerminal(state, State::Failed, "exactWeaponSubgraphRequestReturnedEmpty");
                    return StepResult{ .state = State::Failed };
                }
                job.phase = State::LoadingWeaponSubgraph;
                job.phaseStartedAtMilliseconds = now;
                job.longLoadLogged = false;
                return StepResult{ .state = job.phase };
            }

            if (job.phase == State::LoadingWeaponSubgraph) {
                if (elapsed >= kGraphLoadTimeoutMilliseconds) {
                    finishTerminal(state, State::Failed, "weaponSubgraphLoadTimedOut");
                    return StepResult{ .state = State::Failed };
                }
                auto* holder = graphHolder(job);
                if (!holder || !holder->animationGraphManager) {
                    finishTerminal(state, State::Failed, "backgroundManagerLost");
                    return StepResult{ .state = State::Failed };
                }
                std::int32_t priority = kIoTaskPriority;
                if (!state.native.isAnimationSubGraphLoaded(
                        &holder->animationGraphManager,
                        &job.subgraphHandles,
                        &priority)) {
                    return StepResult{ .state = job.phase };
                }
                if (!copyExactAnimationPaths(state)) {
                    finishTerminal(state, State::Failed, "exactAnimationFileListUnavailable");
                    return StepResult{ .state = State::Failed };
                }
                job.animationPathIndex = 0;
                job.phase = State::LoadingClip;
                job.phaseStartedAtMilliseconds = now;
                job.longLoadLogged = false;
                RDX_LOG_INFO(Animation,
                    "Authored animation preharvest exact subgraph ready weapon={:08X} subgraph={:016X} files={}",
                    job.weaponFormId,
                    job.subgraphIdentifier,
                    job.animationPathCount);
                return StepResult{ .state = job.phase };
            }

            if (job.phase == State::LoadingClip) {
                if (job.animationPathIndex >= job.animationPathCount) {
                    finishTerminal(state, State::Completed, "allExactAnimationFilesSampled");
                    return StepResult{ .state = State::Completed };
                }
                if (!job.clipResource.entry) {
                    job.currentClipPath =
                        job.animationPaths[job.animationPathIndex];
                    RE::BSFixedString path{ job.currentClipPath.data() };
                    if (!state.native.loadAnimationResource(
                            &path, &job.clipResource) ||
                        !job.clipResource.entry) {
                        rejectCurrentClip(state, "directClipLoadRequestFailed");
                        return StepResult{ .state = job.phase };
                    }
                    job.phaseStartedAtMilliseconds = now;
                    job.longLoadLogged = false;
                    return StepResult{ .state = job.phase };
                }
                if (elapsed >= kClipLoadTimeoutMilliseconds) {
                    rejectCurrentClip(state, "directClipLoadTimedOut");
                    return StepResult{ .state = job.phase };
                }
                std::uint32_t resourceFlags = 0;
                if (!native_memory::tryReadField(
                        job.clipResource.entry,
                        kAnimationResourceFlagsOffset,
                        resourceFlags)) {
                    rejectCurrentClip(state, "directClipResourceLayoutUnavailable");
                    return StepResult{ .state = job.phase };
                }
                if (!weapon_animation_preharvest_policy::animationResourceCanExposeData(
                        resourceFlags)) {
                    return StepResult{ .state = job.phase };
                }
                if (!prepareClipWork(
                        state, allowedNodeNames, allowedNodeNameCount)) {
                    rejectCurrentClip(state, "animationBindingOrSkeletonMappingInvalid");
                    return StepResult{ .state = job.phase };
                }
                if (state.clip.targetCount == 0) {
                    ++state.stats.clipsSampled;
                    RDX_LOG_DEBUG(Animation,
                        "Authored animation preharvest skipped sampling clip '{}' (no matching Weapon-descendant scene bones)",
                        job.currentClipPath.data());
                    advancePastCurrentClip(state);
                    return StepResult{ .state = job.phase };
                }
                job.phase = State::SamplingClip;
                job.phaseStartedAtMilliseconds = now;
                job.longLoadLogged = false;
                return StepResult{ .state = job.phase };
            }

            if (job.phase == State::SamplingClip) {
                if (!outGroups || maxGroups == 0) {
                    finishTerminal(state, State::Failed, "authoredGroupOutputUnavailable");
                    return StepResult{ .state = State::Failed };
                }
                if (!sampleClipBatch(state)) {
                    rejectCurrentClip(state, "nativeTrackSamplingOrHierarchyCompositionFailed");
                    return StepResult{ .state = job.phase };
                }
                if (state.clip.nextSample < state.clip.sampleCount) {
                    return StepResult{ .state = job.phase };
                }
                const auto groupCount =
                    finishSampledClip(state, outGroups, maxGroups);
                advancePastCurrentClip(state);
                return StepResult{
                    .state = job.phase,
                    .groupsProduced = groupCount,
                };
            }

            finishTerminal(state, State::Failed, "unknownPreharvestState");
            return StepResult{ .state = State::Failed };
        }
    }

    StepResult step(
        const std::uint32_t weaponFormId,
        const std::uint64_t weaponGenerationKey,
        const char* const* allowedNodeNames,
        const std::uint32_t allowedNodeNameCount,
        weapon_clip_stroke::AuthoredStrokeGroup* outGroups,
        const std::uint32_t maxGroups) noexcept
    {
        auto& state = runtime();
        if (!claimOrValidateThread(state)) {
            return StepResult{ .state = State::Failed };
        }
        if (state.resetRequested.exchange(
                false, std::memory_order_acq_rel)) {
            resetRuntimeJob(state);
        }
        if (weaponFormId == 0 || weaponGenerationKey == 0) {
            resetRuntimeJob(state);
            return {};
        }
        if (!allowedNodeNames || allowedNodeNameCount == 0) {
            return StepResult{ .state = state.job.phase };
        }
        if (!resolveNativeFunctions(state)) {
            return StepResult{ .state = State::Failed };
        }

        if (state.job.weaponFormId != weaponFormId ||
            state.job.weaponGenerationKey != weaponGenerationKey) {
            resetRuntimeJob(state);
            if (!startJob(state, weaponFormId, weaponGenerationKey)) {
                // ROCK's committed scene generation can precede the actor's
                // equipped-item/process view by a few frames. An unavailable
                // initial context is pending, not a terminal failure; leave
                // identity clear so the same generation retries next frame.
                return StepResult{
                    .state = state.job.phase == State::Failed
                        ? State::Failed
                        : State::Idle,
                };
            }
        }
        return progressJob(
            state,
            allowedNodeNames,
            allowedNodeNameCount,
            outGroups,
            maxGroups);
    }

    void reset() noexcept
    {
        auto& state = runtime();
        state.resetRequested.store(true, std::memory_order_release);
        const DWORD ownerThreadId = state.ownerThreadId.load(
            std::memory_order_acquire);
        if (ownerThreadId == 0 || ownerThreadId != GetCurrentThreadId()) {
            // An inactive pre-frame reset must not claim ownership from the
            // future ROCK callback. An active cross-thread reset is consumed
            // by that established owner before its next native operation.
            return;
        }
        state.resetRequested.store(false, std::memory_order_release);
        resetRuntimeJob(state);
    }

    Stats snapshotStats() noexcept
    {
        return runtime().stats;
    }
}
