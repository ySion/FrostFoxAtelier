#include "runtime/native.hpp"
#include "memory/acceptance.hpp"
#include "machine.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

using namespace atelier;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

template<class T>
void reject(const Result<T>& result, ErrorCode expected, const char* message) {
    require(!result && result.error().code == expected, message);
}

using atelier::acceptance::NativeState;
using atelier::acceptance::NativeMachine;

struct FallibleRecord {
    static inline bool fail{true};
    FallibleRecord() {
        if (fail) throw std::runtime_error("Deliberate construction failure.");
    }
};

struct alignas(32) ValueResource {
    static inline int live{}, next_id{}, last_destroyed{}, previous_destroyed{};
    static inline int remaining_constructions{-1};
    static inline bool fail_assignment{};
    static inline Runtime* callback_runtime{};
    static inline TypeId callback_object_type;
    static inline bool callbacks_blocked{true};
    std::unique_ptr<std::uint32_t> payload;
    int id;

    static void check_callback_guard() {
        if (!callback_runtime) return;
        const auto busy = [](const auto& result) { return !result && result.error().code == ErrorCode::busy; };
        TypeDefinition definition;
        definition.name = "values.FromCallback";
        callbacks_blocked &= busy(callback_runtime->register_type(std::move(definition)));
        TypeDefinition batch_definition;
        batch_definition.name = "values.BatchFromCallback";
        callbacks_blocked &= busy(callback_runtime->register_types({std::move(batch_definition)}));
        callbacks_blocked &= busy(callback_runtime->create(callback_object_type));
        callbacks_blocked &= busy(callback_runtime->destroy(ObjectId{}));
        callbacks_blocked &= busy(callback_runtime->begin_collection());
        callbacks_blocked &= busy(callback_runtime->collect_step({1}));
    }
    static int begin_lifetime() {
        check_callback_guard();
        if (remaining_constructions == 0) throw std::runtime_error("Deliberate value construction failure.");
        if (remaining_constructions > 0) --remaining_constructions;
        ++live;
        return ++next_id;
    }
    ValueResource() : payload(std::make_unique<std::uint32_t>(7)), id(begin_lifetime()) {}
    ValueResource(const ValueResource& source)
        : payload(std::make_unique<std::uint32_t>(*source.payload)), id(begin_lifetime()) {}
    ValueResource& operator=(const ValueResource& source) {
        check_callback_guard();
        *payload = *source.payload;
        if (fail_assignment) throw std::runtime_error("Deliberate value assignment failure.");
        return *this;
    }
    ~ValueResource() noexcept {
        check_callback_guard();
        --live;
        previous_destroyed = last_destroyed;
        last_destroyed = id;
    }
};

struct NoncopyableValue {
    std::unique_ptr<int> resource{std::make_unique<int>(42)};
};

struct CopyOnlyValue {
    CopyOnlyValue() = default;
    CopyOnlyValue(const CopyOnlyValue&) = default;
    CopyOnlyValue& operator=(const CopyOnlyValue&) = delete;
};

void verify_value_operations() {
    Runtime runtime;
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto resource = take(NativeRegistration<ValueResource>{runtime, "values.Resource", TypeKind::record}.publish());
    const auto noncopyable = take(NativeRegistration<NoncopyableValue>{runtime, "values.Noncopyable", TypeKind::record}.publish());
    const auto copy_only = take(NativeRegistration<CopyOnlyValue>{runtime, "values.CopyOnly", TypeKind::record}.publish());
    TypeDefinition pair_definition;
    pair_definition.name = "values.Pair";
    pair_definition.kind = TypeKind::record;
    pair_definition.fields = {{"first", {resource}}, {"second", {resource}}};
    const auto pair = take(runtime.register_type(std::move(pair_definition)));
    TypeDefinition record_definition;
    record_definition.name = "values.Record";
    record_definition.kind = TypeKind::record;
    record_definition.fields = {{"revision", {u32}}, {"resource", {resource}}, {"pair", {pair}}};
    const auto record = take(runtime.register_type(std::move(record_definition)));
    TypeDefinition blocked_definition;
    blocked_definition.name = "values.Blocked";
    blocked_definition.kind = TypeKind::record;
    blocked_definition.fields = {{"revision", {u32}}, {"resource", {noncopyable}}};
    const auto blocked = take(runtime.register_type(std::move(blocked_definition)));
    TypeDefinition empty_definition;
    empty_definition.name = "values.Empty";
    empty_definition.kind = TypeKind::record;
    const auto empty = take(runtime.register_type(std::move(empty_definition)));
    TypeDefinition object_definition;
    object_definition.name = "values.Object";
    const auto object = take(runtime.register_type(std::move(object_definition)));
    ValueResource::callback_runtime = &runtime;
    ValueResource::callback_object_type = object;

    alignas(32) std::array<std::byte, 256> source{}, destination{}, scratch{};
    auto source_bytes = std::span{source};
    auto destination_bytes = std::span{destination};
    const auto read_revision = [](const auto& bytes) {
        std::uint32_t result;
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    };
    const auto write_revision = [](auto& bytes, std::uint32_t value) {
        std::memcpy(bytes.data(), &value, sizeof(value));
    };

    // Caller-provided stack storage: values never acquire object identities.
    take(runtime.initialize_value(u32, source));
    require(read_revision(source) == 0, "Scalar initialization failed.");
    write_revision(source, 23);
    take(runtime.copy_construct_value(u32, destination, source));
    write_revision(source, 31);
    require(read_revision(destination) == 23, "Scalar copy aliases its source.");
    take(runtime.assign_value(u32, destination, source));
    require(read_revision(destination) == 31, "Scalar assignment failed.");
    take(runtime.destroy_value(u32, destination));
    // Only the value-sized ranges matter; the larger supplied spans may overlap.
    take(runtime.copy_construct_value(u32, source_bytes.subspan(sizeof(std::uint32_t)), source));
    take(runtime.destroy_value(u32, source_bytes.subspan(sizeof(std::uint32_t))));
    take(runtime.destroy_value(u32, source));

    take(runtime.initialize_value(resource, source));
    auto* native_source = std::launder(reinterpret_cast<ValueResource*>(source.data()));
    *native_source->payload = 42;
    take(runtime.copy_construct_value(resource, destination, source));
    auto* native_destination = std::launder(reinterpret_cast<ValueResource*>(destination.data()));
    require(ValueResource::live == 2 && *native_destination->payload == 42 &&
        native_source->payload != native_destination->payload, "Native value copy did not use its C++ semantics.");
    *native_source->payload = 99;
    require(*native_destination->payload == 42, "Native copy did not own independent content.");
    take(runtime.assign_value(resource, destination, source));
    require(*native_destination->payload == 99, "Native value assignment failed.");
    ValueResource::fail_assignment = true;
    take(runtime.assign_value(resource, destination, destination));
    *native_source->payload = 123;
    reject(runtime.assign_value(resource, destination, source), ErrorCode::assignment_failed,
        "Native assignment exception escaped its boundary.");
    require(*native_destination->payload == 123, "Assignment unexpectedly rolled back its partial update.");
    ValueResource::fail_assignment = false;
    take(runtime.destroy_value(resource, destination));
    take(runtime.destroy_value(resource, source));
    require(ValueResource::live == 0, "Native value destruction leaked a resource.");
    std::cout << "[pass] Caller-owned scalar/native values use initialization, copy, assignment and destruction\n";

    const auto* record_info = take(runtime.describe(record));
    require(record_info->copy_constructible && record_info->copy_assignable, "Nested copy capabilities are missing.");
    take(runtime.initialize_value(record, source));
    write_revision(source, 17); // The first described field has offset zero.
    take(runtime.copy_construct_value(record, destination, source));
    write_revision(source, 29);
    require(ValueResource::live == 6 && read_revision(destination) == 17, "Nested record copy failed.");
    take(runtime.assign_value(record, destination, source));
    require(read_revision(destination) == 29, "Nested record assignment failed.");

    ValueResource::remaining_constructions = 2;
    reject(runtime.copy_construct_value(record, scratch, source), ErrorCode::construction_failed,
        "Nested copy failure was not reported.");
    require(ValueResource::live == 6 && ValueResource::previous_destroyed == ValueResource::next_id &&
        ValueResource::last_destroyed == ValueResource::next_id - 1, "Nested copy did not unwind in reverse order.");
    ValueResource::remaining_constructions = 2;
    reject(runtime.initialize_value(record, scratch), ErrorCode::construction_failed, "Initialization failure was not reported.");
    require(ValueResource::live == 6 && ValueResource::previous_destroyed == ValueResource::next_id &&
        ValueResource::last_destroyed == ValueResource::next_id - 1, "Nested initialization did not unwind in reverse order.");
    ValueResource::remaining_constructions = -1;
    take(runtime.initialize_value(record, scratch));
    take(runtime.destroy_value(record, scratch));

    ValueResource::fail_assignment = true;
    write_revision(source, 53);
    reject(runtime.assign_value(record, destination, source), ErrorCode::assignment_failed, "Nested assignment failure was not reported.");
    require(read_revision(destination) == 53 && ValueResource::live == 6, "Failed assignment rolled back or broke value lifetime.");
    take(runtime.assign_value(record, destination, destination));
    // A partially assigned value remains destructible without a recovery assignment.
    take(runtime.destroy_value(record, destination));
    take(runtime.destroy_value(record, source));
    ValueResource::fail_assignment = false;
    require(ValueResource::live == 0, "Failed value operations leaked resources.");
    std::cout << "[pass] Nested values unwind failed construction and retain destructible partial assignments\n";

    const auto* blocked_info = take(runtime.describe(blocked));
    require(!blocked_info->copy_constructible && !blocked_info->copy_assignable, "Noncopyable fields did not restrict their record.");
    take(runtime.initialize_value(blocked, source));
    take(runtime.initialize_value(blocked, destination));
    write_revision(source, 100);
    reject(runtime.assign_value(blocked, destination, source), ErrorCode::unsupported_operation, "Unsupported assignment was accepted.");
    require(read_revision(destination) == 0, "Unsupported assignment modified an earlier field.");
    scratch.fill(std::byte{0x5a});
    reject(runtime.copy_construct_value(blocked, scratch, source), ErrorCode::unsupported_operation, "Unsupported copy was accepted.");
    require(std::ranges::all_of(scratch, [](auto byte) { return byte == std::byte{0x5a}; }), "Unsupported copy touched destination storage.");
    take(runtime.destroy_value(blocked, destination));
    take(runtime.destroy_value(blocked, source));
    const auto* copy_info = take(runtime.describe(copy_only));
    require(copy_info->copy_constructible && !copy_info->copy_assignable, "Independent copy capabilities were conflated.");
    take(runtime.initialize_value(copy_only, source));
    take(runtime.copy_construct_value(copy_only, destination, source));
    reject(runtime.assign_value(copy_only, destination, source), ErrorCode::unsupported_operation, "Nonassignable value accepted assignment.");
    take(runtime.destroy_value(copy_only, destination));
    take(runtime.destroy_value(copy_only, source));
    take(runtime.initialize_value(empty, source));
    take(runtime.copy_construct_value(empty, destination, source));
    take(runtime.assign_value(empty, destination, source));
    take(runtime.destroy_value(empty, destination));
    take(runtime.destroy_value(empty, source));

    Runtime other;
    reject(runtime.initialize_value(take(other.native_type<std::uint32_t>()), source), ErrorCode::invalid_type, "Foreign value type was accepted.");
    reject(runtime.initialize_value(object, source), ErrorCode::invalid_type, "Object was initialized through the value API.");
    reject(runtime.destroy_value(TypeId{}, source), ErrorCode::invalid_type, "Invalid value type was accepted.");
    take(runtime.initialize_value(record, source));
    take(runtime.initialize_value(record, destination));
    reject(runtime.initialize_value(record, {}), ErrorCode::invalid_storage, "Empty value storage was accepted.");
    reject(runtime.initialize_value(record, std::span{scratch}.first(record_info->size - 1)), ErrorCode::invalid_storage,
        "Undersized value storage was accepted.");
    reject(runtime.initialize_value(record, std::span{scratch}.subspan(1)), ErrorCode::invalid_storage, "Misaligned value storage was accepted.");
    reject(runtime.copy_construct_value(record, scratch, source_bytes.first(record_info->size - 1)), ErrorCode::invalid_storage,
        "Undersized copy source was accepted.");
    reject(runtime.copy_construct_value(record, source, source), ErrorCode::invalid_storage, "Copy construction allowed aliasing.");
    reject(runtime.copy_construct_value(record, source_bytes.subspan(32), source), ErrorCode::invalid_storage, "Copy construction allowed partial overlap.");
    reject(runtime.assign_value(record, source, source_bytes.subspan(32)), ErrorCode::invalid_storage, "Assignment allowed partial overlap.");
    reject(runtime.assign_value(record, destination_bytes.first(record_info->size - 1), source), ErrorCode::invalid_storage,
        "Assignment allowed undersized destination storage.");
    reject(runtime.assign_value(record, destination, source_bytes.subspan(1)), ErrorCode::invalid_storage, "Assignment allowed misaligned source storage.");
    reject(runtime.destroy_value(record, destination_bytes.subspan(1)), ErrorCode::invalid_storage, "Destruction allowed misaligned storage.");
    require(ValueResource::live == 6, "Invalid storage checks invoked value lifecycle operations.");
    take(runtime.destroy_value(record, destination));
    take(runtime.destroy_value(record, source));
    require(ValueResource::live == 0 && runtime.live_objects() == 0, "Value operations leaked values or created object identities.");
    require(ValueResource::callbacks_blocked, "Value callbacks bypassed runtime lifecycle protection.");
    ValueResource::callback_runtime = nullptr;
    // Values do not seal the object registration phase.
    TypeDefinition after_values;
    after_values.name = "values.AfterValueOperations";
    take(runtime.register_type(std::move(after_values)));
    std::cout << "[pass] Value capabilities and storage validation reject unsupported operations before mutation\n";
}

Result<void> advance_described(CallContext& call) {
    // Temporary native implementation of a future language method. Uses the shared runtime.
    const auto type = call.runtime.type_of(call.self);
    if (!type) return std::unexpected(type.error());
    const auto field = call.runtime.find_field(*type, "progress");
    if (!field) return std::unexpected(field.error());
    const auto value = call.runtime.read(call.self, *field);
    if (!value) return std::unexpected(value.error());
    return call.runtime.write(call.self, *field,
        std::get<float>(*value) + call.arguments[0].get<float>());
}

Result<void> attempt_self_destruction(CallContext& call) {
    return call.runtime.destroy(call.self);
}

Result<void> raise_native_error(CallContext&) {
    throw std::runtime_error("Deliberate method failure.");
}

struct Types {
    TypeId native_machine;
    TypeId described_machine;
    TypeId failing_machine;
};

Types register_types(Runtime& runtime) {
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto native_state = take(NativeRegistration<NativeState>{runtime, "acceptance.NativeState", TypeKind::record}
        .field<&NativeState::cycles>("cycles")
        .field<&NativeState::heat>("heat")
        .publish());
    // Registration order deliberately differs from native memory order.
    const auto native_machine = take(NativeRegistration<NativeMachine>{runtime, "acceptance.NativeMachine", TypeKind::object}
        .field<&NativeMachine::state>("state")
        .field<&NativeMachine::progress>("progress")
        .method<&NativeMachine::advance>("advance")
        .publish());

    TypeDefinition state;
    state.name = "acceptance.State";
    state.kind = TypeKind::record;
    state.fields = {{"heat", {f32}}, {"cycles", {u32}}};
    const auto described_state = take(runtime.register_type(std::move(state)));
    TypeDefinition machine;
    machine.name = "acceptance.DescribedMachine";
    machine.fields = {{"state", {described_state}}, {"progress", {f32}}};
    machine.methods = {
        {"advance", {{{f32}}}, advance_described},
        {"attempt_self_destruction", {}, attempt_self_destruction},
        {"raise_native_error", {}, raise_native_error}
    };
    const auto described_machine = take(runtime.register_type(std::move(machine)));

    const auto fallible = take(NativeRegistration<FallibleRecord>{runtime, "acceptance.FallibleRecord", TypeKind::record}.publish());
    TypeDefinition failing;
    failing.name = "acceptance.FailingMachine";
    failing.fields = {{"initialized_first", {native_state}}, {"fails_second", {fallible}}};
    const auto failing_machine = take(runtime.register_type(std::move(failing)));

    TypeDefinition duplicate;
    duplicate.name = "acceptance.DescribedMachine";
    reject(runtime.register_type(std::move(duplicate)), ErrorCode::duplicate_name, "Duplicate type was accepted.");
    TypeDefinition invalid;
    invalid.name = "acceptance.Invalid";
    invalid.fields = {{"invalid_inline_object", {native_machine}}};
    reject(runtime.register_type(std::move(invalid)), ErrorCode::invalid_definition, "Object accepted as inline value.");
    reject(runtime.find_type("acceptance.Invalid"), ErrorCode::invalid_type, "Failed registration published a type.");
    std::cout << "[pass] Unified registration validates and publishes definitions atomically\n";
    return {native_machine, described_machine, failing_machine};
}

// This consumer has no knowledge of C++ classes or type origin.
void exercise_machine(Runtime& runtime, TypeId type) {
    const auto* info = take(runtime.describe(type));
    const auto progress = take(runtime.find_field(type, "progress"));
    const auto advance = take(runtime.find_method(type, "advance"));
    const auto state = take(runtime.find_field(type, "state"));
    const auto state_info = std::ranges::find_if(info->fields,
        [](const auto& field) { return field.name == "state"; });
    require(state_info != info->fields.end(), "State metadata is missing.");
    const auto heat = take(runtime.find_field(state_info->value.type, "heat"));
    const auto cycles = take(runtime.find_field(state_info->value.type, "cycles"));
    const std::array heat_path{state, heat};
    const std::array cycles_path{state, cycles};
    reject(runtime.create(state_info->value.type), ErrorCode::invalid_type, "Value record received independent object identity.");

    const auto object_root = take(runtime.create(type));
    const auto object = object_root.id();
    require(take(runtime.type_of(object)) == type, "Object has the wrong runtime type.");
    require(std::get<float>(take(runtime.read(object, progress))) == 0.0f, "Progress was not initialized.");
    require(std::get<float>(take(runtime.read(object, heat_path))) == 0.0f, "Inline float was not initialized.");
    require(std::get<std::uint32_t>(take(runtime.read(object, cycles_path))) == 0, "Inline integer was not initialized.");
    take(runtime.write(object, heat_path, 42.0f));
    take(runtime.write(object, cycles_path, std::uint32_t{7}));
    require(std::get<float>(take(runtime.read(object, heat_path))) == 42.0f, "Inline float read/write failed.");
    require(std::get<std::uint32_t>(take(runtime.read(object, cycles_path))) == 7, "Inline integer read/write failed.");

    take(runtime.write(object, progress, 0.25f));
    const std::array<Scalar, 1> arguments{0.125f};
    take(runtime.call(object, advance, arguments));
    require(std::get<float>(take(runtime.read(object, progress))) == 0.375f, "Method did not advance progress.");
    const std::array<Scalar, 1> wrong_arguments{std::uint32_t{99}};
    reject(runtime.call(object, advance, wrong_arguments), ErrorCode::argument_mismatch, "Incorrect argument type was accepted.");
    reject(runtime.call(object, advance, {}), ErrorCode::argument_mismatch, "Incorrect argument count was accepted.");
    reject(runtime.write(object, progress, std::uint32_t{99}), ErrorCode::type_mismatch, "Incorrect field type was accepted.");
    reject(runtime.read(object, heat), ErrorCode::invalid_member, "Nested member accepted as a top-level field.");
    require(std::get<float>(take(runtime.read(object, progress))) == 0.375f, "Rejected operation changed object state.");

    take(runtime.destroy(object));
    reject(runtime.read(object, progress), ErrorCode::invalid_object, "Destroyed object remained readable.");
    reject(runtime.call(object, advance, arguments), ErrorCode::invalid_object, "Destroyed object remained callable.");
    reject(runtime.destroy(object), ErrorCode::invalid_object, "Double destruction was accepted.");
    const auto replacement_root = take(runtime.create(type));
    const auto replacement = replacement_root.id();
    require(replacement != object, "Slot reuse resurrected an old identity.");
    reject(runtime.read(object, progress), ErrorCode::invalid_object, "Stale handle reached a replacement object.");
    require(std::get<float>(take(runtime.read(replacement, progress))) == 0.0f, "Replacement reused old field state.");
    take(runtime.destroy(replacement));
    require(runtime.live_objects() == 0, "Explicit destruction leaked an object.");
    std::cout << "[pass] " << info->name << ": inspect -> create -> inline fields -> invoke -> destroy -> reuse\n";
}

void verify_runtime() {
    {
        Runtime runtime;
        const auto types = register_types(runtime);
        reject(runtime.create(types.failing_machine), ErrorCode::construction_failed, "Constructor exception was not reported.");
        require(NativeState::live == 0 && runtime.live_objects() == 0, "Partial construction leaked an inline value.");
        // Failed construction must not seal registration.
        TypeDefinition after_failure;
        after_failure.name = "acceptance.AfterFailedConstruction";
        take(runtime.register_type(std::move(after_failure)));
        FallibleRecord::fail = false;
        take(runtime.destroy(take(runtime.create(types.failing_machine)).id()));
        require(NativeState::live == 0, "Recovered construction did not release its inline value.");
        std::cout << "[pass] Failed construction unwinds initialized records and permits recovery\n";

        exercise_machine(runtime, types.native_machine);
        exercise_machine(runtime, types.described_machine);

        const auto described_root = take(runtime.create(types.described_machine));
        const auto described = described_root.id();
        const auto native_method = take(runtime.find_method(types.native_machine, "advance"));
        const std::array<Scalar, 1> arguments{0.125f};
        reject(runtime.call(described, native_method, arguments), ErrorCode::invalid_member, "Method accepted the wrong receiver type.");
        const auto self_destruct = take(runtime.find_method(types.described_machine, "attempt_self_destruction"));
        reject(runtime.call(described, self_destruct, {}), ErrorCode::busy, "Callback destroyed its active receiver.");
        const auto raises = take(runtime.find_method(types.described_machine, "raise_native_error"));
        reject(runtime.call(described, raises, {}), ErrorCode::invocation_failed, "Native exception escaped the invocation boundary.");
        take(runtime.destroy(described));
        TypeDefinition late;
        late.name = "acceptance.TooLate";
        reject(runtime.register_type(std::move(late)), ErrorCode::busy, "Registration remained open during execution.");
        std::cout << "[pass] Receiver checks, callback lifetime protection and native error reporting\n";

        Runtime other;
        const auto other_types = register_types(other);
        const auto local_root = take(runtime.create(types.native_machine));
        const auto local = local_root.id();
        const auto foreign_root = take(other.create(other_types.native_machine));
        const auto foreign = foreign_root.id();
        reject(runtime.type_of(foreign), ErrorCode::invalid_object, "Foreign object was accepted.");
        reject(other.type_of(local), ErrorCode::invalid_object, "Object identity escaped its runtime.");
        reject(runtime.describe(other_types.native_machine), ErrorCode::invalid_type, "Foreign type was accepted.");
        std::cout << "[pass] Independent runtimes reject foreign type and object identities\n";
        // Leave both native objects alive: Runtime owns final cleanup.
        require(NativeState::live == 2, "Unexpected native lifetime before shutdown.");
    }
    require(NativeState::live == 0, "Runtime shutdown leaked a native record.");
    require(NativeMachine::created == 4 && NativeMachine::destroyed == NativeMachine::created,
        "Native construction/destruction counts do not match.");
    std::cout << "[pass] Runtime shutdown releases remaining native objects exactly once\n";
}

struct SignatureNative {
    static inline std::size_t calls{};
    std::uint32_t value{};
    float total{};

    std::uint32_t increment(std::uint32_t amount) { ++calls; return value += amount; }
    std::uint32_t read() const noexcept { ++calls; return value; }
    float add(float amount) noexcept { ++calls; return total += amount; }
    float sample(float amount) const { ++calls; return total + amount; }
    void note(std::uint32_t amount) { ++calls; value += amount; }
    std::uint32_t fail(std::uint32_t amount) {
        ++calls;
        value += amount;
        throw std::runtime_error("Native scalar result failure.");
    }
};

TypeId register_signature_native(Runtime& runtime) {
    return take(NativeRegistration<SignatureNative>{runtime, "signature.Native", TypeKind::object}
        .field<&SignatureNative::value>("value")
        .field<&SignatureNative::total>("total")
        .method<&SignatureNative::increment>("increment")
        .method<&SignatureNative::read>("read")
        .method<&SignatureNative::add>("add")
        .method<&SignatureNative::sample>("sample")
        .method<&SignatureNative::note>("note")
        .method<&SignatureNative::fail>("fail").publish());
}

const FunctionSignature& method_signature(const TypeInfo& type, std::string_view name) {
    const auto method = std::ranges::find_if(type.methods, [&](const auto& value) { return value.name == name; });
    require(method != type.methods.end(), "Expected method signature is missing.");
    return method->signature;
}

void verify_signature_publication() {
    Runtime runtime;
    Runtime other;
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto native = register_signature_native(runtime);
    const auto foreign = register_signature_native(other);
    const auto* info = take(runtime.describe(native));
    const auto record = take(runtime.register_type({"signature.Record", TypeKind::record, {}, {}, {}}));

    SignatureDefinition definition;
    definition.parameters = {{TypeReference{"core.u32"}}};
    definition.result = ValueDefinition{TypeReference::native<std::uint32_t>()};
    definition.receiver = ReceiverDefinition{native, false};
    const auto resolved = take(runtime.resolve_signature(definition));
    require(resolved == method_signature(*info, "increment"), "Native and public signature resolution disagree.");
    definition.parameters.clear();
    definition.result.reset();
    definition.receiver.reset();
    require(resolved.parameters == std::vector<ParameterDescription>{{u32}} &&
        resolved.result == std::optional{ValueDescription{u32}} &&
        resolved.receiver == std::optional{ReceiverDescription{native, false}},
        "Resolved signature borrowed mutable producer storage.");
    const auto empty = take(runtime.resolve_signature({}));
    require(empty.parameters.empty() && !empty.result && !empty.receiver, "Free void signature acquired a receiver or value.");
    SignatureDefinition coroutine_definition;
    coroutine_definition.execution = FunctionExecution::coroutine;
    require(take(runtime.resolve_signature(coroutine_definition)).execution == FunctionExecution::coroutine,
        "Signature resolution lost coroutine execution semantics.");
    TypeDefinition invalid_coroutine{"signature.InvalidCoroutine", TypeKind::object, {}, {}, {}};
    MethodDefinition invalid_method;
    invalid_method.name = "suspend";
    invalid_method.signature.execution = FunctionExecution::coroutine;
    invalid_method.invoke = [](CallContext&) -> Result<void> { return {}; };
    invalid_coroutine.methods.push_back(std::move(invalid_method));
    reject(runtime.register_type(std::move(invalid_coroutine)), ErrorCode::invalid_definition,
        "A synchronous native Invoker accepted a coroutine method contract.");
    for (const auto* name : {"read", "sample"}) {
        const auto& signature = method_signature(*info, name);
        require(signature.receiver == std::optional{ReceiverDescription{native, true}},
            "Native const receiver information was lost.");
    }
    require(method_signature(*info, "sample").result == std::optional{ValueDescription{f32}} &&
        method_signature(*info, "add").result == std::optional{ValueDescription{f32}} &&
        !method_signature(*info, "add").receiver->read_only && !method_signature(*info, "note").result,
        "Native scalar/void results or mutable receiver information were misinferred.");

    const std::vector<ValueDefinition> invalid_values{
        {record}, {native}, {native, ValueForm::strong_reference}, {native, ValueForm::weak_reference},
        {u32, ValueForm::strong_reference}, {u32, static_cast<ValueForm>(99)},
        {take(other.native_type<std::uint32_t>())}, {TypeReference{"signature.Missing"}}
    };
    for (const auto& value : invalid_values) {
        SignatureDefinition invalid;
        invalid.parameters = {value};
        reject(runtime.resolve_signature(invalid), ErrorCode::invalid_definition, "Invalid signature parameter was resolved.");
        invalid.parameters.clear();
        invalid.result = value;
        reject(runtime.resolve_signature(invalid), ErrorCode::invalid_definition, "Invalid signature result was resolved.");
    }
    const std::array<TypeReference, 4> invalid_receivers{
        TypeReference{f32}, TypeReference{record}, TypeReference{foreign}, TypeReference{"signature.Missing"}
    };
    for (const auto& receiver : invalid_receivers) {
        SignatureDefinition invalid;
        invalid.receiver = ReceiverDefinition{receiver, true};
        reject(runtime.resolve_signature(invalid), ErrorCode::invalid_definition, "Invalid signature receiver was resolved.");
    }

    SignatureDefinition staged;
    staged.result = ValueDefinition{u32};
    staged.receiver = ReceiverDefinition{TypeReference{"signature.Pending"}, true};
    reject(runtime.resolve_signature(staged), ErrorCode::invalid_definition, "Public resolver accepted an unpublished receiver.");
    TypeDefinition pending;
    pending.name = "signature.Pending";
    pending.methods = {{"read", staged, [](CallContext& call) -> Result<void> {
        call.result = std::uint32_t{41};
        return {};
    }}};
    TypeDefinition later = pending;
    later.name = "signature.Later";
    later.methods[0].signature.receiver.reset();
    later.methods[0].signature.result = ValueDefinition{record};
    reject(runtime.register_types({pending, later}), ErrorCode::invalid_definition,
        "Batch accepted an unsupported result after resolving an earlier receiver.");
    for (const auto* name : {"signature.Pending", "signature.Later"})
        reject(runtime.find_type(name), ErrorCode::invalid_type, "Failed signature batch leaked a provisional type.");
    reject(runtime.resolve_signature(staged), ErrorCode::invalid_definition, "Failed batch left a resolvable provisional receiver.");
    require(take(runtime.describe(native)) == info && take(runtime.native_type<SignatureNative>()) == native,
        "Failed signature batch changed existing metadata or native mapping.");
    later.methods[0].signature.result = ValueDefinition{u32};
    const auto ids = take(runtime.register_types({pending, later}));
    const auto published = take(runtime.resolve_signature(staged));
    require(published == method_signature(*take(runtime.describe(ids[0])), "read") &&
        published.receiver == std::optional{ReceiverDescription{ids[0], true}} &&
        method_signature(*take(runtime.describe(ids[1])), "read").receiver ==
            std::optional{ReceiverDescription{ids[1], false}},
        "Staged explicit or inferred member receiver did not resolve to its owner.");
    pending.name = "signature.WrongOwner";
    pending.methods[0].signature.receiver = ReceiverDefinition{native, false};
    reject(runtime.register_type(std::move(pending)), ErrorCode::invalid_definition, "Explicit method receiver named another owner.");
    reject(runtime.find_type("signature.WrongOwner"), ErrorCode::invalid_type, "Receiver mismatch leaked a type.");

    auto object = take(runtime.create(ids[0]));
    require(take(runtime.resolve_signature(staged)) == published,
        "Read-only signature resolution stopped after object creation sealed registration.");
    reject(runtime.register_type({"signature.TooLate", TypeKind::object, {}, {}, {}}), ErrorCode::busy,
        "Read-only signature resolution reopened registration.");
    Scalar output{0.0f};
    take(runtime.call(object.id(), take(runtime.find_method(ids[0], "read")), {}, &output));
    require(output == Scalar{std::uint32_t{41}}, "Published staged result method was not executable.");
    std::cout << "[pass] Shared signatures own resolved metadata, infer receivers and preserve atomic sealed registration\n";
}

Result<void> read_signature_value(CallContext& call) {
    auto type = call.runtime.type_of(call.self);
    if (!type) return std::unexpected(std::move(type.error()));
    auto field = call.runtime.find_field(*type, "value");
    if (!field) return std::unexpected(std::move(field.error()));
    auto value = call.runtime.read(call.self, *field);
    if (!value) return std::unexpected(std::move(value.error()));
    call.result = *value;
    return {};
}

Result<void> increment_signature_value(CallContext& call) {
    auto type = call.runtime.type_of(call.self);
    if (!type) return std::unexpected(std::move(type.error()));
    auto field = call.runtime.find_field(*type, "value");
    if (!field) return std::unexpected(std::move(field.error()));
    auto value = call.runtime.read(call.self, *field);
    if (!value) return std::unexpected(std::move(value.error()));
    const auto next = std::get<std::uint32_t>(*value) + call.arguments[0].get<std::uint32_t>();
    auto written = call.runtime.write(call.self, *field, next);
    if (!written) return written;
    call.result = next;
    return {};
}

void verify_method_results() {
    Runtime runtime;
    SignatureNative::calls = 0;
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto native = register_signature_native(runtime);
    SignatureDefinition increment;
    increment.parameters = {{u32}};
    increment.result = ValueDefinition{u32};
    SignatureDefinition read;
    read.result = ValueDefinition{u32};
    read.receiver = ReceiverDefinition{TypeReference{"signature.Described"}, true};
    auto echo = read;
    echo.parameters = {{f32}};
    echo.result = ValueDefinition{f32};
    auto void_signature = increment;
    void_signature.result.reset();
    TypeDefinition definition;
    definition.name = "signature.Described";
    definition.fields = {{"value", {u32}}};
    definition.methods = {
        {"increment", increment, increment_signature_value},
        {"read", read, read_signature_value},
        {"echo", echo, [](CallContext& call) -> Result<void> {
            call.result = call.arguments[0].get<float>();
            return {};
        }},
        {"missing", increment, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            call.result.reset();
            return result;
        }},
        {"wrong", increment, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            call.result = 1.0f;
            return result;
        }},
        {"failure", increment, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            if (!result) return result;
            call.result = 1.0f; // The declared failure must win over a bad staged result.
            return std::unexpected(Error{ErrorCode::assignment_failed, "Declared result failure wins."});
        }},
        {"raises", increment, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            if (!result) return result;
            throw std::runtime_error("Described scalar result failure.");
        }},
        {"unknown", increment, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            if (!result) return result;
            throw 17;
        }},
        {"fabricated_void", void_signature, increment_signature_value},
        {"clean_void", void_signature, [](CallContext& call) -> Result<void> {
            auto result = increment_signature_value(call);
            call.result.reset();
            return result;
        }},
        {"forbid_source", {}, [](CallContext& call) -> Result<void> { return call.runtime.check_execution_entry(); }}
    };
    const auto described = take(runtime.register_type(std::move(definition)));
    const auto* info = take(runtime.describe(described));
    require(method_signature(*info, "increment").receiver == std::optional{ReceiverDescription{described, false}} &&
        method_signature(*info, "read").receiver == std::optional{ReceiverDescription{described, true}},
        "Described inferred/explicit const receiver metadata disagrees with native semantics.");
    auto native_object = take(runtime.create(native));
    auto described_object = take(runtime.create(described));
    const std::array<Scalar, 1> arguments{std::uint32_t{3}};
    const std::array<Scalar, 1> floats{2.5f};
    const Scalar sentinel{-731.25f};
    Scalar output = sentinel;
    const auto native_increment = take(runtime.find_method(native, "increment"));
    take(runtime.call(native_object.id(), native_increment, arguments, &output));
    require(output == Scalar{std::uint32_t{3}}, "Native uint result was not committed.");
    take(runtime.call(native_object.id(), take(runtime.find_method(native, "read")), {}, &output));
    require(output == Scalar{std::uint32_t{3}}, "Const noexcept native method returned the wrong uint.");
    take(runtime.call(native_object.id(), take(runtime.find_method(native, "add")), floats, &output));
    require(output == Scalar{2.5f}, "Native float result was not committed.");
    take(runtime.call(native_object.id(), take(runtime.find_method(native, "sample")), floats, &output));
    require(output == Scalar{5.0f}, "Const throwing-capable native float method was not invoked correctly.");
    output = sentinel;
    take(runtime.call(native_object.id(), take(runtime.find_method(native, "note")), arguments, &output));
    require(output == sentinel, "Successful void native method wrote result storage.");
    const auto calls = SignatureNative::calls;
    reject(runtime.call(native_object.id(), native_increment, arguments), ErrorCode::invalid_storage,
        "Non-void native method accepted missing result storage.");
    reject(runtime.call(native_object.id(), native_increment, floats, &output), ErrorCode::argument_mismatch,
        "Non-void native method accepted wrong argument kinds.");
    reject(runtime.call(native_object.id(), native_increment, {}, &output), ErrorCode::argument_mismatch,
        "Non-void native method accepted wrong argument count.");
    require(SignatureNative::calls == calls && output == sentinel, "Rejected native call ran code or changed output.");
    auto failed_native = runtime.call(native_object.id(), take(runtime.find_method(native, "fail")), arguments, &output);
    reject(failed_native, ErrorCode::invocation_failed, "Native scalar exception escaped.");
    require(output == sentinel && failed_native.error().message == "Native scalar result failure." &&
        std::get<std::uint32_t>(take(runtime.read(native_object.id(), take(runtime.find_field(native, "value"))))) == 9,
        "Native failure changed output, lost its cause or rolled back prior side effects.");

    const auto described_increment = take(runtime.find_method(described, "increment"));
    const auto value_field = take(runtime.find_field(described, "value"));
    reject(runtime.call(described_object.id(), described_increment, arguments), ErrorCode::invalid_storage,
        "Described non-void method accepted missing result storage.");
    require(std::get<std::uint32_t>(take(runtime.read(described_object.id(), value_field))) == 0,
        "Missing result storage was rejected after described side effects.");
    take(runtime.call(described_object.id(), described_increment, arguments, &output));
    require(output == Scalar{std::uint32_t{3}}, "Described uint result did not share native result semantics.");
    take(runtime.call(described_object.id(), take(runtime.find_method(described, "read")), {}, &output));
    require(output == Scalar{std::uint32_t{3}}, "Described const result metadata was not executable.");
    take(runtime.call(described_object.id(), take(runtime.find_method(described, "echo")), floats, &output));
    require(output == Scalar{2.5f}, "Described float result did not share native result semantics.");
    struct FailureCase { const char* name; ErrorCode code; const char* diagnostic; };
    const std::array failures{
        FailureCase{"missing", ErrorCode::invocation_failed, nullptr},
        FailureCase{"wrong", ErrorCode::invocation_failed, nullptr},
        FailureCase{"failure", ErrorCode::assignment_failed, "Declared result failure wins."},
        FailureCase{"raises", ErrorCode::invocation_failed, "Described scalar result failure."},
        FailureCase{"unknown", ErrorCode::invocation_failed, "Native method raised an unknown exception."},
        FailureCase{"fabricated_void", ErrorCode::invocation_failed, nullptr}
    };
    for (const auto& failure : failures) {
        const auto before = std::get<std::uint32_t>(take(runtime.read(described_object.id(), value_field)));
        output = sentinel;
        const auto result = runtime.call(described_object.id(), take(runtime.find_method(described, failure.name)), arguments, &output);
        reject(result, failure.code, "Invalid callback result or failure was accepted.");
        require(output == sentinel &&
            std::get<std::uint32_t>(take(runtime.read(described_object.id(), value_field))) == before + 3,
            "Failed result validation committed output or rolled back callback side effects.");
        if (failure.diagnostic) require(result.error().message == failure.diagnostic, "First callback error was replaced by result validation.");
    }
    take(runtime.call(described_object.id(), take(runtime.find_method(described, "clean_void")), arguments, &output));
    require(output == sentinel, "Successful void described method wrote result storage.");
    reject(runtime.call(described_object.id(), take(runtime.find_method(described, "forbid_source")), {}, &output), ErrorCode::busy,
        "Native CallbackScope stopped rejecting generated-code reentry.");
    reject(runtime.call(described_object.id(), native_increment, arguments, &output), ErrorCode::invalid_member,
        "Result-bearing method accepted another receiver type.");
    auto doomed = take(runtime.create(described));
    const auto stale = doomed.id();
    take(runtime.destroy(stale));
    reject(runtime.call(stale, described_increment, arguments, &output), ErrorCode::invalid_object,
        "Result-bearing method accepted a stale receiver.");
    Runtime other;
    auto foreign = take(other.create(register_signature_native(other)));
    reject(runtime.call(foreign.id(), native_increment, arguments, &output), ErrorCode::invalid_object,
        "Result-bearing method accepted a foreign Runtime receiver.");
    require(output == sentinel, "Rejected receiver or source reentry changed result storage.");
    std::cout << "[pass] Native/described scalar results commit only success, preserve errors and retain callback lifetime guards\n";
}

void verify_forward_publication() {
    Runtime runtime;
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto* scalar_info = take(runtime.describe(f32));
    std::vector<TypeDefinition> definitions;
    definitions.push_back(NativeRegistration<NativeMachine>{runtime, "batch.NativeMachine", TypeKind::object}
        .field<&NativeMachine::state>("state")
        .field<&NativeMachine::progress>("progress")
        .method<&NativeMachine::advance>("advance").build());
    definitions.push_back({"batch.DescribedMachine", TypeKind::object,
        {{"state", {TypeReference{"batch.State"}}}, {"progress", {f32}}},
        {{"advance", {{{TypeReference{"core.f32"}}}}, advance_described}}, {}});
    definitions.push_back({"batch.AlignedOwner", TypeKind::object,
        {{"value", {TypeReference{"batch.AlignedValue"}}}}, {}, {}});
    definitions.push_back({"batch.AlignedValue", TypeKind::record,
        {{"revision", {u32}}, {"state", {TypeReference::native<NativeState>()}}}, {}, {}});
    definitions.push_back(NativeRegistration<NativeState>{runtime, "batch.NativeState", TypeKind::record}
        .field<&NativeState::cycles>("cycles").field<&NativeState::heat>("heat").build());
    definitions.push_back({"batch.State", TypeKind::record, {{"heat", {f32}}, {"cycles", {u32}}}, {}, {}});
    definitions.push_back({"batch.NoncopyableValue", TypeKind::record,
        {{"resource", {TypeReference::native<NoncopyableValue>()}}}, {}, {}});
    definitions.push_back(NativeRegistration<NoncopyableValue>{runtime, "batch.NativeNoncopyable", TypeKind::record}.build());
    std::vector<std::string> names;
    for (const auto& definition : definitions) names.push_back(definition.name);
    const auto ids = take(runtime.register_types(std::move(definitions)));
    require(ids.size() == names.size(), "Batch did not return every type identity.");
    for (std::size_t i = 0; i != ids.size(); ++i) {
        require(take(runtime.find_type(names[i])) == ids[i], "Batch result order differs from definition order.");
    }
    require(take(runtime.describe(f32)) == scalar_info, "Publication invalidated existing metadata addresses.");
    require(take(runtime.native_type<NativeState>()) == ids[4], "Native representation resolved to another identity.");
    const auto* aligned = take(runtime.describe(ids[3]));
    require(aligned->alignment == alignof(NativeState) && aligned->size == sizeof(NativeState) * 2,
        "Forward inline dependency lost alignment or padding.");
    require(aligned->copy_constructible && aligned->copy_assignable, "Inline copy capabilities were lost.");
    const auto* noncopyable = take(runtime.describe(ids[6]));
    require(!noncopyable->copy_constructible && !noncopyable->copy_assignable,
        "Forward inline dependency incorrectly gained copy operations.");
    const auto* machine = take(runtime.describe(ids[1]));
    require(machine->fields.front().value.type == ids[5] &&
        machine->fields.front().value.form == ValueForm::inline_value &&
        machine->methods.front().signature.parameters.front().type == f32,
        "Published fields or parameters retained unresolved selectors.");

    exercise_machine(runtime, ids[0]);
    exercise_machine(runtime, ids[1]);
    const auto owner_root = take(runtime.create(ids[2]));
    const auto owner = owner_root.id();
    const std::array path{take(runtime.find_field(ids[2], "value")),
        take(runtime.find_field(ids[3], "state")), take(runtime.find_field(ids[4], "heat"))};
    take(runtime.write(owner, path, 17.5f));
    require(std::get<float>(take(runtime.read(owner, path))) == 17.5f, "Forward native inline storage was not usable.");
    take(runtime.destroy(owner));
    require(NativeState::live == 0, "Grouped construction leaked a native inline value.");
    reject(runtime.register_types({{"batch.TooLate", TypeKind::object, {}, {}, {}}}), ErrorCode::busy,
        "Batch registration bypassed sealing.");
    std::cout << "[pass] Atomic batches resolve native and described forward layouts, fields and methods\n";
}

struct NativeReferenceNode {
    static inline int constructions{};
    ReferenceSlot next;
    NativeReferenceNode() { ++constructions; }
};

Result<void> collect_reference(void* context, const ObjectReference& reference) {
    static_cast<std::vector<ObjectReference>*>(context)->push_back(reference);
    return {};
}

void verify_reference_definitions() {
    Runtime runtime;
    const auto f32 = take(runtime.native_type<float>());
    std::vector<TypeDefinition> definitions;
    definitions.push_back({"references.A", TypeKind::object,
        {{"next", {TypeReference::native<NativeReferenceNode>(), ValueForm::strong_reference}}, {"weight", {f32}}}, {}, {}});
    definitions.push_back(NativeRegistration<NativeReferenceNode>{runtime, "references.B", TypeKind::object}
        .field<&NativeReferenceNode::next>("previous", {TypeReference{"references.A"}, ValueForm::weak_reference}).build());
    definitions.push_back({"references.Self", TypeKind::object,
        {{"self", {TypeReference{"references.Self"}, ValueForm::strong_reference}}}, {}, {}});
    definitions.push_back({"references.Outer", TypeKind::record,
        {{"inner", {TypeReference{"references.Inner"}}}}, {}, {}});
    definitions.push_back({"references.Inner", TypeKind::record,
        {{"target", {TypeReference{"references.A"}, ValueForm::strong_reference}}}, {}, {}});
    // The cycle through an object reference is not an inline layout cycle.
    definitions.push_back({"references.Owner", TypeKind::object,
        {{"value", {TypeReference{"references.Record"}}}}, {}, {}});
    definitions.push_back({"references.Record", TypeKind::record,
        {{"owner", {TypeReference{"references.Owner"}, ValueForm::weak_reference}}}, {}, {}});
    const auto ids = take(runtime.register_types(std::move(definitions)));
    const auto* a = take(runtime.describe(ids[0]));
    const auto* b = take(runtime.describe(ids[1]));
    require(a->fields[0].value.type == ids[1] && a->fields[0].value.form == ValueForm::strong_reference,
        "Strong native target was not resolved in the same batch.");
    require(b->fields[0].value.type == ids[0] && b->fields[0].value.form == ValueForm::weak_reference,
        "Weak described target was not resolved in the same batch.");
    const auto* self = take(runtime.describe(ids[2]));
    require(self->fields[0].value.type == ids[2] && self->size == sizeof(ReferenceSlot), "Self reference expanded its target layout.");
    const auto* inner = take(runtime.describe(ids[4]));
    require(inner->size == sizeof(ReferenceSlot) && inner->alignment == alignof(ReferenceSlot) && inner->size < a->size,
        "A reference slot used the target object's instance size.");

    require(NativeReferenceNode::constructions == 0, "Definition publication ran a native constructor.");
    alignas(32) std::array<std::byte, 256> source{}, destination{};
    std::vector<ObjectReference> references;
    for (const auto id : ids) {
        const auto* info = take(runtime.describe(id));
        require(info->contains_references, "Reference-bearing layout lost its tracing metadata.");
        references.clear();
        if (info->kind == TypeKind::object) {
            auto object = take(runtime.create(id));
            require(object.registered() && runtime.root_count() == 1, "Creation did not deliver a protected object.");
            take(runtime.visit_references(object.id(), {&references, collect_reference}));
            take(runtime.destroy(object.id()));
            object.reset();
        } else {
            require(info->copy_constructible && info->copy_assignable, "Reference records lost value-copy capabilities.");
            require(info->size <= source.size() && info->alignment <= 32, "Acceptance buffers do not fit the record.");
            take(runtime.initialize_value(id, source));
            auto root = take(runtime.root_value(id, source));
            take(runtime.copy_construct_value(id, destination, source));
            take(runtime.assign_value(id, source, destination));
            take(runtime.visit_roots({&references, collect_reference}));
            reject(runtime.destroy_value(id, source), ErrorCode::busy, "Rooted value storage was destroyed.");
            root.reset();
            take(runtime.destroy_value(id, destination));
            take(runtime.destroy_value(id, source));
        }
        require(references.empty(), "Default reference slots were not null.");
        require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Reference value operations leaked roots or objects.");
    }
    require(NativeReferenceNode::constructions == 1, "Native reference storage was not constructed exactly once.");
    std::cout << "[pass] Published reference cycles instantiate safely and reference records support value operations\n";
}

struct RootLifetimeProbe {
    static inline int live{};
    RootLifetimeProbe() { ++live; }
    ~RootLifetimeProbe() noexcept { --live; }
};

void verify_root_handles() {
    Runtime runtime;
    const auto type = take(NativeRegistration<RootLifetimeProbe>{runtime, "roots.Object", TypeKind::object}.publish());
    auto owner = take(runtime.create(type));
    require(owner.registered() && runtime.root_count() == 1 && RootLifetimeProbe::live == 1,
        "Creation returned before registering its first root.");
    const auto identity = owner.id();
    const auto strong = owner.reference();
    {
        ObjectRoot copied{owner};
        require(runtime.root_count() == 2 && copied.id() == identity, "Copying a root cloned or lost the object.");
        ObjectRoot moved{std::move(copied)};
        require(!copied.registered() && moved.registered() && runtime.root_count() == 2, "Moving a root changed registration count.");
        ObjectRoot assigned;
        assigned = moved;
        require(runtime.root_count() == 3 && assigned.reference() == strong, "Root assignment did not retain the reference.");
        assigned.reset();
        require(runtime.root_count() == 2, "Reset did not remove exactly one root.");
    }
    require(runtime.root_count() == 1 && runtime.live_objects() == 1, "Root destruction destroyed the target.");
    const auto weak = take(runtime.observe(strong));
    require(runtime.root_count() == 1 && weak.description().form == ValueForm::weak_reference,
        "Observation created a strong root.");
    reject(runtime.hold(weak), ErrorCode::type_mismatch, "Weak reference bypassed checked acquisition.");
    reject(runtime.try_acquire(strong), ErrorCode::type_mismatch, "Strong reference entered the weak acquisition interface.");
    {
        auto acquired = take(runtime.try_acquire(weak));
        require(acquired && acquired->registered() && acquired->id() == identity && runtime.root_count() == 2,
            "Weak acquisition returned an unprotected identity.");
    }
    {
        auto held = take(runtime.hold(identity));
        auto empty = take(runtime.hold(take(runtime.null_reference(type))));
        require(held.id() == identity && empty.reference().is_null() && runtime.root_count() == 3,
            "Explicit holding or typed null registration failed.");
        std::vector<ObjectReference> edges;
        take(runtime.visit_roots({&edges, collect_reference}));
        require(edges.size() == 2 && edges[0] == strong && edges[1] == strong, "Null roots were reported as object edges.");
    }
    require(!take(runtime.try_acquire(take(runtime.null_reference(type, ValueForm::weak_reference)))),
        "A null observer acquired an object.");
    reject(runtime.null_reference(type, ValueForm::inline_value), ErrorCode::type_mismatch, "Inline form became an object reference.");
    reject(runtime.null_reference(take(runtime.native_type<float>())), ErrorCode::invalid_type, "Scalar became a reference target.");
    reject(runtime.visit_roots({}), ErrorCode::argument_mismatch, "Missing reference visitor was accepted.");

    Runtime other;
    const auto other_type = take(other.register_type({"roots.Other", TypeKind::object, {}, {}, {}}));
    auto other_root = take(other.create(other_type));
    reject(runtime.hold(other_root.reference()), ErrorCode::invalid_type, "Foreign typed reference was rooted locally.");
    reject(runtime.try_acquire(take(other.observe(other_root.reference()))), ErrorCode::invalid_type,
        "Foreign observer acquired a local root.");

    take(runtime.destroy(identity));
    require(owner.reference() == strong && runtime.root_count() == 1 && RootLifetimeProbe::live == 0,
        "Explicit destruction silently cleared roots or retained the target.");
    require(!take(runtime.try_acquire(weak)), "Destroyed target was acquired through an observer.");
    auto replacement = take(runtime.create(type));
    require(replacement.id() != identity, "Slot reuse resurrected the previous identity.");
    {
        auto stale_copy = take(runtime.hold(strong));
        require(stale_copy.reference() == strong, "Copying a stale strong reference changed identity.");
        reject(runtime.type_of(stale_copy.id()), ErrorCode::invalid_object, "Stale root reached the replacement object.");
        require(!take(runtime.try_acquire(take(runtime.observe(stale_copy.reference())))), "Stale observer reached a reused slot.");
    }
    owner.reset();
    const auto replacement_id = replacement.id();
    replacement.reset();
    require(runtime.root_count() == 0 && runtime.live_objects() == 1, "Releasing the last root performed implicit collection.");
    take(runtime.destroy(replacement_id));

    ObjectRoot survivor;
    ObjectReference surviving_identity;
    {
        Runtime scoped;
        const auto scoped_type = take(NativeRegistration<RootLifetimeProbe>{scoped, "roots.Scoped", TypeKind::object}.publish());
        survivor = take(scoped.create(scoped_type));
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
    std::cout << "[pass] Roots copy and move independently; acquisition protects targets and shutdown detaches handles\n";
}

struct NativeGraphLinks {
    ReferenceSlot visible;
    ReferenceSlot hidden;
};

struct NativeGraphNode {
    NativeGraphLinks links;
    static inline int mirrors{};
};

Result<void> mirror_hidden_reference(CallContext& call) {
    ++NativeGraphNode::mirrors;
    auto& node = *static_cast<NativeGraphNode*>(call.storage);
    const auto target = call.runtime.find_type("graph.A");
    if (!target) return std::unexpected(target.error());
    const auto reference = call.runtime.read_reference({*target, ValueForm::strong_reference}, node.links.visible);
    if (!reference) return std::unexpected(reference.error());
    auto held = call.runtime.hold(*reference);
    if (!held) return std::unexpected(held.error());
    return call.runtime.write_reference({*target, ValueForm::weak_reference}, node.links.hidden, held->reference());
}

void verify_reference_graph() {
    Runtime runtime;
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    std::vector<TypeDefinition> definitions;
    definitions.push_back({"graph.A", TypeKind::object,
        {{"next", {TypeReference::native<NativeGraphNode>(), ValueForm::strong_reference}},
         {"observer", {TypeReference::native<NativeGraphNode>(), ValueForm::weak_reference}},
         {"spare", {TypeReference::native<NativeGraphNode>(), ValueForm::strong_reference}}, {"count", {u32}}}, {}, {}});
    auto native_node = NativeRegistration<NativeGraphNode>{runtime, "graph.B", TypeKind::object}
        .field<&NativeGraphNode::links>("links").build();
    native_node.methods.push_back({"mirror_hidden", {}, mirror_hidden_reference});
    definitions.push_back(std::move(native_node));
    // Hidden first: public FieldId indices must not assume an all-public layout.
    definitions.push_back(NativeRegistration<NativeGraphLinks>{runtime, "graph.NativeLinks", TypeKind::record}
        .hidden_field<&NativeGraphLinks::hidden>("hidden", {TypeReference{"graph.A"}, ValueForm::weak_reference})
        .field<&NativeGraphLinks::visible>("visible", {TypeReference{"graph.A"}, ValueForm::strong_reference}).build());
    definitions.push_back({"graph.Value", TypeKind::record,
        {{"count", {u32}}, {"links", {TypeReference::native<NativeGraphLinks>()}},
         {"keep", {TypeReference{"graph.A"}, ValueForm::strong_reference}},
         {"watch", {TypeReference{"graph.B"}, ValueForm::weak_reference}}}, {}, {}});
    definitions.push_back({"graph.FallibleValue", TypeKind::record,
        {{"keep", {TypeReference{"graph.A"}, ValueForm::strong_reference}},
         {"resource", {TypeReference::native<ValueResource>()}}}, {}, {}});
    definitions.push_back(NativeRegistration<ValueResource>{runtime, "graph.Resource", TypeKind::record}.build());
    const auto ids = take(runtime.register_types(std::move(definitions)));
    const auto next = take(runtime.find_field(ids[0], "next"));
    const auto observer = take(runtime.find_field(ids[0], "observer"));
    const auto spare = take(runtime.find_field(ids[0], "spare"));
    const auto count = take(runtime.find_field(ids[0], "count"));
    const auto visible = take(runtime.find_field(ids[2], "visible"));
    const std::array native_path{take(runtime.find_field(ids[1], "links")), visible};
    const std::array value_path{take(runtime.find_field(ids[3], "links")), visible};
    const std::array keep_path{take(runtime.find_field(ids[3], "keep"))};
    const std::array watch_path{take(runtime.find_field(ids[3], "watch"))};
    const auto mirror = take(runtime.find_method(ids[1], "mirror_hidden"));
    reject(runtime.find_field(ids[2], "hidden"), ErrorCode::invalid_member, "Hidden native field was exposed for dynamic access.");
    require(take(runtime.describe(ids[2]))->fields.size() == 1, "Hidden field leaked into public metadata.");

    auto a = take(runtime.create(ids[0]));
    auto b = take(runtime.create(ids[1]));
    const auto a_id = a.id();
    const auto b_id = b.id();
    const auto strong_a = a.reference();
    const auto strong_b = b.reference();
    const auto weak_b = take(runtime.observe(strong_b));
    take(runtime.write_reference(a_id, next, strong_b));
    take(runtime.write_reference(a_id, observer, weak_b));
    take(runtime.write_reference(b_id, native_path, strong_a));
    take(runtime.call(b_id, mirror, {}));
    require(runtime.root_count() == 2, "Field edges or native callback temporaries became permanent external roots.");
    std::vector<ObjectReference> edges;
    take(runtime.visit_references(b_id, {&edges, collect_reference}));
    require(edges.size() == 2 && edges[0] == take(runtime.observe(strong_a)) && edges[1] == strong_a,
        "Nested native tracing missed a hidden weak or public strong field.");
    b.reset();
    edges.clear();
    take(runtime.visit_roots({&edges, collect_reference}));
    require(edges.size() == 1 && edges[0] == strong_a && runtime.root_count() == 1,
        "Object fields were mistaken for external roots.");
    reject(runtime.write_reference(a_id, next, strong_a), ErrorCode::type_mismatch, "Reference accepted another target type.");
    reject(runtime.write_reference(a_id, next, weak_b), ErrorCode::type_mismatch, "Weak-to-strong assignment bypassed acquisition.");
    reject(runtime.write_reference(a_id, count, strong_b), ErrorCode::type_mismatch, "Reference replaced an inline scalar.");
    const std::array through_reference{next, native_path[0]};
    reject(runtime.read_reference(a_id, through_reference), ErrorCode::invalid_member, "Field path dereferenced object identity storage.");
    require(take(runtime.read_reference(a_id, next)) == strong_b, "Rejected reference mutation changed the field.");

    alignas(32) std::array<std::byte, 256> source{}, destination{}, failed{};
    const auto value_size = take(runtime.describe(ids[3]))->size;
    require(value_size <= source.size(), "Reference record exceeds acceptance storage.");
    take(runtime.initialize_value(ids[3], source));
    auto source_root = take(runtime.root_value(ids[3], source));
    take(runtime.write_reference(ids[3], source, value_path, strong_a));
    take(runtime.write_reference(ids[3], source, keep_path, strong_a));
    take(runtime.write_reference(ids[3], source, watch_path, strong_b));
    std::uint32_t quantity = 7;
    std::memcpy(source.data(), &quantity, sizeof(quantity));
    take(runtime.copy_construct_value(ids[3], destination, source));
    auto destination_root = take(runtime.root_value(ids[3], destination));
    quantity = 99;
    std::memcpy(source.data(), &quantity, sizeof(quantity));
    take(runtime.write_reference(ids[3], source, keep_path, take(runtime.null_reference(ids[0]))));
    std::memcpy(&quantity, destination.data(), sizeof(quantity));
    require(quantity == 7 && take(runtime.read_reference(ids[3], destination, keep_path)) == strong_a,
        "Copying a reference record aliased value storage or cloned its target.");
    take(runtime.assign_value(ids[3], source, destination));
    edges.clear();
    take(runtime.visit_references(ids[3], source, {&edges, collect_reference}));
    require(edges.size() == 3 && std::ranges::count(edges, strong_a) == 2 && std::ranges::count(edges, weak_b) == 1,
        "Described record copy or assignment lost nested reference forms.");
    reject(runtime.initialize_value(ids[3], source), ErrorCode::busy, "Rooted value was reconstructed in place.");
    reject(runtime.copy_construct_value(ids[3], destination, source), ErrorCode::busy, "Rooted value was copy-constructed twice.");
    reject(runtime.destroy_value(ids[3], source), ErrorCode::busy, "Rooted external storage was destroyed.");
    reject(runtime.root_value(ids[3], std::span{source}.first(value_size - 1)), ErrorCode::invalid_storage,
        "Value root accepted insufficient storage.");
    reject(runtime.root_value(ids[3], std::span{source}.subspan(1)), ErrorCode::invalid_storage,
        "Value root accepted misaligned storage.");

    NativeGraphLinks native_source, native_copy;
    auto native_source_bytes = std::as_writable_bytes(std::span{&native_source, 1});
    auto native_copy_bytes = std::as_writable_bytes(std::span{&native_copy, 1});
    take(runtime.write_reference({ids[0], ValueForm::strong_reference}, native_source.visible, strong_a));
    take(runtime.write_reference({ids[0], ValueForm::weak_reference}, native_source.hidden, strong_a));
    auto native_root = take(runtime.root_value(ids[2], native_source_bytes));
    take(runtime.assign_value(ids[2], native_copy_bytes, native_source_bytes));
    edges.clear();
    take(runtime.visit_references(ids[2], native_copy_bytes, {&edges, collect_reference}));
    require(edges.size() == 2 && edges[0] == take(runtime.observe(strong_a)) && edges[1] == strong_a,
        "Native record assignment lost hidden references.");
    const auto roots_before_move = runtime.root_count();
    ValueRoot moved_value_root{std::move(native_root)};
    require(!native_root.registered() && moved_value_root.registered() && runtime.root_count() == roots_before_move,
        "Moving a value-root handle changed its registration.");

    const auto roots_before_failure = runtime.root_count();
    ValueResource::remaining_constructions = 0;
    reject(runtime.initialize_value(ids[4], failed), ErrorCode::construction_failed, "Reference-record construction failure was not reported.");
    ValueResource::remaining_constructions = -1;
    require(ValueResource::live == 0 && runtime.root_count() == roots_before_failure,
        "Failed reference-record construction leaked roots or native values.");
    take(runtime.initialize_value(ids[4], failed));
    take(runtime.destroy_value(ids[4], failed));

    struct Probe {
        Runtime& runtime;
        ObjectId owner, native;
        FieldId field;
        MethodId method;
        TypeId value_type;
        std::span<std::byte> storage;
        ObjectReference strong, weak;
        std::size_t calls{};
    } probe{runtime, a_id, b_id, next, mirror, ids[3], source, strong_b, weak_b};
    const auto mirrors_before = NativeGraphNode::mirrors;
    take(runtime.visit_references(a_id, {&probe, [](void* context, const ObjectReference&) -> Result<void> {
        auto& p = *static_cast<Probe*>(context);
        ++p.calls;
        reject(p.runtime.destroy(p.owner), ErrorCode::busy, "Traversal allowed object destruction.");
        reject(p.runtime.write_reference(p.owner, p.field, p.strong), ErrorCode::busy, "Traversal allowed reference mutation.");
        reject(p.runtime.hold(p.strong), ErrorCode::busy, "Traversal allowed root insertion.");
        reject(p.runtime.try_acquire(p.weak), ErrorCode::busy, "Traversal allowed weak acquisition.");
        reject(p.runtime.root_value(p.value_type, p.storage), ErrorCode::busy, "Traversal allowed external root insertion.");
        reject(p.runtime.assign_value(p.value_type, p.storage, p.storage), ErrorCode::busy, "Traversal allowed value mutation.");
        reject(p.runtime.call(p.native, p.method, {}), ErrorCode::busy, "Traversal entered a mutating native method.");
        require(take(p.runtime.read_reference(p.owner, p.field)) == p.strong, "Traversal prevented a valid read.");
        return {};
    }}));
    require(probe.calls == 2 && NativeGraphNode::mirrors == mirrors_before, "Traversal guard failed before entering user code.");
    reject(runtime.visit_roots({nullptr, [](void*, const ObjectReference&) -> Result<void> {
        throw std::runtime_error("Deliberate visitor failure.");
    }}), ErrorCode::invocation_failed, "Reference visitor exception escaped its boundary.");
    take(runtime.write_reference(a_id, next, strong_b));

    moved_value_root.reset();
    source_root.reset();
    destination_root.reset();
    take(runtime.destroy_value(ids[3], source));
    take(runtime.destroy_value(ids[3], destination));
    require(runtime.root_count() == 1, "External value roots leaked after storage cleanup.");
    take(runtime.destroy(b_id));
    require(!take(runtime.try_acquire(weak_b)), "Observer acquired an explicitly destroyed native object.");
    auto replacement = take(runtime.create(ids[1]));
    require(replacement.id() != b_id, "Reference graph slot reuse preserved a destroyed identity.");
    const auto stale = take(runtime.read_reference(a_id, next));
    require(!stale.is_null() && stale == strong_b, "Target destruction cleared or rebound the stored reference.");
    take(runtime.write_reference(a_id, spare, stale));
    require(take(runtime.read_reference(a_id, spare)) == stale, "Copying an expired field reference required a live target.");
    take(runtime.write_reference(a_id, next, take(runtime.null_reference(ids[1]))));
    require(take(runtime.read_reference(a_id, next)).is_null(), "Typed null assignment failed.");
    take(runtime.destroy(replacement.id()));
    replacement.reset();
    a.reset();
    edges.clear();
    take(runtime.visit_roots({&edges, collect_reference}));
    require(edges.empty() && runtime.root_count() == 0 && runtime.live_objects() == 1,
        "Unrooting implicitly collected objects or retained hidden field roots.");
    take(runtime.destroy(a_id));
    std::cout << "[pass] Mixed reference graphs trace hidden and external values without treating fields as roots\n";
}

struct CollectionLinks {
    ReferenceSlot first;
    ReferenceSlot second;
    ReferenceSlot observer;
    static inline const void* last_traced{};
};

struct CollectionNode {
    CollectionLinks links;
    const CollectionNode* original{this};
    static inline CollectionNode* last_constructed{};
    static inline int live{};
    static inline int destroyed{};
    static inline bool stable{true};
    static inline Runtime* checking_runtime{};
    static inline std::array<ObjectId, 2> condemned;
    static inline bool death_visible{true};
    static inline bool callbacks_blocked{true};

    CollectionNode() { ++live; last_constructed = this; }
    void check_address() { stable &= this == original; }
    ~CollectionNode() noexcept {
        --live;
        ++destroyed;
        stable &= this == original;
        if (!checking_runtime) return;
        for (const auto object : condemned) {
            const auto result = checking_runtime->type_of(object);
            death_visible &= !result && result.error().code == ErrorCode::invalid_object;
        }
        const auto begin = checking_runtime->begin_collection();
        const auto step = checking_runtime->collect_step({1});
        callbacks_blocked &= !begin && begin.error().code == ErrorCode::busy &&
            !step && step.error().code == ErrorCode::busy;
    }
};

std::vector<TypeId> register_collection_types(Runtime& runtime) {
    auto links = NativeRegistration<CollectionLinks>{runtime, "gc.Links", TypeKind::record}
        .hidden_field<&CollectionLinks::first>("first", {TypeReference{"gc.A"}, ValueForm::strong_reference})
        .field<&CollectionLinks::second>("second", {TypeReference{"gc.A"}, ValueForm::strong_reference})
        .field<&CollectionLinks::observer>("observer", {TypeReference{"gc.A"}, ValueForm::weak_reference}).build();
    // Observe real field visitation so interleaving checks do not assume a fixed step count.
    links.fields[0].native_address = [](void* storage) noexcept -> void* {
        CollectionLinks::last_traced = storage;
        return &static_cast<CollectionLinks*>(storage)->first;
    };
    return take(runtime.register_types({
        {"gc.A", TypeKind::object,
            {{"next", {TypeReference::native<CollectionNode>(), ValueForm::strong_reference}}}, {}, {}},
        NativeRegistration<CollectionNode>{runtime, "gc.B", TypeKind::object}
            .field<&CollectionNode::links>("links").method<&CollectionNode::check_address>("check_address").build(),
        std::move(links)
    }));
}

CollectionProgress step_collection(Runtime& runtime, CollectionBudget budget = {1}) {
    const auto before = runtime.collection_phase();
    const auto progress = take(runtime.collect_step(budget));
    require(progress.work_done <= budget.max_work, "Collection exceeded its work budget.");
    require(before == CollectionPhase::idle || progress.work_done != 0, "An active collection failed to advance.");
    require(progress.phase == runtime.collection_phase() &&
        progress.pending_reclamation == runtime.pending_reclamation(), "Collection progress disagreed with runtime state.");
    return progress;
}

void advance_collection(Runtime& runtime, CollectionPhase phase) {
    std::size_t steps = 0;
    while (runtime.collection_phase() != phase) {
        require(runtime.collection_phase() != CollectionPhase::idle && ++steps < 10000,
            "Collection did not reach the requested phase.");
        step_collection(runtime);
    }
}

std::size_t finish_collection(Runtime& runtime) {
    std::size_t steps = 0, reclaimed = 0;
    while (runtime.collection_phase() != CollectionPhase::idle) {
        require(++steps < 10000, "Collection did not finish within the acceptance work limit.");
        reclaimed += step_collection(runtime).reclaimed;
    }
    require(runtime.pending_reclamation() == 0, "An idle collector retained pending reclamation.");
    return reclaimed;
}

void advance_to_reference(Runtime& runtime, const void* storage) {
    std::size_t steps = 0;
    while (CollectionLinks::last_traced != storage) {
        require((runtime.collection_phase() == CollectionPhase::roots || runtime.collection_phase() == CollectionPhase::mark) &&
            ++steps < 10000, "Collection did not visit the protected reference storage.");
        step_collection(runtime);
    }
}

void verify_collection_cycle() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    const auto next = take(runtime.find_field(ids[0], "next"));
    const auto check_address = take(runtime.find_method(ids[1], "check_address"));
    require(step_collection(runtime, {8}).work_done == 0, "Idle collection performed work.");
    reject(runtime.collect_step({0}), ErrorCode::argument_mismatch, "Zero collection work budget was accepted.");
    reject(runtime.collect_step({1, std::chrono::nanoseconds{-1}}), ErrorCode::argument_mismatch,
        "Negative collection time budget was accepted.");
    take(runtime.begin_collection());
    reject(runtime.begin_collection(), ErrorCode::busy, "An active collection was restarted.");
    reject(runtime.register_type({"gc.DuringCollection", TypeKind::object, {}, {}, {}}), ErrorCode::busy,
        "Type publication invalidated an active collection cursor.");
    require(finish_collection(runtime) == 0, "An empty collection reclaimed objects.");

    auto a = take(runtime.create(ids[0]));
    auto b = take(runtime.create(ids[1]));
    auto* native = CollectionNode::last_constructed;
    const auto a_id = a.id(), b_id = b.id();
    const auto strong = a.reference();
    const auto weak = take(runtime.observe(strong));
    take(runtime.write_reference(a_id, next, b.reference()));
    take(runtime.write_reference({ids[0], ValueForm::strong_reference}, native->links.first, strong));
    b.reset();
    take(runtime.begin_collection());
    step_collection(runtime, {128, std::chrono::nanoseconds{1}});
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 2 && CollectionNode::live == 1,
        "A rooted mixed native/described cycle was collected.");
    take(runtime.call(b_id, check_address, {}));
    require(CollectionNode::stable && take(runtime.try_acquire(weak)).has_value(), "Collection moved a live object or lost its identity.");

    a.reset();
    require(runtime.live_objects() == 2, "Root removal implicitly collected a cycle.");
    CollectionNode::checking_runtime = &runtime;
    CollectionNode::condemned = {a_id, b_id};
    const auto destroyed_before = CollectionNode::destroyed;
    take(runtime.begin_collection());
    advance_collection(runtime, CollectionPhase::sweep);
    require(runtime.live_objects() == 0 && runtime.pending_reclamation() == 2 && CollectionNode::live == 1,
        "Logical death did not precede physical reclamation of the entire cycle.");
    require(!take(runtime.try_acquire(weak)), "A committed-dead observer resurrected an unswept object.");
    reject(runtime.hold(a_id), ErrorCode::invalid_object, "A bare identity resurrected a committed-dead object.");
    auto stale = take(runtime.hold(strong));
    require(stale.reference() == strong, "Logical death erased a stale reference identity.");
    reject(runtime.type_of(b_id), ErrorCode::invalid_object, "A committed-dead peer remained accessible.");
    auto fresh = take(runtime.create(ids[1]));
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 1 && CollectionNode::live == 1 &&
        CollectionNode::destroyed == destroyed_before + 1, "Sweep lost a new object or reclaimed a native object more than once.");
    CollectionNode::checking_runtime = nullptr;
    require(CollectionNode::death_visible && CollectionNode::callbacks_blocked,
        "Native destruction observed a live condemned peer or allowed recursive collection.");

    auto replacement = take(runtime.create(ids[0]));
    require(replacement.id() != a_id && !take(runtime.try_acquire(weak)), "Collection slot reuse resurrected a stale identity.");
    stale.reset();
    fresh.reset();
    replacement.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 0 && CollectionNode::live == 0,
        "Subsequent collection leaked sweep-time allocations.");
    std::cout << "[pass] Incremental collection preserves rooted mixed cycles and commits death before bounded sweeping\n";
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
    // The hidden slot has already been scanned. These targets were not roots at cycle start.
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
    for (std::size_t i = 0; i != 4; ++i) take(runtime.type_of(targets[i].id()));
    take(runtime.type_of(newborn_id));
    take(runtime.type_of(old_native_reference.id()));
    require(!take(runtime.try_acquire(observers[4])), "A weak-only target survived collection.");
    require(take(runtime.read_reference({ids[0], ValueForm::weak_reference}, native->links.observer)) == observers[4],
        "Weak processing erased the expired identity.");
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 2 && runtime.live_objects() == 5,
        "Unrooted mark-time allocations were not reclaimed by the next cycle.");
    copied_root.reset();
    acquired.reset();
    holder.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 5 && runtime.live_objects() == 0 && CollectionNode::live == 0,
        "Late roots or barrier bookkeeping leaked across collection cycles.");
    std::cout << "[pass] Cross-step field writes, late/copy roots, weak acquisition and new objects preserve reachability\n";
}

struct CollectionValue {
    CollectionLinks links;
    ValueResource resource;
};

void verify_collection_value_mutations() {
    Runtime runtime;
    const auto ids = register_collection_types(runtime);
    const auto value_types = take(runtime.register_types({
        NativeRegistration<CollectionValue>{runtime, "gc.Value", TypeKind::record}
            .field<&CollectionValue::links>("links").field<&CollectionValue::resource>("resource").build(),
        NativeRegistration<ValueResource>{runtime, "gc.Resource", TypeKind::record}.build()
    }));
    CollectionValue source, destination;
    CollectionLinks late_value, copy_source;
    const auto source_bytes = std::as_bytes(std::span{&source, 1});
    const auto destination_bytes = std::as_writable_bytes(std::span{&destination, 1});
    const auto late_bytes = std::as_bytes(std::span{&late_value, 1});
    const auto copy_source_bytes = std::as_bytes(std::span{&copy_source, 1});
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
    reject(assignment, ErrorCode::assignment_failed, "The deliberate partial assignment did not fail.");
    require(take(runtime.read_reference(strong, destination.links.first)) == targets[0],
        "Failed native assignment did not preserve its documented partial update.");
    const std::array second_path{take(runtime.find_field(value_types[0], "links")), take(runtime.find_field(ids[2], "second"))};
    take(runtime.write_reference(value_types[0], destination_bytes, second_path, targets[3]));
    auto late_root = take(runtime.root_value(ids[2], late_bytes));
    take(runtime.copy_construct_value(ids[2], copied_storage, copy_source_bytes));
    require(finish_collection(runtime) == 0 && runtime.live_objects() == 4,
        "Partial assignment, external field write, late value root or copy construction lost a target.");
    for (const auto& target : targets) take(runtime.type_of(target.id()));
    destination_root.reset();
    late_root.reset();
    take(runtime.destroy_value(ids[2], copied_storage));
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 4 && runtime.live_objects() == 0,
        "Unregistered external values or an earlier write barrier became permanent roots.");
    std::cout << "[pass] External value roots and successful/partial native value operations preserve new strong edges\n";
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
    const auto partial_target = target.reference();
    take(runtime.write_reference(strong, partial_value.second, partial_target));
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
    require(finish_collection(runtime) == 1 && runtime.live_objects() == 1,
        "A removed queued root retained freed storage or a reused root entry lost its value.");
    reject(runtime.type_of(abandoned_id), ErrorCode::invalid_object, "Removed queued value retained its target.");
    take(runtime.type_of(retained_id));
    reused_root.reset();
    take(runtime.begin_collection());
    require(finish_collection(runtime) == 1 && CollectionNode::live == 0, "Root-queue reuse leaked an object.");

    for (const auto phase : {CollectionPhase::roots, CollectionPhase::mark, CollectionPhase::sweep}) {
        ObjectRoot survivor;
        ObjectReference identity;
        {
            Runtime scoped;
            const auto scoped_ids = register_collection_types(scoped);
            survivor = take(scoped.create(scoped_ids[1]));
            identity = survivor.reference();
            auto doomed = take(scoped.create(scoped_ids[1]));
            doomed.reset();
            take(scoped.begin_collection());
            advance_collection(scoped, phase);
        }
        require(CollectionNode::live == 0 && !survivor.registered() && survivor.reference() == identity,
            "Shutdown during collection leaked objects or left a root attached to the dead runtime.");
        ObjectRoot copy{survivor};
        survivor.reset();
        copy.reset();
    }
    std::cout << "[pass] Paused trace cursors tolerate destruction, root removal/reuse and shutdown in every phase\n";
}

void verify_publication_failures() {
    Runtime runtime;
    Runtime other;
    const auto f32 = take(runtime.native_type<float>());
    const auto* existing = take(runtime.describe(f32));
    const auto batch = [&] {
        return std::vector<TypeDefinition>{
            {"transaction.A", TypeKind::record, {{"value", {TypeReference{"transaction.B"}}}}, {}, {}},
            {"transaction.B", TypeKind::record, {{"number", {f32}}}, {}, {}}
        };
    };
    const auto rejected = [&](std::vector<TypeDefinition> definitions, ErrorCode expected) {
        std::vector<std::string> unpublished;
        for (const auto& definition : definitions) {
            if (!runtime.find_type(definition.name)) unpublished.push_back(definition.name);
        }
        reject(runtime.register_types(std::move(definitions)), expected, "Invalid batch was accepted or reported the wrong error.");
        for (const auto& name : unpublished) {
            reject(runtime.find_type(name), ErrorCode::invalid_type, "Failed batch exposed a partial definition.");
        }
        require(take(runtime.describe(f32)) == existing && existing->name == "core.f32",
            "Failed publication changed existing metadata.");
    };
    rejected({}, ErrorCode::invalid_definition);
    auto definitions = batch();
    definitions[1].name = definitions[0].name;
    rejected(std::move(definitions), ErrorCode::duplicate_name);
    definitions = batch();
    definitions[1].name = "core.f32";
    rejected(std::move(definitions), ErrorCode::duplicate_name);
    for (const auto* missing : {"transaction.Missing", "B"}) {
        definitions = batch();
        definitions[0].fields[0].value.type = TypeReference{std::string{missing}};
        rejected(std::move(definitions), ErrorCode::invalid_definition);
    }
    definitions = batch();
    definitions[0].fields[0].value.type = take(other.native_type<float>());
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[0].fields[0].value.type = TypeReference{};
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[0].fields[0].value.type = TypeReference{"transaction.A"};
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[1].fields[0].value.type = TypeReference{"transaction.A"};
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[1].kind = TypeKind::object;
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    for (const auto form : {ValueForm::strong_reference, ValueForm::weak_reference, static_cast<ValueForm>(99)}) {
        definitions = batch();
        definitions[0].fields[0].value.form = form;
        rejected(std::move(definitions), ErrorCode::invalid_definition);
    }
    definitions = batch();
    definitions[1].fields[0].value.form = ValueForm::strong_reference;
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[0].kind = TypeKind::object;
    definitions[0].methods = {{"bad_parameter", {{{TypeReference{"transaction.B"}}}}, advance_described}};
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[0].kind = TypeKind::object;
    definitions[0].methods = {{"missing_implementation", {}, nullptr}};
    rejected(std::move(definitions), ErrorCode::invalid_definition);
    definitions = batch();
    definitions[1].fields.push_back(definitions[1].fields[0]);
    rejected(std::move(definitions), ErrorCode::duplicate_name);

    rejected({NativeRegistration<NativeMachine>{runtime, "transaction.UnresolvedNative", TypeKind::object}
        .field<&NativeMachine::state>("state").build()}, ErrorCode::invalid_definition);
    rejected({NativeRegistration<NativeMachine>{runtime, "transaction.WrongRepresentation", TypeKind::object}
        .field<&NativeMachine::state>("state", {f32}).build()}, ErrorCode::invalid_definition);
    rejected({NativeRegistration<NativeState>{runtime, "transaction.NativeOne", TypeKind::record}.build(),
        NativeRegistration<NativeState>{runtime, "transaction.NativeTwo", TypeKind::record}.build()}, ErrorCode::invalid_definition);
    reject(runtime.native_type<NativeState>(), ErrorCode::invalid_type, "Failed batch leaked a C++ representation mapping.");

    std::vector<TypeDefinition> oversized;
    for (int i = 0; i != 65; ++i) {
        const auto next = "oversized.Record" + std::to_string(i + 1);
        oversized.push_back({"oversized.Record" + std::to_string(i), TypeKind::record,
            i == 64 ? std::vector<FieldDefinition>{{"leaf", {f32}}}
                    : std::vector<FieldDefinition>{{"left", {TypeReference{next}}}, {"right", {TypeReference{next}}}}, {}, {}});
    }
    rejected(std::move(oversized), ErrorCode::invalid_definition);
    const auto ids = take(runtime.register_types(batch()));
    require(take(runtime.describe(ids[0]))->fields[0].value.type == ids[1],
        "Corrected batch could not reuse names after failed publications.");
    const auto native = take(NativeRegistration<NativeState>{runtime, "transaction.NativeOne", TypeKind::record}.publish());
    rejected({NativeRegistration<NativeState>{runtime, "transaction.AlreadyPublishedNative", TypeKind::record}.build()},
        ErrorCode::invalid_definition);
    require(take(runtime.native_type<NativeState>()) == native, "Rejected duplicate replaced the published native type.");
    std::cout << "[pass] Failed batches roll back names, identities and layouts; corrected definitions can be retried\n";
}

struct LocatorGuardValue {
    std::uint32_t number{41};
    ReferenceSlot target;
    static inline Runtime* runtime{};
    static inline TypeId extra_type;
    static inline ObjectId other;
    static inline bool blocked{true};
    static inline std::size_t calls{};
    static void check() noexcept {
        const auto busy = [](const auto& result) { return !result && result.error().code == ErrorCode::busy; };
        ++calls;
        blocked &= busy(runtime->create(extra_type));
        blocked &= busy(runtime->destroy(other));
        blocked &= busy(runtime->begin_collection());
        blocked &= busy(runtime->collect_step({1}));
    }
};

struct CancelLeaf { ReferenceSlot target; };
struct CancelOuter { CancelLeaf inner; };
struct CancelControl {
    static inline ValueRoot* root{};
    static inline bool cancel_outer{}, cancel_leaf{}, canceled{};
    static inline std::size_t late_accesses{};
    static void cancel() noexcept { root->reset(); canceled = true; }
};

void verify_locator_boundaries() {
    {
        Runtime runtime;
        LocatorGuardValue::runtime = &runtime;
        LocatorGuardValue::blocked = true;
        LocatorGuardValue::calls = 0;
        LocatorGuardValue::extra_type = take(runtime.register_type({"locator.Extra", TypeKind::object, {}, {}, {}}));
        auto definition = NativeRegistration<LocatorGuardValue>{runtime, "locator.Object", TypeKind::object}
            .field<&LocatorGuardValue::number>("number")
            .field<&LocatorGuardValue::target>("target", {LocatorGuardValue::extra_type, ValueForm::strong_reference}).build();
        definition.fields[0].native_address = [](void* storage) noexcept -> void* {
            LocatorGuardValue::check();
            return &static_cast<LocatorGuardValue*>(storage)->number;
        };
        definition.fields[1].native_address = [](void* storage) noexcept -> void* {
            LocatorGuardValue::check();
            return &static_cast<LocatorGuardValue*>(storage)->target;
        };
        const auto type = take(runtime.register_type(std::move(definition)));
        auto receiver = take(runtime.create(type));
        auto other = take(runtime.create(LocatorGuardValue::extra_type));
        LocatorGuardValue::other = other.id();
        const auto number = take(runtime.find_field(type, "number"));
        const auto target = take(runtime.find_field(type, "target"));
        require(std::get<std::uint32_t>(take(runtime.read(receiver.id(), number))) == 41, "Locator read failed.");
        take(runtime.write(receiver.id(), number, std::uint32_t{73}));
        take(runtime.write_reference(receiver.id(), target, other.reference()));
        require(take(runtime.read_reference(receiver.id(), target)).id() == other.id(), "Locator reference access failed.");
        require(LocatorGuardValue::blocked && LocatorGuardValue::calls == 4 && runtime.live_objects() == 2,
            "A native field locator bypassed callback lifetime protection.");
        LocatorGuardValue::runtime = nullptr;
    }
    for (const bool cancel_inline : {true, false}) {
        Runtime runtime;
        auto leaf = NativeRegistration<CancelLeaf>{runtime, "cancel.Leaf", TypeKind::record}
            .field<&CancelLeaf::target>("target", {TypeReference{"cancel.Target"}, ValueForm::strong_reference}).build();
        leaf.fields[0].native_address = [](void* storage) noexcept -> void* {
            if (CancelControl::canceled) ++CancelControl::late_accesses;
            if (CancelControl::cancel_leaf) {
                CancelControl::cancel_leaf = false;
                CancelControl::cancel();
            }
            return &static_cast<CancelLeaf*>(storage)->target;
        };
        auto outer = NativeRegistration<CancelOuter>{runtime, "cancel.Outer", TypeKind::record}
            .field<&CancelOuter::inner>("inner").build();
        outer.fields[0].native_address = [](void* storage) noexcept -> void* {
            if (CancelControl::cancel_outer) {
                CancelControl::cancel_outer = false;
                CancelControl::cancel();
            }
            return &static_cast<CancelOuter*>(storage)->inner;
        };
        const auto ids = take(runtime.register_types({
            {"cancel.Target", TypeKind::object, {}, {}, {}}, std::move(leaf), std::move(outer)}));
        CancelOuter value;
        auto target = take(runtime.create(ids[0]));
        take(runtime.write_reference({ids[0], ValueForm::strong_reference}, value.inner.target, target.reference()));
        target.reset();
        auto root = cancel_inline
            ? take(runtime.root_value(ids[2], std::as_bytes(std::span{&value, 1})))
            : take(runtime.root_value(ids[1], std::as_bytes(std::span{&value.inner, 1})));
        CancelControl::root = &root;
        CancelControl::cancel_outer = cancel_inline;
        CancelControl::cancel_leaf = !cancel_inline;
        CancelControl::canceled = false;
        CancelControl::late_accesses = 0;
        take(runtime.begin_collection());
        for (int i = 0; !CancelControl::canceled && i != 100; ++i) take(runtime.collect_step({1}));
        require(CancelControl::canceled && !root.registered(), "Locator did not revoke its current root.");
        while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({1}));
        require(CancelControl::late_accesses == 0 && runtime.live_objects() == 0,
            "Cancelled cursor resumed or marked a reference after revoking its borrow.");
        CancelControl::root = nullptr;
    }
    std::cout << "[pass] Native locators reject lifecycle reentry; callback root revocation stops inline and leaf tracing\n";
}

void verify_active_value_roots() {
    Runtime runtime;
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto type = take(runtime.register_type({"roots.History", TypeKind::object, {}, {}, {}}));
    auto object = take(runtime.create(type));
    std::vector<ObjectRoot> history(10000, object);
    history.clear();
    std::array<std::uint32_t, 4> values{};
    const auto bytes = [&](std::size_t index) { return std::as_writable_bytes(std::span{&values[index], 1}); };
    auto first = take(runtime.root_value(u32, bytes(0)));
    auto middle = take(runtime.root_value(u32, bytes(1)));
    auto last = take(runtime.root_value(u32, bytes(2)));
    auto duplicate = take(runtime.root_value(u32, bytes(2)));
    middle.reset();
    auto reused = take(runtime.root_value(u32, bytes(3)));
    first.reset();
    last.reset();
    ValueRoot moved = std::move(reused);
    require(!reused.registered() && moved.registered(), "Value root move lost its registration.");
    reject(runtime.destroy_value(u32, bytes(2)), ErrorCode::busy, "Removing one overlapping root removed its peer.");
    reject(runtime.initialize_value(u32, bytes(3)), ErrorCode::busy, "Moved root no longer protects its storage.");
    reject(runtime.copy_construct_value(u32, bytes(3), bytes(0)), ErrorCode::busy, "Rooted storage accepted reconstruction.");
    duplicate.reset();
    take(runtime.destroy_value(u32, bytes(2)));
    take(runtime.initialize_value(u32, bytes(2)));
    last = std::move(moved);
    auto replacement_object_root = object;
    reject(runtime.destroy_value(u32, bytes(3)), ErrorCode::busy, "Root-slot reuse corrupted active value indexing.");
    last.reset();
    for (std::size_t i = 0; i != values.size(); ++i) take(runtime.destroy_value(u32, bytes(i)));
    require(runtime.root_count() == 2, "Value root removal changed object root registrations.");
    std::cout << "[pass] Active value roots preserve overlap protection across moves, swap removal and historical slot reuse\n";
}

struct ReentrantValue {
    static inline void* context{};
    static inline void (*exercise)(void*){};
    static inline bool inside{}, failed{};
    static inline std::size_t calls{};
    static void run() noexcept {
        if (!exercise || inside) return;
        inside = true;
        ++calls;
        try { exercise(context); } catch (...) { failed = true; }
        inside = false;
    }
    ReentrantValue() { run(); }
    ReentrantValue(const ReentrantValue&) { run(); }
    ReentrantValue& operator=(const ReentrantValue&) { run(); return *this; }
    ~ReentrantValue() noexcept { run(); }
};

void verify_deep_value_walks() {
    {
        Runtime runtime;
        const auto resource = take(NativeRegistration<ValueResource>{runtime, "deep.Resource", TypeKind::record}.publish());
        const auto reentrant = take(NativeRegistration<ReentrantValue>{runtime, "deep.Reentrant", TypeKind::record}.publish());
        const auto u32 = take(runtime.native_type<std::uint32_t>());
        constexpr std::size_t depth = 10000;
        std::vector<TypeDefinition> definitions;
        definitions.reserve(depth + 3);
        for (std::size_t i = 0; i != depth; ++i) {
            const auto next = i + 1 == depth ? "deep.Leaf" : "deep.R" + std::to_string(i + 1);
            definitions.push_back({"deep.R" + std::to_string(i), TypeKind::record, {{"value", {TypeReference{next}}}}, {}, {}});
        }
        definitions.push_back({"deep.Leaf", TypeKind::record,
            {{"number", {u32}}, {"first", {resource}},
             {"strong", {TypeReference{"deep.Target"}, ValueForm::strong_reference}},
             {"weak", {TypeReference{"deep.Target"}, ValueForm::weak_reference}},
             {"last", {resource}}, {"reentrant", {reentrant}}}, {}, {}});
        definitions.push_back({"deep.Target", TypeKind::object, {}, {}, {}});
        definitions.push_back({"deep.Object", TypeKind::object,
            {{"first", {resource}}, {"body", {TypeReference{"deep.R0"}}}, {"last", {resource}}}, {}, {}});
        const auto types = take(runtime.register_types(std::move(definitions)));
        const auto deep = types[0], leaf = types[depth], target_type = types[depth + 1], object_type = types[depth + 2];
        alignas(32) std::array<std::byte, 512> source{}, destination{}, scratch{}, nested_source{}, nested_copy{};
        require(take(runtime.describe(deep))->size <= source.size(), "Deep value fixture storage is too small.");
        struct NestedValues { Runtime* runtime; TypeId type; std::span<std::byte> source, copy; };
        NestedValues nested{&runtime, deep, nested_source, nested_copy};
        ReentrantValue::context = &nested;
        ReentrantValue::calls = 0;
        ReentrantValue::failed = false;
        ReentrantValue::exercise = [](void* context) {
            auto& values = *static_cast<NestedValues*>(context);
            take(values.runtime->initialize_value(values.type, values.source));
            take(values.runtime->copy_construct_value(values.type, values.copy, values.source));
            take(values.runtime->assign_value(values.type, values.source, values.copy));
            take(values.runtime->destroy_value(values.type, values.copy));
            take(values.runtime->destroy_value(values.type, values.source));
        };
        const auto exercise = ReentrantValue::exercise;
        take(runtime.initialize_value(deep, source));
        take(runtime.copy_construct_value(deep, destination, source));
        take(runtime.assign_value(deep, destination, source));
        require(!ReentrantValue::failed && ReentrantValue::calls == 3 && ValueResource::live == 4,
            "Reentrant deep value operations damaged their enclosing walk.");

        ReentrantValue::exercise = nullptr;
        for (const int remaining : {0, 1}) {
            ValueResource::remaining_constructions = remaining;
            reject(runtime.initialize_value(deep, scratch), ErrorCode::construction_failed, "Deep construction failure escaped.");
            require(ValueResource::live == 4, "Deep construction failure leaked resources.");
            ValueResource::remaining_constructions = remaining;
            reject(runtime.copy_construct_value(deep, scratch, source), ErrorCode::construction_failed, "Deep copy failure escaped.");
            require(ValueResource::live == 4, "Deep copy failure leaked resources.");
        }
        ValueResource::remaining_constructions = 3;
        reject(runtime.create(object_type), ErrorCode::construction_failed, "Deep object failure escaped.");
        require(ValueResource::live == 4 && ValueResource::last_destroyed == ValueResource::next_id - 2 &&
            ValueResource::previous_destroyed == ValueResource::next_id - 1,
            "Completed deep siblings did not unwind in reverse construction order.");
        ValueResource::remaining_constructions = -1;
        ReentrantValue::exercise = exercise;

        auto strong = take(runtime.create(target_type));
        auto weak = take(runtime.create(target_type));
        const auto strong_id = strong.id(), weak_id = weak.id();
        const auto strong_field = take(runtime.find_field(leaf, "strong"));
        const auto weak_field = take(runtime.find_field(leaf, "weak"));
        take(runtime.write_reference(leaf, source, {&strong_field, 1}, strong.reference()));
        take(runtime.write_reference(leaf, source, {&weak_field, 1}, take(runtime.observe(weak.reference()))));
        take(runtime.assign_value(deep, destination, source));
        struct NestedTrace { Runtime* runtime; TypeId type; std::span<const std::byte> storage; std::size_t visits{}, nested_visits{}; };
        NestedTrace tracing{&runtime, deep, destination};
        const ReferenceVisitor visitor{&tracing, [](void* context, const ObjectReference& reference) -> Result<void> {
            auto& state = *static_cast<NestedTrace*>(context);
            require(reference.description().form == (state.visits == 0 ? ValueForm::strong_reference : ValueForm::weak_reference),
                "Deep reference traversal changed strong/weak order.");
            if (state.visits++ == 0) {
                take(state.runtime->visit_references(state.type, state.storage, {&state.nested_visits,
                    [](void* nested_context, const ObjectReference&) -> Result<void> {
                        ++*static_cast<std::size_t*>(nested_context); return {};
                    }}));
            }
            return {};
        }};
        take(runtime.visit_references(deep, source, visitor));
        require(tracing.visits == 2 && tracing.nested_visits == 2, "Nested deep reference traversal lost edges.");
        reject(runtime.visit_references(deep, source, {nullptr, [](void*, const ObjectReference&) -> Result<void> {
            throw std::runtime_error("Deliberate deep visitor failure.");
        }}), ErrorCode::invocation_failed, "Deep visitor exception escaped.");
        tracing.visits = tracing.nested_visits = 0;
        take(runtime.visit_references(deep, source, visitor));
        require(tracing.visits == 2, "Failed deep traversal left stale walk frames.");

        auto root = take(runtime.root_value(deep, source));
        strong.reset();
        weak.reset();
        take(runtime.begin_collection());
        take(runtime.assign_value(deep, destination, source));
        while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({127}));
        take(runtime.type_of(strong_id));
        reject(runtime.type_of(weak_id), ErrorCode::invalid_object, "Deep weak edge incorrectly retained its object.");
        tracing.visits = tracing.nested_visits = 0;
        take(runtime.visit_references(deep, source, visitor));
        require(tracing.visits == 2, "Deep traversal discarded a stale weak identity.");
        root.reset();
        take(runtime.begin_collection());
        while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({127}));
        require(runtime.live_objects() == 0, "Deep unrooted graph survived a fresh collection.");
        take(runtime.destroy_value(deep, destination));
        take(runtime.destroy_value(deep, source));
        require(ValueResource::live == 0 && !ReentrantValue::failed, "Deep reentrant destruction leaked resources.");

        auto object = take(runtime.create(object_type));
        take(runtime.destroy(object.id()));
        object.reset();
        auto collected = take(runtime.create(object_type));
        collected.reset();
        take(runtime.begin_collection());
        while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({127}));
        require(ValueResource::live == 0 && !ReentrantValue::failed, "Deep object destruction or collection leaked resources.");
        ReentrantValue::exercise = nullptr;
        ReentrantValue::context = nullptr;
        auto shutdown = take(runtime.create(object_type));
        require(ValueResource::live == 4, "Deep shutdown fixture was not constructed.");
    }
    require(ValueResource::live == 0, "Runtime shutdown failed to destroy a deep object.");
    std::cout << "[pass] 10000-level mixed layouts support reentrant lifetimes, reverse failure cleanup, tracing and collection\n";
}

template<class T>
void verify_scalar_block() {
    Runtime runtime;
    const auto scalar = take(runtime.native_type<T>());
    const auto types = take(runtime.register_types({
        {"scalar.Pair", TypeKind::record, {{"left", {scalar}}, {"right", {scalar}}}, {}, {}},
        {"scalar.Block", TypeKind::record, {{"first", {TypeReference{"scalar.Pair"}}},
            {"second", {TypeReference{"scalar.Pair"}}}, {"third", {TypeReference{"scalar.Pair"}}}}, {}, {}}
    }));
    const auto type = types[1];
    const auto size = take(runtime.describe(type))->size;
    require(size == 6 * sizeof(T), "Scalar block unexpectedly contains padding.");
    alignas(T) std::array<std::byte, 128> source, destination;
    source.fill(std::byte{0x5a});
    destination.fill(std::byte{0x6b});
    take(runtime.initialize_value(type, source));
    for (std::size_t i = 0; i != 6; ++i) {
        T value;
        std::memcpy(&value, source.data() + i * sizeof(T), sizeof(T));
        require(value == T{}, "Scalar block initialization did not establish zero values.");
        value = static_cast<T>(i * 17 + 3);
        std::memcpy(source.data() + i * sizeof(T), &value, sizeof(T));
    }
    take(runtime.copy_construct_value(type, destination, source));
    require(std::memcmp(source.data(), destination.data(), size) == 0, "Scalar block copy lost values.");
    const T changed = static_cast<T>(93);
    std::memcpy(source.data(), &changed, sizeof(T));
    require(std::memcmp(source.data(), destination.data(), sizeof(T)) != 0, "Scalar block copy aliases its source.");
    take(runtime.assign_value(type, destination, source));
    take(runtime.assign_value(type, source, source));
    require(std::memcmp(source.data(), destination.data(), size) == 0, "Scalar block assignment lost values.");
    reject(runtime.assign_value(type, std::span{source}.subspan(sizeof(T)), source), ErrorCode::invalid_storage,
        "Scalar block fast path accepted partial overlap.");
    require(std::ranges::all_of(std::span{source}.subspan(size), [](auto byte) { return byte == std::byte{0x5a}; }) &&
        std::ranges::all_of(std::span{destination}.subspan(size), [](auto byte) { return byte == std::byte{0x6b}; }),
        "Scalar block fast path touched bytes beyond its type size.");
    take(runtime.destroy_value(type, destination));
    take(runtime.destroy_value(type, source));
}

struct ScalarCallbacks {
    std::uint32_t value{};
    static inline std::array<int, 4> calls{};
};

void verify_scalar_plans() {
    verify_scalar_block<float>();
    verify_scalar_block<std::uint32_t>();
    Runtime runtime;
    ScalarCallbacks::calls = {};
    auto native = NativeRegistration<ScalarCallbacks>{runtime, "scalar.Callbacks", TypeKind::record}
        .field<&ScalarCallbacks::value>("value").build();
    native.native.construct = [](void* address) {
        ++ScalarCallbacks::calls[0];
        std::construct_at(static_cast<ScalarCallbacks*>(address), ScalarCallbacks{99});
    };
    native.native.copy_construct = [](void* address, const void* source) {
        ++ScalarCallbacks::calls[1];
        std::construct_at(static_cast<ScalarCallbacks*>(address), ScalarCallbacks{static_cast<const ScalarCallbacks*>(source)->value + 2});
    };
    native.native.copy_assign = [](void* address, const void* source) {
        ++ScalarCallbacks::calls[2];
        static_cast<ScalarCallbacks*>(address)->value = static_cast<const ScalarCallbacks*>(source)->value + 1;
    };
    native.native.destroy = [](void* address) noexcept {
        ++ScalarCallbacks::calls[3];
        std::destroy_at(static_cast<ScalarCallbacks*>(address));
    };
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    const auto ids = take(runtime.register_types({std::move(native), {"scalar.Opaque", TypeKind::record,
        {{"callback", {TypeReference{"scalar.Callbacks"}}}, {"number", {u32}}}, {}, {}}}));
    alignas(ScalarCallbacks) std::array<std::byte, 32> source{}, destination{};
    take(runtime.initialize_value(ids[1], source));
    take(runtime.copy_construct_value(ids[1], destination, source));
    require(reinterpret_cast<const ScalarCallbacks*>(destination.data())->value == 101, "Custom copy callback was replaced by a byte copy.");
    take(runtime.assign_value(ids[1], destination, source));
    require(reinterpret_cast<const ScalarCallbacks*>(destination.data())->value == 100, "Custom assignment callback was skipped.");
    take(runtime.destroy_value(ids[1], destination));
    take(runtime.destroy_value(ids[1], source));
    require(ScalarCallbacks::calls == std::array<int, 4>{1, 1, 1, 2}, "Scalar-shaped native callbacks were elided or duplicated.");
    std::cout << "[pass] Homogeneous scalar blocks preserve values and bounds without bypassing opaque native callbacks\n";
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
    AddressSensitiveRecord::address_calls = 0;
    Runtime runtime;
    const auto types = take(runtime.register_types({
        {"address.Target", TypeKind::object, {}, {}, {}},
        NativeRegistration<AddressSensitiveRecord>{runtime, "address.Record", TypeKind::record}
            .field<&AddressSensitiveRecord::number>("number")
            .field<&AddressSensitiveRecord::target>("target",
                {TypeReference{"address.Target"}, ValueForm::strong_reference}).build(),
        NativeRegistration<AddressSensitiveOwner>{runtime, "address.Owner", TypeKind::object}
            .field<&AddressSensitiveOwner::inner>("inner").build()
    }));
    auto owner = take(runtime.create(types[2]));
    auto* actual = AddressSensitiveOwner::latest;
    const std::array number_path{take(runtime.find_field(types[2], "inner")),
        take(runtime.find_field(types[1], "number"))};
    require(std::get<std::uint32_t>(take(runtime.read(owner.id(), number_path))) == 17,
        "Default native locator used overloaded operator& instead of the actual member.");
    take(runtime.write(owner.id(), number_path, std::uint32_t{73}));
    require(actual->inner.number == 73 && sentinel.number == 91,
        "Native field write changed an unrelated instance.");

    auto target = take(runtime.create(types[0]));
    const auto target_id = target.id();
    take(runtime.write_reference({types[0], ValueForm::strong_reference},
        actual->inner.target, target.reference()));
    struct Edges { ObjectId expected; std::size_t count{}; } edges{target_id};
    take(runtime.visit_references(owner.id(), {&edges,
        [](void* context, const ObjectReference& reference) -> Result<void> {
            auto& visited = *static_cast<Edges*>(context);
            require(reference.id() == visited.expected, "Native locator enumerated the wrong reference.");
            ++visited.count;
            return {};
        }}));
    require(edges.count == 1, "Native member's strong edge was not enumerated exactly once.");
    target.reset();
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(runtime.type_of(target_id).has_value() && runtime.live_objects() == 2,
        "Native member's strong edge did not retain its target through collection.");
    owner.reset();
    take(runtime.begin_collection());
    finish_collection(runtime);
    require(runtime.live_objects() == 0 && runtime.root_count() == 0 &&
            AddressSensitiveRecord::address_calls == 0,
        "Native member access/trace invoked overloaded operator& or leaked roots/objects.");
    AddressSensitiveRecord::alternate = nullptr;
    AddressSensitiveOwner::latest = nullptr;
    std::cout << "[pass] Native member locators bypass overloaded operator& for field access and GC strong edges\n";
}

}

void verify_runtime_inheritance();
void verify_reference_sequences();

int main() {
    try {
        atelier::acceptance::verify_default_allocator();
        verify_runtime();
        verify_signature_publication();
        verify_method_results();
        verify_forward_publication();
        verify_reference_definitions();
        verify_root_handles();
        verify_reference_graph();
        verify_publication_failures();
        verify_value_operations();
        verify_collection_cycle();
        verify_collection_mutations();
        verify_collection_value_mutations();
        verify_collection_cursor_lifetimes();
        verify_locator_boundaries();
        verify_active_value_roots();
        verify_deep_value_walks();
        verify_scalar_plans();
        verify_native_member_address();
        verify_runtime_inheritance();
        verify_reference_sequences();
        std::cout << "Runtime acceptance: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Runtime acceptance: FAIL: " << error.what() << '\n';
        return 1;
    }
}
