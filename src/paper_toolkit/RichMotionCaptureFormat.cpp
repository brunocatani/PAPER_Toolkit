#include "paper_toolkit/RichMotionCaptureFormat.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <type_traits>

namespace paper_toolkit::rich_capture
{
    namespace
    {
        using nlohmann::json;
        using weapon_part_motion_path::MotionPath;
        using weapon_part_motion_path::PoseSample;

        std::string decimal64(std::uint64_t value)
        {
            return std::to_string(value);
        }

        std::string hex32(std::uint32_t value)
        {
            char text[16]{};
            std::snprintf(text, sizeof(text), "0x%08X", value);
            return text;
        }

        std::string hex64(std::uint64_t value)
        {
            char text[24]{};
            std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(value));
            return text;
        }

        json poseToJson(const PoseSample& pose)
        {
            return json::array({
                pose.translate.x,
                pose.translate.y,
                pose.translate.z,
                pose.rotate.w,
                pose.rotate.x,
                pose.rotate.y,
                pose.rotate.z,
            });
        }

        json pointToJson(const Point3& point)
        {
            return json::array({ point.x, point.y, point.z });
        }

        json formToJson(const FormInfo& form)
        {
            json out;
            if (!form.ref.empty()) {
                out["ref"] = {
                    { "plugin", form.ref.plugin },
                    { "id", hex32(form.ref.localFormId) },
                };
            } else {
                out["ref"] = nullptr;
            }
            out["runtimeIdDiagnostic"] = hex32(form.runtimeFormId);
            out["formType"] = form.formType;
            if (!form.editorId.empty()) {
                out["editorId"] = form.editorId;
            }
            if (!form.displayName.empty()) {
                out["name"] = form.displayName;
            }
            return out;
        }

        json contextToJson(const EventContext& context)
        {
            return json{
                { "schema", kSchemaName },
                { "schemaVersion", kSchemaVersion },
                { "sessionId", context.sessionId },
                { "sequence", decimal64(context.sequence) },
                { "capturedAtUnixMs", decimal64(context.capturedAtUnixMs) },
                { "rockFrame", decimal64(context.rockFrameIndex) },
                { "weaponGenerationDiagnostic", hex64(context.weaponGenerationKey) },
                { "weapon", formToJson(context.weapon) },
                { "paperVersion", context.paperVersion },
                { "rockApiVersion", context.rockApiVersion },
                { "rockModVersion", context.rockModVersion },
                { "rockFeatureBits", hex32(context.rockFeatureBits) },
            };
        }

        template <std::size_t N>
        const char* enumName(std::uint32_t value, const std::array<const char*, N>& names)
        {
            return value < names.size() ? names[value] : "Unknown";
        }

        const char* partKindName(std::uint32_t value)
        {
            static constexpr std::array names{
                "Receiver", "Barrel", "Handguard", "Foregrip", "Pump", "Stock", "Grip", "Magazine",
                "Magwell", "Bolt", "Slide", "ChargingHandle", "BreakAction", "Cylinder", "Chamber", "Shell",
                "Round", "LaserCell", "Lever", "Sight", "Accessory", "CosmeticAmmo", "Other",
                "LaserSight", "Flashlight", "LaserFlashlightCombo", "Scope", "MuzzleDevice", "Bipod",
            };
            return enumName(value, names);
        }

        const char* reloadRoleName(std::uint32_t value)
        {
            static constexpr std::array names{ "None", "MagazineBody", "AmmoPiece", "CosmeticAmmo", "Receiver" };
            return enumName(value, names);
        }

        const char* supportRoleName(std::uint32_t value)
        {
            static constexpr std::array names{
                "None", "SupportSurface", "Foregrip", "PumpGrip", "MagwellHold", "StockForward", "ReceiverSupport",
            };
            return enumName(value, names);
        }

        const char* socketRoleName(std::uint32_t value)
        {
            static constexpr std::array names{ "None", "Magwell", "Chamber", "Cylinder", "LaserCell", "LoadingGate" };
            return enumName(value, names);
        }

        const char* actionRoleName(std::uint32_t value)
        {
            static constexpr std::array names{
                "None", "Bolt", "Slide", "ChargingHandle", "Pump", "BreakAction", "Cylinder", "Lever", "Latch",
            };
            return enumName(value, names);
        }

        const char* gripPoseName(std::uint32_t value)
        {
            static constexpr std::array names{
                "None", "BarrelWrap", "HandguardClamp", "VerticalForegrip", "AngledForegrip", "PumpGrip",
                "MagwellHold", "ReceiverSupport",
            };
            return enumName(value, names);
        }

        const char* classificationSourceName(std::uint32_t value)
        {
            static constexpr std::array names{ "NameToken", "SlotAnchor", "RigAnchor" };
            return enumName(value, names);
        }

        const char* geometryDeliveryName(GeometryDelivery delivery)
        {
            switch (delivery) {
            case GeometryDelivery::None:
                return "none";
            case GeometryDelivery::Embedded:
                return "embedded";
            case GeometryDelivery::Chunks:
                return "chunks";
            }
            return "unknown";
        }

        const char* weaponSizeName(std::uint32_t value)
        {
            static constexpr std::array names{ "Melee", "Pistol", "Rifle", "Heavy" };
            return enumName(value, names);
        }

        const char* weaponClassSourceName(std::uint32_t value)
        {
            static constexpr std::array names{ "None", "Keyword", "WeightFallback", "Default" };
            return enumName(value, names);
        }

        json keywordNames(std::uint64_t flags)
        {
            static constexpr std::array<const char*, 35> names{
                "Pistol", "Rifle", "Shotgun", "AssaultRifle", "Sniper", "GaussRifle", "LaserMusket", "HeavyGun",
                "HandToHand", "Melee1H", "Melee2H", "Unarmed", "Minigun", "Fatman", "MissileLauncher",
                "GatlingLaser", "Flamer", "Cryolater", "JunkJet", "RailwayRifle", "Broadsider", "Syringer",
                "FlareGun", "GammaGun", "AlienBlaster", "Ripper", "Shishkebab", "Laser", "Plasma", "Ballistic",
                "Thrown", "Grenade", "Mine", "Explosive", "Automatic",
            };
            json out = json::array();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if ((flags & (std::uint64_t{ 1 } << i)) != 0) {
                    out.push_back(names[i]);
                }
            }
            return out;
        }

        json settingsToJson(const CaptureSettings& settings)
        {
            return json{
                { "motionPathMode", settings.motionPathMode },
                { "fullSubtreeObservation", settings.fullSubtreeObservation },
                { "coTimedFollowers", settings.coTimedFollowers },
                { "coTimedMinOverlap", settings.coTimedMinOverlap },
                { "coTimedMaxArcRatio", settings.coTimedMaxArcRatio },
                { "rigidFollowerDistanceToleranceGameUnits", settings.rigidFollowerDistanceToleranceGameUnits },
                { "followerMinimumExcursionGameUnits", settings.followerMinimumExcursionGameUnits },
                { "minimumOverlapSamples", settings.minimumOverlapSamples },
                { "stageTransitions", settings.stageTransitions },
                { "stageChainToleranceGameUnits", settings.stageChainToleranceGameUnits },
                { "translationStillEpsilonGameUnits", settings.translationStillEpsilonGameUnits },
                { "rotationStillEpsilonRadians", settings.rotationStillEpsilonRadians },
                { "rotationArcRadiusGameUnits", settings.rotationArcRadiusGameUnits },
                { "minimumPathExcursionGameUnits", settings.minimumPathExcursionGameUnits },
                { "replacementRatio", settings.replacementRatio },
                { "restFramesToArm", settings.restFramesToArm },
                { "settleFramesToComplete", settings.settleFramesToComplete },
                { "maximumRawSamples", settings.maximumRawSamples },
                { "resampledKeyCount", settings.resampledKeyCount },
                { "maximumObservedParts", settings.maximumObservedParts },
            };
        }

        json pathToJson(const MotionPath& path)
        {
            if (!path.valid) {
                return nullptr;
            }
            json keys = json::array();
            for (const auto& key : path.keys) {
                keys.push_back(poseToJson(key));
            }
            return json{
                { "arcLength", path.totalArcLength },
                { "keys", std::move(keys) },
            };
        }

        const char* terminationName(StrokeTermination termination)
        {
            switch (termination) {
            case StrokeTermination::Settled:
                return "settled";
            case StrokeTermination::UntrustedDrive:
                return "untrustedDrive";
            case StrokeTermination::SampleCapacity:
                return "sampleCapacity";
            case StrokeTermination::RecorderReclaimed:
                return "recorderReclaimed";
            case StrokeTermination::WeaponChanged:
                return "weaponChanged";
            case StrokeTermination::RuntimeReset:
                return "runtimeReset";
            case StrokeTermination::CaptureDisabled:
                return "captureDisabled";
            case StrokeTermination::RuntimeShutdown:
                return "runtimeShutdown";
            }
            return "unknown";
        }

        const char* servingDecisionName(ServingDecision decision)
        {
            switch (decision) {
            case ServingDecision::NotEvaluated:
                return "notEvaluated";
            case ServingDecision::RejectedBelowNoise:
                return "rejectedBelowNoise";
            case ServingDecision::RejectedInvalidPath:
                return "rejectedInvalidPath";
            case ServingDecision::RejectedSmallerPrimary:
                return "rejectedSmallerPrimary";
            case ServingDecision::RejectedSmallerReturn:
                return "rejectedSmallerReturn";
            case ServingDecision::NoServingSlot:
                return "noServingSlot";
            case ServingDecision::StoredPrimary:
                return "storedPrimary";
            case ServingDecision::ReplacedPrimary:
                return "replacedPrimary";
            case ServingDecision::StoredReturn:
                return "storedReturn";
            case ServingDecision::ReplacedReturn:
                return "replacedReturn";
            }
            return "unknown";
        }

        json rawSampleToJson(const RawSample& sample)
        {
            // Compact positional encoding is intentional: raw strokes are by
            // far the largest event type and the field order is versioned.
            return json::array({
                decimal64(sample.rockFrameIndex),
                sample.pose.translate.x,
                sample.pose.translate.y,
                sample.pose.translate.z,
                sample.pose.rotate.w,
                sample.pose.rotate.x,
                sample.pose.rotate.y,
                sample.pose.rotate.z,
                sample.scale,
                sample.trusted ? 1 : 0,
                decimal64(sample.clip.activityId),
                decimal64(sample.clip.scrubSessionId),
                sample.clip.concurrentActivityCount,
                sample.clip.fraction,
                sample.clip.localTimeSeconds,
            });
        }

        json serializeWeaponSnapshot(const WeaponSnapshotEvent& event)
        {
            json out = contextToJson(event.context);
            out["event"] = "weaponSnapshot";
            out["coordinateConventions"] = {
                { "nodeWeaponLocalPoses", "weapon-root-local" },
                { "nodeLocalPoses", "direct-parent-local" },
                { "geometryPoints", "weapon-root-local" },
                { "translationUnits", "Fallout game units" },
                { "quaternionOrder", "w,x,y,z" },
                { "scale", "unitless" },
            };
            out["classification"] = {
                { "available", event.classification.available },
                { "valid", event.classification.valid },
                { "keywordFlags", hex64(event.classification.keywordFlags) },
                { "keywords", keywordNames(event.classification.keywordFlags) },
                { "sizeClass", event.classification.sizeClass },
                { "sizeClassName", event.classification.available && event.classification.valid
                    ? weaponSizeName(event.classification.sizeClass)
                    : "Unavailable" },
                { "source", event.classification.source },
                { "sourceName", event.classification.available
                    ? weaponClassSourceName(event.classification.source)
                    : "Unavailable" },
                { "runtimeFormIdDiagnostic", hex32(event.classification.runtimeFormId) },
            };
            out["settings"] = settingsToJson(event.settings);
            out["completeness"] = {
                { "providerEvidenceCount", event.providerEvidenceCount },
                { "copiedEvidenceCount", event.copiedEvidenceCount },
                { "evidenceTruncated", event.evidenceTruncated },
                { "discoveredNodeCount", event.discoveredNodeCount },
                { "omittedNodeCount", event.omittedNodeCount },
                { "nodeCatalogTruncated", event.nodeCatalogTruncated },
                { "observedTargetCapacity", event.observedTargetCapacity },
                { "omittedObservationTargetCount", event.omittedObservationTargetCount },
            };

            json nodes = json::array();
            for (const auto& node : event.nodes) {
                nodes.push_back({
                    { "id", node.id },
                    { "parentId", node.parentId },
                    { "childIndex", node.childIndex },
                    { "sameNameSiblingOrdinal", node.sameNameSiblingOrdinal },
                    { "name", node.name },
                    { "path", node.rootRelativePath },
                    { "isNode", node.isNode },
                    { "childCount", node.childCount },
                    { "localPoseValid", node.localPoseValid },
                    { "localPose", poseToJson(node.localPose) },
                    { "localScale", node.localScale },
                    { "weaponLocalPoseValid", node.weaponLocalPoseValid },
                    { "weaponLocalPose", poseToJson(node.weaponLocalPose) },
                    { "weaponLocalScale", node.weaponLocalScale },
                });
            }
            out["nodes"] = std::move(nodes);

            json evidence = json::array();
            for (const auto& part : event.evidence) {
                json points = json::array();
                for (const auto& point : part.pointsWeaponLocalGame) {
                    points.push_back(pointToJson(point));
                }
                // Treat existing schema-v1 producers that populate the
                // embedded vector without setting the new enum as embedded.
                const auto delivery =
                    part.geometryDelivery == GeometryDelivery::None && !part.pointsWeaponLocalGame.empty()
                    ? GeometryDelivery::Embedded
                    : part.geometryDelivery;
                evidence.push_back({
                    { "id", part.id },
                    { "bodyIdDiagnostic", part.bodyId },
                    { "providerSourceName", part.providerSourceName },
                    { "sourceNodeId", part.sourceNodeId },
                    { "interactionNodeId", part.interactionNodeId },
                    { "sourceNodePath", part.sourceNodePath },
                    { "interactionNodePath", part.interactionNodePath },
                    { "partKind", part.partKind },
                    { "partKindName", partKindName(part.partKind) },
                    { "reloadRole", part.reloadRole },
                    { "reloadRoleName", reloadRoleName(part.reloadRole) },
                    { "supportRole", part.supportRole },
                    { "supportRoleName", supportRoleName(part.supportRole) },
                    { "socketRole", part.socketRole },
                    { "socketRoleName", socketRoleName(part.socketRole) },
                    { "actionRole", part.actionRole },
                    { "actionRoleName", actionRoleName(part.actionRole) },
                    { "fallbackGripPose", part.fallbackGripPose },
                    { "fallbackGripPoseName", gripPoseName(part.fallbackGripPose) },
                    { "classificationSource", part.classificationSource },
                    { "classificationSourceName", classificationSourceName(part.classificationSource) },
                    { "omod", formToJson(part.omod) },
                    { "attachPoint", formToJson(part.attachPoint) },
                    { "localBoundsGame", {
                        { "valid", part.localBoundsGame.valid },
                        { "min", pointToJson(part.localBoundsGame.min) },
                        { "max", pointToJson(part.localBoundsGame.max) },
                    } },
                    { "providerPointCount", part.providerPointCount },
                    { "queriedPointCount", part.queriedPointCount },
                    { "copiedPointCount", part.copiedPointCount },
                    { "scheduledPointCount", part.scheduledPointCount },
                    { "geometryDelivery", geometryDeliveryName(delivery) },
                    { "geometryDeferred", delivery == GeometryDelivery::Chunks },
                    { "pointCloudTruncated", part.pointCloudTruncated },
                    { "pointsWeaponLocalGame", std::move(points) },
                });
            }
            out["evidence"] = std::move(evidence);

            json targets = json::array();
            for (const auto& target : event.observationTargets) {
                targets.push_back({
                    { "catalogPartId", target.catalogPartId },
                    { "evidenceId", target.evidenceId },
                    { "nodeId", target.nodeId },
                    { "bodyIdDiagnostic", target.bodyId },
                    { "observationOnly", target.observationOnly },
                    { "sourceName", target.sourceName },
                    { "omod", formToJson(target.omod) },
                });
            }
            out["observationTargets"] = std::move(targets);
            return out;
        }

        json serializeGeometryChunk(const GeometryChunkEvent& event)
        {
            json out = contextToJson(event.context);
            out["event"] = "geometryChunk";
            out["snapshotSequence"] = decimal64(event.snapshotSequence);
            out["coordinateConventions"] = {
                { "pointsWeaponLocalGame", "weapon-root-local" },
                { "translationUnits", "Fallout game units" },
                { "pointOrder", "provider source order; pointOffset is zero-based within this evidence point cloud" },
            };
            out["evidenceId"] = event.evidenceId;
            out["bodyIdDiagnostic"] = event.bodyId;
            out["pointOffset"] = event.pointOffset;
            out["totalPointCount"] = event.totalPointCount;
            out["chunkIndex"] = event.chunkIndex;
            out["pointCount"] = event.pointsWeaponLocalGame.size();
            out["finalChunk"] = event.finalChunk;
            out["sourceComplete"] = event.sourceComplete;
            out["evidenceComplete"] = event.finalChunk && event.sourceComplete;
            out["snapshotComplete"] = event.snapshotComplete;
            json points = json::array();
            for (const auto& point : event.pointsWeaponLocalGame) {
                points.push_back(pointToJson(point));
            }
            out["pointsWeaponLocalGame"] = std::move(points);
            return out;
        }

        json serializeRawStroke(const RawStrokeEvent& event)
        {
            json out = contextToJson(event.context);
            out["event"] = "rawStroke";
            out["coordinateConventions"] = {
                { "poses", "weapon-root-local" },
                { "translationUnits", "Fallout game units" },
                { "quaternionOrder", "w,x,y,z" },
                { "scale", "weapon-root-local scalar" },
                { "sampling", "one trusted ROCK frame observation per sample; explicit frame ids may contain gaps" },
                { "clipAttribution", "newest active weapon-track clip; concurrentClipActivityCount reports layered ambiguity" },
                { "clipIds", "clipActivityId links authoredClip definitions; clipScrubSessionId is a separate live-scrub namespace" },
            };
            out["part"] = {
                { "catalogPartId", event.catalogPartId },
                { "bodyIdDiagnostic", event.bodyId },
                { "omod", formToJson(event.omod) },
                { "sourceName", event.sourceName },
                { "nodePath", event.nodePath },
                { "nodePathTruncated", event.nodePathTruncated },
            };
            out["termination"] = terminationName(event.termination);
            out["servingDecision"] = servingDecisionName(event.servingDecision);
            out["learnerStartFrame"] = decimal64(event.learnerStartFrame);
            out["peakSampleIndex"] = event.peakSampleIndex;
            out["peakExcursion"] = event.peakExcursion;
            out["fullRecordingArcLength"] = event.fullRecordingArcLength;
            out["candidateValid"] = event.candidateValid;
            out["candidatePath"] = pathToJson(event.candidatePath);
            out["classifiedAsReturnStage"] = event.classifiedAsReturnStage;
            out["replacedServingPath"] = event.replacedServingPath;
            out["selectedFollowers"] = {
                { "rigid", event.selectedRigidFollowerCount },
                { "coTimed", event.selectedCoTimedFollowerCount },
            };
            json selectedFollowerDetails = json::array();
            for (const auto& follower : event.selectedFollowers) {
                selectedFollowerDetails.push_back({
                    { "catalogPartId", follower.catalogPartId },
                    { "bodyIdDiagnostic", follower.bodyId },
                    { "omod", formToJson(follower.omod) },
                    { "sourceName", follower.sourceName },
                    { "tier", follower.tier },
                });
            }
            out["selectedFollowerDetails"] = std::move(selectedFollowerDetails);
            out["settings"] = settingsToJson(event.settings);
            if (!event.clipName.empty()) {
                out["clipContext"] = {
                    { "name", event.clipName },
                    { "durationSeconds", event.clipDurationSeconds },
                    { "croppedDurationSeconds", event.clipCroppedDurationSeconds },
                };
            }
            json samples = json::array();
            for (const auto& sample : event.samples) {
                samples.push_back(rawSampleToJson(sample));
            }
            out["rawSampleLayout"] = json::array({
                "rockFrame", "tx", "ty", "tz", "qw", "qx", "qy", "qz", "scale", "trusted",
                "clipActivityId", "clipScrubSessionId", "concurrentClipActivityCount", "clipFraction",
                "clipLocalTimeSeconds",
            });
            out["samples"] = std::move(samples);
            out["terminalSample"] = event.terminalSamplePresent ? rawSampleToJson(event.terminalSample) : json(nullptr);
            return out;
        }

        json serializeAuthoredClip(const AuthoredClipEvent& event)
        {
            json out = contextToJson(event.context);
            out["event"] = "authoredClip";
            out["activityId"] = decimal64(event.activityId);
            out["coordinateConventions"] = {
                { "poses", "animation transform-track local space" },
                { "translationUnits", "clip-authored rig units" },
                { "quaternionOrder", "w,x,y,z" },
                { "scaleSamples", "x,y,z unitless" },
                { "sampleTimes", "uniform inclusive 0..durationSeconds" },
            };
            out["activatedClip"] = event.activatedClip;
            out["animationName"] = event.animationName;
            out["durationSeconds"] = event.durationSeconds;
            out["rawTransformTrackCount"] = event.rawTransformTrackCount;
            out["capturedWeaponTrackCount"] = event.capturedWeaponTrackCount;
            out["weaponTracksTruncated"] = event.weaponTracksTruncated;
            out["rawAnnotationTrackCount"] = event.rawAnnotationTrackCount;
            out["rawTriggerCount"] = event.rawTriggerCount;
            out["graphEventNameCount"] = event.graphEventNameCount;
            out["annotationsTruncated"] = event.annotationsTruncated;
            out["triggersTruncated"] = event.triggersTruncated;
            json tracks = json::array();
            for (const auto& track : event.weaponTracks) {
                json samples = json::array();
                for (const auto& sample : track.samples) {
                    samples.push_back(poseToJson(sample));
                }
                json scales = json::array();
                for (const auto& scale : track.scaleSamples) {
                    scales.push_back(pointToJson(scale));
                }
                tracks.push_back({
                    { "bone", track.boneName },
                    { "samples", std::move(samples) },
                    { "scaleSamples", std::move(scales) },
                });
            }
            out["weaponTracks"] = std::move(tracks);
            json annotations = json::array();
            for (const auto& annotation : event.annotations) {
                annotations.push_back({
                    { "timeSeconds", annotation.timeSeconds },
                    { "track", annotation.trackName },
                    { "text", annotation.text },
                });
            }
            out["annotations"] = std::move(annotations);
            json triggers = json::array();
            for (const auto& trigger : event.triggers) {
                triggers.push_back({
                    { "localTimeSeconds", trigger.localTimeSeconds },
                    { "eventId", trigger.eventId },
                    { "eventName", trigger.eventName },
                });
            }
            out["triggers"] = std::move(triggers);
            return out;
        }

        json serializeGap(const CaptureGapEvent& event)
        {
            json out = contextToJson(event.context);
            out["event"] = "captureGap";
            out["relatedSnapshotSequence"] = decimal64(event.relatedSnapshotSequence);
            out["firstDroppedSequence"] = decimal64(event.firstDroppedSequence);
            out["lastDroppedSequence"] = decimal64(event.lastDroppedSequence);
            out["droppedEventCount"] = event.droppedEventCount;
            out["reason"] = event.reason;
            return out;
        }
    }

    const EventContext& contextOf(const Event& event)
    {
        return std::visit([](const auto& value) -> const EventContext& { return value.context; }, event);
    }

    std::size_t estimateOwnedBytes(const Event& event)
    {
        const auto stringBytes = [](const std::string& value) { return value.capacity() + 1; };
        const auto formBytes = [&](const FormInfo& form) {
            return stringBytes(form.ref.plugin) + stringBytes(form.editorId) + stringBytes(form.displayName);
        };
        const auto contextBytes = [&](const EventContext& context) {
            return stringBytes(context.sessionId) + formBytes(context.weapon) + stringBytes(context.paperVersion) +
                   stringBytes(context.rockModVersion);
        };
        return std::visit(
            [&](const auto& value) -> std::size_t {
                using Value = std::decay_t<decltype(value)>;
                std::size_t bytes = sizeof(Value) + contextBytes(value.context);
                if constexpr (std::is_same_v<Value, WeaponSnapshotEvent>) {
                    bytes += value.settings.motionPathMode.capacity() + 1;
                    bytes += value.nodes.capacity() * sizeof(NodeSnapshot);
                    for (const auto& node : value.nodes) {
                        bytes += stringBytes(node.name) + stringBytes(node.rootRelativePath);
                    }
                    bytes += value.evidence.capacity() * sizeof(EvidenceSnapshot);
                    for (const auto& part : value.evidence) {
                        bytes += stringBytes(part.providerSourceName) + stringBytes(part.sourceNodePath) +
                                 stringBytes(part.interactionNodePath) + formBytes(part.omod) + formBytes(part.attachPoint) +
                                 part.pointsWeaponLocalGame.capacity() * sizeof(Point3);
                    }
                    bytes += value.observationTargets.capacity() * sizeof(ObservationTarget);
                    for (const auto& target : value.observationTargets) {
                        bytes += stringBytes(target.sourceName) + formBytes(target.omod);
                    }
                } else if constexpr (std::is_same_v<Value, GeometryChunkEvent>) {
                    bytes += value.pointsWeaponLocalGame.capacity() * sizeof(Point3);
                } else if constexpr (std::is_same_v<Value, RawStrokeEvent>) {
                    bytes += formBytes(value.omod) + stringBytes(value.sourceName) + stringBytes(value.nodePath) +
                             stringBytes(value.settings.motionPathMode) + stringBytes(value.clipName) +
                             value.samples.capacity() * sizeof(RawSample) +
                             value.selectedFollowers.capacity() * sizeof(SelectedFollower);
                    for (const auto& follower : value.selectedFollowers) {
                        bytes += formBytes(follower.omod) + stringBytes(follower.sourceName) + stringBytes(follower.tier);
                    }
                } else if constexpr (std::is_same_v<Value, AuthoredClipEvent>) {
                    bytes += stringBytes(value.animationName) + value.weaponTracks.capacity() * sizeof(ClipTrack) +
                             value.annotations.capacity() * sizeof(ClipAnnotation) +
                             value.triggers.capacity() * sizeof(ClipTrigger);
                    for (const auto& track : value.weaponTracks) {
                        bytes += stringBytes(track.boneName) +
                                 track.samples.capacity() * sizeof(weapon_part_motion_path::PoseSample) +
                                 track.scaleSamples.capacity() * sizeof(Point3);
                    }
                    for (const auto& annotation : value.annotations) {
                        bytes += stringBytes(annotation.trackName) + stringBytes(annotation.text);
                    }
                    for (const auto& trigger : value.triggers) {
                        bytes += stringBytes(trigger.eventName);
                    }
                } else {
                    bytes += stringBytes(value.reason);
                }
                return bytes;
            },
            event);
    }

    std::string serializeLine(const Event& event)
    {
        const json serialized = std::visit(
            [](const auto& value) -> json {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, WeaponSnapshotEvent>) {
                    return serializeWeaponSnapshot(value);
                } else if constexpr (std::is_same_v<Value, GeometryChunkEvent>) {
                    return serializeGeometryChunk(value);
                } else if constexpr (std::is_same_v<Value, RawStrokeEvent>) {
                    return serializeRawStroke(value);
                } else if constexpr (std::is_same_v<Value, AuthoredClipEvent>) {
                    return serializeAuthoredClip(value);
                } else {
                    return serializeGap(value);
                }
            },
            event);
        // Engine/NIF/plugin strings are not guaranteed to be UTF-8. Replace
        // invalid byte sequences instead of throwing out of the background
        // writer thread (which would otherwise terminate the game process).
        return serialized.dump(-1, ' ', false, json::error_handler_t::replace);
    }
}
