#include "../helpers.hpp"
#include "business.hpp"
#include "jit/source_backend_verification.hpp"
#include "machine.hpp"
#include "memory/fatal.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"

#include <windows.h>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace atelier::jit_acceptance {
namespace {
using atelier::acceptance::NativeMachine;

using atelier::acceptance::NativeState;

namespace source_detail = atelier::source::detail;

using namespace source_detail;

using BusinessProgram = atelier::source::Program;

using namespace atelier::acceptance::helpers;

std::span<const FieldId> fixture_field(const Binding& binding, std::string_view name) {
    for (const auto& field : binding.fields)
        if (field.name == name)
            return field.path;
    fatal("Fixture field was not exposed by the Runtime.");
}

std::array<BusinessValue, 2> machine_arguments(float amount, std::uint32_t steps) {
    return {scalar(amount), scalar(steps)};
}

CodeOwner compile_bound(const Binding& binding, std::string_view text, const char* name,
                        const BusinessServices& callbacks = services, const char* entry = nullptr) {
    const EnvironmentView storage(binding);
    const std::string type_name(binding.name);
    const auto environment = storage.view(type_name.c_str());
    BusinessCode* output{};
    char diagnostic[1024]{};
    if (atelier_compile_business(&callbacks, name, text.data(), static_cast<std::uint32_t>(text.size()), &environment,
                                 &output, diagnostic, sizeof(diagnostic), entry) != 0)
        throw std::runtime_error(diagnostic);
    return CodeOwner{output, &atelier_destroy_business};
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

    Result<void> (*run)(void*, ObjectId) noexcept;
};

thread_local NativeAction* native_action{};

struct ActionScope {
    NativeAction* previous;

    explicit ActionScope(NativeAction& action) : previous(std::exchange(native_action, &action)) {}

    ~ActionScope() { native_action = previous; }
};

Result<void> advance_machine(Runtime&, ObjectId self, NativeMachine& machine, float amount) noexcept {
    machine.advance(amount);
    if (native_action)
        return native_action->run(native_action->context, self);
    return {};
}

Result<void> assign_machine(Runtime&, ObjectId, NativeMachine&, float& target, float value) {
    target = value;
    return {};
}

TypeId register_machine(Runtime& runtime) {
    take(NativeRegistration<NativeState>{runtime, "business.NativeState", TypeKind::record}
             .field<&NativeState::cycles>("cycles")
             .field<&NativeState::heat>("heat")
             .publish());
    auto definition = NativeRegistration<NativeMachine>{runtime, "business.NativeMachine", TypeKind::object}
                          .field<&NativeMachine::state>("state")
                          .field<&NativeMachine::progress>("progress")
                          .method<&advance_machine>("advance")
                          .method<&assign_machine>("assign")
                          .build();
    return take(runtime.register_type(std::move(definition)));
}

void set_machine(Runtime& runtime, const Binding& binding, ObjectId object, float progress, float heat,
                 std::uint32_t cycles) noexcept {
    if (auto written = runtime.write(object, fixture_field(binding, "progress"), progress); !written)
        fatal(written.error().message);
    if (auto written = runtime.write(object, fixture_field(binding, "state.heat"), heat); !written)
        fatal(written.error().message);
    if (auto written = runtime.write(object, fixture_field(binding, "state.cycles"), cycles); !written)
        fatal(written.error().message);
}

void check_machine(Runtime& runtime, const Binding& binding, ObjectId object, float progress, float heat,
                   std::uint32_t cycles) {
    require(std::get<float>(take(runtime.read(object, fixture_field(binding, "progress")))) == progress &&
                std::get<float>(take(runtime.read(object, fixture_field(binding, "state.heat")))) == heat &&
                std::get<std::uint32_t>(take(runtime.read(object, fixture_field(binding, "state.cycles")))) == cycles,
            "Generated business state is incorrect.");
}

void collect(Runtime& runtime) {
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({64}));
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({64}));
}

void verify_frame_protocol() {
    Runtime runtime;
    Runtime foreign;
    const auto type = register_machine(runtime);
    auto receiver = take(runtime.create(type));
    auto other = take(foreign.create(register_machine(foreign)));
    ObjectRoot transferred;
    {
        std::array<ObjectRoot, 2> roots;
        ExecutionFrame frame(runtime, roots);
        take(frame.hold(0, receiver.id()));
        require(!frame.hold(0, other.id()) && take(frame.object(0)) == receiver.id() && runtime.root_count() == 2,
                "Failed replacement discarded the old execution root.");
        take(frame.create(1, type));
        const auto first = take(frame.object(1));
        take(runtime.begin_collection());
        take(runtime.collect_step({1}));
        take(frame.clear(1));
        take(frame.create(1, type));
        const auto replacement = take(frame.object(1));
        collect(runtime);
        require(first != replacement && !runtime.type_of(first) && runtime.type_of(replacement).has_value(),
                "Execution root replacement retained the wrong object.");
        transferred = take(frame.take(1));
        require(runtime.root_count() == 3, "Return handoff lost or duplicated a registration.");
        take(frame.clear(0));
        require(frame.empty(), "Explicit frame teardown retained registrations.");
    }
    require(runtime.root_count() == 2, "Transferred root did not outlive its producer frame.");
    transferred.reset();
    try {
        std::array<ObjectRoot, 1> roots;
        ExecutionFrame frame(runtime, roots);
        take(frame.hold(0, receiver.id()));
        throw std::runtime_error("Host scope exit.");
    } catch (const std::runtime_error&) {
    }
    require(runtime.root_count() == 1, "Host exception bypassed frame RAII cleanup.");
}

void verify_reference_returns(Runtime& runtime, const Binding& binding) {
    struct State {
        bool fail_publication;

        bool publish_seen{};
    };
    auto callbacks = services;
    callbacks.finish = [](void* context) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        const auto status = bridge(invocation, [&]() -> Result<void> {
            auto& state = *static_cast<State*>(fixture(invocation).hook_state);
            state.publish_seen = true;
            if (state.fail_publication)
                return std::unexpected(Error{ErrorCode::invocation_failed, "Snapshot publication failure."});
            return {};
        });
        return status == 0 ? finish(context) : status;
    };
    callbacks.clear = [](void* context, std::uint32_t slot) noexcept {
        clear_slot(context, slot);
        auto& invocation = *static_cast<Invocation*>(context);
        const auto& state = *static_cast<State*>(fixture(invocation).hook_state);
        if (!state.fail_publication && state.publish_seen)
            invocation.fail(ErrorCode::invocation_failed, "Cleanup after snapshot publication failed.");
    };
    auto code = compile_bound(binding,
                              reference_source(R"(
        void reference_returns(float amount, uint steps) {
            $T held = new $T();
            weak $T watched = observe(held);
            held.advance(amount);
            return;
        })",
                                               binding.name),
                              "reference-returns.source", callbacks);
    for (const bool publication : {true, false}) {
        State state{publication};
        auto receiver = take(runtime.create(binding.type));
        ExecutionStats stats;
        auto result = execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, &stats, nullptr, &state);
        require(!result && state.publish_seen && stats.explicit_cleanup && runtime.root_count() == 1,
                "Failed snapshot publication retained an output or execution storage.");
        collect(runtime);
        require(runtime.live_objects() == 1, "Failed publication retained source objects or a snapshot.");
        receiver.reset();
        collect(runtime);
        require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Publication failure leaked its receiver.");
    }
}

void verify_stale_source_references(Runtime& runtime, const Binding& binding) {
    auto code = compile_bound(binding,
                              reference_source(R"(
        void stale_references(float delta, uint loops) {
            $T held = new $T();
            weak $T copy = observe(held);
            held.advance(delta);
            copy.progress = held.progress;
            self.assign(copy.progress, later_argument());
        }
        float later_argument() {
            uint turn = 0;
            while (turn < 1) { turn = turn + 1; }
            return 88.0;
        })",
                                               binding.name),
                              "stale-references.source", services, "stale_references");
    auto receiver = take(runtime.create(binding.type));
    struct Replacement {
        ObjectId old;

        ObjectRoot root;
    } state;
    NativeAction capture{&state, [](void* context, ObjectId self) noexcept -> Result<void> {
                             static_cast<Replacement*>(context)->old = self;
                             return {};
                         }};
    ExecutionStats stats;
    {
        ActionScope scope(capture);
        auto result = execute_machine(
            runtime, binding, code.get(), receiver.id(), 0.5f, 0, &stats,
            [](void* context, Invocation& invocation) noexcept -> Result<void> {
                auto& state = *static_cast<Replacement*>(context);
                if (fixture(invocation).stats.polls != 2)
                    return {};
                if (auto destroyed = invocation.runtime.destroy(state.old); !destroyed)
                    fatal(destroyed.error().message);
                auto created = invocation.runtime.create(invocation.binding.type);
                if (!created)
                    fatal(created.error().message);
                state.root = std::move(*created);
                if (state.root.id() == state.old)
                    fatal("Replacement reused the stale identity.");
                set_machine(invocation.runtime, invocation.binding, state.root.id(), 99.0f, 7.0f, 25);
                return {};
            },
            &state);
        require(!result && stats.explicit_cleanup && runtime.root_count() == 2,
                "Stale source access reached replacement storage or retained roots.");
    }
    check_machine(runtime, binding, state.root.id(), 99.0f, 7.0f, 25);
    collect(runtime);
    require(runtime.live_objects() == 2, "Stale-reference cleanup retained a snapshot.");
}

void verify_reference_failed_initializer(Runtime& runtime, const Binding& binding) {
    auto code = compile_reference(binding, R"(
        void failed_initializer(float delta, uint loops) {
            $T live = new $T();
            weak $T watched = observe(live);
            self.progress = 7.0;
            $T absent;
            float failed = try_acquire(observe(absent)).progress;
            self.progress = 99.0;
        })",
                                  "failed-reference-initializer.source");
    auto receiver = take(runtime.create(binding.type));
    ExecutionStats stats;
    require(!execute_machine(runtime, binding, code.get(), receiver.id(), 0.5f, 0, &stats) && stats.explicit_cleanup &&
                runtime.root_count() == 1,
            "Failed initialization retained its temporaries.");
    check_machine(runtime, binding, receiver.id(), 7.0f, 0.0f, 0);
    collect(runtime);
    require(runtime.live_objects() == 1, "Failed initializer retained a local or snapshot.");
}

std::string reload_source(unsigned factor) {
    return "void reload_tick(float delta, uint loops) { uint turn = 0; while (turn < loops) { "
           "self.advance(delta * " +
           std::to_string(factor) +
           ".0); "
           "self.state.cycles = self.state.cycles + " +
           std::to_string(factor) +
           "; turn = turn + 1; } "
           "self.state.heat = self.state.heat + " +
           std::to_string(factor) + ".0; }";
}

void require_code_state(std::uintptr_t address, bool alive) {
    DWORD64 image_base{};
    const auto* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory),
            "VirtualQuery failed to inspect generated code.");
    if (alive)
        require(function != nullptr && memory.State == MEM_COMMIT && (memory.Protect & 0xff) == PAGE_EXECUTE_READ,
                "A live code owner lost executable storage or unwind data.");
    else
        require(function == nullptr && memory.State == MEM_FREE, "Retired code retained storage or unwind data.");
}

void verify_reload_request_order(Runtime& runtime, const Binding& binding) {
    auto program = take(BusinessProgram::create(runtime, binding.type));
    auto initial = take(BusinessProgram::prepare(take(program->request("initial.source", reload_source(3)))));
    take(program->try_publish(initial));
    auto receiver = take(runtime.create(binding.type));
    set_machine(runtime, binding, receiver.id(), 7.0f, 8.0f, 9);
    const auto current = ProgramAccess::address(take(program->pin()));
    auto previous = take(BusinessProgram::prepare(take(program->request("previous.source", reload_source(4)))));
    auto newest = take(program->request("missing-poll.source", reload_source(5)));
    auto failed = ProgramAccess::prepare(newest, atelier_compile_business_without_poll_for_verification);
    require(!failed && !program->try_publish(previous) && static_cast<bool>(previous) &&
                ProgramAccess::address(take(program->pin())) == current,
            "Real link failure consumed an older candidate or replaced the published version.");
    (void)take(invoke_machine(program->entry(), receiver.id(), 0.5f, 1));
    check_machine(runtime, binding, receiver.id(), 8.5f, 11.0f, 12);
}

void verify_reload_owner_destruction(Runtime& runtime, const Binding& binding) {
    auto program = take(BusinessProgram::create(runtime, binding.type));
    auto candidate = take(BusinessProgram::prepare(take(program->request("closed-owner.source", reload_source(1)))));
    take(program->try_publish(candidate));
    auto lease = take(program->pin());
    const auto address = ProgramAccess::address(lease);
    auto receiver = take(runtime.create(binding.type));
    auto active_entry = std::make_unique<BusinessProgram::Entry>(program->entry());
    struct Closing {
        std::unique_ptr<BusinessProgram>& program;

        std::unique_ptr<BusinessProgram::Entry>& entry;

        std::uintptr_t address;
    } state{program, active_entry, address};
    NativeAction close{&state, [](void* context, ObjectId) noexcept -> Result<void> {
                           auto& state = *static_cast<Closing*>(context);
                           state.program.reset();
                           state.entry.reset();
                           require_code_state(state.address, true);
                           return std::unexpected(
                               Error{ErrorCode::invocation_failed, "Program closed inside failing native call."});
                       }};
    ExecutionStats stats;
    {
        ActionScope scope(close);
        require(!invoke_machine(*active_entry, receiver.id(), 0.5f, 1, &stats),
                "Owner destruction lost its native failure.");
    }
    require(!program && !active_entry && stats.explicit_cleanup && runtime.root_count() == 1,
            "Owner destruction lost execution cleanup.");
    require_code_state(address, true);
    (void)take(invoke_machine(lease, receiver.id(), 0.5f, 1, &stats));
    check_machine(runtime, binding, receiver.id(), 1.0f, 1.0f, 1);
    lease = {};
    require_code_state(address, false);
    collect(runtime);
    require(stats.explicit_cleanup && runtime.root_count() == 1 && runtime.live_objects() == 1,
            "Owner deletion or final lease release retained roots or source objects.");
}

#include "typed_business.inl"
#include "coroutine_business.inl"
}

void verify_math_abi();

void verify_business_execution() {
    verify_math_abi();
    verify_frame_protocol();
    {
        Runtime runtime;
        const auto binding = take(bind_type(runtime, register_machine(runtime)));
        verify_reference_returns(runtime, binding);
        verify_stale_source_references(runtime, binding);
        collect(runtime);
        verify_reference_failed_initializer(runtime, binding);
        collect(runtime);
        verify_reload_request_order(runtime, binding);
        collect(runtime);
        verify_reload_owner_destruction(runtime, binding);
    }
    verify_typed_business();
    verify_coroutines();
    require(NativeState::live == 0, "Business acceptance leaked native state instances.");
    std::puts("Business execution acceptance: PASS");
}
}
