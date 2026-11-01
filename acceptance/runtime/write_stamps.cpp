#include "../helpers.hpp"
#include "runtime/native.hpp"

#include <array>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct StampNative {
    float x{};
};
}

void verify_write_stamps() {
    Runtime runtime;
    const auto native =
        take(runtime.register_type(NativeRegistration<StampNative>{runtime, "stamps.Native", TypeKind::object}
                                       .field<&StampNative::x>("x")
                                       .build()));
    const std::array x{take(runtime.find_field(native, "x"))};
    auto written = take(runtime.create(native));
    auto untouched = take(runtime.create(native));
    const auto first = take(runtime.change_cursor());
    take(runtime.write(written.id(), x, Scalar{7.f}));
    const auto second = take(runtime.change_cursor());
    std::vector<ObjectId> changed;
    runtime.changed_objects(first, changed);
    require(changed == std::vector{written.id()} && runtime.changed_since(written.id(), first) &&
                !runtime.changed_since(untouched.id(), first),
            "Write tracking missed the written object or included the untouched object.");
    require(second > first && !runtime.changed_since(written.id(), second),
            "A cursor taken after a write still reports it.");
    runtime.changed_objects(second, changed);
    require(changed.empty(), "changed_objects reported a write that preceded its cursor.");
    std::puts("[pass] Write stamps: one write, untouched control and cursor order");
}
