#pragma once

#include <cstdint>
#include <string_view>

namespace redux
{
    /*
     * Which independent motion source may DRIVE a grabbed part. There is no
     * mixed/hybrid selector: exact authored data can never silently fall back
     * to runtime-learned observations, and learned mode never consumes an
     * authored path.
     */
    enum class MotionPathMode : std::uint8_t
    {
        // Only exact equipped-weapon animation preharvest drives parts.
        AuthoredOnly = 0,
        // Only runtime-learned paths drive parts.
        LearnedOnly = 1,
        /*
         * Clip scrub: the hand drives the live reload CLIP's time (Havok
         * user-controlled mode on the captured hkbClipGenerator) and the
         * ENGINE poses every part — no stored path geometry is replayed.
         * Stored paths are consulted only for their EXISTENCE by the
         * eligibility gate, never as drive-time geometry.
         */
        ClipScrub = 2,
    };

    [[nodiscard]] inline constexpr const char* motionPathModeName(MotionPathMode mode)
    {
        switch (mode) {
        case MotionPathMode::AuthoredOnly:
            return "authored";
        case MotionPathMode::LearnedOnly:
            return "learned";
        case MotionPathMode::ClipScrub:
            return "scrub";
        default:
            return "authored";
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
        if (equalsIgnoreCase(text, "authored")) {
            outMode = MotionPathMode::AuthoredOnly;
            return true;
        }
        if (equalsIgnoreCase(text, "learned")) {
            outMode = MotionPathMode::LearnedOnly;
            return true;
        }
        if (equalsIgnoreCase(text, "scrub")) {
            outMode = MotionPathMode::ClipScrub;
            return true;
        }
        return false;
    }
}
