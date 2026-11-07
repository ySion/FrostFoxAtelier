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

    // Grow slots and stamp pages; the first two objects occupy slots 0 and 1.
    std::array<ObjectRoot, 130> roots;
    for (auto& root : roots)
        root = take(runtime.create(native));
    const auto across_pages = take(runtime.change_cursor());
    take(runtime.write(roots[61].id(), x, Scalar{11.f}));
    take(runtime.write(roots[129].id(), x, Scalar{23.f}));
    runtime.changed_objects(across_pages, changed);
    require(changed == std::vector{roots[61].id(), roots[129].id()},
            "Growing stamp pages lost writes or reported an untouched page.");
    const auto next_page = take(runtime.change_cursor());
    take(runtime.write(roots[62].id(), x, Scalar{17.f}));
    runtime.changed_objects(next_page, changed);
    require(changed == std::vector{roots[62].id()}, "A page-boundary write retained stale page results.");
    std::puts("[pass] Write stamps: one write, untouched control and cursor order");
}
