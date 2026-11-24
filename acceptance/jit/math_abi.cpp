#include "../helpers.hpp"
#include "../math/rotation_error.hpp"
#include "math/math.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"

#include <cmath>
#include <limits>
#include <numbers>

namespace atelier::jit_acceptance {
namespace {
struct MathAbiHost {
    atelier::math::Vector3d value() const { return {1.25, -2.5, 3.75}; }
};

struct PackedRig {
    float tag{7.0f};

    atelier::math::Matrix4 transform{};

    atelier::math::Quaternion spin{};

    atelier::math::Vector3 pivot{};

    float guard{42.5f};
};
}

void verify_math_abi() {
    using namespace atelier::acceptance::helpers;
    Runtime runtime;
    (void)take(
        NativeRegistration<PackedRig>{runtime, "abi.PackedRig", TypeKind::record}
            .field<&PackedRig::tag>("tag", {TypeReference::native<float>()}, offsetof(PackedRig, tag))
            .field<&PackedRig::transform>("transform", {TypeReference::native<math::Matrix4>()},
                                          offsetof(PackedRig, transform))
            .field<&PackedRig::spin>("spin", {TypeReference::native<math::Quaternion>()}, offsetof(PackedRig, spin))
            .field<&PackedRig::pivot>("pivot", {TypeReference::native<math::Vector3>()}, offsetof(PackedRig, pivot))
            .field<&PackedRig::guard>("guard", {TypeReference::native<float>()}, offsetof(PackedRig, guard))
            .publish());
    const auto type = take(NativeRegistration<MathAbiHost>{runtime, "abi.MathHost", TypeKind::object}
                               .method<&MathAbiHost::value>("value", true)
                               .publish());
    auto program = take(source::Program::create(runtime, type));
    auto candidate = take(source::Program::prepare(take(program->request("math-abi.source", R"(
        vector3d run() { return self.value() * 2.0d + vector3d(4.0d, 5.0d, -6.0d); }
    )"))));
    take(program->try_publish(candidate));
    auto object = take(runtime.create(type));
    math::Vector3d output{};
    take(program->entry().invoke(object.id(), std::span<const Scalar>{},
                                 ValueOutput{take(runtime.native_type<math::Vector3d>()), &output}));
    require(output == math::Vector3d{6.5, 0, 1.5}, "Native aggregate result or JIT sret changed coordinates.");

    auto packed_program = take(source::Program::create(runtime));
    auto packed_candidate = take(source::Program::prepare(take(packed_program->request("packed-math.source", R"(
        void step(abi.PackedRig& rig, const matrix4& delta_m, const quaternion& delta_q) {
            rig.transform = math.transpose(rig.transform * delta_m);
            rig.spin = math.conjugate(rig.spin * delta_q);
            rig.pivot = math.transform_point(math.rotate(rig.spin, rig.pivot), rig.transform);
        }
    )",
                                                                                       "step"))));
    take(packed_program->try_publish(packed_candidate));
    const auto step =
        take(take(packed_program->pin()).bind<void, PackedRig&, const math::Matrix4&, const math::Quaternion&>());
    const auto q0 = math::from_axis_angle(math::Vector3{0.0f, 1.0f, 0.0f}, 0.45f);
    const auto dq = math::from_axis_angle(math::Vector3{1.0f, 0.0f, 0.0f}, -0.3f);
    const auto m0 = math::trs_matrix(math::Vector3{1.5f, -2.0f, 0.75f}, q0, math::Vector3{1.2f, 0.8f, 1.1f});
    const auto dm = math::trs_matrix(math::Vector3{-0.5f, 3.0f, 1.25f}, dq, math::Vector3{0.9f, 1.3f, 0.95f});
    PackedRig actual{7.0f, m0, q0, {2.0f, -1.5f, 3.25f}, 42.5f};
    take(step.invoke({}, actual, dm, dq));
    const auto expected_m = math::transpose(m0 * dm);
    const auto expected_q = math::conjugate(q0 * dq);
    const auto expected_p =
        math::transform_point(math::rotate(expected_q, math::Vector3{2.0f, -1.5f, 3.25f}), expected_m);
    require(actual.tag == 7.0f && actual.guard == 42.5f,
            "Packed Vector3/Matrix4/Quaternion clobbered adjacent fields.");
    require(actual.transform == expected_m && actual.spin == expected_q && actual.pivot == expected_p,
            "Packed SIMD math diverged from native C++ evaluation.");

    auto math_program = take(source::Program::create(runtime));
    source::SourceSnapshot snapshot{"math-variants",
                                    1,
                                    {{"variants.source", 1,
                                      R"(
        void eval_slerp(quaternion& exact_out, quaternion& fast_out,
                        const quaternion& a, const quaternion& b, float t) {
            exact_out = math.slerp01(a, b, t);
            fast_out = math.fast_slerp01(a, b, t);
        }

        void eval_slerp_d(quaterniond& exact_out, quaterniond& fast_out,
                          const quaterniond& a, const quaterniond& b, double t) {
            exact_out = math.slerp01(a, b, t);
            fast_out = math.fast_slerp01(a, b, t);
        }

        float eval_rsqrt_f(float x) { return math.fast_rsqrt(x); }

        double eval_rsqrt_d(double x) { return math.fast_rsqrt(x); }

        void eval_fast_vec(vector3& rsqrt_out, vector3& norm_out, quaternion& qnorm_out,
                           const vector3& v, const quaternion& q) {
            rsqrt_out = math.fast_rsqrt(v);
            norm_out = math.fast_normalize_or_zero(v);
            qnorm_out = math.fast_normalize_or_zero(q);
        }

        void eval_packed(vector4& v, vector4& unit) {
            unit = math.fast_normalize_or_zero(v);
            v = math.fast_rsqrt(v);
        }

        void transpose_d(matrix4d& m) { m = math.transpose(m); }

        int stateless_divide(int a, int b) {
            return a / b;
        }
    )"}}};
    auto variants_candidate = take(source::Program::prepare(take(math_program->request(std::move(snapshot)))));
    take(math_program->try_publish(variants_candidate));

    const auto eval_slerp = take(take(math_program->pin("eval_slerp"))
                                     .bind<void, math::Quaternion&, math::Quaternion&, const math::Quaternion&,
                                           const math::Quaternion&, float>());
    const auto eval_slerp_d = take(take(math_program->pin("eval_slerp_d"))
                                       .bind<void, math::Quaterniond&, math::Quaterniond&, const math::Quaterniond&,
                                             const math::Quaterniond&, double>());
    const auto eval_rsqrt_f = take(take(math_program->pin("eval_rsqrt_f")).bind<float, float>());
    const auto eval_rsqrt_d = take(take(math_program->pin("eval_rsqrt_d")).bind<double, double>());
    const auto eval_fast_vec = take(take(math_program->pin("eval_fast_vec"))
                                        .bind<void, math::Vector3&, math::Vector3&, math::Quaternion&,
                                              const math::Vector3&, const math::Quaternion&>());
    const auto eval_packed = take(take(math_program->pin("eval_packed")).bind<void, math::Vector4&, math::Vector4&>());
    const auto transpose_d = take(take(math_program->pin("transpose_d")).bind<void, math::Matrix4d&>());
    math::Matrix4d matrix{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    const auto transposed = math::transpose(matrix);
    take(transpose_d.invoke({}, matrix));
    require(matrix == transposed, "JIT matrix4d transpose misplaced a component.");
    const auto stateless_divide =
        take(take(math_program->pin("stateless_divide")).bind<std::int32_t, std::int32_t, std::int32_t>());

    // Stateless Typed::invoke must still reject an unexpected receiver and report exact failure source locations.
    require(!stateless_divide.invoke(object.id(), 8, 2).has_value(),
            "Stateless Typed::invoke accepted a receiver on a free function.");
    require(take(stateless_divide.invoke({}, 18, 3)) == 6, "Stateless Typed::invoke returned wrong quotient.");
    const auto div_zero = stateless_divide.invoke({}, 18, 0);
    require(!div_zero.has_value() && div_zero.error().code == ErrorCode::invocation_failed &&
                div_zero.error().source.has_value() && div_zero.error().source->path == "variants.source" &&
                div_zero.error().source->line > 0,
            "Stateless Typed::invoke lost runtime failure diagnostics or source location.");

    const auto axis = *math::try_normalize(math::Vector3{1.0f, -2.0f, 0.5f});
    const auto axis_d = *math::try_normalize(math::Vector3d{1.0, -2.0, 0.5});
    for (const float angle : {0.0f, 1e-5f, 0.7f, 1.5707963f, 2.8f}) {
        const auto target_q = math::from_axis_angle(axis, angle);
        const auto target_qd = math::from_axis_angle(axis_d, static_cast<double>(angle));
        for (const float t : {-1.5f, 0.0f, 0.25f, 0.5f, 0.85f, 1.0f, 3.0f, std::numeric_limits<float>::quiet_NaN()}) {
            math::Quaternion jit_exact{};
            math::Quaternion jit_fast{};
            take(eval_slerp.invoke({}, jit_exact, jit_fast, math::Quaternion{}, -target_q, t));
            const auto cpp_exact = math::slerp01(math::Quaternion{}, -target_q, t);
            require(std::abs(jit_exact.x - cpp_exact.x) <= 1e-5f && std::abs(jit_exact.y - cpp_exact.y) <= 1e-5f &&
                        std::abs(jit_exact.z - cpp_exact.z) <= 1e-5f && std::abs(jit_exact.w - cpp_exact.w) <= 1e-5f,
                    "JIT math.slerp01(float) diverged from C++ slerp01.");
            require(atelier::acceptance::rotation_error(cpp_exact, jit_fast) <= 1e-4f &&
                        std::abs(math::dot(jit_fast, jit_fast) - 1.0f) <= 1e-4f,
                    "JIT math.fast_slerp01(float) exceeded angular or norm bound.");
            if (t == 0.0f || std::isnan(t))
                require(jit_fast == math::Quaternion{}, "JIT math.fast_slerp01(float) lost exact t=0/NaN endpoint.");
            if (t == 1.0f)
                require(jit_fast == target_q, "JIT math.fast_slerp01(float) lost exact shortest-arc t=1 endpoint.");

            const double td = std::isnan(t) ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(t);
            math::Quaterniond jit_exact_d{};
            math::Quaterniond jit_fast_d{};
            take(eval_slerp_d.invoke({}, jit_exact_d, jit_fast_d, math::Quaterniond{}, -target_qd, td));
            const auto cpp_exact_d = math::slerp01(math::Quaterniond{}, -target_qd, td);
            require(std::abs(jit_exact_d.x - cpp_exact_d.x) <= 1e-13 &&
                        std::abs(jit_exact_d.y - cpp_exact_d.y) <= 1e-13 &&
                        std::abs(jit_exact_d.z - cpp_exact_d.z) <= 1e-13 &&
                        std::abs(jit_exact_d.w - cpp_exact_d.w) <= 1e-13,
                    "JIT math.slerp01(double) diverged from C++ slerp01.");
            require(atelier::acceptance::rotation_error(cpp_exact_d, jit_fast_d) <= 1e-7 &&
                        std::abs(math::dot(jit_fast_d, jit_fast_d) - 1.0) <= 1e-7,
                    "JIT math.fast_slerp01(double) exceeded angular or norm bound.");
        }
    }

    constexpr float inf_f = std::numeric_limits<float>::infinity();
    constexpr float nan_f = std::numeric_limits<float>::quiet_NaN();
    for (const float x : {std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), 0.25f, 9.0f, 1e30f}) {
        const double expected = 1.0 / std::sqrt(static_cast<double>(x));
        const float actual_r = take(eval_rsqrt_f.invoke({}, x));
        require(std::abs(static_cast<double>(actual_r) - expected) / expected <= 1.5e-4,
                "JIT math.fast_rsqrt(float) exceeded relative error bound.");
    }
    require(take(eval_rsqrt_f.invoke({}, 0.0f)) == inf_f && take(eval_rsqrt_f.invoke({}, -0.0f)) == -inf_f &&
                take(eval_rsqrt_f.invoke({}, inf_f)) == 0.0f && !std::signbit(take(eval_rsqrt_f.invoke({}, inf_f))) &&
                std::isnan(take(eval_rsqrt_f.invoke({}, -4.0f))) && std::isnan(take(eval_rsqrt_f.invoke({}, nan_f))),
            "JIT math.fast_rsqrt(float) special values violated contract.");
    for (const double x : {std::numeric_limits<double>::min(), std::numeric_limits<double>::max(), 0.25, 16.0, 1e250}) {
        const double expected = 1.0 / std::sqrt(x);
        const double actual_r = take(eval_rsqrt_d.invoke({}, x));
        require(std::abs(actual_r - expected) / expected <= 1e-10,
                "JIT math.fast_rsqrt(double) exceeded relative error bound.");
    }

    constexpr double tiny_d = std::numeric_limits<double>::denorm_min();
    require(take(eval_rsqrt_d.invoke({}, tiny_d)) == std::numeric_limits<double>::infinity() &&
                std::isnan(take(eval_rsqrt_d.invoke({}, -tiny_d))),
            "JIT math.fast_rsqrt(double) subnormal inputs violated contract.");
    math::Vector3 vec_rsqrt{};
    math::Vector3 vec_norm{};
    math::Quaternion quat_norm{};
    take(eval_fast_vec.invoke({}, vec_rsqrt, vec_norm, quat_norm, math::Vector3{4.0f, 16.0f, 25.0f},
                              math::Quaternion{1.0f, 2.0f, -2.0f, 4.0f}));
    require(std::abs(vec_rsqrt.x - 0.5f) <= 1e-4f && std::abs(vec_rsqrt.y - 0.25f) <= 1e-4f &&
                std::abs(vec_rsqrt.z - 0.2f) <= 1e-4f,
            "JIT vector math.fast_rsqrt failed.");
    require(std::abs(math::length_squared(vec_norm) - 1.0f) <= 3e-4f &&
                std::abs(math::dot(quat_norm, quat_norm) - 1.0f) <= 3e-4f,
            "JIT math.fast_normalize_or_zero exceeded unit-norm bound.");
    take(eval_fast_vec.invoke({}, vec_rsqrt, vec_norm, quat_norm, math::Vector3{inf_f, 1.0f, 0.0f},
                              math::Quaternion{0.0f, 0.0f, 0.0f, 0.0f}));
    require(vec_norm == math::Vector3{} && quat_norm == math::Quaternion{0.0f, 0.0f, 0.0f, 0.0f},
            "JIT math.fast_normalize_or_zero failed zero/nonfinite fallback.");
    constexpr float tiny = std::numeric_limits<float>::denorm_min();
    constexpr float minimum = std::numeric_limits<float>::min();
    math::Vector4 packed{tiny, -tiny, minimum, -0.f};
    math::Vector4 packed_unit;
    take(eval_packed.invoke({}, packed, packed_unit));
    require(packed.x == inf_f && std::isnan(packed.y) && std::abs(packed.z * std::sqrt(minimum) - 1.f) <= 1.5e-4f &&
                packed.w == -inf_f,
            "JIT packed reciprocal square root failed at the normal/subnormal boundary.");
    packed = {1e-21f, 0.f, 0.f, 0.f};
    take(eval_packed.invoke({}, packed, packed_unit));
    take(eval_fast_vec.invoke({}, vec_rsqrt, vec_norm, quat_norm, math::Vector3{0x1p-63f, 0.f, 0.f},
                              math::Quaternion{0.f, -1e-21f, 0.f, 0.f}));
    require(packed_unit == math::Vector4{} && quat_norm == math::Quaternion{0.f, 0.f, 0.f, 0.f} &&
                std::abs(vec_norm.x - 1.f) <= 1.5e-4f,
            "JIT normalization failed at the normal/subnormal squared-length boundary.");

    // Static type checking must reject mismatched precision or unsupported shapes on new intrinsics.
    auto bad_slerp = source::Program::prepare(take(packed_program->request("bad-slerp.source", R"(
        quaterniond bad(quaterniond a, quaterniond b) { return math.slerp01(a, b, 0.5); }
    )")));
    require(!bad_slerp.has_value(), "math.slerp01 accepted mismatched float scalar for quaterniond.");
    auto bad_rsqrt = source::Program::prepare(take(packed_program->request("bad-rsqrt.source", R"(
        quaternion bad(quaternion q) { return math.fast_rsqrt(q); }
    )")));
    require(!bad_rsqrt.has_value(), "math.fast_rsqrt accepted a quaternion.");

    // S-SOP-1: hex & unsigned literals, broadcast/precision math casts, cwise_mul/div,
    // length_squared/distance/distance_squared, round (ties-to-even), radians/degrees/PI/TAU,
    // and integer min/max/clamp/abs (with checked INT_MIN overflow).
    auto sop_math_prog = take(source::Program::create(runtime));
    source::SourceSnapshot sop_math_snap{"sop_math_batch1", 1, {{"sop-math-batch1.source", 1, R"(
        [Flags] enum Mask : uint { A = 0x01u, B = 0x1fu, Top = 0x80000000u }
        const float HALF_TURN_RAD = math.PI;
        const double FULL_TURN_RAD_D = math.TAU_D;

        uint check_literals_and_ints(int signed_in, uint unsigned_in, int& abs_out) {
            const int min_i32 = -0x80000000;
            const int hex_adapt = 0x7f;
            const uint hex_hash = 0x7feb352du;
            if (min_i32 != -2147483648 || hex_adapt != 127 || hex_hash != 2146121005u) {
                return 0u;
            }
            // Exact > 2^24 integer clamping that float32 would round away.
            const int clamped_i = math.clamp(signed_in, -16777219, 16777219);
            const int reversed_i = math.clamp(signed_in, 10, -10);
            const uint clamped_u = math.clamp(unsigned_in, 16777217u, 4000000001u);
            abs_out = math.abs(signed_in);
            if (reversed_i != 10 || math.min(-5, 0) != -5 || math.max(0u, 0xffffffffu) != 0xffffffffu) {
                return 0u;
            }
            return uint(clamped_i == 16777219 ? 1 : 0) + (clamped_u == 4000000001u ? 2u : 0u);
        }

        void check_vectors_and_round(const vector3& a, const vector3& b, vector4& rounds_out, vector3& cwise_out,
                                     double& dist_wide_out, float& dist_rescaled_out) {
            const vector3 splat = vector3(2.5f);
            const vector3d wide_a = vector3d(a);
            const vector3d wide_b = vector3d(b) + vector3d(0.5d);
            dist_wide_out = math.distance_squared(wide_a, wide_b);
            const vector3 narrowed = vector3(wide_b);
            cwise_out = math.cwise_div(math.cwise_mul(narrowed, splat), 2.5f);
            // Large separation overflows distance_squared to +Inf in float, while rescaled distance stays finite.
            const vector3 far_a = vector3(1e25f, 0.0f, 0.0f);
            const vector3 far_b = vector3(-1e25f, 0.0f, 0.0f);
            dist_rescaled_out = math.is_finite(math.distance_squared(far_a, far_b))
                                    ? -1.0f
                                    : math.distance(far_a, far_b);
            rounds_out = math.round(vector4(0.5f, 1.5f, -0.5f, -1.5f));
            const float deg = math.degrees(HALF_TURN_RAD);
            const double rad_d = math.radians(180.0d);
            if (math.abs(deg - 180.0f) > 1e-4f || math.abs(rad_d - math.PI_D) > 1e-15d ||
                math.abs(FULL_TURN_RAD_D - 2.0d * math.PI_D) > 1e-15d) {
                rounds_out = vector4(-999.0f);
            }
        }
        )"}}};
    auto sop_math_cand = take(source::Program::prepare(take(sop_math_prog->request(std::move(sop_math_snap)))));
    take(sop_math_prog->try_publish(sop_math_cand));

    auto sop_ints = take(take(sop_math_prog->pin("check_literals_and_ints"))
                             .bind<std::uint32_t, std::int32_t, std::uint32_t, std::int32_t&>());
    auto sop_vecs = take(
        take(sop_math_prog->pin("check_vectors_and_round"))
            .bind<void, const math::Vector3&, const math::Vector3&, math::Vector4&, math::Vector3&, double&, float&>());

    std::int32_t abs_val = 0;
    require(take(sop_ints.invoke({}, 16777225, 4100000000u, abs_val)) == 3u && abs_val == 16777225,
            "Integer min/max/clamp lost >2^24 precision or hex/u literal evaluation failed.");
    require(take(sop_ints.invoke({}, -42, 16777218u, abs_val)) == 0u && abs_val == 42,
            "Signed math.abs failed on negative int.");
    const auto int_min_fail = sop_ints.invoke({}, std::numeric_limits<std::int32_t>::min(), 20000000u, abs_val);
    require(!int_min_fail.has_value() && int_min_fail.error().code == ErrorCode::invocation_failed &&
                int_min_fail.error().message.contains("INT_MIN"),
            "math.abs(INT_MIN) did not trap with checked integer_overflow.");

    math::Vector4 rounds_out{};
    math::Vector3 cwise_out{};
    double dist_wide_out = 0.0;
    float dist_rescaled_out = 0.0f;
    take(sop_vecs.invoke({}, math::Vector3{1.0f, -2.0f, 3.0f}, math::Vector3{2.5f, -1.5f, 4.5f}, rounds_out, cwise_out,
                         dist_wide_out, dist_rescaled_out));
    // wide_b = (3.0, -1.0, 5.0), wide_a = (1.0, -2.0, 3.0) -> diff = (2, 1, 2) -> dist_sq = 9.0.
    require(std::abs(dist_wide_out - 9.0) <= 1e-12 && std::abs(cwise_out.x - 3.0f) <= 1e-6f &&
                std::abs(cwise_out.y - (-1.0f)) <= 1e-6f && std::abs(cwise_out.z - 5.0f) <= 1e-6f &&
                std::abs(dist_rescaled_out - 2e25f) <= 1e19f,
            "Vector broadcast/precision conversion, cwise_mul/div, or rescaled distance failed.");
    require(rounds_out.x == 0.0f && !std::signbit(rounds_out.x) && rounds_out.y == 2.0f && rounds_out.z == 0.0f &&
                std::signbit(rounds_out.z) && rounds_out.w == -2.0f,
            "math.round violated IEEE round-ties-to-even or signed-zero preservation.");

    // Reject invalid unsigned negation, int <- uint literal suffix, and unsigned math.abs.
    require(!source::Program::prepare(take(packed_program->request("bad-neg-u.source", R"(
        int bad() { return -1u; }
    )")))
                 .has_value(),
            "Accepted unary minus on an explicit unsigned literal.");
    require(!source::Program::prepare(take(packed_program->request("bad-assign-u.source", R"(
        int bad() { int x = 1u; return x; }
    )")))
                 .has_value(),
            "Implicitly converted a 'u'-suffixed uint literal to int.");
    require(!source::Program::prepare(take(packed_program->request("bad-abs-u.source", R"(
        uint bad(uint x) { return math.abs(x); }
    )")))
                 .has_value(),
            "Accepted unsigned uint in math.abs.");
}
}
