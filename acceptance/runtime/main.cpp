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
}
}

int main() {
    atelier::install_fatal_handler();
    runtime_sequence();
    verify_reflection();
    atelier::acceptance::verify_default_allocator();
    std::puts("Runtime acceptance: PASS (root collection and cancelled cursor lifetime)");
}
