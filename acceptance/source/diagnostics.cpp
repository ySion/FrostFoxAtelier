#include "math/math.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/diagnostics.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using source::DiagnosticCall;
using source::DiagnosticStatus;
using source::Diagnostics;
using source::Program;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
void publish(Program& program, std::string_view name, std::string_view text,
    source::CompileOptions options = {.diagnostics = true}, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request(name, text, entry, options))));
    take(program.try_publish(candidate));
}
void publish(Program& program, source::SourceSnapshot snapshot) {
    auto candidate = take(Program::prepare(take(program.request(std::move(snapshot), {.diagnostics = true}))));
    take(program.try_publish(candidate));
}
void require_clean(const source::DiagnosticReport& report) {
    require(report.resources_cleared && report.remaining_owned_values == 0 &&
        report.before.roots == report.after.roots,
        "Diagnostic completion did not include explicit value/root cleanup.");
}
struct Host {
    std::uint32_t value{};
    Coroutine* task{};
    void fail() { throw std::runtime_error("Diagnostic native failure."); }
    void cancel() { task->cancel(); }
    void cancel_fail() { task->cancel(); fail(); }
};
TypeId register_host(Runtime& runtime) {
    return take(runtime.register_type(NativeRegistration<Host>{runtime, "diagnostic.Host", TypeKind::object}
        .field<&Host::value>("value").method<&Host::fail>("fail", true)
        .method<&Host::cancel>("cancel", true).method<&Host::cancel_fail>("cancel_fail", true).build()));
}

void verify_call_boundaries() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program program(runtime, type);
    constexpr auto body = "void run(uint count) { uint i = 0; while (i < count) { self.value = self.value + 1; i = i + 1; } }";
    publish(program, "loop.source", body, {});
    require(!take(program.pin()).artifacts(), "Default compilation retained optional artifacts.");
    Diagnostics capture;
    const std::array args{Scalar{std::uint32_t{7}}};
    const auto rejected = program.entry().invoke(host.id(), args, {1}, &capture);
    require(!rejected && rejected.error().code == ErrorCode::unsupported_operation &&
        capture.snapshot().status == DiagnosticStatus::rejected && !capture.active() &&
        std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 0,
        "An unsupported capture executed user code or failed to record its rejection.");
    take(program.entry().invoke(host.id(), args));
    require(std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 7,
        "Default invocation changed when diagnostics were added.");

    publish(program, "loop.source", body, {.diagnostics = true, .artifacts = true});
    take(program.entry().invoke(host.id(), args)); // A diagnostic version also accepts an ordinary invocation.
    take(program.entry().invoke(host.id(), args, {1}, &capture));
    const auto report = capture.snapshot();
    require(report.status == DiagnosticStatus::completed && !capture.active() &&
        report.program_id != 0 && report.code_version != 0 && report.entry == "run" &&
        report.source_name == "loop.source" && !report.error &&
        !report.timing && report.active_time == std::chrono::nanoseconds::zero() &&
        std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 21,
        "Call capture lost its identity, state, count-only timing contract or user result.");
    require(report.calls[DiagnosticCall::begin] == 1 && report.calls[DiagnosticCall::finish] == 1 &&
        report.calls[DiagnosticCall::field_read] == 7 && report.calls[DiagnosticCall::field_write] == 7 &&
        report.calls[DiagnosticCall::poll] == 8 && report.calls[DiagnosticCall::native_call] == 0 &&
        report.calls[DiagnosticCall::source_call] == 0,
        "Object-loop diagnostics did not count actual JIT/native boundaries.");
    require_clean(report);
    const auto artifacts = take(take(program.pin()).artifacts());
    require(!artifacts.entry_symbol.empty() && !artifacts.target_triple.empty() &&
        !artifacts.original_ir.empty() && !artifacts.optimized_ir.empty() &&
        !artifacts.object_code.empty() && artifacts.assembly.find(artifacts.entry_symbol) != std::string::npos &&
        artifacts.original_ir.find("diagnostic.trace") != std::string::npos,
        "Requested artifacts omitted the actual module, object, entry symbol or diagnostic instrumentation.");

    for (int i = 0; i < 8; ++i) take(runtime.create(type)).reset();
    take(runtime.begin_collection());
    const std::array many{Scalar{std::uint32_t{256}}};
    take(program.entry().invoke(host.id(), many, {1}, &capture));
    const auto collected = capture.snapshot();
    require(collected.before.objects == 9 && collected.after.objects == 1 &&
        collected.gc_work > 0 && collected.gc_reclaimed == 8 &&
        collected.after.collection_phase == CollectionPhase::idle &&
        collected.calls[DiagnosticCall::field_write] == 256,
        "Reused capture accumulated old counts or lost GC work/reclamation.");
    require_clean(collected);
}

void verify_borrowed_and_owned_paths() {
    Runtime runtime;
    const auto type = register_host(runtime);
    take(runtime.register_types({string_definition(runtime),
        array_definition<math::Vector3>(runtime, "diagnostic.Vectors")}));
    auto host = take(runtime.create(type));
    Program batch(runtime);
    publish(batch, "batch.source", R"(
void run(Array<vector3>& values) {
    uint i = 0;
    while (i < values.size()) { values[i] = values[i] * 2.0; i = i + 1; }
}
)");
    Array<math::Vector3> values;
    values.resize(64);
    for (auto& value : values) value = {1.0f, 2.0f, 3.0f};
    Diagnostics capture({.timing = true});
    take(batch.entry().invoke({}, std::array{Argument::borrow(values)}, {1}, &capture));
    const auto report = capture.snapshot();
    require(values[0] == math::Vector3{2.0f, 4.0f, 6.0f} && values[values.size() - 1] == values[0] &&
        report.status == DiagnosticStatus::completed && report.timing && report.active_time.count() > 0 &&
        report.calls[DiagnosticCall::sequence_view] == 1 && report.calls[DiagnosticCall::poll] == 0 &&
        report.calls[DiagnosticCall::field_read] == 0 && report.calls[DiagnosticCall::field_write] == 0 &&
        report.calls[DiagnosticCall::native_call] == 0 && report.calls[DiagnosticCall::source_call] == 0,
        "Borrowed math loop gained per-element native calls or was omitted from diagnostics.");
    require_clean(report);

    Program owned(runtime, type);
    publish(owned, "owned-diagnostic.source", R"(
uint leaf() {
    String label = "diagnostic scope owns a heap string";
    self.fail();
    return label.size();
}
uint run() { return leaf(); }
)", {.diagnostics = true}, "run");
    const auto failed = owned.entry().invoke(host.id(), {}, {1}, &capture);
    const auto failure = capture.snapshot();
    require(!failed && failed.error().message == "Diagnostic native failure." &&
        failure.status == DiagnosticStatus::failed && failure.error &&
        failure.error->message == failed.error().message && failure.location &&
        failure.location->function == "leaf" && failure.location->source.path == "owned-diagnostic.source" &&
        failure.location->source.line == 4 && failed.error().source && failed.error().source->line == 4 &&
        failure.calls[DiagnosticCall::source_call] == 1 && failure.calls[DiagnosticCall::native_call] == 1 &&
        failure.peak_owned_values > 0 && failure.calls[DiagnosticCall::value_construct] > 0 &&
        failure.calls[DiagnosticCall::value_construct] == failure.calls[DiagnosticCall::value_destroy],
        "Nested native failure lost its first cause/source or skipped owned-value accounting.");
    require_clean(failure);
}

void verify_source_mapping() {
    Runtime runtime;
    Program program(runtime);
    publish(program, {"diagnostic-project", 3, {
        {"Source/Math.source", 17, "\nuint leaf(uint divisor) {\n    return 10 / divisor;\n}\n"},
        {"Source/Entry.source", 29, "uint run(uint divisor) { return leaf(divisor); }"}}});
    Diagnostics capture;
    const auto result = take(program.entry("run")).invoke({}, std::array{Scalar{std::uint32_t{0}}}, {1}, &capture);
    const auto report = capture.snapshot();
    require(!result && report.status == DiagnosticStatus::failed && report.location && report.error &&
        report.location->function == "leaf" && report.location->source.path == "Source/Math.source" &&
        report.location->source.version == 17 && report.location->source.line == 3 &&
        result.error().source && result.error().source->path == "Source/Math.source" &&
        result.error().source->version == 17 && report.error->message == result.error().message,
        "A nested runtime failure was attributed to the caller or combined project source.");
    require_clean(report);
}

void verify_coroutine_lifetime() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto host = take(runtime.create(type));
    const auto roots = runtime.root_count();
    Program program(runtime, type);
    publish(program, "suspend.source", "async uint run(uint value) {\n    yield;\n    return value + 1;\n}");
    const auto old = take(program.pin());
    const auto entry = program.entry();
    const std::array args{Scalar{std::uint32_t{41}}};
    Diagnostics capture({.timing = true});
    auto task = take(entry.start(host.id(), args, {1}, &capture));
    const auto ready = capture.snapshot();
    require(capture.active() && ready.status == DiagnosticStatus::ready && ready.resumes == 0 &&
        ready.calls[DiagnosticCall::coroutine_allocate] == 1 && ready.calls[DiagnosticCall::coroutine_free] == 0,
        "Creating a coroutine failed to record allocation or executed its body.");
    const auto busy = entry.start(host.id(), args, {1}, &capture);
    require(!busy && busy.error().code == ErrorCode::busy &&
        capture.snapshot().status == DiagnosticStatus::ready &&
        capture.snapshot().calls[DiagnosticCall::coroutine_allocate] == 1,
        "Reusing an active capture replaced its report or allocated a second coroutine.");
    require(take(task.resume()) == CoroutineState::suspended, "Diagnostic coroutine did not yield.");
    const auto suspended = capture.snapshot();
    require(suspended.status == DiagnosticStatus::suspended && suspended.resumes == 1 && suspended.location &&
        suspended.location->source.path == "suspend.source" && suspended.location->source.line == 2 &&
        capture.snapshot().active_time == suspended.active_time,
        "Suspension lost its location or kept advancing the active execution clock.");
    publish(program, "replacement.source", "async uint run(uint value) { yield; return value + 100; }");
    Diagnostics fresh;
    auto replacement = take(entry.start(host.id(), args, {1}, &fresh));
    require(fresh.snapshot().program_id == ready.program_id && fresh.snapshot().code_version != ready.code_version,
        "Hot publication did not distinguish code versions within the same program.");
    replacement.cancel();
    program.close();
    require(take(task.resume()) == CoroutineState::completed &&
        std::get<std::uint32_t>(*take(task.result())) == 42,
        "Diagnostics prevented a suspended old version from completing after close.");
    auto completed = capture.snapshot();
    require(completed.status == DiagnosticStatus::completed && !capture.active() && completed.resumes == 2 &&
        completed.code_version == ready.code_version && completed.calls[DiagnosticCall::coroutine_free] == 1 &&
        completed.after.roots == roots,
        "Coroutine completion reported a new version, leaked roots or missed code-frame destruction.");
    require_clean(completed);

    auto cancelled = take(old.start(host.id(), args, {1}, &capture));
    cancelled.cancel();
    cancelled.cancel();
    auto cancelled_report = capture.snapshot();
    require(cancelled_report.status == DiagnosticStatus::cancelled && cancelled_report.resumes == 0 &&
        cancelled_report.calls[DiagnosticCall::coroutine_allocate] == 1 &&
        cancelled_report.calls[DiagnosticCall::coroutine_free] == 1,
        "Ready cancellation or repeated cancellation double-counted resource cleanup.");
    require_clean(cancelled_report);
    cancelled = take(old.start(host.id(), args, {1}, &capture));
    take(cancelled.resume());
    cancelled.cancel();
    cancelled_report = capture.snapshot();
    require(cancelled_report.status == DiagnosticStatus::cancelled && cancelled_report.resumes == 1 &&
        cancelled_report.calls[DiagnosticCall::coroutine_free] == 1, "Suspended cancellation was not terminal.");
    require_clean(cancelled_report);

    Diagnostics observer;
    Coroutine detached;
    {
        Diagnostics temporary;
        observer = temporary;
        detached = take(old.start(host.id(), args, {1}, &temporary));
    }
    take(detached.resume());
    take(detached.resume());
    require(observer.snapshot().status == DiagnosticStatus::completed && !observer.active(),
        "Releasing the original capture handle invalidated its active coroutine.");

    Program callback(runtime, type);
    for (const bool fail : {false, true}) {
        publish(callback, "cancel-callback.source", fail ?
            "async uint run(uint value) { self.cancel_fail(); return value; }" :
            "async uint run(uint value) { self.cancel(); return value; }");
        auto active = take(callback.entry().start(host.id(), args, {1}, &capture));
        take(runtime.with_native<Host>(host.id(), [&](Host& value) -> Result<void> { value.task = &active; return {}; }));
        const auto state = active.resume();
        const auto terminal = capture.snapshot();
        require(fail ? (!state && terminal.status == DiagnosticStatus::failed && terminal.error &&
                terminal.error->message == "Diagnostic native failure.") :
            (state && *state == CoroutineState::cancelled && terminal.status == DiagnosticStatus::cancelled),
            "Capture guessed the terminal state before Runtime resolved cancellation and failure.");
        require(terminal.calls[DiagnosticCall::coroutine_free] == 1 && !capture.active(),
            "Running cancellation/failure failed to close its capture once.");
        require_clean(terminal);
        take(runtime.with_native<Host>(host.id(), [](Host& value) -> Result<void> { value.task = nullptr; return {}; }));
    }
}

void verify_detached_reports_and_artifacts() {
    Diagnostics capture;
    source::CompilationArtifacts artifacts;
    std::uint64_t first_program{};
    {
        Runtime runtime;
        Program program(runtime);
        publish(program, "detached.source", "uint run(uint divisor) { return 9 / divisor; }",
            {.diagnostics = true, .artifacts = true});
        artifacts = take(take(program.pin()).artifacts());
        require(!program.entry().invoke({}, std::array{Scalar{std::uint32_t{0}}}, {1}, &capture),
            "Detached-report scenario did not fail.");
        first_program = capture.snapshot().program_id;
    }
    const auto retained = capture.snapshot();
    require(retained.status == DiagnosticStatus::failed && retained.error && retained.location &&
        retained.location->source.path == "detached.source" && !artifacts.object_code.empty() &&
        artifacts.optimized_ir.find(artifacts.entry_symbol) != std::string::npos,
        "Diagnostic output borrowed data from a retired Runtime or executable version.");
    {
        Runtime runtime;
        Program program(runtime);
        publish(program, "plain-artifact.source", "uint run(uint value) { return value + 1; }",
            {.artifacts = true});
        const auto plain = take(take(program.pin()).artifacts());
        require(plain.original_ir.find("diagnostic.trace") == std::string::npos &&
            plain.original_ir.find("diagnostic.record") == std::string::npos &&
            plain.optimized_ir.find("diagnostic.trace") == std::string::npos &&
            std::get<std::uint32_t>(*take(program.entry().invoke({}, std::array{Scalar{std::uint32_t{41}}}))) == 42,
            "Artifact-only compilation inserted diagnostic work into the default execution path.");
        publish(program, "second-runtime.source", "uint run(uint value) { return value + 2; }");
        take(program.entry().invoke({}, std::array{Scalar{std::uint32_t{40}}}, {1}, &capture));
        require(capture.snapshot().program_id != first_program && !capture.snapshot().error,
            "A terminal handle reused across Runtime lifetimes retained stale identity or failure.");
    }
}
}

void verify_source_diagnostics() {
    verify_call_boundaries();
    verify_borrowed_and_owned_paths();
    verify_source_mapping();
    verify_coroutine_lifetime();
    verify_detached_reports_and_artifacts();
    std::puts("Source diagnostics: PASS (boundary counts, borrowed/owned values, GC, source versions, coroutine lifetime, detached artifacts)");
}
