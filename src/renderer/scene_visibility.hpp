#pragma once

#include "geometry/mesh.hpp"
#include "math/math.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace atelier::renderer::detail {

// Primary raster visibility only. Never remove these objects from the scene,
// material table, acceleration structures or off-camera lighting/shadow support.
class SceneFrustum {
public:
    SceneFrustum(const std::array<float, 16>& view, const std::array<float, 16>& projection, float width, float height,
                 float far_depth) {
        math::Matrix4d v;
        math::Matrix4d p;
        for (std::size_t i = 0; i < 16; ++i) {
            v.elements[i] = view[i];
            p.elements[i] = projection[i];
        }
        const auto vp = v * p;
        // Enclose both temporal jitter phases and fractional viewport edges.
        const double x = 1 + 2 / std::max(1.f, width);
        const double y = 1 + 2 / std::max(1.f, height);
        for (std::size_t row = 0; row < 4; ++row) {
            const auto offset = row * 4;
            const double cx = vp.elements[offset];
            const double cy = vp.elements[offset + 1];
            const double cz = vp.elements[offset + 2];
            const double cw = vp.elements[offset + 3];
            planes_[0][row] = cw * x + cx;
            planes_[1][row] = cw * x - cx;
            planes_[2][row] = cw * y + cy;
            planes_[3][row] = cw * y - cy;
            planes_[4][row] = cz - far_depth * cw;
            planes_[5][row] = cw - cz;
            // Uncombined magnitudes retain cancellation error in w +/- x/y/z.
            magnitude_[row] = std::max(
                {std::abs(cw) * x + std::abs(cx), std::abs(cw) * y + std::abs(cy), std::abs(cw) + std::abs(cz)});
            // GPU matrices/positions are floats. If those cannot represent the
            // projection, keep the object and leave clipping to the raster path.
            finite_ &= std::isfinite(magnitude_[row]) && magnitude_[row] < std::numeric_limits<float>::max();
        }
    }

    [[nodiscard]] bool visible(const geometry::Bounds& bounds, const std::array<float, 16>& world) const {
        if (!finite_)
            return true;
        std::array<double, 4> center{};
        std::array<double, 4> extent{};
        std::array<double, 4> world_magnitude{};
        for (std::size_t row = 0; row < 3; ++row) {
            center[row] = (double(bounds.minimum[row]) + bounds.maximum[row]) * .5;
            extent[row] = (double(bounds.maximum[row]) - bounds.minimum[row]) * .5;
        }
        center[3] = 1;
        // Bound the rounding in both world and clip transforms, even when
        // large coordinates cancel. No perspective divide or normalized plane
        // is needed, so eye/near-plane crossings and reflected scales are safe.
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t row = 0; row < 4; ++row)
                world_magnitude[column] +=
                    (std::abs(center[row]) + extent[row]) * std::abs(double(world[row * 4 + column]));
        double scale = 1;
        for (std::size_t i = 0; i < 4; ++i) {
            if (world_magnitude[i] >= std::numeric_limits<float>::max())
                return true;
            scale += world_magnitude[i] * magnitude_[i];
        }
        if (scale >= std::numeric_limits<float>::max())
            return true;
        const double epsilon = 64 * std::numeric_limits<float>::epsilon() * scale;
        for (const auto& plane : planes_) {
            double maximum = 0;
            for (std::size_t row = 0; row < 4; ++row) {
                double coefficient = 0;
                for (std::size_t column = 0; column < 4; ++column)
                    coefficient += double(world[row * 4 + column]) * plane[column];
                maximum += center[row] * coefficient + extent[row] * std::abs(coefficient);
            }
            if (maximum < -epsilon)
                return false;
        }
        return true;
    }

private:
    std::array<std::array<double, 4>, 6> planes_{};
    std::array<double, 4> magnitude_{};
    bool finite_{true};
};
}
