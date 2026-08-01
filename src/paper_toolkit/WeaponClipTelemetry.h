#pragma once

#include <array>
#include <cstdint>

#include "paper_toolkit/RichMotionCaptureFormat.h"
#include "paper_toolkit/WeaponClipStrokePolicy.h"

namespace paper_toolkit::weapon_clip_telemetry
{
    /*
     * Passive live-clip telemetry. Weapon part tracks, markers, triggers, and
     * clip-time activity are copied into the append-only evidence plane. This
     * module never produces serving MotionPaths and never writes clip mode,
     * local time, or user-controlled fractions.
     *
     * Tracks are kept only when their bone name matches one of the caller's
     * weapon scene-node names, which on an actor graph filters out every
     * body clip.
     *
     * All engine access below the holder pointer retains the existing
     * FO4VR-verified offsets and plausibility gates; this refactor changes
     * ownership and mutation policy, not those binary contracts.
     *
     * Thread model: the graph lifecycle shims copy into fixed, locked packet
     * storage; the frame-thread walk is time-sliced so a large clip set cannot
     * hitch a frame. No walk pointer is retained between calls — the chain is
     * re-resolved from the holder every step.
     */

    enum class StepResult : std::uint32_t
    {
        // Bindings not available yet (graph still loading) or budget spent
        // for this call; call again next frame.
        Pending = 0,
        // Every binding of the weapon graph has been processed for this
        // (formId, generationKey).
        Completed = 1,
    };

    /*
     * Resolve a WeaponAnimationGraphManagerHolder's BSAnimationGraphManager
     * smart pointer (+0x18, plausibility-gated); null when unavailable. The
     * walk itself is manager-based because weapon clips can live either on a
     * weapon holder's own graph or — as in-game diagnostics showed for the
     * biped-slot holders (one-bone 'x_bone01' dummy rigs, 2026-07-04) — on
     * the ACTOR's graph manager after subgraph activation.
     */
    [[nodiscard]] const void* managerFromWeaponHolder(const void* weaponGraphHolder);

    /*
     * Advance the passive evidence walk over active graph bindings.
     * `graphManager` is a live BSAnimationGraphManager (non-owning; must not
     * be retained). Only clip tracks whose rig bone name matches one of
     * `allowedNodeNames` (weapon scene-node names; ':N' instancing suffix on
     * the node side is tolerated, comparison is case-insensitive) are
     * captured — on an actor graph this filters out every body clip. A
     * change of formId/generationKey, or of the underlying binding-set data
     * (graph swap mid-walk), resets the cursor automatically.
     */
    StepResult stepCapture(
        const void* graphManager,
        std::uint32_t weaponFormId,
        std::uint64_t weaponGenerationKey,
        const char* const* allowedNodeNames,
        std::uint32_t allowedNodeNameCount);

    // Full weapon-generation boundary: forget the walk cursor, processed
    // bindings, hook targets/identity, and live rich clip activities.
    void resetWalk();

    /*
     * Rewind the walk cursor for another pass over the same weapon
     * generation, keeping cumulative stats and the per-generation bail-dump
     * budget. Clip spline payloads stream in only while a clip is playing,
     * so the walk owner re-runs completed walks periodically — a reload
     * performed while the weapon is held makes its clips resident and the
     * next pass captures them.
     */
    void restartWalkPass();

    // Deepest holder-chain hop reached by the most recent resolve attempt
    // ("ok" when bindings were reachable); for give-up diagnostics.
    [[nodiscard]] const char* lastResolveStage();

    /*
     * Cheap pointer-walk probe: true when the manager's active graph
     * currently exposes a non-empty binding set. Used to pick the walk
     * target among several candidate managers — both biped-slot holder
     * copies carry a one-bone dummy rig ('x_bone01') with an empty set
     * (verified in-game 2026-07-04), so the first candidate whose set has
     * bindings wins.
     */
    [[nodiscard]] bool probeBindings(const void* graphManager);

    /*
     * FO4 streams clip spline payloads on demand: the binding-set entries
     * are permanent stubs (headers only, no sampleable data — in-game
     * confirmed: firing/reloading never fills them), and the loaded binding
     * lives on the hkbClipGenerator while its clip plays. This hook swaps
     * the hkbClipGenerator lifecycle slots and passively copies the freshly
     * resident binding plus per-frame activity time. Idempotent.
     */
    void ensureClipTelemetryHooksInstalled();

    /*
     * Register what the hook may capture: the characters of the candidate
     * managers' graphs (the hook fires for every actor, so anything else is
     * ignored) and the weapon's scene-node names (copied — the hook thread
     * never touches scene-graph memory). Refresh whenever the walk steps;
     * clear when the sandbox shuts down.
     */
    void setClipTelemetryTargets(
        const void* const* graphManagers,
        std::uint32_t managerCount,
        const char* const* allowedNodeNames,
        std::uint32_t allowedNodeNameCount,
        std::uint32_t weaponFormId,
        std::uint64_t weaponGenerationKey);
    void clearClipTelemetryTargets();

    /*
     * Optional case-insensitive diagnostic filter for verbose per-track name
     * lines. It does not filter structured capture packets or alter clips.
     */
    void setDiagnosticClipFilter(const char* clipNameFilter);

    /*
     * Raw clip evidence for the append-only mapper archive. The
     * activation/walk code already owns the exact 64-sample weapon tracks;
     * this queue preserves them without reducing them into serving paths.
     * Annotation/trigger arrays are bounded
     * and carry explicit truncation flags. Graph thread produces, main
     * thread drains; all storage is fixed-capacity.
     */
    inline constexpr std::size_t kMaxCapturedClipAnnotations = 128;
    inline constexpr std::size_t kMaxCapturedClipTriggers = 128;
    inline constexpr std::size_t kMaxCapturedMarkerText = 96;

    struct CapturedClipAnnotation
    {
        float timeSeconds{ 0.0f };
        std::array<char, weapon_clip_stroke::kMaxBoneName> trackName{};
        std::array<char, kMaxCapturedMarkerText> text{};
    };

    struct CapturedClipTrigger
    {
        float localTimeSeconds{ 0.0f };
        std::int32_t eventId{ -1 };
        std::array<char, kMaxCapturedMarkerText> eventName{};
    };

    struct RichClipCapturePacket
    {
        std::uint32_t weaponFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint64_t activityId{ 0 };
        rich_capture::ClipAcquisition acquisition{
            rich_capture::ClipAcquisition::LoadedGraphBinding
        };
        std::array<char, weapon_clip_stroke::kMaxBoneName> animationName{};
        float durationSeconds{ 0.0f };
        std::uint32_t rawTransformTrackCount{ 0 };
        std::uint32_t capturedWeaponTrackCount{ 0 };
        bool weaponTracksTruncated{ false };
        std::array<weapon_clip_stroke::TrackSamples, weapon_clip_stroke::kMaxTracksPerClip> weaponTracks{};
        std::int32_t rawAnnotationTrackCount{ 0 };
        std::int32_t rawTriggerCount{ 0 };
        std::int32_t graphEventNameCount{ 0 };
        std::uint32_t annotationCount{ 0 };
        std::uint32_t triggerCount{ 0 };
        bool annotationsTruncated{ false };
        bool triggersTruncated{ false };
        std::array<CapturedClipAnnotation, kMaxCapturedClipAnnotations> annotations{};
        std::array<CapturedClipTrigger, kMaxCapturedClipTriggers> triggers{};
    };
    static_assert(sizeof(RichClipCapturePacket) < 512 * 1024,
        "live graph-thread capture must not inherit exact preharvest sample storage");

    void setRichCaptureEnabled(bool enabled);
    // Generation transition barrier: discard only live clip-activity
    // telemetry. Authored capture packets remain queued with their own
    // stamped weapon provenance.
    void clearRichClipActivities();
    std::uint32_t drainRichClipCaptures(RichClipCapturePacket* outPackets, std::uint32_t maxPackets);
    struct RichClipDropInfo
    {
        std::uint64_t count{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
    };
    [[nodiscard]] RichClipDropInfo drainRichClipDropInfo();

    struct RichClipActivityState
    {
        bool active{ false };
        // Multiple behavior-graph layers can drive weapon tracks at once;
        // the remaining fields describe the newest active layer.
        std::uint32_t concurrentActivityCount{ 0 };
        std::uint64_t activityId{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::array<char, weapon_clip_stroke::kMaxBoneName> animationName{};
        float durationSeconds{ 0.0f };
        float cropStartSeconds{ 0.0f };
        float croppedDurationSeconds{ 0.0f };
        float localTimeSeconds{ 0.0f };
        float fraction{ 0.0f };
    };
    [[nodiscard]] RichClipActivityState richClipActivityState();

    /*
     * One-shot dump of the manager→bindings chain: raw pointer of every hop,
     * each object's vtable rebased to a module offset (identifies the actual
     * runtime type in Ghidra), skeleton bone count/names, and the binding
     * set's raw data/count. Called by the walk owner when it gives up, so a
     * failing hop can be diagnosed from the log without a debugger. Reads are
     * plausibility-gated the same way as the resolve itself. `label` names
     * the candidate in the log (e.g. "weapon-holder" / "actor").
     */
    void logResolveDiagnostics(const void* graphManager, const char* label);

    struct Stats
    {
        std::uint64_t bindingsSeen{ 0 };
        std::uint64_t bindingsCaptured{ 0 };
        std::uint64_t bindingsNoTargets{ 0 };
        std::uint64_t skippedNonSpline{ 0 };
        std::uint64_t walksCompleted{ 0 };
        // Which captureBindingEvidence gate rejected bindings (details are dumped,
        // capped per walk, as "binding bail" warnings).
        std::uint64_t bailAnimationPtr{ 0 };
        std::uint64_t bailClipParams{ 0 };
        std::uint64_t bailTrackMap{ 0 };
        std::uint64_t bailBoneCount{ 0 };
        std::uint64_t bailSampler{ 0 };
        // Spline payload not resident (engine sampler would crash on it).
        std::uint64_t bailSplineData{ 0 };
        // Clip-activation hook: total shim entries / entries that passed the
        // registered-character filter.
        std::uint64_t hookFires{ 0 };
        std::uint64_t hookActivations{ 0 };
    };
    [[nodiscard]] Stats snapshotStats();
}
