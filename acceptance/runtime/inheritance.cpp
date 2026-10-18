#include "inheritance_fixture.hpp"
#include "runtime/execution.hpp"
#include <array>
#include <cstdio>
#include <unordered_set>

using namespace atelier;
using namespace atelier::acceptance::inheritance;

namespace {
void publication_failures() {
    Runtime runtime;
    require(take(runtime.describe(runtime.object_type()))->name == "core.Object" &&
        take(runtime.is_a(runtime.object_type(), runtime.object_type())), "Object root bootstrap failed.");
    TypeDefinition a{"cycle.A", TypeKind::object, {}, {}, {}}, b{"cycle.B", TypeKind::object, {}, {}, {}};
    a.base = TypeReference{"cycle.B"}; b.base = TypeReference{"cycle.A"};
    reject(runtime.register_types({a, b}), ErrorCode::invalid_definition);
    reject(runtime.find_type("cycle.A"), ErrorCode::invalid_type);
    TypeDefinition record{"bad.Record", TypeKind::record, {}, {}, {}}; record.base = runtime.object_type();
    reject(runtime.register_type(record), ErrorCode::invalid_definition);
    TypeDefinition plain{"plain.Base", TypeKind::object, {}, {}, {}};
    const auto plain_type = take(runtime.register_type(plain));
    auto native = NativeRegistration<Derived>{runtime, "bad.Native", TypeKind::object}.base<Base>().build();
    native.base = plain_type;
    reject(runtime.register_type(std::move(native)), ErrorCode::invalid_definition);
    const auto types = register_types(runtime);
    TypeDefinition shadow{"bad.Shadow", TypeKind::object, {}, {}, {}}; shadow.base = types.base;
    shadow.fields = {{"hidden", {TypeReference::native<float>()}}};
    reject(runtime.register_type(shadow), ErrorCode::invalid_definition);
    shadow.fields.clear(); shadow.methods = {{"bump", {}, [](CallContext&) -> Result<void> { return {}; }}};
    reject(runtime.register_type(shadow), ErrorCode::invalid_definition);
    Failing::fail = true;
    const auto before = Base::destroyed;
    reject(runtime.create(types.failing), ErrorCode::construction_failed);
    Failing::fail = false;
    require(Base::live == 0 && Base::destroyed == before + 1 && Resource::live == 0 &&
        Resource::destroyed == 1 && runtime.root_count() == 0, "Failed derived construction did not unwind exactly once.");
    // Registration stays open after objects exist. New types may derive from
    // published ones, and published metadata keeps its address and identity.
    take(runtime.register_type({"after.Failure", TypeKind::object, {}, {}, {}}));
    const auto parent = take(runtime.find_type("after.Failure"));
    const auto* parent_info = take(runtime.describe(parent));
    const auto first = runtime.type_count();
    auto root = take(runtime.create(runtime.object_type()));
    TypeDefinition late{"after.Success", TypeKind::object, {}, {}, {}}; late.base = TypeReference{"after.Failure"};
    const auto late_type = take(runtime.register_type(late));
    auto late_root = take(runtime.create(late_type));
    require(runtime.type_count() == first + 1 && runtime.types(first) == std::vector{late_type} &&
        take(runtime.find_type("after.Success")) == late_type && take(runtime.describe(parent)) == parent_info &&
        take(runtime.is_a(late_type, parent)), "Late registration changed published metadata or missed the catalog.");
    reject(runtime.register_type(late), ErrorCode::duplicate_name);
    take(runtime.destroy(late_root.id()));
    take(runtime.destroy(root.id()));
    take(runtime.register_type({"after.Destroy", TypeKind::object, {}, {}, {}}));
}

void objects_and_views() {
    Runtime runtime, foreign;
    require(!runtime.belongs_to_context({}) && !foreign.belongs_to_context({}), "Null identity acquired a runtime context.");
    const auto types = register_types(runtime);
    const auto value = take(runtime.find_field(types.base, "value"));
    const auto bump = take(runtime.find_method(types.base, "bump"));
    require(take(runtime.find_field(types.derived, "value")) == value &&
        take(runtime.find_method(types.derived, "bump")) == bump, "Inheritance copied declaration identities.");
    require(take(runtime.describe(types.derived))->fields.front().id == value &&
        take(runtime.describe(types.derived))->methods.front().signature.receiver->type == types.base,
        "Effective member view lost declaration order or receiver type.");
    require(take(runtime.is_a(types.derived, types.base)) && take(runtime.is_a(types.described_native, types.base)) &&
        !take(runtime.is_a(types.base, types.derived)), "Object ancestry is incorrect.");
    reject(runtime.is_a(types.base, foreign.object_type()), ErrorCode::invalid_type);
    reject(runtime.is_a(types.base, take(runtime.native_type<float>())), ErrorCode::invalid_type);
    std::unordered_set<TypeId> type_set{types.base, types.base, types.derived, foreign.object_type()};
    require(type_set.size() == 3, "Opaque type hashing does not preserve identity.");

    for (const auto type : {types.derived, types.described_native, types.grand}) {
        const auto live_before = Base::live;
        const auto destroyed_before = Base::destroyed;
        auto object = take(runtime.create(type));
        require(runtime.belongs_to_context(object.id()) && !foreign.belongs_to_context(object.id()),
            "Live object identity lost its runtime context.");
        auto* native_base = Base::latest;
        if (type == types.derived) {
            require(static_cast<void*>(Derived::latest) != static_cast<void*>(static_cast<Base*>(Derived::latest)),
                "Fixture did not exercise an adjusted native base address.");
        }
        const auto extra = take(runtime.find_field(type, "extra"));
        take(runtime.write(object.id(), value, 20.f));
        take(runtime.write(object.id(), extra, 99.f));
        Scalar result{}; const std::array args{Scalar{2.f}};
        take(runtime.call(object.id(), bump, args, &result));
        require(std::get<float>(result) == 22 && native_base->value == 22 &&
            std::get<float>(take(runtime.read(object.id(), extra))) == 99,
            "Ancestor address/lifetime handling changed the wrong fields.");
        if (type == types.derived) {
            take(runtime.call(object.id(), take(runtime.find_method(type, "derived_value")), {}, &result));
            require(std::get<float>(result) == 99 && Derived::latest->sentinel == 0x12345678,
                "Own method index or native prefix was corrupted.");
        }
        auto view = take(runtime.hold(object.id(), types.base));
        const auto weak = take(runtime.observe(view.reference()));
        require(view.id() == object.id() && view.reference().description().type == types.base &&
            view.reference() != object.reference(), "Declared reference view replaced identity or value equality.");
        auto acquired = take(runtime.try_acquire(weak));
        require(acquired && acquired->reference().description().type == types.base,
            "Weak promotion lost its declaration view.");
        acquired.reset();
        require(take(runtime.cast_reference(view.reference(), type)) == object.reference(), "Checked downcast failed.");
        reject(runtime.cast_reference(view.reference(), types.leaf), ErrorCode::type_mismatch);
        const auto null_base = take(runtime.null_reference(types.base));
        require(take(runtime.cast_reference(null_base, type)).is_null(), "Related null conversion failed.");
        reject(runtime.cast_reference(null_base, types.leaf), ErrorCode::type_mismatch);
        const auto roots_before = runtime.root_count();
        reject(runtime.hold(object.id(), types.leaf), ErrorCode::type_mismatch);
        require(runtime.root_count() == roots_before, "Failed view acquisition published a root.");
        std::array<ObjectRoot, 1> slots;
        { ExecutionFrame frame(runtime, slots); take(frame.hold(0, object.id(), types.base));
          require(take(frame.reference(0)).description().type == types.base, "Frame did not bind a declared self view."); }

        auto holder = take(runtime.create(types.holder));
        ReferenceSlot invalid_slot;
        reject(runtime.write_reference({TypeId{}, ValueForm::strong_reference}, invalid_slot, object.reference()),
            ErrorCode::type_mismatch);
        require(invalid_slot.id() == ObjectId{}, "Invalid native slot description was mutated.");
        const auto strong_field = take(runtime.find_field(types.holder, "strong"));
        const auto weak_field = take(runtime.find_field(types.holder, "weak"));
        take(runtime.write_reference(holder.id(), strong_field, object.reference()));
        take(runtime.write_reference(holder.id(), weak_field, weak));
        require(take(runtime.read_reference(holder.id(), strong_field)) == view.reference(), "Field did not normalize its view.");
        auto unrelated = take(runtime.create(types.leaf));
        reject(runtime.write_reference(holder.id(), strong_field, unrelated.reference()), ErrorCode::type_mismatch);
        reject(runtime.write_reference(holder.id(), strong_field, weak), ErrorCode::type_mismatch);
        auto alien = take(foreign.create(foreign.object_type()));
        require(foreign.belongs_to_context(alien.id()) && !runtime.belongs_to_context(alien.id()),
            "Foreign live identity was treated as local.");
        reject(runtime.write_reference(holder.id(), strong_field, alien.reference()), ErrorCode::invalid_type);
        take(foreign.destroy(alien.id()));
        require(foreign.belongs_to_context(alien.id()) && !runtime.belongs_to_context(alien.id()),
            "Destroying a foreign object changed its identity context.");
        require(take(runtime.read_reference(holder.id(), strong_field)) == view.reference(), "Failed assignment changed the field.");
        reject(runtime.read(unrelated.id(), value), ErrorCode::invalid_member);
        reject(runtime.call(unrelated.id(), bump, args, &result), ErrorCode::invalid_member);

        auto hidden_target = take(runtime.create(types.leaf));
        auto own_target = take(runtime.create(types.leaf));
        const auto hidden_id = hidden_target.id(), own_id = own_target.id(), id = object.id();
        take(runtime.write_reference({types.leaf, ValueForm::strong_reference}, native_base->hidden, hidden_target.reference()));
        take(runtime.write_reference(id, take(runtime.find_field(type, "edge")), own_target.reference()));
        std::size_t edges{};
        take(runtime.visit_references(id, {&edges, [](void* count, const ObjectReference&) -> Result<void> {
            ++*static_cast<std::size_t*>(count); return {}; }}));
        require(edges == 2, "Inherited and derived edges were not enumerated exactly once.");
        hidden_target.reset(); own_target.reset(); object.reset(); view.reset(); unrelated.reset();
        collect(runtime);
        require(runtime.type_of(hidden_id).has_value() && runtime.type_of(own_id).has_value(),
            "Base-only view lost an ancestor or derived strong edge.");
        take(runtime.destroy(id));
        require(runtime.belongs_to_context(id) && !foreign.belongs_to_context(id), "Stale identity lost its original context.");
        require(!take(runtime.try_acquire(weak)), "Strong holder prevented explicit destruction.");
        reject(runtime.cast_reference(take(runtime.read_reference(holder.id(), strong_field)), type), ErrorCode::invalid_object);
        auto stale = take(runtime.cast_reference(take(runtime.read_reference(holder.id(), strong_field)), runtime.object_type()));
        require(stale.id() == id, "Stale upcast replaced identity.");
        require(Base::live == live_before && Base::destroyed == destroyed_before + 1,
            "Complete native/description object destroyed its base twice.");
        holder.reset(); collect(runtime);
        require(runtime.live_objects() == 0, "Inherited graph leaked objects.");
        auto reused = take(runtime.create(type));
        require(runtime.belongs_to_context(id) && runtime.belongs_to_context(reused.id()),
            "Slot reuse changed the provenance of an existing identity.");
        std::unordered_set<ObjectId> ids{id, reused.id(), id};
        require(ids.size() == 2 && reused.id() != id, "Object generation/hash rebound an old identity.");
        reused.reset(); collect(runtime);
    }
    auto plain = take(runtime.create(types.described_derived));
    const auto number = take(runtime.find_field(types.described_base, "number"));
    take(runtime.write(plain.id(), number, 123.f));
    take(runtime.write(plain.id(), take(runtime.find_field(types.described_derived, "extra")), 456.f));
    require(std::get<float>(take(runtime.read(plain.id(), number))) == 123,
        "Described derived field overwrote its base segment.");
    plain.reset(); collect(runtime);
    auto observed = take(runtime.create(types.grand));
    const auto weak = take(runtime.observe(take(runtime.cast_reference(observed.reference(), types.base))));
    observed.reset(); collect(runtime);
    require(!take(runtime.try_acquire(weak)), "A weak ancestor view retained a derived instance.");
}
}

void verify_runtime_inheritance() {
    publication_failures();
    objects_and_views();
    require(Base::live == 0 && Derived::live == 0 && Resource::live == 0, "Inheritance lifecycle left live native storage.");
    std::puts("[pass] Runtime inheritance: roots, views, native adjustment, described/native lifecycle, declaration IDs and complete GC");
}
