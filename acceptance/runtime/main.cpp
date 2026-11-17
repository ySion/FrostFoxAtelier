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

void native_instance_defaults() {
    struct Sample {
        std::int32_t serial;
    };
    static std::int32_t constructions;
    Runtime runtime;
    auto definition =
        NativeRegistration<Sample>{runtime, "gc.Sample", TypeKind::record}.field<&Sample::serial>("serial").build();
    definition.native.construct = [](void* address) noexcept -> Result<void> {
        std::construct_at(static_cast<Sample*>(address), Sample{++constructions});
        return {};
    };
    const auto sample = take(runtime.register_type(std::move(definition)));
    TypeDefinition holder;
    holder.name = "gc.SampleHolder";
    holder.kind = TypeKind::object;
    holder.fields = {{"sample", {sample}}};
    const auto type = take(runtime.register_type(std::move(holder)));
    const std::array path{take(runtime.describe(type))->fields[0].id, take(runtime.describe(sample))->fields[0].id};
    require(constructions == 0, "Publication ran a per-instance native constructor.");
    auto first = take(runtime.create(type));
    auto second = take(runtime.create(type));
    require(std::get<std::int32_t>(take(runtime.read(first.id(), path))) == 1 &&
                std::get<std::int32_t>(take(runtime.read(second.id(), path))) == 2 && constructions == 2,
            "A described object must run its nested native constructor once per instance.");
}

void sparse_sweep() {
    Runtime runtime;
    const auto type = take(runtime.register_type({"gc.Sparse", TypeKind::object, {}, {}, {}}));
    std::vector<ObjectRoot> survivors;
    for (unsigned i = 0; i != 8192; ++i)
        survivors.push_back(take(runtime.create(type)));
    auto victim = take(runtime.create(type));
    const auto dead = victim.id();
    victim.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::sweep)
        take(runtime.collect_step({1}));
    require(!runtime.type_of(dead) && runtime.pending_reclamation() == 1,
            "Logical death must precede sparse sweeping.");
    take(runtime.destroy(survivors.front().id()));
    auto reused = take(runtime.create(type));
    auto appended = take(runtime.create(type));
    const auto progress = take(runtime.collect_step({1}));
    require(progress.work_done == 1 && progress.reclaimed == 0 && progress.pending_reclamation == 1,
            "A sparse sweep must yield before scanning the whole occupied prefix.");
    collect(runtime);
    require(runtime.live_objects() == survivors.size() + 1 && take(runtime.type_of(reused.id())) == type &&
                take(runtime.type_of(appended.id())) == type,
            "Sparse sweeping reclaimed a reused or newly appended live slot.");
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
    native_instance_defaults();
    sparse_sweep();
    verify_reflection();
    atelier::acceptance::verify_default_allocator();
    std::puts("Runtime acceptance: PASS (root collection and cancelled cursor lifetime)");
}
