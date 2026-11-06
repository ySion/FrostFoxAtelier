#include "inheritance_fixture.hpp"
#include "memory/fatal.hpp"
#include <array>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

using atelier::acceptance::inheritance::Prefix;

struct Element {
    ReferenceSlot weak_link;

    ReferenceSlot strong_link;
};

struct Holder {
    ReferenceSequence items;

    ReferenceSequence hidden;

    ReferenceSlot tail;
};

struct HolderDerived : Prefix, Holder {};

std::vector<TypeId> publish(Runtime& runtime) {
    TypeDefinition mixed{"sequence.MixedHolder", TypeKind::object, {}, {}, {}};
    mixed.base = TypeReference::native<HolderDerived>();
    return take(runtime.register_types({
        NativeRegistration<Element>{runtime, "sequence.Element", TypeKind::object}
            .hidden_field<&Element::weak_link>("weak_link", {runtime.object_type(), ValueForm::weak_reference})
            .hidden_field<&Element::strong_link>("strong_link", {runtime.object_type(), ValueForm::strong_reference})
            .build(),
        NativeRegistration<Holder>{runtime, "sequence.Holder", TypeKind::object}
            .field<&Holder::items>("items", {TypeReference::native<Element>(), ValueForm::strong_sequence})
            .hidden_field<&Holder::hidden>("hidden", {TypeReference::native<Element>(), ValueForm::strong_sequence})
            .field<&Holder::tail>("tail", {TypeReference::native<Element>(), ValueForm::strong_reference})
            .build(),
        NativeRegistration<HolderDerived>{runtime, "sequence.NativeHolder", TypeKind::object}.base<Holder>().build(),
        std::move(mixed),
    }));
}

void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
    // A fresh cycle also releases targets shaded before the interleaved mutation.
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
}

void splice(Runtime& runtime, const ReferenceField& field, std::size_t first, std::size_t erased,
            std::span<const ObjectReference> inserted = {}) {
    const std::array<ReferenceChange, 1> changes{SequenceSplice{field, first, erased, inserted}};
    if (auto applied = runtime.apply_reference_changes(changes); !applied)
        fatal(applied.error().message);
}

void graph_and_batch() {
    Runtime runtime;
    Runtime other;
    const auto types = publish(runtime);
    auto holder = take(runtime.create(types[2]));
    const auto items = take(runtime.reference_field(holder.id(), take(runtime.find_field(types[2], "items"))));
    auto first = take(runtime.create(types[0]));
    auto second = take(runtime.create(types[0]));
    const std::array incoming{first.reference(), second.reference()};
    take(runtime.with_native<Element>(first.id(), [&](Element& a) -> Result<void> {
        return runtime.with_native<Element>(second.id(), [&](Element& b) -> Result<void> {
            const auto a_weak = runtime.reference_field(first.id(), a.weak_link);
            if (!a_weak)
                fatal(a_weak.error().message);
            const auto b_weak = runtime.reference_field(second.id(), b.weak_link);
            if (!b_weak)
                fatal(b_weak.error().message);
            const std::array<ReferenceChange, 3> changes{SequenceSplice{items, 0, 0, incoming},
                                                         ReferenceWrite{*a_weak, holder.reference()},
                                                         ReferenceWrite{*b_weak, holder.reference()}};
            if (auto applied = runtime.apply_reference_changes(changes); !applied)
                fatal(applied.error().message);
            auto alien = other.create(other.object_type());
            if (!alien)
                fatal(alien.error().message);
            const std::array<ReferenceChange, 2> bad{SequenceSplice{items, 0, 1, {}},
                                                     ReferenceWrite{*b_weak, alien->reference()}};
            if (runtime.apply_reference_changes(bad))
                fatal("The late invalid reference did not fail.");
            const auto count = runtime.sequence_size(items);
            if (!count)
                fatal(count.error().message);
            const auto reference = runtime.read_reference(*b_weak);
            if (!reference)
                fatal(reference.error().message);
            if (*count != 2 || reference->id() != holder.id())
                fatal("Late invalid reference partially committed a splice.");
            const auto strong = runtime.reference_field(first.id(), a.strong_link);
            if (!strong)
                fatal(strong.error().message);
            const std::array<ReferenceChange, 1> strong_cycle{ReferenceWrite{*strong, holder.reference()}};
            return runtime.apply_reference_changes(strong_cycle);
        });
    }));
    const auto a_id = first.id();
    const auto b_id = second.id();
    auto tail = take(runtime.create(types[0]));
    take(runtime.write_reference(holder.id(), take(runtime.find_field(types[2], "tail")), tail.reference()));
    first.reset();
    second.reset();
    tail.reset();
    collect(runtime);
    require(runtime.live_objects() == 4 && runtime.type_of(a_id) && runtime.type_of(b_id), "Sequence lost its graph.");
    holder.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Unrooted sequence/reverse-strong cycle leaked.");
}

void borrow_contract() {
    Runtime runtime;
    const auto types = publish(runtime);
    auto holder = take(runtime.create(types[3]));
    ReferenceField hidden;
    take(runtime.with_native<Holder>(holder.id(), [&](Holder& value) -> Result<void> {
        auto field = runtime.reference_field(holder.id(), value.hidden);
        if (!field)
            fatal(field.error().message);
        hidden = std::move(*field);
        return {};
    }));
    const auto failed = runtime.with_native<Holder>(holder.id(), [](Holder&) -> Result<void> {
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "borrow failure"});
    });
    require(!failed && failed.error().code == ErrorCode::capacity_exceeded,
            "The native borrow lost its explicit failure.");
    auto element = take(runtime.create(types[0]));
    take(runtime.with_native<Holder>(holder.id(), [&](Holder&) -> Result<void> {
        const std::array refs{element.reference()};
        splice(runtime, hidden, 0, 0, refs);
        return {};
    }));
    const auto element_id = element.id();
    element.reset();
    collect(runtime);
    require(runtime.type_of(element_id).has_value(), "Hidden inherited sequence was not traced.");
    take(runtime.destroy(holder.id()));
    holder.reset();
    collect(runtime);
    require(runtime.live_objects() == 0, "Hidden sequence storage leaked after explicit destroy.");
}

void paused_sequence() {
    Runtime runtime;
    const auto types = publish(runtime);
    auto holder = take(runtime.create(types[1]));
    const auto items = take(runtime.reference_field(holder.id(), take(runtime.find_field(types[1], "items"))));
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> refs;
    for (unsigned i = 0; i < 3; ++i) {
        roots.push_back(take(runtime.create(types[0])));
        refs.push_back(roots.back().reference());
    }
    splice(runtime, items, 0, 0, refs);
    auto tail = take(runtime.create(types[0]));
    const auto tail_id = tail.id();
    take(runtime.write_reference(holder.id(), take(runtime.find_field(types[1], "tail")), tail.reference()));
    roots.clear();
    tail.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::mark)
        take(runtime.collect_step({1}));
    // Start the owner, enter items, visit two elements; the third is still unvisited.
    take(runtime.collect_step({4}));
    splice(runtime, items, 0, 1);
    refs.clear();
    for (unsigned i = 0; i < 3; ++i) {
        roots.push_back(take(runtime.create(types[0])));
        refs.push_back(roots.back().reference());
    }
    splice(runtime, items, take(runtime.sequence_size(items)), 0, refs);
    roots.clear();
    collect(runtime);
    require(runtime.live_objects() == 7 && runtime.type_of(tail_id),
            "Paused mutation lost its suffix or following field.");
    for (std::size_t i = 0; i < take(runtime.sequence_size(items)); ++i)
        require(runtime.type_of(take(runtime.sequence_read(items, i)).id()).has_value(),
                "Splice left an unmarked live element.");
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::mark)
        take(runtime.collect_step({1}));
    take(runtime.collect_step({5}));
    splice(runtime, items, 0, take(runtime.sequence_size(items)));
    collect(runtime);
    require(runtime.live_objects() == 2 && runtime.type_of(tail_id),
            "Clear cancelled the cursor without requeuing its owner.");
    const std::array tail_ref{take(runtime.hold(tail_id)).reference()};
    splice(runtime, items, 0, 0, tail_ref);
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::mark)
        take(runtime.collect_step({1}));
    take(runtime.collect_step({3}));
    take(runtime.destroy(holder.id()));
    holder.reset();
    collect(runtime);
    require(runtime.live_objects() == 0, "Destroyed sequence owner left a borrowed GC cursor or object.");
}
}

void verify_reference_sequences() {
    graph_and_batch();
    borrow_contract();
    paused_sequence();
    std::puts("[pass] Reference sequences: batch rollback, failed borrow cleanup and paused cursor mutation");
}
