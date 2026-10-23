#include "runtime/execution.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace atelier;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

struct StampLeaf { float value{}; };
struct StampNative {
    float x{};
    StampLeaf leaf;
    Array<float> values;
    ReferenceSlot target;
    void bump() { x += 1.f; }
    float peek() const { return x; }
    void add_to(float& total) const { total += x; }
    float plus(const float& other) const { return other + x; }
};
}

void verify_write_stamps() {
    Runtime runtime;
    const auto types = take(runtime.register_types({
        NativeRegistration<StampLeaf>{runtime, "stamps.Leaf", TypeKind::record}.field<&StampLeaf::value>("value").build(),
        array_definition<float>(runtime, "stamps.FloatArray"),
        NativeRegistration<StampNative>{runtime, "stamps.Native", TypeKind::object}
            .field<&StampNative::x>("x").field<&StampNative::leaf>("leaf").field<&StampNative::values>("values")
            .field<&StampNative::target>("target", {runtime.object_type(), ValueForm::strong_reference})
            .method<&StampNative::bump>("bump").method<&StampNative::peek>("peek")
            .method<&StampNative::add_to>("add_to").method<&StampNative::plus>("plus").build()}));
    const auto leaf_type = types[0], native = types[2];
    const auto field = [&](const char* name) { return take(runtime.find_field(native, name)); };
    const auto method = [&](const char* name) { return take(runtime.find_method(native, name)); };
    const std::array x{field("x")}, leaf{field("leaf")}, values{field("values")}, target{field("target")};
    // Seventy objects span two write pages; a is on the first and b on the second.
    std::vector<ObjectRoot> roots;
    for (int i = 0; i < 70; ++i) roots.push_back(take(runtime.create(native)));
    const auto a = roots[3].id(), b = roots[66].id(), other = roots[5].id();
    std::vector<ObjectId> changed{other};
    const auto expect = [&](const char* what, auto&& action, std::vector<ObjectId> written) {
        const auto cursor = take(runtime.change_cursor());
        action();
        runtime.changed_objects(cursor, changed);
        require(changed == written, std::string(what) + ": changed_objects reported the wrong objects.");
        for (const auto id : {a, b, other})
            require(runtime.changed_since(id, cursor) == std::ranges::contains(written, id),
                std::string(what) + ": changed_since disagrees with the objects it wrote.");
    };

    expect("Runtime::write(Scalar)", [&] { take(runtime.write(a, x, Scalar{1.f})); }, {a});
    expect("Runtime::write(Scalar) on the second page", [&] { take(runtime.write(b, x, Scalar{1.f})); }, {b});
    expect("Two writes", [&] { take(runtime.write(b, x, Scalar{2.f})); take(runtime.write(a, x, Scalar{2.f})); }, {a, b});
    const StampLeaf next{2.f};
    expect("Runtime::write(ValueInput)", [&] { take(runtime.write(a, leaf, ValueInput{leaf_type, &next})); }, {a});
    StampLeaf previous{};
    take(runtime.read(a, leaf, ValueOutput{leaf_type, &previous}));
    expect("Runtime::restore", [&] { take(runtime.restore(a, leaf, ValueOutput{leaf_type, &previous})); }, {a});
    expect("insert_element", [&] { take(runtime.insert_element(a, values, 0)); take(runtime.insert_element(a, values, 0)); }, {a});
    expect("move_element", [&] { take(runtime.move_element(a, values, 0, 1)); }, {a});
    expect("remove_elements", [&] { take(runtime.remove_elements(a, values, 0)); }, {a});
    // The written slot's owner changes, not the referenced object.
    expect("write_reference", [&] { take(runtime.write_reference(a, target, roots[66].reference())); }, {a});
    const auto slot = take(runtime.reference_field(a, target));
    expect("apply_reference_changes", [&] {
        const std::array<ReferenceChange, 1> changes{ReferenceWrite{slot, roots[5].reference()}};
        take(runtime.apply_reference_changes(changes));
    }, {a});
    expect("with_native<T>", [&] {
        take(runtime.with_native<StampNative>(a, [](StampNative& value) -> Result<void> { value.x = 5.f; return {}; }));
    }, {a});
    expect("with_native<T> on a field", [&] {
        take(runtime.with_native<StampLeaf>(a, leaf, [](StampLeaf& value) -> Result<void> { value.value = 3.f; return {}; }));
    }, {a});
    expect("A method with a writable receiver", [&] { take(runtime.call(a, method("bump"), std::span<const Scalar>{})); }, {a});
    // A read-only receiver takes a writable field of another object: only that owner is written.
    expect("A writable field argument", [&] {
        const std::array arguments{Argument::field(a, x)};
        take(runtime.call(b, method("add_to"), arguments));
    }, {a});

    float total{};
    expect("with_native<const T>", [&] {
        total += take(runtime.with_native<const StampNative>(a, [](const StampNative& value) -> Result<float> { return value.x; }));
    }, {});
    expect("with_native<const T> on a field", [&] {
        total += take(runtime.with_native<const StampLeaf>(a, leaf, [](const StampLeaf& value) -> Result<float> { return value.value; }));
    }, {});
    expect("Reads", [&] {
        total += std::get<float>(take(runtime.read(a, x)));
        StampLeaf copy{};
        take(runtime.read(a, leaf, ValueOutput{leaf_type, &copy}));
        total += copy.value + float(take(runtime.element_count(a, values)));
        require(take(runtime.read_reference(a, target)).id() == other, "The reference write did not land.");
        require(take(runtime.validate(a)) == std::nullopt, "A stamped object failed validation.");
    }, {});
    expect("A method with a read-only receiver", [&] {
        Scalar result;
        take(runtime.call(a, method("peek"), std::span<const Scalar>{}, &result));
    }, {});
    expect("A read-only field argument", [&] {
        const std::array arguments{Argument::field(a, x)};
        Scalar result;
        take(runtime.call(b, method("plus"), arguments, &result));
    }, {});
    expect("Rejected writes before their field resolves", [&] {
        const std::array wrong{field("x"), field("x")};
        require(!runtime.write(a, wrong, Scalar{1.f}), "A path through a scalar resolved.");
    }, {});

    // Cursors advance; a writer that takes a cursor after its own writes does not see them.
    const auto first = take(runtime.change_cursor());
    take(runtime.write(a, x, Scalar{7.f}));
    const auto second = take(runtime.change_cursor());
    require(second > first && runtime.changed_since(a, first) && !runtime.changed_since(a, second),
        "A cursor taken after a write still reports it, or one taken before misses it.");
    runtime.changed_objects(second, changed);
    require(changed.empty(), "changed_objects reported a write that preceded its cursor.");

    // A borrow's writes carry the stamp taken when it began, so no cursor may split it.
    take(runtime.with_native<StampNative>(a, [&](StampNative&) -> Result<void> {
        const auto inside = runtime.change_cursor();
        require(!inside && inside.error().code == ErrorCode::busy, "A cursor was taken inside a mutable native borrow.");
        return {};
    }));
    take(runtime.with_native<const StampNative>(a, [&](const StampNative&) -> Result<void> {
        require(!runtime.change_cursor(), "A cursor was taken inside a const native borrow.");
        return {};
    }));
    {
        auto scope = take(ExecutionScope::enter(runtime));
        require(!runtime.change_cursor(), "A cursor was taken during generated execution.");
    }

    // Creation stamps; destroyed objects leave changed_objects and stay changed by identity.
    {
        const auto cursor = take(runtime.change_cursor());
        auto created = take(runtime.create(native));
        runtime.changed_objects(cursor, changed);
        require(changed == std::vector{created.id()} && runtime.changed_since(created.id(), cursor), "Creation did not stamp the object.");
    }
    const auto before = take(runtime.change_cursor());
    take(runtime.write(other, x, Scalar{4.f}));
    take(runtime.destroy(other));
    roots[5].reset();
    const auto after = take(runtime.change_cursor());
    runtime.changed_objects(before, changed);
    require(changed.empty() && runtime.changed_since(other, after), "A destroyed object was listed or reported unchanged.");
    auto reused = take(runtime.create(native));
    const auto latest = take(runtime.change_cursor());
    require(reused.id() != other && runtime.changed_since(other, latest) && runtime.changed_since(reused.id(), after) &&
        !runtime.changed_since(reused.id(), latest), "A reused slot confused the old and new identities.");
    Runtime foreign;
    auto stranger = take(foreign.create(foreign.object_type()));
    require(runtime.changed_since(stranger.id(), 0) && runtime.changed_since(ObjectId{}, 0),
        "A foreign or null identity was reported unchanged.");
    require(total != 0.f, "Read results were not observed.");
    std::puts("[pass] Write stamps: every write entry point, const borrows and reads unstamped, cursor order, busy cursors, page scan and lifetimes");
}
