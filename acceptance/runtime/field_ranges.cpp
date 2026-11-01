#include "../helpers.hpp"
#include "runtime/foundation.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct RangedOwner {
    math::Vector3 albedo{0.5f, 0.5f, 0.5f};
};
}

void verify_field_ranges() {
    Runtime runtime;
    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const auto owner =
        take(runtime.register_type(NativeRegistration<RangedOwner>{runtime, "ranges.Owner", TypeKind::object}
                                       .field<&RangedOwner::albedo>("albedo")
                                       .range({.hard_min = Scalar{0.0f}, .hard_max = Scalar{1.0f}})
                                       .build()));
    auto object = take(runtime.create(owner));
    const std::array path{take(runtime.find_field(owner, "albedo"))};
    const auto read_albedo = [&] {
        math::Vector3 value;
        take(runtime.read(object.id(), path, ValueOutput{vector3, &value}));
        return value;
    };
    math::Vector3 input{2.0f, 0.25f, -1.0f};
    take(runtime.write(object.id(), path, ValueInput{vector3, &input}));
    require(read_albedo() == math::Vector3{1.0f, 0.25f, 0.0f}, "A whole vector write ignored per-component bounds.");
    input = {0.1f, 0.2f, std::numeric_limits<float>::quiet_NaN()};
    require(!runtime.write(object.id(), path, ValueInput{vector3, &input}), "The fault injection did not fail.");
    require(read_albedo() == math::Vector3{1.0f, 0.25f, 0.0f}, "A rejected vector write changed earlier components.");
    std::puts("[pass] Field ranges: vector clamp and failed-write atomicity");
}
