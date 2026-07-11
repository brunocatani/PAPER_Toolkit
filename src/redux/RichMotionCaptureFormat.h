#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "redux/MotionLibraryFormat.h"
#include "redux/WeaponClipStrokePolicy.h"
#include "redux/WeaponPartMotionPathPolicy.h"

/*
 * Append-only evidence format for weapon mapping.
 *
 * The normal MotionLibrary .json remains the small, mutable SERVING map that
 * the game loads on equip.  These events are written to a sibling
 * .capture.jsonl file and are never loaded by gameplay.  Keeping evidence in
 * an append-only plane is the central invariant: path selection may replace a
 * winner, but it can never erase a raw recording, rejected candidate, loadout
 * inventory, ROCK classification, geometry cloud, or authored clip sample.
 *
 * This module is engine-free.  JSON construction happens on the background
 * writer thread; event-scoped vectors deliberately own everything copied from
 * transient ROCK/scene-graph pointers before the frame callback returns.
 */
namespace redux::rich_capture
{
    inline constexpr std::uint32_t kSchemaVersion = 1;
    inline constexpr std::string_view kSchemaName = "paper-redux-motion-capture";

    struct FormInfo
    {
        motion_library::FormRef ref;
        // Diagnostics only. Runtime IDs are never persistent identity.
        std::uint32_t runtimeFormId{ 0 };
        std::uint32_t formType{ 0 };
        std::string editorId;
        std::string displayName;
    };

    struct EventContext
    {
        std::string sessionId;
        std::uint64_t sequence{ 0 };
        std::uint64_t capturedAtUnixMs{ 0 };
        std::uint64_t rockFrameIndex{ 0 };
        // Diagnostic only; load-order and session dependent.
        std::uint64_t weaponGenerationKey{ 0 };
        FormInfo weapon;
        std::string paperVersion;
        std::uint32_t rockApiVersion{ 0 };
        std::string rockModVersion;
        std::uint32_t rockFeatureBits{ 0 };
    };

    struct Point3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    struct Bounds3
    {
        bool valid{ false };
        Point3 min{};
        Point3 max{};
    };

    struct NodeSnapshot
    {
        std::uint32_t id{ 0 };
        std::int32_t parentId{ -1 };
        std::uint32_t childIndex{ 0 };
        std::uint32_t sameNameSiblingOrdinal{ 0 };
        std::string name;
        // Human-readable diagnostic; parentId + childIndex is canonical.
        std::string rootRelativePath;
        bool isNode{ false };
        std::uint32_t childCount{ 0 };
        bool localPoseValid{ false };
        weapon_part_motion_path::PoseSample localPose{};
        float localScale{ 1.0f };
        bool weaponLocalPoseValid{ false };
        weapon_part_motion_path::PoseSample weaponLocalPose{};
        float weaponLocalScale{ 1.0f };
    };

    enum class GeometryDelivery : std::uint32_t
    {
        None = 0,
        Embedded = 1,
        Chunks = 2,
    };

    struct EvidenceSnapshot
    {
        // Stable only inside this generation snapshot. Persistent identity is
        // weapon + loadout/form refs + structural node path.
        std::uint32_t id{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFFu };
        std::string providerSourceName;
        std::int32_t sourceNodeId{ -1 };
        std::int32_t interactionNodeId{ -1 };
        std::string sourceNodePath;
        std::string interactionNodePath;
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uint32_t fallbackGripPose{ 0 };
        std::uint32_t classificationSource{ 0 };
        FormInfo omod;
        FormInfo attachPoint;
        Bounds3 localBoundsGame{};
        std::uint32_t providerPointCount{ 0 };
        std::uint32_t queriedPointCount{ 0 };
        std::uint32_t copiedPointCount{ 0 };
        // Number of source points the runtime intends to deliver after this
        // metadata row. For Chunks, copiedPointCount remains zero here and
        // the point payload follows in ordered GeometryChunkEvent rows.
        std::uint32_t scheduledPointCount{ 0 };
        GeometryDelivery geometryDelivery{ GeometryDelivery::None };
        bool pointCloudTruncated{ false };
        // Retained for schema-v1 readers and small embedded captures. New
        // full-fidelity snapshots normally use GeometryDelivery::Chunks.
        std::vector<Point3> pointsWeaponLocalGame;
    };

    struct ObservationTarget
    {
        std::uint32_t catalogPartId{ 0 };
        std::int32_t evidenceId{ -1 };
        std::int32_t nodeId{ -1 };
        std::uint32_t bodyId{ 0x7FFF'FFFFu };
        bool observationOnly{ false };
        std::string sourceName;
        FormInfo omod;
    };

    struct CaptureSettings
    {
        std::string motionPathMode;
        bool fullSubtreeObservation{ false };
        bool coTimedFollowers{ false };
        float coTimedMinOverlap{ 0.0f };
        float coTimedMaxArcRatio{ 0.0f };
        float rigidFollowerDistanceToleranceGameUnits{ 0.0f };
        float followerMinimumExcursionGameUnits{ 0.0f };
        std::uint32_t minimumOverlapSamples{ 0 };
        bool stageTransitions{ false };
        float stageChainToleranceGameUnits{ 0.0f };
        float translationStillEpsilonGameUnits{ 0.0f };
        float rotationStillEpsilonRadians{ 0.0f };
        float rotationArcRadiusGameUnits{ 0.0f };
        float minimumPathExcursionGameUnits{ 0.0f };
        float replacementRatio{ 0.0f };
        std::uint32_t restFramesToArm{ 0 };
        std::uint32_t settleFramesToComplete{ 0 };
        std::uint32_t maximumRawSamples{ 0 };
        std::uint32_t resampledKeyCount{ 0 };
        std::uint32_t maximumObservedParts{ 0 };
    };

    struct WeaponClassification
    {
        bool available{ false };
        bool valid{ false };
        std::uint64_t keywordFlags{ 0 };
        std::uint32_t sizeClass{ 0 };
        std::uint32_t source{ 0 };
        std::uint32_t runtimeFormId{ 0 };
    };

    struct WeaponSnapshotEvent
    {
        EventContext context;
        WeaponClassification classification{};
        CaptureSettings settings{};
        std::uint32_t providerEvidenceCount{ 0 };
        std::uint32_t copiedEvidenceCount{ 0 };
        bool evidenceTruncated{ false };
        std::uint32_t discoveredNodeCount{ 0 };
        std::uint32_t omittedNodeCount{ 0 };
        bool nodeCatalogTruncated{ false };
        std::uint32_t observedTargetCapacity{ 0 };
        std::uint32_t omittedObservationTargetCount{ 0 };
        std::vector<NodeSnapshot> nodes;
        std::vector<EvidenceSnapshot> evidence;
        std::vector<ObservationTarget> observationTargets;
    };

    struct GeometryChunkEvent
    {
        EventContext context;
        // Links this payload to the sequence of its WeaponSnapshotEvent. It
        // is serialized as a decimal string so values above 2^53 stay exact.
        std::uint64_t snapshotSequence{ 0 };
        std::uint32_t evidenceId{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFFu };
        std::uint32_t pointOffset{ 0 };
        std::uint32_t totalPointCount{ 0 };
        std::uint32_t chunkIndex{ 0 };
        // finalChunk describes delivery, while sourceComplete describes
        // fidelity. A capped/short provider copy can be final but incomplete.
        bool finalChunk{ false };
        bool sourceComplete{ false };
        // True only on the final emitted chunk for the entire snapshot.
        bool snapshotComplete{ false };
        std::vector<Point3> pointsWeaponLocalGame;
    };

    enum class StrokeTermination : std::uint32_t
    {
        Settled = 0,
        UntrustedDrive = 1,
        SampleCapacity = 2,
        RecorderReclaimed = 3,
        WeaponChanged = 4,
        RuntimeReset = 5,
        CaptureDisabled = 6,
        RuntimeShutdown = 7,
    };

    enum class ServingDecision : std::uint32_t
    {
        NotEvaluated = 0,
        RejectedBelowNoise = 1,
        RejectedInvalidPath = 2,
        RejectedSmallerPrimary = 3,
        RejectedSmallerReturn = 4,
        NoServingSlot = 5,
        StoredPrimary = 6,
        ReplacedPrimary = 7,
        StoredReturn = 8,
        ReplacedReturn = 9,
    };

    struct ClipSampleContext
    {
        std::uint64_t activityId{ 0 };
        std::uint64_t scrubSessionId{ 0 };
        std::uint32_t concurrentActivityCount{ 0 };
        float fraction{ 0.0f };
        float localTimeSeconds{ 0.0f };
    };

    struct RawSample
    {
        std::uint64_t rockFrameIndex{ 0 };
        weapon_part_motion_path::PoseSample pose{};
        float scale{ 1.0f };
        bool trusted{ true };
        ClipSampleContext clip{};
    };

    struct SelectedFollower
    {
        std::uint32_t catalogPartId{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFFu };
        FormInfo omod;
        std::string sourceName;
        std::string tier;
    };

    struct RawStrokeEvent
    {
        EventContext context;
        std::uint32_t catalogPartId{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFFu };
        FormInfo omod;
        std::string sourceName;
        std::string nodePath;
        bool nodePathTruncated{ false };
        StrokeTermination termination{ StrokeTermination::Settled };
        ServingDecision servingDecision{ ServingDecision::NotEvaluated };
        std::uint64_t learnerStartFrame{ 0 };
        std::uint32_t peakSampleIndex{ 0 };
        float peakExcursion{ 0.0f };
        float fullRecordingArcLength{ 0.0f };
        bool candidateValid{ false };
        weapon_part_motion_path::MotionPath candidatePath{};
        bool classifiedAsReturnStage{ false };
        bool replacedServingPath{ false };
        std::uint32_t selectedRigidFollowerCount{ 0 };
        std::uint32_t selectedCoTimedFollowerCount{ 0 };
        std::vector<SelectedFollower> selectedFollowers;
        CaptureSettings settings{};
        std::string clipName;
        float clipDurationSeconds{ 0.0f };
        float clipCroppedDurationSeconds{ 0.0f };
        std::vector<RawSample> samples;
        bool terminalSamplePresent{ false };
        RawSample terminalSample{};
    };

    struct ClipAnnotation
    {
        float timeSeconds{ 0.0f };
        std::string trackName;
        std::string text;
    };

    struct ClipTrigger
    {
        float localTimeSeconds{ 0.0f };
        std::int32_t eventId{ -1 };
        std::string eventName;
    };

    struct ClipTrack
    {
        std::string boneName;
        std::vector<weapon_part_motion_path::PoseSample> samples;
        std::vector<Point3> scaleSamples;
    };

    struct AuthoredClipEvent
    {
        EventContext context;
        std::uint64_t activityId{ 0 };
        bool activatedClip{ false };
        std::string animationName;
        float durationSeconds{ 0.0f };
        std::uint32_t rawTransformTrackCount{ 0 };
        std::uint32_t capturedWeaponTrackCount{ 0 };
        bool weaponTracksTruncated{ false };
        std::int32_t rawAnnotationTrackCount{ 0 };
        std::int32_t rawTriggerCount{ 0 };
        std::int32_t graphEventNameCount{ 0 };
        bool annotationsTruncated{ false };
        bool triggersTruncated{ false };
        std::vector<ClipTrack> weaponTracks;
        std::vector<ClipAnnotation> annotations;
        std::vector<ClipTrigger> triggers;
    };

    struct CaptureGapEvent
    {
        EventContext context;
        // Optional linkage when the incompleteness belongs to a deferred
        // weapon snapshot rather than a known range of queued sequences.
        std::uint64_t relatedSnapshotSequence{ 0 };
        std::uint64_t firstDroppedSequence{ 0 };
        std::uint64_t lastDroppedSequence{ 0 };
        std::uint32_t droppedEventCount{ 0 };
        std::string reason;
    };

    using Event = std::variant<WeaponSnapshotEvent, GeometryChunkEvent, RawStrokeEvent, AuthoredClipEvent, CaptureGapEvent>;

    [[nodiscard]] const EventContext& contextOf(const Event& event);
    [[nodiscard]] std::size_t estimateOwnedBytes(const Event& event);
    [[nodiscard]] std::string serializeLine(const Event& event);
}
