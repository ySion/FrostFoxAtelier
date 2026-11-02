#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"
#include "source/program.hpp"

#include <windows.h>
#include <array>
#include <cstdio>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void code_state(std::uintptr_t address, bool live) {
    DWORD64 image{};
    const auto* unwind = RtlLookupFunctionEntry(address, &image, nullptr);
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) == sizeof(memory),
            "Session code memory query failed.");
    require(live ? unwind && memory.State == MEM_COMMIT : !unwind && memory.State == MEM_FREE,
            "Session code or unwind metadata has an incorrect lifetime.");
}

void collect_to_rest(Runtime& runtime) {
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
}

struct Counter {
    static inline std::size_t live{};

    std::uint32_t value{};

    Counter() { ++live; }

    ~Counter() noexcept { --live; }
};

void publish(Program& program, std::uint32_t generation) {
    const auto text = std::string("async uint work(uint seed, uint steps) {"
                                  " session.Counter held = new session.Counter(); held.value = seed;"
                                  " uint index = 0; while (index < steps) {"
                                  " held.value = held.value + 1; yield; index = index + 1; }"
                                  " self.value = self.value + 1; return held.value + ") +
                      std::to_string(generation) + "; }";
    auto candidate = take(Program::prepare(take(program.request("continuous.source", text))));
    take(program.try_publish(candidate));
}
}

void verify_source_session() {
    Runtime runtime;
    const auto type = take(NativeRegistration<Counter>{runtime, "session.Counter", TypeKind::object}
                               .field<&Counter::value>("value")
                               .publish());
    auto receiver = take(runtime.create(type));
    Program program(runtime, type);
    publish(program, 1000);
    const auto entry = take(program.bind<std::uint32_t, std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
    auto old = take(program.pin());
    const auto old_address = source::detail::ProgramAccess::address(old);
    std::array<Coroutine, 4> tasks;
    tasks[0] = take(entry.start(receiver.id(), std::array{Scalar{1u}, Scalar{2u}}));
    take(tasks[0].resume());
    publish(program, 1001);
    auto current = take(program.pin());
    const auto current_address = source::detail::ProgramAccess::address(current);
    tasks[1] = take(entry.start(receiver.id(), std::array{Scalar{8u}, Scalar{2u}}));
    tasks[2] = take(entry.start(receiver.id(), std::array{Scalar{5u}, Scalar{2u}}));
    tasks[3] = take(entry.start(receiver.id(), std::array{Scalar{3u}, Scalar{2u}}));
    tasks[1].cancel();
    take(tasks[2].resume());
    take(tasks[3].resume());
    collect_to_rest(runtime);
    take(tasks[0].resume());
    take(tasks[2].resume());
    take(tasks[3].resume());
    tasks[2].cancel();
    require(take(tasks[0].resume()) == CoroutineState::completed &&
                std::get<std::uint32_t>(*take(tasks[0].result())) == 1003 &&
                take(tasks[3].resume()) == CoroutineState::completed &&
                std::get<std::uint32_t>(*take(tasks[3].result())) == 1006 &&
                tasks[1].state() == CoroutineState::cancelled && !tasks[1].result() &&
                tasks[2].state() == CoroutineState::cancelled && !tasks[2].result(),
            "Interleaved tasks lost their version, result or cancellation state.");
    tasks = {};
    collect_to_rest(runtime);
    require(runtime.root_count() == 1 && runtime.live_objects() == 1 && runtime.pending_reclamation() == 0 &&
                Counter::live == 1 &&
                std::get<std::uint32_t>(take(runtime.read(receiver.id(), take(runtime.find_field(type, "value"))))) ==
                    2,
            "Session completion or cancellation retained task resources.");
    code_state(old_address, true);
    code_state(current_address, true);
    old = {};
    code_state(old_address, false);
    current = {};
    program.close();
    code_state(current_address, false);
    receiver.reset();
    collect_to_rest(runtime);
    require(runtime.root_count() == 0 && runtime.live_objects() == 0 && runtime.pending_reclamation() == 0 &&
                Counter::live == 0,
            "Session shutdown retained runtime resources.");
    std::puts("Source session: PASS (two interleaved versions, completion/cancellation, GC and retirement)");
}
