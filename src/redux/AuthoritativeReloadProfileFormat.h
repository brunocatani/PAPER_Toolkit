#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "redux/MotionLibraryFormat.h"

namespace redux::motion_library::authoritative_profile_format
{
    [[nodiscard]] nlohmann::json toJson(const AuthoritativeReloadProfile& profile);
    [[nodiscard]] bool fromJson(
        const nlohmann::json& json,
        AuthoritativeReloadProfile& out,
        std::string* outError);
}
