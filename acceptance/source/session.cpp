#include "../helpers.hpp"
#include "source/program.hpp"
#include "runtime/native.hpp"

#include <windows.h>
#include <psapi.h>
#include <mimalloc-stats.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace atelier;

using source::Program;

using Clock = std::chrono::steady_clock;

constexpr std::size_t warmup_cycles = 4;

constexpr std::size_t measured_cycles = 32;

constexpr std::int64_t live_byte_tolerance = 64 * 1024;

using namespace atelier::acceptance::helpers;

double elapsed_ns(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

struct Snapshot {
    std::int64_t live_bytes{};

    std::int64_t private_bytes{};
};

Snapshot snapshot() {
    // Checkpoint-only: flush mimalloc's page counters outside timed work.
    // This changes retained pages, so PrivateUsage is not an untouched peak.
    mi_collect(true);
    mi_stats_t stats;
    mi_stats_init(&stats);
    require(mi_stats_get(&stats), "Session allocator statistics are unavailable.");
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)) != 0,
            "Session process memory query failed.");
    return {stats.malloc_normal.current + stats.malloc_huge.current, static_cast<std::int64_t>(memory.PrivateUsage)};
}

void verify_statistics() {
    const auto before = snapshot();
    auto* block = ::operator new(4096);
    const auto allocated = snapshot();
    ::operator delete(block);
    const auto released = snapshot();
    require(allocated.live_bytes >= before.live_bytes + 4096 && released.live_bytes < allocated.live_bytes,
            "Session allocator statistics did not observe an actual allocation and release.");
}

void code_state(std::uintptr_t address, bool live) {
    require(address != 0, "Session code address is missing.");
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
        take(runtime.collect_step({4096}));
    // A fresh cycle also collects objects created during the previous sweep.
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({4096}));
}

struct Counter {
    static inline std::size_t live{};

    std::uint32_t value{};

    Counter() { ++live; }

    ~Counter() noexcept { --live; }
};

TypeId register_counter(Runtime& runtime, bool native) {
    if (native)
        return take(NativeRegistration<Counter>{runtime, "session.Counter", TypeKind::object}
                        .field<&Counter::value>("value")
                        .publish());
    TypeDefinition definition;
    definition.name = "session.Counter";
    definition.fields = {{"value", {take(runtime.native_type<std::uint32_t>())}}};
    return take(runtime.register_type(std::move(definition)));
}

void publish(Program& program, std::uint32_t generation) {
    const auto source_text = std::string("async uint work(uint seed, uint steps) {"
                                         " session.Counter held = new session.Counter(); held.value = seed;"
                                         " uint index = 0; while (index < steps) {"
                                         " held.value = held.value + 1; yield; index = index + 1; }"
                                         " self.value = self.value + 1; return held.value + ") +
                             std::to_string(generation) + "; }";
    auto candidate = take(Program::prepare(take(program.request("continuous.source", source_text))));
    take(program.try_publish(candidate));
}

struct Pending {
    Coroutine task;

    std::uint32_t seed{};

    std::uint32_t steps{};

    std::uint32_t generation{};

    std::uint32_t resumes{};
};

struct Totals {
    std::uint32_t created{};

    std::uint32_t completed{};

    std::uint32_t cancelled_ready{};

    std::uint32_t cancelled_suspended{};

    std::uint32_t old_completions{};

    std::size_t ticks{};

    std::size_t gc_steps{};

    std::size_t retired_versions{};

    double max_tick_ns{};
};

void advance(Runtime& runtime, std::vector<Pending>& tasks, std::uint32_t current, Totals& totals) {
    for (std::size_t index = 0; index < tasks.size();) {
        auto& pending = tasks[index];
        if (pending.resumes == 0 && pending.seed % 8 == 0) {
            pending.task.cancel();
            ++totals.cancelled_ready;
        } else if (pending.resumes >= 2 && pending.seed % 5 == 0) {
            pending.task.cancel();
            ++totals.cancelled_suspended;
        } else {
            const auto state = take(pending.task.resume());
            ++pending.resumes;
            if (state == CoroutineState::suspended) {
                ++index;
                continue;
            }
            require(state == CoroutineState::completed, "Session task entered an unexpected terminal state.");
            const auto result = take(pending.task.result());
            require(result && std::get<std::uint32_t>(*result) == pending.seed + pending.steps + pending.generation,
                    "Session task lost its state or resumed a different code version.");
            ++totals.completed;
            if (pending.generation != current)
                ++totals.old_completions;
        }
        if (pending.task.state() == CoroutineState::cancelled)
            require(!pending.task.result(), "Cancelled session task published a result.");
        if (index + 1 != tasks.size())
            tasks[index] = std::move(tasks.back());
        tasks.pop_back();
    }
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    require(take(runtime.collect_step({16})).work_done <= 16, "Session GC exceeded its work budget.");
    ++totals.gc_steps;
}

void verify_session(bool native) {
    const char* model = native ? "native" : "described";
    Totals totals;
    Snapshot baseline{};
    Snapshot final{};
    Snapshot maximum{};
    std::array<double, measured_cycles> retirement_ns{};
    const auto session_started = Clock::now();
    {
        Runtime runtime;
        const auto type = register_counter(runtime, native);
        const auto field = take(runtime.find_field(type, "value"));
        auto receiver = take(runtime.create(type));
        Program program(runtime, type);
        std::uint32_t generation = 1000;
        publish(program, generation);
        const auto entry =
            take(program.bind<std::uint32_t, std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
        std::vector<Pending> tasks;
        tasks.reserve(128); // A fixed bounded workload; capacity is outside checkpoints.

        for (std::size_t cycle = 0; cycle < warmup_cycles + measured_cycles; ++cycle) {
            // Hold the preceding version and each replacement. Four old units
            // are released together after all their task frames have exited.
            std::array<Program::Lease, 5> leases;
            std::array<std::uintptr_t, 5> addresses{};
            leases[0] = take(program.pin());
            addresses[0] = leases[0].address();
            for (std::size_t tick = 0; tick < 32; ++tick) {
                if (tick % 8 == 0) {
                    publish(program, ++generation);
                    const auto version = tick / 8 + 1;
                    leases[version] = take(program.pin());
                    addresses[version] = leases[version].address();
                }
                const auto started = Clock::now(); // Compilation is outside host tick timing.
                for (std::uint32_t spawn = 0; spawn < 8; ++spawn) {
                    const auto seed = ++totals.created;
                    const auto steps = 2 + seed % 11;
                    const std::array arguments{Scalar{seed}, Scalar{steps}};
                    require(tasks.size() < 128, "Session exceeded its bounded task population.");
                    tasks.push_back({take(entry.start(receiver.id(), arguments)), seed, steps, generation, 0});
                }
                advance(runtime, tasks, generation, totals);
                const auto duration = elapsed_ns(started);
                if (cycle >= warmup_cycles)
                    totals.max_tick_ns = std::max(totals.max_tick_ns, duration);
                ++totals.ticks;
            }
            while (!tasks.empty())
                advance(runtime, tasks, generation, totals);
            collect_to_rest(runtime);
            require(runtime.root_count() == 1 && runtime.live_objects() == 1 && runtime.pending_reclamation() == 0,
                    "Session checkpoint retained task roots or objects.");
            require(std::get<std::uint32_t>(take(runtime.read(receiver.id(), field))) == totals.completed,
                    "Session completion effects do not match validated results.");
            require(Counter::live == (native ? 1u : 0u), "Session native object lifetime count drifted.");
            for (const auto address : addresses)
                code_state(address, true);
            const auto started = Clock::now();
            for (std::size_t index = 0; index < 4; ++index)
                leases[index] = {};
            const auto duration = elapsed_ns(started);
            for (std::size_t index = 0; index < 4; ++index)
                code_state(addresses[index], false);
            totals.retired_versions += 4;
            leases[4] = {};
            code_state(addresses[4], true); // Only Program retains the current unit.

            const auto memory = snapshot();
            if (cycle + 1 == warmup_cycles)
                baseline = maximum = memory;
            if (cycle >= warmup_cycles) {
                const auto sample = cycle - warmup_cycles;
                retirement_ns[sample] = duration;
                final = memory;
                maximum.live_bytes = std::max(maximum.live_bytes, memory.live_bytes);
                maximum.private_bytes = std::max(maximum.private_bytes, memory.private_bytes);
                std::printf("{\"kind\":\"source_session_checkpoint\",\"model\":\"%s\",\"cycle\":%zu,"
                            "\"live_byte_delta\":%lld,\"private_byte_delta\":%lld,\"retire_four_ns\":%.0f,"
                            "\"roots\":1,\"objects\":1}\n",
                            model, sample, static_cast<long long>(memory.live_bytes - baseline.live_bytes),
                            static_cast<long long>(memory.private_bytes - baseline.private_bytes), duration);
                require(memory.live_bytes <= baseline.live_bytes + live_byte_tolerance,
                        "Session live allocation drift exceeded the 64-KiB checkpoint tolerance.");
            }
        }
        require(totals.completed && totals.cancelled_ready && totals.cancelled_suspended && totals.old_completions,
                "Session missed completion, cancellation or old-version execution.");
        require(totals.created == totals.completed + totals.cancelled_ready + totals.cancelled_suspended,
                "Session lost a task owner.");

        // Finish the same long-lived host with both ready and suspended work.
        for (std::uint32_t index = 0; index < 32; ++index) {
            const std::array arguments{Scalar{index}, Scalar{std::uint32_t{3}}};
            tasks.push_back({take(entry.start(receiver.id(), arguments)), index, 3, generation, 0});
            if (index % 2 != 0)
                require(take(tasks.back().task.resume()) == CoroutineState::suspended, "Shutdown setup failed.");
        }
        const auto current_address = take(program.pin()).address();
        program.close();
        const std::array closed_arguments{Scalar{std::uint32_t{0}}, Scalar{std::uint32_t{1}}};
        require(!entry.start(receiver.id(), closed_arguments), "Session closed entry started work.");
        for (auto& pending : tasks) {
            pending.task.cancel();
            require(pending.task.state() == CoroutineState::cancelled, "Session shutdown did not cancel a task.");
        }
        take(runtime.check_code_publication());
        tasks.clear();
        code_state(current_address, false);
        receiver.reset();
        collect_to_rest(runtime);
        require(runtime.root_count() == 0 && runtime.live_objects() == 0 && runtime.pending_reclamation() == 0,
                "Session shutdown retained runtime resources.");
    }
    require(Counter::live == 0, "Session Runtime destruction leaked native objects.");
    const auto elapsed = elapsed_ns(session_started);
    std::sort(retirement_ns.begin(), retirement_ns.end());
    const auto median = (retirement_ns[measured_cycles / 2 - 1] + retirement_ns[measured_cycles / 2]) / 2;
    std::printf("{\"kind\":\"source_session_summary\",\"model\":\"%s\",\"ticks\":%zu,"
                "\"tasks\":%u,\"completed\":%u,\"cancelled_ready\":%u,\"cancelled_suspended\":%u,"
                "\"completed_old_version\":%u,\"gc_steps\":%zu,\"retired_old_versions\":%zu,"
                "\"max_live_byte_delta\":%lld,\"final_live_byte_delta\":%lld,\"live_byte_tolerance\":%lld,"
                "\"max_private_byte_delta\":%lld,\"final_private_byte_delta\":%lld,"
                "\"retire_four_median_ns\":%.0f,\"retire_four_max_ns\":%.0f,\"max_tick_ns\":%.0f,"
                "\"wall_ns\":%.0f,\"shutdown_roots\":0,\"shutdown_objects\":0,\"status\":\"PASS\"}\n",
                model, totals.ticks, totals.created, totals.completed, totals.cancelled_ready,
                totals.cancelled_suspended, totals.old_completions, totals.gc_steps, totals.retired_versions,
                static_cast<long long>(maximum.live_bytes - baseline.live_bytes),
                static_cast<long long>(final.live_bytes - baseline.live_bytes),
                static_cast<long long>(live_byte_tolerance),
                static_cast<long long>(maximum.private_bytes - baseline.private_bytes),
                static_cast<long long>(final.private_bytes - baseline.private_bytes), median, retirement_ns.back(),
                totals.max_tick_ns, elapsed);
}
}

void verify_source_session() {
    verify_statistics();
    verify_session(true);
    verify_session(false);
    std::puts("Source continuous session: PASS (same Runtime, mixed tasks, GC, updates, retirement, shutdown)");
}
