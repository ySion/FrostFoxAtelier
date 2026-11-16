#include "memory/fatal.hpp"
#include "math/math.hpp"
#include "../helpers.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>

namespace {
using namespace atelier::math;

using atelier::acceptance::helpers::require;

bool near(float a, float b) {
    return std::abs(a - b) <= std::numeric_limits<float>::epsilon() * 64 * std::max(1.f, std::abs(b));
}

void scene() {
    // A 120-degree turn about (1,1,1) maps x->y, y->z, z->x exactly.
    constexpr Quaternion turn{0.5f, 0.5f, 0.5f, 0.5f};
    const auto axis = try_normalize(Vector3{1, 1, 1});
    require(axis.has_value(), "Diagonal axis is normalizable.");
    const auto built_turn = from_axis_angle(*axis, 2 * std::numbers::pi_v<float> / 3);
    require(near(built_turn.x, turn.x) && near(built_turn.y, turn.y) && near(built_turn.z, turn.z) &&
                near(built_turn.w, turn.w),
            "Axis-angle quaternion diverged from analytic turn.");
    const auto z_quarter = from_axis_angle(Vector3{0, 0, 1}, std::numbers::pi_v<float> * 0.5f);
    const auto z_eighth = from_axis_angle(Vector3{0, 0, 1}, std::numbers::pi_v<float> * 0.25f);
    const auto halfway = slerp01(Quaternion{}, -z_quarter, 0.5f);
    require(near(halfway.x, z_eighth.x) && near(halfway.y, z_eighth.y) && near(halfway.z, z_eighth.z) &&
                near(halfway.w, z_eighth.w),
            "Quaternion slerp01 failed shortest-arc midpoint.");
    const auto slerp_low = slerp01(Quaternion{}, -z_quarter, -2.f);
    const auto slerp_high = slerp01(Quaternion{}, -z_quarter, 3.f);
    const auto slerp_nan = slerp01(z_quarter, Quaternion{}, std::numeric_limits<float>::quiet_NaN());
    require(near(slerp_low.x, 0.f) && near(slerp_low.y, 0.f) && near(slerp_low.z, 0.f) && near(slerp_low.w, 1.f),
            "Quaternion slerp01 failed t < 0 endpoint clamp.");
    require(near(slerp_high.x, z_quarter.x) && near(slerp_high.y, z_quarter.y) && near(slerp_high.z, z_quarter.z) &&
                near(slerp_high.w, z_quarter.w),
            "Quaternion slerp01 failed t > 1 shortest-arc endpoint clamp.");
    require(near(slerp_nan.x, z_quarter.x) && near(slerp_nan.y, z_quarter.y) && near(slerp_nan.z, z_quarter.z) &&
                near(slerp_nan.w, z_quarter.w),
            "Quaternion slerp01 failed NaN parameter clamp.");
    const auto z_quarter_near = from_axis_angle(Vector3{0, 0, 1}, std::numbers::pi_v<float> * 0.5f + 1e-5f);
    const auto near_same = slerp01(z_quarter, z_quarter_near, 0.5f);
    const auto near_clamped = slerp01(z_quarter, z_quarter_near, 100.f);
    require(near(dot(near_same, near_same), 1.f) && near(dot(near_clamped, near_clamped), 1.f) &&
                near(near_clamped.z, z_quarter_near.z) && near(near_clamped.w, z_quarter_near.w),
            "Near-collinear slerp01 lost unit norm or clamp.");
    const auto z_quarter_d = from_axis_angle(Vector3d{0, 0, 1}, std::numbers::pi * 0.5);
    const auto z_eighth_d = from_axis_angle(Vector3d{0, 0, 1}, std::numbers::pi * 0.25);
    const auto halfway_d = slerp01(Quaterniond{}, -z_quarter_d, 0.5);
    require(std::abs(halfway_d.z - z_eighth_d.z) <= 1e-14 && std::abs(halfway_d.w - z_eighth_d.w) <= 1e-14,
            "Double quaternion slerp01 failed shortest-arc midpoint.");

    // Approximate variants: exact endpoints, explicit error bounds, and special-value contracts.
    static_assert(fast_slerp01(turn, Quaternion{}, 0.f) == turn);
    static_assert(fast_slerp01(Quaternion{}, -turn, 1.f) == turn);
    static_assert(fast_slerp01(turn, Quaternion{}, -4.f) == turn);
    static_assert(fast_slerp01(Quaternion{}, -turn, 9.f) == turn);
    static_assert(fast_slerp01(turn, Quaternion{}, std::numeric_limits<float>::quiet_NaN()) == turn);
    static_assert(fast_slerp01(Quaterniond{0.5, 0.5, 0.5, 0.5}, Quaterniond{}, 0.0) == Quaterniond{0.5, 0.5, 0.5, 0.5});
    static_assert(fast_slerp01(Quaterniond{}, Quaterniond{-0.5, -0.5, -0.5, -0.5}, 1.0) ==
                  Quaterniond{0.5, 0.5, 0.5, 0.5});
    for (int step = 0; step <= 16; ++step) {
        const float angle_f = static_cast<float>(step) * (std::numbers::pi_v<float> * 0.98f / 16.f);
        const double angle_d = static_cast<double>(step) * (std::numbers::pi * 0.98 / 16.0);
        const auto target_f = from_axis_angle(*axis, angle_f);
        const auto axis_d = *try_normalize(Vector3d{1, -2, 0.5});
        const auto target_d = from_axis_angle(axis_d, angle_d);
        for (int k = 0; k <= 8; ++k) {
            const float tf = static_cast<float>(k) / 8.f;
            const double td = static_cast<double>(k) / 8.0;
            const auto exact_f = slerp01(Quaternion{}, (step & 1) ? -target_f : target_f, tf);
            const auto approx_f = fast_slerp01(Quaternion{}, (step & 1) ? -target_f : target_f, tf);
            const auto unit_f = *try_normalize(approx_f);
            const auto aligned_f = dot(exact_f, unit_f) < 0.f ? -unit_f : unit_f;
            const float angle_err_f =
                2.f * std::atan2(length(Vector4{exact_f.x - aligned_f.x, exact_f.y - aligned_f.y,
                                                exact_f.z - aligned_f.z, exact_f.w - aligned_f.w}),
                                 length(Vector4{exact_f.x + aligned_f.x, exact_f.y + aligned_f.y,
                                                exact_f.z + aligned_f.z, exact_f.w + aligned_f.w}));
            require(angle_err_f <= 1e-4f && std::abs(dot(approx_f, approx_f) - 1.f) <= 1e-4f,
                    "fast_slerp01(float) exceeded angular or norm bound.");
            const auto exact_d = slerp01(Quaterniond{}, (step & 1) ? -target_d : target_d, td);
            const auto approx_d = fast_slerp01(Quaterniond{}, (step & 1) ? -target_d : target_d, td);
            const auto unit_d = *try_normalize(approx_d);
            const auto aligned_d = dot(exact_d, unit_d) < 0.0 ? -unit_d : unit_d;
            const double angle_err_d =
                2.0 * std::atan2(length(Vector4d{exact_d.x - aligned_d.x, exact_d.y - aligned_d.y,
                                                 exact_d.z - aligned_d.z, exact_d.w - aligned_d.w}),
                                 length(Vector4d{exact_d.x + aligned_d.x, exact_d.y + aligned_d.y,
                                                 exact_d.z + aligned_d.z, exact_d.w + aligned_d.w}));
            require(angle_err_d <= 1e-7 && std::abs(dot(approx_d, approx_d) - 1.0) <= 1e-7,
                    "fast_slerp01(double) exceeded angular or norm bound.");
        }
    }

    constexpr float compile_rsqrt = fast_rsqrt(4.f);
    static_assert(compile_rsqrt > 0.4999f && compile_rsqrt < 0.5001f);
    constexpr double compile_rsqrtd = fast_rsqrt(0.25);
    static_assert(compile_rsqrtd > 1.999999999 && compile_rsqrtd < 2.000000001);
    for (const float x : {1e-42f, 1e-20f, 0.25f, 1.f, 7.5f, 1e20f, 1e36f}) {
        const double expected = 1.0 / std::sqrt(static_cast<double>(x));
        require(std::abs(static_cast<double>(fast_rsqrt(x)) - expected) / expected <= 1.5e-4,
                "fast_rsqrt(float) exceeded relative error bound.");
    }
    for (const double x : {1e-315, 1e-150, 0.25, 1.0, 13.75, 1e150, 1e300}) {
        const double expected = 1.0 / std::sqrt(x);
        require(std::abs(fast_rsqrt(x) - expected) / expected <= 1e-10,
                "fast_rsqrt(double) exceeded relative error bound.");
    }
    constexpr float inf_f = std::numeric_limits<float>::infinity();
    constexpr float nan_f = std::numeric_limits<float>::quiet_NaN();
    require(fast_rsqrt(0.f) == inf_f && !std::signbit(fast_rsqrt(0.f)) && fast_rsqrt(-0.f) == -inf_f &&
                fast_rsqrt(inf_f) == 0.f && !std::signbit(fast_rsqrt(inf_f)) && std::isnan(fast_rsqrt(-1.f)) &&
                std::isnan(fast_rsqrt(-inf_f)) && std::isnan(fast_rsqrt(nan_f)),
            "fast_rsqrt special values violated contract.");
    const auto rsqrt_vec = fast_rsqrt(Vector3{4.f, 16.f, 25.f});
    require(std::abs(rsqrt_vec.x - 0.5f) <= 1e-4f && std::abs(rsqrt_vec.y - 0.25f) <= 1e-4f &&
                std::abs(rsqrt_vec.z - 0.2f) <= 1e-4f,
            "Vector fast_rsqrt failed component-wise evaluation.");

    static_assert(fast_normalize_or_zero(Vector3{}) == Vector3{});
    static_assert(fast_normalize_or_zero(Quaternion{0.f, 0.f, 0.f, 0.f}) == Quaternion{0.f, 0.f, 0.f, 0.f});
    require(fast_normalize_or_zero(Vector3{inf_f, 1.f, 0.f}) == Vector3{} &&
                fast_normalize_or_zero(Vector3{nan_f, 1.f, 0.f}) == Vector3{},
            "Nonfinite Vector3 fast_normalize_or_zero must return zero.");
    const auto fast_unit_v = fast_normalize_or_zero(Vector3{3.f, -4.f, 12.f});
    require(std::abs(fast_unit_v.x - 3.f / 13.f) <= 1e-4f && std::abs(fast_unit_v.y + 4.f / 13.f) <= 1e-4f &&
                std::abs(fast_unit_v.z - 12.f / 13.f) <= 1e-4f,
            "Vector3 fast_normalize_or_zero exceeded error bound.");
    const auto fast_small_v = fast_normalize_or_zero(Vector3{1e-18f, 0.f, 0.f});
    require(std::abs(fast_small_v.x - 1.f) <= 1.5e-4f && fast_small_v.y == 0.f && fast_small_v.z == 0.f,
            "Small normal-squared Vector3 fast_normalize_or_zero exceeded error bound.");
    const auto fast_sub_v = fast_normalize_or_zero(Vector3{1e-21f, 0.f, 0.f});
    require(std::abs(fast_sub_v.x - 1.f) <= 2e-3f && fast_sub_v.y == 0.f && fast_sub_v.z == 0.f,
            "Subnormal-squared Vector3 fast_normalize_or_zero failed graceful degradation.");
    require(fast_normalize_or_zero(Vector3{1e25f, 1e25f, 0.f}) == Vector3{},
            "Overflowing Vector3 fast_normalize_or_zero must return zero.");
    const auto fast_unit_q = fast_normalize_or_zero(Quaterniond{1.0, 2.0, -2.0, 4.0});
    require(std::abs(dot(fast_unit_q, fast_unit_q) - 1.0) <= 1e-10 &&
                fast_normalize_or_zero(Quaterniond{0, 0, 0, 0}) == Quaterniond{0, 0, 0, 0},
            "Quaterniond fast_normalize_or_zero failed.");
    static_assert(lerp01(Vector2{2.f, -4.f}, Vector2{10.f, 8.f}, 0.25f) == Vector2{4.f, -1.f});
    static_assert(lerp01(Vector2{2.f, -4.f}, Vector2{10.f, 8.f}, -3.f) == Vector2{2.f, -4.f});
    static_assert(lerp01(Vector2{2.f, -4.f}, Vector2{10.f, 8.f}, 5.f) == Vector2{10.f, 8.f});
    static_assert(lerp01(Vector2{2.f, -4.f}, Vector2{10.f, 8.f}, std::numeric_limits<float>::quiet_NaN()) ==
                  Vector2{2.f, -4.f});
    static_assert(Vector3(Vector3d(Vector3{1.5f, -2.25f, 4.f})) == Vector3{1.5f, -2.25f, 4.f});
    static_assert(Quaternion(Quaterniond(turn)) == turn);
    static_assert(cwise_div(cwise_mul(Vector3{2, 3, 4}, Vector3{5, 6, 7}), Vector3{2, 3, 4}) == Vector3{5, 6, 7});
    constexpr auto local = trs_matrix(Vector3{4, 5, 6}, turn, Vector3{2, 3, 4});
    static_assert(Matrix4(Matrix4d(local)) == local);
    static_assert(local == Matrix4{{0, 2, 0, 0, 0, 0, 3, 0, 4, 0, 0, 0, 4, 5, 6, 1}});
    constexpr Matrix4 parent{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 1, 3, 1}};
    constexpr auto compile_combined = local * parent;
    const auto runtime_local = local;
    const auto runtime_parent = parent;
    const auto combined = runtime_local * runtime_parent;
    require(combined == compile_combined && combined == Matrix4{{0, 2, 0, 0, 0, 0, 3, 0, 4, 0, 0, 0, 2, 6, 9, 1}},
            "Matrix composition changed row-vector order.");
    constexpr Matrix4d local_d = trs_matrix(Vector3d{4, 5, 6}, Quaterniond{0.5, 0.5, 0.5, 0.5}, Vector3d{2, 3, 4});
    constexpr Matrix4d parent_d{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, 1, 3, 1}};
    constexpr auto compile_combined_d = local_d * parent_d;
    const auto runtime_local_d = local_d;
    const auto runtime_parent_d = parent_d;
    const auto combined_d = runtime_local_d * runtime_parent_d;
    require(combined_d == compile_combined_d, "Double matrix composition diverged between constexpr and runtime.");
    constexpr Vector4 sample_v4{1.25f, -2.5f, 3.75f, 0.5f};
    constexpr Vector4d sample_v4d{1.25, -2.5, 3.75, 0.5};
    constexpr auto compile_v4 = sample_v4 * compile_combined;
    constexpr auto compile_v4d = sample_v4d * compile_combined_d;
    constexpr auto compile_transpose = transpose(compile_combined);
    constexpr auto compile_transpose_d = transpose(compile_combined_d);
    constexpr auto compile_pt = transform_point(Vector3{1.5f, -2.25f, 3.5f}, compile_combined);
    constexpr auto compile_ptd = transform_point(Vector3d{1.5, -2.25, 3.5}, compile_combined_d);
    constexpr auto compile_dir = transform_direction(Vector3{1.5f, -2.25f, 3.5f}, compile_combined);
    constexpr auto compile_dird = transform_direction(Vector3d{1.5, -2.25, 3.5}, compile_combined_d);
    require(sample_v4 * combined == compile_v4 && sample_v4d * combined_d == compile_v4d,
            "Vector4 * Matrix4 diverged between constexpr and runtime.");
    require(transpose(combined) == compile_transpose && transpose(combined_d) == compile_transpose_d,
            "Matrix transpose diverged between constexpr and runtime.");
    require(transform_point(Vector3{1, 2, 3}, combined) == Vector3{14, 8, 15} &&
                transform_point(Vector3{1.5f, -2.25f, 3.5f}, combined) == compile_pt &&
                transform_point(Vector3d{1.5, -2.25, 3.5}, combined_d) == compile_ptd,
            "Point transform diverged between constexpr and runtime.");
    require(transform_direction(Vector3{1.5f, -2.25f, 3.5f}, combined) == compile_dir &&
                transform_direction(Vector3d{1.5, -2.25, 3.5}, combined_d) == compile_dird,
            "Direction transform diverged between constexpr and runtime.");
    static_assert(length_squared(Vector3{2, 3, 6}) == 49.f);
    static_assert(distance_squared(Vector3{1, 2, 3}, Vector3{3, 5, 9}) == 49.f);
    require(near(distance(Vector3{1, 2, 3}, Vector3{3, 5, 9}), 7.f), "Vector distance is incorrect.");
    static_assert(cwise_min(Vector3{1, -4, 3}, Vector3{-2, 5, 3}) == Vector3{-2, -4, 3});
    static_assert(cwise_max(Vector3{1, -4, 3}, Vector3{-2, 5, 3}) == Vector3{1, 5, 3});
    static_assert(cwise_abs(Vector3{-2.5f, 0.f, 4.f}) == Vector3{2.5f, 0.f, 4.f});
    static_assert(std::bit_cast<std::uint32_t>(cwise_abs(Vector2{-0.f, -1.f}).x) == 0u);
    require(!std::signbit(cwise_abs(Vector2{-0.f, -1.f}).x), "Runtime cwise_abs preserved negative zero sign.");
    static_assert(clamp(Vector3{-3, 2, 9}, Vector3{-1, 0, 4}, Vector3{5, 3, 6}) == Vector3{-1, 2, 6});
    static_assert(clamp(Vector3{-3, 0.5f, 9}, 0.f, 1.f) == Vector3{0, 0.5f, 1});
    static_assert(clamp(Vector2{std::numeric_limits<float>::quiet_NaN(), 3.f}, Vector2{-1.f, 5.f}, Vector2{2.f, 1.f}) ==
                  Vector2{-1.f, 5.f});
    static_assert(min_component(Vector4{3, -5, 2, -1}) == -5.f);
    static_assert(max_component(Vector4{3, -5, 7, -1}) == 7.f);
    static_assert(scale_matrix(Vector3{2, 3, 4}) * rotation_matrix(turn) * translation_matrix(Vector3{4, 5, 6}) ==
                  local);
    const Vector3 camera_eye{2, 4, -6};
    const Vector3 camera_target{-1, 1, 3};
    const auto view = try_look_at_lh(camera_eye, camera_target, Vector3{0, 1, 0});
    require(view.has_value(), "Valid look-at camera produces a view matrix.");
    const auto eye_in_view = transform_point(camera_eye, *view);
    const auto target_in_view = transform_point(camera_target, *view);
    require(near(eye_in_view.x, 0.f) && near(eye_in_view.y, 0.f) && near(eye_in_view.z, 0.f),
            "View matrix did not place camera eye at origin.");
    require(near(target_in_view.x, 0.f) && near(target_in_view.y, 0.f) &&
                near(target_in_view.z, distance(camera_eye, camera_target)),
            "View matrix did not align target along +Z.");
    require(!try_look_at_lh(camera_eye, camera_eye, Vector3{0, 1, 0}).has_value(),
            "Coincident eye and target must fail look-at.");
    require(!try_look_to_lh(camera_eye, Vector3{0, 2, 0}, Vector3{0, 1, 0}).has_value(),
            "Collinear forward and up must fail look-to.");
    require(!try_look_to_lh(Vector3{3e38f, 3e38f, 0.f}, Vector3{-1.f, -1.f, 0.f}, Vector3{0.f, 0.f, 1.f}).has_value(),
            "Overflowing view translation dot product must fail look-to.");
    constexpr auto empty_box = Bounds::empty();
    static_assert(is_empty(empty_box));
    static_assert(!is_empty(Bounds{}) && contains(Bounds{}, Vector3{}) && intersects(Bounds{}, Bounds{}));
    static_assert(is_empty(transform_bounds(empty_box, combined)));
    static_assert(!intersects(empty_box, empty_box));
    constexpr auto box = encapsulate(encapsulate(empty_box, Vector3{-1, 2, 0}), Vector3{3, -2, 6});
    static_assert(!is_empty(box) && box == Bounds{{-1, -2, 0}, {3, 2, 6}});
    static_assert(encapsulate(empty_box, box) == box && encapsulate(box, empty_box) == box);
    static_assert(encapsulate(Bounds{{-1, 0, 2}, {1, 3, 4}}, Bounds{{0, -2, 1}, {5, 2, 6}}) ==
                  Bounds{{-1, -2, 1}, {5, 3, 6}});
    constexpr Bounds inverted_x{{1, 0, 0}, {-1, 2, 2}};
    static_assert(is_empty(inverted_x) && !contains(inverted_x, Vector3{0, 1, 1}) && !intersects(inverted_x, box));
    constexpr auto negative_trs = trs_matrix(Vector3{4, -5, 6}, turn, Vector3{-2, 3, -4});
    constexpr auto transformed_box = transform_bounds(box, negative_trs);
    constexpr auto transformed_box_d = transform_bounds(Boundsd(box), Matrix4d(negative_trs));
    const auto runtime_negative_trs = negative_trs;
    Bounds corner_box = Bounds::empty();
    for (std::uint32_t i = 0; i < 8; ++i)
        corner_box = encapsulate(corner_box, transform_point(corner(box, i), runtime_negative_trs));
    require(transformed_box == corner_box && transform_bounds(box, runtime_negative_trs) == transformed_box &&
                transform_bounds(Boundsd(box), Matrix4d(runtime_negative_trs)) == transformed_box_d,
            "Affine bounds transform diverged from constexpr or eight-corner hull.");
    static_assert(Bounds(Boundsd(box)) == box);
    static_assert(center(box) == Vector3{1, 0, 3} && size(box) == Vector3{4, 4, 6} && extents(box) == Vector3{2, 2, 3});
    static_assert(corner(box, 0) == Vector3{-1, -2, 0} && corner(box, 7) == Vector3{3, 2, 6});
    static_assert(contains(box, Vector3{1, 0, 3}) && !contains(box, Vector3{4, 0, 3}));
    static_assert(intersects(box, Bounds{{3, 2, 6}, {5, 4, 8}}) && !intersects(box, Bounds{{4, 0, 0}, {5, 1, 1}}));
    static_assert(distance_squared(box, Vector3{1, 0, 3}) == 0.f);
    static_assert(distance_squared(box, Vector3{5, -5, 12}) == 49.f);
    static_assert(is_finite(box) && !is_finite(empty_box) && is_finite(turn) && is_finite(local));
    // x' = 4z+2, y' = 2x+6, z' = 3y+9; solve each equation independently.
    const Matrix4 expected_inverse{{0, 0, 0.25f, 0, 0.5f, 0, 0, 0, 0, 1.f / 3, 0, 0, -3, -3, -0.5f, 1}};
    const auto inverse = try_inverse(combined);
    require(inverse.has_value(), "Scene matrix is invertible.");
    for (std::size_t i = 0; i < 16; ++i)
        require(near((*inverse)[i], expected_inverse[i]), "Matrix inverse is incorrect.");
}
}

int main() {
    atelier::install_fatal_handler();
    scene();
    std::puts("Native math acceptance: PASS (analytic transform and inverse)");
    return 0;
}
