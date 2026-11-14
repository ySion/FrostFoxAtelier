#include "../helpers.hpp"
#include "math/math.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

namespace atelier::jit_acceptance {
namespace {
struct MathAbiHost {
    atelier::math::Vector3d value() const { return {1.25, -2.5, 3.75}; }
};
}

void verify_math_abi() {
    using namespace atelier::acceptance::helpers;
    Runtime runtime;
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
}
}
