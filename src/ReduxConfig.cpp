#include "ReduxConfig.h"

#include "ReduxLog.h"

#include <SimpleIni.h>

namespace redux
{
    namespace
    {
        constexpr const char* kIniPath = "Data\\F4SE\\Plugins\\PAPERRedux.ini";
        constexpr const char* kSection = "PAPERRedux";
    }

    void ReduxConfig::load()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        const auto rc = ini.LoadFile(kIniPath);
        if (rc < 0) {
            RDX_LOG_INFO(Config, "No INI at '{}' (rc={}); using defaults (enabled={})", kIniPath, static_cast<int>(rc), enabled);
            return;
        }

        enabled = ini.GetBoolValue(kSection, "bEnabled", enabled);
        RDX_LOG_INFO(Config, "Config loaded from '{}': enabled={}", kIniPath, enabled);
    }
}
