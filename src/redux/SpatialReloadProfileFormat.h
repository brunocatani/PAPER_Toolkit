#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "redux/MotionLibraryFormat.h"

namespace redux::motion_library::spatial_profile_format
{
    [[nodiscard]] nlohmann::json toJson(const SpatialReloadProfile& profile);
    [[nodiscard]] bool fromJson(
        const nlohmann::json& json,
        SpatialReloadProfile& out,
        std::string* outError);
}
