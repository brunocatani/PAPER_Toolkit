#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace paper_toolkit::authoring_timeline
{
    struct QsTransform
    {
        std::array<float, 3> translate{};
        // Public PAPER samples use Havok's xyzw order.
        std::array<float, 4> rotate{ 0.0f, 0.0f, 0.0f, 1.0f };
        std::array<float, 3> scale{ 1.0f, 1.0f, 1.0f };
    };

    [[nodiscard]] inline bool finite(const QsTransform& value) noexcept
    {
        for (const float component : value.translate) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        for (const float component : value.rotate) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        for (const float component : value.scale) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool normalizeQuaternion(std::array<float, 4>& quaternion) noexcept
    {
        float normSquared = 0.0f;
        for (const float component : quaternion) {
            if (!std::isfinite(component)) {
                return false;
            }
            normSquared += component * component;
        }
        if (!std::isfinite(normSquared) || normSquared < 1.0e-8f) {
            return false;
        }
        const float inverseNorm = 1.0f / std::sqrt(normSquared);
        for (float& component : quaternion) {
            component *= inverseNorm;
        }
        return true;
    }

    [[nodiscard]] inline bool uniformPositiveScale(
        const std::array<float, 3>& scale,
        float& outUniformScale,
        float tolerance = 1.0e-4f) noexcept
    {
        if (!std::isfinite(scale[0]) || !std::isfinite(scale[1]) ||
            !std::isfinite(scale[2]) || scale[0] <= 0.0f ||
            scale[1] <= 0.0f || scale[2] <= 0.0f) {
            return false;
        }
        const float largest = (std::max)({ scale[0], scale[1], scale[2] });
        const float smallest = (std::min)({ scale[0], scale[1], scale[2] });
        const float allowed = tolerance * (std::max)(1.0f, largest);
        if (largest - smallest > allowed) {
            return false;
        }
        outUniformScale = (scale[0] + scale[1] + scale[2]) / 3.0f;
        return std::isfinite(outUniformScale) && outUniformScale > 0.0f;
    }

    [[nodiscard]] inline bool interpolate(
        const QsTransform& from,
        const QsTransform& to,
        float fraction,
        QsTransform& out) noexcept
    {
        if (!finite(from) || !finite(to) || !std::isfinite(fraction)) {
            return false;
        }
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        for (std::size_t i = 0; i < 3; ++i) {
            out.translate[i] = from.translate[i] +
                (to.translate[i] - from.translate[i]) * fraction;
            out.scale[i] = from.scale[i] +
                (to.scale[i] - from.scale[i]) * fraction;
        }

        auto fromRotation = from.rotate;
        auto toRotation = to.rotate;
        if (!normalizeQuaternion(fromRotation) || !normalizeQuaternion(toRotation)) {
            return false;
        }
        float dot = 0.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            dot += fromRotation[i] * toRotation[i];
        }
        // Quaternions q and -q encode the same orientation. Always take the
        // shortest interpolation arc so timeline scrubbing cannot flip.
        if (dot < 0.0f) {
            for (float& component : toRotation) {
                component = -component;
            }
        }
        for (std::size_t i = 0; i < 4; ++i) {
            out.rotate[i] = fromRotation[i] +
                (toRotation[i] - fromRotation[i]) * fraction;
        }
        return normalizeQuaternion(out.rotate) && finite(out);
    }

    [[nodiscard]] inline std::string normalizedBoneName(std::string_view name)
    {
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.remove_suffix(1);
        }
        const auto suffix = name.rfind(':');
        if (suffix != std::string_view::npos && suffix + 1 < name.size()) {
            bool numeric = true;
            for (std::size_t i = suffix + 1; i < name.size(); ++i) {
                if (name[i] < '0' || name[i] > '9') {
                    numeric = false;
                    break;
                }
            }
            if (numeric) {
                name = name.substr(0, suffix);
            }
        }

        std::string result;
        result.reserve(name.size());
        for (const unsigned char character : name) {
            if (character >= 'A' && character <= 'Z') {
                result.push_back(static_cast<char>(character + ('a' - 'A')));
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
        return result;
    }
}
