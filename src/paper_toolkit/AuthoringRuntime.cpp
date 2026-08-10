#include "paper_toolkit/AuthoringRuntime.h"

#include "PAPERApi.h"
#include "PaperToolkitConfig.h"
#include "PaperToolkitLog.h"
#include "PrismaUI_F4VR_API.h"
#include "paper_toolkit/AuthoringPointerClickGate.h"
#include "paper_toolkit/AuthoringTimelinePolicy.h"

#include "vrcf/VRControllersManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace paper_toolkit
{
    namespace
    {
        using nlohmann::json;
        using paper::api::PaperApi;
        using paper::api::PaperConsumerCapabilityV1;
        using paper::api::PaperConsumerHandleV1;
        using paper::api::PaperConsumerRegistrationV1;
        using paper::api::PaperReloadAnimationAcquisitionV1;
        using paper::api::PaperReloadAnimationAnnotationV1;
        using paper::api::PaperReloadAnimationCatalogStateV1;
        using paper::api::PaperReloadAnimationClipV1;
        using paper::api::PaperReloadAnimationLimitsV1;
        using paper::api::PaperReloadAnimationLiveStateV1;
        using paper::api::PaperReloadAnimationSampleV1;
        using paper::api::PaperReloadAnimationTrackSpaceV1;
        using paper::api::PaperReloadAnimationTrackV1;
        using paper::api::PaperReloadAnimationTriggerV1;
        using paper::api::PaperResultV1;
        using rock::provider::RockProviderApi;
        using rock::provider::RockProviderConsumerCapabilityV1;
        using rock::provider::RockProviderConsumerHandleV1;
        using rock::provider::RockProviderConsumerRegistrationV1;
        using rock::provider::RockProviderFrameSnapshot;
        using rock::provider::RockProviderHand;
        using rock::provider::RockProviderHandInputSuppressionFlagV1;
        using rock::provider::RockProviderHandInputSuppressionRequestV1;
        using rock::provider::RockProviderLifecycleFlag;
        using rock::provider::RockProviderRawWandButtonStateV1;
        using rock::provider::RockProviderResultV1;
        using rock::provider::RockProviderWeaponEvidenceDetailV1;
        using rock::provider::RockProviderWeaponPartDriveSpaceV1;
        using rock::provider::RockProviderWeaponPartDriveTargetV1;
        using rock::provider::RockProviderWeaponPartTargetFlagV1;

        constexpr std::uint32_t kPanelWidthPixels = 1600;
        constexpr std::uint32_t kPanelHeightPixels = 1120;
        constexpr float kBasePanelPhysicalWidthGameUnits = 27.0f;
        constexpr float kBasePanelPhysicalHeightGameUnits =
            kBasePanelPhysicalWidthGameUnits *
            static_cast<float>(kPanelHeightPixels) /
            static_cast<float>(kPanelWidthPixels);
        constexpr float kPointerMaxDistanceGameUnits = 500.0f;
        constexpr std::uint32_t kPointerSuppressionLeaseFrames = 3;
        constexpr std::uint32_t kPreviewLeaseFrames = 2;
        constexpr std::uint32_t kPreviewPriority = 100;
        constexpr std::uint32_t kLeftTriggerButtonId =
            static_cast<std::uint32_t>(f4cf::vrcf::k_EButton_SteamVR_Trigger);
        constexpr std::uint32_t kLeftXButtonId =
            static_cast<std::uint32_t>(f4cf::vrcf::k_EButton_A);
        constexpr std::uint32_t kF4VrApiFlavor = 0x52563446u;
        constexpr std::uint64_t kRequiredSpatialFeatures =
            PRISMA_UI_VR_API::SpatialFeature_FullPose |
            PRISMA_UI_VR_API::SpatialFeature_IndependentDimensions |
            PRISMA_UI_VR_API::SpatialFeature_LatestOnlyUpdates |
            PRISMA_UI_VR_API::SpatialFeature_AppliedSequenceQuery |
            PRISMA_UI_VR_API::SpatialFeature_GpuRendering |
            PRISMA_UI_VR_API::SpatialFeature_NativeNetworkPolicy |
            PRISMA_UI_VR_API::SpatialFeature_SceneDepthOcclusion |
            PRISMA_UI_VR_API::SpatialFeature_WorldPointerInput |
            PRISMA_UI_VR_API::SpatialFeature_CentralPointerRouting;
        constexpr std::size_t kMaxCatalogClips = 512;
        constexpr std::size_t kMaxTracksPerClip = 512;
        constexpr std::size_t kMaxSamplesPerTrack = 4096;
        constexpr std::size_t kMaxMarkersPerClip = 2048;
        constexpr auto kProviderRetryInterval = std::chrono::seconds(1);
        constexpr auto kModelPublishInterval = std::chrono::milliseconds(80);

        struct Vec3
        {
            float x{ 0.0f };
            float y{ 0.0f };
            float z{ 0.0f };

            Vec3 operator+(const Vec3& other) const noexcept
            {
                return { x + other.x, y + other.y, z + other.z };
            }

            Vec3 operator-(const Vec3& other) const noexcept
            {
                return { x - other.x, y - other.y, z - other.z };
            }

            Vec3 operator*(float scalar) const noexcept
            {
                return { x * scalar, y * scalar, z * scalar };
            }
        };

        struct PanelPose
        {
            Vec3 position{};
            std::array<float, 4> orientation{ 0.0f, 0.0f, 0.0f, 1.0f };
        };

        struct PointerRay
        {
            Vec3 origin{};
            Vec3 direction{};
        };

        [[nodiscard]] bool finite(const Vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z);
        }

        [[nodiscard]] float dot(const Vec3& left, const Vec3& right) noexcept
        {
            return left.x * right.x + left.y * right.y + left.z * right.z;
        }

        [[nodiscard]] Vec3 cross(const Vec3& left, const Vec3& right) noexcept
        {
            return {
                left.y * right.z - left.z * right.y,
                left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x,
            };
        }

        [[nodiscard]] std::optional<Vec3> normalized(const Vec3& value) noexcept
        {
            if (!finite(value)) {
                return std::nullopt;
            }
            const float lengthSquared = dot(value, value);
            if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8f) {
                return std::nullopt;
            }
            return value * (1.0f / std::sqrt(lengthSquared));
        }

        [[nodiscard]] std::optional<std::array<float, 4>> normalizedQuaternion(
            std::array<float, 4> value) noexcept
        {
            float normSquared = 0.0f;
            for (const float component : value) {
                if (!std::isfinite(component)) {
                    return std::nullopt;
                }
                normSquared += component * component;
            }
            if (!std::isfinite(normSquared) || normSquared < 1.0e-8f) {
                return std::nullopt;
            }
            const float inverseNorm = 1.0f / std::sqrt(normSquared);
            for (float& component : value) {
                component *= inverseNorm;
            }
            return value;
        }

        [[nodiscard]] std::optional<std::array<float, 4>> multiplyQuaternions(
            const std::array<float, 4>& left,
            const std::array<float, 4>& right) noexcept
        {
            return normalizedQuaternion({
                left[3] * right[0] + left[0] * right[3] +
                    left[1] * right[2] - left[2] * right[1],
                left[3] * right[1] - left[0] * right[2] +
                    left[1] * right[3] + left[2] * right[0],
                left[3] * right[2] + left[0] * right[1] -
                    left[1] * right[0] + left[2] * right[3],
                left[3] * right[3] - left[0] * right[0] -
                    left[1] * right[1] - left[2] * right[2],
            });
        }

        [[nodiscard]] std::array<float, 4> axisAngleQuaternion(
            const Vec3& axis,
            float radians) noexcept
        {
            const float half = radians * 0.5f;
            const float sine = std::sin(half);
            return { axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half) };
        }

        [[nodiscard]] std::optional<std::array<float, 4>> configuredOrientation() noexcept
        {
            constexpr float kDegreesToRadians = 0.01745329251994329577f;
            const auto x = axisAngleQuaternion(
                { 1.0f, 0.0f, 0.0f },
                g_paperToolkitConfig.authoringPanelRotationXDegrees * kDegreesToRadians);
            const auto y = axisAngleQuaternion(
                { 0.0f, 1.0f, 0.0f },
                g_paperToolkitConfig.authoringPanelRotationYDegrees * kDegreesToRadians);
            const auto z = axisAngleQuaternion(
                { 0.0f, 0.0f, 1.0f },
                g_paperToolkitConfig.authoringPanelRotationZDegrees * kDegreesToRadians);
            const auto yx = multiplyQuaternions(y, x);
            if (!yx) {
                return std::nullopt;
            }
            auto orientation = multiplyQuaternions(z, *yx);
            if (!orientation) {
                return std::nullopt;
            }
            constexpr std::array<float, 4> kFlipX{ 1.0f, 0.0f, 0.0f, 0.0f };
            constexpr std::array<float, 4> kFlipY{ 0.0f, 1.0f, 0.0f, 0.0f };
            constexpr std::array<float, 4> kFlipZ{ 0.0f, 0.0f, 1.0f, 0.0f };
            if (g_paperToolkitConfig.authoringPanelFlipRotationX) {
                orientation = multiplyQuaternions(*orientation, kFlipX);
            }
            if (orientation && g_paperToolkitConfig.authoringPanelFlipRotationY) {
                orientation = multiplyQuaternions(*orientation, kFlipY);
            }
            if (orientation && g_paperToolkitConfig.authoringPanelFlipRotationZ) {
                orientation = multiplyQuaternions(*orientation, kFlipZ);
            }
            return orientation;
        }

        [[nodiscard]] std::optional<std::array<float, 4>> quaternionFromBasis(
            const Vec3& right,
            const Vec3& up,
            const Vec3& front) noexcept
        {
            const float m00 = right.x;
            const float m01 = up.x;
            const float m02 = front.x;
            const float m10 = right.y;
            const float m11 = up.y;
            const float m12 = front.y;
            const float m20 = right.z;
            const float m21 = up.z;
            const float m22 = front.z;
            std::array<float, 4> quaternion{};
            const float trace = m00 + m11 + m22;
            if (trace > 0.0f) {
                const float scale = std::sqrt(trace + 1.0f) * 2.0f;
                quaternion = {
                    (m21 - m12) / scale,
                    (m02 - m20) / scale,
                    (m10 - m01) / scale,
                    0.25f * scale,
                };
            } else if (m00 > m11 && m00 > m22) {
                const float scale = std::sqrt((std::max)(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
                quaternion = {
                    0.25f * scale,
                    (m01 + m10) / scale,
                    (m02 + m20) / scale,
                    (m21 - m12) / scale,
                };
            } else if (m11 > m22) {
                const float scale = std::sqrt((std::max)(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
                quaternion = {
                    (m01 + m10) / scale,
                    0.25f * scale,
                    (m12 + m21) / scale,
                    (m02 - m20) / scale,
                };
            } else {
                const float scale = std::sqrt((std::max)(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
                quaternion = {
                    (m02 + m20) / scale,
                    (m12 + m21) / scale,
                    0.25f * scale,
                    (m10 - m01) / scale,
                };
            }
            return normalizedQuaternion(quaternion);
        }

        [[nodiscard]] bool snapshotAllowsPanel(const RockProviderFrameSnapshot& snapshot) noexcept
        {
            return snapshot.providerReady != 0 && snapshot.menuBlocking == 0 &&
                snapshot.configBlocking == 0 &&
                rock::provider::hasLifecycleFlag(
                    snapshot.lifecycleFlags, RockProviderLifecycleFlag::WorldAvailable) &&
                rock::provider::hasLifecycleFlag(
                    snapshot.lifecycleFlags, RockProviderLifecycleFlag::SkeletonReady) &&
                rock::provider::hasLifecycleFlag(
                    snapshot.lifecycleFlags, RockProviderLifecycleFlag::ProviderReady);
        }

        [[nodiscard]] std::optional<PanelPose> computePanelPose(
            const RockProviderFrameSnapshot& snapshot,
            const std::array<float, 4>& localOrientation) noexcept
        {
            const auto& transform = snapshot.rightHandTransform;
            const Vec3 origin{
                transform.translate[0], transform.translate[1], transform.translate[2]
            };
            const Vec3 rawForward{
                transform.rotate[0], transform.rotate[1], transform.rotate[2]
            };
            const Vec3 rawLateral{
                transform.rotate[3], transform.rotate[4], transform.rotate[5]
            };
            const Vec3 rawUp{
                transform.rotate[6], transform.rotate[7], transform.rotate[8]
            };
            const auto forward = normalized(rawForward);
            if (!finite(origin) || !forward) {
                return std::nullopt;
            }
            auto up = normalized(rawUp - (*forward * dot(rawUp, *forward)));
            if (!up) {
                const auto lateral = normalized(
                    rawLateral - (*forward * dot(rawLateral, *forward)));
                if (!lateral) {
                    return std::nullopt;
                }
                up = normalized(cross(*forward, *lateral));
                if (!up) {
                    return std::nullopt;
                }
            }
            const auto handLateral = normalized(cross(*up, *forward));
            if (!handLateral) {
                return std::nullopt;
            }
            up = normalized(cross(*forward, *handLateral));
            if (!up) {
                return std::nullopt;
            }
            const auto base = quaternionFromBasis(
                *handLateral * -1.0f, *up, *forward * -1.0f);
            if (!base) {
                return std::nullopt;
            }
            const auto orientation = multiplyQuaternions(*base, localOrientation);
            if (!orientation) {
                return std::nullopt;
            }
            PanelPose pose{};
            pose.position = origin +
                (*forward * g_paperToolkitConfig.authoringPanelPositionX) +
                (*handLateral * g_paperToolkitConfig.authoringPanelPositionY) +
                (*up * g_paperToolkitConfig.authoringPanelPositionZ);
            pose.orientation = *orientation;
            return finite(pose.position) ? std::optional<PanelPose>{ pose } : std::nullopt;
        }

        [[nodiscard]] std::optional<PointerRay> computePointerRay(
            const RockProviderFrameSnapshot& snapshot,
            float maximumWorldPosition) noexcept
        {
            const auto& transform = snapshot.leftHandTransform;
            PointerRay ray{};
            ray.origin = {
                transform.translate[0], transform.translate[1], transform.translate[2]
            };
            ray.direction = {
                transform.rotate[0], transform.rotate[1], transform.rotate[2]
            };
            const auto direction = normalized(ray.direction);
            if (!finite(ray.origin) || !direction || !std::isfinite(maximumWorldPosition) ||
                maximumWorldPosition <= 0.0f ||
                std::fabs(ray.origin.x) > maximumWorldPosition ||
                std::fabs(ray.origin.y) > maximumWorldPosition ||
                std::fabs(ray.origin.z) > maximumWorldPosition) {
                return std::nullopt;
            }
            ray.direction = *direction;
            return ray;
        }

        [[nodiscard]] std::string boundedString(const char* text, std::size_t capacity)
        {
            if (!text || capacity == 0) {
                return {};
            }
            std::size_t length = 0;
            while (length < capacity && text[length] != '\0') {
                ++length;
            }
            return std::string(text, length);
        }

        [[nodiscard]] const char* acquisitionName(
            PaperReloadAnimationAcquisitionV1 acquisition) noexcept
        {
            switch (acquisition) {
            case PaperReloadAnimationAcquisitionV1::LoadedGraphBinding:
                return "loaded";
            case PaperReloadAnimationAcquisitionV1::LiveClipActivation:
                return "live";
            case PaperReloadAnimationAcquisitionV1::ExactWeaponPreharvest:
                return "exact";
            default:
                return "unknown";
            }
        }

        [[nodiscard]] bool catalogIdentityEqual(
            const PaperReloadAnimationCatalogStateV1& left,
            const PaperReloadAnimationCatalogStateV1& right) noexcept
        {
            return left.catalogSequence == right.catalogSequence &&
                left.catalogRevision == right.catalogRevision &&
                left.weaponFormId == right.weaponFormId &&
                left.weaponGenerationKey == right.weaponGenerationKey;
        }

        [[nodiscard]] authoring_timeline::QsTransform toTimelineTransform(
            const paper::api::PaperReloadQsTransformV1& value) noexcept
        {
            return {
                { value.translate[0], value.translate[1], value.translate[2] },
                { value.rotate[0], value.rotate[1], value.rotate[2], value.rotate[3] },
                { value.scale[0], value.scale[1], value.scale[2] },
            };
        }

        [[nodiscard]] bool fillProviderTransform(
            const authoring_timeline::QsTransform& value,
            rock::provider::RockProviderTransform& out) noexcept
        {
            auto quaternion = value.rotate;
            float uniformScale = 1.0f;
            if (!authoring_timeline::finite(value) ||
                !authoring_timeline::normalizeQuaternion(quaternion) ||
                !authoring_timeline::uniformPositiveScale(value.scale, uniformScale)) {
                return false;
            }
            const float x = quaternion[0];
            const float y = quaternion[1];
            const float z = quaternion[2];
            const float w = quaternion[3];
            out.rotate[0] = 1.0f - 2.0f * (y * y + z * z);
            out.rotate[1] = 2.0f * (x * y + w * z);
            out.rotate[2] = 2.0f * (x * z - w * y);
            out.rotate[3] = 2.0f * (x * y - w * z);
            out.rotate[4] = 1.0f - 2.0f * (x * x + z * z);
            out.rotate[5] = 2.0f * (y * z + w * x);
            out.rotate[6] = 2.0f * (x * z + w * y);
            out.rotate[7] = 2.0f * (y * z - w * x);
            out.rotate[8] = 1.0f - 2.0f * (x * x + y * y);
            out.translate[0] = value.translate[0];
            out.translate[1] = value.translate[1];
            out.translate[2] = value.translate[2];
            out.scale = uniformScale;
            return true;
        }

        template <class Value, class CopyFunction>
        [[nodiscard]] bool copyBoundedVector(
            std::uint32_t expectedCount,
            std::size_t hardLimit,
            CopyFunction&& copyFunction,
            std::vector<Value>& out)
        {
            const auto count = (std::min)(
                static_cast<std::size_t>(expectedCount), hardLimit);
            if (count == 0) {
                out.clear();
                return true;
            }
            out.assign(count, Value{});
            std::uint32_t copied = 0;
            const auto result = copyFunction(
                out.empty() ? nullptr : out.data(),
                static_cast<std::uint32_t>(out.size()),
                &copied);
            if (result != PaperResultV1::Ok || copied > out.size()) {
                out.clear();
                return false;
            }
            out.resize(copied);
            return copied == count;
        }
    }

    struct AuthoringRuntime::Impl
    {
        struct TrackBuffer
        {
            PaperReloadAnimationTrackV1 track{};
            std::vector<PaperReloadAnimationSampleV1> samples;
            std::int32_t evidenceIndex{ -1 };
            std::uint32_t matchingEvidenceCount{ 0 };
            float translationTravel{ 0.0f };
            bool previewEnabled{ true };
            bool unsupportedScale{ false };
        };

        struct SelectedClip
        {
            bool loaded{ false };
            PaperReloadAnimationClipV1 clip{};
            std::vector<TrackBuffer> tracks;
            std::vector<PaperReloadAnimationAnnotationV1> annotations;
            std::vector<PaperReloadAnimationTriggerV1> triggers;
            std::uint64_t catalogSequence{ 0 };
            std::uint64_t catalogRevision{ 0 };
        };

        PRISMA_UI_API::IVPrismaUI4* prisma{ nullptr };
        PRISMA_UI_VR_API::IVPrismaUIVR1* prismaVR{ nullptr };
        PRISMA_UI_VR_API::SpatialCapabilitiesV1 spatialCapabilities{};
        PrismaView view{ 0 };
        bool domReady{ false };
        bool panelVisible{ false };
        bool pointerActive{ false };
        std::uint64_t nextSpatialSequence{ 1 };
        std::uint64_t nextPointerSequence{ 1 };
        std::uint64_t seenPanelRevision{ 0 };
        std::optional<std::array<float, 4>> panelLocalOrientation;
        authoring_pointer_gate::State clickGate{};

        std::uint64_t rockOwnerToken{ 0 };
        std::uint64_t paperOwnerToken{ 0 };
        std::chrono::steady_clock::time_point nextRockRegistrationAttempt{};
        std::chrono::steady_clock::time_point nextPaperRegistrationAttempt{};
        std::chrono::steady_clock::time_point nextPrismaAttempt{};

        PaperReloadAnimationLimitsV1 animationLimits{};
        PaperReloadAnimationCatalogStateV1 catalog{};
        PaperReloadAnimationLiveStateV1 live{};
        std::vector<PaperReloadAnimationClipV1> clips;
        std::array<RockProviderWeaponEvidenceDetailV1,
            rock::provider::ROCK_PROVIDER_MAX_WEAPON_BODIES> evidence{};
        std::uint32_t evidenceCount{ 0 };
        std::uint64_t evidenceGenerationKey{ 0 };
        SelectedClip selected{};
        std::uint32_t requestedClipId{ 0 };
        bool selectionRequested{ false };
        bool previewRequested{ false };
        float previewTimeSeconds{ 0.0f };
        bool modelDirty{ true };
        std::chrono::steady_clock::time_point nextModelPublish{};
        std::string status{ "Waiting for PAPER and ROCK" };
        RockProviderFrameSnapshot lastSnapshot{};

        static Impl* active;

        void onGameLoaded();
        void onFrame(const RockProviderFrameSnapshot& snapshot);
        void shutdown();
        void ensureRockOwner();
        void ensurePaperConsumer();
        void ensurePrisma();
        void createView();
        void destroyView();
        void refreshEvidence(const RockProviderFrameSnapshot& snapshot);
        void refreshPaperCatalog();
        bool loadSelectedClip(std::uint32_t clipId);
        void mapSelectedTracks();
        void updatePreview(const RockProviderFrameSnapshot& snapshot);
        void clearPreview();
        void updatePanel(const RockProviderFrameSnapshot& snapshot);
        void cancelPointer();
        bool samplePointerButton(const RockProviderFrameSnapshot& snapshot, bool routed);
        void publishModel(bool force = false);
        json buildModel() const;
        void handleUiEvent(const char* payload);
        static void onDomReady(PrismaView readyView);
        static void onUiEvent(const char* payload);
    };

    AuthoringRuntime::Impl* AuthoringRuntime::Impl::active = nullptr;

    void AuthoringRuntime::Impl::onGameLoaded()
    {
        active = this;
        nextRockRegistrationAttempt = {};
        nextPaperRegistrationAttempt = {};
        nextPrismaAttempt = {};
        ensureRockOwner();
        ensurePaperConsumer();
        ensurePrisma();
    }

    void AuthoringRuntime::Impl::ensureRockOwner()
    {
        if (rockOwnerToken != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextRockRegistrationAttempt) {
            return;
        }
        nextRockRegistrationAttempt = now + kProviderRetryInterval;
        if (!RockProviderApi::inst || !RockProviderApi::inst->registerConsumerV1 ||
            !rock::provider::supportsWeaponPartInteractionV1() ||
            !rock::provider::supportsHandInputSuppressionV1() ||
            !rock::provider::supportsRawWandButtonStateV1()) {
            return;
        }

        RockProviderConsumerRegistrationV1 registration{};
        std::snprintf(
            registration.modName,
            sizeof(registration.modName),
            "PAPER Toolkit Authoring");
        registration.requestedCapabilities =
            static_cast<std::uint32_t>(
                RockProviderConsumerCapabilityV1::WeaponPartInteraction) |
            static_cast<std::uint32_t>(
                RockProviderConsumerCapabilityV1::HandInputSuppression);
        RockProviderConsumerHandleV1 handle{};
        const auto result = RockProviderApi::inst->registerConsumerV1(
            &registration, &handle);
        const bool granted = result == RockProviderResultV1::Ok &&
            handle.ownerToken != 0 &&
            rock::provider::hasConsumerCapabilityV1(
                handle.grantedCapabilities,
                RockProviderConsumerCapabilityV1::WeaponPartInteraction) &&
            rock::provider::hasConsumerCapabilityV1(
                handle.grantedCapabilities,
                RockProviderConsumerCapabilityV1::HandInputSuppression);
        if (!granted) {
            if (handle.ownerToken != 0 && RockProviderApi::inst->unregisterConsumerV1) {
                (void)RockProviderApi::inst->unregisterConsumerV1(handle.ownerToken);
            }
            return;
        }
        rockOwnerToken = handle.ownerToken;
        modelDirty = true;
        PAPER_TOOLKIT_LOG_INFO(Drive,
            "Authoring workstation registered ROCK owner {} (capabilities={:#x})",
            rockOwnerToken,
            handle.grantedCapabilities);
    }

    void AuthoringRuntime::Impl::ensurePaperConsumer()
    {
        if (paperOwnerToken != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextPaperRegistrationAttempt) {
            return;
        }
        nextPaperRegistrationAttempt = now + kProviderRetryInterval;
        const int initializeResult = PaperApi::initialize(
            paper::api::PAPER_API_VERSION,
            paper::api::PAPER_PROVIDER_API_V1_RELOAD_ANIMATION_TABLE_BYTES);
        if (initializeResult != 0 || !PaperApi::inst ||
            !paper::api::supportsReloadAnimationEvidenceV1() ||
            !PaperApi::inst->registerConsumerV1) {
            status = "Waiting for PAPER exact animation API";
            modelDirty = true;
            return;
        }

        PaperConsumerRegistrationV1 registration{};
        std::snprintf(
            registration.modName,
            sizeof(registration.modName),
            "PAPER Toolkit Authoring");
        registration.requestedCapabilities = static_cast<std::uint32_t>(
            PaperConsumerCapabilityV1::ReloadAnimationEvidence);
        PaperConsumerHandleV1 handle{};
        const auto result = PaperApi::inst->registerConsumerV1(
            &registration, &handle);
        const bool granted = result == PaperResultV1::Ok &&
            handle.ownerToken != 0 &&
            (handle.grantedCapabilities & registration.requestedCapabilities) ==
                registration.requestedCapabilities;
        if (!granted) {
            if (handle.ownerToken != 0 && PaperApi::inst->unregisterConsumerV1) {
                (void)PaperApi::inst->unregisterConsumerV1(handle.ownerToken);
            }
            status = "PAPER denied exact animation evidence";
            modelDirty = true;
            return;
        }
        paperOwnerToken = handle.ownerToken;
        animationLimits = {};
        const auto limitsResult = PaperApi::inst->getReloadAnimationLimitsV1(
            paperOwnerToken, &animationLimits);
        if (limitsResult != PaperResultV1::Ok) {
            (void)PaperApi::inst->unregisterConsumerV1(paperOwnerToken);
            paperOwnerToken = 0;
            status = "PAPER animation limits unavailable";
            modelDirty = true;
            return;
        }
        status = "PAPER exact catalog online";
        modelDirty = true;
        PAPER_TOOLKIT_LOG_INFO(Drive,
            "Authoring workstation connected to PAPER owner {} (maxClips={}, maxExactTracks={}, maxExactSamples={})",
            paperOwnerToken,
            animationLimits.maxClips,
            animationLimits.maxExactTracksPerClip,
            animationLimits.maxExactSamplesPerTrack);
    }

    void AuthoringRuntime::Impl::ensurePrisma()
    {
        if (prisma && prismaVR) {
            if (g_paperToolkitConfig.authoringPanelEnabled && view == 0) {
                createView();
            }
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextPrismaAttempt) {
            return;
        }
        nextPrismaAttempt = now + kProviderRetryInterval;
        prisma = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI4>();
        prismaVR =
            PRISMA_UI_VR_API::RequestPluginVRAPI<PRISMA_UI_VR_API::IVPrismaUIVR1>();
        if (!prisma || !prismaVR) {
            prisma = nullptr;
            prismaVR = nullptr;
            return;
        }
        spatialCapabilities = {};
        spatialCapabilities.structSize = sizeof(spatialCapabilities);
        const auto result = prismaVR->GetSpatialCapabilities(&spatialCapabilities);
        const auto worldQuadMask = 1ull << static_cast<std::uint32_t>(
            PRISMA_UI_VR_API::SpatialPresentationMode::WorldQuad);
        const bool accepted = result == PRISMA_UI_VR_API::SpatialResult::Ok &&
            spatialCapabilities.apiFlavor == kF4VrApiFlavor &&
            (spatialCapabilities.featureBits & kRequiredSpatialFeatures) ==
                kRequiredSpatialFeatures &&
            (spatialCapabilities.presentationModeMask & worldQuadMask) != 0 &&
            (spatialCapabilities.supportedUpdateFlags &
                PRISMA_UI_VR_API::SpatialUpdate_SceneDepthOcclusion) != 0 &&
            spatialCapabilities.maxPixelWidth >= kPanelWidthPixels &&
            spatialCapabilities.maxPixelHeight >= kPanelHeightPixels &&
            spatialCapabilities.maxSpatialViews > 0;
        if (!accepted) {
            PAPER_TOOLKIT_LOG_ERROR(Drive,
                "Authoring workstation rejected Prisma FO4VR spatial contract (result={}, flavor={:#x}, features={:#x})",
                static_cast<std::int32_t>(result),
                spatialCapabilities.apiFlavor,
                spatialCapabilities.featureBits);
            prisma = nullptr;
            prismaVR = nullptr;
            return;
        }
        PAPER_TOOLKIT_LOG_INFO(Drive,
            "Authoring workstation acquired Prisma WorldQuad contract ({}x{}, features={:#x})",
            spatialCapabilities.maxPixelWidth,
            spatialCapabilities.maxPixelHeight,
            spatialCapabilities.featureBits);
        if (g_paperToolkitConfig.authoringPanelEnabled) {
            createView();
        }
    }

    void AuthoringRuntime::Impl::createView()
    {
        if (!prisma || !prismaVR || view != 0) {
            return;
        }
        PRISMA_UI_VR_API::ViewCreateOptionsV1 options{};
        options.structSize = sizeof(options);
        options.networkAccessPolicy =
            PRISMA_UI_VR_API::NetworkAccessPolicy::LocalOnly;
        view = prismaVR->CreateViewWithOptions(
            "PAPER-Toolkit-Authoring/index.html",
            &Impl::onDomReady,
            &options);
        if (view == 0) {
            PAPER_TOOLKIT_LOG_ERROR(Drive,
                "Authoring workstation failed to create LocalOnly Prisma view");
            return;
        }
        PRISMA_UI_VR_API::NetworkAccessPolicy retainedPolicy{};
        if (!prismaVR->GetNetworkAccessPolicy(view, &retainedPolicy) ||
            retainedPolicy != PRISMA_UI_VR_API::NetworkAccessPolicy::LocalOnly) {
            prisma->Destroy(view);
            view = 0;
            PAPER_TOOLKIT_LOG_ERROR(Drive,
                "Authoring workstation destroyed a view that failed LocalOnly policy retention");
            return;
        }
        prisma->BindUIEvent(view, "paperToolkitAuthoringAction", &Impl::onUiEvent);
        domReady = false;
        modelDirty = true;
        PAPER_TOOLKIT_LOG_INFO(Drive,
            "Authoring workstation requested Prisma view {}", view);
    }

    void AuthoringRuntime::Impl::destroyView()
    {
        cancelPointer();
        if (view != 0 && prisma) {
            prisma->Destroy(view);
        }
        view = 0;
        domReady = false;
        panelVisible = false;
    }

    void AuthoringRuntime::Impl::onDomReady(PrismaView readyView)
    {
        if (!active || readyView == 0 || readyView != active->view) {
            return;
        }
        active->domReady = true;
        active->modelDirty = true;
        active->publishModel(true);
        PAPER_TOOLKIT_LOG_INFO(Drive,
            "Authoring workstation DOM ready for view {}", readyView);
    }

    void AuthoringRuntime::Impl::onUiEvent(const char* payload)
    {
        if (active) {
            active->handleUiEvent(payload);
        }
    }

    void AuthoringRuntime::Impl::refreshEvidence(
        const RockProviderFrameSnapshot& snapshot)
    {
        if (snapshot.weaponGenerationKey == evidenceGenerationKey) {
            return;
        }
        evidence = {};
        evidenceCount = 0;
        evidenceGenerationKey = snapshot.weaponGenerationKey;
        if (snapshot.weaponGenerationKey == 0 || !RockProviderApi::inst ||
            !RockProviderApi::inst->copyWeaponEvidenceDetailsV1) {
            mapSelectedTracks();
            modelDirty = true;
            return;
        }
        evidenceCount = (std::min)(
            RockProviderApi::inst->copyWeaponEvidenceDetailsV1(
                evidence.data(), static_cast<std::uint32_t>(evidence.size())),
            static_cast<std::uint32_t>(evidence.size()));
        std::uint32_t write = 0;
        for (std::uint32_t read = 0; read < evidenceCount; ++read) {
            if (evidence[read].weaponGenerationKey != snapshot.weaponGenerationKey ||
                evidence[read].sourceRoot == 0 || evidence[read].sourceName[0] == '\0') {
                continue;
            }
            if (write != read) {
                evidence[write] = evidence[read];
            }
            ++write;
        }
        evidenceCount = write;
        mapSelectedTracks();
        modelDirty = true;
    }

    void AuthoringRuntime::Impl::refreshPaperCatalog()
    {
        if (paperOwnerToken == 0 || !PaperApi::inst) {
            return;
        }
        PaperReloadAnimationCatalogStateV1 before{};
        const auto catalogResult = PaperApi::inst->getReloadAnimationCatalogStateV1(
            paperOwnerToken, &before);
        if (catalogResult == PaperResultV1::UnknownOwner) {
            paperOwnerToken = 0;
            clearPreview();
            status = "PAPER reset; reconnecting";
            modelDirty = true;
            return;
        }
        if (catalogResult != PaperResultV1::Ok) {
            return;
        }

        PaperReloadAnimationLiveStateV1 nextLive{};
        if (PaperApi::inst->getReloadAnimationLiveStateV1(
                paperOwnerToken, &nextLive) == PaperResultV1::Ok) {
            const bool liveChanged = nextLive.flags != live.flags ||
                nextLive.activityId != live.activityId ||
                nextLive.catalogSequence != live.catalogSequence ||
                std::fabs(nextLive.localTimeSeconds - live.localTimeSeconds) > 0.01f ||
                boundedString(nextLive.animationName, sizeof(nextLive.animationName)) !=
                    boundedString(live.animationName, sizeof(live.animationName));
            live = nextLive;
            modelDirty = modelDirty || liveChanged;
        }

        const bool catalogChanged = !catalogIdentityEqual(before, catalog) ||
            before.clipCount != catalog.clipCount ||
            before.statusFlags != catalog.statusFlags ||
            before.exactPreharvestState != catalog.exactPreharvestState;
        if (!catalogChanged) {
            return;
        }

        std::vector<PaperReloadAnimationClipV1> copiedClips;
        const bool copied = copyBoundedVector<PaperReloadAnimationClipV1>(
            before.clipCount,
            kMaxCatalogClips,
            [&](PaperReloadAnimationClipV1* output,
                std::uint32_t capacity,
                std::uint32_t* outCopied) {
                return PaperApi::inst->copyReloadAnimationClipsV1(
                    paperOwnerToken,
                    before.catalogSequence,
                    0,
                    output,
                    capacity,
                    outCopied);
            },
            copiedClips);
        PaperReloadAnimationCatalogStateV1 after{};
        const auto afterResult = PaperApi::inst->getReloadAnimationCatalogStateV1(
            paperOwnerToken, &after);
        if (!copied || afterResult != PaperResultV1::Ok ||
            !catalogIdentityEqual(before, after)) {
            // PAPER appends exact clips across frames. Sequence alone is not
            // an immutable snapshot token, so reject a mixed revision.
            return;
        }

        std::string selectedName;
        std::string selectedPath;
        if (selected.loaded) {
            selectedName = boundedString(
                selected.clip.animationName,
                sizeof(selected.clip.animationName));
            selectedPath = boundedString(
                selected.clip.animationPath,
                sizeof(selected.clip.animationPath));
        }
        const bool selectedIdentityChanged = selected.loaded &&
            (selected.catalogSequence != after.catalogSequence ||
                selected.catalogRevision != after.catalogRevision);
        if (selectedIdentityChanged) {
            clearPreview();
            selected = {};
        }

        catalog = after;
        clips = std::move(copiedClips);
        status = "PAPER exact catalog online";
        modelDirty = true;

        // Restore a user selection across a mutable catalog revision by its
        // stable authored name/path pair, never by the revision-local clipId.
        if (selectedIdentityChanged && (!selectedName.empty() || !selectedPath.empty())) {
            for (const auto& clip : clips) {
                if (clip.acquisition !=
                        PaperReloadAnimationAcquisitionV1::ExactWeaponPreharvest ||
                    clip.trackSpace !=
                        PaperReloadAnimationTrackSpaceV1::WeaponRootLocal) {
                    continue;
                }
                if (boundedString(clip.animationName, sizeof(clip.animationName)) == selectedName &&
                    boundedString(clip.animationPath, sizeof(clip.animationPath)) == selectedPath) {
                    requestedClipId = clip.clipId;
                    selectionRequested = true;
                    break;
                }
            }
        }
    }

    bool AuthoringRuntime::Impl::loadSelectedClip(std::uint32_t clipId)
    {
        if (paperOwnerToken == 0 || !PaperApi::inst) {
            return false;
        }
        const auto found = std::find_if(
            clips.begin(), clips.end(),
            [&](const PaperReloadAnimationClipV1& clip) {
                return clip.clipId == clipId;
            });
        if (found == clips.end() ||
            found->acquisition !=
                PaperReloadAnimationAcquisitionV1::ExactWeaponPreharvest ||
            found->trackSpace !=
                PaperReloadAnimationTrackSpaceV1::WeaponRootLocal) {
            status = "Preview requires an exact WeaponRootLocal clip";
            modelDirty = true;
            return false;
        }

        PaperReloadAnimationCatalogStateV1 before{};
        if (PaperApi::inst->getReloadAnimationCatalogStateV1(
                paperOwnerToken, &before) != PaperResultV1::Ok ||
            !catalogIdentityEqual(before, catalog)) {
            return false;
        }

        SelectedClip next{};
        next.clip = *found;
        std::vector<PaperReloadAnimationTrackV1> trackHeaders;
        if (!copyBoundedVector<PaperReloadAnimationTrackV1>(
                found->trackCount,
                kMaxTracksPerClip,
                [&](PaperReloadAnimationTrackV1* output,
                    std::uint32_t capacity,
                    std::uint32_t* outCopied) {
                    return PaperApi::inst->copyReloadAnimationTracksV1(
                        paperOwnerToken,
                        before.catalogSequence,
                        found->clipId,
                        0,
                        output,
                        capacity,
                        outCopied);
                },
                trackHeaders)) {
            status = "Track snapshot changed while loading";
            modelDirty = true;
            return false;
        }

        next.tracks.reserve(trackHeaders.size());
        for (const auto& track : trackHeaders) {
            TrackBuffer buffer{};
            buffer.track = track;
            if (!copyBoundedVector<PaperReloadAnimationSampleV1>(
                    track.sampleCount,
                    kMaxSamplesPerTrack,
                    [&](PaperReloadAnimationSampleV1* output,
                        std::uint32_t capacity,
                        std::uint32_t* outCopied) {
                        return PaperApi::inst->copyReloadAnimationSamplesV1(
                            paperOwnerToken,
                            before.catalogSequence,
                            found->clipId,
                            track.trackId,
                            0,
                            output,
                            capacity,
                            outCopied);
                    },
                    buffer.samples)) {
                status = "Sample snapshot changed while loading";
                modelDirty = true;
                return false;
            }
            if (!buffer.samples.empty()) {
                const auto origin = toTimelineTransform(
                    buffer.samples.front().transform);
                for (const auto& sample : buffer.samples) {
                    const auto value = toTimelineTransform(sample.transform);
                    const float dx = value.translate[0] - origin.translate[0];
                    const float dy = value.translate[1] - origin.translate[1];
                    const float dz = value.translate[2] - origin.translate[2];
                    buffer.translationTravel = (std::max)(
                        buffer.translationTravel,
                        std::sqrt(dx * dx + dy * dy + dz * dz));
                    float uniformScale = 1.0f;
                    if (!authoring_timeline::uniformPositiveScale(
                            value.scale, uniformScale)) {
                        buffer.unsupportedScale = true;
                    }
                }
            }
            next.tracks.push_back(std::move(buffer));
        }

        if (!copyBoundedVector<PaperReloadAnimationAnnotationV1>(
                found->annotationCount,
                kMaxMarkersPerClip,
                [&](PaperReloadAnimationAnnotationV1* output,
                    std::uint32_t capacity,
                    std::uint32_t* outCopied) {
                    return PaperApi::inst->copyReloadAnimationAnnotationsV1(
                        paperOwnerToken,
                        before.catalogSequence,
                        found->clipId,
                        0,
                        output,
                        capacity,
                        outCopied);
                },
                next.annotations) ||
            !copyBoundedVector<PaperReloadAnimationTriggerV1>(
                found->triggerCount,
                kMaxMarkersPerClip,
                [&](PaperReloadAnimationTriggerV1* output,
                    std::uint32_t capacity,
                    std::uint32_t* outCopied) {
                    return PaperApi::inst->copyReloadAnimationTriggersV1(
                        paperOwnerToken,
                        before.catalogSequence,
                        found->clipId,
                        0,
                        output,
                        capacity,
                        outCopied);
                },
                next.triggers)) {
            status = "Marker snapshot changed while loading";
            modelDirty = true;
            return false;
        }

        PaperReloadAnimationCatalogStateV1 after{};
        if (PaperApi::inst->getReloadAnimationCatalogStateV1(
                paperOwnerToken, &after) != PaperResultV1::Ok ||
            !catalogIdentityEqual(before, after)) {
            status = "Catalog revision advanced; retry selection";
            modelDirty = true;
            return false;
        }
        next.loaded = true;
        next.catalogSequence = after.catalogSequence;
        next.catalogRevision = after.catalogRevision;
        clearPreview();
        selected = std::move(next);
        previewTimeSeconds = 0.0f;
        previewRequested = false;
        mapSelectedTracks();
        status = "Exact clip loaded for non-native preview";
        modelDirty = true;
        return true;
    }

    void AuthoringRuntime::Impl::mapSelectedTracks()
    {
        if (!selected.loaded) {
            return;
        }
        std::unordered_map<std::string, std::vector<std::uint32_t>> byName;
        byName.reserve(evidenceCount);
        for (std::uint32_t index = 0; index < evidenceCount; ++index) {
            const auto name = authoring_timeline::normalizedBoneName(
                boundedString(evidence[index].sourceName, sizeof(evidence[index].sourceName)));
            if (!name.empty()) {
                byName[name].push_back(index);
            }
        }
        for (auto& track : selected.tracks) {
            track.evidenceIndex = -1;
            track.matchingEvidenceCount = 0;
            const auto name = authoring_timeline::normalizedBoneName(
                boundedString(track.track.boneName, sizeof(track.track.boneName)));
            const auto match = byName.find(name);
            if (match == byName.end()) {
                continue;
            }
            track.matchingEvidenceCount =
                static_cast<std::uint32_t>(match->second.size());
            if (match->second.size() == 1) {
                track.evidenceIndex = static_cast<std::int32_t>(match->second.front());
            }
        }
        std::stable_sort(
            selected.tracks.begin(), selected.tracks.end(),
            [](const TrackBuffer& left, const TrackBuffer& right) {
                if (left.track.chainDepth != right.track.chainDepth) {
                    return left.track.chainDepth < right.track.chainDepth;
                }
                return left.track.trackId < right.track.trackId;
            });
    }

    void AuthoringRuntime::Impl::clearPreview()
    {
        if (rockOwnerToken != 0 && RockProviderApi::inst &&
            RockProviderApi::inst->clearWeaponPartDriveTargetsV1) {
            const auto result = RockProviderApi::inst->clearWeaponPartDriveTargetsV1(
                rockOwnerToken);
            if (result == RockProviderResultV1::OwnerNotRegistered) {
                rockOwnerToken = 0;
            }
        }
        previewRequested = false;
    }

    void AuthoringRuntime::Impl::updatePreview(
        const RockProviderFrameSnapshot& snapshot)
    {
        const bool identityMatches = selected.loaded && previewRequested &&
            rockOwnerToken != 0 && snapshot.weaponGenerationKey != 0 &&
            selected.catalogSequence == catalog.catalogSequence &&
            selected.catalogRevision == catalog.catalogRevision &&
            catalog.weaponFormId == snapshot.weaponFormId &&
            catalog.weaponGenerationKey == snapshot.weaponGenerationKey &&
            snapshotAllowsPanel(snapshot) && g_paperToolkitConfig.authoringPanelEnabled;
        if (!identityMatches || !RockProviderApi::inst ||
            !RockProviderApi::inst->setWeaponPartDriveTargetsV1) {
            if (previewRequested) {
                clearPreview();
                modelDirty = true;
            }
            return;
        }

        const float duration = (std::max)(0.0f, selected.clip.durationSeconds);
        previewTimeSeconds = std::clamp(previewTimeSeconds, 0.0f, duration);
        std::array<RockProviderWeaponPartDriveTargetV1,
            rock::provider::ROCK_PROVIDER_MAX_WEAPON_PART_DRIVES_V1> drives{};
        std::uint32_t driveCount = 0;
        for (const auto& track : selected.tracks) {
            if (!track.previewEnabled || track.evidenceIndex < 0 ||
                track.unsupportedScale || track.samples.empty() ||
                driveCount >= drives.size()) {
                continue;
            }
            const auto evidenceIndex = static_cast<std::uint32_t>(track.evidenceIndex);
            if (evidenceIndex >= evidenceCount) {
                continue;
            }
            const auto& part = evidence[evidenceIndex];
            if (part.weaponGenerationKey != snapshot.weaponGenerationKey ||
                part.sourceRoot == 0) {
                continue;
            }

            auto upper = std::lower_bound(
                track.samples.begin(), track.samples.end(), previewTimeSeconds,
                [](const PaperReloadAnimationSampleV1& sample, float time) {
                    return sample.timeSeconds < time;
                });
            authoring_timeline::QsTransform value{};
            if (upper == track.samples.begin()) {
                value = toTimelineTransform(upper->transform);
            } else if (upper == track.samples.end()) {
                value = toTimelineTransform(track.samples.back().transform);
            } else {
                const auto& after = *upper;
                const auto& before = *(upper - 1);
                const float interval = after.timeSeconds - before.timeSeconds;
                const float fraction = interval > 1.0e-6f ?
                    (previewTimeSeconds - before.timeSeconds) / interval : 0.0f;
                if (!authoring_timeline::interpolate(
                        toTimelineTransform(before.transform),
                        toTimelineTransform(after.transform),
                        fraction,
                        value)) {
                    continue;
                }
            }

            auto& drive = drives[driveCount];
            drive.flags =
                static_cast<std::uint32_t>(
                    RockProviderWeaponPartTargetFlagV1::MatchBodyId) |
                static_cast<std::uint32_t>(
                    RockProviderWeaponPartTargetFlagV1::MatchSourceRoot) |
                static_cast<std::uint32_t>(
                    RockProviderWeaponPartTargetFlagV1::MatchSourceName);
            drive.driveSpace = RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
            drive.weaponGenerationKey = snapshot.weaponGenerationKey;
            drive.sourceRoot = part.sourceRoot;
            drive.bodyId = part.bodyId;
            drive.groupId = selected.clip.clipId;
            drive.priority = kPreviewPriority;
            drive.leaseFrames = kPreviewLeaseFrames;
            std::memcpy(
                drive.sourceName, part.sourceName, sizeof(drive.sourceName));
            drive.sourceName[sizeof(drive.sourceName) - 1] = '\0';
            if (!fillProviderTransform(value, drive.targetTransform)) {
                continue;
            }
            ++driveCount;
        }

        if (driveCount == 0) {
            clearPreview();
            status = "No unambiguous, representable part tracks to preview";
            modelDirty = true;
            return;
        }
        const auto result = RockProviderApi::inst->setWeaponPartDriveTargetsV1(
            rockOwnerToken, drives.data(), driveCount);
        if (result == RockProviderResultV1::OwnerNotRegistered) {
            rockOwnerToken = 0;
            previewRequested = false;
            modelDirty = true;
        } else if (result != RockProviderResultV1::Ok) {
            previewRequested = false;
            status = "ROCK rejected preview drives";
            modelDirty = true;
        }
    }

    void AuthoringRuntime::Impl::cancelPointer()
    {
        const bool hadLease = authoring_pointer_gate::reset(clickGate);
        if (hadLease && rockOwnerToken != 0 && RockProviderApi::inst &&
            RockProviderApi::inst->clearHandInputSuppressionV1) {
            const auto result = RockProviderApi::inst->clearHandInputSuppressionV1(
                rockOwnerToken, RockProviderHand::Left);
            if (result == RockProviderResultV1::OwnerNotRegistered) {
                rockOwnerToken = 0;
            }
        }
        if (pointerActive && prismaVR && view != 0) {
            (void)prismaVR->CancelSpatialPointer(view);
        }
        pointerActive = false;
    }

    bool AuthoringRuntime::Impl::samplePointerButton(
        const RockProviderFrameSnapshot& snapshot,
        bool routed)
    {
        RockProviderRawWandButtonStateV1 trigger{};
        RockProviderRawWandButtonStateV1 xButton{};
        const bool rawAvailable = RockProviderApi::inst &&
            RockProviderApi::inst->getRawWandButtonStateV1 &&
            RockProviderApi::inst->getRawWandButtonStateV1(
                RockProviderHand::Left, kLeftTriggerButtonId, &trigger) &&
            RockProviderApi::inst->getRawWandButtonStateV1(
                RockProviderHand::Left, kLeftXButtonId, &xButton) &&
            trigger.available != 0 && xButton.available != 0;
        const bool primaryDown = rawAvailable &&
            (trigger.held != 0 || xButton.held != 0);
        bool leaseAccepted = false;
        if (routed && rawAvailable && rockOwnerToken != 0 &&
            RockProviderApi::inst &&
            RockProviderApi::inst->setHandInputSuppressionV1) {
            RockProviderHandInputSuppressionRequestV1 request{};
            request.hand = RockProviderHand::Left;
            request.flags =
                static_cast<std::uint32_t>(
                    RockProviderHandInputSuppressionFlagV1::SuppressConfigModeChord) |
                static_cast<std::uint32_t>(
                    RockProviderHandInputSuppressionFlagV1::SuppressOpenVrGameInput);
            request.leaseFrames = kPointerSuppressionLeaseFrames;
            request.worldGeneration = snapshot.worldGeneration;
            request.skeletonGeneration = snapshot.skeletonGeneration;
            request.providerGeneration = snapshot.providerGeneration;
            const auto result = RockProviderApi::inst->setHandInputSuppressionV1(
                rockOwnerToken, &request);
            leaseAccepted = result == RockProviderResultV1::Ok;
            if (result == RockProviderResultV1::OwnerNotRegistered) {
                rockOwnerToken = 0;
            }
        }
        const auto gate = authoring_pointer_gate::advance(
            clickGate,
            snapshot.frameIndex,
            routed,
            rawAvailable,
            primaryDown,
            leaseAccepted);
        if (gate.clearLease && rockOwnerToken != 0 && RockProviderApi::inst &&
            RockProviderApi::inst->clearHandInputSuppressionV1) {
            (void)RockProviderApi::inst->clearHandInputSuppressionV1(
                rockOwnerToken, RockProviderHand::Left);
        }
        return gate.forwardPrimaryDown;
    }

    void AuthoringRuntime::Impl::updatePanel(
        const RockProviderFrameSnapshot& snapshot)
    {
        if (seenPanelRevision != g_paperToolkitConfig.authoringPanelRevision) {
            seenPanelRevision = g_paperToolkitConfig.authoringPanelRevision;
            panelLocalOrientation = configuredOrientation();
            cancelPointer();
            clearPreview();
            modelDirty = true;
        }
        if (!g_paperToolkitConfig.authoringPanelEnabled) {
            if (view != 0) {
                destroyView();
            }
            return;
        }
        ensurePrisma();
        if (!prisma || !prismaVR || view == 0 || !domReady) {
            return;
        }
        if (!snapshotAllowsPanel(snapshot)) {
            if (panelVisible) {
                prisma->Hide(view);
                panelVisible = false;
            }
            cancelPointer();
            clearPreview();
            return;
        }
        if (!panelLocalOrientation) {
            if (panelVisible) {
                prisma->Hide(view);
                panelVisible = false;
            }
            cancelPointer();
            return;
        }
        const auto pose = computePanelPose(snapshot, *panelLocalOrientation);
        const auto ray = computePointerRay(
            snapshot, spatialCapabilities.maxAbsoluteWorldPosition);
        const float physicalWidth = kBasePanelPhysicalWidthGameUnits *
            g_paperToolkitConfig.authoringPanelScale;
        const float physicalHeight = kBasePanelPhysicalHeightGameUnits *
            g_paperToolkitConfig.authoringPanelScale;
        if (!pose || !std::isfinite(physicalWidth) ||
            !std::isfinite(physicalHeight) || physicalWidth <= 0.0f ||
            physicalHeight <= 0.0f ||
            physicalWidth > spatialCapabilities.maxPhysicalDimension ||
            physicalHeight > spatialCapabilities.maxPhysicalDimension) {
            if (panelVisible) {
                prisma->Hide(view);
                panelVisible = false;
            }
            cancelPointer();
            return;
        }

        PRISMA_UI_VR_API::SpatialUpdateV1 spatial{};
        spatial.structSize = sizeof(spatial);
        spatial.coordinateSpace =
            PRISMA_UI_VR_API::SpatialCoordinateSpace::GameWorld;
        spatial.presentationMode =
            PRISMA_UI_VR_API::SpatialPresentationMode::WorldQuad;
        spatial.flags = PRISMA_UI_VR_API::SpatialUpdate_SceneDepthOcclusion;
        spatial.sequence = nextSpatialSequence++;
        spatial.pose.position[0] = pose->position.x;
        spatial.pose.position[1] = pose->position.y;
        spatial.pose.position[2] = pose->position.z;
        for (std::size_t i = 0; i < pose->orientation.size(); ++i) {
            spatial.pose.orientation[i] = pose->orientation[i];
        }
        spatial.dimensions.pixelWidth = kPanelWidthPixels;
        spatial.dimensions.pixelHeight = kPanelHeightPixels;
        spatial.dimensions.physicalWidth = physicalWidth;
        spatial.dimensions.physicalHeight = physicalHeight;
        const auto spatialResult = prismaVR->SubmitSpatialUpdate(view, &spatial);
        const bool spatialAccepted =
            spatialResult == PRISMA_UI_VR_API::SpatialResult::Ok ||
            spatialResult ==
                PRISMA_UI_VR_API::SpatialResult::PendingUpdateReplaced;
        if (!spatialAccepted) {
            cancelPointer();
            return;
        }
        if (!panelVisible) {
            prisma->Show(view);
            panelVisible = true;
        }

        if (!ray) {
            cancelPointer();
            return;
        }

        PRISMA_UI_VR_API::SpatialPointerStateV1 pointerState{};
        pointerState.structSize = sizeof(pointerState);
        const auto pointerStateResult =
            prismaVR->GetSpatialPointerState(view, &pointerState);
        constexpr std::uint32_t kRequiredRouteFlags =
            PRISMA_UI_VR_API::SpatialPointerState_Active |
            PRISMA_UI_VR_API::SpatialPointerState_Applied |
            PRISMA_UI_VR_API::SpatialPointerState_BackendReady |
            PRISMA_UI_VR_API::SpatialPointerState_Routed;
        const bool routed = pointerStateResult ==
                PRISMA_UI_VR_API::SpatialResult::Ok &&
            pointerState.pointerSourceId ==
                PRISMA_UI_VR_API::SpatialPointerSource_PhysicalLeftController &&
            (pointerState.stateFlags & kRequiredRouteFlags) == kRequiredRouteFlags &&
            (pointerState.stateFlags &
                (PRISMA_UI_VR_API::SpatialPointerState_Hit |
                    PRISMA_UI_VR_API::SpatialPointerState_Captured)) != 0;
        const bool primaryDown = samplePointerButton(snapshot, routed);

        PRISMA_UI_VR_API::SpatialPointerUpdateV1 pointer{};
        pointer.structSize = sizeof(pointer);
        pointer.coordinateSpace =
            PRISMA_UI_VR_API::SpatialCoordinateSpace::GameWorld;
        pointer.flags = PRISMA_UI_VR_API::SpatialPointerUpdate_Active;
        pointer.buttonLevels = primaryDown ?
            PRISMA_UI_VR_API::SpatialPointerButton_Primary : 0u;
        pointer.sequence = nextPointerSequence++;
        pointer.rayOrigin[0] = ray->origin.x;
        pointer.rayOrigin[1] = ray->origin.y;
        pointer.rayOrigin[2] = ray->origin.z;
        pointer.maxDistance = kPointerMaxDistanceGameUnits;
        pointer.rayDirection[0] = ray->direction.x;
        pointer.rayDirection[1] = ray->direction.y;
        pointer.rayDirection[2] = ray->direction.z;
        pointer.pointerSourceId =
            PRISMA_UI_VR_API::SpatialPointerSource_PhysicalLeftController;
        const auto pointerResult =
            prismaVR->SubmitSpatialPointerUpdate(view, &pointer);
        pointerActive = pointerResult == PRISMA_UI_VR_API::SpatialResult::Ok ||
            pointerResult ==
                PRISMA_UI_VR_API::SpatialResult::PendingUpdateReplaced;
        if (!pointerActive) {
            cancelPointer();
        }
    }

    json AuthoringRuntime::Impl::buildModel() const
    {
        json model{
            { "schemaVersion", 1 },
            { "status", status },
            { "connections", {
                { "paper", paperOwnerToken != 0 },
                { "rock", rockOwnerToken != 0 },
                { "prisma", prisma != nullptr && prismaVR != nullptr },
            } },
            { "weapon", {
                { "formId", catalog.weaponFormId },
                { "generation", catalog.weaponGenerationKey },
                { "partCount", evidenceCount },
            } },
            { "catalog", {
                { "sequence", catalog.catalogSequence },
                { "revision", catalog.catalogRevision },
                { "state", static_cast<std::uint32_t>(catalog.exactPreharvestState) },
                { "flags", catalog.statusFlags },
                { "clipCount", catalog.clipCount },
                { "exactClipCount", catalog.exactClipCount },
                { "liveClipCount", catalog.liveClipCount },
                { "files", catalog.exactAnimationFileCount },
                { "sampled", catalog.exactClipsSampled },
                { "rejected", catalog.exactClipsRejected },
                { "storedBytes", catalog.storedSampleBytes },
                { "budgetBytes", catalog.sampleStorageBudgetBytes },
            } },
            { "live", {
                { "active", (live.flags & 1u) != 0 },
                { "name", boundedString(live.animationName, sizeof(live.animationName)) },
                { "time", live.localTimeSeconds },
                { "fraction", live.fraction },
                { "duration", live.durationSeconds },
                { "activityId", live.activityId },
                { "concurrent", live.concurrentActivityCount },
            } },
            { "cache", {
                { "mode", "memory-only exact value snapshots" },
                { "writesSaveOrJson", false },
                { "maximumPreviewTargets", rock::provider::ROCK_PROVIDER_MAX_WEAPON_PART_DRIVES_V1 },
            } },
        };

        auto& clipArray = model["clips"] = json::array();
        const auto liveName = boundedString(live.animationName, sizeof(live.animationName));
        for (const auto& clip : clips) {
            const auto name = boundedString(clip.animationName, sizeof(clip.animationName));
            const bool exact = clip.acquisition ==
                    PaperReloadAnimationAcquisitionV1::ExactWeaponPreharvest &&
                clip.trackSpace == PaperReloadAnimationTrackSpaceV1::WeaponRootLocal;
            clipArray.push_back({
                { "id", clip.clipId },
                { "name", name },
                { "path", boundedString(clip.animationPath, sizeof(clip.animationPath)) },
                { "acquisition", acquisitionName(clip.acquisition) },
                { "exact", exact },
                { "activeNameMatch", !liveName.empty() && liveName == name },
                { "selected", selected.loaded && selected.clip.clipId == clip.clipId &&
                    selected.catalogSequence == catalog.catalogSequence &&
                    selected.catalogRevision == catalog.catalogRevision },
                { "duration", clip.durationSeconds },
                { "tracks", clip.trackCount },
                { "samples", clip.sampleCount },
                { "annotations", clip.annotationCount },
                { "triggers", clip.triggerCount },
                { "flags", clip.flags },
            });
        }

        auto& selection = model["selection"];
        selection = {
            { "loaded", selected.loaded },
            { "preview", previewRequested },
            { "time", previewTimeSeconds },
            { "duration", selected.loaded ? selected.clip.durationSeconds : 0.0f },
            { "id", selected.loaded ? selected.clip.clipId : 0u },
            { "name", selected.loaded ? boundedString(
                selected.clip.animationName, sizeof(selected.clip.animationName)) : "" },
            { "path", selected.loaded ? boundedString(
                selected.clip.animationPath, sizeof(selected.clip.animationPath)) : "" },
        };
        auto& tracks = selection["tracks"] = json::array();
        for (const auto& track : selected.tracks) {
            json mapping{
                { "state", track.matchingEvidenceCount == 0 ? "missing" :
                    (track.matchingEvidenceCount == 1 ? "mapped" : "ambiguous") },
                { "matches", track.matchingEvidenceCount },
            };
            if (track.evidenceIndex >= 0 &&
                static_cast<std::uint32_t>(track.evidenceIndex) < evidenceCount) {
                const auto& part = evidence[static_cast<std::uint32_t>(track.evidenceIndex)];
                mapping["bodyId"] = part.bodyId;
                mapping["sourceName"] = boundedString(part.sourceName, sizeof(part.sourceName));
                mapping["omodFormId"] = part.omodFormId;
                mapping["partKind"] = part.partKind;
                mapping["reloadRole"] = part.reloadRole;
                mapping["actionRole"] = part.actionRole;
            }
            tracks.push_back({
                { "id", track.track.trackId },
                { "name", boundedString(track.track.boneName, sizeof(track.track.boneName)) },
                { "boneIndex", track.track.boneIndex },
                { "parentBoneIndex", track.track.parentBoneIndex },
                { "chainDepth", track.track.chainDepth },
                { "samples", track.samples.size() },
                { "translationTravel", track.translationTravel },
                { "previewEnabled", track.previewEnabled },
                { "unsupportedScale", track.unsupportedScale },
                { "mapping", std::move(mapping) },
            });
        }
        auto& annotations = selection["annotations"] = json::array();
        for (const auto& marker : selected.annotations) {
            annotations.push_back({
                { "id", marker.annotationId },
                { "time", marker.timeSeconds },
                { "lane", boundedString(marker.trackName, sizeof(marker.trackName)) },
                { "text", boundedString(marker.text, sizeof(marker.text)) },
            });
        }
        auto& triggers = selection["triggers"] = json::array();
        for (const auto& marker : selected.triggers) {
            triggers.push_back({
                { "id", marker.triggerId },
                { "time", marker.localTimeSeconds },
                { "eventId", marker.eventId },
                { "text", boundedString(marker.eventName, sizeof(marker.eventName)) },
            });
        }
        return model;
    }

    void AuthoringRuntime::Impl::publishModel(bool force)
    {
        if (!domReady || !prisma || view == 0 || !modelDirty ||
            (!force && !panelVisible)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < nextModelPublish) {
            return;
        }
        const auto payload = buildModel().dump();
        prisma->InteropCall(view, "paperToolkitAuthoringUpdate", payload.c_str());
        modelDirty = false;
        nextModelPublish = now + kModelPublishInterval;
    }

    void AuthoringRuntime::Impl::handleUiEvent(const char* payload)
    {
        if (!payload) {
            return;
        }
        const std::string commandText(payload, strnlen(payload, 4097));
        if (commandText.size() > 4096) {
            return;
        }
        try {
            const auto command = json::parse(commandText);
            const auto action = command.value("action", std::string{});
            if (action == "selectClip" && command.contains("clipId") &&
                command["clipId"].is_number_unsigned()) {
                requestedClipId = command["clipId"].get<std::uint32_t>();
                selectionRequested = true;
            } else if (action == "scrub" && command.contains("time") &&
                command["time"].is_number()) {
                const float time = command["time"].get<float>();
                if (std::isfinite(time)) {
                    previewTimeSeconds = std::clamp(
                        time, 0.0f, selected.loaded ? selected.clip.durationSeconds : 0.0f);
                }
            } else if (action == "preview" && command.contains("enabled") &&
                command["enabled"].is_boolean()) {
                if (command["enabled"].get<bool>() && selected.loaded) {
                    previewRequested = true;
                } else {
                    clearPreview();
                }
            } else if (action == "trackPreview" && command.contains("trackId") &&
                command.contains("enabled") && command["trackId"].is_number_unsigned() &&
                command["enabled"].is_boolean()) {
                const auto trackId = command["trackId"].get<std::uint32_t>();
                for (auto& track : selected.tracks) {
                    if (track.track.trackId == trackId) {
                        track.previewEnabled = command["enabled"].get<bool>();
                        break;
                    }
                }
            } else if (action == "refresh") {
                catalog = {};
            }
            modelDirty = true;
        } catch (const std::exception&) {
            status = "Rejected malformed workstation command";
            modelDirty = true;
        }
    }

    void AuthoringRuntime::Impl::onFrame(const RockProviderFrameSnapshot& snapshot)
    {
        lastSnapshot = snapshot;
        active = this;
        ensureRockOwner();
        ensurePaperConsumer();
        refreshEvidence(snapshot);
        refreshPaperCatalog();
        if (selectionRequested) {
            selectionRequested = false;
            (void)loadSelectedClip(requestedClipId);
        }
        updatePanel(snapshot);
        updatePreview(snapshot);
        publishModel();
    }

    void AuthoringRuntime::Impl::shutdown()
    {
        clearPreview();
        destroyView();
        if (paperOwnerToken != 0 && PaperApi::inst &&
            PaperApi::inst->unregisterConsumerV1) {
            (void)PaperApi::inst->unregisterConsumerV1(paperOwnerToken);
        }
        if (rockOwnerToken != 0 && RockProviderApi::inst &&
            RockProviderApi::inst->unregisterConsumerV1) {
            (void)RockProviderApi::inst->unregisterConsumerV1(rockOwnerToken);
        }
        paperOwnerToken = 0;
        rockOwnerToken = 0;
        clips.clear();
        selected = {};
        catalog = {};
        live = {};
        evidence = {};
        evidenceCount = 0;
        evidenceGenerationKey = 0;
        prisma = nullptr;
        prismaVR = nullptr;
        if (active == this) {
            active = nullptr;
        }
    }

    AuthoringRuntime::AuthoringRuntime() : _impl(std::make_unique<Impl>()) {}
    AuthoringRuntime::~AuthoringRuntime() = default;

    void AuthoringRuntime::onGameLoaded()
    {
        _impl->onGameLoaded();
    }

    void AuthoringRuntime::onFrame(
        const rock::provider::RockProviderFrameSnapshot& snapshot)
    {
        _impl->onFrame(snapshot);
    }

    void AuthoringRuntime::shutdown()
    {
        _impl->shutdown();
    }
}
