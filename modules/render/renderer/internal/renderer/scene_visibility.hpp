#pragma once

#include "math/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <limits>

namespace atelier::renderer::detail {

// Primary raster visibility only. Never remove these objects from the scene,
// material table, acceleration structures or off-camera lighting/shadow support.
class SceneFrustum {
public:
    SceneFrustum(const math::Matrix4& view, const math::Matrix4& projection, float width, float height,
                 float far_depth) {
        const auto vp = math::Matrix4d(view) * math::Matrix4d(projection);
        // Enclose both temporal jitter phases and fractional viewport edges.
        const double x = 1 + 2 / std::max(1.f, width);
        const double y = 1 + 2 / std::max(1.f, height);
        for (std::size_t row = 0; row < 4; ++row) {
            const auto offset = row * 4;
            const double cx = vp.elements[offset];
            const double cy = vp.elements[offset + 1];
            const double cz = vp.elements[offset + 2];
            const double cw = vp.elements[offset + 3];
            plane_pairs_[0][row] = {cw * x + cx, cw * x - cx};
            plane_pairs_[1][row] = {cw * y + cy, cw * y - cy};
            plane_pairs_[2][row] = {cz - far_depth * cw, cw - cz};
            // Uncombined magnitudes retain cancellation error in w +/- x/y/z.
            magnitude_[row] = std::max(std::max(std::abs(cw) * x + std::abs(cx), std::abs(cw) * y + std::abs(cy)),
                                       std::abs(cw) + std::abs(cz));
            // GPU matrices/positions are floats. If those cannot represent the
            // projection, keep the object and leave clipping to the raster path.
            finite_ &= std::isfinite(magnitude_[row]) && magnitude_[row] < std::numeric_limits<float>::max();
        }
    }

    [[nodiscard]] bool visible(const math::Bounds& bounds, const math::Matrix4& world) const {
        if (!finite_)
            return true;
        std::array<double, 4> center{};
        std::array<double, 4> extent{};
        for (std::size_t row = 0; row < 3; ++row) {
            center[row] = (double(bounds.minimum[row]) + bounds.maximum[row]) * .5;
            extent[row] = (double(bounds.maximum[row]) - bounds.minimum[row]) * .5;
        }
        center[3] = 1;
        // Bound the rounding in both world and clip transforms, even when
        // large coordinates cancel. No perspective divide or normalized plane
        // is needed, so eye/near-plane crossings and reflected scales are safe.
        const __m128d sign_mask = _mm_set1_pd(-0.0);
        std::array<__m128d, 4> w_lo;
        std::array<__m128d, 4> w_hi;
        __m128d mag_lo = _mm_setzero_pd();
        __m128d mag_hi = _mm_setzero_pd();
        for (std::size_t row = 0; row < 4; ++row) {
            const __m128 rf = _mm_loadu_ps(&world[row * 4]);
            w_lo[row] = _mm_cvtps_pd(rf);
            w_hi[row] = _mm_cvtps_pd(_mm_movehl_ps(rf, rf));
            const __m128d reach = _mm_set1_pd(std::abs(center[row]) + extent[row]);
            mag_lo = _mm_add_pd(mag_lo, _mm_mul_pd(reach, _mm_andnot_pd(sign_mask, w_lo[row])));
            mag_hi = _mm_add_pd(mag_hi, _mm_mul_pd(reach, _mm_andnot_pd(sign_mask, w_hi[row])));
        }
        alignas(16) double world_magnitude[4];
        _mm_store_pd(&world_magnitude[0], mag_lo);
        _mm_store_pd(&world_magnitude[2], mag_hi);
        double scale = 1;
        for (std::size_t i = 0; i < 4; ++i) {
            if (world_magnitude[i] >= std::numeric_limits<float>::max())
                return true;
            scale += world_magnitude[i] * magnitude_[i];
        }
        if (scale >= std::numeric_limits<float>::max())
            return true;
        const __m128d neg_epsilon = _mm_set1_pd(-64 * std::numeric_limits<float>::epsilon() * scale);
        for (const auto& pair : plane_pairs_) {
            const __m128d p0 = _mm_loadu_pd(pair[0].data());
            const __m128d p1 = _mm_loadu_pd(pair[1].data());
            const __m128d p2 = _mm_loadu_pd(pair[2].data());
            const __m128d p3 = _mm_loadu_pd(pair[3].data());
            __m128d maximum = _mm_setzero_pd();
            for (std::size_t row = 0; row < 3; ++row) {
                __m128d coeff = _mm_add_pd(_mm_mul_pd(_mm_unpacklo_pd(w_lo[row], w_lo[row]), p0),
                                           _mm_mul_pd(_mm_unpackhi_pd(w_lo[row], w_lo[row]), p1));
                coeff = _mm_add_pd(coeff, _mm_mul_pd(_mm_unpacklo_pd(w_hi[row], w_hi[row]), p2));
                coeff = _mm_add_pd(coeff, _mm_mul_pd(_mm_unpackhi_pd(w_hi[row], w_hi[row]), p3));
                maximum = _mm_add_pd(maximum,
                                     _mm_add_pd(_mm_mul_pd(_mm_set1_pd(center[row]), coeff),
                                                _mm_mul_pd(_mm_set1_pd(extent[row]), _mm_andnot_pd(sign_mask, coeff))));
            }
            __m128d coeff3 = _mm_add_pd(_mm_mul_pd(_mm_unpacklo_pd(w_lo[3], w_lo[3]), p0),
                                        _mm_mul_pd(_mm_unpackhi_pd(w_lo[3], w_lo[3]), p1));
            coeff3 = _mm_add_pd(coeff3, _mm_mul_pd(_mm_unpacklo_pd(w_hi[3], w_hi[3]), p2));
            coeff3 = _mm_add_pd(coeff3, _mm_mul_pd(_mm_unpackhi_pd(w_hi[3], w_hi[3]), p3));
            maximum = _mm_add_pd(maximum, coeff3);
            if (_mm_movemask_pd(_mm_cmplt_pd(maximum, neg_epsilon)) != 0)
                return false;
        }
        return true;
    }

private:
    std::array<std::array<std::array<double, 2>, 4>, 3> plane_pairs_{};

    std::array<double, 4> magnitude_{};

    bool finite_{true};
};
}
