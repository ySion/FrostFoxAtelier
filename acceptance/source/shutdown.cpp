#include "../helpers.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"

#include <windows.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void code_state(std::uintptr_t address, bool live) {
    require(address != 0, "Shutdown code address is missing.");
    DWORD64 image{};
    const auto* function = RtlLookupFunctionEntry(address, &image, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory),
            "Shutdown code memory query failed.");
    require(live ? function && memory.State == MEM_COMMIT : !function && memory.State == MEM_FREE,
            "Shutdown retired code too early or retained code after the last owner.");
}

struct HostOwner;

thread_local HostOwner* active_owner{};

struct Signal {
    static inline unsigned live{};

    Signal() { ++live; }

    ~Signal() noexcept { --live; }

    void signal(std::uint32_t mode);
};

// A concrete single-threaded consumer: ownership order is visible here rather
// than hidden in a scheduler. Runtime is the first member and the last released.
struct HostOwner {
    std::optional<Runtime> runtime{std::in_place};

    TypeId type;

    ObjectRoot receiver;

    std::optional<Program> program;

    Program::Entry entry;

    std::array<Coroutine, 5> tasks;

    bool stopping{};

    bool busy_in_callback{};

    std::uintptr_t current_address{};

    HostOwner() {
        type = take(NativeRegistration<Signal>{*runtime, "stage.Signal", TypeKind::object}
                        .method<&Signal::signal>("signal")
                        .publish());
        program.emplace(*runtime, type);
        receiver = take(runtime->create(type));
    }

    void publish(std::string_view text) {
        auto candidate = take(Program::prepare(take(program->request("shutdown.source", text))));
        take(program->try_publish(candidate));
    }

    Result<Coroutine> start(std::uint32_t mode) {
        if (stopping)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Host is stopping."});
        const std::array arguments{Scalar{mode}};
        return entry.start(receiver.id(), arguments);
    }

    void request_stop() noexcept {
        if (stopping)
            return;
        stopping = true;
        program->close();
        for (auto& task : tasks)
            task.cancel();
    }

    Result<void> finish_shutdown() {
        if (!runtime)
            return {};
        require(stopping, "Shutdown must first stop new work.");
        // Covers synchronous execution and native/traversal callbacks as well
        // as the tasks owned here. Busy must leave Runtime and frames intact.
        auto quiet = runtime->check_code_publication();
        if (!quiet)
            return quiet;
        for (const auto& task : tasks)
            require(task.state() != CoroutineState::running, "A running task reached shutdown teardown.");
        for (auto& task : tasks)
            task = {};
        program.reset();
        receiver.reset();
        require(runtime->root_count() == 0, "Shutdown retained task or receiver roots.");
        runtime.reset();
        require(Signal::live == 0, "Shutdown leaked native objects.");
        return {};
    }
};

struct ActiveOwner {
    HostOwner* previous;

    explicit ActiveOwner(HostOwner& owner) : previous(std::exchange(active_owner, &owner)) {}

    ~ActiveOwner() { active_owner = previous; }
};

void Signal::signal(std::uint32_t mode) {
    if (mode == 2)
        throw std::runtime_error("Shutdown fixture failure.");
    if (mode != 1)
        return;
    require(active_owner != nullptr, "Shutdown callback has no host owner.");
    auto& owner = *active_owner;
    owner.request_stop();
    auto finished = owner.finish_shutdown();
    owner.busy_in_callback = !finished && finished.error().code == ErrorCode::busy;
    require(owner.busy_in_callback && owner.runtime.has_value(), "Shutdown destroyed an active Runtime.");
    code_state(owner.current_address, true);
}

void verify_mixed_shutdown() {
    HostOwner owner;
    ActiveOwner active(owner);
    owner.publish(R"(
        async uint run(uint mode) {
            stage.Signal held = new stage.Signal();
            yield;
            self.signal(mode);
            return mode + 100;
        })");
    owner.entry = take(owner.program->bind<std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
    owner.current_address = take(owner.program->pin()).address();
    owner.tasks[0] = take(owner.start(0));
    owner.tasks[1] = take(owner.start(0));
    take(owner.tasks[1].resume());
    for (std::size_t index = 2; index < owner.tasks.size(); ++index) {
        const auto mode = index == 2 ? 1u : index == 3 ? 0u : 2u;
        owner.tasks[index] = take(owner.start(mode));
        require(take(owner.tasks[index].resume()) == CoroutineState::suspended, "New task failed to suspend.");
    }
    require(take(owner.tasks[3].resume()) == CoroutineState::completed, "Completion setup failed.");
    require(!owner.tasks[4].resume() && owner.tasks[4].state() == CoroutineState::failed, "Failure setup failed.");

    // The real native call requests shutdown while this generated frame runs.
    require(take(owner.tasks[2].resume()) == CoroutineState::cancelled, "Running shutdown did not defer cancellation.");
    require(owner.busy_in_callback, "Shutdown did not reject teardown from the active callback.");
    for (std::size_t index = 0; index < 3; ++index)
        require(owner.tasks[index].state() == CoroutineState::cancelled && !owner.tasks[index].result(),
                "Shutdown left an unfinished task or committed a cancelled result.");
    require(owner.tasks[3].state() == CoroutineState::completed &&
                std::get<std::uint32_t>(*take(owner.tasks[3].result())) == 100,
            "Shutdown changed an already completed result.");
    require(owner.tasks[4].state() == CoroutineState::failed && !owner.tasks[4].result(),
            "Shutdown published an already failed task's result.");
    require(owner.runtime->root_count() == 1, "Cancelled tasks retained roots beyond the host receiver.");
    code_state(owner.current_address, false);
    take(owner.finish_shutdown());
}

void verify_synchronous_shutdown() {
    HostOwner owner;
    ActiveOwner active(owner);
    owner.publish("uint run(uint mode) { self.signal(mode); return 23; }");
    owner.entry = take(owner.program->bind<std::uint32_t, std::uint32_t>());
    owner.current_address = take(owner.program->pin()).address();
    const std::array arguments{Scalar{std::uint32_t{1}}};
    const auto result = take(owner.entry.invoke(owner.receiver.id(), arguments));
    require(result && std::get<std::uint32_t>(*result) == 23 && owner.busy_in_callback,
            "Closing during a synchronous callback invalidated the active call or Runtime.");
    code_state(owner.current_address, false);
    take(owner.finish_shutdown());
}
}

void verify_source_shutdown() {
    verify_mixed_shutdown();
    verify_synchronous_shutdown();
    std::puts(
        "Source shutdown: PASS (mixed states, deferred cancellation, synchronous callback, code/root retirement)");
}
