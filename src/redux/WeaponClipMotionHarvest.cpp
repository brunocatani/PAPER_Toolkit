#include "redux/WeaponClipMotionHarvest.h"

#include "ReduxLog.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>

namespace redux::weapon_clip_motion_harvest
{
    namespace
    {
        /*
         * All offsets below were verified against the FO4VR binary on
         * 2026-07-03 (Ghidra decompiles + VR address library cross-check);
         * see docs/research/2026-07-03-baked-animation-motion-extraction.md
         * Addendum 2. Chain: WeaponAnimationGraphManagerHolder →
         * BSAnimationGraphManager → BShkbAnimationGraph → inline hkbCharacter
         * → hkbCharacterSetup → { hkaSkeleton, hkbAnimationBindingSet }.
         */

        // WeaponAnimationGraphManagerHolder (ctor disassembly, 0x140812c20):
        // +0x00 IAnimationGraphManagerHolder vtable, +0x08 second base
        // vtable, +0x10 refcount, +0x18 =
        // BSTSmartPointer<BSAnimationGraphManager>. (+0x8 is a vtable
        // pointer — plausible-looking, which is why a wrong read here fails
        // one hop later at the graphs array.)
        constexpr std::uintptr_t kHolderManagerOffset = 0x18;

        // BSAnimationGraphManager (ctor decompile, 0x14168f4f0): graphs are a
        // BSTSmallArray of BSTSmartPointer<BShkbAnimationGraph> — capacity
        // dword at +0x40 (bit31 set = inline storage), storage at +0x48
        // (inline entry or heap pointer), active graph index at +0xD8.
        constexpr std::uintptr_t kManagerGraphsCapacityOffset = 0x40;
        constexpr std::uintptr_t kManagerGraphsStorageOffset = 0x48;
        constexpr std::uintptr_t kManagerActiveGraphOffset = 0xD8;
        constexpr std::uint32_t kGraphsInlineStorageFlag = 0x8000'0000u;

        // BShkbAnimationGraph: hkbCharacter is INLINE at +0x1C8 — the graph
        // ctor (0x1416a3150) constructs it at this[1].field_0x50 with a
        // 0x178-byte Ghidra struct (0x178 + 0x50), matching the binding-set
        // builder receiving graph+0x1C8 as its owner (hkBaseObject_data base
        // is +0x8, so its field_0x1c0 is absolute 0x1C8).
        constexpr std::uintptr_t kGraphCharacterOffset = 0x1C8;

        // hkbCharacter: +0x78 hkbCharacterSetup*, +0x90 binding-set override
        // (engine getter 0x141902dc0 prefers the override).
        constexpr std::uintptr_t kCharacterSetupOffset = 0x78;
        constexpr std::uintptr_t kCharacterBindingSetOverrideOffset = 0x90;

        // hkbCharacterSetup: +0x20 m_animationSkeleton (the skeleton the clip
        // tracks map to — for the weapon graph, the weapon rig), +0x38
        // m_animationBindingSet.
        constexpr std::uintptr_t kSetupAnimationSkeletonOffset = 0x20;
        constexpr std::uintptr_t kSetupBindingSetOffset = 0x38;

        // hkbAnimationBindingSet: +0x10 bindings data
        // (hkbAnimationBindingWithTriggers*[]), +0x18 int count.
        constexpr std::uintptr_t kBindingSetDataOffset = 0x10;
        constexpr std::uintptr_t kBindingSetCountOffset = 0x18;
        constexpr std::int32_t kMaxPlausibleBindingCount = 4096;

        // hkbAnimationBindingWithTriggers (0x30-byte hkReferencedObject):
        // +0x8 is the memSizeAndFlags/refCount header — the binding pointer
        // is at +0x10 (crash-log verified; reading +0x8 dereferences the
        // 0xFFFF0001 refcount pattern).
        constexpr std::uintptr_t kBindingWithTriggersBindingOffset = 0x10;

        // hkaAnimationBinding members — corrected 2026-07-04 against the
        // engine's own clip validator (hkbClipGenerator vtable slot 14,
        // 0x14192d7a0): +0x18 animation (duration read at anim+0x14 confirms),
        // +0x20/+0x28 transform track-to-bone shorts (the validator requires
        // this array to be identity when partitions are used), +0x40/+0x48 is
        // the PARTITION indices array — the earlier hook-era claim that the
        // track map lived there was a misidentification (weapon clips showing
        // "trackToBone=1" were reporting one partition).
        constexpr std::uintptr_t kBindingAnimationOffset = 0x18;
        constexpr std::uintptr_t kBindingTrackToBoneDataOffset = 0x20;
        constexpr std::uintptr_t kBindingTrackToBoneCountOffset = 0x28;
        // Partition-mapped clips (the weapon clips: empty track map, one
        // partition): tracks cover the bones of the listed skeleton
        // partitions in order. Engine validator 0x141a0c4e0: binding+0x40
        // partition indices (ushort) / +0x48 count, checked against the
        // skeleton's partition count at skeleton+0x80 (partitions hkArray
        // data at +0x78). Partition entry = { name*, int16 startBone,
        // int16 numBones }, 0x10 stride.
        constexpr std::uintptr_t kBindingPartitionIndicesOffset = 0x40;
        constexpr std::uintptr_t kBindingPartitionCountOffset = 0x48;
        constexpr std::uintptr_t kSkeletonPartitionsDataOffset = 0x78;
        constexpr std::uintptr_t kSkeletonPartitionsCountOffset = 0x80;
        constexpr std::uintptr_t kSkeletonPartitionStride = 0x10;
        constexpr std::uintptr_t kPartitionStartBoneOffset = 0x8;
        constexpr std::uintptr_t kPartitionNumBonesOffset = 0xA;

        // hkaSkeleton members (bones array of 0x10-byte hkaBone entries with
        // the name char* at +0; low pointer bit is an engine flag).
        // parentIndices (int16 per bone) sits at +0x18/+0x20, consistent
        // with the verified bones array at +0x28.
        constexpr std::uintptr_t kSkeletonParentIndicesOffset = 0x18;
        constexpr std::uintptr_t kSkeletonParentIndicesCountOffset = 0x20;
        constexpr std::uintptr_t kSkeletonBonesDataOffset = 0x28;
        constexpr std::uintptr_t kSkeletonBonesCountOffset = 0x30;
        constexpr std::uintptr_t kSkeletonBoneStride = 0x10;

        // hkaAnimation members.
        constexpr std::uintptr_t kAnimationDurationOffset = 0x14;
        constexpr std::uintptr_t kAnimationTrackCountOffset = 0x18;

        // hkaSplineCompressedAnimation internals (playback sampler
        // disassembly 0x141F71B70, block resolver 0x142051280): +0x3C
        // numBlocks, +0x40 maxFramesPerBlock (the sampler divides by
        // maxFramesPerBlock-1 unguarded), hkArrays m_blockOffsets (+0x58
        // data / +0x60 count), m_floatBlockOffsets (+0x68 / +0x70 — read
        // unconditionally by the sampler even with zero float tracks) and
        // m_data (+0x98 / +0xA0). The sampler guards none of these — it
        // assumes the clip is loaded because it only samples playing clips.
        // Binding-set STUBS carry token allocations here (m_data ~0x20
        // bytes), so residency is gated on the array SIZES: a real clip's
        // mask stream needs at least one byte per track.
        constexpr std::uintptr_t kSplineNumBlocksOffset = 0x3C;
        constexpr std::uintptr_t kSplineMaxFramesPerBlockOffset = 0x40;
        constexpr std::uintptr_t kSplineBlockOffsetsOffset = 0x58;
        constexpr std::uintptr_t kSplineBlockOffsetsCountOffset = 0x60;
        constexpr std::uintptr_t kSplineFloatBlockOffsetsOffset = 0x68;
        constexpr std::uintptr_t kSplineFloatBlockOffsetsCountOffset = 0x70;
        constexpr std::uintptr_t kSplineDataBaseOffset = 0x98;
        constexpr std::uintptr_t kSplineDataSizeOffset = 0xA0;
        // Full-pose decode buffer: covers the 95-bone player rigs with
        // slack; targets on higher track indices are skipped rather than
        // silently truncated.
        constexpr std::uint32_t kSampleBufferTracks = 160;

        /*
         * hkbClipGenerator (0x160 bytes; clone at 0x14192d950 allocates it).
         * Lifecycle slots, raw-disassembly verified 2026-07-05 (vtable read
         * from the unpacked exe + body disasm of each target):
         *  - +0x38 activate   (0x14192CA40): backs up m_triggers +0x98 into
         *    +0xD8, constructs the hkaDefaultAnimationControl and stores it
         *    at +0xD0, caches the binding at +0xE8 — after it returns the
         *    payload is resident (control+0x38 = hkaAnimationBinding);
         *  - +0x40 update     (0x14192D0D0): per-frame time/trigger step;
         *  - +0x50 deactivate (0x14192D510): decrefs and NULLS +0xD0,
         *    restores +0x98 from the +0xD8 backup.
         * The harvest hooks ACTIVATE with an atomic pointer swap and runs
         * after the original. A 2026-07-04..05 regression had this hook on
         * +0x50 (then believed to be Bethesda's binding-install override):
         * the shim read [clip+0xD0] right after deactivate nulled it, so
         * the activation path silently never harvested or dumped markers —
         * the equip-time walk masked it.
         */
        constexpr std::uintptr_t kClipGeneratorVtableModuleOffset = 0x2E0FB38;
        constexpr std::uintptr_t kClipGeneratorActivateSlotOffset = 0x38;
        constexpr std::uintptr_t kClipGeneratorUpdateSlotOffset = 0x40;
        constexpr std::uintptr_t kClipGeneratorDeactivateSlotOffset = 0x50;
        constexpr std::uintptr_t kClipGeneratorLoadedBindingOffset = 0xD0;
        /*
         * hkbClipGenerator native scrub interface, raw-disassembly verified
         * 2026-07-05 with 2-3 agreeing sources per member (update
         * 0x14192d0d0, computeLocalTime 0x14192f560, ctor 0x14192c520; see
         * docs/research/2026-07-05-clip-scrub-hkbClipGenerator-verified-
         * offsets.md): +0xA4/+0xA8 crop start/end amounts (local-time
         * seconds), +0xB8 float m_userControlledTimeFraction (engine clamps
         * to [0,1] while mode == 2), +0xBE byte m_mode (2 = user-controlled:
         * localTime = fraction * croppedDuration + cropStart, timestep-
         * independent; the trigger walker and echo/loop logic are skipped
         * entirely), +0x140 float m_localTime, +0x148 float
         * m_previousUserControlledTimeFraction — MUST be seeded together
         * with +0xB8 before flipping the mode, or the first scrubbed update
         * derives a bogus previous local time.
         */
        constexpr std::uintptr_t kClipGeneratorCropStartOffset = 0xA4;
        constexpr std::uintptr_t kClipGeneratorCropEndOffset = 0xA8;
        constexpr std::uintptr_t kClipGeneratorUserFractionOffset = 0xB8;
        constexpr std::uintptr_t kClipGeneratorModeOffset = 0xBE;
        constexpr std::uintptr_t kClipGeneratorLocalTimeOffset = 0x140;
        constexpr std::uintptr_t kClipGeneratorPrevUserFractionOffset = 0x148;
        constexpr std::uint8_t kClipModeUserControlled = 2;
        // hkbClipGenerator::m_animationName (hkStringPtr — mask low bit).
        // Raw disassembly 0x141939911: [clip+0x90] & ~1 formatted into
        // "Animation loaded directly from clip's animationName".
        constexpr std::uintptr_t kClipGeneratorAnimationNameOffset = 0x90;
        constexpr std::uintptr_t kLoadedBindingWrapperBindingOffset = 0x38;
        // hkaAnimation vtable slot 5 — the engine's own playback sampler
        // ("TtSampleSpline" profile marker, 0x141F71B70): sampleTracks(
        //   float time, uint32 transformTracks, hkQsTransform* out,
        //   uint32 floatTracks, float* floatsOut). Decodes tracks
        // 0..transformTracks-1 sequentially from the per-block mask streams
        // inside m_data. Slot 6 (per-track sampling through
        // m_transformOffsets) is DEAD code in FO4 — that array is never
        // populated, loaded or stub, which is what crashed the 2026-07-04
        // session and what made every clip look non-resident.
        constexpr std::size_t kSampleTracksSlot = 5;

        /*
         * Stage-marker offsets (phase 4, verified 2026-07-05 via the
         * destructor chain + trigger walker, two raw-disassembly sources):
         *  - ~hkaAnimation (0x141A31580) walks m_annotationTracks at
         *    +0x28 (data) / +0x30 (count) with stride 0x18 and frees
         *    count*0x18 bytes; +0x20 is the refcount-released
         *    extractedMotion, consistent with the +0x14/+0x18 members
         *    verified earlier.
         *  - ~hkaAnnotationTrack (0x141A31650) releases the hkStringPtr
         *    trackName at +0x0, walks annotations at +0x8/+0x10 with
         *    stride 0x10, and releases each annotation's hkStringPtr text
         *    at element+0x8 (element+0x0 is the float time — never
         *    destructed).
         *  - The hkbClipGenerator trigger walker (0x14192F160, called from
         *    update/vfunction9) reads m_triggers at clip+0x98 (refcounted
         *    object with hkArray at +0x10 data / +0x18 count) and fires
         *    triggers of stride 0x20: +0x0 float localTime, +0x8 int32
         *    eventId (graph-local), +0x10 payload.
         * The reflection name pool independently confirms the member names
         * ("annotationTracks", "triggers", "hkaAnnotationTrackAnnotation").
         */
        constexpr std::uintptr_t kAnimationAnnotationTracksDataOffset = 0x28;
        constexpr std::uintptr_t kAnimationAnnotationTracksCountOffset = 0x30;
        constexpr std::uintptr_t kAnnotationTrackStride = 0x18;
        constexpr std::uintptr_t kAnnotationTrackNameOffset = 0x0;
        constexpr std::uintptr_t kAnnotationTrackAnnotationsDataOffset = 0x8;
        constexpr std::uintptr_t kAnnotationTrackAnnotationsCountOffset = 0x10;
        constexpr std::uintptr_t kAnnotationStride = 0x10;
        constexpr std::uintptr_t kAnnotationTimeOffset = 0x0;
        constexpr std::uintptr_t kAnnotationTextOffset = 0x8;
        constexpr std::uintptr_t kClipGeneratorTriggersOffset = 0x98;
        constexpr std::uintptr_t kTriggerArrayDataOffset = 0x10;
        constexpr std::uintptr_t kTriggerArrayCountOffset = 0x18;
        constexpr std::uintptr_t kTriggerStride = 0x20;
        constexpr std::uintptr_t kTriggerLocalTimeOffset = 0x0;
        constexpr std::uintptr_t kTriggerEventIdOffset = 0x8;
        /*
         * eventId -> name chain, raw-disassembly verified 2026-07-05 (two
         * agreeing sources per hop; see docs/research 2026-07-05 note):
         *  - hkbContext+0x10 = hkbBehaviorGraph* (the engine's own trigger
         *    walker 0x14192f160 reads [ctx+0x10]+0xC8 and indexes its
         *    +0x40/+0x48 arrays with the trigger eventId = exactly
         *    hkbBehaviorGraphData::eventInfos); gated by an exact vtable
         *    match, so a context-layout surprise degrades into "no name".
         *  - graph+0xC8 = m_data (finish-ctor 0x1417c3c80 preserves it as a
         *    serialized ref between pseudoRandomGenerator +0xB8, seed 0x529,
         *    and the reset runtime template +0xD0).
         *  - data+0x68 = m_stringData (two static hkClassMember arrays,
         *    0x142e26f50 / 0x142e27040-4*0x28, registration objectSize 0x70).
         *  - stringData+0x10/+0x18 = m_eventNames hkArray<hkStringPtr>
         *    (reflection record 0x142e41000 + ctor 0x141937eb0).
         * eventIds are GRAPH-LOCAL indices into this array.
         */
        constexpr std::uintptr_t kContextBehaviorGraphOffset = 0x10;
        constexpr std::uintptr_t kBehaviorGraphVtableModuleOffset = 0x2E04848;
        constexpr std::uintptr_t kBehaviorGraphDataOffset = 0xC8;
        constexpr std::uintptr_t kGraphDataStringDataOffset = 0x68;
        constexpr std::uintptr_t kStringDataEventNamesDataOffset = 0x10;
        constexpr std::uintptr_t kStringDataEventNamesCountOffset = 0x18;
        constexpr std::int32_t kMaxPlausibleEventNames = 4096;
        /*
         * Annotation tracks are one-per-transform-track (per bone), so the
         * cap must cover full 1P rigs. The original cap of 64 silently
         * skipped the whole annotation walk on every real clip (they report
         * 94..123 tracks) — the 2026-07-05 "VR clips carry no annotations"
         * summaries were reporting an unexecuted loop, not empty tracks.
         */
        constexpr std::int32_t kMaxPlausibleAnnotationTracks = 512;
        constexpr std::int32_t kMaxPlausibleAnnotations = 256;
        constexpr std::int32_t kMaxPlausibleTriggers = 128;
        /*
         * Per-weapon-generation line budget for the stage-marker dump. The
         * processed-binding registry (128 slots) can overflow on weapons
         * with hundreds of bindings, after which activations re-dump every
         * reload; the budget keeps the log bounded no matter what. Reset
         * where the registry resets (new walk generation).
         */
        constexpr std::uint32_t kStageMarkerLogBudgetPerGeneration = 160;

        constexpr float kMinClipDurationSeconds = 0.01f;
        constexpr float kMaxClipDurationSeconds = 300.0f;
        constexpr std::int32_t kMaxPlausibleTrackCount = 512;
        constexpr std::int32_t kMaxPlausibleBoneCount = 4096;
        // Sized for a burst of consecutive weapon clips on an actor graph
        // (several groups per clip between per-frame drains).
        constexpr std::size_t kQueueCapacity = 64;
        // Bindings sampled per stepHarvest call; bounds the per-frame cost of
        // the at-equip walk (each binding = up to kMaxTracksPerClip tracks x
        // kClipSampleCount engine sampler calls).
        constexpr std::int32_t kBindingsPerStep = 6;

        // hkQsTransform: vec4 translate, quaternion (x,y,z,w), vec4 scale.
        struct HkQsTransform
        {
            float translate[4];
            float rotate[4];
            float scale[4];
        };
        static_assert(sizeof(HkQsTransform) == 48);

        // sampleTracks(this, time, transformTrackCount, transformsOut,
        // floatTrackCount, floatsOut) — MSVC x64: time lands in XMM1.
        using SampleTracks_t = void (*)(void*, float, std::uint32_t, HkQsTransform*, std::uint32_t, float*);

        std::mutex s_queueMutex;
        std::array<weapon_clip_stroke::AuthoredStrokeGroup, kQueueCapacity> s_queue{};
        std::uint32_t s_queueCount = 0;

        /*
         * Harvest scratch: at 32 tracks / 16 groups per clip these buffers
         * are far too large for engine-thread stacks (the hook fires on the
         * animation thread). Shared by the hook and walk paths and
         * serialized by their own mutex — the hook already holds s_hookMutex
         * through harvestBinding, the walk path does not. Lock order is
         * always scratch → queue; neither path takes s_hookMutex while
         * holding the scratch lock.
         */
        std::mutex s_harvestScratchMutex;
        std::array<std::int16_t, weapon_clip_stroke::kMaxTracksPerClip> s_scratchTrackIndices{};
        std::array<weapon_clip_stroke::TrackSamples, weapon_clip_stroke::kMaxTracksPerClip> s_scratchTracks{};
        std::array<weapon_clip_stroke::AuthoredStrokeGroup, weapon_clip_stroke::kMaxGroupsPerClip> s_scratchGroups{};

        std::atomic<std::uint64_t> s_bindingsSeen{ 0 };
        std::atomic<std::uint64_t> s_bindingsHarvested{ 0 };
        std::atomic<std::uint64_t> s_bindingsNoTargets{ 0 };
        std::atomic<std::uint64_t> s_groupsQueued{ 0 };
        std::atomic<std::uint64_t> s_groupsDropped{ 0 };
        std::atomic<std::uint64_t> s_skippedNonSpline{ 0 };
        std::atomic<std::uint64_t> s_walksCompleted{ 0 };
        // Bail-reason counters: which harvestBinding gate rejected a binding
        // (a binding that passes all gates lands in harvested/noTargets).
        std::atomic<std::uint64_t> s_bailAnimationPtr{ 0 };
        std::atomic<std::uint64_t> s_bailClipParams{ 0 };
        std::atomic<std::uint64_t> s_bailTrackMap{ 0 };
        std::atomic<std::uint64_t> s_bailBoneCount{ 0 };
        std::atomic<std::uint64_t> s_bailSampler{ 0 };
        std::atomic<std::uint64_t> s_bailSplineData{ 0 };
        // Hook telemetry: every shim entry, entries passing the character
        // filter, and entries that reached sampling. fires==0 means the
        // engine never dispatched the hooked slot; fires>0 with matched==0
        // means the filter rejects the characters the engine passes.
        std::atomic<std::uint64_t> s_hookFires{ 0 };
        std::atomic<std::uint64_t> s_hookActivations{ 0 };
        std::atomic<std::uint32_t> s_hookUnmatchedLogs{ 0 };
        constexpr std::uint32_t kMaxHookUnmatchedLogs = 3;

        /*
         * Clip-activation hook targets. The hook fires on the engine's graph
         * update thread for EVERY actor's clip generators, so it harvests
         * only when the character it receives is one of the registered
         * candidate-graph characters, and node names are COPIED here so the
         * hook never touches scene-graph memory. Guarded by s_hookMutex
         * (writers: main thread, rare; reader: hook, rare — clip activations
         * are sparse).
         */
        constexpr std::size_t kMaxHookCharacters = 8;
        constexpr std::size_t kMaxHookNodeNames = 64;
        constexpr std::size_t kMaxHookNodeNameLength = 64;
        std::mutex s_hookMutex;
        std::array<std::uintptr_t, kMaxHookCharacters> s_hookCharacters{};
        std::uint32_t s_hookCharacterCount = 0;
        std::array<std::array<char, kMaxHookNodeNameLength>, kMaxHookNodeNames> s_hookNodeNames{};
        std::array<const char*, kMaxHookNodeNames> s_hookNodeNamePointers{};
        std::uint32_t s_hookNodeNameCount = 0;

        /*
         * Bindings terminally processed this weapon generation (harvested or
         * proven target-less): re-walk passes and repeat clip activations
         * skip them, so a part cannot be overwritten by a different clip on
         * every 2s pass. Bails (e.g. payload not loaded yet) are NOT marked
         * and retry naturally. Guarded by s_hookMutex; the *Locked helpers
         * assume the caller holds it (the hook already does).
         *
         * Dedup is per PROVENANCE: a binding the walk processed from the
         * merely-LOADED set may still be re-harvested once when the weapon
         * actually ACTIVATES it (the hook) — activation-provenance strokes
         * are the weapon's own animation and outrank walk fallback data, so
         * the upgrade must not be swallowed by the walk's earlier pass. An
         * activation-processed binding is terminal for both paths.
         */
        struct ProcessedBinding
        {
            std::uintptr_t binding{ 0 };
            bool activated{ false };
        };
        std::array<ProcessedBinding, 128> s_processedBindings{};
        std::uint32_t s_processedBindingCount = 0;
        // Guarded by s_hookMutex like the registry above.
        std::uint32_t s_stageMarkerLogBudget = kStageMarkerLogBudgetPerGeneration;

        /*
         * Clip-scrub sweep probe (milestone 1 of clip scrub mode, INI
         * bClipScrubSweepTest): the first activating clip whose animation
         * name contains the filter is flipped into Havok's user-controlled
         * mode and its time fraction is ramped 0 -> 1 over the configured
         * seconds; the saved mode byte is restored at ramp end or on
         * deactivation. Log-only — validates in-game that an engine-scrubbed
         * reload moves only the weapon rig while FRIK keeps the arms on the
         * controllers.
         *
         * Thread model: the config fields are written by the main thread
         * and read by the activation path, both under s_hookMutex. The
         * active-sweep fields are seeded inside the activate shim (mutex
         * held) BEFORE s_sweepClip is published with release order; after
         * that only the update/deactivate shims of that same clip touch
         * them (one clip's graph updates on one thread at a time), reading
         * s_sweepClip with acquire order. The update shim's cost for every
         * other clip in the game is a single relaxed load and compare.
         * One sweep at a time; disabling the INI key mid-sweep lets the
         * active sweep finish on its own (it restores itself).
         */
        bool s_sweepConfigEnabled = false;
        float s_sweepConfigSeconds = 6.0f;
        std::array<char, 48> s_sweepConfigFilter{};

        std::atomic<std::uintptr_t> s_sweepClip{ 0 };
        float s_sweepElapsedSeconds = 0.0f;
        float s_sweepSeconds = 6.0f;
        float s_sweepClipDuration = 0.0f;
        std::uint8_t s_sweepSavedMode = 0;
        std::uint32_t s_sweepNextLogDecile = 0;
        std::array<char, 64> s_sweepClipName{};

        [[nodiscard]] bool bindingProcessedLocked(std::uintptr_t binding, bool fromActivation)
        {
            for (std::uint32_t i = 0; i < s_processedBindingCount; ++i) {
                if (s_processedBindings[i].binding == binding) {
                    return fromActivation ? s_processedBindings[i].activated : true;
                }
            }
            return false;
        }

        void markBindingProcessedLocked(std::uintptr_t binding, bool fromActivation)
        {
            for (std::uint32_t i = 0; i < s_processedBindingCount; ++i) {
                if (s_processedBindings[i].binding == binding) {
                    s_processedBindings[i].activated |= fromActivation;
                    return;
                }
            }
            if (s_processedBindingCount < s_processedBindings.size()) {
                s_processedBindings[s_processedBindingCount++] = ProcessedBinding{ binding, fromActivation };
            }
        }

        // Detailed bail dumps per walk (main thread; reset with the cursor)
        // so a failing binding is identifiable without flooding the log.
        constexpr std::uint32_t kMaxBindingDetailLogsPerWalk = 8;
        std::uint32_t s_bindingDetailLogs = 0;

        // Walk cursor (main thread only). No engine pointers are stored —
        // the chain is re-resolved from the holder on every step.
        std::uint32_t s_walkFormId = 0;
        std::uint64_t s_walkGenerationKey = 0;
        // Cursor spans every graph in the manager's array (weapon clips do
        // not live on the active graph).
        std::uint32_t s_walkGraphIndex = 0;
        std::int32_t s_walkBindingIndex = 0;
        // Data pointer of the binding set the cursor indexes into; a change
        // (graph swap / candidate switch) restarts the current graph's walk.
        std::uintptr_t s_walkBindingsData = 0;
        // Bindings visited across the whole walk; a pass that saw none keeps
        // the walk pending (sets may still be filling at equip).
        std::uint32_t s_walkBindingsVisited = 0;
        bool s_walkDone = false;
        // Pass bookkeeping for periodic re-walks (clip payloads stream in
        // only while playing): completion is logged for the first pass and
        // for any pass that harvested something new.
        std::uint32_t s_walkPassIndex = 0;
        std::uint64_t s_walkPassStartHarvested = 0;
        // Deepest chain hop reached by the most recent resolve attempt;
        // reported by the caller when a walk gives up so the failing stage
        // is visible in the log instead of a generic timeout.
        const char* s_lastResolveStage = "none";

        // Coarse pointer plausibility gate for values read out of engine
        // objects; rejects null, refcount headers, and other small integers
        // before they are dereferenced.
        [[nodiscard]] bool plausiblePointer(std::uintptr_t value)
        {
            return value > 0x10000 && value < 0x0000'8000'0000'0000ull;
        }

        [[nodiscard]] std::uintptr_t moduleRelative(std::uintptr_t address)
        {
            const auto base = REL::Module::get().base();
            return address >= base ? address - base : address;
        }

        // Module-relative vtable of a heap object (0 when the pointer is
        // implausible); a rebased value can be looked up directly in the
        // binary to identify the object's real runtime type.
        [[nodiscard]] std::uintptr_t objectVtableRel(std::uintptr_t object)
        {
            return plausiblePointer(object) ? moduleRelative(*reinterpret_cast<const std::uintptr_t*>(object)) : 0;
        }

        void logBindingBail(
            const char* reason,
            std::uintptr_t binding,
            std::uintptr_t animation,
            float duration,
            std::int32_t trackCount,
            std::int32_t trackToBoneCount,
            std::int32_t boneCount)
        {
            if (s_bindingDetailLogs >= kMaxBindingDetailLogsPerWalk) {
                return;
            }
            ++s_bindingDetailLogs;
            RDX_LOG_WARN(Weapon,
                "WeaponClipMotionHarvest: binding bail [{}] binding={:#x}(vt+{:#x}) anim={:#x}(vt+{:#x}) duration={} trackCount={} trackToBone={} bones={}",
                reason,
                binding,
                objectVtableRel(binding),
                animation,
                objectVtableRel(animation),
                duration,
                trackCount,
                trackToBoneCount,
                boneCount);
        }

        const char* skeletonBoneName(std::uintptr_t skeleton, std::int32_t boneIndex)
        {
            const auto bonesData = *reinterpret_cast<std::uintptr_t*>(skeleton + kSkeletonBonesDataOffset);
            if (!plausiblePointer(bonesData)) {
                return nullptr;
            }
            const auto entry = bonesData + static_cast<std::uintptr_t>(boneIndex) * kSkeletonBoneStride;
            const auto namePtr = *reinterpret_cast<std::uintptr_t*>(entry) & ~static_cast<std::uintptr_t>(1);
            return plausiblePointer(namePtr) ? reinterpret_cast<const char*>(namePtr) : nullptr;
        }

        // ASCII case-insensitive equality over `length` characters.
        bool namesEqualNoCase(const char* a, const char* b, std::size_t length)
        {
            for (std::size_t i = 0; i < length; ++i) {
                const auto ca = static_cast<unsigned char>(a[i]);
                const auto cb = static_cast<unsigned char>(b[i]);
                if (std::tolower(ca) != std::tolower(cb)) {
                    return false;
                }
            }
            return true;
        }

        // ASCII case-insensitive substring test (clip paths are ASCII).
        [[nodiscard]] bool nameContainsNoCase(const char* haystack, const char* needle)
        {
            if (!haystack || !needle || needle[0] == '\0') {
                return false;
            }
            const auto haystackLength = std::strlen(haystack);
            const auto needleLength = std::strlen(needle);
            if (needleLength > haystackLength) {
                return false;
            }
            for (std::size_t start = 0; start + needleLength <= haystackLength; ++start) {
                if (namesEqualNoCase(haystack + start, needle, needleLength)) {
                    return true;
                }
            }
            return false;
        }

        /*
         * A rig bone is a harvest target when it matches one of the weapon's
         * scene-node names: exact (case-insensitive, engine names are
         * case-insensitive) or with a ':N' instancing suffix on the node
         * side ('Bolt_Carrier' bone vs 'Bolt_Carrier:0' node). The filter is
         * what makes walking the ACTOR's graph safe — body-clip tracks never
         * match a weapon node name.
         */
        bool isHarvestTargetBone(const char* boneName, const char* const* allowedNodeNames, std::uint32_t allowedNodeNameCount)
        {
            if (!boneName || boneName[0] == '\0' || !allowedNodeNames) {
                return false;
            }
            const auto boneLength = std::strlen(boneName);
            for (std::uint32_t i = 0; i < allowedNodeNameCount; ++i) {
                const char* nodeName = allowedNodeNames[i];
                if (!nodeName) {
                    continue;
                }
                const auto nodeLength = std::strlen(nodeName);
                if (nodeLength < boneLength || !namesEqualNoCase(boneName, nodeName, boneLength)) {
                    continue;
                }
                if (nodeLength == boneLength || nodeName[boneLength] == ':') {
                    return true;
                }
            }
            return false;
        }

        // Copies a printable hkStringPtr (low-bit owned-flag convention)
        // into out; returns the length, 0 on any implausible byte.
        std::size_t copyHkStringPtr(std::uintptr_t stringField, char* out, std::size_t capacity)
        {
            out[0] = '\0';
            const auto pointer = stringField & ~static_cast<std::uintptr_t>(1);
            if (!plausiblePointer(pointer)) {
                return 0;
            }
            const char* chars = reinterpret_cast<const char*>(pointer);
            std::size_t length = 0;
            while (length < capacity - 1 && chars[length] != '\0') {
                const unsigned char c = static_cast<unsigned char>(chars[length]);
                if (c < 0x20 || c > 0x7E) {
                    out[0] = '\0';
                    return 0;
                }
                out[length] = chars[length];
                ++length;
            }
            out[length] = '\0';
            return length;
        }

        /*
         * Resolves a graph-local trigger eventId to its authored name via
         * graph->m_data->m_stringData->m_eventNames[id]. Every hop fails
         * closed into "no name"; the caller already vtable-gated the graph.
         */
        [[nodiscard]] std::int32_t behaviorGraphEventNameCount(std::uintptr_t behaviorGraph)
        {
            if (behaviorGraph == 0) {
                return -1;
            }
            const auto data = *reinterpret_cast<std::uintptr_t*>(behaviorGraph + kBehaviorGraphDataOffset);
            if (!plausiblePointer(data)) {
                return -1;
            }
            const auto stringData = *reinterpret_cast<std::uintptr_t*>(data + kGraphDataStringDataOffset);
            if (!plausiblePointer(stringData)) {
                return -1;
            }
            const auto count =
                *reinterpret_cast<std::int32_t*>(stringData + kStringDataEventNamesCountOffset);
            return (count >= 0 && count <= kMaxPlausibleEventNames) ? count : -1;
        }

        bool resolveGraphEventName(
            std::uintptr_t behaviorGraph,
            std::int32_t eventId,
            char* out,
            std::size_t capacity)
        {
            out[0] = '\0';
            if (behaviorGraph == 0 || eventId < 0) {
                return false;
            }
            const auto data = *reinterpret_cast<std::uintptr_t*>(behaviorGraph + kBehaviorGraphDataOffset);
            if (!plausiblePointer(data)) {
                return false;
            }
            const auto stringData = *reinterpret_cast<std::uintptr_t*>(data + kGraphDataStringDataOffset);
            if (!plausiblePointer(stringData)) {
                return false;
            }
            const auto count =
                *reinterpret_cast<std::int32_t*>(stringData + kStringDataEventNamesCountOffset);
            if (count <= 0 || count > kMaxPlausibleEventNames || eventId >= count) {
                return false;
            }
            const auto names =
                *reinterpret_cast<std::uintptr_t*>(stringData + kStringDataEventNamesDataOffset);
            if (!plausiblePointer(names)) {
                return false;
            }
            return copyHkStringPtr(
                       *reinterpret_cast<std::uintptr_t*>(names + static_cast<std::uintptr_t>(eventId) * 8),
                       out,
                       capacity) > 0;
        }

        /*
         * STAGE-MARKER DUMP (phase 4, Bruno 2026-07-05): the engine's own
         * named animation stage data, log-only for now — the data source
         * for future named-stage segmentation. Two kinds:
         *  - hkaAnimation annotation tracks: authored {time, text} markers
         *    baked into the clip (real names at exact times);
         *  - hkbClipGenerator triggers: {localTime, eventId} pairs the game
         *    itself fires as anim events (SoundPlay, EjectShellCasing,
         *    reloadComplete — in-game validated 2026-07-05). Event ids are
         *    graph-local; each is resolved through the clip's own graph
         *    (hkbBehaviorGraphStringData::eventNames).
         * Offsets are destructor/walker-verified (see the constants above);
         * every hop is plausibility-gated and degrades into a logged skip,
         * and every dumped clip emits exactly one INFO summary line with
         * raw structure counts. Runs on the activation path, deduped by
         * the processed-binding registry and hard-capped by
         * s_stageMarkerLogBudget per weapon generation.
         */
        void dumpClipStageMarkers(
            std::uintptr_t clipGenerator,
            std::uintptr_t binding,
            const char* clipName,
            std::uintptr_t behaviorGraph)
        {
            if (s_stageMarkerLogBudget == 0) {
                return;
            }
            const auto animation = *reinterpret_cast<std::uintptr_t*>(binding + kBindingAnimationOffset);
            if (!plausiblePointer(animation)) {
                --s_stageMarkerLogBudget;
                RDX_LOG_INFO(Weapon,
                    "STAGE-MARKER summary clip='{}': skipped, animation pointer implausible", clipName);
                return;
            }
            const float duration = *reinterpret_cast<float*>(animation + kAnimationDurationOffset);
            if (!std::isfinite(duration) || duration <= 0.0f || duration > kMaxClipDurationSeconds) {
                --s_stageMarkerLogBudget;
                RDX_LOG_INFO(Weapon,
                    "STAGE-MARKER summary clip='{}': skipped, duration {:.3f} implausible", clipName, duration);
                return;
            }

            const auto trackCount =
                *reinterpret_cast<std::int32_t*>(animation + kAnimationAnnotationTracksCountOffset);
            const auto trackData =
                *reinterpret_cast<std::uintptr_t*>(animation + kAnimationAnnotationTracksDataOffset);
            std::uint32_t annotationLines = 0;
            /*
             * Per-track names for filter-matching clips (sweep filter,
             * whether or not the sweep itself is enabled): annotation
             * tracks are one per transform track and their names are the
             * rig bone names — the cheap proof of whether hand bones are
             * addressable inside the animation data (authored hand-pose
             * harvest feasibility). Compact, several names per line,
             * budget-gated like every detail line.
             */
            const bool dumpTrackNames =
                s_sweepConfigFilter[0] != '\0' && nameContainsNoCase(clipName, s_sweepConfigFilter.data());
            char trackNameLine[224];
            std::size_t trackNameLineLength = 0;
            std::int32_t trackNameLineStart = 0;
            std::uint32_t trackNamesInLine = 0;
            if (trackCount > 0 && trackCount <= kMaxPlausibleAnnotationTracks && plausiblePointer(trackData)) {
                for (std::int32_t t = 0; t < trackCount; ++t) {
                    const auto track = trackData + static_cast<std::uintptr_t>(t) * kAnnotationTrackStride;
                    char trackName[64];
                    copyHkStringPtr(
                        *reinterpret_cast<std::uintptr_t*>(track + kAnnotationTrackNameOffset),
                        trackName,
                        sizeof(trackName));
                    if (dumpTrackNames) {
                        const char* printableName = trackName[0] != '\0' ? trackName : "?";
                        const auto nameLength = std::strlen(printableName);
                        if (trackNamesInLine == 8 ||
                            trackNameLineLength + nameLength + 2 >= sizeof(trackNameLine)) {
                            if (s_stageMarkerLogBudget > 1) {
                                --s_stageMarkerLogBudget;
                                RDX_LOG_INFO(Weapon,
                                    "STAGE-MARKER [tracks] clip='{}' {}..{}: {}",
                                    clipName,
                                    trackNameLineStart,
                                    t - 1,
                                    trackNameLine);
                            }
                            trackNameLineLength = 0;
                            trackNamesInLine = 0;
                            trackNameLineStart = t;
                        }
                        if (trackNamesInLine > 0) {
                            trackNameLine[trackNameLineLength++] = '|';
                        }
                        std::memcpy(trackNameLine + trackNameLineLength, printableName, nameLength);
                        trackNameLineLength += nameLength;
                        trackNameLine[trackNameLineLength] = '\0';
                        ++trackNamesInLine;
                    }
                    const auto annotationCount =
                        *reinterpret_cast<std::int32_t*>(track + kAnnotationTrackAnnotationsCountOffset);
                    const auto annotationData =
                        *reinterpret_cast<std::uintptr_t*>(track + kAnnotationTrackAnnotationsDataOffset);
                    if (annotationCount <= 0 || annotationCount > kMaxPlausibleAnnotations ||
                        !plausiblePointer(annotationData)) {
                        continue;
                    }
                    for (std::int32_t a = 0; a < annotationCount; ++a) {
                        const auto annotation =
                            annotationData + static_cast<std::uintptr_t>(a) * kAnnotationStride;
                        const float time = *reinterpret_cast<float*>(annotation + kAnnotationTimeOffset);
                        if (!std::isfinite(time) || time < -1.0f || time > duration + 1.0f) {
                            continue;
                        }
                        char text[96];
                        if (copyHkStringPtr(
                                *reinterpret_cast<std::uintptr_t*>(annotation + kAnnotationTextOffset),
                                text,
                                sizeof(text)) == 0) {
                            continue;
                        }
                        ++annotationLines;
                        if (s_stageMarkerLogBudget > 1) {
                            --s_stageMarkerLogBudget;
                            RDX_LOG_INFO(Weapon,
                                "STAGE-MARKER [annotation] clip='{}' track='{}' t={:.3f}/{:.3f}s text='{}'",
                                clipName,
                                trackName,
                                time,
                                duration,
                                text);
                        }
                    }
                }
                if (dumpTrackNames && trackNamesInLine > 0 && s_stageMarkerLogBudget > 1) {
                    --s_stageMarkerLogBudget;
                    RDX_LOG_INFO(Weapon,
                        "STAGE-MARKER [tracks] clip='{}' {}..{}: {}",
                        clipName,
                        trackNameLineStart,
                        trackCount - 1,
                        trackNameLine);
                }
            }

            std::uint32_t triggerLines = 0;
            std::int32_t triggerCountRaw = 0;
            const auto triggersObject =
                *reinterpret_cast<std::uintptr_t*>(clipGenerator + kClipGeneratorTriggersOffset);
            if (plausiblePointer(triggersObject)) {
                triggerCountRaw =
                    *reinterpret_cast<std::int32_t*>(triggersObject + kTriggerArrayCountOffset);
                const auto triggerData =
                    *reinterpret_cast<std::uintptr_t*>(triggersObject + kTriggerArrayDataOffset);
                if (triggerCountRaw > 0 && triggerCountRaw <= kMaxPlausibleTriggers &&
                    plausiblePointer(triggerData)) {
                    for (std::int32_t i = 0; i < triggerCountRaw; ++i) {
                        const auto trigger = triggerData + static_cast<std::uintptr_t>(i) * kTriggerStride;
                        const float localTime = *reinterpret_cast<float*>(trigger + kTriggerLocalTimeOffset);
                        // relativeToEndOfClip triggers carry negative times.
                        if (!std::isfinite(localTime) || localTime < -(duration + 1.0f) ||
                            localTime > duration + 1.0f) {
                            continue;
                        }
                        const auto eventId =
                            *reinterpret_cast<std::int32_t*>(trigger + kTriggerEventIdOffset);
                        ++triggerLines;
                        if (s_stageMarkerLogBudget > 1) {
                            --s_stageMarkerLogBudget;
                            char eventName[96];
                            resolveGraphEventName(behaviorGraph, eventId, eventName, sizeof(eventName));
                            RDX_LOG_INFO(Weapon,
                                "STAGE-MARKER [trigger] clip='{}' t={:.3f}/{:.3f}s eventId={} name='{}'",
                                clipName,
                                localTime,
                                duration,
                                eventId,
                                eventName);
                        }
                    }
                }
            }
            /*
             * Always exactly one summary per dumped clip, raw structure
             * counts included, so "VR clips carry no markers" and "a
             * plausibility gate rejected everything" are distinguishable
             * from the log alone (2026-07-05: a full session produced zero
             * marker lines and the old silent bails/DEBUG-only empty case
             * could not say which).
             */
            --s_stageMarkerLogBudget;
            RDX_LOG_INFO(Weapon,
                "STAGE-MARKER summary clip='{}' duration={:.2f}s annotationTracksRaw={} annotationsLogged={} triggersObject={} triggersRaw={} triggersLogged={} graphEventNames={}",
                clipName,
                duration,
                trackCount,
                annotationLines,
                plausiblePointer(triggersObject) ? "set" : "null",
                triggerCountRaw,
                triggerLines,
                behaviorGraphEventNameCount(behaviorGraph));
        }

        // Returns true when the binding reached a terminal outcome (harvested
        // or target-less) and should not be revisited this generation; false
        // on bails that may succeed later.
        bool harvestBinding(
            std::uintptr_t binding,
            std::uintptr_t skeleton,
            const char* const* allowedNodeNames,
            std::uint32_t allowedNodeNameCount,
            bool fromActivation,
            const char* clipAnimationName)
        {
            const auto animation = *reinterpret_cast<std::uintptr_t*>(binding + kBindingAnimationOffset);
            if (!plausiblePointer(animation)) {
                s_bailAnimationPtr.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("animation-ptr", binding, animation, 0.0f, 0, 0, 0);
                return false;
            }

            // Only spline-compressed clips are supported; the sampler slot is
            // dispatched virtually but the slot semantics were verified on
            // this class specifically. Other compressions are counted so the
            // gap is visible instead of silent.
            const auto vtable = *reinterpret_cast<std::uintptr_t*>(animation);
            if (vtable != RE::VTABLE::hkaSplineCompressedAnimation[0].address()) {
                s_skippedNonSpline.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("non-spline", binding, animation, 0.0f, 0, 0, 0);
                return false;
            }

            const float duration = *reinterpret_cast<float*>(animation + kAnimationDurationOffset);
            const auto animationTrackCount = *reinterpret_cast<std::int32_t*>(animation + kAnimationTrackCountOffset);
            if (!std::isfinite(duration) || duration < kMinClipDurationSeconds || duration > kMaxClipDurationSeconds ||
                animationTrackCount <= 0 || animationTrackCount > kMaxPlausibleTrackCount) {
                s_bailClipParams.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("clip-params", binding, animation, duration, animationTrackCount, 0, 0);
                return false;
            }

            // The engine sampler dereferences the spline payload unguarded;
            // binding-set stubs (token allocations, tiny m_data) and
            // anything else unloadable is skipped here. A real clip's mask
            // stream needs at least one byte per track per block.
            const auto splineNumBlocks = *reinterpret_cast<std::int32_t*>(animation + kSplineNumBlocksOffset);
            const auto splineMaxFramesPerBlock = *reinterpret_cast<std::int32_t*>(animation + kSplineMaxFramesPerBlockOffset);
            const auto splineBlockOffsets = *reinterpret_cast<std::uintptr_t*>(animation + kSplineBlockOffsetsOffset);
            const auto splineBlockOffsetsCount = *reinterpret_cast<std::int32_t*>(animation + kSplineBlockOffsetsCountOffset);
            const auto splineFloatBlockOffsets = *reinterpret_cast<std::uintptr_t*>(animation + kSplineFloatBlockOffsetsOffset);
            const auto splineFloatBlockOffsetsCount = *reinterpret_cast<std::int32_t*>(animation + kSplineFloatBlockOffsetsCountOffset);
            const auto splineDataBase = *reinterpret_cast<std::uintptr_t*>(animation + kSplineDataBaseOffset);
            const auto splineDataSize = *reinterpret_cast<std::int32_t*>(animation + kSplineDataSizeOffset);
            if (splineNumBlocks <= 0 || splineMaxFramesPerBlock < 2 ||
                !plausiblePointer(splineBlockOffsets) || splineBlockOffsetsCount < splineNumBlocks ||
                !plausiblePointer(splineFloatBlockOffsets) || splineFloatBlockOffsetsCount < splineNumBlocks ||
                !plausiblePointer(splineDataBase) || splineDataSize < animationTrackCount) {
                s_bailSplineData.fetch_add(1, std::memory_order_relaxed);
                if (s_bindingDetailLogs < kMaxBindingDetailLogsPerWalk) {
                    ++s_bindingDetailLogs;
                    RDX_LOG_WARN(Weapon,
                        "WeaponClipMotionHarvest: binding bail [spline-data] anim={:#x} duration={} tracks={} blocks={} maxFrames={} blockOffsets={:#x}({}) floatBlockOffsets={:#x}({}) data={:#x}({})",
                        animation,
                        duration,
                        animationTrackCount,
                        splineNumBlocks,
                        splineMaxFramesPerBlock,
                        splineBlockOffsets,
                        splineBlockOffsetsCount,
                        splineFloatBlockOffsets,
                        splineFloatBlockOffsetsCount,
                        splineDataBase,
                        splineDataSize);
                }
                return false;
            }

            const auto trackToBoneData = *reinterpret_cast<std::uintptr_t*>(binding + kBindingTrackToBoneDataOffset);
            const auto trackToBoneCount = *reinterpret_cast<std::int32_t*>(binding + kBindingTrackToBoneCountOffset);
            const auto boneCount = *reinterpret_cast<std::int32_t*>(skeleton + kSkeletonBonesCountOffset);
            if (boneCount <= 0 || boneCount > kMaxPlausibleBoneCount) {
                s_bailBoneCount.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("bone-count", binding, animation, duration, animationTrackCount, trackToBoneCount, boneCount);
                return false;
            }

            /*
             * Resolve which bone each transform track drives, in priority:
             * an explicit track-to-bone map; the flattened bone ranges of
             * the clip's skeleton partitions (weapon clips ship this way:
             * empty map, one partition — identity over the FULL rig would
             * scatter the tracks onto wrong bones); identity, valid only
             * when the clip covers the whole rig.
             */
            std::array<std::int16_t, kSampleBufferTracks> trackBones{};
            std::int32_t mappedTrackCount = 0;
            const auto partitionIndicesData = *reinterpret_cast<std::uintptr_t*>(binding + kBindingPartitionIndicesOffset);
            const auto partitionCount = *reinterpret_cast<std::int32_t*>(binding + kBindingPartitionCountOffset);
            if (trackToBoneCount > 0) {
                if (!plausiblePointer(trackToBoneData) || trackToBoneCount > kMaxPlausibleTrackCount) {
                    s_bailTrackMap.fetch_add(1, std::memory_order_relaxed);
                    logBindingBail("track-map", binding, animation, duration, animationTrackCount, trackToBoneCount, boneCount);
                    return false;
                }
                const auto* map = reinterpret_cast<const std::int16_t*>(trackToBoneData);
                const auto count = (std::min)(trackToBoneCount, static_cast<std::int32_t>(trackBones.size()));
                for (std::int32_t i = 0; i < count; ++i) {
                    trackBones[static_cast<std::size_t>(i)] = map[i];
                }
                mappedTrackCount = count;
            } else if (partitionCount > 0) {
                const auto skeletonPartitionsData = *reinterpret_cast<std::uintptr_t*>(skeleton + kSkeletonPartitionsDataOffset);
                const auto skeletonPartitionCount = *reinterpret_cast<std::int32_t*>(skeleton + kSkeletonPartitionsCountOffset);
                if (!plausiblePointer(partitionIndicesData) || partitionCount > kMaxPlausibleTrackCount ||
                    !plausiblePointer(skeletonPartitionsData) || skeletonPartitionCount <= 0 ||
                    skeletonPartitionCount > kMaxPlausibleBoneCount) {
                    s_bailTrackMap.fetch_add(1, std::memory_order_relaxed);
                    logBindingBail("partition-map", binding, animation, duration, animationTrackCount, partitionCount, boneCount);
                    return false;
                }
                const auto* partitionIndices = reinterpret_cast<const std::uint16_t*>(partitionIndicesData);
                for (std::int32_t p = 0; p < partitionCount && mappedTrackCount < static_cast<std::int32_t>(trackBones.size()); ++p) {
                    const auto partitionIndex = static_cast<std::int32_t>(partitionIndices[p]);
                    if (partitionIndex >= skeletonPartitionCount) {
                        continue;
                    }
                    const auto entry = skeletonPartitionsData +
                                       static_cast<std::uintptr_t>(partitionIndex) * kSkeletonPartitionStride;
                    const auto startBone = *reinterpret_cast<const std::int16_t*>(entry + kPartitionStartBoneOffset);
                    const auto numBones = *reinterpret_cast<const std::int16_t*>(entry + kPartitionNumBonesOffset);
                    if (startBone < 0 || numBones <= 0 || startBone + numBones > boneCount) {
                        continue;
                    }
                    for (std::int16_t b = 0; b < numBones && mappedTrackCount < static_cast<std::int32_t>(trackBones.size()); ++b) {
                        trackBones[static_cast<std::size_t>(mappedTrackCount++)] = static_cast<std::int16_t>(startBone + b);
                    }
                }
                if (mappedTrackCount == 0) {
                    s_bailTrackMap.fetch_add(1, std::memory_order_relaxed);
                    logBindingBail("partition-map", binding, animation, duration, animationTrackCount, partitionCount, boneCount);
                    return false;
                }
            } else if (animationTrackCount == boneCount) {
                const auto count = (std::min)(animationTrackCount, static_cast<std::int32_t>(trackBones.size()));
                for (std::int32_t i = 0; i < count; ++i) {
                    trackBones[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i);
                }
                mappedTrackCount = count;
            } else {
                // No map, no partitions, and the track count does not cover
                // the rig — identity would misattribute every track.
                s_bailTrackMap.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("track-map", binding, animation, duration, animationTrackCount, trackToBoneCount, boneCount);
                return false;
            }

            // Collect the weapon-part tracks for this clip. Tracks beyond
            // the decode buffer cannot be sampled (slot 5 decodes
            // sequentially from track 0) and are skipped.
            std::scoped_lock scratchLock(s_harvestScratchMutex);
            auto& trackIndices = s_scratchTrackIndices;
            auto& tracks = s_scratchTracks;
            std::uint32_t targetCount = 0;
            std::int32_t maxTargetTrack = 0;
            const auto usableTrackCount = (std::min)(mappedTrackCount, animationTrackCount);
            for (std::int32_t track = 0; track < usableTrackCount && targetCount < tracks.size(); ++track) {
                const auto boneIndex = trackBones[static_cast<std::size_t>(track)];
                if (boneIndex < 0 || boneIndex >= boneCount) {
                    continue;
                }
                const char* name = skeletonBoneName(skeleton, boneIndex);
                if (!isHarvestTargetBone(name, allowedNodeNames, allowedNodeNameCount)) {
                    continue;
                }
                trackIndices[targetCount] = static_cast<std::int16_t>(track);
                maxTargetTrack = (std::max)(maxTargetTrack, track);
                auto& samples = tracks[targetCount];
                samples = {};
                std::size_t nameLength = 0;
                while (nameLength < samples.boneName.size() - 1 && name[nameLength] != '\0') {
                    ++nameLength;
                }
                std::memcpy(samples.boneName.data(), name, nameLength);
                ++targetCount;
            }
            if (targetCount == 0) {
                s_bindingsNoTargets.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            const auto sampler = reinterpret_cast<SampleTracks_t>(
                reinterpret_cast<std::uintptr_t*>(vtable)[kSampleTracksSlot]);
            if (!sampler) {
                s_bailSampler.fetch_add(1, std::memory_order_relaxed);
                logBindingBail("sampler", binding, animation, duration, animationTrackCount, trackToBoneCount, boneCount);
                return false;
            }

            // Full-pose decode: slot 5 decodes tracks 0..decodeCount-1, then
            // the target tracks are picked out of the buffer.
            const auto decodeCount = static_cast<std::uint32_t>(maxTargetTrack) + 1;
            std::array<HkQsTransform, kSampleBufferTracks> sampled{};
            for (std::uint32_t step = 0; step < weapon_clip_stroke::kClipSampleCount; ++step) {
                const float time = duration * static_cast<float>(step) /
                                   static_cast<float>(weapon_clip_stroke::kClipSampleCount - 1);
                sampler(reinterpret_cast<void*>(animation), time, decodeCount, sampled.data(), 0, nullptr);
                for (std::uint32_t i = 0; i < targetCount; ++i) {
                    const auto& decoded = sampled[static_cast<std::size_t>(trackIndices[i])];
                    auto& pose = tracks[i].samples[step];
                    pose.translate = weapon_part_motion_path::Vec3{
                        decoded.translate[0],
                        decoded.translate[1],
                        decoded.translate[2],
                    };
                    // Havok quaternion order is (x, y, z, w).
                    pose.rotate = weapon_part_motion_path::quatNormalizeOrIdentity(weapon_part_motion_path::Quat{
                        decoded.rotate[3],
                        decoded.rotate[0],
                        decoded.rotate[1],
                        decoded.rotate[2],
                    });
                }
            }
            for (std::uint32_t i = 0; i < targetCount; ++i) {
                tracks[i].sampleCount = weapon_clip_stroke::kClipSampleCount;
            }

            auto& groups = s_scratchGroups;
            const auto groupCount = weapon_clip_stroke::buildAuthoredGroups(
                tracks.data(),
                targetCount,
                groups.data(),
                static_cast<std::uint32_t>(groups.size()));
            if (groupCount == 0) {
                return false;
            }
            s_bindingsHarvested.fetch_add(1, std::memory_order_relaxed);

            // Frame diagnostics (capped): the targets' rig parents plus the
            // lead target's raw sample endpoints. Compared against the
            // learned-path endpoints for the same part, this pins down any
            // frame mismatch between rig-derived and scene-observed motion.
            if (s_bindingDetailLogs < kMaxBindingDetailLogsPerWalk) {
                ++s_bindingDetailLogs;
                const auto parentIndicesData = *reinterpret_cast<std::uintptr_t*>(skeleton + kSkeletonParentIndicesOffset);
                const auto parentIndicesCount = *reinterpret_cast<std::int32_t*>(skeleton + kSkeletonParentIndicesCountOffset);
                const auto parentNameOf = [&](std::uint32_t target) -> const char* {
                    const auto bone = trackBones[static_cast<std::size_t>(trackIndices[target])];
                    if (!plausiblePointer(parentIndicesData) || parentIndicesCount < boneCount ||
                        bone < 0 || bone >= boneCount) {
                        return "?";
                    }
                    const auto parent = reinterpret_cast<const std::int16_t*>(parentIndicesData)[bone];
                    if (parent < 0 || parent >= boneCount) {
                        return "<root>";
                    }
                    const char* name = skeletonBoneName(skeleton, parent);
                    return name ? name : "?";
                };
                const auto& rawStart = tracks[0].samples[0].translate;
                const auto& rawEnd = tracks[0].samples[weapon_clip_stroke::kClipSampleCount - 1].translate;
                RDX_LOG_INFO(Weapon,
                    "WeaponClipMotionHarvest: harvested clip duration={:.2f} targets={} lead {}(parent={}) next [{}(parent={}) {}(parent={})] lead raw start=({:.2f},{:.2f},{:.2f}) end=({:.2f},{:.2f},{:.2f})",
                    duration,
                    targetCount,
                    tracks[0].boneName.data(),
                    parentNameOf(0),
                    targetCount > 1 ? tracks[1].boneName.data() : "",
                    targetCount > 1 ? parentNameOf(1) : "",
                    targetCount > 2 ? tracks[2].boneName.data() : "",
                    targetCount > 2 ? parentNameOf(2) : "",
                    rawStart.x,
                    rawStart.y,
                    rawStart.z,
                    rawEnd.x,
                    rawEnd.y,
                    rawEnd.z);
            }

            // Provenance travels with each group: activation strokes are the
            // weapon's own animation; walk strokes are loaded-set fallback.
            for (std::uint32_t i = 0; i < groupCount; ++i) {
                groups[i].activatedClip = fromActivation;
                groups[i].clipAnimationName = {};
                if (clipAnimationName) {
                    std::size_t length = 0;
                    while (length < groups[i].clipAnimationName.size() - 1 && clipAnimationName[length] != '\0') {
                        groups[i].clipAnimationName[length] = clipAnimationName[length];
                        ++length;
                    }
                }
            }

            std::scoped_lock lock(s_queueMutex);
            for (std::uint32_t i = 0; i < groupCount; ++i) {
                if (s_queueCount >= s_queue.size()) {
                    /*
                     * Activated groups outrank fallback data and must not be
                     * lost to an equip-time walk flood (in-game 2026-07-04:
                     * groupsDropped grew by exactly one clip per equip and no
                     * activated stroke ever reached the store): evict a queued
                     * fallback group instead of dropping the activated one.
                     */
                    bool evicted = false;
                    if (groups[i].activatedClip) {
                        for (std::uint32_t slot = 0; slot < s_queueCount; ++slot) {
                            if (!s_queue[slot].activatedClip) {
                                s_queue[slot] = groups[i];
                                s_groupsQueued.fetch_add(1, std::memory_order_relaxed);
                                evicted = true;
                                break;
                            }
                        }
                    }
                    if (!evicted) {
                        s_groupsDropped.fetch_add(1, std::memory_order_relaxed);
                    }
                    continue;
                }
                s_queue[s_queueCount++] = groups[i];
                s_groupsQueued.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }

        /*
         * Resolve holder → binding set + skeleton. Returns false while the
         * weapon graph or its bindings are not available yet (still loading)
         * or any pointer fails the plausibility gate.
         */
        struct ResolvedBindings
        {
            std::uintptr_t skeleton{ 0 };
            std::uintptr_t bindingsData{ 0 };
            std::int32_t bindingCount{ 0 };
        };

        struct GraphArrayView
        {
            std::uintptr_t base{ 0 };
            std::uint32_t capacity{ 0 };
        };

        // Graph slots the walk may visit; bounded far above any real manager
        // (the player carries two graphs: first- and third-person). Weapon
        // clips do NOT live on the active graph — in-game diagnostics
        // (2026-07-04) showed the player's active graph is the 94-bone body
        // rig — so every entry is visited, gated per slot by the
        // BShkbAnimationGraph vtable (in-game confirmed module offset) so a
        // stale capacity slot degrades into a skip.
        constexpr std::uint32_t kMaxWalkGraphs = 8;
        constexpr std::uintptr_t kGraphVtableModuleOffset = 0x2E00A48;

        bool resolveGraphArray(std::uintptr_t manager, GraphArrayView& out)
        {
            s_lastResolveStage = "manager";
            if (!plausiblePointer(manager)) {
                return false;
            }
            s_lastResolveStage = "graphs-array";
            const auto capacityAndFlags = *reinterpret_cast<std::uint32_t*>(manager + kManagerGraphsCapacityOffset);
            const auto storageAddress = manager + kManagerGraphsStorageOffset;
            const auto graphsBase = (capacityAndFlags & kGraphsInlineStorageFlag) != 0
                ? storageAddress
                : *reinterpret_cast<std::uintptr_t*>(storageAddress);
            if (!plausiblePointer(graphsBase)) {
                return false;
            }
            out.base = graphsBase;
            out.capacity = (std::min)(capacityAndFlags & ~kGraphsInlineStorageFlag, kMaxWalkGraphs);
            return true;
        }

        // Vtable-gated graphs-array entry; 0 when the slot is empty, stale,
        // or not a BShkbAnimationGraph.
        [[nodiscard]] std::uintptr_t graphAtIndex(const GraphArrayView& graphs, std::uint32_t index)
        {
            if (index >= graphs.capacity) {
                return 0;
            }
            const auto graph = reinterpret_cast<const std::uintptr_t*>(graphs.base)[index];
            if (!plausiblePointer(graph) || objectVtableRel(graph) != kGraphVtableModuleOffset) {
                return 0;
            }
            return graph;
        }

        bool resolveGraphBindings(std::uintptr_t graph, ResolvedBindings& out)
        {
            s_lastResolveStage = "graph";
            if (!graph) {
                return false;
            }
            s_lastResolveStage = "character-setup";
            const auto character = graph + kGraphCharacterOffset;
            const auto setup = *reinterpret_cast<std::uintptr_t*>(character + kCharacterSetupOffset);
            if (!plausiblePointer(setup)) {
                return false;
            }
            s_lastResolveStage = "skeleton";
            const auto skeleton = *reinterpret_cast<std::uintptr_t*>(setup + kSetupAnimationSkeletonOffset);
            if (!plausiblePointer(skeleton)) {
                return false;
            }
            s_lastResolveStage = "binding-set";
            auto bindingSet = *reinterpret_cast<std::uintptr_t*>(character + kCharacterBindingSetOverrideOffset);
            if (!plausiblePointer(bindingSet)) {
                bindingSet = *reinterpret_cast<std::uintptr_t*>(setup + kSetupBindingSetOffset);
            }
            if (!plausiblePointer(bindingSet)) {
                return false;
            }
            s_lastResolveStage = "bindings";
            const auto bindingsData = *reinterpret_cast<std::uintptr_t*>(bindingSet + kBindingSetDataOffset);
            const auto bindingCount = *reinterpret_cast<std::int32_t*>(bindingSet + kBindingSetCountOffset);
            if (!plausiblePointer(bindingsData) || bindingCount <= 0 || bindingCount > kMaxPlausibleBindingCount) {
                return false;
            }
            s_lastResolveStage = "ok";
            out.skeleton = skeleton;
            out.bindingsData = bindingsData;
            out.bindingCount = bindingCount;
            return true;
        }

        using ClipGeneratorActivateFn = void (*)(void*, void*);
        ClipGeneratorActivateFn s_originalClipActivate = nullptr;
        // update(this, const hkbContext&, hkReal timestep) — timestep in
        // XMM2 per MSVC x64; verified signature of 0x14192D0D0.
        using ClipGeneratorUpdateFn = void (*)(void*, void*, float);
        ClipGeneratorUpdateFn s_originalClipUpdate = nullptr;
        using ClipGeneratorDeactivateFn = void (*)(void*, void*);
        ClipGeneratorDeactivateFn s_originalClipDeactivate = nullptr;

        /*
         * Sweep hijack, called with s_hookMutex held right after the
         * original activate returned (payload resident, m_localTime still
         * at its start value). Recipe per the verified interface: save the
         * mode byte, seed BOTH time fractions from the current local time,
         * then flip to user-controlled — publication of s_sweepClip
         * (release) is last so the update shim never sees a half-seeded
         * sweep. Every read is plausibility-gated; a bail leaves the clip
         * untouched.
         */
        void maybeBeginScrubSweepLocked(std::uintptr_t clipGenerator, std::uintptr_t binding, const char* clipName)
        {
            if (!s_sweepConfigEnabled || s_sweepClip.load(std::memory_order_relaxed) != 0) {
                return;
            }
            if (s_sweepConfigFilter[0] == '\0' || !nameContainsNoCase(clipName, s_sweepConfigFilter.data())) {
                return;
            }
            const auto animation = *reinterpret_cast<std::uintptr_t*>(binding + kBindingAnimationOffset);
            if (!plausiblePointer(animation)) {
                return;
            }
            const float duration = *reinterpret_cast<float*>(animation + kAnimationDurationOffset);
            if (!std::isfinite(duration) || duration < kMinClipDurationSeconds || duration > kMaxClipDurationSeconds) {
                return;
            }
            const float cropStart = *reinterpret_cast<float*>(clipGenerator + kClipGeneratorCropStartOffset);
            const float cropEnd = *reinterpret_cast<float*>(clipGenerator + kClipGeneratorCropEndOffset);
            const float localTime = *reinterpret_cast<float*>(clipGenerator + kClipGeneratorLocalTimeOffset);
            float croppedDuration = duration;
            if (std::isfinite(cropStart) && std::isfinite(cropEnd) && cropStart >= 0.0f && cropEnd >= 0.0f &&
                cropStart + cropEnd < duration) {
                croppedDuration = duration - cropStart - cropEnd;
            }
            float seedFraction = 0.0f;
            if (std::isfinite(localTime) && croppedDuration > kMinClipDurationSeconds) {
                seedFraction = localTime - (std::isfinite(cropStart) && cropStart > 0.0f ? cropStart : 0.0f);
                seedFraction = seedFraction / croppedDuration;
                seedFraction = seedFraction < 0.0f ? 0.0f : (seedFraction > 1.0f ? 1.0f : seedFraction);
            }

            s_sweepSavedMode = *reinterpret_cast<std::uint8_t*>(clipGenerator + kClipGeneratorModeOffset);
            *reinterpret_cast<float*>(clipGenerator + kClipGeneratorUserFractionOffset) = seedFraction;
            *reinterpret_cast<float*>(clipGenerator + kClipGeneratorPrevUserFractionOffset) = seedFraction;
            *reinterpret_cast<std::uint8_t*>(clipGenerator + kClipGeneratorModeOffset) = kClipModeUserControlled;

            s_sweepElapsedSeconds = 0.0f;
            s_sweepSeconds = s_sweepConfigSeconds;
            s_sweepClipDuration = duration;
            s_sweepNextLogDecile = 1;
            s_sweepClipName = {};
            if (clipName) {
                std::size_t length = 0;
                while (length < s_sweepClipName.size() - 1 && clipName[length] != '\0') {
                    s_sweepClipName[length] = clipName[length];
                    ++length;
                }
            }
            s_sweepClip.store(clipGenerator, std::memory_order_release);
            RDX_LOG_INFO(Weapon,
                "CLIP-SWEEP start clip='{}' duration={:.2f}s cropped={:.2f}s sweep={:.1f}s seed={:.3f} savedMode={} (mode 2 hijack; watch arms vs weapon rig)",
                s_sweepClipName.data(),
                duration,
                croppedDuration,
                s_sweepSeconds,
                seedFraction,
                s_sweepSavedMode);
        }

        /*
         * Runs on the engine's graph-update thread right after the original
         * hkbClipGenerator::activate completed, i.e. while the animation
         * control at clipGenerator+0xD0 (and its binding at control+0x38)
         * is guaranteed resident. Every read is plausibility-gated and the
         * sampler's spline gates still apply, so an unexpected state
         * degrades into a counted skip. The character filter keeps NPC clip
         * activations out.
         */
        void harvestFromClipGenerator(void* clipGeneratorRaw, void* contextRaw)
        {
            s_hookFires.fetch_add(1, std::memory_order_relaxed);
            const auto clipGenerator = reinterpret_cast<std::uintptr_t>(clipGeneratorRaw);
            const auto context = reinterpret_cast<std::uintptr_t>(contextRaw);
            if (!plausiblePointer(clipGenerator) || !plausiblePointer(context)) {
                return;
            }

            std::scoped_lock lock(s_hookMutex);
            /*
             * The second argument is a stack-built hkbContext, not the
             * character (in-game hook dumps 2026-07-04: stack-range
             * addresses, first qword not a module vtable). The owning
             * hkbCharacter is one of its leading pointer members, so the
             * first few qwords are identity-matched against the registered
             * candidate-graph characters; only the match is dereferenced.
             */
            std::uintptr_t character = 0;
            std::array<std::uintptr_t, 4> contextSlots{};
            for (std::uint32_t slot = 0; slot < contextSlots.size(); ++slot) {
                contextSlots[slot] = *reinterpret_cast<const std::uintptr_t*>(context + slot * sizeof(std::uintptr_t));
            }
            for (std::uint32_t slot = 0; slot < contextSlots.size() && character == 0; ++slot) {
                for (std::uint32_t i = 0; i < s_hookCharacterCount; ++i) {
                    if (s_hookCharacters[i] == contextSlots[slot]) {
                        character = contextSlots[slot];
                        break;
                    }
                }
            }
            if (character == 0 || s_hookNodeNameCount == 0) {
                if (s_hookCharacterCount > 0 &&
                    s_hookUnmatchedLogs.fetch_add(1, std::memory_order_relaxed) < kMaxHookUnmatchedLogs) {
                    RDX_LOG_WARN(Weapon,
                        "WeaponClipMotionHarvest: hook fired, no registered character in context {:#x} slots=[{:#x}|{:#x}|{:#x}|{:#x}] clipGen={:#x}; registered[0]={:#x}",
                        context,
                        contextSlots[0],
                        contextSlots[1],
                        contextSlots[2],
                        contextSlots[3],
                        clipGenerator,
                        s_hookCharacters[0]);
                }
                return;
            }
            s_hookActivations.fetch_add(1, std::memory_order_relaxed);

            const auto wrapper = *reinterpret_cast<std::uintptr_t*>(clipGenerator + kClipGeneratorLoadedBindingOffset);
            if (!plausiblePointer(wrapper)) {
                return;
            }
            const auto binding = *reinterpret_cast<std::uintptr_t*>(wrapper + kLoadedBindingWrapperBindingOffset);
            if (!plausiblePointer(binding)) {
                return;
            }
            const auto setup = *reinterpret_cast<std::uintptr_t*>(character + kCharacterSetupOffset);
            if (!plausiblePointer(setup)) {
                return;
            }
            const auto skeleton = *reinterpret_cast<std::uintptr_t*>(setup + kSetupAnimationSkeletonOffset);
            if (!plausiblePointer(skeleton)) {
                return;
            }
            /*
             * hkbClipGenerator::m_animationName, hkStringPtr at +0x90.
             * Raw-disassembly verified (FO4VR 0x141939911: RSI =
             * [clip+0x90] & ~1 appended to "Animation loaded directly from
             * clip's animationName"; the AND -2 is the hkStringPtr
             * owned-bit convention). Copied under plausibility + printable
             * gates — a bad read degrades into an unnamed harvest. Copied
             * BEFORE the processed-binding dedup: the sweep probe below
             * must see every activation, not just the first per binding.
             */
            std::array<char, 64> animationName{};
            const auto namePointer =
                *reinterpret_cast<std::uintptr_t*>(clipGenerator + kClipGeneratorAnimationNameOffset) &
                ~static_cast<std::uintptr_t>(1);
            if (plausiblePointer(namePointer)) {
                const char* nameChars = reinterpret_cast<const char*>(namePointer);
                std::size_t length = 0;
                while (length < animationName.size() - 1 && nameChars[length] != '\0') {
                    const unsigned char c = static_cast<unsigned char>(nameChars[length]);
                    if (c < 0x20 || c > 0x7E) {
                        length = 0;
                        break;
                    }
                    animationName[length] = nameChars[length];
                    ++length;
                }
                animationName[length] = '\0';
            }
            maybeBeginScrubSweepLocked(clipGenerator, binding, animationName.data());
            if (bindingProcessedLocked(binding, /*fromActivation=*/true)) {
                return;
            }
            s_bindingsSeen.fetch_add(1, std::memory_order_relaxed);
            /*
             * The clip's own hkbBehaviorGraph rides at hkbContext+0x10 —
             * needed to turn graph-local trigger eventIds into authored
             * names. Exact-vtable gate: anything else degrades into
             * nameless trigger lines, never a bad dereference.
             */
            std::uintptr_t behaviorGraph = *reinterpret_cast<std::uintptr_t*>(context + kContextBehaviorGraphOffset);
            if (!plausiblePointer(behaviorGraph) ||
                *reinterpret_cast<std::uintptr_t*>(behaviorGraph) !=
                    REL::Module::get().base() + kBehaviorGraphVtableModuleOffset) {
                behaviorGraph = 0;
            }
            // Stage markers dump BEFORE the harvest so the names/times land
            // in the log even when the stroke harvest itself bails.
            dumpClipStageMarkers(clipGenerator, binding, animationName.data(), behaviorGraph);
            if (harvestBinding(
                    binding,
                    skeleton,
                    s_hookNodeNamePointers.data(),
                    s_hookNodeNameCount,
                    /*fromActivation=*/true,
                    animationName.data())) {
                markBindingProcessedLocked(binding, /*fromActivation=*/true);
            }
        }

        void clipGeneratorActivateShim(void* clipGenerator, void* context)
        {
            if (s_originalClipActivate) {
                s_originalClipActivate(clipGenerator, context);
            }
            harvestFromClipGenerator(clipGenerator, context);
        }

        /*
         * Sweep driver. Fires for EVERY clip generator in the game each
         * frame; the non-swept path is one relaxed load and a compare. For
         * the swept clip: advance the ramp by the engine's own timestep,
         * write the fraction BEFORE the original update (mode 2 computes
         * localTime from it this same call), then log the engine-computed
         * localTime as the probe's evidence. The clip is only ever touched
         * inside its own engine update/deactivate, so its lifetime is
         * guaranteed by the caller — no retained-pointer dereference risk.
         */
        void clipGeneratorUpdateShim(void* clipGeneratorRaw, void* context, float timestep)
        {
            const auto clipGenerator = reinterpret_cast<std::uintptr_t>(clipGeneratorRaw);
            if (s_sweepClip.load(std::memory_order_acquire) != clipGenerator) {
                if (s_originalClipUpdate) {
                    s_originalClipUpdate(clipGeneratorRaw, context, timestep);
                }
                return;
            }
            if (std::isfinite(timestep) && timestep > 0.0f && timestep < 1.0f) {
                s_sweepElapsedSeconds += timestep;
            }
            const float fraction =
                s_sweepSeconds > 0.0f ? (std::min)(s_sweepElapsedSeconds / s_sweepSeconds, 1.0f) : 1.0f;
            *reinterpret_cast<float*>(clipGenerator + kClipGeneratorUserFractionOffset) = fraction;
            if (s_originalClipUpdate) {
                s_originalClipUpdate(clipGeneratorRaw, context, timestep);
            }
            const auto decile = static_cast<std::uint32_t>(fraction * 10.0f);
            if (decile >= s_sweepNextLogDecile) {
                s_sweepNextLogDecile = decile + 1;
                RDX_LOG_INFO(Weapon,
                    "CLIP-SWEEP clip='{}' fraction={:.2f} engineLocalTime={:.3f}/{:.2f}s",
                    s_sweepClipName.data(),
                    fraction,
                    *reinterpret_cast<float*>(clipGenerator + kClipGeneratorLocalTimeOffset),
                    s_sweepClipDuration);
            }
            if (fraction >= 1.0f) {
                // Restore AFTER the fraction-1.0 update posed the clip end;
                // native mode resumes from there and finishes the reload
                // normally (missed triggers may fire in a burst — expected
                // probe artifact, commit semantics are ours in the real
                // feature).
                *reinterpret_cast<std::uint8_t*>(clipGenerator + kClipGeneratorModeOffset) = s_sweepSavedMode;
                s_sweepClip.store(0, std::memory_order_release);
                RDX_LOG_INFO(Weapon,
                    "CLIP-SWEEP done clip='{}' swept {:.1f}s, mode {} restored",
                    s_sweepClipName.data(),
                    s_sweepSeconds,
                    s_sweepSavedMode);
            }
        }

        void clipGeneratorDeactivateShim(void* clipGeneratorRaw, void* context)
        {
            const auto clipGenerator = reinterpret_cast<std::uintptr_t>(clipGeneratorRaw);
            if (s_sweepClip.load(std::memory_order_acquire) == clipGenerator) {
                // Restore before the original tears the payload down; the
                // mode byte survives on the clone for its next activation.
                *reinterpret_cast<std::uint8_t*>(clipGenerator + kClipGeneratorModeOffset) = s_sweepSavedMode;
                s_sweepClip.store(0, std::memory_order_release);
                RDX_LOG_INFO(Weapon,
                    "CLIP-SWEEP aborted clip='{}' deactivated at {:.2f}s of {:.1f}s sweep (mode restored)",
                    s_sweepClipName.data(),
                    s_sweepElapsedSeconds,
                    s_sweepSeconds);
            }
            if (s_originalClipDeactivate) {
                s_originalClipDeactivate(clipGeneratorRaw, context);
            }
        }
    }

    const char* lastResolveStage()
    {
        return s_lastResolveStage;
    }

    const void* managerFromWeaponHolder(const void* weaponGraphHolder)
    {
        const auto holder = reinterpret_cast<std::uintptr_t>(weaponGraphHolder);
        if (!plausiblePointer(holder)) {
            return nullptr;
        }
        const auto manager = *reinterpret_cast<std::uintptr_t*>(holder + kHolderManagerOffset);
        return plausiblePointer(manager) ? reinterpret_cast<const void*>(manager) : nullptr;
    }

    void ensureClipActivationHookInstalled()
    {
        static bool s_installed = false;
        if (s_installed) {
            return;
        }
        s_installed = true;
        const auto vtableBase = REL::Module::get().base() + kClipGeneratorVtableModuleOffset;
        // 8-byte aligned pointer stores are atomic on x64, so concurrent
        // graph updates dispatching through the slots stay safe during the
        // swaps. Activate feeds the harvest + sweep hijack; update drives
        // an active sweep's ramp; deactivate restores an interrupted sweep.
        const auto activateSlot = vtableBase + kClipGeneratorActivateSlotOffset;
        s_originalClipActivate =
            reinterpret_cast<ClipGeneratorActivateFn>(*reinterpret_cast<std::uintptr_t*>(activateSlot));
        REL::safe_write(activateSlot, reinterpret_cast<std::uintptr_t>(&clipGeneratorActivateShim));
        const auto updateSlot = vtableBase + kClipGeneratorUpdateSlotOffset;
        s_originalClipUpdate =
            reinterpret_cast<ClipGeneratorUpdateFn>(*reinterpret_cast<std::uintptr_t*>(updateSlot));
        REL::safe_write(updateSlot, reinterpret_cast<std::uintptr_t>(&clipGeneratorUpdateShim));
        const auto deactivateSlot = vtableBase + kClipGeneratorDeactivateSlotOffset;
        s_originalClipDeactivate =
            reinterpret_cast<ClipGeneratorDeactivateFn>(*reinterpret_cast<std::uintptr_t*>(deactivateSlot));
        REL::safe_write(deactivateSlot, reinterpret_cast<std::uintptr_t>(&clipGeneratorDeactivateShim));
        RDX_LOG_INFO(Weapon,
            "WeaponClipMotionHarvest: clip lifecycle hooks installed (activate +{:#x}, update +{:#x}, deactivate +{:#x})",
            moduleRelative(reinterpret_cast<std::uintptr_t>(s_originalClipActivate)),
            moduleRelative(reinterpret_cast<std::uintptr_t>(s_originalClipUpdate)),
            moduleRelative(reinterpret_cast<std::uintptr_t>(s_originalClipDeactivate)));
    }

    void setScrubSweepConfig(bool enabled, float sweepSeconds, const char* clipNameFilter)
    {
        std::scoped_lock lock(s_hookMutex);
        s_sweepConfigEnabled = enabled;
        if (std::isfinite(sweepSeconds)) {
            s_sweepConfigSeconds = sweepSeconds < 1.0f ? 1.0f : (sweepSeconds > 60.0f ? 60.0f : sweepSeconds);
        }
        s_sweepConfigFilter = {};
        if (clipNameFilter) {
            std::size_t length = 0;
            while (length < s_sweepConfigFilter.size() - 1 && clipNameFilter[length] != '\0') {
                s_sweepConfigFilter[length] = clipNameFilter[length];
                ++length;
            }
        }
    }

    void setClipActivationTargets(
        const void* const* graphManagers,
        std::uint32_t managerCount,
        const char* const* allowedNodeNames,
        std::uint32_t allowedNodeNameCount)
    {
        std::scoped_lock lock(s_hookMutex);
        s_hookCharacterCount = 0;
        for (std::uint32_t m = 0; m < managerCount; ++m) {
            GraphArrayView graphs{};
            if (!resolveGraphArray(reinterpret_cast<std::uintptr_t>(graphManagers[m]), graphs)) {
                continue;
            }
            for (std::uint32_t i = 0; i < graphs.capacity && s_hookCharacterCount < s_hookCharacters.size(); ++i) {
                const auto graph = graphAtIndex(graphs, i);
                if (graph != 0) {
                    s_hookCharacters[s_hookCharacterCount++] = graph + kGraphCharacterOffset;
                }
            }
        }
        s_hookNodeNameCount = 0;
        for (std::uint32_t i = 0; i < allowedNodeNameCount && s_hookNodeNameCount < s_hookNodeNames.size(); ++i) {
            const char* name = allowedNodeNames[i];
            if (!name || name[0] == '\0') {
                continue;
            }
            auto& storage = s_hookNodeNames[s_hookNodeNameCount];
            std::size_t length = 0;
            while (length < storage.size() - 1 && name[length] != '\0') {
                storage[length] = name[length];
                ++length;
            }
            storage[length] = '\0';
            s_hookNodeNamePointers[s_hookNodeNameCount] = storage.data();
            ++s_hookNodeNameCount;
        }
    }

    void clearClipActivationTargets()
    {
        std::scoped_lock lock(s_hookMutex);
        s_hookCharacterCount = 0;
        s_hookNodeNameCount = 0;
    }

    bool probeBindings(const void* graphManager)
    {
        GraphArrayView graphs{};
        if (!resolveGraphArray(reinterpret_cast<std::uintptr_t>(graphManager), graphs)) {
            return false;
        }
        for (std::uint32_t i = 0; i < graphs.capacity; ++i) {
            ResolvedBindings resolved{};
            if (resolveGraphBindings(graphAtIndex(graphs, i), resolved)) {
                return true;
            }
        }
        return false;
    }

    void logResolveDiagnostics(const void* graphManager, const char* label)
    {
        const auto manager = reinterpret_cast<std::uintptr_t>(graphManager);
        std::uint32_t capacityAndFlags = 0;
        std::uint32_t activeGraphIndex = 0;
        if (plausiblePointer(manager)) {
            capacityAndFlags = *reinterpret_cast<std::uint32_t*>(manager + kManagerGraphsCapacityOffset);
            activeGraphIndex = *reinterpret_cast<std::uint32_t*>(manager + kManagerActiveGraphOffset);
        }
        GraphArrayView graphs{};
        const bool haveArray = resolveGraphArray(manager, graphs);
        RDX_LOG_WARN(Weapon,
            "WeaponClipMotionHarvest diagnostics [{}]: mgr={:#x}(vt+{:#x}) graphsFlags={:#010x} activeIdx={} slots={}",
            label ? label : "?",
            manager,
            objectVtableRel(manager),
            capacityAndFlags,
            activeGraphIndex,
            graphs.capacity);
        if (!haveArray) {
            return;
        }

        for (std::uint32_t index = 0; index < graphs.capacity; ++index) {
            const auto rawGraph = reinterpret_cast<const std::uintptr_t*>(graphs.base)[index];
            // Deep reads only through the vtable gate; the raw slot value and
            // its vtable are still printed so a rejected slot is explainable.
            const auto graph = graphAtIndex(graphs, index);
            std::uintptr_t character = 0;
            std::uintptr_t setup = 0;
            std::uintptr_t skeleton = 0;
            std::uintptr_t bindingSet = 0;
            std::uintptr_t bindingsData = 0;
            std::int32_t bindingCount = -1;
            std::int32_t boneCount = -1;
            const char* bindingSetSource = "none";
            std::array<const char*, 3> firstBoneNames{ "", "", "" };
            const char* lastBoneName = "";
            if (graph != 0) {
                character = graph + kGraphCharacterOffset;
                setup = *reinterpret_cast<std::uintptr_t*>(character + kCharacterSetupOffset);
                const auto overrideSet = *reinterpret_cast<std::uintptr_t*>(character + kCharacterBindingSetOverrideOffset);
                if (plausiblePointer(overrideSet)) {
                    bindingSet = overrideSet;
                    bindingSetSource = "override";
                } else if (plausiblePointer(setup)) {
                    bindingSet = *reinterpret_cast<std::uintptr_t*>(setup + kSetupBindingSetOffset);
                    if (plausiblePointer(bindingSet)) {
                        bindingSetSource = "setup";
                    }
                }
                if (plausiblePointer(setup)) {
                    skeleton = *reinterpret_cast<std::uintptr_t*>(setup + kSetupAnimationSkeletonOffset);
                }
                if (plausiblePointer(skeleton)) {
                    boneCount = *reinterpret_cast<std::int32_t*>(skeleton + kSkeletonBonesCountOffset);
                    if (boneCount > 0 && boneCount <= kMaxPlausibleBoneCount) {
                        for (std::int32_t i = 0; i < boneCount && i < 3; ++i) {
                            if (const char* name = skeletonBoneName(skeleton, i)) {
                                firstBoneNames[static_cast<std::size_t>(i)] = name;
                            }
                        }
                        if (const char* name = skeletonBoneName(skeleton, boneCount - 1)) {
                            lastBoneName = name;
                        }
                    }
                }
                if (plausiblePointer(bindingSet)) {
                    bindingsData = *reinterpret_cast<std::uintptr_t*>(bindingSet + kBindingSetDataOffset);
                    bindingCount = *reinterpret_cast<std::int32_t*>(bindingSet + kBindingSetCountOffset);
                }
            }
            RDX_LOG_WARN(Weapon,
                "WeaponClipMotionHarvest diagnostics [{}] graph[{}]{}: graph={:#x}(vt+{:#x}) charVt=+{:#x} "
                "setup={:#x}(vt+{:#x}) skel={:#x}(vt+{:#x}) bones={} first=[{}|{}|{}] last=[{}] "
                "setSrc={} set={:#x}(vt+{:#x}) data={:#x} count={}",
                label ? label : "?",
                index,
                index == activeGraphIndex ? "*" : "",
                rawGraph,
                objectVtableRel(rawGraph),
                graph != 0 ? objectVtableRel(character) : 0,
                setup,
                objectVtableRel(setup),
                skeleton,
                objectVtableRel(skeleton),
                boneCount,
                firstBoneNames[0],
                firstBoneNames[1],
                firstBoneNames[2],
                lastBoneName,
                bindingSetSource,
                bindingSet,
                objectVtableRel(bindingSet),
                bindingsData,
                bindingCount);
        }
    }

    StepResult stepHarvest(
        const void* graphManager,
        std::uint32_t weaponFormId,
        std::uint64_t weaponGenerationKey,
        const char* const* allowedNodeNames,
        std::uint32_t allowedNodeNameCount)
    {
        if (s_walkFormId != weaponFormId || s_walkGenerationKey != weaponGenerationKey) {
            s_walkFormId = weaponFormId;
            s_walkGenerationKey = weaponGenerationKey;
            s_walkGraphIndex = 0;
            s_walkBindingIndex = 0;
            s_walkBindingsData = 0;
            s_walkBindingsVisited = 0;
            s_walkDone = false;
            s_bindingDetailLogs = 0;
            s_walkPassIndex = 0;
            s_walkPassStartHarvested = s_bindingsHarvested.load(std::memory_order_relaxed);
            std::scoped_lock lock(s_hookMutex);
            s_processedBindingCount = 0;
            s_stageMarkerLogBudget = kStageMarkerLogBudgetPerGeneration;
        }
        if (s_walkDone) {
            return StepResult::Completed;
        }

        GraphArrayView graphs{};
        if (!resolveGraphArray(reinterpret_cast<std::uintptr_t>(graphManager), graphs)) {
            // Manager not readable yet; the caller retries next frame.
            return StepResult::Pending;
        }

        std::int32_t processed = 0;
        while (processed < kBindingsPerStep) {
            if (s_walkGraphIndex >= graphs.capacity) {
                if (s_walkBindingsVisited == 0) {
                    // Every graph was empty/unresolvable this pass; the sets
                    // may still be filling at equip — retry from the top
                    // until the caller's attempt budget runs out.
                    s_walkGraphIndex = 0;
                    s_walkBindingIndex = 0;
                    s_walkBindingsData = 0;
                    return StepResult::Pending;
                }
                s_walkDone = true;
                s_walksCompleted.fetch_add(1, std::memory_order_relaxed);
                const auto harvested = s_bindingsHarvested.load(std::memory_order_relaxed);
                // Periodic re-walk passes only log when something new landed.
                if (s_walkPassIndex == 0 || harvested != s_walkPassStartHarvested) {
                    RDX_LOG_INFO(Weapon,
                        "WeaponClipMotionHarvest: walked weapon {:08X} pass {} — {} graph slot(s), {} binding(s) visited, {} harvested, {} without part tracks (cumulative)",
                        weaponFormId,
                        s_walkPassIndex,
                        graphs.capacity,
                        s_walkBindingsVisited,
                        harvested,
                        s_bindingsNoTargets.load(std::memory_order_relaxed));
                }
                ++s_walkPassIndex;
                return StepResult::Completed;
            }

            ResolvedBindings resolved{};
            const auto graph = graphAtIndex(graphs, s_walkGraphIndex);
            if (!graph || !resolveGraphBindings(graph, resolved)) {
                // Empty slot, dummy graph, or empty binding set: next graph.
                ++s_walkGraphIndex;
                s_walkBindingIndex = 0;
                s_walkBindingsData = 0;
                continue;
            }
            if (resolved.bindingsData != s_walkBindingsData) {
                // Different binding set than the cursor was walking (rebuilt
                // at equip); indices are not comparable across sets, restart
                // this graph.
                s_walkBindingsData = resolved.bindingsData;
                s_walkBindingIndex = 0;
            }
            if (s_walkBindingIndex >= resolved.bindingCount) {
                ++s_walkGraphIndex;
                s_walkBindingIndex = 0;
                s_walkBindingsData = 0;
                continue;
            }

            const auto* bindings = reinterpret_cast<const std::uintptr_t*>(resolved.bindingsData);
            const auto bindingWithTriggers = bindings[s_walkBindingIndex++];
            ++processed;
            ++s_walkBindingsVisited;
            if (!plausiblePointer(bindingWithTriggers)) {
                continue;
            }
            const auto binding = *reinterpret_cast<std::uintptr_t*>(bindingWithTriggers + kBindingWithTriggersBindingOffset);
            if (!plausiblePointer(binding)) {
                continue;
            }
            {
                std::scoped_lock lock(s_hookMutex);
                if (bindingProcessedLocked(binding, /*fromActivation=*/false)) {
                    continue;
                }
            }
            s_bindingsSeen.fetch_add(1, std::memory_order_relaxed);
            // Walk strokes come from the merely-LOADED binding set — fallback
            // provenance; the clip name lives on the hkbClipGenerator, which
            // only the activation hook sees.
            if (harvestBinding(
                    binding,
                    resolved.skeleton,
                    allowedNodeNames,
                    allowedNodeNameCount,
                    /*fromActivation=*/false,
                    nullptr)) {
                std::scoped_lock lock(s_hookMutex);
                markBindingProcessedLocked(binding, /*fromActivation=*/false);
            }
        }
        return StepResult::Pending;
    }

    void resetWalk()
    {
        s_walkFormId = 0;
        s_walkGenerationKey = 0;
        s_walkGraphIndex = 0;
        s_walkBindingIndex = 0;
        s_walkBindingsData = 0;
        s_walkBindingsVisited = 0;
        s_walkDone = false;
        s_bindingDetailLogs = 0;
        s_walkPassIndex = 0;
        s_walkPassStartHarvested = s_bindingsHarvested.load(std::memory_order_relaxed);
        std::scoped_lock lock(s_hookMutex);
        s_processedBindingCount = 0;
        s_stageMarkerLogBudget = kStageMarkerLogBudgetPerGeneration;
    }

    void restartWalkPass()
    {
        s_walkGraphIndex = 0;
        s_walkBindingIndex = 0;
        s_walkBindingsData = 0;
        s_walkBindingsVisited = 0;
        s_walkDone = false;
        s_walkPassStartHarvested = s_bindingsHarvested.load(std::memory_order_relaxed);
        // The detail-log budget is intentionally NOT reset: bail dumps stay
        // capped per weapon generation so periodic re-walks cannot spam.
    }

    std::uint32_t drainGroups(weapon_clip_stroke::AuthoredStrokeGroup* outGroups, std::uint32_t maxGroups)
    {
        if (!outGroups || maxGroups == 0) {
            return 0;
        }
        std::scoped_lock lock(s_queueMutex);
        const auto count = (std::min)(maxGroups, s_queueCount);
        for (std::uint32_t i = 0; i < count; ++i) {
            outGroups[i] = s_queue[i];
        }
        if (count < s_queueCount) {
            for (std::uint32_t i = count; i < s_queueCount; ++i) {
                s_queue[i - count] = s_queue[i];
            }
        }
        s_queueCount -= count;
        return count;
    }

    void clearPending()
    {
        std::scoped_lock lock(s_queueMutex);
        s_queueCount = 0;
    }

    Stats snapshotStats()
    {
        return Stats{
            .bindingsSeen = s_bindingsSeen.load(std::memory_order_relaxed),
            .bindingsHarvested = s_bindingsHarvested.load(std::memory_order_relaxed),
            .bindingsNoTargets = s_bindingsNoTargets.load(std::memory_order_relaxed),
            .groupsQueued = s_groupsQueued.load(std::memory_order_relaxed),
            .groupsDropped = s_groupsDropped.load(std::memory_order_relaxed),
            .skippedNonSpline = s_skippedNonSpline.load(std::memory_order_relaxed),
            .walksCompleted = s_walksCompleted.load(std::memory_order_relaxed),
            .bailAnimationPtr = s_bailAnimationPtr.load(std::memory_order_relaxed),
            .bailClipParams = s_bailClipParams.load(std::memory_order_relaxed),
            .bailTrackMap = s_bailTrackMap.load(std::memory_order_relaxed),
            .bailBoneCount = s_bailBoneCount.load(std::memory_order_relaxed),
            .bailSampler = s_bailSampler.load(std::memory_order_relaxed),
            .bailSplineData = s_bailSplineData.load(std::memory_order_relaxed),
            .hookFires = s_hookFires.load(std::memory_order_relaxed),
            .hookActivations = s_hookActivations.load(std::memory_order_relaxed),
        };
    }
}
