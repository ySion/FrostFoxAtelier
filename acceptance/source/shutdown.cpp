#include "../helpers.hpp"
#include "source/detail/bridge.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
#include "foundation/tasks.hpp"
#include "memory/fatal.hpp"

#include <windows.h>
#include <array>
#include <cstdio>
#include <utility>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void code_state(std::uintptr_t address, bool live) noexcept {
    if (address == 0)
        fatal("Shutdown code address is missing.");
    DWORD64 image{};
    const auto* function = RtlLookupFunctionEntry(address, &image, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) != sizeof(memory))
        fatal("Shutdown code memory query failed.");
    if (!(live ? function && memory.State == MEM_COMMIT : !function && memory.State == MEM_FREE))
        fatal("Shutdown retired code too early or retained code after the last owner.");
}

struct HostOwner;

thread_local HostOwner* active_owner{};

struct Signal {
    static inline unsigned live{};

    Signal() { ++live; }

    ~Signal() noexcept { --live; }

    Result<void> signal(std::uint32_t mode) noexcept;
};

// A concrete single-threaded consumer: ownership order is visible here rather
// than hidden in a scheduler. Runtime is the first member and the last released.
struct HostOwner {
    std::optional<Runtime> runtime{std::in_place};

    TypeId type;

    ObjectRoot receiver;

    std::unique_ptr<Program> program;

    Program::Entry entry;

    Program::Typed<std::uint32_t, std::uint32_t> synchronous;

    Program::Typed<std::uint32_t, std::uint32_t> following;

    std::array<Coroutine, 5> tasks;

    bool stopping{};

    bool busy_in_callback{};

    std::uintptr_t current_address{};

    HostOwner() {
        type = take(NativeRegistration<Signal>{*runtime, "stage.Signal", TypeKind::object}
                        .method<&Signal::signal>("signal")
                        .publish());
        program = take(Program::create(*runtime, type));
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
        synchronous = {};
        following = {};
        for (auto& task : tasks)
            task.cancel();
    }

    Result<void> finish_shutdown() noexcept {
        if (!runtime)
            return {};
        if (!stopping)
            fatal("Shutdown must first stop new work.");
        // Covers synchronous execution and native/traversal callbacks as well
        // as the tasks owned here. Busy must leave Runtime and frames intact.
        auto quiet = runtime->check_code_publication();
        if (!quiet)
            return quiet;
        for (const auto& task : tasks)
            if (task.state() == CoroutineState::running)
                fatal("A running task reached shutdown teardown.");
        for (auto& task : tasks)
            task = {};
        program.reset();
        receiver.reset();
        if (runtime->root_count() != 0)
            fatal("Shutdown retained task or receiver roots.");
        runtime.reset();
        if (Signal::live != 0)
            fatal("Shutdown leaked native objects.");
        return {};
    }
};

struct ActiveOwner {
    HostOwner* previous;

    explicit ActiveOwner(HostOwner& owner) : previous(std::exchange(active_owner, &owner)) {}

    ~ActiveOwner() { active_owner = previous; }
};

Result<void> Signal::signal(std::uint32_t mode) noexcept {
    if (mode == 2)
        return std::unexpected(Error{ErrorCode::invocation_failed, "Shutdown fixture failure."});
    if (mode != 1)
        return {};
    if (!active_owner)
        fatal("Shutdown callback has no host owner.");
    auto& owner = *active_owner;
    owner.request_stop();
    auto finished = owner.finish_shutdown();
    owner.busy_in_callback = !finished && finished.error().code == ErrorCode::busy;
    if (!owner.busy_in_callback || !owner.runtime)
        fatal("Shutdown destroyed an active Runtime.");
    code_state(owner.current_address, true);
    return {};
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
    owner.entry = owner.program->entry();
    owner.current_address = source::detail::ProgramAccess::address(take(owner.program->pin()));
    owner.tasks[0] = take(owner.start(0));
    owner.tasks[1] = take(owner.start(0));
    take(owner.tasks[1].resume());
    for (std::size_t index = 2; index < owner.tasks.size(); ++index) {
        const auto mode = index == 2 ? 1u : index == 3 ? 0u : 2u;
        owner.tasks[index] = take(owner.start(mode));
        require(take(owner.tasks[index].resume()) == CoroutineState::suspended, "New task failed to suspend.");
    }

    // Ordinary-value work may fail while Source frames remain suspended. Join
    // must finish nested cleanup and restore the caller before Runtime resumes.
    unsigned cleaned{};
    bool joined_failure{};
    struct Cleanup {
        unsigned& count;

        ~Cleanup() { ++count; }
    };
    {
        tasks::JoinGroup computation;
        computation.run([&cleaned]() noexcept -> std::expected<void, tasks::Failure> {
            Cleanup outer{cleaned};
            const auto nested =
                tasks::parallel_for({0, 1}, [&cleaned](tasks::Range) noexcept -> std::expected<void, tasks::Failure> {
                    Cleanup inner{cleaned};
                    return std::unexpected(tasks::Failure{31});
                });
            if (!nested)
                return std::unexpected(nested.error());
            return {};
        });
        const auto joined = computation.join();
        joined_failure = !joined && joined.error().code == 31 && joined.error().item == SIZE_MAX;
    }
    require(joined_failure && cleaned == 2 && !tasks::in_parallel_body(),
            "Failed computation returned before cleanup or retained its body marker.");
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
    owner.following = take(owner.program->entry().bind<std::uint32_t, std::uint32_t>());
    owner.synchronous = take(take(owner.program->pin()).bind<std::uint32_t, std::uint32_t>());
    owner.current_address = source::detail::ProgramAccess::address(take(owner.program->pin()));
    owner.publish("uint run(uint mode) { self.signal(mode); return 61; }");
    require(take(owner.synchronous.invoke(owner.receiver.id(), 0)) == 23 &&
                take(owner.following.invoke(owner.receiver.id(), 0)) == 61,
            "Fixed typed binding followed a new publication or following binding retained the old version.");
    owner.program->close();
    require(!owner.following.invoke(owner.receiver.id(), 0), "Closing the program left following typed code callable.");
    require(take(owner.synchronous.invoke(owner.receiver.id(), 0)) == 23,
            "Closing the program invalidated its pinned typed binding.");
    // The callback clears the final public typed handle while its call is active.
    {
        auto outer = take(ExecutionScope::enter(*owner.runtime));
        const auto result = take(owner.synchronous.invoke(owner.receiver.id(), 1));
        require(result == 23 && owner.busy_in_callback,
                "Releasing a typed handle inside its callback invalidated the active call or Runtime.");
        code_state(owner.current_address, true);
    }
    code_state(owner.current_address, false);
    take(owner.finish_shutdown());
}

void verify_following_shutdown() {
    HostOwner owner;
    ActiveOwner active(owner);
    owner.program = take(Program::create(*owner.runtime));
    source::SourceSnapshot snapshot{"shutdown", 1, {{"Source/Shutdown.source", 1, R"(
            class stage.Script : stage.Signal {
                uint run(uint mode) { self.signal(mode); return 37 / (mode - 1); }
            })"}}};
    auto candidate = take(Program::prepare(take(owner.program->request(snapshot, {.diagnostics = true}))));
    take(owner.program->try_publish(candidate));
    candidate = {};
    owner.receiver = take(owner.runtime->create(take(owner.runtime->find_type("stage.Script"))));
    owner.following = take(take(owner.program->entry("stage.Script.run")).bind<std::uint32_t, std::uint32_t>());
    owner.current_address = source::detail::ProgramAccess::address(take(owner.program->pin("stage.Script.run")));
    source::Diagnostics diagnostics;
    // close releases the entire module; no pinned Lease or public Typed remains.
    // The subsequent generated failure must still locate its source and finish capture.
    const auto result = owner.following.invoke(owner.receiver.id(), 1, &diagnostics);
    require(!result && result.error().source && result.error().source->path == "Source/Shutdown.source" &&
                owner.busy_in_callback && owner.runtime->root_count() == 1,
            "Following shutdown lost error metadata, execution lifetime or root cleanup.");
    code_state(owner.current_address, false);
    take(owner.finish_shutdown());
}
}

void verify_source_shutdown() {
    verify_mixed_shutdown();
    verify_synchronous_shutdown();
    verify_following_shutdown();
    std::puts(
        "Source shutdown: PASS (joined failure, mixed states, deferred cancellation, synchronous callback, code/root retirement)");
}
