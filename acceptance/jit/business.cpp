#include "business.hpp"
#include "jit/source_backend.hpp"
#include "source/program.hpp"
#include "source/detail/bridge.hpp"
#include "source_backend_verification.hpp"
#include "native_client.hpp"
#include "machine.hpp"
#include "runtime/execution.hpp"
#include "runtime/native.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace atelier::jit_acceptance {
namespace {
using atelier::acceptance::NativeMachine;
using atelier::acceptance::NativeState;
namespace source_detail = atelier::source::detail;
using namespace source_detail;
using BusinessProgram = atelier::source::Program;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T take(Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) {
    if (!value) throw std::runtime_error(value.error().message);
}
template<class T> void reject(const Result<T>& result, ErrorCode code, const char* message) {
    require(!result && result.error().code == code, message);
}

std::span<const FieldId> fixture_field(const Binding& binding, std::string_view name) {
    for (const auto& field : binding.fields) if (field.name == name) return field.path;
    throw std::runtime_error("Fixture field was not exposed by the Runtime.");
}
MethodId fixture_method(const Binding& binding, std::string_view name) {
    for (const auto& method : binding.methods) if (method.name == name) return method.id;
    throw std::runtime_error("Fixture method was not exposed by the Runtime.");
}

std::array<BusinessValue, 2> machine_arguments(float amount, std::uint32_t steps) { return {scalar(amount), scalar(steps)}; }
std::array<TypeId, 2> register_machines(Runtime& runtime);

const std::string& business_source() {
    static const std::string source = [] {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length != 0 && length < executable.size(), "Cannot locate the business source beside the executable.");
        const auto path = std::filesystem::path(executable.data()).parent_path() / "advance_machine.source";
        std::ifstream input(path, std::ios::binary);
        require(input.is_open(), "Cannot open advance_machine.source beside the executable.");
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>{});
        require(!input.bad(), "Failed to read advance_machine.source.");
        return text;
    }();
    return source;
}
CodeOwner compile_bound(const Binding& binding, std::string_view source, const char* name,
    const BusinessServices& callbacks = services) {
    require(source.size() <= std::numeric_limits<std::uint32_t>::max(), "Business source exceeds the compiler interface limit.");
    const EnvironmentView storage(binding);
    const std::string type_name(binding.name);
    const auto environment = storage.view(type_name.c_str());
    BusinessCode* output{};
    char diagnostic[1024]{};
    if (atelier_compile_business(&callbacks, name, source.data(), static_cast<std::uint32_t>(source.size()),
            &environment, &output, diagnostic, sizeof(diagnostic)) != 0)
        throw std::runtime_error(diagnostic);
    return CodeOwner{output, &atelier_destroy_business};
}
// Older machine scenarios share this fixture catalog, not a compiler builtin.
CodeOwner compile_business(std::string_view source = business_source(), const char* name = "advance_machine.source") {
    Runtime runtime;
    auto binding = bind_type(runtime, register_machines(runtime)[0]);
    binding.name = {};
    return compile_bound(binding, source, name);
}

std::string reference_source(std::string_view source, std::string_view type_name) {
    std::string text(source);
    for (std::size_t offset = 0; (offset = text.find("$T", offset)) != std::string::npos; offset += type_name.size())
        text.replace(offset, 2, type_name);
    return text;
}
CodeOwner compile_reference(const Binding& binding, std::string_view source, const char* name) {
    return compile_bound(binding, reference_source(source, binding.name), name);
}

#include "source_fixture.inl"

struct NativeAction {
    void* context;
    void (*run)(void*, CallContext&);
};
thread_local NativeAction* native_action{};
struct ActionScope {
    NativeAction* previous;
    explicit ActionScope(NativeAction& action) : previous(std::exchange(native_action, &action)) {}
    ~ActionScope() { native_action = previous; }
};

Result<void> advance_native(CallContext& call) {
    static_cast<NativeMachine*>(call.storage)->advance(call.arguments[0].get<float>());
    if (native_action) native_action->run(native_action->context, call);
    return {};
}
Result<void> advance_described(CallContext& call) {
    // Preserve the existing fixture's dynamic callback. Its name lookup is not
    // attributed to the new, cold-bound JIT bridge or claimed to be optimized.
    auto type = call.runtime.type_of(call.self);
    if (!type) return std::unexpected(std::move(type.error()));
    auto field = call.runtime.find_field(*type, "progress");
    if (!field) return std::unexpected(std::move(field.error()));
    auto current = call.runtime.read(call.self, *field);
    if (!current) return std::unexpected(std::move(current.error()));
    auto written = call.runtime.write(call.self, *field,
        std::get<float>(*current) + call.arguments[0].get<float>());
    if (!written) return written;
    if (native_action) native_action->run(native_action->context, call);
    return {};
}

std::array<TypeId, 2> register_machines(Runtime& runtime) {
    const auto f32 = take(runtime.native_type<float>());
    const auto u32 = take(runtime.native_type<std::uint32_t>());
    take(NativeRegistration<NativeState>{runtime, "business.NativeState", TypeKind::record}
        .field<&NativeState::cycles>("cycles").field<&NativeState::heat>("heat").publish());
    auto native = NativeRegistration<NativeMachine>{runtime, "business.NativeMachine", TypeKind::object}
        .field<&NativeMachine::state>("state").field<&NativeMachine::progress>("progress")
        .method<&NativeMachine::advance>("advance").build();
    native.methods[0].invoke = advance_native;
    const auto native_type = take(runtime.register_type(std::move(native)));
    TypeDefinition state;
    state.name = "business.State";
    state.kind = TypeKind::record;
    state.fields = {{"heat", {f32}}, {"cycles", {u32}}};
    const auto state_type = take(runtime.register_type(std::move(state)));
    TypeDefinition machine;
    machine.name = "business.DescribedMachine";
    machine.fields = {{"state", {state_type}}, {"progress", {f32}}};
    machine.methods = {{"advance", {{{f32}}, std::nullopt, std::nullopt}, advance_described}};
    return {native_type, take(runtime.register_type(std::move(machine)))};
}

void set_machine(Runtime& runtime, const Binding& binding, ObjectId object,
                 float progress, float heat, std::uint32_t cycles) {
    take(runtime.write(object, fixture_field(binding, "progress"), progress));
    take(runtime.write(object, fixture_field(binding, "state.heat"), heat));
    take(runtime.write(object, fixture_field(binding, "state.cycles"), cycles));
}
void check_machine(Runtime& runtime, const Binding& binding, ObjectId object,
                   float progress, float heat, std::uint32_t cycles) {
    require(std::get<float>(take(runtime.read(object, fixture_field(binding, "progress")))) == progress &&
            std::get<float>(take(runtime.read(object, fixture_field(binding, "state.heat")))) == heat &&
            std::get<std::uint32_t>(take(runtime.read(object, fixture_field(binding, "state.cycles")))) == cycles,
            "Generated business state or snapshot is incorrect.");
}
void collect(Runtime& runtime) {
    if (runtime.collection_phase() != CollectionPhase::idle)
        while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({1}));
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle) take(runtime.collect_step({1}));
}

struct RootObservation {
    ObjectId snapshot;
    ObjectId garbage;
    bool observed{}, completed{}, request_fresh_cycle{}, fresh_requested{};
};
void observe_roots(void* data, Invocation& invocation, const CollectionProgress& progress) {
    auto& observation = *static_cast<RootObservation*>(data);
    const auto snapshot = fixture(invocation).snapshot.id();
    if (!observation.observed) {
        observation.snapshot = snapshot;
        observation.observed = true;
    }
    require(invocation.runtime.root_count() == 3,
            "Snapshot survival is hidden by an unexpected host root.");
    take(invocation.runtime.type_of(snapshot));
    if (progress.phase == CollectionPhase::idle && progress.work_done != 0) {
        if (observation.request_fresh_cycle && !observation.fresh_requested) {
            observation.fresh_requested = true;
            invocation.frame.request_collection();
        } else {
            reject(invocation.runtime.type_of(observation.garbage), ErrorCode::invalid_object,
                   "Unrooted control object survived a completed collection.");
            observation.completed = true;
        }
    }
}

void verify_business_state(BusinessCode* code) {
    Runtime runtime;
    const auto types = register_machines(runtime);
    for (const auto type : types) {
        const auto binding = bind_type(runtime, type);
        auto receiver = take(runtime.create(type));
        for (const auto amount : {0.25f, 0.0f, -0.25f}) {
            set_machine(runtime, binding, receiver.id(), 6.0f, 3.5f, 8);
            auto garbage = take(runtime.create(type));
            RootObservation observation;
            observation.garbage = garbage.id();
            garbage.reset();
            ExecutionStats stats;
            auto snapshot = take(execute_machine(runtime, binding, code, receiver.id(), amount, 128,
                true, &stats, observe_roots, &observation));
            require(stats.explicit_cleanup && observation.observed && observation.completed &&
                    stats.completed_cycles == 1 && stats.reclaimed != 0,
                    "Generated safe points did not complete the root-survival scenario.");
            require(snapshot.id() == observation.snapshot && snapshot.id() != receiver.id() && runtime.root_count() == 2,
                    "Returned snapshot did not transfer its original execution root.");
            const auto expected_progress = amount > 0 ? 38.0f : 6.0f;
            const auto expected_cycles = amount > 0 ? 136u : 8u;
            check_machine(runtime, binding, receiver.id(), expected_progress, 3.5f, expected_cycles);
            check_machine(runtime, binding, snapshot.id(), expected_progress, 3.5f, expected_cycles);
            collect(runtime);
            check_machine(runtime, binding, snapshot.id(), expected_progress, 3.5f, expected_cycles);
            const auto retired = snapshot.id();
            snapshot.reset();
            collect(runtime);
            reject(runtime.type_of(retired), ErrorCode::invalid_object, "Returned snapshot leaked after its host root was released.");
        }
        set_machine(runtime, binding, receiver.id(), 6.0f, 3.5f, std::numeric_limits<std::uint32_t>::max() - 1);
        auto wrapped = take(execute_machine(runtime, binding, code, receiver.id(), 0.5f, 4));
        check_machine(runtime, binding, receiver.id(), 8.0f, 3.5f, 2);
        check_machine(runtime, binding, wrapped.id(), 8.0f, 3.5f, 2);
        wrapped.reset();
        auto unchanged = take(execute_machine(runtime, binding, code, receiver.id(), 1.0f, 0));
        check_machine(runtime, binding, unchanged.id(), 8.0f, 3.5f, 2);
        unchanged.reset();
        receiver.reset();
        collect(runtime);
        require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Business iteration left owned objects or roots.");
    }
    std::puts("[pass] Same generated business advances native/described machines, branches, wraps cycles and returns rooted snapshots");
    std::puts("[pass] Execution-only snapshot roots survive complete GC cycles; transferred results die after host root release");
}

void verify_paused_collection(BusinessCode* code) {
    Runtime runtime;
    const auto types = register_machines(runtime);
    const auto binding = bind_type(runtime, types[0]);
    auto receiver = take(runtime.create(binding.type));
    auto garbage = take(runtime.create(binding.type));
    RootObservation observation;
    observation.garbage = garbage.id();
    observation.request_fresh_cycle = true;
    garbage.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() == CollectionPhase::roots) take(runtime.collect_step({1}));
    require(runtime.collection_phase() != CollectionPhase::idle, "Could not pause an existing collection.");
    ExecutionStats stats;
    auto snapshot = take(execute_machine(runtime, binding, code, receiver.id(), 0.25f, 128,
        false, &stats, observe_roots, &observation));
    require(stats.completed_cycles == 2 && observation.completed && observation.fresh_requested && stats.explicit_cleanup,
            "Late execution roots did not survive an independently started fresh cycle.");
    check_machine(runtime, binding, snapshot.id(), 32.0f, 0.0f, 128);
    std::puts("[pass] Execution enters paused marking safely and newly added roots survive a later complete collection");
}

void verify_failures_and_admission(BusinessCode* code) {
    Runtime runtime;
    const auto types = register_machines(runtime);
    const auto binding = bind_type(runtime, types[0]);
    auto receiver = take(runtime.create(binding.type));
    unsigned native_calls{};
    NativeAction fail_action{&native_calls, [](void* context, CallContext&) {
        if (++*static_cast<unsigned*>(context) == 2) throw std::runtime_error("Native production operation failed.");
    }};
    ExecutionStats stats;
    {
        ActionScope action(fail_action);
        const auto result = execute_machine(runtime, binding, code, receiver.id(), 0.5f, 8, false, &stats);
        reject(result, ErrorCode::invocation_failed, "Native exception escaped the checked generated execution.");
        require(result.error().message == "Native production operation failed." && stats.explicit_cleanup,
                "Native diagnostic or explicit IR error cleanup was lost.");
    }
    require(runtime.root_count() == 1, "Native failure leaked execution roots.");
    check_machine(runtime, binding, receiver.id(), 1.0f, 0.0f, 1);
    collect(runtime);
    require(runtime.live_objects() == 1, "Failed snapshot was not reclaimable.");
    const auto poll_error = execute_machine(runtime, binding, code, receiver.id(), 0.5f, 8, true, &stats,
        [](void*, Invocation&, const CollectionProgress&) { throw std::runtime_error("Safe-point service failed."); });
    reject(poll_error, ErrorCode::invocation_failed, "Safe-point error did not return through generated cleanup.");
    require(stats.explicit_cleanup && runtime.root_count() == 1, "Safe-point failure leaked roots before RAII fallback.");
    collect(runtime);

    struct Reentry { const Binding* binding; BusinessCode* code; unsigned checked{}; } reentry{&binding, code};
    NativeAction reentry_action{&reentry, [](void* context, CallContext& call) {
        auto& state = *static_cast<Reentry*>(context);
        const auto roots = call.runtime.root_count();
        reject(execute_machine(call.runtime, *state.binding, state.code, call.self, 100.0f, 1),
               ErrorCode::busy, "Generated execution reentered through a native borrow.");
        reject(call.runtime.create(state.binding->type), ErrorCode::busy, "Native borrow allowed object creation.");
        reject(call.runtime.destroy(call.self), ErrorCode::busy, "Native borrow allowed receiver destruction.");
        reject(call.runtime.begin_collection(), ErrorCode::busy, "Native borrow allowed a collection start.");
        reject(call.runtime.collect_step({1}), ErrorCode::busy, "Native borrow allowed a collection step.");
        require(call.runtime.root_count() == roots, "Rejected reentry changed roots.");
        ++state.checked;
    }};
    set_machine(runtime, binding, receiver.id(), 0.0f, 2.0f, 0);
    {
        ActionScope action(reentry_action);
        auto snapshot = take(execute_machine(runtime, binding, code, receiver.id(), 0.25f, 4));
        check_machine(runtime, binding, snapshot.id(), 1.0f, 2.0f, 4);
    }
    require(reentry.checked == 4, "Native admission checks did not execute.");
    take(runtime.check_execution_entry());
    struct Traversal { Runtime* runtime; const Binding* binding; BusinessCode* code; ObjectId receiver; bool checked{}; }
        traversal{&runtime, &binding, code, receiver.id()};
    take(runtime.visit_roots({&traversal, [](void* context, const ObjectReference&) -> Result<void> {
        auto& state = *static_cast<Traversal*>(context);
        reject(execute_machine(*state.runtime, *state.binding, state.code, state.receiver, 100.0f, 1),
               ErrorCode::busy, "Generated execution entered a read-only root traversal.");
        state.checked = true;
        return {};
    }}));
    require(traversal.checked, "Traversal admission check did not run.");
    check_machine(runtime, binding, receiver.id(), 1.0f, 2.0f, 4);
    std::puts("[pass] Native/safe-point errors explicitly clear roots without rolling back completed business effects");
    std::puts("[pass] Native borrowing and root traversal reject generated entry before business side effects");
}

void verify_identity_boundaries(BusinessCode* code) {
    Runtime runtime, foreign;
    const auto types = register_machines(runtime);
    const auto foreign_types = register_machines(foreign);
    const auto binding = bind_type(runtime, types[0]);
    auto receiver = take(runtime.create(binding.type));
    auto wrong_type = take(runtime.create(types[1]));
    auto other = take(foreign.create(foreign_types[0]));
    const auto root_count = runtime.root_count();
    reject(execute_machine(runtime, binding, code, other.id(), 1.0f, 1), ErrorCode::invalid_object,
           "Foreign identity reached generated business code.");
    reject(execute_machine(foreign, binding, code, other.id(), 1.0f, 1), ErrorCode::invalid_type,
           "A business binding crossed Runtime ownership.");
    reject(execute_machine(runtime, binding, code, wrong_type.id(), 1.0f, 1), ErrorCode::type_mismatch,
           "Wrong receiver type reached generated business code.");
    require(runtime.root_count() == root_count, "Rejected identities changed root ownership.");
    struct Replacement { ObjectId old; ObjectRoot root{}; bool replaced{}; } replacement{receiver.id()};
    ExecutionStats stats;
    auto result = execute_machine(runtime, binding, code, receiver.id(), 1.0f, 4, false, &stats,
        [](void* data, Invocation& invocation, const CollectionProgress&) {
            auto& state = *static_cast<Replacement*>(data);
            if (state.replaced) return;
            state.replaced = true;
            take(invocation.runtime.destroy(state.old));
            state.root = take(invocation.runtime.create(invocation.binding.type));
            require(state.root.id() != state.old, "Replacement object reused a stale identity.");
            set_machine(invocation.runtime, invocation.binding, state.root.id(), 99.0f, 7.0f, 25);
        }, &replacement);
    reject(result, ErrorCode::invalid_object, "A root was incorrectly treated as an explicit-destruction lease.");
    require(stats.explicit_cleanup && replacement.replaced, "Stale access skipped explicit cleanup.");
    check_machine(runtime, binding, replacement.root.id(), 99.0f, 7.0f, 25);
    reject(execute_machine(runtime, binding, code, replacement.old, 1.0f, 1), ErrorCode::invalid_object,
           "A stale generation reached the replacement object.");
    std::puts("[pass] Foreign runtimes, wrong types and destroyed/reused identities cannot mutate replacement objects");
}

void verify_frame_protocol() {
    Runtime runtime, foreign;
    const auto types = register_machines(runtime);
    const auto other_types = register_machines(foreign);
    auto receiver = take(runtime.create(types[0]));
    auto other = take(foreign.create(other_types[0]));
    const auto baseline = runtime.root_count();
    ObjectRoot transferred;
    {
        std::array<ObjectRoot, 2> roots;
        ExecutionFrame frame(runtime, roots);
        take(frame.hold(0, receiver.id()));
        reject(frame.hold(0, other.id()), ErrorCode::invalid_object, "Failed root replacement crossed Runtime ownership.");
        require(take(frame.object(0)) == receiver.id() && runtime.root_count() == baseline + 1,
                "Failed replacement discarded the old execution root.");
        reject(frame.create(2, types[0]), ErrorCode::argument_mismatch, "Invalid root slot was accepted.");
        frame.request_collection();
        reject(frame.poll({0}), ErrorCode::argument_mismatch, "Invalid safe-point budget was accepted.");
        require(runtime.collection_phase() == CollectionPhase::idle, "Invalid budget changed collection state.");
        take(frame.create(1, types[0]));
        const auto first = take(frame.object(1));
        take(frame.poll({1}));
        take(frame.clear(1));
        take(frame.create(1, types[0]));
        const auto replacement = take(frame.object(1));
        require(first != replacement, "Root slot replacement returned the old object.");
        while (runtime.collection_phase() != CollectionPhase::idle) take(frame.poll({1}));
        frame.request_collection();
        do { take(frame.poll({1})); } while (runtime.collection_phase() != CollectionPhase::idle);
        reject(runtime.type_of(first), ErrorCode::invalid_object, "Cleared execution root retained its old object.");
        take(runtime.type_of(replacement));
        const auto rooted_count = runtime.root_count();
        transferred = take(frame.take(1));
        require(runtime.root_count() == rooted_count, "Return handoff lost or duplicated a registration.");
        take(frame.clear(0));
        require(frame.empty(), "Explicit frame teardown retained registrations.");
    }
    require(runtime.root_count() == baseline + 1, "Transferred root did not outlive its producer frame.");
    transferred.reset();
    try {
        std::array<ObjectRoot, 1> roots;
        ExecutionFrame frame(runtime, roots);
        take(frame.hold(0, receiver.id()));
        throw std::runtime_error("Host scope exit.");
    } catch (const std::runtime_error&) {}
    require(runtime.root_count() == baseline, "Host exception bypassed frame RAII cleanup.");
    {
        std::array<ObjectRoot, 2> roots;
        ExecutionFrame frame(runtime, roots);
        reject(frame.reference(0), ErrorCode::invalid_object, "Empty typed reference slot was readable.");
        reject(frame.reference(2), ErrorCode::argument_mismatch, "Typed reference accepted an invalid slot.");
        const auto null = take(runtime.null_reference(types[0]));
        take(frame.hold(0, null));
        require(roots[0].registered() && take(frame.reference(0)) == null &&
                take(frame.object(0)) == ObjectId{} && runtime.root_count() == baseline + 1,
                "Typed null lost its target type or root registration.");
        reject(frame.take(0), ErrorCode::invalid_object, "Validated return transfer accepted typed null.");
        reject(frame.hold(0, ObjectId{}), ErrorCode::invalid_object, "Raw identity hold accepted null.");
        require(take(frame.reference(0)) == null, "Rejected raw null replaced the typed null slot.");
        take(frame.hold(0, receiver.reference()));
        const auto previous = take(frame.reference(0));
        const auto rooted_count = runtime.root_count();
        reject(frame.hold(0, other.reference()), ErrorCode::invalid_type, "Typed foreign reference crossed Runtime ownership.");
        reject(frame.hold(0, take(foreign.null_reference(other_types[0]))), ErrorCode::invalid_type,
               "Typed foreign null crossed Runtime ownership.");
        reject(frame.hold(0, take(runtime.observe(receiver.reference()))), ErrorCode::type_mismatch,
               "Typed hold silently promoted an observation.");
        reject(frame.hold(0, take(runtime.null_reference(types[0], ValueForm::weak_reference))), ErrorCode::type_mismatch,
               "Typed hold silently promoted weak null.");
        require(take(frame.reference(0)) == previous && runtime.root_count() == rooted_count,
                "Failed typed replacement changed the old reference or root count.");
        auto doomed = take(runtime.create(types[0]));
        const auto stale = doomed.reference();
        take(runtime.destroy(doomed.id()));
        doomed.reset();
        take(frame.hold(1, stale));
        require(take(frame.reference(1)) == stale && take(frame.object(1)) == stale.id(),
                "Typed hold discarded a stale identity.");
        reject(frame.take(1), ErrorCode::invalid_object, "Validated return transfer accepted a stale reference.");
        reject(frame.hold(1, stale.id()), ErrorCode::invalid_object, "Raw identity hold accepted a stale reference.");
        require(take(frame.reference(1)) == stale && roots[1].registered(),
                "Failed stale take or raw hold removed the previous typed root.");
        take(frame.clear(0));
        take(frame.clear(1));
        require(frame.empty() && runtime.root_count() == baseline, "Typed reference cleanup leaked registrations.");
    }
    std::puts("[pass] Raw and typed frame roots preserve null/stale identities, reject invalid replacement atomically and validate return transfer");
}

void verify_active_code_lifetime(bool fail_after_retirement) {
    auto owner = compile_business();
    Runtime runtime;
    const auto types = register_machines(runtime);
    const auto binding = bind_type(runtime, types[0]);
    auto receiver = take(runtime.create(binding.type));
    struct Retirement { CodeOwner* owner; std::uintptr_t address{}; unsigned calls{}; bool fail{}; }
        retirement{&owner, 0, 0, fail_after_retirement};
    NativeAction action{&retirement, [](void* context, CallContext&) {
        auto& state = *static_cast<Retirement*>(context);
        ++state.calls;
        if (state.address != 0) return;
        StackSnapshot stack;
        require(atelier_jit_native_capture(&stack) != 0, "Could not unwind the real business invocation.");
        for (std::uint32_t i = 0; i < stack.count; ++i) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(stack.frames[i], &memory, sizeof(memory)) != sizeof(memory)) continue;
            const auto protection = memory.Protect & 0xff;
            if (memory.Type == MEM_PRIVATE && memory.State == MEM_COMMIT && protection == PAGE_EXECUTE_READ) {
                state.address = reinterpret_cast<std::uintptr_t>(stack.frames[i]);
                break;
            }
        }
        require(state.address != 0, "Native callback did not unwind across a real generated business frame.");
        state.owner->reset();
        DWORD64 base{};
        require(RtlLookupFunctionEntry(state.address, &base, nullptr) != nullptr,
                "Destroying an active code handle revoked its unwind metadata.");
        if (state.fail) throw std::bad_alloc();
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(action);
        auto result = execute_machine(runtime, binding, owner.get(), receiver.id(), 0.5f, 4, true, &stats);
        if (fail_after_retirement) {
            reject(result, ErrorCode::invocation_failed, "Retired active code did not return its native allocation error.");
        } else snapshot = take(std::move(result));
    }
    require(!owner && retirement.calls == (fail_after_retirement ? 1u : 4u) && stats.explicit_cleanup,
            "Generated business did not preserve code ownership or clean up after retirement.");
    if (fail_after_retirement) {
        check_machine(runtime, binding, receiver.id(), 0.5f, 0.0f, 0);
        require(runtime.root_count() == 1, "Failed retired invocation retained output or execution roots.");
        collect(runtime);
        require(runtime.live_objects() == 1, "Failed retired invocation retained its unrooted snapshot.");
    } else check_machine(runtime, binding, snapshot.id(), 2.0f, 0.0f, 4);
    DWORD64 base{};
    MEMORY_BASIC_INFORMATION memory{};
    require(RtlLookupFunctionEntry(retirement.address, &base, nullptr) == nullptr &&
            VirtualQuery(reinterpret_cast<void*>(retirement.address), &memory, sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_FREE, "Returned business invocation retained its retired code or unwind table.");
    std::puts(fail_after_retirement
        ? "[pass] Native bad_alloc after active owner retirement preserves effects, clears roots and releases code after failure return"
        : "[pass] An active invocation pins code and unwind metadata after callback handle destruction, then releases both");
}

void verify_source_execution() {
    auto changed = compile_business(R"(
        void changed(float delta, uint loops) {
            float gain = delta * 2.0 + 0.5;
            uint index = 0;
            while (index < loops) {
                if (gain > 0.0) {
                    self.advance(gain);
                    self.state.cycles = self.state.cycles + 2;
                } else {
                    self.state.heat = self.state.heat - 1.0;
                }
                index = index + 1;
            }
            self.state.heat = self.state.heat + 3.0;
        }
    )", "changed.source");
    auto early = compile_business(R"(
        void early(float delta, uint loops) {
            uint index = 0;
            while (index < loops) {
                if (index >= 2) { return; }
                { float gain = (delta + 0.5) * 2.0; self.advance(gain); }
                index = index + 1;
            }
            self.state.heat = 99.0;
        }
    )", "early.source");
    auto numbers = compile_business(R"(
        void numbers(float delta, uint loops) {
            uint wrapped = 4294967295 + 1;
            (wrapped) = +wrapped;
            self.state.cycles = wrapped - 1;
            self.progress = -(+delta);
            if (delta != delta) { self.state.heat = 1.0; }
            else if (loops == 0) { self.state.heat = 2.0; }
            else { self.state.heat = 3.0; }
        }
    )", "numbers.source");
    auto nested = compile_business(R"(
        void nested(float delta, uint loops) {
            uint outer = 0;
            while (outer < loops) {
                uint inner = 0;
                while (inner <= 1) {
                    self.advance(delta);
                    inner = inner + 1;
                }
                outer = outer + 1;
            }
            { uint reused = 3; self.state.cycles = reused * loops; }
            { uint reused = 2; self.state.cycles = self.state.cycles + reused; }
        }
    )", "nested.source");
    Runtime runtime;
    const auto types = register_machines(runtime);
    for (const auto type : types) {
        const auto binding = bind_type(runtime, type);
        auto receiver = take(runtime.create(type));
        set_machine(runtime, binding, receiver.id(), 6.0f, 3.5f, 8);
        ExecutionStats stats;
        auto snapshot = take(execute_machine(runtime, binding, changed.get(), receiver.id(), 0.25f, 4, true, &stats));
        check_machine(runtime, binding, receiver.id(), 10.0f, 6.5f, 16);
        check_machine(runtime, binding, snapshot.id(), 10.0f, 6.5f, 16);
        require(stats.explicit_cleanup, "Changed source did not clear its execution roots.");
        set_machine(runtime, binding, receiver.id(), 6.0f, 3.5f, 8);
        snapshot = take(execute_machine(runtime, binding, changed.get(), receiver.id(), -1.0f, 4));
        check_machine(runtime, binding, snapshot.id(), 6.0f, 2.5f, 8);
        set_machine(runtime, binding, receiver.id(), 0.0f, 3.5f, 0);
        snapshot = take(execute_machine(runtime, binding, early.get(), receiver.id(), 0.5f, 8, true, &stats));
        check_machine(runtime, binding, snapshot.id(), 4.0f, 3.5f, 0);
        require(stats.explicit_cleanup, "Early return did not clear its execution roots.");
        snapshot = take(execute_machine(runtime, binding, numbers.get(), receiver.id(), 0.0f, 0));
        check_machine(runtime, binding, snapshot.id(), 0.0f, 2.0f, std::numeric_limits<std::uint32_t>::max());
        require(std::signbit(std::get<float>(take(runtime.read(snapshot.id(), fixture_field(binding, "progress"))))),
            "Float unary minus lost the sign of zero.");
        snapshot = take(execute_machine(runtime, binding, numbers.get(), receiver.id(), std::numeric_limits<float>::quiet_NaN(), 1));
        require(std::isnan(std::get<float>(take(runtime.read(snapshot.id(), fixture_field(binding, "progress"))))) &&
                std::get<float>(take(runtime.read(snapshot.id(), fixture_field(binding, "state.heat")))) == 1.0f,
            "Float NaN arithmetic or comparison used incorrect semantics.");
        set_machine(runtime, binding, receiver.id(), 0.0f, 3.5f, 0);
        snapshot = take(execute_machine(runtime, binding, nested.get(), receiver.id(), 0.5f, 3, true));
        check_machine(runtime, binding, snapshot.id(), 3.0f, 3.5f, 11);
    }
    std::puts("[pass] Source changes drive native and described machines: precedence, branches, scopes, nested loops, early return, wrapping uint and IEEE float");
}

void verify_scalar_reference_storage(Runtime& runtime, const Binding& binding, BusinessCode* code) {
    const auto layout = atelier_business_layout(code);
    require(layout.strong_slots == business_root_slots && layout.weak_slots == 0,
            "Scalar source acquired a reference-local layout.");
    auto receiver = take(runtime.create(binding.type));
    ExecutionStats stats;
    auto snapshot = take(execute_machine(runtime, binding, code, receiver.id(), 1.0f, 0, false, &stats,
        [](void*, Invocation& invocation, const CollectionProgress&) {
            require(invocation.expanded_roots.capacity() == 0 && invocation.observations.capacity() == 0 &&
                    invocation.roots.data() == invocation.inline_roots.data() &&
                    invocation.roots.size() == business_root_slots,
                    "Scalar execution allocated a reference array instead of its original stack roots.");
        }));
    require(stats.polls == 1 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Scalar reference-storage control changed execution or cleanup.");
}

void verify_reference_values(Runtime& runtime, const Binding& binding, const Binding& wrong_binding) {
    auto code = compile_reference(binding, R"(
        void references(float delta, uint loops) {
            $T empty;
            weak $T watched;
            $T alias = self;
            $T copy = alias;
            weak $T observed = observe(alias);
            weak $T observed_copy = observed;
            $T acquired = try_acquire(observed_copy);
            self.state.cycles = 0;
            if (empty == null) { self.state.cycles = self.state.cycles + 1; }
            if (watched == null) { self.state.cycles = self.state.cycles + 1; }
            if (empty == watched) { self.state.cycles = self.state.cycles + 1; }
            if (null == null) { self.state.cycles = self.state.cycles + 1; }
            if (copy == self) { self.state.cycles = self.state.cycles + 1; }
            if (observed == copy) { self.state.cycles = self.state.cycles + 1; }
            if (acquired == observed_copy) { self.state.cycles = self.state.cycles + 1; }
            if (try_acquire(watched) == null) { self.state.cycles = self.state.cycles + 1; }
            alias = alias;
            acquired = null;
            observed_copy = null;
            if (acquired == observed_copy) { self.state.cycles = self.state.cycles + 1; }
            $T fresh = new $T();
            $T fresh_alias = fresh;
            fresh.progress = 2.0;
            fresh_alias.state.heat = 3.0;
            fresh.state.cycles = 4;
            fresh_alias.advance(delta);
            self.progress = fresh.progress;
            self.state.heat = fresh_alias.state.heat;
            self.state.cycles = self.state.cycles + fresh_alias.state.cycles;
        }
    )", "reference-values.source");
    const auto layout = atelier_business_layout(code.get());
    require(layout.object_type_name && std::string_view(layout.object_type_name) == binding.name &&
            layout.strong_slots > business_root_slots && layout.weak_slots != 0,
            "Compiled source did not retain its owned object type name or typed reference layout.");
    auto wrong_receiver = take(runtime.create(wrong_binding.type));
    set_machine(runtime, wrong_binding, wrong_receiver.id(), 9.0f, 8.0f, 7);
    const auto roots = runtime.root_count();
    ExecutionStats stats;
    reject(execute_machine(runtime, wrong_binding, code.get(), wrong_receiver.id(), 0.5f, 0, false, &stats),
           ErrorCode::type_mismatch, "Named source entered code with a different Runtime object type.");
    require(!stats.entered && stats.polls == 0 && runtime.root_count() == roots,
            "Type-name rejection happened after entering code or registering execution roots.");
    check_machine(runtime, wrong_binding, wrong_receiver.id(), 9.0f, 8.0f, 7);
    wrong_receiver.reset();
    collect(runtime);
    auto receiver = take(runtime.create(binding.type));
    auto snapshot = take(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, false, &stats));
    check_machine(runtime, binding, receiver.id(), 2.5f, 3.0f, 13);
    check_machine(runtime, binding, snapshot.id(), 2.5f, 3.0f, 13);
    require(stats.entered && stats.polls == 1 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Reference values changed safe-point count or left aliases registered.");
    collect(runtime);
    require(runtime.live_objects() == 2, "Source-created reference value survived its lexical owner.");
}

void verify_reference_collection(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        void reference_collection(float delta, uint loops) {
            weak $T watched;
            {
                $T held = new $T();
                watched = observe(held);
                held.advance(delta);
                uint turn = 0;
                while (turn < 2) { turn = turn + 1; }
            }
            uint tail = 0;
            while (tail < 1) { tail = tail + 1; }
            if (watched != null) {
                if (try_acquire(watched) == null) { self.state.cycles = 7; }
            }
            self.progress = 10.0;
        }
    )", "reference-collection.source");
    auto receiver = take(runtime.create(binding.type));
    auto garbage = take(runtime.create(binding.type));
    const auto garbage_id = garbage.id();
    garbage.reset();
    struct Observation { ObjectId object; ObjectId garbage; unsigned calls{}, cycles{}; } state{{}, garbage_id};
    NativeAction capture{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Observation*>(context);
        require(++state.calls == 1, "Reference collection unexpectedly called native code twice.");
        state.object = call.self; // Identity only: no host root may hide source ownership.
        require(call.runtime.root_count() == 4, "New-expression temporary survived its declaration.");
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(capture);
        snapshot = take(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, false, &stats,
            [](void* context, Invocation& invocation, const CollectionProgress&) {
                auto& state = *static_cast<Observation*>(context);
                const auto poll = fixture(invocation).stats.polls;
                require(poll <= 4 && invocation.runtime.root_count() == ((poll == 2 || poll == 3) ? 4u : 3u),
                        "Lexical source root or weak observation changed the expected ownership count.");
                collect(invocation.runtime); // Finish any old work, then complete an independent new cycle.
                ++state.cycles;
                reject(invocation.runtime.type_of(state.garbage), ErrorCode::invalid_object,
                       "Fresh source-local collection left its unrooted control alive.");
                if (poll == 2 || poll == 3) {
                    require(state.calls == 1 && state.object != ObjectId{} && invocation.runtime.live_objects() == 3,
                            "Source-only strong reference did not survive a complete fresh cycle.");
                    take(invocation.runtime.type_of(state.object));
                } else {
                    require(invocation.runtime.live_objects() == 2, "Weak-only object was retained after its inner block.");
                    if (poll == 4)
                        reject(invocation.runtime.type_of(state.object), ErrorCode::invalid_object,
                               "Observation kept an out-of-scope source object alive.");
                }
            }, &state));
    }
    require(stats.polls == 4 && state.cycles == 4 && state.calls == 1 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Source-local strong/weak collection skipped a real safe point or explicit cleanup.");
    check_machine(runtime, binding, snapshot.id(), 10.0f, 0.0f, 7);
}

void verify_reference_loop_slots(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        void reference_loop(float delta, uint loops) {
            weak $T previous;
            uint index = 0;
            while (index < loops) {
                {
                    $T current = new $T();
                    weak $T local = observe(current);
                    current.advance(delta);
                    previous = local;
                }
                index = index + 1;
            }
            if (previous != null) {
                if (try_acquire(previous) == null) { self.state.cycles = loops; }
            }
        }
    )", "reference-loop.source");
    auto receiver = take(runtime.create(binding.type));
    struct Iterations { std::array<ObjectId, 4> objects{}; std::size_t calls{}, cycles{}; } state;
    NativeAction capture{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Iterations*>(context);
        require(state.calls < state.objects.size() && call.runtime.root_count() == 4,
                "Repeated declaration retained an old root or expression temporary.");
        for (std::size_t i = 0; i < state.calls; ++i) {
            require(state.objects[i] != call.self, "Reused object storage repeated an earlier generation.");
            reject(call.runtime.type_of(state.objects[i]), ErrorCode::invalid_object,
                   "A previous loop-local object survived into the next iteration.");
        }
        state.objects[state.calls++] = call.self;
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(capture);
        snapshot = take(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 4, false, &stats,
            [](void* context, Invocation& invocation, const CollectionProgress&) {
                auto& state = *static_cast<Iterations*>(context);
                require(fixture(invocation).stats.polls == state.calls + 1 && invocation.runtime.root_count() == 3,
                        "Loop block did not release its source root before the backedge poll.");
                collect(invocation.runtime);
                ++state.cycles;
                require(invocation.runtime.live_objects() == 2, "Reused loop-local slot leaked an object.");
                for (std::size_t i = 0; i < state.calls; ++i)
                    reject(invocation.runtime.type_of(state.objects[i]), ErrorCode::invalid_object,
                           "An old slot identity survived an independent collection.");
                if (state.calls < state.objects.size()) {
                    take(invocation.runtime.begin_collection());
                    take(invocation.runtime.collect_step({1}));
                    require(invocation.runtime.collection_phase() != CollectionPhase::idle,
                            "Slot-reuse scenario did not leave a paused collection for the next iteration.");
                }
            }, &state));
    }
    require(stats.polls == 5 && state.calls == 4 && state.cycles == 5 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Loop slot reuse missed a backedge or explicit final cleanup.");
    check_machine(runtime, binding, snapshot.id(), 0.0f, 0.0f, 4);
}

void verify_reference_exit(Runtime& runtime, const Binding& binding, bool fail_native) {
    auto code = compile_reference(binding, R"(
        void reference_exit(float delta, uint loops) {
            $T held = new $T();
            weak $T watched = observe(held);
            self.progress = 3.0;
            if (watched == held) { self.state.heat = 2.0; }
            held.advance(delta);
            if (loops == 0) { self.state.cycles = 1; return; }
            self.advance(delta);
            self.state.heat = 99.0;
            $T unreached = new $T();
            weak $T not_initialized = observe(unreached);
        }
    )", "reference-exit.source");
    auto receiver = take(runtime.create(binding.type));
    struct Exit { ObjectId held; ObjectId receiver; unsigned calls{}; bool fail{}; } state{{}, receiver.id(), 0, fail_native};
    NativeAction callback{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Exit*>(context);
        require(call.runtime.root_count() == 4, "Early/native-failure scenario lost its initialized source owner.");
        ++state.calls;
        if (state.calls == 1) {
            require(call.self != state.receiver, "Source new expression aliased self.");
            state.held = call.self;
        } else {
            require(state.calls == 2 && state.fail && call.self == state.receiver,
                    "Native-failure scenario executed an unexpected statement.");
            throw std::runtime_error("Reference-local native failure.");
        }
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(callback);
        auto result = execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, fail_native ? 1u : 0u, false, &stats);
        if (fail_native) {
            reject(result, ErrorCode::invocation_failed, "Native failure with live and weak locals escaped generated cleanup.");
            require(result.error().message == "Reference-local native failure.", "Reference-local failure lost its native diagnostic.");
        } else snapshot = take(std::move(result));
    }
    require(state.calls == (fail_native ? 2u : 1u) && stats.polls == 1 && stats.explicit_cleanup &&
            runtime.root_count() == (fail_native ? 1u : 2u),
            "Early/native-failure exit relied on RAII or executed a later source statement.");
    check_machine(runtime, binding, receiver.id(), fail_native ? 3.5f : 3.0f, 2.0f, fail_native ? 0u : 1u);
    if (!fail_native) check_machine(runtime, binding, snapshot.id(), 3.0f, 2.0f, 1);
    collect(runtime);
    reject(runtime.type_of(state.held), ErrorCode::invalid_object, "Early/native-failure cleanup retained its source-created object.");
    require(runtime.live_objects() == (fail_native ? 1u : 2u), "Early/native-failure exit leaked snapshot or unexecuted locals.");
}

void verify_reference_returns(Runtime& runtime, const Binding& binding) {
    constexpr std::uint32_t return_count = 16;
    enum class Failure { none, native, publication, cleanup };
    struct State {
        Failure failure;
        std::uint32_t calls{};
        bool publish_seen{};
    };
    std::string source = "void reference_returns(float amount, uint steps) { $T outer = self; ";
    for (std::uint32_t index = 0; index < return_count; ++index) {
        source += "{ $T held = new $T(); weak $T watched = observe(held); "
                  "uint turn = 0; while (turn < 1) { turn = turn + 1; } "
                  "held.advance(amount); self.state.cycles = self.state.cycles + 1; "
                  "if (steps == " + std::to_string(index) + ") { return; }}";
    }
    source += "self.progress = 7.0; }";
    const std::string type_name(binding.name);
    source = reference_source(source, type_name);
    auto checked_services = services;
    checked_services.finish = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        const auto status = bridge(invocation, [&]() -> Result<void> {
            // Final cleanup after publication would mask a missing early-return
            // cleanup. Inspect actual slots before handing off the snapshot.
            for (std::size_t index = business_root_slots; index < invocation.roots.size(); ++index)
                require(!invocation.roots[index].registered(), "Return kept a user root until snapshot publication.");
            for (const auto& observation : invocation.observations)
                require(observation.description().type == TypeId{}, "Return kept a weak slot until snapshot publication.");
            auto& state = *static_cast<State*>(fixture(invocation).hook_state);
            state.publish_seen = true;
            if (state.failure == Failure::publication)
                return std::unexpected(Error{ErrorCode::invocation_failed, "Reference-return publication failure."});
            return {};
        });
        return status == 0 ? finish(context) : status;
    };
    checked_services.clear = [](void* context, std::uint32_t slot) noexcept {
        clear_slot(context, slot);
        auto& invocation = *static_cast<Invocation*>(context);
        const auto& state = *static_cast<State*>(fixture(invocation).hook_state);
        if (state.failure == Failure::cleanup && state.publish_seen)
            invocation.fail(ErrorCode::invocation_failed, "Reference-return cleanup failure.");
    };
    auto code = compile_bound(binding, source, "reference-returns.source", checked_services);
    for (const auto steps : {0u, return_count / 2, return_count - 1, return_count}) {
        for (const auto failure : {Failure::none, Failure::native, Failure::publication, Failure::cleanup}) {
            State state{failure};
            auto receiver = take(runtime.create(binding.type));
            NativeAction callback{&state, [](void* context, CallContext& call) {
                auto& state = *static_cast<State*>(context);
                ++state.calls;
                require(call.runtime.live_objects() == 3 && call.runtime.root_count() == 5,
                    "Multi-return path changed lexical roots across a complete collection.");
                if (state.failure == Failure::native)
                    throw std::runtime_error("Reference-return native failure.");
            }};
            ExecutionStats stats;
            ObjectRoot snapshot;
            {
                ActionScope action(callback);
                auto result = execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, steps, true, &stats,
                    [](void*, Invocation& invocation, const CollectionProgress&) { collect(invocation.runtime); }, &state);
                if (failure == Failure::none) snapshot = take(std::move(result));
                else {
                    reject(result, ErrorCode::invocation_failed, "Multi-return error escaped generated cleanup.");
                    require(result.error().message == (failure == Failure::native ?
                        "Reference-return native failure." : failure == Failure::publication ?
                        "Reference-return publication failure." : "Failed generated code published an output."),
                        "Multi-return path lost its first failure or failed-output protocol check.");
                    require(!snapshot.registered() && runtime.root_count() == 1,
                        "Failed generated code exposed or retained a snapshot after returning.");
                }
            }
            const auto calls = failure == Failure::native ? 1u : std::min(steps + 1, return_count);
            require(state.calls == calls && stats.polls == calls + 1 && stats.explicit_cleanup &&
                state.publish_seen == (failure != Failure::native),
                "Multi-return path changed calls, polls, publication or explicit cleanup.");
            const auto cycles = failure == Failure::native ? 0u : calls;
            const auto progress = failure != Failure::native && steps == return_count ? 7.0f : 0.0f;
            check_machine(runtime, binding, receiver.id(), progress, 0.0f, cycles);
            if (failure == Failure::none) check_machine(runtime, binding, snapshot.id(), progress, 0.0f, cycles);
            collect(runtime);
            const auto expected_live = failure == Failure::none ? 2u : 1u;
            require(runtime.live_objects() == expected_live && runtime.root_count() == expected_live,
                "Multi-return path retained a source object or unpublished snapshot.");
            snapshot.reset();
            receiver.reset();
            collect(runtime);
            require(runtime.live_objects() == 0 && runtime.root_count() == 0,
                "Multi-return case leaked state into the next invocation.");
        }
    }
}

void verify_stale_source_references(Runtime& runtime, const Binding& binding, bool dereference) {
    auto code = compile_reference(binding, R"(
        void stale_references(float delta, uint loops) {
            $T held = new $T();
            weak $T before = observe(held);
            held.advance(delta);
            uint turn = 0;
            while (turn < 1) { turn = turn + 1; }
            $T alias = held;
            weak $T after = observe(alias);
            weak $T copy = before;
            $T acquired = try_acquire(after);
            self.state.cycles = 0;
            if (alias == held) { self.state.cycles = self.state.cycles + 1; }
            if (after == before) { self.state.cycles = self.state.cycles + 1; }
            if (alias == copy) { self.state.cycles = self.state.cycles + 1; }
            if (alias != null) { self.state.cycles = self.state.cycles + 1; }
            if (before != null) { self.state.cycles = self.state.cycles + 1; }
            if (acquired == null) { self.state.cycles = self.state.cycles + 1; }
            if (acquired != alias) { self.state.cycles = self.state.cycles + 1; }
            self.progress = 4.0;
            if (loops != 0) {
                alias.progress = 88.0;
                self.state.heat = 99.0;
            }
        }
    )", "stale-references.source");
    auto receiver = take(runtime.create(binding.type));
    struct Replacement {
        ObjectId old;
        ObjectId receiver;
        ObjectRoot root;
        unsigned calls{};
        bool destroyed{};
    } state;
    state.receiver = receiver.id();
    NativeAction capture{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Replacement*>(context);
        require(++state.calls == 1 && call.self != state.receiver && call.runtime.root_count() == 4,
                "Stale-reference scenario did not capture a source-only new object.");
        state.old = call.self;
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(capture);
        auto result = execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, dereference ? 1u : 0u, false, &stats,
            [](void* context, Invocation& invocation, const CollectionProgress&) {
                auto& state = *static_cast<Replacement*>(context);
                if (fixture(invocation).stats.polls == 1) {
                    require(!state.destroyed && state.calls == 0 && invocation.runtime.root_count() == 3,
                            "Source-owned destruction ran before initialization.");
                    return;
                }
                require(fixture(invocation).stats.polls == 2 && state.calls == 1 && !state.destroyed &&
                        state.old != state.receiver && invocation.runtime.root_count() == 4,
                        "Source-owned destruction ran at the wrong safe point.");
                take(invocation.runtime.destroy(state.old));
                state.destroyed = true;
                state.root = take(invocation.runtime.create(invocation.binding.type));
                require(state.root.id() != state.old && state.root.id() != state.receiver,
                        "Replacement reused an old identity or replaced self.");
                set_machine(invocation.runtime, invocation.binding, state.root.id(), 99.0f, 7.0f, 25);
                collect(invocation.runtime);
                require(invocation.runtime.root_count() == 5 && invocation.runtime.live_objects() == 3,
                        "Destroyed source root or host-held replacement has incorrect ownership.");
                reject(invocation.runtime.type_of(state.old), ErrorCode::invalid_object,
                       "Explicitly destroyed local resolved to its replacement.");
            }, &state);
        if (dereference)
            reject(result, ErrorCode::invalid_object, "Dereferencing a stale source alias reached replacement storage.");
        else snapshot = take(std::move(result));
    }
    require(state.destroyed && state.calls == 1 && stats.polls == 2 && stats.explicit_cleanup &&
            runtime.root_count() == (dereference ? 2u : 3u),
            "Stale identity copying/comparison failed or its exit leaked execution roots.");
    check_machine(runtime, binding, receiver.id(), 4.0f, 0.0f, 7);
    check_machine(runtime, binding, state.root.id(), 99.0f, 7.0f, 25);
    if (!dereference) check_machine(runtime, binding, snapshot.id(), 4.0f, 0.0f, 7);
    collect(runtime);
    require(runtime.live_objects() == (dereference ? 2u : 3u), "Stale-reference cleanup did not account for the host replacement.");
}

void verify_reference_temporaries(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        void reference_temporaries(float delta, uint loops) {
            try_acquire(observe(new $T())).advance(delta);
            self.advance(0.0);
            if (try_acquire(observe(new $T())) != null) {
                self.advance(0.0);
            } else {
                $T skipped = new $T();
                weak $T skipped_watch = observe(skipped);
                skipped.advance(delta);
            }
            uint first = 0;
            while (first < 1) { first = first + 1; }
            {
                $T held = new $T();
                weak $T watched = observe(held);
                try_acquire(watched).progress = try_acquire(observe(new $T())).progress + delta;
                try_acquire(observe(try_acquire(watched))).advance(delta);
                held = try_acquire(observe(held));
                if (try_acquire(watched) == held) {
                    self.advance(0.0);
                    self.state.cycles = 1;
                }
                self.progress = held.progress;
            }
            uint last = 0;
            while (last < 1) { last = last + 1; }
        }
    )", "reference-temporaries.source");
    auto receiver = take(runtime.create(binding.type));
    struct Temporaries { ObjectId receiver; ObjectId temporary; ObjectId held; unsigned calls{}, cycles{}; }
        state{receiver.id(), {}, {}};
    NativeAction observe{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Temporaries*>(context);
        const auto step = state.calls++;
        require(step < 5, "Unexecuted temporary branch reached native code.");
        const auto expected_roots = step == 0 ? 5u : step == 3 ? 6u : step == 4 ? 4u : 3u;
        require(call.runtime.root_count() == expected_roots,
                "Full-expression roots were cleared early or survived their statement/condition.");
        if (step == 0) {
            require(call.self != state.receiver, "Temporary receiver did not call the new object.");
            state.temporary = call.self;
        } else if (step == 3) {
            require(call.self != state.receiver && call.self != state.temporary,
                    "Nested acquisition reached an old temporary or self.");
            state.held = call.self;
        } else require(call.self == state.receiver, "Temporary condition selected the wrong native receiver.");
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(observe);
        snapshot = take(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, false, &stats,
            [](void* context, Invocation& invocation, const CollectionProgress&) {
                auto& state = *static_cast<Temporaries*>(context);
                const auto step = fixture(invocation).stats.polls;
                require(step <= 3 && state.calls == (step == 1 ? 0u : step == 2 ? 3u : 5u) &&
                        invocation.runtime.root_count() == 3,
                        "Temporary expression or inner scope retained a root at its next safe point.");
                for (const auto& observation : invocation.observations)
                    require(observation.description().type == TypeId{},
                            "A temporary or unexecuted branch was treated as an initialized weak value.");
                collect(invocation.runtime);
                ++state.cycles;
                require(invocation.runtime.live_objects() == 2, "Full-expression temporaries survived independent collection.");
                if (step >= 2)
                    reject(invocation.runtime.type_of(state.temporary), ErrorCode::invalid_object,
                           "Temporary native receiver was retained after its full expression.");
                if (step == 3)
                    reject(invocation.runtime.type_of(state.held), ErrorCode::invalid_object,
                           "Acquired aliases retained their block-local owner.");
            }, &state));
    }
    require(state.calls == 5 && state.cycles == 3 && stats.polls == 3 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Full-expression reference execution skipped callbacks, safe points or explicit cleanup.");
    check_machine(runtime, binding, receiver.id(), 1.0f, 0.0f, 1);
    check_machine(runtime, binding, snapshot.id(), 1.0f, 0.0f, 1);
}

void verify_reference_failed_initializer(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        void failed_initializer(float delta, uint loops) {
            $T live = new $T();
            weak $T watched = observe(live);
            live.advance(delta);
            self.progress = 7.0;
            self.state.cycles = 2;
            $T absent;
            if (loops == 0) {
                float failed = try_acquire(observe(absent)).progress;
                self.progress = 99.0;
            } else {
                $T skipped = new $T();
                weak $T skipped_watch = observe(skipped);
                skipped.advance(delta);
            }
            $T unreached = new $T();
            self.state.heat = 99.0;
        }
    )", "failed-reference-initializer.source");
    auto receiver = take(runtime.create(binding.type));
    struct Failed { ObjectId live; unsigned calls{}; } state;
    NativeAction capture{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Failed*>(context);
        require(++state.calls == 1 && call.runtime.root_count() == 4,
                "Failed-initializer scenario executed an unselected or unreachable initializer.");
        state.live = call.self;
    }};
    ExecutionStats stats;
    {
        ActionScope scope(capture);
        reject(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, false, &stats),
               ErrorCode::invalid_object, "Null temporary field access did not fail the actual initializer.");
    }
    require(state.calls == 1 && stats.polls == 1 && stats.explicit_cleanup && runtime.root_count() == 1,
            "Failed initialization cleared an uninitialized slot as a live value or retained its temporaries.");
    check_machine(runtime, binding, receiver.id(), 7.0f, 0.0f, 2);
    collect(runtime);
    reject(runtime.type_of(state.live), ErrorCode::invalid_object, "Failed initializer retained an earlier live local.");
    require(runtime.live_objects() == 1, "Failed initializer leaked its snapshot or unreachable objects.");
}

void verify_reference_rejections(Runtime& runtime, const Binding& binding, const Binding& other_binding) {
    auto existing = compile_reference(binding, R"(
        void retained_reference_code(float delta, uint loops) {
            $T owner = self;
            owner.advance(delta);
            owner.state.cycles = loops;
        }
    )", "retained-reference.source");
    const char* cases[]{
        "void bad(float a, uint n) { weak $T w = observe(self); float x = w.progress; }",
        "void bad(float a, uint n) { weak $T w = observe(self); w.advance(a); }",
        "void bad(float a, uint n) { weak $T w = observe(self); w.state.cycles = n; }",
        "void bad(float a, uint n) { $T h = observe(self); }",
        "void bad(float a, uint n) { weak $T w = self; }",
        "void bad(float a, uint n) { $T h = self; weak $T w = observe(h); h = w; }",
        "void bad(float a, uint n) { $T h = self; weak $T w; w = h; }",
        "void bad(float a, uint n) { float x = self + self; }",
        "void bad(float a, uint n) { float x = self - self; }",
        "void bad(float a, uint n) { float x = self * a; }",
        "void bad(float a, uint n) { $T h = +self; }",
        "void bad(float a, uint n) { $T h = -self; }",
        "void bad(float a, uint n) { if (self) { return; } }",
        "void bad(float a, uint n) { weak $T w; if (w) { return; } }",
        "void bad(float a, uint n) { if (null) { return; } }",
        "void bad(float a, uint n) { if (self < self) { return; } }",
        "void bad(float a, uint n) { if (self <= null) { return; } }",
        "void bad(float a, uint n) { if (self > self) { return; } }",
        "void bad(float a, uint n) { if (self >= null) { return; } }",
        "void bad(float a, uint n) { if (self == a) { return; } }",
        "void bad(float a, uint n) { self = self; }",
        "void bad(float a, uint n) { (self) = null; }",
        "void bad(float a, uint n) { business.UnknownMachine h; }",
        "void bad(float a, uint n) { business.NativeState h; }",
        "void bad(float a, uint n) { weak float h; }",
        "void bad(float a, uint n) { $T h = 1.0; }",
        "void bad(float a, uint n) { weak $T w = 1; }",
        "void bad(float a, uint n) { float x = null; }",
        "void bad(float a, uint n) { uint x = self; }",
        "void bad(float a, uint n) { $T h = h; }",
        "void bad(float a, uint n) { weak $T w = w; }",
        "void bad(float a, uint n) { { $T h = self; } h.progress = a; }",
        "void bad(float a, uint n) { { weak $T w = observe(self); } $T h = try_acquire(w); }",
        "void bad(float a, uint n) { $T h = new $T(a); }",
        "void bad(float a, uint n) { $T h = new $T(a, a); }",
        "void bad(float a, uint n) { $T h = new business.UnknownMachine(); }",
        "void bad(float a, uint n) { $T h = new float(); }",
        "void bad(float a, uint n) { $T h = new $T; }",
        "void bad(float a, uint n) { weak $T w = observe(null); }",
        "void bad(float a, uint n) { weak $T w = observe(a); }",
        "void bad(float a, uint n) { weak $T w = observe(observe(self)); }",
        "void bad(float a, uint n) { weak $T w = observe(); }",
        "void bad(float a, uint n) { weak $T w = observe(self, self); }",
        "void bad(float a, uint n) { $T h = try_acquire(null); }",
        "void bad(float a, uint n) { $T h = try_acquire(self); }",
        "void bad(float a, uint n) { $T h = try_acquire(n); }",
        "void bad(float a, uint n) { $T h = try_acquire(); }",
        "void bad(float a, uint n) { weak $T w = observe(self); $T h = try_acquire(w, w); }",
        "void bad(float a, uint n) { $T h = try_acquire(try_acquire(observe(self))); }",
        "void bad(float a, uint n) { return self; }",
    };
    const std::string type_name(binding.name);
    const auto reject_candidate = [&](std::string_view source, const char* environment_name) {
        const auto text = reference_source(source, binding.name);
        auto* retained = existing.get();
        char diagnostic[1024]{};
        const EnvironmentView storage(binding);
        const auto environment = storage.view(environment_name);
        const auto status = atelier_compile_business(&services, "invalid-reference.source", text.data(),
            static_cast<std::uint32_t>(text.size()), &environment, &retained, diagnostic, sizeof(diagnostic));
        if (status == 0 || retained != existing.get())
            throw std::runtime_error("Invalid reference source replaced an existing executable: " + text);
        require(std::string_view(diagnostic).starts_with("invalid-reference.source:"),
                "Reference rejection lost its actual source diagnostic.");
    };
    for (const auto* source : cases) reject_candidate(source, type_name.c_str());
    reject_candidate("void bad(float a, uint n) { " + std::string(other_binding.name) + " h = self; }", type_name.c_str());
    reject_candidate("void bad(float a, uint n) { $T h = new " + std::string(other_binding.name) + "(); }", type_name.c_str());
    reject_candidate("void bad(float a, uint n) { $T h = self; }", nullptr);
    auto receiver = take(runtime.create(binding.type));
    ExecutionStats stats;
    auto snapshot = take(execute_machine(runtime, binding, existing.get(), receiver.id(), 0.5f, 3, false, &stats));
    require(stats.polls == 1 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Rejected reference candidates changed the retained executable or its cleanup.");
    check_machine(runtime, binding, receiver.id(), 0.5f, 0.0f, 3);
    check_machine(runtime, binding, snapshot.id(), 0.5f, 0.0f, 3);
}

void verify_reference_sources(BusinessCode* scalar_code) {
    Runtime runtime;
    const auto types = register_machines(runtime);
    for (std::size_t index = 0; index < types.size(); ++index) {
        const auto binding = bind_type(runtime, types[index]);
        const auto other_binding = bind_type(runtime, types[1 - index]);
        const auto isolated = [&](auto&& scenario) {
            require(runtime.live_objects() == 0 && runtime.root_count() == 0,
                    "Reference scenario inherited objects or roots from another scenario.");
            const auto native_values = NativeState::live;
            scenario();
            collect(runtime);
            require(runtime.live_objects() == 0 && runtime.root_count() == 0 && NativeState::live == native_values,
                    "Reference scenario leaked an object, native value or host/execution root.");
        };
        isolated([&] { verify_scalar_reference_storage(runtime, binding, scalar_code); });
        isolated([&] { verify_reference_values(runtime, binding, other_binding); });
        isolated([&] { verify_reference_collection(runtime, binding); });
        isolated([&] { verify_reference_loop_slots(runtime, binding); });
        isolated([&] { verify_reference_exit(runtime, binding, false); });
        isolated([&] { verify_reference_exit(runtime, binding, true); });
        isolated([&] { verify_reference_returns(runtime, binding); });
        isolated([&] { verify_stale_source_references(runtime, binding, false); });
        isolated([&] { verify_stale_source_references(runtime, binding, true); });
        isolated([&] { verify_reference_temporaries(runtime, binding); });
        isolated([&] { verify_reference_failed_initializer(runtime, binding); });
        isolated([&] { verify_reference_rejections(runtime, binding, other_binding); });
    }
    std::puts("[pass] Named native/described source references preserve typed null, aliases, observations, acquisition and object access without changing scalar storage");
    std::puts("[pass] Fresh GC, loop slot reuse, early/native-error exits and explicit destruction honor lexical roots and stale identities before RAII cleanup");
    std::puts("[pass] Full-expression references, failed initialization and actual compiler rejections preserve effects, identity, cleanup and existing code handles");
    std::puts("[pass] First/middle/last returns and fallthrough clear users before snapshot publication; native/publication errors preserve effects and lexical GC");
}

// A stored editor action owns only a weak entry and an identity, not a receiver
// root or a code version. Tick uses the same stable entry-selection path.
struct ReloadEvent {
    BusinessProgram::Entry entry;
    ObjectId receiver;
    Result<ObjectRoot> fire(float amount, std::uint32_t steps) const {
        return invoke_machine(entry, receiver, amount, steps);
    }
};

std::string reload_source(const Binding& binding, unsigned factor, bool references = false) {
    std::string source = "void reload_tick(float delta, uint loops) { ";
    if (references) source += "$T alias = self; $T live = new $T(); weak $T watched = observe(live); $T acquired = try_acquire(watched); ";
    source += "uint turn = 0; while (turn < loops) { ";
    source += references ? "alias.advance(delta * " : "self.advance(delta * ";
    source += std::to_string(factor) + ".0); self.state.cycles = self.state.cycles + " + std::to_string(factor) + "; ";
    if (references) source += "acquired.progress = acquired.progress + delta; ";
    source += "turn = turn + 1; } self.state.heat = self.state.heat + " + std::to_string(factor) + ".0; ";
    if (references) source += "if (acquired != live) { self.state.heat = 999.0; } ";
    source += "}";
    return reference_source(source, binding.name);
}

BusinessProgram::Candidate prepare_reload(BusinessProgram& program, const Binding& binding,
    unsigned factor, bool references = false) {
    return take(BusinessProgram::prepare(take(program.request("reload.source", reload_source(binding, factor, references)))));
}

void require_code_state(std::uintptr_t address, bool alive) {
    require(address != 0, "Code lifetime inspection has no actual entry address.");
    DWORD64 image_base{};
    const auto* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory),
            "VirtualQuery failed to inspect generated code.");
    if (alive)
        require(function != nullptr && memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
                (memory.Protect & 0xff) == PAGE_EXECUTE_READ,
                "A live code owner lost executable storage or registered Windows unwind data.");
    else
        require(function == nullptr && memory.State == MEM_FREE,
                "A retired code version retained Windows code storage or unwind registration.");
}

void* machine_address(Runtime& runtime, const Binding& binding, ObjectId receiver) {
    void* storage{};
    NativeAction capture{&storage, [](void* context, CallContext& call) { *static_cast<void**>(context) = call.storage; }};
    ActionScope scope(capture);
    const std::array<Scalar, 1> arguments{0.0f};
    take(runtime.call(receiver, fixture_method(binding, "advance"), arguments));
    require(storage != nullptr, "Machine storage was not observed through its real native borrow.");
    return storage;
}

void verify_reload_backend_metadata(Runtime& runtime, const Binding& binding) {
    const auto empty = atelier_business_info(nullptr);
    require(empty.function_name == nullptr && empty.source_name == nullptr && empty.entry_address == 0 &&
            empty.services == BusinessServices{}, "Null compiled-code metadata is not empty.");
    CodeOwner code{nullptr, &atelier_destroy_business};
    {
        std::string name = "owned-reload-source.name";
        auto source = reload_source(binding, 1);
        std::string type_name(binding.name);
        auto callbacks = services;
        BusinessCode* output{};
        char diagnostic[2048]{};
        const EnvironmentView storage(binding);
        const auto environment = storage.view(type_name.c_str());
        require(atelier_compile_business(&callbacks, name.c_str(), source.data(), static_cast<std::uint32_t>(source.size()),
                    &environment, &output, diagnostic, sizeof(diagnostic)) == 0, diagnostic);
        code.reset(output);
        name.assign("destroyed caller source name");
        source.assign("destroyed caller source body");
        type_name.assign("destroyed caller type name");
        callbacks.poll = nullptr;
    }
    const auto info = atelier_business_info(code.get());
    const auto layout = atelier_business_layout(code.get());
    require(info.function_name && std::string_view(info.function_name) == "reload_tick" && info.source_name &&
            std::string_view(info.source_name) == "owned-reload-source.name" && info.services == services &&
            layout.object_type_name && std::string_view(layout.object_type_name) == binding.name,
            "Compiled metadata borrowed caller names, source storage or service table.");
    require_code_state(info.entry_address, true);
    const auto failed_source = reload_source(binding, 2);
    const std::string type_name(binding.name);
    auto* retained = code.get();
    char diagnostic[2048]{};
    const EnvironmentView storage(binding);
    const auto environment = storage.view(type_name.c_str());
    require(atelier_compile_business_without_poll_for_verification(&services, "real-missing-poll.source", failed_source.data(),
                static_cast<std::uint32_t>(failed_source.size()), &environment, &retained, diagnostic, sizeof(diagnostic)) != 0 &&
            retained == code.get(), "A real missing-service link failure replaced an existing nonempty code handle.");
    const std::string_view error(diagnostic);
    require(error.starts_with("real-missing-poll.source:") && error.find("JIT materialization") != error.npos &&
            error.find("business_poll") != error.npos, "Real link failure lost source, phase or missing-service cause.");
    auto receiver = take(runtime.create(binding.type));
    auto snapshot = take(execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 0.5f, 1.0f, 1);
    check_machine(runtime, binding, snapshot.id(), 0.5f, 1.0f, 1);
}

void verify_reload_diagnostics(Runtime& runtime, const Binding& binding) {
    BusinessProgram program(runtime, binding.type);
    const std::string long_name(2100, 'a');
    const std::string very_long_name(8192, 'b');
    std::string unicode_name(95, 'u');
    for (unsigned index = 0; index < 256; ++index) unicode_name += "\xF0\x9F\xA6\x8A";
    unicode_name += "end.source";
    const auto valid_source = reload_source(binding, 1);
    auto initial_request = take(program.request(long_name, valid_source));
    require(initial_request->source_name == long_name + "[request#1]",
        "Request snapshot truncated the complete source name.");
    auto initial = take(BusinessProgram::prepare(initial_request));
    take(program.try_publish(initial)); // Compatibility checks the full compiled source identity.
    auto receiver = take(runtime.create(binding.type));
    const auto old_address = take(program.pin()).address();
    const std::string invalid_source = "void reload_tick(float delta, uint loops) {\n  self.progress = loops;\n}";
    std::string previous_error;
    for (const auto name : {std::string_view{"short-diagnostic.source"}, std::string_view{long_name},
                           std::string_view{very_long_name}, std::string_view{unicode_name}}) {
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            const auto request = take(program.request(name, invalid_source));
            const auto result = BusinessProgram::prepare(request);
            reject(result, ErrorCode::invalid_definition, "Invalid diagnostic source was accepted.");
            const auto& message = result.error().message;
            require(message.find("[request#" + std::to_string(request->sequence) + "]") != std::string::npos &&
                    message.find(":2:19: Type mismatch") != std::string::npos && message != previous_error,
                "Source error lost its request identity, exact position or cause.");
            if (name.size() < 100)
                require(message.starts_with(request->source_name + ":2:19:"), "Short source diagnostic format changed.");
            require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
                        static_cast<int>(message.size()), nullptr, 0) > 0,
                "Source display abbreviation split a UTF-8 character.");
            previous_error = message;
        }
        const auto request = take(program.request(name, valid_source));
        const auto result = ProgramAccess::prepare(request, atelier_compile_business_without_poll_for_verification);
        reject(result, ErrorCode::invalid_definition, "Missing-service source unexpectedly linked.");
        if (result.error().message.find("[request#" + std::to_string(request->sequence) + "]") == std::string::npos ||
            result.error().message.find("JIT materialization") == std::string::npos ||
            result.error().message.find("business_poll") == std::string::npos)
            throw std::runtime_error("Link failure lost identity, phase or cause for request " +
                std::to_string(request->sequence) + " (source-name bytes " + std::to_string(name.size()) + "):\n" +
                result.error().message);
        require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.error().message.data(),
                    static_cast<int>(result.error().message.size()), nullptr, 0) > 0,
            "Link diagnostic display abbreviation split a UTF-8 character.");
    }
    require(take(program.pin()).address() == old_address,
        "Failed diagnostic requests replaced the published version.");
    require_code_state(old_address, true);
    ExecutionStats stats;
    auto snapshot = take(invoke_machine(program.entry(), receiver.id(), 0.5f, 1, &stats));
    check_machine(runtime, binding, receiver.id(), 0.5f, 1.0f, 1);
    check_machine(runtime, binding, snapshot.id(), 0.5f, 1.0f, 1);
    require(stats.explicit_cleanup && stats.polls == 2, "Diagnostic failures damaged old code execution or cleanup.");
    const auto recovered_request = take(program.request(very_long_name, valid_source));
    auto recovered = take(BusinessProgram::prepare(recovered_request));
    take(program.try_publish(recovered)); // Full 8 KiB identity survives COFF display-label abbreviation.
    check_machine(runtime, binding, receiver.id(), 0.5f, 1.0f, 1);
}

void verify_reload_contract_limits(Runtime& runtime, const Binding& binding) {
    BusinessProgram program(runtime, binding.type);
    const auto source = reload_source(binding, 1);
    const std::string type_name(binding.name);
    const auto compile = [&](const BusinessServices& actual, const char* object_type_name) {
        BusinessCode* output{};
        char diagnostic[2048]{};
        const EnvironmentView storage(binding);
        const auto environment = storage.view(object_type_name);
        require(atelier_compile_business(&actual, "actual-contract.source", source.data(),
                    static_cast<std::uint32_t>(source.size()), &environment, &output, diagnostic, sizeof(diagnostic)) == 0,
                diagnostic);
        return CodeOwner{output, &atelier_destroy_business};
    };
    auto altered = services;
    altered.poll = [](void* context) noexcept -> std::uint32_t { return poll(context); };
    require(!(altered == services), "Actual service-ABI rejection did not change a function address.");
    auto different_services = compile(altered, type_name.c_str());
    reject(ProgramAccess::compatible(program, different_services.get(), "actual-contract.source"),
           ErrorCode::invalid_definition, "Compatibility trusted a caller label rather than actual linked services.");
    auto different_type = compile(services, "business.DifferentReceiver");
    reject(ProgramAccess::compatible(program, different_type.get(), "actual-contract.source"),
           ErrorCode::type_mismatch, "Compatibility ignored the actual compiled object-type requirement.");
    auto actual = compile(services, type_name.c_str());
    reject(ProgramAccess::compatible(program, actual.get(), "fabricated-source.name"),
           ErrorCode::invalid_definition, "Compatibility accepted a fabricated compiled-source identity.");
    take(ProgramAccess::compatible(program, actual.get(), "actual-contract.source"));
    take(ProgramAccess::unchanged(program));
    auto reordered = binding;
    std::reverse(reordered.fields.begin(), reordered.fields.end());
    auto changed_catalog = compile_bound(reordered, source, "actual-contract.source");
    reject(ProgramAccess::compatible(program, changed_catalog.get(), "actual-contract.source"),
        ErrorCode::invalid_definition, "Reload ignored changed field-index bindings.");
    // Seed the exact counter boundary in this private acceptance, not a second
    // production sequencing path or a simulated compiler/link failure.
    ProgramAccess::set_request_sequence(program, std::numeric_limits<std::uint64_t>::max() - 1);
    auto last = take(program.request("last-sequence.source", source));
    require(last->sequence == std::numeric_limits<std::uint64_t>::max(), "Last valid request sequence was not accepted.");
    auto candidate = take(BusinessProgram::prepare(last));
    reject(program.request("overflow.source", source), ErrorCode::capacity_exceeded, "Request sequence silently wrapped.");
    take(program.try_publish(candidate));
    require(ProgramAccess::published_sequence(program) == last->sequence && !candidate,
            "Rejected sequence overflow invalidated the final accepted request.");
    require(runtime.root_count() == 0 && runtime.live_objects() == 0,
            "Contract checking or sequence-boundary compilation executed source initialization.");
}

void verify_reload_bootstrap(Runtime& runtime, const Binding& binding, const Binding& wrong_binding) {
    BusinessProgram program(runtime, binding.type);
    const auto tick = program.entry();
    auto receiver = take(runtime.create(binding.type));
    const auto identity = receiver.id();
    const auto address = machine_address(runtime, binding, identity);
    const ReloadEvent editor{tick, identity};
    set_machine(runtime, binding, identity, 4.0f, 6.0f, 8);
    reject(invoke_machine(tick, identity, 10.0f, 1), ErrorCode::invalid_definition, "An unpublished entry executed.");
    reject(program.pin(), ErrorCode::invalid_definition, "An unpublished program supplied a version lease.");
    BusinessProgram::Candidate empty;
    reject(program.try_publish(empty), ErrorCode::invalid_definition, "An empty candidate was published.");
    BusinessProgram::Request first_request;
    {
        std::string source_name = "captured-bootstrap.source";
        auto source_text = reload_source(binding, 1);
        first_request = take(program.request(source_name, source_text));
        source_name.assign("caller name overwritten");
        source_text.assign("caller source overwritten");
    }
    auto first = take(BusinessProgram::prepare(first_request));
    require(first_request->source_name.starts_with("captured-bootstrap.source[request#") &&
            first_request->source.find("void reload_tick") != std::string::npos,
            "Immutable request borrowed destroyed caller text instead of capturing its snapshot.");
    check_machine(runtime, binding, identity, 4.0f, 6.0f, 8);
    require(runtime.live_objects() == 1 && runtime.root_count() == 1,
            "Candidate preparation executed initialization or acquired object roots.");
    take(program.try_publish(first));
    require(!first, "Publication did not consume its candidate.");
    check_machine(runtime, binding, identity, 4.0f, 6.0f, 8);
    require(ProgramAccess::layout(take(program.pin())).strong_slots == business_root_slots, "Scalar bootstrap had a reference-local layout.");
    (void)take(invoke_machine(tick, identity, 0.5f, 2));
    (void)take(editor.fire(0.5f, 1));
    check_machine(runtime, binding, identity, 5.5f, 8.0f, 11);
    auto second = prepare_reload(program, binding, 2, true);
    check_machine(runtime, binding, identity, 5.5f, 8.0f, 11);
    take(program.try_publish(second));
    const auto changed = ProgramAccess::layout(take(program.pin()));
    require(changed.strong_slots > business_root_slots && changed.weak_slots != 0,
            "Body replacement did not change its actual reference-slot requirements.");
    require(receiver.id() == identity && machine_address(runtime, binding, identity) == address,
            "Publishing a method body replaced the business object or moved its storage.");
    check_machine(runtime, binding, identity, 5.5f, 8.0f, 11);
    (void)take(invoke_machine(tick, identity, 0.5f, 2));
    (void)take(editor.fire(0.5f, 1));
    check_machine(runtime, binding, identity, 8.5f, 12.0f, 17);
    const auto current = take(program.pin()).address();
    const std::array<std::string, 3> incompatible{
        "void renamed_entry(float delta, uint loops) {}",
        "void reload_tick(uint delta, uint loops) {}",
        "void reload_tick(float delta, uint loops) { " + std::string(wrong_binding.name) + " foreign = self; }"};
    for (const auto& source : incompatible) {
        auto request = take(program.request("incompatible.source", source));
        reject(BusinessProgram::prepare(request), ErrorCode::invalid_definition,
               "Renamed entry, incompatible signature or wrong source type was accepted.");
        require(take(program.pin()).address() == current, "An incompatible request changed the current executable.");
    }
    BusinessProgram other_owner(runtime, binding.type);
    auto foreign_owner = prepare_reload(other_owner, binding, 3);
    reject(program.try_publish(foreign_owner), ErrorCode::invalid_definition, "A different program owner's candidate was published.");
    require(static_cast<bool>(foreign_owner), "Foreign-candidate rejection consumed its caller-owned code.");
    Runtime other_runtime;
    const auto other_types = register_machines(other_runtime);
    const auto other_binding = bind_type(other_runtime, other_types[0]);
    BusinessProgram other_program(other_runtime, other_binding.type);
    auto foreign_runtime = prepare_reload(other_program, other_binding, 3);
    reject(program.try_publish(foreign_runtime), ErrorCode::invalid_definition, "A foreign Runtime candidate was published.");
    auto wrong_receiver = take(runtime.create(wrong_binding.type));
    reject(invoke_machine(tick, wrong_receiver.id(), 100.0f, 1), ErrorCode::type_mismatch, "Stable entry accepted a wrong receiver type.");
    require(take(program.pin()).address() == current && receiver.id() == identity &&
            machine_address(runtime, binding, identity) == address, "Rejected candidates changed code or object identity.");
    check_machine(runtime, binding, identity, 8.5f, 12.0f, 17);
    receiver.reset();
    collect(runtime);
    reject(editor.fire(1.0f, 1), ErrorCode::invalid_object, "A stored editor event kept its dead receiver alive.");
}

void verify_reload_request_order(Runtime& runtime, const Binding& binding) {
    BusinessProgram program(runtime, binding.type);
    auto initial = prepare_reload(program, binding, 1);
    take(program.try_publish(initial));
    auto receiver = take(runtime.create(binding.type));
    set_machine(runtime, binding, receiver.id(), 7.0f, 8.0f, 9);
    auto older_request = take(program.request("older.source", reload_source(binding, 2)));
    auto newer_request = take(program.request("newer.source", reload_source(binding, 3)));
    require(newer_request->sequence > older_request->sequence &&
            newer_request->source_name.find("[request#" + std::to_string(newer_request->sequence) + "]") != std::string::npos,
            "Source request did not retain its monotonic diagnostic identity.");
    // Deterministic out-of-order completion, not a claim of background-thread support.
    auto newer = take(BusinessProgram::prepare(newer_request));
    auto older = take(BusinessProgram::prepare(older_request));
    reject(program.try_publish(older), ErrorCode::invalid_definition, "An older late-finishing candidate replaced the newest request.");
    take(program.try_publish(newer));
    reject(program.try_publish(newer), ErrorCode::invalid_definition, "A consumed candidate was published twice.");
    auto duplicate = take(BusinessProgram::prepare(newer_request));
    reject(program.try_publish(duplicate), ErrorCode::invalid_definition, "Recompiling a published request allowed duplicate publication.");
    older = {};
    duplicate = {};
    const auto current = take(program.pin()).address();
    std::vector<BusinessProgram::Request> history{older_request, newer_request};
    std::vector<std::string> diagnostics;
    for (unsigned kind = 0; kind != 3; ++kind) {
        auto previous = take(program.request("previous-success.source", reload_source(binding, 4)));
        auto previous_code = take(BusinessProgram::prepare(previous));
        const auto failed_text = kind == 0 ? std::string("void reload_tick(float delta, uint loops) {") :
            kind == 1 ? std::string("void reload_tick(float delta, uint loops) { float x = loops; }") : reload_source(binding, 5);
        auto newest = take(program.request("newest-failure.source", failed_text));
        auto failed = kind == 2 ? ProgramAccess::prepare(newest, atelier_compile_business_without_poll_for_verification) : BusinessProgram::prepare(newest);
        reject(failed, ErrorCode::invalid_definition, "Newest parse/type/real-link error was accepted.");
        require(failed.error().message.starts_with(newest->source_name + ":"), "Preparation diagnostic lost its request identity.");
        if (kind == 2)
            require(failed.error().message.find("JIT materialization") != std::string::npos &&
                    failed.error().message.find("business_poll") != std::string::npos,
                    "Newest link failure was simulated or omitted the real missing service.");
        reject(program.try_publish(previous_code), ErrorCode::invalid_definition,
               "A newest failed request resurrected an older successful candidate.");
        require(static_cast<bool>(previous_code) && take(program.pin()).address() == current,
                "Failure consumed a rejected candidate or replaced the published version.");
        history.push_back(std::move(previous));
        history.push_back(std::move(newest));
        diagnostics.push_back(failed.error().message);
    }
    check_machine(runtime, binding, receiver.id(), 7.0f, 8.0f, 9);
    (void)take(invoke_machine(program.entry(), receiver.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 8.5f, 11.0f, 12);
    require(history.size() == 8 && diagnostics.size() == 3, "Out-of-order request/diagnostic history was not retained.");
}

void verify_reload_active_calls(Runtime& runtime, const Binding& binding) {
    BusinessProgram first(runtime, binding.type);
    BusinessProgram second(runtime, binding.type);
    auto a = prepare_reload(first, binding, 1);
    auto b = prepare_reload(second, binding, 1);
    take(first.try_publish(a));
    take(second.try_publish(b));
    auto old_lease = take(first.pin());
    auto receiver = take(runtime.create(binding.type));
    auto other = take(runtime.create(binding.type));
    auto second_candidate = prepare_reload(second, binding, 2);
    BusinessProgram::Candidate first_candidate;
    struct Active {
        BusinessProgram* first;
        BusinessProgram* second;
        const Binding* binding;
        BusinessProgram::Candidate* first_candidate;
        BusinessProgram::Candidate* second_candidate;
        ObjectId other;
        unsigned calls{}, polls{}, nested{};
    } state{&first, &second, &binding, &first_candidate, &second_candidate, other.id()};
    NativeAction attempt{&state, [](void* context, CallContext& call) {
        auto& state = *static_cast<Active*>(context);
        if (state.calls++ == 0)
            *state.first_candidate = prepare_reload(*state.first, *state.binding, 2, true);
        reject(state.first->try_publish(*state.first_candidate), ErrorCode::busy,
               "Native callback published its active program's replacement.");
        reject(state.second->try_publish(*state.second_candidate), ErrorCode::busy,
               "Native callback published another program in the same Runtime.");
        reject(call.runtime.check_code_publication(), ErrorCode::busy, "Active native code was treated as quiescent.");
        require(static_cast<bool>(*state.first_candidate) && static_cast<bool>(*state.second_candidate),
                "Busy publication consumed a retryable candidate.");
    }};
    ExecutionStats stats;
    {
        ActionScope scope(attempt);
        auto snapshot = take(invoke_machine(first.entry(), receiver.id(), 0.5f, 4, &stats, [](void* context, Invocation& invocation, const CollectionProgress&) {
            auto& state = *static_cast<Active*>(context);
            ++state.polls;
            reject(state.second->try_publish(*state.second_candidate), ErrorCode::busy,
                   "PollHook published another program while language execution was active.");
            if (*state.first_candidate)
                reject(state.first->try_publish(*state.first_candidate), ErrorCode::busy,
                       "PollHook changed the version partway through an invocation.");
            if (state.nested == 0) {
                {
                    auto nested_scope = take(ExecutionScope::enter(invocation.runtime));
                    reject(state.second->try_publish(*state.second_candidate), ErrorCode::busy,
                           "Nested execution admission released the outer publication guard.");
                    (void)take(invoke_machine(state.second->entry(), state.other, 0.0f, 0));
                }
                reject(state.second->try_publish(*state.second_candidate), ErrorCode::busy,
                       "Returning from a nested invocation cleared the outer execution scope.");
                ++state.nested;
            }
        }, &state));
        check_machine(runtime, binding, snapshot.id(), 2.0f, 1.0f, 4);
    }
    require(state.calls == 4 && state.polls == 5 && state.nested == 1 && stats.explicit_cleanup,
            "Active-publication coverage missed a native call, poll or nested scope.");
    check_machine(runtime, binding, receiver.id(), 2.0f, 1.0f, 4);
    check_machine(runtime, binding, other.id(), 0.0f, 1.0f, 0);
    take(first.try_publish(first_candidate));
    take(second.try_publish(second_candidate));
    require(!first_candidate && !second_candidate, "Explicit quiescent retry did not consume candidates.");
    (void)take(invoke_machine(first.entry(), receiver.id(), 0.5f, 1));
    (void)take(invoke_machine(second.entry(), other.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 3.0f, 3.0f, 6);
    check_machine(runtime, binding, other.id(), 1.0f, 3.0f, 2);

    auto pending = prepare_reload(second, binding, 3);
    struct Guarded { BusinessProgram* program; BusinessProgram::Candidate* candidate; unsigned calls{}; }
        guarded{&second, &pending};
    const auto cannot_publish = [](void* context, Invocation&, const CollectionProgress&) {
        auto& guarded = *static_cast<Guarded*>(context);
        reject(guarded.program->try_publish(*guarded.candidate), ErrorCode::busy,
               "An old lease or unversioned execute bypassed the Runtime-wide activity guard.");
        ++guarded.calls;
    };
    (void)take(invoke_machine(old_lease, receiver.id(), 0.5f, 1, &stats, cannot_publish, &guarded));
    require(stats.explicit_cleanup, "Pinned old-version invocation leaked roots.");
    auto unversioned = compile_reference(binding, reload_source(binding, 1), "unversioned-reload-control.source");
    (void)take(execute_machine(runtime, binding, unversioned.get(), receiver.id(), 0.0f, 0, false, &stats, cannot_publish, &guarded));
    require(stats.explicit_cleanup && guarded.calls == 3 && static_cast<bool>(pending),
            "Legacy or pinned invocation did not retain the pending publication candidate.");
    take(second.try_publish(pending));
    take(runtime.check_code_publication());
}

struct ReloadGcAttempt {
    BusinessProgram* program;
    BusinessProgram::Candidate* candidate;
    unsigned calls{};
    bool busy{}, threw{};
};
thread_local ReloadGcAttempt* reload_gc_attempt{};
struct ReloadGcProbe {
    ~ReloadGcProbe() noexcept {
        if (!reload_gc_attempt) return;
        auto& attempt = *reload_gc_attempt;
        ++attempt.calls;
        try {
            const auto result = attempt.program->try_publish(*attempt.candidate);
            attempt.busy = !result && result.error().code == ErrorCode::busy;
        } catch (...) { attempt.threw = true; }
    }
};
struct ReloadGcScope {
    ReloadGcAttempt* previous;
    explicit ReloadGcScope(ReloadGcAttempt& attempt) : previous(std::exchange(reload_gc_attempt, &attempt)) {}
    ~ReloadGcScope() { reload_gc_attempt = previous; }
};

void verify_reload_standalone_guards(Runtime& runtime, const Binding& binding, TypeId probe_type) {
    BusinessProgram program(runtime, binding.type);
    auto initial = prepare_reload(program, binding, 1);
    take(program.try_publish(initial));
    auto receiver = take(runtime.create(binding.type));
    auto pending = prepare_reload(program, binding, 2);
    const auto old_address = take(program.pin()).address();
    struct Attempt { BusinessProgram* program; BusinessProgram::Candidate* candidate; unsigned calls{}; }
        attempt{&program, &pending};
    NativeAction native{&attempt, [](void* context, CallContext&) {
        auto& attempt = *static_cast<Attempt*>(context);
        reject(attempt.program->try_publish(*attempt.candidate), ErrorCode::busy,
               "Standalone native borrow allowed code publication.");
        ++attempt.calls;
    }};
    {
        ActionScope scope(native);
        const std::array<Scalar, 1> arguments{0.0f};
        take(runtime.call(receiver.id(), fixture_method(binding, "advance"), arguments));
    }
    take(runtime.visit_roots({&attempt, [](void* context, const ObjectReference&) -> Result<void> {
        auto& attempt = *static_cast<Attempt*>(context);
        reject(attempt.program->try_publish(*attempt.candidate), ErrorCode::busy,
               "Standalone read-only root traversal allowed publication.");
        ++attempt.calls;
        return {};
    }}));
    auto garbage = take(runtime.create(probe_type));
    const auto garbage_id = garbage.id();
    garbage.reset();
    ReloadGcAttempt gc{&program, &pending};
    {
        ReloadGcScope scope(gc);
        collect(runtime); // Actual sweep/destructor callback, no language invocation is active.
    }
    require(attempt.calls == 2 && gc.calls == 1 && gc.busy && !gc.threw && static_cast<bool>(pending) &&
            take(program.pin()).address() == old_address,
            "Standalone borrow/traversal/real GC callback did not preserve the pending candidate.");
    reject(runtime.type_of(garbage_id), ErrorCode::invalid_object, "GC callback probe was not actually reclaimed.");
    take(program.try_publish(pending));
    require_code_state(old_address, false);
    check_machine(runtime, binding, receiver.id(), 0.0f, 0.0f, 0);

    auto changed_slots = prepare_reload(program, binding, 3, true);
    auto unrooted = take(runtime.create(binding.type));
    const auto unrooted_id = unrooted.id();
    unrooted.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() == CollectionPhase::roots) take(runtime.collect_step({1}));
    require(runtime.collection_phase() != CollectionPhase::idle, "GC was not paused before publication.");
    take(program.try_publish(changed_slots));
    require(runtime.collection_phase() != CollectionPhase::idle, "Publication secretly completed the paused collection.");
    ExecutionStats stats;
    unsigned cycles{};
    auto snapshot = take(invoke_machine(program.entry(), receiver.id(), 0.5f, 3, &stats, [](void* context, Invocation& invocation, const CollectionProgress&) {
        auto& cycles = *static_cast<unsigned*>(context);
        collect(invocation.runtime);
        ++cycles;
        require(invocation.runtime.live_objects() == (fixture(invocation).stats.polls == 1 ? 2u : 3u),
                "Published reference-slot layout lost a source-held object or retained garbage.");
        for (const auto& root : invocation.roots)
            if (root.registered() && root.id() != ObjectId{}) take(invocation.runtime.type_of(root.id()));
    }, &cycles));
    require(cycles == 4 && stats.polls == 4 && stats.explicit_cleanup && runtime.root_count() == 2,
            "Paused-GC publication did not preserve new source references through real collections and cleanup.");
    reject(runtime.type_of(unrooted_id), ErrorCode::invalid_object, "Paused collection retained its unrooted control.");
    check_machine(runtime, binding, receiver.id(), 4.5f, 3.0f, 9);
    check_machine(runtime, binding, snapshot.id(), 4.5f, 3.0f, 9);
    collect(runtime);
    require(runtime.live_objects() == 2, "Reference locals remained alive after the reloaded invocation.");
}

void verify_reload_failure_release(Runtime& runtime, const Binding& binding) {
    for (const bool fail_native : {false, true}) {
        BusinessProgram program(runtime, binding.type);
        auto initial = take(BusinessProgram::prepare(take(program.request("reload-exit.source", R"(
            void reload_tick(float delta, uint loops) {
                self.progress = 3.0;
                self.advance(delta);
                if (loops == 0) { return; }
                self.state.heat = 99.0;
            }
        )"))));
        take(program.try_publish(initial));
        auto receiver = take(runtime.create(binding.type));
        auto pending = prepare_reload(program, binding, 2);
        ExecutionStats stats;
        if (fail_native) {
            NativeAction fail{nullptr, [](void*, CallContext&) { throw std::runtime_error("Reload native failure."); }};
            ActionScope scope(fail);
            auto result = invoke_machine(program.entry(), receiver.id(), 0.5f, 1, &stats);
            reject(result, ErrorCode::invocation_failed, "Reload native error did not use generated failure cleanup.");
            require(result.error().message == "Reload native failure.", "Reload native diagnostic changed.");
        } else {
            (void)take(invoke_machine(program.entry(), receiver.id(), 0.5f, 0, &stats));
        }
        require(stats.explicit_cleanup && runtime.root_count() == 1,
                "Early return or native error retained execution roots.");
        check_machine(runtime, binding, receiver.id(), 3.5f, 0.0f, 0);
        take(program.try_publish(pending));
        (void)take(invoke_machine(program.entry(), receiver.id(), 0.5f, 1));
        check_machine(runtime, binding, receiver.id(), 4.5f, 2.0f, 2);
        receiver.reset();
        collect(runtime);
    }
    BusinessProgram program(runtime, binding.type);
    auto initial = prepare_reload(program, binding, 1);
    take(program.try_publish(initial));
    auto pending = prepare_reload(program, binding, 2);
    auto receiver = take(runtime.create(binding.type));
    ExecutionStats stats;
    reject(invoke_machine(program.entry(), receiver.id(), 0.5f, 1, &stats, [](void*, Invocation&, const CollectionProgress&) { throw std::runtime_error("Reload poll failure."); }),
        ErrorCode::invocation_failed, "Reload safe-point error escaped its checked entry.");
    require(stats.explicit_cleanup && runtime.root_count() == 1, "Poll failure retained execution ownership.");
    check_machine(runtime, binding, receiver.id(), 0.0f, 0.0f, 0);
    take(program.try_publish(pending));
    take(runtime.check_code_publication());
}

void verify_reload_version_retirement(Runtime& runtime, const Binding& binding) {
    BusinessProgram program(runtime, binding.type);
    auto first = prepare_reload(program, binding, 1);
    take(program.try_publish(first));
    auto old = take(program.pin());
    const auto old_address = old.address();
    require_code_state(old_address, true);
    auto next = prepare_reload(program, binding, 2, true); // Materialize before the old address can be reused.
    const auto next_address = next.address();
    require(next_address != old_address, "Simultaneously live versions alias executable storage.");
    take(program.try_publish(next));
    require_code_state(old_address, true);
    require_code_state(next_address, true);
    auto receiver = take(runtime.create(binding.type));
    (void)take(invoke_machine(old, receiver.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 0.5f, 1.0f, 1);
    (void)take(invoke_machine(program.entry(), receiver.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 1.5f, 3.0f, 3);
    old = {};
    require_code_state(old_address, false);
    require_code_state(next_address, true);
}

void verify_reload_owner_destruction(Runtime& runtime, const Binding& binding, bool fail_native) {
    auto program = std::make_unique<BusinessProgram>(runtime, binding.type);
    auto request = take(program->request("closed-owner-history.source", reload_source(binding, 1)));
    auto first = take(BusinessProgram::prepare(request));
    take(program->try_publish(first));
    auto lease = take(program->pin());
    const auto old_address = lease.address();
    // The next executable already occupies distinct storage before any release.
    auto abandoned = prepare_reload(*program, binding, 2);
    const auto abandoned_address = abandoned.address();
    require(abandoned_address != old_address, "Prepared and current versions have the same address.");
    auto receiver = take(runtime.create(binding.type));
    auto active_entry = std::make_unique<BusinessProgram::Entry>(program->entry());
    const ReloadEvent saved_event{program->entry(), receiver.id()};
    struct Closing {
        std::unique_ptr<BusinessProgram>* program;
        std::unique_ptr<BusinessProgram::Entry>* active_entry;
        const ReloadEvent* event;
        const BusinessProgram::Request* request;
        std::uintptr_t address;
        bool fail;
        unsigned calls{};
    } state{&program, &active_entry, &saved_event, &request, old_address, fail_native};
    NativeAction close{&state, [](void* context, CallContext&) {
        auto& state = *static_cast<Closing*>(context);
        if (++state.calls != 1) return;
        state.program->reset();
        state.active_entry->reset(); // Even the currently executing Entry object may be destroyed.
        reject(state.event->fire(100.0f, 1), ErrorCode::invalid_definition,
               "A weak event reopened a program whose active call still retained its state.");
        reject(BusinessProgram::prepare(*state.request), ErrorCode::invalid_definition,
               "Historical source reopened a closed program during its active invocation.");
        require_code_state(state.address, true);
        if (state.fail) throw std::runtime_error("Program closed inside failing native call.");
    }};
    ExecutionStats stats;
    ObjectRoot snapshot;
    {
        ActionScope scope(close);
        auto result = invoke_machine(*active_entry, receiver.id(), 0.5f, 3, &stats);
        if (fail_native) {
            reject(result, ErrorCode::invocation_failed, "Owner destruction lost its active native failure.");
            require(result.error().message == "Program closed inside failing native call.",
                    "Closing an active program changed the native diagnostic.");
        } else snapshot = take(std::move(result));
    }
    require(!program && !active_entry && stats.explicit_cleanup && state.calls == (fail_native ? 1u : 3u) &&
            runtime.root_count() == (fail_native ? 1u : 2u),
            "Closed program/entry destruction lost active code or execution cleanup.");
    check_machine(runtime, binding, receiver.id(), fail_native ? 0.5f : 1.5f, fail_native ? 0.0f : 1.0f,
                  fail_native ? 0u : 3u);
    reject(saved_event.fire(100.0f, 1), ErrorCode::invalid_definition, "Saved event executed after program-owner deletion.");
    require_code_state(old_address, true);
    snapshot.reset();
    (void)take(invoke_machine(lease, receiver.id(), 0.5f, 1, &stats));
    require(stats.explicit_cleanup && runtime.root_count() == 1, "Explicit lease after owner deletion leaked roots.");
    check_machine(runtime, binding, receiver.id(), fail_native ? 1.0f : 2.0f, fail_native ? 1.0f : 2.0f,
                  fail_native ? 1u : 4u);
    lease = {};
    require_code_state(old_address, false);
    require(!request->source.empty(), "Request history disappeared with the executable owner.");
    require_code_state(abandoned_address, true); // Intentionally retained candidate is a genuine code owner.
    abandoned = {};
    require_code_state(abandoned_address, false);
    collect(runtime);
    require(runtime.root_count() == 1 && runtime.live_objects() == 1,
            "Owner deletion, failure or final lease release retained roots/source objects.");
}

void verify_reload_history_retirement(Runtime& runtime, const Binding& binding) {
    std::vector<BusinessProgram::Request> requests;
    std::vector<std::string> diagnostics;
    auto program = std::make_unique<BusinessProgram>(runtime, binding.type);
    auto receiver = take(runtime.create(binding.type));
    const auto object_id = receiver.id();
    const auto object_address = machine_address(runtime, binding, object_id);
    const ReloadEvent event{program->entry(), object_id};
    std::uintptr_t previous{};
    unsigned total{};
    for (unsigned version = 1; version <= 6; ++version) {
        auto request = take(program->request("retained-history.source", reload_source(binding, version, version % 2 == 0)));
        auto candidate = take(BusinessProgram::prepare(request));
        const auto address = candidate.address();
        if (previous != 0) require_code_state(previous, true);
        take(program->try_publish(candidate));
        require(!candidate, "Reload loop retained a consumed candidate's code.");
        if (previous != 0) require_code_state(previous, false); // Inspect before any further code allocation.
        require_code_state(address, true);
        requests.push_back(std::move(request));
        (void)take(event.fire(1.0f, 1));
        total += version;
        check_machine(runtime, binding, object_id, static_cast<float>(total), static_cast<float>(total), total);
        require(receiver.id() == object_id && machine_address(runtime, binding, object_id) == object_address,
                "Repeated publication recreated or moved the business receiver.");
        auto failure = take(program->request("retained-diagnostic.source",
            "void reload_tick(float delta, uint loops) { self.progress = loops; }"));
        auto rejected = BusinessProgram::prepare(failure);
        reject(rejected, ErrorCode::invalid_definition, "Diagnostic-history source unexpectedly compiled.");
        diagnostics.push_back(rejected.error().message);
        requests.push_back(std::move(failure));
        previous = address;
    }
    // Caller-held rejected candidates really do retain their own version; the
    // publisher must neither hide their ownership nor steal it on rejection.
    auto old_request = take(program->request("held-candidate.source", reload_source(binding, 7)));
    auto held = take(BusinessProgram::prepare(old_request));
    const auto held_address = held.address();
    auto latest_request = take(program->request("latest-candidate.source", reload_source(binding, 8)));
    auto latest = take(BusinessProgram::prepare(latest_request));
    const auto final_address = latest.address();
    reject(program->try_publish(held), ErrorCode::invalid_definition, "A stale candidate became current in the retirement loop.");
    take(program->try_publish(latest));
    require_code_state(previous, false);
    require_code_state(held_address, true);
    held = {};
    require_code_state(held_address, false);
    requests.push_back(std::move(old_request));
    requests.push_back(std::move(latest_request));
    program.reset();
    require_code_state(final_address, false);
    require(requests.size() == 14 && diagnostics.size() == 6 && !requests.front()->source.empty() &&
            diagnostics.back().find("retained-diagnostic.source[request#") != std::string::npos,
            "History did not preserve request text and request-specific diagnostics after code retirement.");
    reject(event.fire(1.0f, 1), ErrorCode::invalid_definition, "History accidentally kept a closed program callable.");
    collect(runtime);
    require(runtime.root_count() == 1 && runtime.live_objects() == 1,
            "Repeated reload/request history retained receiver aliases or source-created objects.");
}

void verify_hot_reload() {
    for (std::size_t index = 0; index < 2; ++index) {
        Runtime runtime;
        const auto types = register_machines(runtime);
        const auto probe = take(NativeRegistration<ReloadGcProbe>{runtime, "business.ReloadGcProbe", TypeKind::object}.publish());
        const auto binding = bind_type(runtime, types[index]);
        const auto wrong_binding = bind_type(runtime, types[1 - index]);
        const auto isolated = [&](auto&& scenario) {
            require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Reload scenario inherited roots or objects.");
            const auto native_values = NativeState::live;
            scenario();
            take(runtime.check_code_publication());
            collect(runtime);
            require(runtime.root_count() == 0 && runtime.live_objects() == 0 && NativeState::live == native_values,
                    "Reload scenario leaked execution scope, native values, object roots or receiver storage.");
        };
        isolated([&] { verify_reload_backend_metadata(runtime, binding); });
        isolated([&] { verify_reload_diagnostics(runtime, binding); });
        isolated([&] { verify_reload_contract_limits(runtime, binding); });
        isolated([&] { verify_reload_bootstrap(runtime, binding, wrong_binding); });
        isolated([&] { verify_reload_request_order(runtime, binding); });
        isolated([&] { verify_reload_active_calls(runtime, binding); });
        isolated([&] { verify_reload_standalone_guards(runtime, binding, probe); });
        isolated([&] { verify_reload_failure_release(runtime, binding); });
        isolated([&] { verify_reload_version_retirement(runtime, binding); });
        isolated([&] { verify_reload_owner_destruction(runtime, binding, false); });
        isolated([&] { verify_reload_owner_destruction(runtime, binding, true); });
        isolated([&] { verify_reload_history_retirement(runtime, binding); });
    }
    std::puts("[pass] Reload binding: native/described bootstrap, unchanged object identity/storage/state, changed reference slots, current Tick/editor events and incompatible/foreign rejection");
    std::puts("[pass] Reload ordering: immutable request diagnostics including long source names, deterministic out-of-order completion, newest parse/type/real-link failure without fallback and duplicate-publication rejection");
    std::puts("[pass] Reload admission: Runtime-wide active/nested/legacy/leased entry guards, native/poll/traversal/real-GC rejection, paused-GC publication and error/early-return release");
    std::puts("[pass] Reload lifetime: explicit version leases, callback owner/entry destruction, actual Windows code/unwind retirement and code-free request/diagnostic history");
    std::puts("[pass] Reload boundary: 12 scenarios per native/described receiver; one immutable-metadata host entry only, explicit quiescent retry, no general MethodId dispatch/background compiler/OSR/inlining invalidation");
}

void verify_source_rejections(BusinessCode* existing) {
    char link_error[2048]{};
    if (atelier_verify_business_link_failure(&services, link_error, sizeof(link_error)) != 0)
        throw std::runtime_error(link_error);
    Runtime runtime;
    const auto types = register_machines(runtime);
    const auto binding = bind_type(runtime, types[0]);
    const EnvironmentView storage(binding);
    const auto environment = storage.view(nullptr);
    struct InvalidSource { const char* text; const char* diagnostic; };
    const InvalidSource cases[]{
        {"void bad(float a, uint n) {\n  missing = 1;\n}", "invalid.source:2:3: Unknown identifier"},
        {"void bad(float a, uint n) { float x = 1; }", "Type mismatch"},
        {"void bad(float a, uint n) { self.advance(1); }", "Type mismatch"},
        {"void bad(float a, uint n) { self.advance(); }", "argument count mismatch"},
        {"void bad(float a, uint n) { self.advance(a, a); }", "argument count mismatch"},
        {"void bad(float a, uint n) { self.missing(a); }", "Unknown native method"},
        {"void bad(float a, uint n) { self.missing = a; }", "Unknown receiver field"},
        {"void bad(float a, uint n) { uint x = x; }", "Unknown identifier"},
        {"void bad(float a, uint n) { { uint x = 1; } self.state.cycles = x; }", "Unknown identifier"},
        {"void bad(float a, uint n) { uint n = 0; }", "already visible"},
        {"void bad(float a, uint n) { if (n) { return; } }", "requires a comparison"},
        {"void bad(float a, uint n) { if (a > n) { return; } }", "matching numeric types"},
        {"void bad(float a, uint n) { 1 = 2; }", "Assignment requires"},
        {"void bad(float a, uint n) { +n = 1; }", "Assignment requires"},
        {"void bad(float a, uint n) { +self.progress = a; }", "Assignment requires"},
        {"void bad(float a, uint n) { (+n) = 1; }", "Assignment requires"},
        {"void bad(float a, uint n) { return 1; }", "cannot return a value"},
        {"void bad(float a, uint n) { uint x = 4294967296; }", "outside 0..4294967295"},
        {"void bad(float a, uint n) { float x = 1.0e999; }", "out of range"},
        {"void bad(float a, uint n) { uint x = -1; }", "Type mismatch"},
        {"void bad(float a, uint n) { float x = a / 2.0; }", "Division is not supported"},
        {"void bad(float a, uint n) { float x = 1.0e; }", "exponent requires digits"},
        {"void bad(float a, uint n) { uint x = 1word; }", "Invalid numeric literal suffix"},
        {"void bad(float a, uint n) { /* unfinished", "Unterminated block comment"},
        {"void bad(float a, uint n) {", "Expected '}'"},
        {"void bad(float a, uint n) {} void extra(float a, uint n) {}", "Only one host-bound entry"},
    };
    const auto reject = [&](std::string_view source, std::string_view expected) {
        auto* retained = existing;
        char diagnostic[1024]{};
        const auto status = atelier_compile_business(&services, "invalid.source", source.data(),
            static_cast<std::uint32_t>(source.size()), &environment, &retained, diagnostic, sizeof(diagnostic));
        require(status != 0 && retained == existing, "Invalid source replaced an existing executable.");
        require(std::string_view(diagnostic).starts_with("invalid.source:") &&
                std::string_view(diagnostic).find(expected) != std::string_view::npos,
            "Source diagnostic lost its source location or rejection reason.");
    };
    for (const auto& source : cases) reject(source.text, source.diagnostic);
    std::string long_expression = "void bad(float a, uint n) { uint x = 0";
    for (std::size_t index = 0; index < 160; ++index) long_expression += " + 1";
    long_expression += "; }";
    reject(long_expression, "Expression tree exceeds");
    reject("void bad(float a, uint n) { float x = " + std::string(160, '(') + "a" + std::string(160, ')') + "; }",
        "Source nesting exceeds");
    std::string embedded_null = "void bad(float a, uint n) {}";
    embedded_null.push_back('\0');
    embedded_null += "void hidden(float a, uint n) {}";
    reject(embedded_null, "Unsupported source character");
    auto receiver = take(runtime.create(types[0]));
    auto snapshot = take(execute_machine(runtime, binding, existing, receiver.id(), 0.5f, 4));
    check_machine(runtime, binding, snapshot.id(), 2.0f, 0.0f, 4);
    std::puts("[pass] Source and real link errors retain diagnostics, reject malformed input, and leave existing machine code executable");
}
#include "typed_business.inl"
#include "coroutine_business.inl"
}

void verify_business_execution() {
    const auto native_values = NativeState::live;
    auto code = compile_business();
    verify_business_state(code.get());
    verify_paused_collection(code.get());
    verify_failures_and_admission(code.get());
    verify_identity_boundaries(code.get());
    verify_frame_protocol();
    verify_active_code_lifetime(false);
    verify_active_code_lifetime(true);
    verify_source_execution();
    verify_reference_sources(code.get());
    verify_source_rejections(code.get());
    verify_hot_reload();
    verify_typed_business();
    verify_coroutines();
    require(NativeState::live == native_values, "Business acceptance leaked native state instances.");
    std::puts("Business execution acceptance: PASS");
}
}
