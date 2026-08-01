
#include <cstdint>
#include <cstdio>

#include "PaperToolkitConfig.h"
#include "PaperToolkitLog.h"
#include "api/ROCKProviderApi.h"
#include "paper_toolkit/PaperToolkitRuntime.h"

namespace
{
    using namespace paper_toolkit;

    /*
     * PAPER_Toolkit has no game hooks of its own for the frame loop: ROCK is a
     * hard requirement and its provider frame callback (dispatched at the end
     * of ROCK's own main-loop update, main thread) is the runtime tick. If
     * ROCK is missing or too old the plugin stays dormant and logs why.
     * The only direct engine access this plugin performs is reading the
     * weapon scene subtree and the animation graph chain — both generation-
     * gated against the snapshot ROCK hands us.
     */
    PaperToolkitRuntime s_runtime{};
    std::uint64_t s_ownerToken = 0;
    std::uint64_t s_frameCallbackToken = 0;
    bool s_rockReady = false;
    bool s_runtimeWasEnabled = false;

    void ROCK_PROVIDER_CALL onRockFrame(const rock::provider::RockProviderFrameSnapshot* snapshot, void*)
    {
        if (!snapshot) {
            return;
        }
        // Hot reload applies on the frame thread, before this frame's gates
        // read the config (a mode switch reaches new grips the same frame,
        // an enable toggle tears down / revives the runtime right here).
        const bool configReloaded = g_paperToolkitConfig.processPendingReload();
        if (!g_paperToolkitConfig.enabled) {
            if (s_runtimeWasEnabled) {
                s_runtime.shutdown();
                s_runtimeWasEnabled = false;
                logger::info("PAPER_Toolkit: disabled by config; runtime shut down.");
            }
            return;
        }
        s_runtimeWasEnabled = true;
        // Re-record mode: while the key stays true, every INI save wipes the
        // compact authored/learned serving data (game starts begin empty).
        if (configReloaded && g_paperToolkitConfig.resetMotionData) {
            s_runtime.wipeMotionData();
        }
        s_runtime.onFrame(*snapshot);
    }

    bool initializeRockApi()
    {
        using namespace rock::provider;

        const int err = RockProviderApi::initialize(
            ROCK_PROVIDER_API_VERSION,
            ROCK_PROVIDER_API_V1_OWNER_FRAME_CALLBACKS_TABLE_BYTES);
        if (err != 0) {
            switch (err) {
            case 1:
                logger::critical("PAPER_Toolkit: ROCK.dll is not loaded. PAPER_Toolkit requires ROCK and is now DISABLED.");
                break;
            case 2:
                logger::critical("PAPER_Toolkit: ROCKAPI_GetProviderApi export not found. Deploy a current ROCK.dll. PAPER_Toolkit is now DISABLED.");
                break;
            case 3:
                logger::critical("PAPER_Toolkit: ROCK provider API returned null. PAPER_Toolkit is now DISABLED.");
                break;
            case 4:
                logger::critical("PAPER_Toolkit: loaded ROCK API is older than required v{}. Deploy the matching ROCK.dll. PAPER_Toolkit is now DISABLED.", ROCK_PROVIDER_API_VERSION);
                break;
            case 5:
                logger::critical("PAPER_Toolkit: loaded ROCK API table is missing the owner frame-callback extent. Deploy the matching ROCK.dll. PAPER_Toolkit is now DISABLED.");
                break;
            case 6:
                logger::critical("PAPER_Toolkit: ROCK provider descriptor is missing or invalid. Deploy the matching ROCK.dll. PAPER_Toolkit is now DISABLED.");
                break;
            default:
                logger::critical("PAPER_Toolkit: ROCK API initialization failed (error {}). PAPER_Toolkit is now DISABLED.", err);
                break;
            }
            return false;
        }

        RockProviderLimitsV1 limits{};
        if (!queryProviderLimitsV1(limits) ||
            !supportsWeaponPartInteractionV1(limits) ||
            !supportsWeaponPartGripStateV1(limits) ||
            !supportsOwnerFrameCallbacksV1() ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::FrameCallbacks) ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponEvidence) ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::ConsumerRegistrationV1)) {
            logger::critical(
                "PAPER_Toolkit: ROCK provider is missing required features (bits={:#x}). Deploy the matching ROCK.dll. PAPER_Toolkit is now DISABLED.",
                limits.featureBits);
            return false;
        }

        if (!RockProviderApi::inst->registerConsumerV1 ||
            !RockProviderApi::inst->unregisterConsumerV1 ||
            !RockProviderApi::inst->registerFrameCallbackForOwnerV1 ||
            !RockProviderApi::inst->unregisterFrameCallbackForOwnerV1) {
            logger::critical(
                "PAPER_Toolkit: ROCK provider owner registration/callback functions are null. "
                "Deploy the matching ROCK.dll. PAPER_Toolkit is now DISABLED.");
            return false;
        }

        logger::info(
            "PAPER_Toolkit: ROCK provider API v{} (mod v{}) initialized; featureBits={:#x}.",
            RockProviderApi::inst->getVersion(),
            RockProviderApi::inst->getModVersion(),
            limits.featureBits);
        return true;
    }

    void onF4SEMessage(F4SE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }

        if (msg->type == F4SE::MessagingInterface::kGameLoaded) {
            logger::info("PAPER_Toolkit: GameLoaded -- initializing ROCK provider API and loading config...");
            g_paperToolkitConfig.load();

            s_rockReady = initializeRockApi();
            if (!s_rockReady) {
                return;
            }

            if (s_frameCallbackToken == 0) {
                if (s_ownerToken == 0) {
                    rock::provider::RockProviderConsumerRegistrationV1 registration{};
                    std::snprintf(
                        registration.modName,
                        sizeof(registration.modName),
                        "PAPER_Toolkit Frames");
                    registration.requestedCapabilities = static_cast<std::uint32_t>(
                        rock::provider::RockProviderConsumerCapabilityV1::FrameSnapshots);
                    rock::provider::RockProviderConsumerHandleV1 handle{};
                    const auto result = rock::provider::RockProviderApi::inst->registerConsumerV1(
                        &registration,
                        &handle);
                    const bool granted =
                        result == rock::provider::RockProviderResultV1::Ok &&
                        handle.ownerToken != 0 &&
                        rock::provider::hasConsumerCapabilityV1(
                            handle.grantedCapabilities,
                            rock::provider::RockProviderConsumerCapabilityV1::FrameSnapshots);
                    if (!granted) {
                        if (handle.ownerToken != 0) {
                            (void)rock::provider::RockProviderApi::inst->unregisterConsumerV1(
                                handle.ownerToken);
                        }
                        logger::critical(
                            "PAPER_Toolkit: ROCK frame consumer registration failed (result={}, granted={:#x}). PAPER_Toolkit is now DISABLED.",
                            static_cast<std::uint32_t>(result),
                            handle.grantedCapabilities);
                        s_rockReady = false;
                        return;
                    }
                    s_ownerToken = handle.ownerToken;
                }

                const auto callbackResult =
                    rock::provider::RockProviderApi::inst->registerFrameCallbackForOwnerV1(
                        s_ownerToken,
                        &onRockFrame,
                        nullptr,
                        &s_frameCallbackToken);
                if (callbackResult != rock::provider::RockProviderResultV1::Ok ||
                    s_frameCallbackToken == 0) {
                    logger::critical("PAPER_Toolkit: ROCK frame-callback registration failed. PAPER_Toolkit is now DISABLED.");
                    (void)rock::provider::RockProviderApi::inst->unregisterConsumerV1(
                        s_ownerToken);
                    s_ownerToken = 0;
                    s_rockReady = false;
                    return;
                }
            }
            logger::info("PAPER_Toolkit: initialization complete (frame callback token={}). Waiting for ROCK frames...", s_frameCallbackToken);
        }

        if (msg->type == F4SE::MessagingInterface::kPostLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
            // Session boundary: learned paths, harvest state, and drive
            // sessions belong to the previous session (mirrors ROCK's old
            // PhysicsInteraction reset, which reset this whole stack).
            logger::info("PAPER_Toolkit: new game session -- resetting runtime state...");
            s_runtime.shutdown();
            s_runtimeWasEnabled = false;
            if (s_rockReady) {
                g_paperToolkitConfig.load();
            }
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    logger::init("PAPER_Toolkit");

    logger::info("=== PAPER_Toolkit v{} === F4SE Plugin Query ===", Version::NAME);
    logger::info("PAPER_Toolkit: weapon-part reload runtime (ROCK provider API consumer)");

    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = "PAPER_Toolkit";

    {
        std::string tmp(Version::NAME);
        std::erase(tmp, '.');
        a_info->version = std::stoi(tmp);
    }

    if (a_f4se->IsEditor()) {
        logger::critical("PAPER_Toolkit: Loaded in editor, marking as incompatible.");
        return false;
    }

    if (!REL::Module::IsVR()) {
        logger::critical("PAPER_Toolkit: Fallout 4 VR runtime required; refusing to load in non-VR runtime.");
        return false;
    }

    const auto requiredRuntime = F4SE::RUNTIME_LATEST_VR;

    if (a_f4se->RuntimeVersion() < requiredRuntime) {
        logger::critical("PAPER_Toolkit: Unsupported runtime version {} (need >= {}).", a_f4se->RuntimeVersion().string(), requiredRuntime.string());
        return false;
    }

    logger::info("PAPER_Toolkit: F4SE v{} query passed. Plugin compatible.", a_f4se->F4SEVersion().string());
    return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    logger::info("PAPER_Toolkit: F4SEPlugin_Load -- initializing...");

    F4SE::Init(a_f4se, false);

    const auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("PAPER_Toolkit: Failed to get F4SE MessagingInterface. Cannot continue.");
        return false;
    }
    messaging->RegisterListener(onF4SEMessage);

    logger::info("PAPER_Toolkit: F4SEPlugin_Load complete. Waiting for GameLoaded event...");
    return true;
}
