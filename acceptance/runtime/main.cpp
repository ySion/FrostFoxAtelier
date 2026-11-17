#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "memory/acceptance.hpp"
#include <cstdio>
#include <memory>

void verify_reflection();

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct Links {
    ReferenceSlot first;

    ReferenceSlot second;

    static inline const void* traced{};
};

void collect(Runtime& runtime) {
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
}

void runtime_sequence() {
    Runtime runtime;
    const auto object_type = take(runtime.register_type({"gc.Object", TypeKind::object, {}, {}, {}}));
    auto owner = take(runtime.create(object_type));
    const auto identity = owner.id();
    take(runtime.begin_collection());
    collect(runtime);
    require(runtime.live_objects() == 1 && take(runtime.type_of(identity)) == object_type,
            "A rooted object was not retained.");
    owner.reset();
    take(runtime.begin_collection());
    collect(runtime);
    require(runtime.live_objects() == 0, "An unrooted object was not collected.");

    auto definition = NativeRegistration<Links>{runtime, "gc.Links", TypeKind::record}
                          .field<&Links::first>("first", {object_type, ValueForm::strong_reference})
                          .field<&Links::second>("second", {object_type, ValueForm::strong_reference})
                          .build();
    definition.fields[0].native_address = [](void* storage) noexcept -> void* {
        Links::traced = storage;
        return &static_cast<Links*>(storage)->first;
    };
    const auto links_type = take(runtime.register_type(std::move(definition)));
    auto links = std::make_unique<Links>();
    auto target = take(runtime.create(object_type));
    take(runtime.write_reference({object_type, ValueForm::strong_reference}, links->second, target.reference()));
    target.reset();
    auto root = take(runtime.root_value(links_type, std::as_bytes(std::span{links.get(), 1})));
    take(runtime.begin_collection());
    // Pause on actual field visitation, not an implementation-derived step count.
    unsigned steps{};
    while (Links::traced != links.get()) {
        require(runtime.collection_phase() != CollectionPhase::idle && ++steps < 10000,
                "GC did not visit the owned reference storage.");
        take(runtime.collect_step({1}));
    }
    root.reset();
    links.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0,
            "A cancelled GC cursor traced freed storage or retained its target.");

    // Forward-referenced empty described record embedded inline in outer record and object prototypes.
    const auto f32_type = take(runtime.native_type<float>());
    FieldDefinition holder_weight{"weight", {f32_type}};
    holder_weight.default_leaves = {Scalar{3.5f}};
    TypeDefinition outer_def;
    outer_def.name = "gc.OuterRecord";
    outer_def.kind = TypeKind::record;
    outer_def.fields = {{"marker", {TypeReference{"gc.EmptyRecord"}}}, {"scale", {f32_type}}};
    TypeDefinition holder_def;
    holder_def.name = "gc.HolderObject";
    holder_def.kind = TypeKind::object;
    holder_def.fields = {
        {"outer", {TypeReference{"gc.OuterRecord"}}}, {"empty", {TypeReference{"gc.EmptyRecord"}}}, holder_weight};
    TypeDefinition empty_def;
    empty_def.name = "gc.EmptyRecord";
    empty_def.kind = TypeKind::record;
    const auto empty_batch =
        take(runtime.register_types({std::move(outer_def), std::move(holder_def), std::move(empty_def)}));
    auto holder = take(runtime.create(empty_batch[1]));
    const auto* outer_info = take(runtime.describe(empty_batch[0]));
    const auto* holder_info = take(runtime.describe(empty_batch[1]));
    const auto* empty_info = take(runtime.describe(empty_batch[2]));
    require(empty_info->size == 1 && empty_info->plain_value && outer_info->plain_value,
            "Empty described record lost its 1-byte plain layout.");
    const std::array scale_path{holder_info->fields[0].id, outer_info->fields[1].id};
    require(std::get<float>(take(runtime.read(holder.id(), scale_path))) == 0.0f &&
                std::get<float>(take(runtime.read(holder.id(), holder_info->fields[2].id))) == 3.5f,
            "Outer prototype containing an empty described record did not initialize fields.");
    std::vector<std::byte> outer_bytes(outer_info->size);
    take(runtime.initialize_value(empty_batch[0], outer_bytes));
    take(runtime.destroy_value(empty_batch[0], outer_bytes));
}
}

int main() {
    atelier::install_fatal_handler();
    runtime_sequence();
    verify_reflection();
    atelier::acceptance::verify_default_allocator();
    std::puts("Runtime acceptance: PASS (root collection and cancelled cursor lifetime)");
}
