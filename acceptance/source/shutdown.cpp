#include "../helpers.hpp"
#include "source/detail/bridge.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"
#include <windows.h>
#include <cstdio>
#include <functional>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

void code_state(std::uintptr_t address, bool live) {
    DWORD64 image{};
    const auto* function = RtlLookupFunctionEntry(address, &image, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory),
            "Cannot inspect generated code.");
    require(live ? function && memory.State == MEM_COMMIT : !function && memory.State == MEM_FREE,
            "Generated code retired before return or survived its last owner.");
}

struct Signal {
    static inline unsigned live{};

    static inline std::function<Result<void>()> stop;

    Signal() { ++live; }

    ~Signal() { --live; }

    Result<void> signal() { return stop(); }
};
}

void verify_source_shutdown() {
    Optional<Runtime> runtime{std::in_place};
    const auto type = take(NativeRegistration<Signal>{*runtime, "shutdown.Signal", TypeKind::object}
                               .method<&Signal::signal>("signal")
                               .publish());
    auto receiver = take(runtime->create(type));
    auto program = take(source::Program::create(*runtime, type));
    auto candidate = take(source::Program::prepare(take(program->request("shutdown.source", R"(
        async uint run() {
            shutdown.Signal held = new shutdown.Signal();
            yield;
            self.signal();
            return 100;
        })"))));
    take(program->try_publish(candidate));
    const auto address = source::detail::ProgramAccess::address(take(program->pin()));
    auto task = take(program->entry().start(receiver.id(), {}));
    require(take(task.resume()) == CoroutineState::suspended, "Shutdown task did not suspend.");
    Signal::stop = [&]() -> Result<void> {
        program->close();
        task.cancel();
        require(!runtime->check_code_publication(), "An active callback admitted Runtime teardown.");
        code_state(address, true);
        return {};
    };
    require(take(task.resume()) == CoroutineState::cancelled && !task.result() && runtime->root_count() == 1,
            "Cancelled execution published a result or retained roots.");
    code_state(address, false);
    Signal::stop = {};
    task = {};
    program.reset();
    receiver.reset();
    require(runtime->root_count() == 0, "Shutdown retained a root.");
    runtime.reset();
    require(Signal::live == 0, "Shutdown leaked native objects.");
    std::puts("Source shutdown: PASS (callback cancellation and code/root retirement)");
}
