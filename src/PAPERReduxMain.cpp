
#include <cstdint>

#include "ReduxConfig.h"
#include "ReduxLog.h"
#include "api/ROCKProviderApi.h"
#include "redux/ReduxRuntime.h"

namespace
{
    using namespace redux;

    /*
     * PAPER_Redux has no game hooks of its own for the frame loop: ROCK is a
     * hard requirement and its provider frame callback (dispatched at the end
     * of ROCK's own main-loop update, main thread) is the runtime tick. If
     * ROCK is missing or too old the plugin stays dormant and logs why.
     * The only direct engine access this plugin performs is reading the
     * weapon scene subtree and the animation graph chain — both generation-
     * gated against the snapshot ROCK hands us.
     */
    ReduxRuntime s_runtime{};
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
        const bool configReloaded = g_reduxConfig.processPendingReload();
        if (!g_reduxConfig.enabled) {
            if (s_runtimeWasEnabled) {
                s_runtime.shutdown();
                s_runtimeWasEnabled = false;
                logger::info("PAPERRedux: disabled by config; runtime shut down.");
            }
            return;
        }
        s_runtimeWasEnabled = true;
        // Re-record mode: while the key stays true, every INI save wipes the
        // learned data (game starts begin empty anyway).
        if (configReloaded && g_reduxConfig.resetLearnedPaths) {
            s_runtime.wipeLearnedPaths();
        }
        s_runtime.onFrame(*snapshot);
    }

    bool initializeRockApi()
    {
        using namespace rock::provider;

        const int err = RockProviderApi::initialize(
            ROCK_PROVIDER_API_VERSION,
            ROCK_PROVIDER_API_V1_WEAPON_PART_GRIP_STATE_TABLE_BYTES);
        if (err != 0) {
            switch (err) {
            case 1:
                logger::critical("PAPERRedux: ROCK.dll is not loaded. PAPER_Redux requires ROCK and is now DISABLED.");
                break;
            case 2:
                logger::critical("PAPERRedux: ROCKAPI_GetProviderApi export not found. Deploy a current ROCK.dll. PAPER_Redux is now DISABLED.");
                break;
            case 3:
                logger::critical("PAPERRedux: ROCK provider API returned null. PAPER_Redux is now DISABLED.");
                break;
            case 4:
                logger::critical("PAPERRedux: loaded ROCK API is older than required v{}. Deploy the matching ROCK.dll. PAPER_Redux is now DISABLED.", ROCK_PROVIDER_API_VERSION);
                break;
            case 5:
                logger::critical("PAPERRedux: loaded ROCK API table is missing the weapon-part grip-state functions. Deploy the matching ROCK.dll. PAPER_Redux is now DISABLED.");
                break;
            default:
                logger::critical("PAPERRedux: ROCK API initialization failed (error {}). PAPER_Redux is now DISABLED.", err);
                break;
            }
            return false;
        }

        RockProviderLimitsV1 limits{};
        if (!queryProviderLimitsV1(limits) ||
            !supportsWeaponPartInteractionV1(limits) ||
            !supportsWeaponPartGripStateV1(limits) ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::FrameCallbacks) ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponEvidence) ||
            !hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::ConsumerRegistrationV1)) {
            logger::critical(
                "PAPERRedux: ROCK provider is missing required features (bits={:#x}). Deploy the matching ROCK.dll. PAPER_Redux is now DISABLED.",
                limits.featureBits);
            return false;
        }

        logger::info(
            "PAPERRedux: ROCK provider API v{} (mod v{}) initialized; featureBits={:#x}.",
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
            logger::info("PAPERRedux: GameLoaded -- initializing ROCK provider API and loading config...");
            g_reduxConfig.load();

            s_rockReady = initializeRockApi();
            if (!s_rockReady) {
                return;
            }

            if (s_frameCallbackToken == 0) {
                s_frameCallbackToken = rock::provider::RockProviderApi::inst->registerFrameCallback(&onRockFrame, nullptr);
                if (s_frameCallbackToken == 0) {
                    logger::critical("PAPERRedux: ROCK frame-callback registration failed. PAPER_Redux is now DISABLED.");
                    s_rockReady = false;
                    return;
                }
            }
            logger::info("PAPERRedux: initialization complete (frame callback token={}). Waiting for ROCK frames...", s_frameCallbackToken);
        }

        if (msg->type == F4SE::MessagingInterface::kPostLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
            // Session boundary: learned paths, harvest state, and drive
            // sessions belong to the previous session (mirrors ROCK's old
            // PhysicsInteraction reset, which reset this whole stack).
            logger::info("PAPERRedux: new game session -- resetting runtime state...");
            s_runtime.shutdown();
            s_runtimeWasEnabled = false;
            if (s_rockReady) {
                g_reduxConfig.load();
            }
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    logger::init("PAPERRedux");

    logger::info("=== PAPERRedux v{} === F4SE Plugin Query ===", Version::NAME);
    logger::info("PAPERRedux: weapon-part reload runtime (ROCK provider API consumer)");

    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = "PAPERRedux";

    {
        std::string tmp(Version::NAME);
        std::erase(tmp, '.');
        a_info->version = std::stoi(tmp);
    }

    if (a_f4se->IsEditor()) {
        logger::critical("PAPERRedux: Loaded in editor, marking as incompatible.");
        return false;
    }

    if (!REL::Module::IsVR()) {
        logger::critical("PAPERRedux: Fallout 4 VR runtime required; refusing to load in non-VR runtime.");
        return false;
    }

    const auto requiredRuntime = F4SE::RUNTIME_LATEST_VR;

    if (a_f4se->RuntimeVersion() < requiredRuntime) {
        logger::critical("PAPERRedux: Unsupported runtime version {} (need >= {}).", a_f4se->RuntimeVersion().string(), requiredRuntime.string());
        return false;
    }

    logger::info("PAPERRedux: F4SE v{} query passed. Plugin compatible.", a_f4se->F4SEVersion().string());
    return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    logger::info("PAPERRedux: F4SEPlugin_Load -- initializing...");

    F4SE::Init(a_f4se, false);

    const auto* messaging = F4SE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("PAPERRedux: Failed to get F4SE MessagingInterface. Cannot continue.");
        return false;
    }
    messaging->RegisterListener(onF4SEMessage);

    logger::info("PAPERRedux: F4SEPlugin_Load complete. Waiting for GameLoaded event...");
    return true;
}
