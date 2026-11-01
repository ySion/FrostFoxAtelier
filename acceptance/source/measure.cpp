#include "source/program.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "math_reference.hpp"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <mimalloc-stats.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace atelier;

using source::Program;

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

std::uint32_t uint_result(Result<std::optional<Scalar>> result) {
    auto value = take(std::move(result));
    require(value && std::holds_alternative<std::uint32_t>(*value), "Expected a uint result.");
    return std::get<std::uint32_t>(*value);
}

void publish(Program& program, std::string_view name, std::string_view text) {
    auto candidate = take(Program::prepare(take(program.request(name, text))));
    take(program.try_publish(candidate));
}

double nanoseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

struct Options {
    std::size_t batches{7};

    std::size_t calls{20000};

    std::size_t max_tasks{1024};
};

Options options(int argc, char** argv) {
    Options result;
    for (int index = 1; index < argc; index += 2) {
        require(index + 1 < argc, "Expected --batches N, --calls N or --max-tasks N.");
        const std::string_view value(argv[index + 1]);
        std::size_t number{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && number != 0,
                "Measurement sizes must be positive integers.");
        const std::string_view name(argv[index]);
        if (name == "--batches")
            result.batches = number;
        else if (name == "--calls")
            result.calls = number;
        else if (name == "--max-tasks")
            result.max_tasks = number;
        else
            throw std::runtime_error("Unknown measurement option.");
    }
    require(result.batches >= 3 && result.batches <= 31 && result.calls <= 1000000 && result.max_tasks <= 16384,
            "Use 3..31 batches, at most 1000000 calls and at most 16384 tasks.");
    return result;
}

class Series {
public:
    Series(const char* name, const char* model, std::size_t scale, std::size_t operations, std::size_t batches)
        : name_(name), model_(model), scale_(scale), operations_(operations) {
        values_.reserve(batches);
    }

    template <class F>
    void measure(F&& operation) {
        const auto started = Clock::now();
        operation();
        const auto elapsed = nanoseconds(started);
        values_.push_back(elapsed);
    }

    void output() const {
        require(!values_.empty(), "Cannot report an unmeasured case.");
        for (std::size_t index = 0; index < values_.size(); ++index)
            std::printf("{\"record\":\"sample\",\"case\":\"%s\",\"model\":\"%s\",\"scale\":%zu,"
                        "\"batch\":%zu,\"operations\":%zu,\"elapsed_ns\":%.0f,\"validated\":true}\n",
                        name_, model_, scale_, index, operations_, values_[index]);
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        const auto median = sorted.size() % 2 != 0 ? sorted[sorted.size() / 2]
                                                   : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2.0;
        std::printf("{\"record\":\"summary\",\"case\":\"%s\",\"model\":\"%s\",\"scale\":%zu,"
                    "\"batches\":%zu,\"operations_per_batch\":%zu,\"median_ns\":%.0f,\"max_ns\":%.0f,"
                    "\"median_ns_per_operation\":%.3f,\"max_ns_per_operation\":%.3f,\"validated\":true}\n",
                    name_, model_, scale_, sorted.size(), operations_, median, sorted.back(),
                    median / static_cast<double>(operations_), sorted.back() / static_cast<double>(operations_));
    }

private:
    const char* name_;

    const char* model_;

    std::size_t scale_;

    std::size_t operations_;

    std::vector<double> values_;
};

struct Resources {
    std::int64_t allocations{};

    std::int64_t allocated_bytes{};

    std::int64_t live_bytes{};

    std::int64_t committed_bytes{};

    std::int64_t private_bytes{};

    std::int64_t working_set_bytes{};
};

Resources resources() {
    // v3 accumulates block counters in pages. Flush outside timed regions so
    // current/total counters include the most recent allocations and frees.
    // Forced collection also changes allocator retention; these are explicitly
    // post-collection snapshots, not untouched process high-water marks.
    mi_collect(true);
    mi_stats_t statistics;
    mi_stats_init(&statistics);
    require(mi_stats_get(&statistics), "mimalloc stats ABI mismatch.");
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory)) != 0,
            "Cannot read process memory.");
    return {statistics.malloc_normal_count.total + statistics.malloc_huge_count.total,
            statistics.malloc_normal.total + statistics.malloc_huge.total,
            statistics.malloc_normal.current + statistics.malloc_huge.current,
            statistics.committed.current,
            static_cast<std::int64_t>(memory.PrivateUsage),
            static_cast<std::int64_t>(memory.WorkingSetSize)};
}

void resource_output(const char* model, std::size_t scale, const char* phase, const Resources& base,
                     const Resources& current, std::size_t roots, std::size_t objects) {
    std::printf("{\"record\":\"resources\",\"model\":\"%s\",\"scale\":%zu,\"phase\":\"%s\","
                "\"allocation_count_delta\":%lld,\"allocated_rounded_bytes_delta\":%lld,"
                "\"live_rounded_bytes_delta\":%lld,\"allocator_commit_bytes_delta\":%lld,"
                "\"process_private_bytes_delta\":%lld,\"working_set_bytes_delta\":%lld,"
                "\"root_count\":%zu,\"live_objects\":%zu,\"allocator_flushed\":true,\"validated\":true}\n",
                model, scale, phase, static_cast<long long>(current.allocations - base.allocations),
                static_cast<long long>(current.allocated_bytes - base.allocated_bytes),
                static_cast<long long>(current.live_bytes - base.live_bytes),
                static_cast<long long>(current.committed_bytes - base.committed_bytes),
                static_cast<long long>(current.private_bytes - base.private_bytes),
                static_cast<long long>(current.working_set_bytes - base.working_set_bytes), roots, objects);
}

void verify_allocator() {
    const auto before = resources();
    void* allocation = ::operator new(4096);
    require(mi_usable_size(allocation) >= 4096, "Global C++ allocation did not reach shared mimalloc.");
    const auto during = resources();
    ::operator delete(allocation);
    const auto after = resources();
    require(during.allocations > before.allocations && during.live_bytes - before.live_bytes >= 4096 &&
                after.live_bytes < during.live_bytes,
            "Allocator counters did not observe allocation and release.");
    std::puts("{\"record\":\"validation\",\"case\":\"shared_allocator_counters\",\"validated\":true}");
}

std::size_t collect(Runtime& runtime) {
    take(runtime.begin_collection());
    std::size_t work{};
    while (runtime.collection_phase() != CollectionPhase::idle)
        work += take(runtime.collect_step({4096})).work_done;
    return work;
}

void verify_retired(std::uintptr_t address) {
    DWORD64 image_base{};
    require(RtlLookupFunctionEntry(static_cast<DWORD64>(address), &image_base, nullptr) == nullptr,
            "Retired version retained an unwind entry.");
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != 0 &&
                memory.State == MEM_FREE,
            "Retired version retained code storage.");
}

constexpr std::string_view loop_source =
    "uint sum(uint count) { uint index = 0; uint total = 0; while (index < count) {"
    "total = total + index; index = index + 1; } return total; }";

void measure_compilation(const Options& settings) {
    Runtime runtime;
    Program program(runtime, {});
    const auto cold_start = Clock::now();
    publish(program, "warmup.source", loop_source);
    const auto cold_elapsed = nanoseconds(cold_start);
    const std::array args{Scalar{std::uint32_t{32}}};
    require(uint_result(program.entry().invoke({}, args)) == 496, "Uint loop preflight failed.");
    std::printf("{\"record\":\"warmup\",\"case\":\"first_request_prepare_publish\","
                "\"elapsed_ns\":%.0f,\"included_in_steady_samples\":false,\"validated\":true}\n",
                cold_elapsed);
    Series preparing("prepare", "uint_loop32", 1, 1, settings.batches);
    Series publishing("publish_pinned_old_version", "uint_loop32", 1, 1, settings.batches);
    Series retiring("last_old_lease_release", "uint_loop32", 1, 1, settings.batches);
    for (std::size_t batch = 0; batch < settings.batches; ++batch) {
        auto old = take(program.pin());
        const auto old_address = old.address();
        auto request = take(program.request("replacement.source", loop_source));
        Program::Candidate candidate;
        preparing.measure([&] { candidate = take(Program::prepare(request)); });
        publishing.measure([&] { take(program.try_publish(candidate)); });
        require(uint_result(program.entry().invoke({}, args)) == 496 && uint_result(old.invoke({}, args)) == 496,
                "Publication changed loop results.");
        DWORD64 image_base{};
        require(RtlLookupFunctionEntry(static_cast<DWORD64>(old_address), &image_base, nullptr) != nullptr,
                "Pinned old version lost its unwind entry.");
        retiring.measure([&] { old = {}; });
        verify_retired(old_address);
    }
    preparing.output();
    publishing.output();
    retiring.output();
}

void measure_scalar_calls(const Options& settings) {
    Runtime runtime;
    Program program(runtime, {});
    publish(program, "scalar-calls.source", loop_source);
    const auto entry = take(program.bind<std::uint32_t, std::uint32_t>());
    const auto lease = take(program.pin());
    const std::array args{Scalar{std::uint32_t{32}}};
    for (std::size_t warmup = 0; warmup < 256; ++warmup) {
        require(uint_result(entry.invoke({}, args)) == 496 && uint_result(lease.invoke({}, args)) == 496,
                "Scalar invocation preflight failed.");
    }
    Series entries("entry_invoke", "uint_loop32", settings.calls, settings.calls, settings.batches);
    Series leases("lease_invoke", "uint_loop32", settings.calls, settings.calls, settings.batches);
    for (std::size_t batch = 0; batch < settings.batches; ++batch) {
        std::uint64_t checksum{};
        entries.measure([&] {
            for (std::size_t index = 0; index < settings.calls; ++index)
                checksum += uint_result(entry.invoke({}, args));
        });
        require(checksum == 496 * settings.calls, "Entry loop checksum failed.");
        checksum = 0;
        leases.measure([&] {
            for (std::size_t index = 0; index < settings.calls; ++index)
                checksum += uint_result(lease.invoke({}, args));
        });
        require(checksum == 496 * settings.calls, "Lease loop checksum failed.");
    }
    entries.output();
    leases.output();
    const auto before = resources();
    std::uint64_t checksum{};
    for (std::size_t index = 0; index < settings.calls; ++index)
        checksum += uint_result(entry.invoke({}, args));
    const auto after = resources();
    require(checksum == 496 * settings.calls, "Scalar allocation sample failed.");
    resource_output("uint_loop32", settings.calls, "entry_calls", before, after, runtime.root_count(),
                    runtime.live_objects());
}

struct Counter {
    std::uint32_t value{};
};

TypeId counter_type(Runtime& runtime, bool native) {
    if (native)
        return take(NativeRegistration<Counter>{runtime, "measure.NativeCounter", TypeKind::object}
                        .field<&Counter::value>("value")
                        .publish());
    TypeDefinition definition;
    definition.name = "measure.DescribedCounter";
    definition.fields = {{"value", {take(runtime.native_type<std::uint32_t>())}}};
    return take(runtime.register_type(std::move(definition)));
}

void measure_object_calls(const Options& settings, bool native) {
    Runtime runtime;
    const auto type = counter_type(runtime, native);
    const auto field = take(runtime.find_field(type, "value"));
    auto receiver = take(runtime.create(type));
    Program program(runtime, type);
    publish(program, "object-calls.source",
            "uint advance(uint delta) { self.value = self.value + delta; return self.value; }");
    const auto entry = take(program.bind<std::uint32_t, std::uint32_t>());
    const auto lease = take(program.pin());
    const std::array args{Scalar{std::uint32_t{1}}};
    require(uint_result(entry.invoke(receiver.id(), args)) == 1 && uint_result(lease.invoke(receiver.id(), args)) == 2,
            "Object invocation preflight failed.");
    const char* model = native ? "native_object" : "described_object";
    Series entries("entry_invoke", model, settings.calls, settings.calls, settings.batches);
    Series leases("lease_invoke", model, settings.calls, settings.calls, settings.batches);
    const auto expected = static_cast<std::uint64_t>(settings.calls) * (settings.calls + 1) / 2;
    for (std::size_t batch = 0; batch < settings.batches; ++batch) {
        take(runtime.write(receiver.id(), field, std::uint32_t{0}));
        std::uint64_t checksum{};
        entries.measure([&] {
            for (std::size_t index = 0; index < settings.calls; ++index)
                checksum += uint_result(entry.invoke(receiver.id(), args));
        });
        require(checksum == expected, "Object entry checksum failed.");
        take(runtime.write(receiver.id(), field, std::uint32_t{0}));
        checksum = 0;
        leases.measure([&] {
            for (std::size_t index = 0; index < settings.calls; ++index)
                checksum += uint_result(lease.invoke(receiver.id(), args));
        });
        require(checksum == expected &&
                    std::get<std::uint32_t>(take(runtime.read(receiver.id(), field))) == settings.calls,
                "Object lease checksum or committed field failed.");
    }
    entries.output();
    leases.output();
}

void check_tasks(const std::vector<Coroutine>& tasks, CoroutineState expected) {
    for (const auto& task : tasks)
        require(task.state() == expected, "Unexpected coroutine state.");
}

void measure_task_scale(const Options& settings, Runtime& runtime, Program& program, const char* model,
                        std::size_t count, bool objects) {
    const auto entry = take(program.bind<std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
    const std::array args{Scalar{std::uint32_t{41}}};
    std::vector<Coroutine> tasks;
    tasks.reserve(count); // Measurement excludes the consumer's owner-vector allocation.
    const auto create = [&] {
        for (std::size_t index = 0; index < count; ++index)
            tasks.push_back(take(entry.start({}, args)));
    };
    const auto resume = [&] {
        for (auto& task : tasks)
            take(task.resume());
    };
    const auto cancel = [&] {
        for (auto& task : tasks)
            task.cancel();
    };
    const auto clear = [&] {
        tasks.clear();
        collect(runtime);
        require(runtime.root_count() == 0 && runtime.live_objects() == 0, "Task teardown retained roots or objects.");
    };
    create();
    check_tasks(tasks, CoroutineState::ready);
    resume();
    check_tasks(tasks, CoroutineState::suspended);
    collect(runtime);
    require(runtime.live_objects() == (objects ? count : 0), "Suspended task roots did not preserve objects.");
    resume();
    check_tasks(tasks, CoroutineState::completed);
    for (const auto& task : tasks)
        require(uint_result(task.result()) == 42, "Coroutine preflight result failed.");
    clear();

    Series creation("coroutine_create", model, count, count, settings.batches);
    Series suspension("coroutine_first_resume", model, count, count, settings.batches);
    Series completion("coroutine_complete_resume", model, count, count, settings.batches);
    Series cancellation("coroutine_cancel_suspended", model, count, count, settings.batches);
    for (std::size_t batch = 0; batch < settings.batches; ++batch) {
        creation.measure(create);
        check_tasks(tasks, CoroutineState::ready);
        suspension.measure(resume);
        check_tasks(tasks, CoroutineState::suspended);
        completion.measure(resume);
        check_tasks(tasks, CoroutineState::completed);
        for (const auto& task : tasks)
            require(uint_result(task.result()) == 42, "Timed coroutine result failed.");
        clear();
        create();
        resume();
        check_tasks(tasks, CoroutineState::suspended);
        cancellation.measure(cancel);
        check_tasks(tasks, CoroutineState::cancelled);
        clear();
    }
    creation.output();
    suspension.output();
    completion.output();
    cancellation.output();

    // Keep output and timing-series storage outside the resource deltas.
    const auto baseline = resources();
    create();
    const auto ready = resources();
    const auto ready_roots = runtime.root_count();
    const auto ready_objects = runtime.live_objects();
    resume();
    const auto suspended = resources();
    require(runtime.live_objects() == (objects ? count : 0), "Resource sample created an incorrect object count.");
    const auto suspended_roots = runtime.root_count();
    cancel();
    const auto cancelled = resources();
    const auto cancelled_objects = runtime.live_objects();
    require(runtime.root_count() == 0, "Cancellation did not release roots.");
    tasks.clear();
    collect(runtime);
    const auto released = resources();
    require(runtime.live_objects() == 0, "Resource sample retained objects.");
    // Emit only after every snapshot so formatting cannot pollute the deltas.
    resource_output(model, count, "ready", baseline, ready, ready_roots, ready_objects);
    resource_output(model, count, "suspended", baseline, suspended, suspended_roots, objects ? count : 0);
    resource_output(model, count, "cancelled_controls_retained", baseline, cancelled, 0, cancelled_objects);
    resource_output(model, count, "owners_released_and_gc", baseline, released, runtime.root_count(),
                    runtime.live_objects());
}

void measure_tasks(const Options& settings, const char* model, bool objects, bool native) {
    Runtime runtime;
    const auto type = objects ? counter_type(runtime, native) : TypeId{};
    Program program(runtime, type);
    std::string source_text = "async uint delayed(uint value) { yield; return value + 1; }";
    if (objects) {
        const auto* name = native ? "measure.NativeCounter" : "measure.DescribedCounter";
        source_text = std::string("async uint delayed(uint value) { ") + name + " held = new " + name +
                      "(); held.value = value; yield; return held.value + 1; }";
    }
    publish(program, "tasks.source", source_text);
    for (const auto count : {std::size_t{1}, std::size_t{64}, std::size_t{1024}})
        if (count <= settings.max_tasks)
            measure_task_scale(settings, runtime, program, model, count, objects);
    if (settings.max_tasks != 1 && settings.max_tasks != 64 && settings.max_tasks != 1024)
        measure_task_scale(settings, runtime, program, model, settings.max_tasks, objects);
}

void measure_gc_history(const Options& settings, std::size_t high_water) {
    Runtime runtime;
    const auto type = counter_type(runtime, false);
    auto retained = take(runtime.create(type));
    std::vector<ObjectRoot> spike;
    spike.reserve(high_water);
    for (std::size_t index = 0; index < high_water; ++index)
        spike.push_back(take(runtime.hold(retained.id())));
    spike.clear();
    require(runtime.root_count() == 1 && runtime.live_objects() == 1, "GC history setup changed live roots.");
    take(runtime.begin_collection());
    std::size_t root_work{};
    while (runtime.collection_phase() == CollectionPhase::roots)
        root_work += take(runtime.collect_step({1})).work_done;
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({4096}));
    Series cycle("full_gc", "one_live_object_after_root_spike", high_water, 1, settings.batches);
    std::size_t work{};
    for (std::size_t batch = 0; batch < settings.batches; ++batch) {
        cycle.measure([&] { work = collect(runtime); });
        require(runtime.root_count() == 1 && runtime.live_objects() == 1,
                "GC history collection lost the live object.");
    }
    cycle.output();
    std::printf("{\"record\":\"gc_work\",\"historical_extra_roots\":%zu,\"active_roots\":1,\"live_objects\":1,"
                "\"root_phase_work_units\":%zu,\"full_cycle_work_units\":%zu,\"validated\":true}\n",
                high_water, root_work, work);
}

constexpr std::string_view checked_source =
    "int scale(int value, int divisor) { return value / divisor; }\n"
    "uint mix(uint value, uint shift) { return (value << shift) % (shift + 1); }\n"
    "int truncate(float value) { return int(value); }\n"
    "int combine(int a, int b, uint s, float f) { int total = 0;\n"
    "    for (int i = 0; i < 4; i = i + 1) { total = total + scale(a + i, b) + int(mix(uint(i), s)) + truncate(f); }\n"
    "    return total; }\n";

// Codegen evidence for failure-path changes: per-case assembly, optimized IR and
// object size, plus the retained cost of a module dense in failure sites.
void dump_artifacts(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto write = [&](const std::string& name, std::string_view text) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        require(static_cast<bool>(file), "Cannot write measurement artifact.");
    };
    const auto dump = [&](const char* name, Runtime& runtime, TypeId receiver, std::string_view text,
                          std::string_view entry) {
        Program program(runtime, receiver);
        auto candidate = take(Program::prepare(take(program.request(name, text, entry, {.artifacts = true}))));
        take(program.try_publish(candidate));
        const auto artifacts = take(take(program.pin()).artifacts());
        write(std::string(name) + ".s", artifacts.assembly);
        write(std::string(name) + ".ll", artifacts.optimized_ir);
        std::printf("{\"record\":\"artifact\",\"case\":\"%s\",\"object_code_bytes\":%zu,\"assembly_bytes\":%zu}\n",
                    name, artifacts.object_code.size(), artifacts.assembly.size());
    };
    {
        Runtime runtime;
        dump("uint_loop", runtime, {}, loop_source, {});
    }
    {
        Runtime runtime;
        dump("checked_helpers", runtime, {}, checked_source, "combine");
    }
    {
        Runtime runtime;
        dump("native_object_advance", runtime, counter_type(runtime, true),
             "uint advance(uint delta) { self.value = self.value + delta; return self.value; }", {});
    }
    {
        Runtime runtime;
        dump("async_delayed", runtime, {}, "async uint delayed(uint value) { yield; return value + 1; }", {});
    }

    constexpr int functions = 256;
    std::string dense;
    for (int index = 0; index < functions; ++index)
        dense += "int f" + std::to_string(index) +
                 "(int a, int b, uint s, float x) { int q = a / b; "
                 "uint m = (uint(a) << s) % (s + 1); return q + int(m) + int(x); }\n";
    dense += "int dense(int a) { int t = 0;";
    for (int index = 0; index < functions; ++index)
        dense += " t = t + f" + std::to_string(index) + "(a, 3, 2, 1.5f);";
    dense += " return t; }\n";
    Runtime runtime;
    Program program(runtime, {});
    const auto before = resources();
    const auto started = Clock::now();
    auto candidate = take(Program::prepare(take(program.request("dense.source", dense, "dense"))));
    take(program.try_publish(candidate));
    const auto elapsed = nanoseconds(started);
    candidate = {};
    const auto after = resources();
    std::printf("{\"record\":\"dense_module\",\"functions\":%d,\"prepare_publish_ns\":%.0f,"
                "\"live_rounded_bytes_delta\":%lld,\"process_private_bytes_delta\":%lld}\n",
                functions, elapsed, static_cast<long long>(after.live_bytes - before.live_bytes),
                static_cast<long long>(after.private_bytes - before.private_bytes));
    const std::array args{Scalar{std::int32_t{7}}};
    require(take(program.entry().invoke({}, args)).has_value(), "Dense module preflight failed.");
}

// Republishing a project over a synthetic engine library of 5-line functions in call chains of 8;
// class k calls the end of chain k mod (chains - 1), so the last chain is never called.
void measure_compile_units() {
    constexpr int samples = 5;
    for (const int lines : {500, 3000})
        for (const int classes : {1, 10, 50}) {
            const int functions = lines / 5;
            const int chains = functions / 8;
            const auto snapshot = [&](int class_edit, int called_edit, int uncalled_edit) {
                const int called = 7;
                const int uncalled = (chains - 1) * 8;
                std::string engine;
                std::string project = "float game_probe(float x) { return x + 1.0; }\n";
                for (int i = 0; i < functions; ++i) {
                    const int edit = i == called ? called_edit : i == uncalled ? uncalled_edit : 0;
                    engine += "float bench.f" + std::to_string(i) + "(float x) {\n    float a = x * 1." +
                              std::to_string(i) + " + " + std::to_string(edit) + ".0;\n    float b = " +
                              (i % 8 == 0 ? std::string("math.sin(a)") : "bench.f" + std::to_string(i - 1) + "(a)") +
                              ";\n    return math.fract(b * 1.5) + a * 0.25;\n}\n";
                }
                for (int k = 0; k < classes; ++k)
                    project += "class game.C" + std::to_string(k) +
                               " : measure.NativeCounter {\n    float run(float x) { return bench.f" +
                               std::to_string(k % (chains - 1) * 8 + 7) + "(x) + " +
                               std::to_string(k == 0 ? class_edit : 0) +
                               ".0; }\n    float scaled(float x) { return x * 2.0; }\n}\n";
                return source::SourceSnapshot{
                    "units", 1, {{"Source/Game.source", 1, project}, {"engine/bench/lib.source", 1, engine}}};
            };
            Runtime runtime;
            counter_type(runtime, true);
            Program program(runtime);
            const auto republish = [&](source::SourceSnapshot input) {
                const auto started = Clock::now();
                auto candidate = take(Program::prepare(take(program.request(std::move(input)))));
                take(program.try_publish(candidate));
                return nanoseconds(started) / 1e6;
            };
            const auto codes = [&] {
                std::vector<std::pair<std::string, std::uint64_t>> result;
                for (const auto& entry : program.entries())
                    result.emplace_back(entry.name, entry.code);
                std::ranges::sort(result);
                return result;
            };
            republish(snapshot(0, 0, 0));
            int edits[3]{};
            const std::array scenarios{"edit_class", "edit_called_engine", "edit_uncalled_engine"};
            for (std::size_t which = 0; which < scenarios.size(); ++which) {
                std::vector<double> times;
                std::size_t changed_class{};
                std::size_t changed_global{};
                for (int sample = 0; sample < samples; ++sample) {
                    const auto before = codes();
                    ++edits[which];
                    times.push_back(republish(snapshot(edits[0], edits[1], edits[2])));
                    const auto after = codes();
                    require(after.size() == before.size(), "Republishing changed the entry set.");
                    changed_class = changed_global = 0;
                    for (std::size_t i = 0; i < after.size(); ++i)
                        if (after[i].second != before[i].second)
                            ++(after[i].first.starts_with("game.C") ? changed_class : changed_global);
                }
                std::ranges::sort(times);
                std::printf(
                    "{\"record\":\"compile_units\",\"engine_lines\":%d,\"engine_functions\":%d,\"classes\":%d,\"scenario\":\"%s\","
                    "\"republish_median_ms\":%.1f,\"republish_min_ms\":%.1f,\"republish_max_ms\":%.1f,"
                    "\"changed_class_codes\":%zu,\"changed_global_codes\":%zu,\"entries\":%zu}\n",
                    lines, functions, classes, scenarios[which], times[samples / 2], times.front(), times.back(),
                    changed_class, changed_global, program.entries().size());
            }
        }
}

// Every double transcendental against CORE-MATH over random argument pairs: the largest distance from the
// correctly rounded value and its arguments, with how often the result is not the correctly rounded one.
void measure_math_sweep(std::uint64_t requested) {
    Runtime runtime;
    take(runtime.register_types({array_definition<double>(runtime, "measure.Doubles")}));
    constexpr std::size_t chunk = std::size_t{1} << 22;
    constexpr unsigned segments = 64;
    const std::uint64_t chunks = (requested + chunk - 1) / chunk;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    // Fixed segments with their own seeds, so the arguments do not depend on the thread count.
    const auto parallel = [&](const auto& work) {
        std::atomic<unsigned> next{};
        std::vector<std::jthread> workers;
        for (unsigned t = 0; t < threads; ++t)
            workers.emplace_back([&] {
                for (unsigned s = next++; s < segments; s = next++)
                    work(s * chunk / segments, (s + 1) * chunk / segments, s);
            });
    };
    for (const auto& function : math_reference::functions) {
        const bool periodic = function.name == "sin" || function.name == "cos";
        Program kernel(runtime, {});
        publish(kernel, "sweep.source",
                "void run(Array<double>& a, const Array<double>& b) { uint i = 0; while (i < a.size()) { a[i] = math." +
                    std::string(function.name) + (function.binary ? "(a[i], b[i])" : "(a[i])") + "; i = i + 1; } }");
        Array<double> lanes;
        Array<double> others;
        lanes.resize(chunk);
        others.resize(chunk);
        std::vector<double> inputs(chunk);
        struct Tally {
            std::uint64_t worst{};

            std::uint64_t inexact{};

            std::uint64_t beyond{};

            double x{};

            double y{};

            double result{};

            double expected{};
        };
        std::vector<Tally> tallies(segments);
        const auto started = Clock::now();
        for (std::uint64_t index = 0; index < chunks; ++index) {
            parallel([&](std::size_t begin, std::size_t end, unsigned s) {
                std::mt19937_64 random(index * segments + s + 1);
                for (std::size_t i = begin; i < end; ++i) {
                    const auto [x, y] = math_reference::random_arguments(function.name, random);
                    inputs[i] = lanes[i] = x;
                    others[i] = y;
                }
            });
            take(kernel.entry().invoke({},
                                       std::array{Argument::borrow(lanes), Argument::borrow(std::as_const(others))}));
            parallel([&](std::size_t begin, std::size_t end, unsigned s) {
                auto& tally = tallies[s];
                for (std::size_t i = begin; i < end; ++i) {
                    const double x = inputs[i];
                    const double y = others[i];
                    const double result = lanes[i];
                    if (periodic && !(std::fabs(x) <= math_reference::periodic_limit)) {
                        tally.beyond += std::isfinite(x) ? !(std::fabs(result) <= 1) : !std::isnan(result);
                        continue;
                    }
                    const double expected = function.value(x, y);
                    const auto distance = math_reference::ulp_distance(result, expected);
                    tally.inexact += distance != 0;
                    if (distance > tally.worst)
                        tally = {distance, tally.inexact, tally.beyond, x, y, result, expected};
                }
            });
        }
        const auto seconds = nanoseconds(started) / 1e9;
        Tally total{};
        for (const auto& tally : tallies) {
            total.inexact += tally.inexact;
            total.beyond += tally.beyond;
            if (tally.worst >= total.worst) {
                total.worst = tally.worst;
                total.x = tally.x;
                total.y = tally.y;
                total.result = tally.result;
                total.expected = tally.expected;
            }
        }
        std::printf(
            "{\"record\":\"math_sweep\",\"function\":\"%.*s\",\"pairs\":%llu,\"max_ulp_from_correctly_rounded\":%llu,"
            "\"not_correctly_rounded\":%llu,\"periodic_range_violations\":%llu,\"worst_x\":\"%a\",\"worst_y\":\"%a\",\"worst_result\":\"%a\","
            "\"correctly_rounded\":\"%a\",\"seconds\":%.1f}\n",
            static_cast<int>(function.name.size()), function.name.data(),
            static_cast<unsigned long long>(chunks * chunk), static_cast<unsigned long long>(total.worst),
            static_cast<unsigned long long>(total.inexact), static_cast<unsigned long long>(total.beyond), total.x,
            total.y, total.result, total.expected, seconds);
        std::fflush(stdout);
    }
}

// Nanoseconds per element over 2^20 arguments for the double and float source kernels and for C++ double and
// float loops over the same std:: functions; the median of 9 runs each, after one warm-up.
void measure_math_timing() {
    Runtime runtime;
    take(runtime.register_types(
        {array_definition<double>(runtime, "measure.Doubles"), array_definition<float>(runtime, "measure.Floats")}));
    constexpr std::size_t count = std::size_t{1} << 20;
    constexpr int samples = 9;
    std::mt19937_64 random(20261023);
    const auto uniform = [&](double low, double high) {
        return low + (high - low) * static_cast<double>(random() >> 11) * 0x1p-53;
    };
    const auto median = [](std::vector<double> times) {
        std::ranges::sort(times);
        return times[times.size() / 2] / count;
    };
    // Each native function is its own lambda type, so the C++ loop inlines the std:: call site.
    const auto measure = [&](const char* name, const char* call, double low, double high, double second_low,
                             double second_high, auto native) {
        std::vector<double> first(count);
        std::vector<double> second(count);
        std::vector<double> output(1);
        for (std::size_t i = 0; i < count; ++i) {
            first[i] = uniform(low, high);
            second[i] = uniform(second_low, second_high);
        }
        const auto source = [&]<class T>(const char* type) {
            Program kernel(runtime, {});
            publish(kernel, "timing.source",
                    std::string("void run(Array<") + type + ">& a, const Array<" + type +
                        ">& b) { uint i = 0; "
                        "while (i < a.size()) { a[i] = " +
                        call + "; i = i + 1; } }");
            Array<T> lanes;
            Array<T> others;
            for (std::size_t i = 0; i < count; ++i) {
                lanes.add(static_cast<T>(first[i]));
                others.add(static_cast<T>(second[i]));
            }
            std::vector<double> times;
            for (int sample = 0; sample <= samples; ++sample) {
                for (std::size_t i = 0; i < count; ++i)
                    lanes[i] = static_cast<T>(first[i]);
                const auto started = Clock::now();
                take(kernel.entry().invoke(
                    {}, std::array{Argument::borrow(lanes), Argument::borrow(std::as_const(others))}));
                if (sample)
                    times.push_back(nanoseconds(started));
            }
            return median(times);
        };
        const auto cpp = [&]<class T>() {
            const std::vector<T> lanes(first.begin(), first.end());
            const std::vector<T> others(second.begin(), second.end());
            std::vector<T> results(count);
            std::vector<double> times;
            for (int sample = 0; sample <= samples; ++sample) {
                const auto started = Clock::now();
                for (std::size_t i = 0; i < count; ++i)
                    results[i] = native(lanes[i], others[i]);
                if (sample)
                    times.push_back(nanoseconds(started));
            }
            output[0] += results[count / 2];
            return median(times);
        };
        const double source_double = source.template operator()<double>("double");
        const double source_float = source.template operator()<float>("float");
        const double cpp_double = cpp.template operator()<double>();
        const double cpp_float = cpp.template operator()<float>();
        std::printf(
            "{\"record\":\"math_timing\",\"function\":\"%s\",\"elements\":%zu,\"source_double_ns\":%.3f,\"source_float_ns\":%.3f,"
            "\"cpp_double_ns\":%.3f,\"cpp_float_ns\":%.3f,\"checksum\":\"%a\"}\n",
            name, count, source_double, source_float, cpp_double, cpp_float, output[0]);
        std::fflush(stdout);
    };
    measure("add", "a[i] + b[i]", -1, 1, -1, 1, [](auto x, auto y) { return x + y; });
    measure("sin", "math.sin(a[i])", -1000, 1000, 0, 0, [](auto x, auto) { return std::sin(x); });
    measure("cos", "math.cos(a[i])", -1000, 1000, 0, 0, [](auto x, auto) { return std::cos(x); });
    measure("exp", "math.exp(a[i])", -10, 10, 0, 0, [](auto x, auto) { return std::exp(x); });
    measure("log", "math.log(a[i])", 1e-3, 1000, 0, 0, [](auto x, auto) { return std::log(x); });
    measure("atan2", "math.atan2(a[i], b[i])", -1, 1, -1, 1, [](auto y, auto x) { return std::atan2(y, x); });
    measure("pow", "math.pow(a[i], b[i])", 1e-3, 10, -4, 4, [](auto x, auto y) { return std::pow(x, y); });
}

void configuration(const Options& settings) {
    std::array<char, 49> cpu{};
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(registers[0]) >= 0x80000004u)
        for (unsigned leaf = 0; leaf < 3; ++leaf) {
            __cpuid(registers.data(), static_cast<int>(0x80000002u + leaf));
            std::memcpy(cpu.data() + 16 * leaf, registers.data(), 16);
        }
    for (auto& character : cpu)
        if (character == '"' || character == '\\' || character < 0)
            character = '_';
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    require(GlobalMemoryStatusEx(&memory) != 0, "Cannot read machine memory.");
    LARGE_INTEGER frequency{};
    require(QueryPerformanceFrequency(&frequency) != 0, "Cannot read timer frequency.");
    std::printf(
        "{\"record\":\"configuration\",\"configuration\":\"Release\",\"compiler\":\"Clang %s\","
        "\"cpu\":\"%s\",\"logical_processors\":%lu,\"physical_memory_bytes\":%llu,\"timer_frequency_hz\":%lld,"
        "\"mimalloc_version\":%d,\"batches\":%zu,\"calls\":%zu,\"max_tasks\":%zu,"
        "\"affinity_pinned\":false,\"timing_includes_result_checks\":true,\"resource_snapshots_force_allocator_collection\":true}\n",
        __clang_version__, cpu.data(), static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
        static_cast<unsigned long long>(memory.ullTotalPhys), static_cast<long long>(frequency.QuadPart), mi_version(),
        settings.batches, settings.calls, settings.max_tasks);
}
}

int main(int argc, char** argv) {
    try {
#if !defined(NDEBUG)
        throw std::runtime_error("Run this measurement target in Release configuration.");
#endif
        if (argc == 3 && std::string_view(argv[1]) == "--artifacts") {
            dump_artifacts(argv[2]);
            std::puts("{\"record\":\"complete\",\"validated\":true}");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--compile-units") {
            measure_compile_units();
            std::puts("{\"record\":\"complete\",\"validated\":true}");
            return 0;
        }
        if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--math-sweep") {
            measure_math_sweep(argc == 3 ? std::stoull(argv[2]) : 1'000'000'000ull);
            std::puts("{\"record\":\"complete\",\"validated\":true}");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--math-timing") {
            measure_math_timing();
            std::puts("{\"record\":\"complete\",\"validated\":true}");
            return 0;
        }
        const auto settings = options(argc, argv);
        configuration(settings);
        verify_allocator();
        measure_compilation(settings);
        measure_scalar_calls(settings);
        measure_object_calls(settings, true);
        measure_object_calls(settings, false);
        measure_tasks(settings, "scalar", false, false);
        measure_tasks(settings, "native_object", true, true);
        measure_tasks(settings, "described_object", true, false);
        for (const auto history : {std::size_t{0}, std::size_t{64}, std::size_t{1024}})
            if (history <= settings.max_tasks)
                measure_gc_history(settings, history);
        std::puts("{\"record\":\"complete\",\"validated\":true}");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Source measurement failed: %s\n", error.what());
        return 1;
    }
}
