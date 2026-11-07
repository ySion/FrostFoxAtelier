#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "runtime/foundation.hpp"
#include "memory/acceptance.hpp"

#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <utility>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void verify_registration_text() {
    Runtime runtime;
    const auto scalar = runtime.native_type<std::uint32_t>();
    if (!scalar)
        fatal("Registration text fixture requires core.u32.");
    TypeDefinition definition{"文本.记录", TypeKind::record, {{"数量", {*scalar}}}, {}, {}};
    definition.fields[0].label = "数量（个）";
    definition.fields[0].category = "属性";
    const auto registered = runtime.register_type(definition);
    if (!registered || !runtime.find_field(*registered, "数量"))
        fatal("Valid non-ASCII registration text was rejected or changed.");
    definition.name = "文本.拒绝";
    definition.fields[0].name = "\xc0\xaf";
    const auto rejected = runtime.register_type(std::move(definition));
    if (rejected || rejected.error().code != ErrorCode::invalid_definition || runtime.find_type("文本.拒绝"))
        fatal("Invalid UTF-8 field name was accepted or published a partial type.");
}

struct alignas(32) ValueResource {
    static inline int live{};

    static inline int next_id{};

    static inline int last_destroyed{};

    static inline int previous_destroyed{};

    static inline int remaining_constructions{-1};

    static inline bool fail_assignment{};

    std::unique_ptr<std::uint32_t> payload;

    int id;

    static int begin_lifetime() noexcept {
        ++live;
        return ++next_id;
    }

    ValueResource() : payload(std::make_unique<std::uint32_t>(7)), id(begin_lifetime()) {}

    ValueResource(const ValueResource& source)
        : payload(std::make_unique<std::uint32_t>(*source.payload)), id(begin_lifetime()) {}

    explicit ValueResource(std::unique_ptr<std::uint32_t> value) noexcept
        : payload(std::move(value)), id(begin_lifetime()) {}

    ValueResource& operator=(const ValueResource& source) {
        *payload = *source.payload;
        return *this;
    }

    static Result<void> construct(void* destination, const void* source) noexcept {
        // A failed native callback owns this prepared resource; no destination is live yet.
        auto payload =
            std::make_unique<std::uint32_t>(source ? *static_cast<const ValueResource*>(source)->payload : 7);
        if (remaining_constructions == 0)
            return std::unexpected(Error{ErrorCode::construction_failed, "Deliberate value construction failure."});
        if (remaining_constructions > 0)
            --remaining_constructions;
        std::construct_at(static_cast<ValueResource*>(destination), std::move(payload));
        return {};
    }

    static Result<void> assign(void* destination, const void* source) noexcept {
        *static_cast<ValueResource*>(destination) = *static_cast<const ValueResource*>(source);
        if (fail_assignment)
            return std::unexpected(Error{ErrorCode::assignment_failed, "Deliberate value assignment failure."});
        return {};
    }

    ~ValueResource() noexcept {
        --live;
        previous_destroyed = last_destroyed;
        last_destroyed = id;
    }
};

TypeDefinition value_resource_definition(Runtime& runtime, std::string name) {
    auto definition = NativeRegistration<ValueResource>{runtime, std::move(name), TypeKind::record}.build();
    definition.native.construct = [](void* destination) noexcept {
        return ValueResource::construct(destination, nullptr);
    };
    definition.native.copy_construct = ValueResource::construct;
    definition.native.copy_assign = ValueResource::assign;
    return definition;
}

void verify_value_operations() {
    Runtime runtime;
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto resource = take(runtime.register_type(value_resource_definition(runtime, "values.Resource")));
    const auto pair = take(runtime.register_type(
        {"values.Pair", TypeKind::record, {{"first", {resource}}, {"second", {resource}}}, {}, {}}));
    const auto record = take(runtime.register_type({"values.Record",
                                                    TypeKind::record,
                                                    {{"revision", {u32}}, {"resource", {resource}}, {"pair", {pair}}},
                                                    {},
                                                    {}}));
    alignas(32) std::array<std::byte, 256> source{};
    alignas(32) std::array<std::byte, 256> destination{};
    alignas(32) std::array<std::byte, 256> scratch{};
    const auto read_revision = [](const auto& bytes) {
        std::uint32_t result;
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    };
    const auto write_revision = [](auto& bytes, std::uint32_t value) {
        std::memcpy(bytes.data(), &value, sizeof(value));
    };
    take(runtime.initialize_value(resource, source));
    auto* native_source = std::launder(reinterpret_cast<ValueResource*>(source.data()));
    *native_source->payload = 42;
    take(runtime.copy_construct_value(resource, destination, source));
    auto* native_destination = std::launder(reinterpret_cast<ValueResource*>(destination.data()));
    require(ValueResource::live == 2 && *native_destination->payload == 42 &&
                native_source->payload != native_destination->payload,
            "Native value copy did not use its C++ semantics.");
    ValueResource::fail_assignment = true;
    *native_source->payload = 123;
    require(!runtime.assign_value(resource, destination, source), "Native assignment did not fail.");
    require(*native_destination->payload == 123, "Assignment unexpectedly rolled back its partial update.");
    ValueResource::fail_assignment = false;
    take(runtime.destroy_value(resource, destination));
    take(runtime.destroy_value(resource, source));
    require(ValueResource::live == 0, "Native value destruction leaked a resource.");

    take(runtime.initialize_value(record, source));
    write_revision(source, 17);
    take(runtime.copy_construct_value(record, destination, source));
    ValueResource::remaining_constructions = 2;
    require(!runtime.copy_construct_value(record, scratch, source), "Nested copy did not fail.");
    require(ValueResource::live == 6 && ValueResource::previous_destroyed == ValueResource::next_id &&
                ValueResource::last_destroyed == ValueResource::next_id - 1,
            "Nested copy did not unwind in reverse order.");
    ValueResource::remaining_constructions = 2;
    require(!runtime.initialize_value(record, scratch), "Nested initialization did not fail.");
    require(ValueResource::live == 6 && ValueResource::previous_destroyed == ValueResource::next_id &&
                ValueResource::last_destroyed == ValueResource::next_id - 1,
            "Nested initialization did not unwind in reverse order.");
    ValueResource::remaining_constructions = -1;
    take(runtime.initialize_value(record, scratch));
    take(runtime.destroy_value(record, scratch));
    ValueResource::fail_assignment = true;
    write_revision(source, 53);
    require(!runtime.assign_value(record, destination, source), "Nested assignment did not fail.");
    require(read_revision(destination) == 53 && ValueResource::live == 6,
            "Failed assignment rolled back or broke value lifetime.");
    take(runtime.destroy_value(record, destination));
    take(runtime.destroy_value(record, source));
    ValueResource::fail_assignment = false;
    require(ValueResource::live == 0, "Failed value operations leaked resources.");

    // A completed non-SSO text field must unwind when a later child fails, and the
    // next creation must still use the declared text rather than an empty fallback.
    static int text_destructions = 0;
    auto text_definition = string_definition(runtime);
    text_definition.native.destroy = [](void* storage) noexcept {
        ++text_destructions;
        std::destroy_at(static_cast<String*>(storage));
    };
    const auto text_type = take(runtime.register_type(std::move(text_definition)));
    FieldDefinition text_field{"text", {text_type}};
    text_field.default_text = "completed_non_SSO_text_0123456789";
    const auto owner =
        take(runtime.register_type({"values.TextOwner", TypeKind::object, {text_field, {"pair", {pair}}}, {}, {}}));
    ValueResource::remaining_constructions = 1;
    const auto failed = runtime.create(owner);
    require(!failed && failed.error().code == ErrorCode::construction_failed && text_destructions == 1 &&
                ValueResource::live == 0,
            "Later construction failure leaked or double-destroyed a completed String field.");
    ValueResource::remaining_constructions = -1;
    auto recovered = take(runtime.create(owner));
    String text;
    take(runtime.read(recovered.id(), take(runtime.find_field(owner, "text")), ValueOutput{text_type, &text}));
    require(text.view() == StringView{"completed_non_SSO_text_0123456789"} && ValueResource::live == 2,
            "Creation after a failed text-bearing object lost its default or child lifetimes.");
    const auto before_destroy = text_destructions;
    take(runtime.destroy(recovered.id()));
    require(text_destructions == before_destroy + 1 && ValueResource::live == 0,
            "Recovered text-bearing object did not release its completed fields exactly once.");
    std::cout << "[pass] Values unwind failed construction and retain destructible partial assignments\n";
}

struct SignatureNative {
    std::uint32_t value{};

    Result<std::uint32_t> fail(std::uint32_t amount) noexcept {
        value += amount;
        return std::unexpected(Error{ErrorCode::capacity_exceeded, "Native scalar result failure."});
    }
};

void verify_method_results() {
    Runtime runtime;
    const auto native = take(NativeRegistration<SignatureNative>{runtime, "signature.Native", TypeKind::object}
                                 .field<&SignatureNative::value>("value")
                                 .method<&SignatureNative::fail>("fail")
                                 .publish());
    auto object = take(runtime.create(native));
    const auto value = take(runtime.find_field(native, "value"));
    take(runtime.write(object.id(), value, std::uint32_t{6}));
    const std::array<Scalar, 1> arguments{std::uint32_t{3}};
    const Scalar sentinel{-731.25f};
    Scalar output = sentinel;
    const auto failed = runtime.call(object.id(), take(runtime.find_method(native, "fail")), arguments, &output);
    require(!failed && failed.error().code == ErrorCode::capacity_exceeded,
            "Native scalar call lost its explicit failure.");
    require(output == sentinel && std::get<std::uint32_t>(take(runtime.read(object.id(), value))) == 9,
            "Native failure changed output or rolled back prior side effects.");
    std::cout << "[pass] Native result failure leaves caller output untouched\n";
}

struct RootLifetimeProbe {
    static inline int live{};

    RootLifetimeProbe() { ++live; }

    ~RootLifetimeProbe() noexcept { --live; }
};

void verify_root_handles() {
    ObjectRoot survivor;
    ObjectReference surviving_identity;
    {
        Runtime runtime;
        const auto type =
            take(NativeRegistration<RootLifetimeProbe>{runtime, "roots.Object", TypeKind::object}.publish());
        auto owner = take(runtime.create(type));
        const auto identity = owner.id();
        const auto strong = owner.reference();
        {
            ObjectRoot copied{owner};
            require(runtime.root_count() == 2 && copied.id() == identity, "Copying a root cloned or lost the object.");
            ObjectRoot moved{std::move(copied)};
            require(!copied.registered() && moved.registered() && runtime.root_count() == 2,
                    "Moving a root changed registration count.");
        }
        require(runtime.root_count() == 1 && runtime.live_objects() == 1, "Root destruction destroyed the target.");
        const auto weak = take(runtime.observe(strong));
        take(runtime.destroy(identity));
        require(owner.reference() == strong && runtime.root_count() == 1 && RootLifetimeProbe::live == 0,
                "Explicit destruction silently cleared roots or retained the target.");
        require(!take(runtime.try_acquire(weak)), "Destroyed target was acquired through an observer.");
        survivor = take(runtime.create(type));
        require(survivor.id() != identity && !take(runtime.try_acquire(weak)),
                "Slot reuse resurrected the previous identity.");
        owner.reset();
        surviving_identity = survivor.reference();
        require(RootLifetimeProbe::live == 1, "Scoped rooted object was not created.");
    }
    require(RootLifetimeProbe::live == 0 && !survivor.registered() && survivor.reference() == surviving_identity,
            "A root prolonged Runtime ownership or lost its stale identity at shutdown.");
    ObjectRoot copied_survivor{survivor};
    survivor.reset();
    require(!copied_survivor.registered() && copied_survivor.reference() == surviving_identity,
            "A detached root could not safely preserve its reference value.");
    copied_survivor.reset();
    std::cout << "[pass] Root copy/move, destruction, slot reuse and Runtime shutdown\n";
}

struct CollectionLinks {
    ReferenceSlot first;

    ReferenceSlot second;

    ReferenceSlot observer;

    static inline const void* last_traced{};
};

struct CollectionNode {
    CollectionLinks links;

    static inline CollectionNode* last_constructed{};

    static inline int live{};

    static inline int destroyed{};

    static inline Runtime* checking_runtime{};

    static inline std::array<ObjectId, 2> condemned;

    static inline bool death_visible{true};

    CollectionNode() {
        ++live;
        last_constructed = this;
    }

    ~CollectionNode() noexcept {
        --live;
        ++destroyed;
        if (checking_runtime)
            for (const auto object : condemned)
                death_visible &= !checking_runtime->type_of(object);
    }
};

std::vector<TypeId> register_collection_types(Runtime& runtime) {
    auto links =
        NativeRegistration<CollectionLinks>{runtime, "gc.Links", TypeKind::record}
            .hidden_field<&CollectionLinks::first>("first", {TypeReference{"gc.A"}, ValueForm::strong_reference})
            .field<&CollectionLinks::second>("second", {TypeReference{"gc.A"}, ValueForm::strong_reference})
            .field<&CollectionLinks::observer>("observer", {TypeReference{"gc.A"}, ValueForm::weak_reference})
            .build();
    // Pause on actual field visitation rather than assuming a fixed GC step count.
    links.fields[0].native_address = [](void* storage) noexcept -> void* {
        CollectionLinks::last_traced = storage;
        return &static_cast<CollectionLinks*>(storage)->first;
    };
    return take(runtime.register_types({
        {"gc.A",
         TypeKind::object,
         {{"next", {TypeReference::native<CollectionNode>(), ValueForm::strong_reference}}},
         {},
         {}},
        NativeRegistration<CollectionNode>{runtime, "gc.B", TypeKind::object}
            .field<&CollectionNode::links>("links")
            .build(),
        std::move(links),
    }));
}

void advance_collection(Runtime& runtime, CollectionPhase phase) {
    std::size_t steps = 0;
    while (runtime.collection_phase() != phase) {
        require(runtime.collection_phase() != CollectionPhase::idle && ++steps < 10000,
                "Collection did not reach the requested phase.");
        take(runtime.collect_step({1}));
    }
}

std::size_t finish_collection(Runtime& runtime) {
    std::size_t steps = 0;
    std::size_t reclaimed = 0;
    while (runtime.collection_phase() != CollectionPhase::idle) {
        require(++steps < 10000, "Collection did not finish within the acceptance work limit.");
        reclaimed += take(runtime.collect_step({1})).reclaimed;
    }
    return reclaimed;
}

void advance_to_reference(Runtime& runtime, const void* storage) {
    std::size_t steps = 0;
    while (CollectionLinks::last_traced != storage) {
        require((runtime.collection_phase() == CollectionPhase::roots ||
                 runtime.collection_phase() == CollectionPhase::mark) &&
                    ++steps < 10000,
                "Collection did not visit the protected reference storage.");
        take(runtime.collect_step({1}));
    }
}

void verify_collection_cycle() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    auto a = take(runtime.create(ids[0]));
    auto b = take(runtime.create(ids[1]));
    const auto a_id = a.id();
    const auto b_id = b.id();
    const auto weak = take(runtime.observe(a.reference()));
    take(runtime.write_reference(a_id, take(runtime.find_field(ids[0], "next")), b.reference()));
    take(runtime.write_reference({ids[0], ValueForm::strong_reference}, CollectionNode::last_constructed->links.first,
                                 a.reference()));
    b.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 2 && CollectionNode::live == 1,
            "A rooted mixed native/described cycle was collected.");
    a.reset();
    CollectionNode::checking_runtime = &runtime;
    CollectionNode::condemned = {a_id, b_id};
    const auto destroyed_before = CollectionNode::destroyed;
    take(runtime.begin_collection());
    advance_collection(runtime, CollectionPhase::sweep);
    require(runtime.live_objects() == 0 && runtime.pending_reclamation() == 2 && CollectionNode::live == 1,
            "Logical death did not precede physical reclamation of the entire cycle.");
    require(!take(runtime.try_acquire(weak)), "A committed-dead observer resurrected an unswept object.");
    auto fresh = take(runtime.create(ids[1]));
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 1 && CollectionNode::live == 1 &&
                CollectionNode::destroyed == destroyed_before + 1,
            "Sweep lost a new object or reclaimed a native object more than once.");
    CollectionNode::checking_runtime = nullptr;
    require(CollectionNode::death_visible, "Native destruction observed a live condemned peer.");
    fresh.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 1 && runtime.live_objects() == 0 && CollectionNode::live == 0,
            "Subsequent collection leaked sweep-time allocations.");
    std::cout << "[pass] Mixed cycle death precedes reclamation; sweep-time allocations survive\n";
}

void verify_late_registration() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    auto target = take(runtime.create(ids[0]));
    take(runtime.begin_collection());
    finish_collection(runtime);
    const auto late = take(runtime.register_type(
        {"gc.Late", TypeKind::object, {{"peer", {TypeReference{"gc.A"}, ValueForm::strong_reference}}}, {}, {}}));
    auto holder = take(runtime.create(late));
    take(runtime.write_reference(holder.id(), take(runtime.find_field(late, "peer")), target.reference()));
    target.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 2, "A late type lost its reference target.");
    holder.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 0,
            "Objects of a late type were not collected.");
}

void verify_collection_mutations() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    const auto next = take(runtime.find_field(ids[0], "next"));
    const std::array second_path{take(runtime.find_field(ids[1], "links")), take(runtime.find_field(ids[2], "second"))};
    auto holder = take(runtime.create(ids[1]));
    auto* native = CollectionNode::last_constructed;
    std::array<ObjectReference, 5> targets;
    std::array<ObjectReference, 5> observers;
    for (std::size_t i = 0; i != targets.size(); ++i) {
        auto root = take(runtime.create(ids[0]));
        targets[i] = root.reference();
        observers[i] = take(runtime.observe(targets[i]));
    }
    auto old_native = take(runtime.create(ids[1]));
    const auto old_native_reference = old_native.reference();
    old_native.reset();
    CollectionLinks::last_traced = nullptr;
    take(runtime.begin_collection());
    advance_to_reference(runtime, &native->links);
    take(runtime.write_reference({ids[0], ValueForm::strong_reference}, native->links.first, targets[0]));
    take(runtime.write_reference(holder.id(), second_path, targets[1]));
    take(runtime.write_reference({ids[0], ValueForm::weak_reference}, native->links.observer, observers[4]));
    auto late_root = take(runtime.hold(targets[2]));
    ObjectRoot copied_root{late_root};
    late_root.reset();
    auto acquired = take(runtime.try_acquire(observers[3]));
    require(acquired && acquired->registered(), "Mark-time weak acquisition did not publish a root.");
    auto newborn = take(runtime.create(ids[0]));
    const auto newborn_id = newborn.id();
    take(runtime.write_reference(newborn_id, next, old_native_reference));
    newborn.reset();
    require(finish_collection(runtime) == 1 && runtime.live_objects() == 7,
            "Cross-step mutation lost a new strong edge/root/object or treated a weak edge as strong.");
    for (std::size_t i = 0; i != 4; ++i)
        take(runtime.type_of(targets[i].id()));
    take(runtime.type_of(newborn_id));
    take(runtime.type_of(old_native_reference.id()));
    require(!take(runtime.try_acquire(observers[4])), "A weak-only target survived collection.");
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 5,
            "Unrooted mark-time allocations were not reclaimed by the next cycle.");
    copied_root.reset();
    acquired.reset();
    holder.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 5 && runtime.live_objects() == 0 && CollectionNode::live == 0,
            "Late roots or barrier bookkeeping leaked across collection cycles.");
}

struct CollectionValue {
    CollectionLinks links;

    ValueResource resource;
};

void verify_collection_value_mutations() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    auto value_definition = NativeRegistration<CollectionValue>{runtime, "gc.Value", TypeKind::record}
                                .field<&CollectionValue::links>("links")
                                .field<&CollectionValue::resource>("resource")
                                .build();
    value_definition.native.copy_assign = [](void* destination, const void* source) noexcept -> Result<void> {
        auto& target = *static_cast<CollectionValue*>(destination);
        const auto& origin = *static_cast<const CollectionValue*>(source);
        target.links = origin.links;
        return ValueResource::assign(&target.resource, &origin.resource);
    };
    const auto value_types = take(runtime.register_types({
        std::move(value_definition),
        value_resource_definition(runtime, "gc.Resource"),
    }));
    CollectionValue source;
    CollectionValue destination;
    CollectionLinks late_value;
    CollectionLinks copy_source;
    const auto source_bytes = std::as_bytes(std::span{&source, 1});
    const auto destination_bytes = std::as_writable_bytes(std::span{&destination, 1});
    alignas(CollectionLinks) std::array<std::byte, sizeof(CollectionLinks)> copied_storage;
    std::array<ObjectReference, 4> targets;
    for (auto& target : targets) {
        auto root = take(runtime.create(ids[0]));
        target = root.reference();
    }
    const ValueDescription strong{ids[0], ValueForm::strong_reference};
    take(runtime.write_reference(strong, source.links.first, targets[0]));
    take(runtime.write_reference(strong, late_value.first, targets[1]));
    take(runtime.write_reference(strong, copy_source.first, targets[2]));
    auto destination_root = take(runtime.root_value(value_types[0], destination_bytes));
    CollectionLinks::last_traced = nullptr;
    take(runtime.begin_collection());
    advance_to_reference(runtime, &destination.links);
    ValueResource::fail_assignment = true;
    const auto assignment = runtime.assign_value(value_types[0], destination_bytes, source_bytes);
    ValueResource::fail_assignment = false;
    require(!assignment, "The deliberate partial assignment did not fail.");
    require(take(runtime.read_reference(strong, destination.links.first)) == targets[0],
            "Failed native assignment did not preserve its documented partial update.");
    const std::array second_path{take(runtime.find_field(value_types[0], "links")),
                                 take(runtime.find_field(ids[2], "second"))};
    take(runtime.write_reference(value_types[0], destination_bytes, second_path, targets[3]));
    auto late_root = take(runtime.root_value(ids[2], std::as_bytes(std::span{&late_value, 1})));
    take(runtime.copy_construct_value(ids[2], copied_storage, std::as_bytes(std::span{&copy_source, 1})));
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 4,
            "Partial assignment, external field write, late value root or copy construction lost a target.");
    for (const auto& target : targets)
        take(runtime.type_of(target.id()));
    destination_root.reset();
    late_root.reset();
    take(runtime.destroy_value(ids[2], copied_storage));
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 4 && runtime.live_objects() == 0,
            "Unregistered external values or an earlier write barrier became permanent roots.");
}

void verify_collection_cursor_lifetimes() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    const ValueDescription strong{ids[0], ValueForm::strong_reference};
    auto object = take(runtime.create(ids[1]));
    auto* native = CollectionNode::last_constructed;
    CollectionLinks::last_traced = nullptr;
    take(runtime.begin_collection());
    advance_to_reference(runtime, &native->links);
    const auto previous_id = object.id();
    take(runtime.destroy(previous_id));
    auto replacement = take(runtime.create(ids[1]));
    require(replacement.id() != previous_id, "Mark-time destruction reused an old identity.");
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 1 && CollectionNode::live == 1,
            "An object trace cursor survived destruction or lost the replacement slot.");
    object.reset();
    replacement.reset();

    CollectionLinks partial_value;
    auto target = take(runtime.create(ids[0]));
    take(runtime.write_reference(strong, partial_value.second, target.reference()));
    target.reset();
    auto partial_root = take(runtime.root_value(ids[2], std::as_bytes(std::span{&partial_value, 1})));
    CollectionLinks::last_traced = nullptr;
    take(runtime.begin_collection());
    advance_to_reference(runtime, &partial_value);
    partial_root.reset();
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 0,
            "A canceled value-root cursor continued tracing unregistered storage.");

    auto queued_value = std::make_unique<CollectionLinks>();
    auto abandoned = take(runtime.create(ids[0]));
    auto retained = take(runtime.create(ids[0]));
    const auto abandoned_id = abandoned.id();
    const auto retained_id = retained.id();
    CollectionLinks replacement_value;
    take(runtime.write_reference(strong, queued_value->first, abandoned.reference()));
    take(runtime.write_reference(strong, replacement_value.first, retained.reference()));
    abandoned.reset();
    retained.reset();
    auto queued_root = take(runtime.root_value(ids[2], std::as_bytes(std::span{queued_value.get(), 1})));
    take(runtime.begin_collection());
    advance_collection(runtime, CollectionPhase::mark);
    queued_root.reset();
    queued_value.reset();
    auto reused_root = take(runtime.root_value(ids[2], std::as_bytes(std::span{&replacement_value, 1})));
    require(finish_collection(runtime) == 1 && runtime.live_objects() == 1 && !runtime.type_of(abandoned_id) &&
                runtime.type_of(retained_id),
            "A removed queued root retained freed storage or a reused root entry lost its value.");
    reused_root.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 1 && CollectionNode::live == 0, "Root-queue reuse leaked an object.");

    ObjectRoot survivor;
    ObjectReference identity;
    {
        Runtime scoped;
        const auto scoped_ids = register_collection_types(scoped);
        survivor = take(scoped.create(scoped_ids[1]));
        identity = survivor.reference();
        auto* storage = &CollectionNode::last_constructed->links;
        auto doomed = take(scoped.create(scoped_ids[1]));
        doomed.reset();
        CollectionLinks::last_traced = nullptr;
        take(scoped.begin_collection());
        advance_to_reference(scoped, storage);
    }
    require(CollectionNode::live == 0 && !survivor.registered() && survivor.reference() == identity,
            "Shutdown during collection leaked objects or left a root attached to the dead runtime.");
    survivor.reset();
    std::cout << "[pass] Paused trace cursors tolerate destruction, root removal/reuse and shutdown\n";
}

struct CancelLeaf {
    ReferenceSlot target;
};

struct CancelOuter {
    CancelLeaf inner;
};

struct CancelControl {
    static inline ValueRoot* root{};

    static inline bool canceled{};

    static inline std::size_t late_accesses{};
};

void verify_locator_boundaries() {
    Runtime runtime;
    auto leaf = NativeRegistration<CancelLeaf>{runtime, "cancel.Leaf", TypeKind::record}
                    .field<&CancelLeaf::target>("target", {TypeReference{"cancel.Target"}, ValueForm::strong_reference})
                    .build();
    leaf.fields[0].native_address = [](void* storage) noexcept -> void* {
        if (CancelControl::canceled)
            ++CancelControl::late_accesses;
        return &static_cast<CancelLeaf*>(storage)->target;
    };
    auto outer = NativeRegistration<CancelOuter>{runtime, "cancel.Outer", TypeKind::record}
                     .field<&CancelOuter::inner>("inner")
                     .build();
    outer.fields[0].native_address = [](void* storage) noexcept -> void* {
        CancelControl::root->reset();
        CancelControl::canceled = true;
        return &static_cast<CancelOuter*>(storage)->inner;
    };
    const auto ids = take(
        runtime.register_types({{"cancel.Target", TypeKind::object, {}, {}, {}}, std::move(leaf), std::move(outer)}));
    CancelOuter value;
    auto target = take(runtime.create(ids[0]));
    take(runtime.write_reference({ids[0], ValueForm::strong_reference}, value.inner.target, target.reference()));
    target.reset();
    auto root = take(runtime.root_value(ids[2], std::as_bytes(std::span{&value, 1})));
    CancelControl::root = &root;
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(CancelControl::canceled && !root.registered(), "Locator did not revoke its current root.");
    require(CancelControl::late_accesses == 0 && runtime.live_objects() == 0,
            "Cancelled cursor resumed or marked a reference after revoking its borrow.");
    CancelControl::root = nullptr;
}

struct ReentrantValue {
    static inline void* context{};

    static inline void (*exercise)(void*){};

    static inline bool inside{};

    static inline bool failed{};

    static inline std::size_t calls{};

    static void run() noexcept {
        if (!exercise || inside)
            return;
        inside = true;
        ++calls;
        try {
            exercise(context);
        } catch (...) {
            failed = true;
        }
        inside = false;
    }

    ReentrantValue() { run(); }

    ReentrantValue(const ReentrantValue&) { run(); }

    ReentrantValue& operator=(const ReentrantValue&) {
        run();
        return *this;
    }

    ~ReentrantValue() noexcept { run(); }
};

void verify_deep_value_walks() {
    Runtime runtime;
    const auto resource = take(runtime.register_type(value_resource_definition(runtime, "deep.Resource")));
    const auto reentrant =
        take(NativeRegistration<ReentrantValue>{runtime, "deep.Reentrant", TypeKind::record}.publish());
    const auto types = take(runtime.register_types({
        {"deep.R0", TypeKind::record, {{"value", {TypeReference{"deep.Leaf"}}}}, {}, {}},
        {"deep.Leaf",
         TypeKind::record,
         {{"first", {resource}},
          {"strong", {TypeReference{"deep.Target"}, ValueForm::strong_reference}},
          {"weak", {TypeReference{"deep.Target"}, ValueForm::weak_reference}},
          {"last", {resource}},
          {"reentrant", {reentrant}}},
         {},
         {}},
        {"deep.Target", TypeKind::object, {}, {}, {}},
    }));
    const auto deep = types[0];
    const auto leaf = types[1];
    alignas(32) std::array<std::byte, 512> source{};
    alignas(32) std::array<std::byte, 512> destination{};
    alignas(32) std::array<std::byte, 512> nested_source{};
    alignas(32) std::array<std::byte, 512> nested_copy{};
    struct NestedValues {
        Runtime* runtime;

        TypeId type;

        std::span<std::byte> source;

        std::span<std::byte> copy;
    };
    NestedValues nested{&runtime, deep, nested_source, nested_copy};
    ReentrantValue::context = &nested;
    ReentrantValue::exercise = [](void* context) {
        auto& values = *static_cast<NestedValues*>(context);
        take(values.runtime->initialize_value(values.type, values.source));
        take(values.runtime->copy_construct_value(values.type, values.copy, values.source));
        take(values.runtime->assign_value(values.type, values.source, values.copy));
        take(values.runtime->destroy_value(values.type, values.copy));
        take(values.runtime->destroy_value(values.type, values.source));
    };
    take(runtime.initialize_value(deep, source));
    take(runtime.copy_construct_value(deep, destination, source));
    take(runtime.assign_value(deep, destination, source));
    require(!ReentrantValue::failed && ReentrantValue::calls == 3 && ValueResource::live == 4,
            "Reentrant value operations damaged their enclosing walk.");
    auto strong = take(runtime.create(types[2]));
    auto weak = take(runtime.create(types[2]));
    const auto strong_id = strong.id();
    const auto weak_id = weak.id();
    const auto strong_field = take(runtime.find_field(leaf, "strong"));
    const auto weak_field = take(runtime.find_field(leaf, "weak"));
    take(runtime.write_reference(leaf, source, {&strong_field, 1}, strong.reference()));
    take(runtime.write_reference(leaf, source, {&weak_field, 1}, take(runtime.observe(weak.reference()))));
    take(runtime.assign_value(deep, destination, source));
    struct NestedTrace {
        Runtime* runtime;

        TypeId type;

        std::span<const std::byte> storage;

        std::size_t visits{};

        std::size_t nested_visits{};
    };
    NestedTrace tracing{&runtime, deep, destination};
    const ReferenceVisitor visitor{
        &tracing, [](void* context, const ObjectReference&) noexcept -> Result<void> {
            auto& state = *static_cast<NestedTrace*>(context);
            if (state.visits++ == 0)
                return state.runtime->visit_references(
                    state.type, state.storage,
                    {&state.nested_visits, [](void* nested_context, const ObjectReference&) noexcept -> Result<void> {
                         ++*static_cast<std::size_t*>(nested_context);
                         return {};
                     }});
            return {};
        }};
    take(runtime.visit_references(deep, source, visitor));
    require(tracing.visits == 2 && tracing.nested_visits == 2, "Nested reference traversal lost edges.");
    const auto failed = runtime.visit_references(
        deep, source, {nullptr, [](void*, const ObjectReference&) noexcept -> Result<void> {
                           return std::unexpected(Error{ErrorCode::capacity_exceeded, "Deliberate visitor failure."});
                       }});
    require(!failed && failed.error().code == ErrorCode::capacity_exceeded,
            "The reference visitor lost its explicit failure.");
    tracing.visits = tracing.nested_visits = 0;
    take(runtime.visit_references(deep, source, visitor));
    require(tracing.visits == 2 && tracing.nested_visits == 2, "Failed traversal left stale walk frames.");
    auto root = take(runtime.root_value(deep, source));
    strong.reset();
    weak.reset();
    take(runtime.begin_collection());
    take(runtime.assign_value(deep, destination, source));
    finish_collection(runtime);
    take(runtime.type_of(strong_id));
    require(!runtime.type_of(weak_id), "A weak edge incorrectly retained its object.");
    root.reset();
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(runtime.live_objects() == 0, "Unrooted value graph survived a fresh collection.");
    take(runtime.destroy_value(deep, destination));
    take(runtime.destroy_value(deep, source));
    require(ValueResource::live == 0 && !ReentrantValue::failed, "Reentrant destruction leaked resources.");
    ReentrantValue::exercise = nullptr;
    ReentrantValue::context = nullptr;
    std::cout << "[pass] Nested reentrant value walks recover from visitor failure and release their graph\n";
}

struct AddressSensitiveRecord {
    std::uint32_t number{17};

    ReferenceSlot target;

    static inline AddressSensitiveRecord* alternate{};

    static inline std::size_t address_calls{};

    AddressSensitiveRecord* operator&() noexcept {
        ++address_calls;
        return alternate ? alternate : this;
    }
};

struct AddressSensitiveOwner {
    AddressSensitiveRecord inner;

    static inline AddressSensitiveOwner* latest{};

    AddressSensitiveOwner() { latest = this; }
};

void verify_native_member_address() {
    AddressSensitiveRecord sentinel;
    sentinel.number = 91;
    AddressSensitiveRecord::alternate = std::addressof(sentinel);
    Runtime runtime;
    const auto types = take(runtime.register_types({
        {"address.Target", TypeKind::object, {}, {}, {}},
        NativeRegistration<AddressSensitiveRecord>{runtime, "address.Record", TypeKind::record}
            .field<&AddressSensitiveRecord::number>("number")
            .field<&AddressSensitiveRecord::target>("target",
                                                    {TypeReference{"address.Target"}, ValueForm::strong_reference})
            .build(),
        NativeRegistration<AddressSensitiveOwner>{runtime, "address.Owner", TypeKind::object}
            .field<&AddressSensitiveOwner::inner>("inner")
            .build(),
    }));
    auto owner = take(runtime.create(types[2]));
    auto* actual = AddressSensitiveOwner::latest;
    const std::array number_path{take(runtime.find_field(types[2], "inner")),
                                 take(runtime.find_field(types[1], "number"))};
    require(std::get<std::uint32_t>(take(runtime.read(owner.id(), number_path))) == 17,
            "Default native locator used overloaded operator& instead of the actual member.");
    take(runtime.write(owner.id(), number_path, std::uint32_t{73}));
    require(actual->inner.number == 73 && sentinel.number == 91, "Native field write changed an unrelated instance.");
    auto target = take(runtime.create(types[0]));
    const auto target_id = target.id();
    take(runtime.write_reference({types[0], ValueForm::strong_reference}, actual->inner.target, target.reference()));
    target.reset();
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(runtime.type_of(target_id).has_value() && runtime.live_objects() == 2,
            "Native member's strong edge did not retain its target through collection.");
    owner.reset();
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0 && AddressSensitiveRecord::address_calls == 0,
            "Native member access/trace invoked overloaded operator& or leaked roots/objects.");
    AddressSensitiveRecord::alternate = nullptr;
    AddressSensitiveOwner::latest = nullptr;
}
}

void verify_runtime_inheritance();

void verify_reference_sequences();

void verify_field_ranges();

void verify_array_elements();

void verify_value_paths();

void verify_write_stamps();

int main() {
    atelier::install_fatal_handler();
    try {
        atelier::acceptance::verify_default_allocator();
        verify_registration_text();
        verify_method_results();
        verify_root_handles();
        verify_value_operations();
        verify_collection_cycle();
        verify_late_registration();
        verify_collection_mutations();
        verify_collection_value_mutations();
        verify_collection_cursor_lifetimes();
        verify_locator_boundaries();
        verify_deep_value_walks();
        verify_native_member_address();
        verify_runtime_inheritance();
        verify_reference_sequences();
        verify_field_ranges();
        verify_array_elements();
        verify_value_paths();
        verify_write_stamps();
        std::cout << "Runtime acceptance: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Runtime acceptance: FAIL: " << error.what() << '\n';
        return 1;
    }
}
