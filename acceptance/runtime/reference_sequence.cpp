#include "inheritance_fixture.hpp"
#include "runtime/execution.hpp"
#include <array>
#include <cstdio>

namespace {
using namespace atelier;
using atelier::acceptance::inheritance::take;
using atelier::acceptance::inheritance::reject;
using atelier::acceptance::inheritance::require;
using atelier::acceptance::inheritance::collect;
using atelier::acceptance::inheritance::Prefix;
struct Element {
    ReferenceSlot weak_link, strong_link;
    float value{};
};
struct ElementDerived : Element {
    float extra{};
};
struct Holder {
    ReferenceSequence items, hidden;
    ReferenceSlot tail;
    float value{};
};
struct HolderDerived : Prefix, Holder {
    float extra{};
};
struct Types {
    TypeId element, derived, holder, holder_derived, mixed, described;
};

Types publish(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(
        NativeRegistration<Element>{runtime, "sequence.Element", TypeKind::object}
            .hidden_field<&Element::weak_link>("weak_link", {runtime.object_type(), ValueForm::weak_reference})
            .hidden_field<&Element::strong_link>("strong_link", {runtime.object_type(), ValueForm::strong_reference})
            .field<&Element::value>("value")
            .build());
    definitions.push_back(NativeRegistration<ElementDerived>{runtime, "sequence.Derived", TypeKind::object}
                              .base<Element>()
                              .field<&ElementDerived::extra>("extra")
                              .build());
    definitions.push_back(
        NativeRegistration<Holder>{runtime, "sequence.Holder", TypeKind::object}
            .field<&Holder::items>("items", {TypeReference::native<Element>(), ValueForm::strong_sequence})
            .hidden_field<&Holder::hidden>("hidden", {TypeReference::native<Element>(), ValueForm::strong_sequence})
            .field<&Holder::tail>("tail", {TypeReference::native<Element>(), ValueForm::strong_reference})
            .field<&Holder::value>("value")
            .build());
    definitions.push_back(NativeRegistration<HolderDerived>{runtime, "sequence.NativeHolder", TypeKind::object}
                              .base<Holder>()
                              .field<&HolderDerived::extra>("extra")
                              .build());
    TypeDefinition mixed{"sequence.MixedHolder", TypeKind::object, {}, {}, {}};
    mixed.base = TypeReference::native<HolderDerived>();
    mixed.fields = {{"last", {TypeReference::native<float>()}}};
    definitions.push_back(std::move(mixed));
    TypeDefinition described{"sequence.DescribedHolder", TypeKind::object, {}, {}, {}};
    described.fields = {{"items", {TypeReference::native<Element>(), ValueForm::strong_sequence}},
                        {"tail", {TypeReference::native<Element>(), ValueForm::strong_reference}}};
    definitions.push_back(std::move(described));
    const auto ids = take(runtime.register_types(std::move(definitions)));
    return {ids[0], ids[1], ids[2], ids[3], ids[4], ids[5]};
}
void splice(Runtime& runtime, const ReferenceField& field, std::size_t first, std::size_t erased,
            std::span<const ObjectReference> inserted = {}) {
    const std::array<ReferenceChange, 1> changes{SequenceSplice{field, first, erased, inserted}};
    take(runtime.apply_reference_changes(changes));
}
void graph_and_batch(TypeId Types::* holder_kind) {
    Runtime runtime, other;
    const auto types = publish(runtime);
    const auto holder_type = types.*holder_kind;
    auto holder = take(runtime.create(holder_type));
    const auto items_id = take(runtime.find_field(holder_type, "items"));
    const auto items = take(runtime.reference_field(holder.id(), items_id));
    const auto tail = take(runtime.find_field(holder_type, "tail"));
    reject(runtime.read_reference(holder.id(), items_id), ErrorCode::type_mismatch);
    auto first = take(runtime.create(types.derived)), second = take(runtime.create(types.derived));
    const std::array incoming{first.reference(), second.reference()};
    take(runtime.with_native<Element>(first.id(), [&](Element& a) -> Result<void> {
        return runtime.with_native<Element>(second.id(), [&](Element& b) -> Result<void> {
            const auto a_weak = take(runtime.reference_field(first.id(), a.weak_link));
            const auto b_weak = take(runtime.reference_field(second.id(), b.weak_link));
            const std::array<ReferenceChange, 3> changes{SequenceSplice{items, 0, 0, incoming},
                                                         ReferenceWrite{a_weak, holder.reference()},
                                                         ReferenceWrite{b_weak, holder.reference()}};
            take(runtime.apply_reference_changes(changes));
            require(take(runtime.read_reference(a_weak)).description().form == ValueForm::weak_reference &&
                        take(runtime.read_reference(b_weak)).id() == holder.id(),
                    "Batch ignored registered reference forms.");
            auto alien = take(other.create(other.object_type()));
            const std::array<ReferenceChange, 2> bad{SequenceSplice{items, 0, 1, {}},
                                                     ReferenceWrite{b_weak, alien.reference()}};
            reject(runtime.apply_reference_changes(bad), ErrorCode::invalid_type);
            require(take(runtime.sequence_size(items)) == 2 && take(runtime.read_reference(b_weak)).id() == holder.id(),
                    "Late invalid reference partially committed a splice.");
            const auto strong = take(runtime.reference_field(first.id(), a.strong_link));
            const std::array<ReferenceChange, 2> wrong_strength{
                SequenceSplice{items, 0, 1, {}}, ReferenceWrite{strong, take(runtime.observe(holder.reference()))}};
            reject(runtime.apply_reference_changes(wrong_strength), ErrorCode::type_mismatch);
            const std::array<ReferenceChange, 1> strong_cycle{ReferenceWrite{strong, holder.reference()}};
            take(runtime.apply_reference_changes(strong_cycle));
            require(take(runtime.read_reference(strong)).description().form == ValueForm::strong_reference,
                    "Batch hardcoded the reverse edge to weak.");
            return {};
        });
    }));
    require(take(runtime.sequence_read(items, 0)).id() == first.id() &&
                take(runtime.sequence_read(items, 0)).description().type == types.element,
            "Sequence lost element identity or static view.");
    reject(runtime.sequence_read(items, 2), ErrorCode::argument_mismatch);
    const std::array<ReferenceChange, 2> duplicate{SequenceSplice{items, 0, 0, {}}, SequenceSplice{items, 0, 0, {}}};
    reject(runtime.apply_reference_changes(duplicate), ErrorCode::argument_mismatch);
    const std::array<ReferenceChange, 1> range{SequenceSplice{items, 3, 0, {}}};
    reject(runtime.apply_reference_changes(range), ErrorCode::argument_mismatch);
    const std::array wrong_type{holder.reference()};
    const std::array<ReferenceChange, 1> bad_type{SequenceSplice{items, 0, 0, wrong_type}};
    reject(runtime.apply_reference_changes(bad_type), ErrorCode::type_mismatch);
    require(take(runtime.sequence_size(items)) == 2, "Rejected sequence operation changed its contents.");
    // Reordering consumes copied views, preserving original object identities.
    const std::array reordered{take(runtime.sequence_read(items, 1)), take(runtime.sequence_read(items, 0))};
    splice(runtime, items, 0, 2, reordered);
    require(take(runtime.sequence_read(items, 0)).id() == second.id(), "Whole-range splice did not restore order.");
    std::size_t enumerated{};
    struct Visit {
        Runtime& runtime;
        const ReferenceField& items;
        std::size_t& count;
        ObjectId holder;
    } visit{runtime, items, enumerated, holder.id()};
    take(runtime.visit_sequence(
        items, {&visit, [](void* data, const ObjectReference&) -> Result<void> {
                    auto& state = *static_cast<Visit*>(data);
                    ++state.count;
                    const std::array<ReferenceChange, 1> change{SequenceSplice{state.items, 0, 0, {}}};
                    reject(state.runtime.apply_reference_changes(change), ErrorCode::busy);
                    reject(state.runtime.with_native<Holder>(state.holder, [](Holder&) -> Result<void> { return {}; }),
                           ErrorCode::busy);
                    return {};
                }}));
    require(enumerated == 2, "Sequence enumeration lost elements.");
    const auto id = holder.id(), a_id = first.id(), b_id = second.id();
    auto tail_object = take(runtime.create(types.derived));
    take(runtime.write_reference(id, tail, tail_object.reference()));
    first.reset();
    second.reset();
    tail_object.reset();
    collect(runtime);
    require(runtime.live_objects() == 4 && runtime.type_of(a_id) && runtime.type_of(b_id),
            "Sequence did not retain its derived graph.");
    holder.reset();
    collect(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Unrooted sequence/reverse-strong cycle leaked.");
    reject(runtime.sequence_size(items), ErrorCode::invalid_object);
}

void borrow_contract() {
    Runtime runtime;
    const auto types = publish(runtime);
    auto holder = take(runtime.create(types.mixed));
    ReferenceField hidden;
    take(runtime.with_native<Holder>(holder.id(), [&](Holder& value) -> Result<void> {
        value.value = 12;
        hidden = take(runtime.reference_field(holder.id(), value.hidden));
        require(take(runtime.sequence_size(hidden)) == 0, "Native hidden sequence resolution failed.");
        reject(runtime.create(types.derived), ErrorCode::busy);
        reject(runtime.destroy(holder.id()), ErrorCode::busy);
        reject(runtime.begin_collection(), ErrorCode::busy);
        reject(runtime.collect_step({1}), ErrorCode::busy);
        reject(ExecutionScope::enter(runtime), ErrorCode::busy);
        ReferenceSlot outside;
        reject(runtime.reference_field(holder.id(), outside), ErrorCode::invalid_member);
        return runtime.with_native<HolderDerived>(holder.id(), [](HolderDerived& derived) -> Result<void> {
            require(derived.value == 12, "Nested borrowed bases do not refer to the same object.");
            return {};
        });
    }));
    reject(runtime.sequence_size(hidden), ErrorCode::busy);
    reject(runtime.with_native<Element>(holder.id(), [](Element&) -> Result<void> { return {}; }),
           ErrorCode::type_mismatch);
    reject(runtime.with_native<Holder>(holder.id(),
                                       [](Holder&) -> Result<void> { throw std::runtime_error("borrow failure"); }),
           ErrorCode::invocation_failed);
    take(runtime.check_code_publication());
    auto element = take(runtime.create(types.derived));
    take(runtime.with_native<Holder>(holder.id(), [&](Holder&) -> Result<void> {
        const std::array refs{element.reference()};
        splice(runtime, hidden, 0, 0, refs);
        return {};
    }));
    const auto element_id = element.id();
    element.reset();
    collect(runtime);
    require(runtime.type_of(element_id).has_value(), "Hidden inherited sequence was not traced.");
    const auto id = holder.id();
    take(runtime.destroy(id));
    reject(runtime.with_native<Holder>(id, [](Holder&) -> Result<void> { return {}; }), ErrorCode::invalid_object);
    holder.reset();
    collect(runtime);
    require(runtime.live_objects() == 0, "Hidden sequence storage leaked after explicit destroy.");
}

void paused_sequence() {
    Runtime runtime;
    const auto types = publish(runtime);
    auto holder = take(runtime.create(types.holder));
    const auto items = take(runtime.reference_field(holder.id(), take(runtime.find_field(types.holder, "items"))));
    std::vector<ObjectRoot> roots;
    std::vector<ObjectReference> refs;
    for (unsigned i = 0; i < 80; ++i) {
        roots.push_back(take(runtime.create(types.derived)));
        refs.push_back(roots.back().reference());
    }
    splice(runtime, items, 0, 0, refs);
    auto tail = take(runtime.create(types.derived));
    const auto tail_id = tail.id();
    take(runtime.write_reference(holder.id(), take(runtime.find_field(types.holder, "tail")), tail.reference()));
    roots.clear();
    tail.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::mark)
        take(runtime.collect_step({1}));
    take(runtime.collect_step({4})); // holder cursor is inside the sequence
    splice(runtime, items, 0, 1); // shifts yet-unvisited elements to earlier indices
    roots.clear();
    refs.clear();
    for (unsigned i = 0; i < 90; ++i) {
        roots.push_back(take(runtime.create(types.derived)));
        refs.push_back(roots.back().reference());
    }
    splice(runtime, items, take(runtime.sequence_size(items)), 0, refs); // forces growth
    roots.clear();
    collect(runtime);
    require(runtime.live_objects() == 171 && runtime.type_of(tail_id),
            "Paused sequence mutation lost its suffix or following field.");
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
            "Clear cancelled the cursor without requeuing its marked owner.");
    // Physical destruction invalidates any suspended sequence header as well.
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
    {
        Runtime runtime;
        TypeDefinition invalid{"sequence.Record", TypeKind::record, {}, {}, {}};
        invalid.fields = {{"items", {runtime.object_type(), ValueForm::strong_sequence}}};
        reject(runtime.register_type(invalid), ErrorCode::invalid_definition);
    }
    graph_and_batch(&Types::holder_derived);
    graph_and_batch(&Types::mixed);
    graph_and_batch(&Types::described);
    borrow_contract();
    paused_sequence();
    std::puts(
        "[pass] Reference sequences: typed graph, protected native access, atomic splice/slot changes, GC rescan and bounded cursor lifetime");
}
