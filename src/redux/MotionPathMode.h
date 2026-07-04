#pragma once

#include <cstdint>
#include <string_view>

namespace redux
{
    /*
     * Which stored motion-path source may DRIVE a grabbed part. Selection
     * happens at lookup time only: collection is always unrestricted (the
     * learner records every part and the harvest keeps extracting clips in
     * every mode), so switching modes via INI hot reload takes effect
     * immediately with whatever data both sources have accumulated.
     */
    enum class MotionPathMode : std::uint8_t
    {
        // Learned paths outrank authored clip strokes; authored bootstraps
        // parts the player has not taught yet (the original behavior).
        Hybrid = 0,
        // Only clip-harvested strokes drive parts.
        AuthoredOnly = 1,
        // Only runtime-learned paths drive parts.
        LearnedOnly = 2,
    };

    [[nodiscard]] inline constexpr const char* motionPathModeName(MotionPathMode mode)
    {
        switch (mode) {
        case MotionPathMode::AuthoredOnly:
            return "authored";
        case MotionPathMode::LearnedOnly:
            return "learned";
        case MotionPathMode::Hybrid:
        default:
            return "hybrid";
        }
    }

    // Case-insensitive parse; unknown values report failure so the caller
    // can warn and keep its current setting.
    [[nodiscard]] inline bool parseMotionPathMode(std::string_view text, MotionPathMode& outMode)
    {
        auto equalsIgnoreCase = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
                if (ca != b[i]) {
                    return false;
                }
            }
            return true;
        };
        if (equalsIgnoreCase(text, "hybrid")) {
            outMode = MotionPathMode::Hybrid;
            return true;
        }
        if (equalsIgnoreCase(text, "authored")) {
            outMode = MotionPathMode::AuthoredOnly;
            return true;
        }
        if (equalsIgnoreCase(text, "learned")) {
            outMode = MotionPathMode::LearnedOnly;
            return true;
        }
        return false;
    }
}
