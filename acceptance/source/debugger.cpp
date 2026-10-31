#include "math/math.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/debugger.hpp"
#include "source/program.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using source::DebugAction;
using source::DebugSnapshot;
using source::DebugStatus;
using source::DebugStopReason;
using source::DebugValue;
using source::DebugValueState;
using source::Debugger;
using source::Program;

void require(bool value, const char* message) {
    if (!value)
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
template <class T>
void require_busy(const Result<T>& result, const char* message) {
    require(!result && result.error().code == ErrorCode::busy, message);
}
void publish(Program& program, std::string_view path, std::string_view text,
             source::CompileOptions options = {.debugging = true}, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request(path, text, entry, options))));
    take(program.try_publish(candidate));
}
source::DebugBreakpoint breakpoint(const Program::Lease& lease, std::string_view path, std::size_t line) {
    const auto sites = take(lease.debug_sites());
    for (const auto& site : sites)
        if (site.source.path == path && site.source.line == line)
            return {site.source.path, site.source.version, site.source.line, site.source.column};
    throw std::runtime_error("Requested source line has no executable debug site.");
}
const DebugValue* local(const source::DebugFrame& frame, std::string_view name) {
    for (const auto& value : frame.locals)
        if (value.name == name)
            return &value;
    return nullptr;
}
std::uint32_t number(const DebugValue& value) {
    if (value.state != DebugValueState::available || !value.scalar ||
        !std::holds_alternative<std::uint32_t>(*value.scalar))
        throw std::runtime_error("Debug uint local '" + value.name + "' has state " +
                                 std::to_string(static_cast<int>(value.state)) + " and no uint value: " + value.text);
    return std::get<std::uint32_t>(*value.scalar);
}
struct Host {
    std::uint32_t value{};
    void set_value(std::uint32_t next) noexcept { value = next; }
    void fail() { throw std::runtime_error("Debugger native first failure."); }
};
TypeId register_host(Runtime& runtime) {
    return take(runtime.register_type(NativeRegistration<Host>{runtime, "debugger.Host", TypeKind::object}
                                          .field<&Host::value>("value")
                                          .method<&Host::set_value>("set_value", true)
                                          .method<&Host::fail>("fail", true)
                                          .build()));
}

void verify_compile_modes_and_stepping() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program plain(runtime, type);
    constexpr auto change = "void run() { self.value = self.value + 1; }";
    publish(plain, "plain-debug.source", change, {.artifacts = true});
    std::size_t stops{};
    Debugger unsupported([&](const DebugSnapshot& stop) {
        ++stops;
        return stop.command(DebugAction::continue_execution);
    });
    const auto refused = plain.entry().invoke(host.id(), {}, nullptr, &unsupported);
    require(!refused && refused.error().code == ErrorCode::unsupported_operation &&
                unsupported.status() == DebugStatus::rejected && !unsupported.active() && stops == 0 &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 0,
            "A non-debug version ran user code before rejecting its debugger.");
    const auto lease = take(plain.pin());
    require(!lease.debug_sites(), "A non-debug version advertised executable debug sites.");
    const auto artifacts = take(lease.artifacts());
    require(artifacts.original_ir.find("business_debug") == std::string::npos &&
                artifacts.optimized_ir.find("business_debug") == std::string::npos,
            "Normal compilation inserted debugger hooks.");
    publish(plain, "plain-debug.source", change);
    take(plain.entry().invoke(host.id(), {}));
    take(plain.entry().invoke(host.id(), {}, nullptr, &unsupported));
    require(stops == 1 && unsupported.status() == DebugStatus::completed &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 2,
            "Debug compilation changed ordinary behavior or could not reuse a terminal debugger.");

    publish(plain, "reference-preview.source", R"(void run() {
    debugger.Host held = self;
    weak debugger.Host observed = observe(held);
    debugger.Host empty;
    observed.value = held.value + 1;
}
)");
    bool references_seen{};
    Debugger references(
        [&](const DebugSnapshot& stop) {
            const auto& frame = stop.frames.at(0);
            const auto* held = local(frame, "held");
            const auto* observed = local(frame, "observed");
            const auto* empty = local(frame, "empty");
            require(held && held->state == DebugValueState::available && held->object == host.id() &&
                        held->type == type && held->object_live && !held->weak && observed &&
                        observed->state == DebugValueState::available && observed->object == host.id() &&
                        observed->object_live && observed->weak && empty &&
                        empty->state == DebugValueState::available && empty->type == type &&
                        empty->object == ObjectId{} && !empty->object_live && runtime.root_count() == 1,
                    "Synchronous reference previews lost identity, type, liveness or the rootless storage policy.");
            references_seen = true;
            return stop.command(DebugAction::continue_execution);
        },
        {.break_on_entry = false});
    take(references.set_breakpoints({breakpoint(take(plain.pin()), "reference-preview.source", 5)}));
    take(plain.entry().invoke(host.id(), {}, nullptr, &references));
    require(references_seen && std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 3,
            "Reference preview did not resume weak member access correctly.");

    Program calls(runtime);
    publish(calls, "steps.source", R"(uint leaf(uint value) {
    uint doubled = value * 2;
    return doubled;
}
uint run(uint input) {
    uint result = leaf(input);
    uint following = result + 1;
    return following;
}
)",
            {.debugging = true}, "run");
    std::uint64_t caller_id{};
    bool entered_leaf{}, returned_to_caller{}, initialized_result{};
    std::size_t into_stops{};
    Debugger into([&](const DebugSnapshot& stop) {
        require(++into_stops < 64 && !stop.frames.empty(), "Stepping did not make bounded progress.");
        const auto& frame = stop.frames[0];
        if (frame.location.function == "leaf") {
            require(!entered_leaf && stop.frames.size() == 2 && stop.frames[1].location.function == "run" &&
                        frame.activation_id != caller_id && stop.frames[1].activation_id == caller_id,
                    "Step-into/out lost the dynamic source stack or exposed a synthetic frame.");
            const auto* parameter = local(frame, "value");
            const auto* initializing = local(stop.frames[1], "result");
            require(parameter && number(*parameter) == 21 && initializing &&
                        initializing->state == DebugValueState::uninitialized && !initializing->scalar,
                    "Helper entry read an unfinished caller initializer or lost its scalar parameter.");
            entered_leaf = true;
            return stop.command(DebugAction::step_out);
        }
        require(frame.location.function == "run", "Unexpected frame while stepping source helpers.");
        if (!caller_id) {
            caller_id = frame.activation_id;
            const auto* parameter = local(frame, "input");
            require(parameter && number(*parameter) == 21, "Entry snapshot lost its typed parameter.");
        }
        if (entered_leaf) {
            returned_to_caller = true;
            if (const auto* result = local(frame, "result"); result && result->state == DebugValueState::available)
                initialized_result = number(*result) == 42;
            return stop.command(DebugAction::step_over);
        }
        return stop.command(DebugAction::step_into);
    });
    const std::array args{Scalar{std::uint32_t{21}}};
    require(std::get<std::uint32_t>(*take(calls.entry().invoke({}, args, nullptr, &into))) == 43 && entered_leaf &&
                returned_to_caller && initialized_result && into.status() == DebugStatus::completed,
            "Step-into/out did not resume the same caller and publish its initialized result.");
    std::size_t over_stops{};
    Debugger over([&](const DebugSnapshot& stop) {
        require(++over_stops < 32 && stop.frames.size() == 1 && stop.frames[0].location.function == "run",
                "Step-over entered a helper or exposed a synthetic export wrapper.");
        return stop.command(DebugAction::step_over);
    });
    require(std::get<std::uint32_t>(*take(calls.entry().invoke({}, args, nullptr, &over))) == 43 && over_stops > 1,
            "Step-over skipped every executable caller site.");

    Program loops(runtime);
    publish(loops, "loop-debug.source", R"(uint run(uint limit) {
    uint i = 0;
    while (i < limit) { uint scoped = i; i = i + 1; if (i == 1) { continue; } if (i == 3) { break; } }
    return i;
}
)");
    std::size_t loop_stops{}, all_stops{};
    bool scope_available{}, scope_finished{};
    Debugger loop([&](const DebugSnapshot& stop) {
        require(++all_stops < 128 && stop.frames.size() == 1, "Loop stepping failed to advance.");
        const auto& frame = stop.frames[0];
        const auto* scoped = local(frame, "scoped");
        if (frame.location.source.line == 3) {
            ++loop_stops;
            scope_available = scope_available || (scoped && scoped->state == DebugValueState::available);
        }
        if (frame.location.source.line == 4) {
            scope_finished = scoped && scoped->state == DebugValueState::out_of_scope && !scoped->scalar;
        }
        return stop.command(DebugAction::step_over);
    });
    require(std::get<std::uint32_t>(
                *take(loops.entry().invoke({}, std::array{Scalar{std::uint32_t{10}}}, nullptr, &loop))) == 3 &&
                loop_stops > 3 && scope_available && scope_finished,
            "Same-line loop stops or break/continue lexical availability were lost.");
}

void verify_exact_breakpoints() {
    Runtime runtime;
    Program project(runtime);
    source::SourceSnapshot snapshot{
        "debug-project",
        9,
        {{"Source/Math.source", 41, "uint helper(uint value) {\n    return value + 1;\n}\n"},
         {"Source/Run.source", 52, "uint run(uint value) {\n    return helper(value);\n}\n"}}};
    auto candidate = take(Program::prepare(take(project.request(snapshot, {.debugging = true}))));
    take(project.try_publish(candidate));
    auto lease = take(project.pin("run"));
    auto bound = breakpoint(lease, "Source/Math.source", 2);
    require(bound.source_version == 41, "Debug sites lost the original file version.");
    std::size_t hits{};
    Debugger debugger(
        [&](const DebugSnapshot& stop) {
            ++hits;
            require(stop.reason == DebugStopReason::breakpoint && stop.frames.size() == 2 &&
                        stop.frames[0].location.function == "helper" && stop.frames[1].location.function == "run" &&
                        stop.frames[0].location.source.path == bound.path &&
                        stop.frames[0].location.source.version == bound.source_version &&
                        stop.frames[0].location.source.line == bound.line &&
                        stop.frames[0].location.source.column == bound.column,
                    "Exact breakpoint stopped at a different file/version/site or exposed an export wrapper.");
            return stop.command(DebugAction::continue_execution);
        },
        {.break_on_entry = false});
    take(debugger.set_breakpoints({bound}));
    const std::array args{Scalar{std::uint32_t{41}}};
    require(std::get<std::uint32_t>(*take(lease.invoke({}, args, nullptr, &debugger))) == 42 && hits > 0,
            "A discovered executable site did not bind.");
    auto wrong = bound;
    wrong.source_version = 0;
    take(debugger.set_breakpoints({wrong}));
    const auto previous_hits = hits;
    require(!lease.invoke({}, args, nullptr, &debugger) && debugger.status() == DebugStatus::rejected &&
                hits == previous_hits,
            "A zero source version acted as a wildcard for a different published file version.");
    snapshot.files[0].version = 0;
    candidate = take(Program::prepare(take(project.request(snapshot, {.debugging = true}))));
    take(project.try_publish(candidate));
    lease = take(project.pin("run"));
    bound = breakpoint(lease, "Source/Math.source", 2);
    require(bound.source_version == 0, "A real zero file version was replaced by the publication version.");
    take(debugger.set_breakpoints({bound}));
    require(std::get<std::uint32_t>(*take(lease.invoke({}, args, nullptr, &debugger))) == 42 && hits > previous_hits,
            "An exact breakpoint could not bind a real zero-version SourceSnapshot file.");
}

DebugSnapshot verify_inspection_and_previews() {
    Runtime runtime;
    const auto type = register_host(runtime);
    take(runtime.register_types({string_definition(runtime), array_definition<float>(runtime, "debugger.Floats")}));
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program program(runtime, type);
    constexpr auto body = R"(float run(const Array<float>& values, const String& text) {
    vector3 vector = vector3(1.0, 2.0, 3.0);
    String owned = text;
    uint checkpoint = 0;
    self.set_value(5);
    return values[0] + vector.x;
}
)";
    publish(program, "preview.source", body);
    const auto at = breakpoint(take(program.pin()), "preview.source", 4);
    auto pending = take(Program::prepare(take(program.request("preview.source", body, {}, {.debugging = true}))));
    Array<float> values;
    for (int i = 0; i < 5; ++i)
        values.add(static_cast<float>(i + 1));
    String text("abcdefghijklmnop");
    const std::array args{Argument::borrow(std::as_const(values)), Argument::borrow(std::as_const(text))};
    bool inspected{}, entered_native{};
    DebugSnapshot retained;
    Debugger debugger(
        [&](const DebugSnapshot& stop) {
            require(!inspected && debugger.status() == DebugStatus::paused && debugger.active() && !stop.frames.empty(),
                    "The inspection callback was not a single paused session.");
            retained = stop;
            const auto& frame = stop.frames[0];
            const auto* array = local(frame, "values");
            const auto* string = local(frame, "text");
            const auto* vector = local(frame, "vector");
            const auto* owned = local(frame, "owned");
            const auto* pending_local = local(frame, "checkpoint");
            require(array && array->state == DebugValueState::available && !array->type_name.empty() &&
                        array->total_children == 5 && array->children.size() == 5 && !array->truncated &&
                        array->children[0].scalar && std::get<float>(*array->children[0].scalar) == 1.0f && string &&
                        string->text == "abcdefghijklmnop" && !string->truncated && vector &&
                        vector->state == DebugValueState::available && vector->total_children == 3 &&
                        vector->children.size() == 2 && vector->truncated && owned &&
                        owned->text == "abcdefghijklmnop" && !owned->truncated && pending_local &&
                        pending_local->state == DebugValueState::uninitialized && !pending_local->scalar,
                    "Borrowed/owned String, Array, math or initializer previews were unsafe or unbounded.");
            require(std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 0,
                    "Read-only Runtime inspection changed the paused object.");
            require_busy(runtime.write(host.id(), field, std::uint32_t{99}), "Paused Runtime write was admitted.");
            require_busy(runtime.create(type), "Paused object creation was admitted.");
            require_busy(runtime.destroy(host.id()), "Paused object destruction was admitted.");
            require_busy(runtime.with_native<Host>(host.id(),
                                                   [&](Host&) -> Result<void> {
                                                       entered_native = true;
                                                       return {};
                                                   }),
                         "Paused mutable native borrowing was admitted.");
            require_busy(runtime.begin_collection(), "Paused collection start was admitted.");
            require_busy(runtime.collect_step({1}), "Paused collection progress was admitted.");
            require_busy(program.entry().invoke(host.id(), args), "Paused Source reentry was admitted.");
            require_busy(program.try_publish(pending), "Paused code publication was admitted.");
            require_busy(debugger.set_breakpoints({at}), "Paused debugger mutation replaced its active binding.");
            inspected = true;
            return stop.command(DebugAction::continue_execution);
        },
        {.break_on_entry = false, .max_elements = 7, .max_text_bytes = 1024});
    take(debugger.set_breakpoints({at}));
    require(std::get<float>(*take(program.entry().invoke(host.id(), args, nullptr, &debugger))) == 2.0f && inspected &&
                !entered_native && pending && debugger.status() == DebugStatus::completed &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 5,
            "Resuming inspection lost normal behavior or consumed the rejected publication.");
    bool bounded_text{};
    Debugger bounded(
        [&](const DebugSnapshot& stop) {
            std::size_t bytes{};
            const auto count_value = [&](const auto& self, const DebugValue& value) -> void {
                bytes += value.name.size() + value.type_name.size() + value.text.size();
                for (const auto& child : value.children)
                    self(self, child);
            };
            for (const auto& frame : stop.frames) {
                bytes += frame.location.function.size() + frame.location.source.path.size();
                for (const auto& value : frame.locals)
                    count_value(count_value, value);
            }
            bounded_text = stop.truncated && bytes <= 8;
            return stop.command(DebugAction::continue_execution);
        },
        {.break_on_entry = false, .max_elements = 1, .max_text_bytes = 8});
    take(bounded.set_breakpoints({at}));
    take(program.entry().invoke(host.id(), args, nullptr, &bounded));
    require(bounded_text, "Copied names, source paths and values exceeded the shared snapshot text budget.");
    take(program.try_publish(pending));
    values.clear();
    text.clear();
    return retained;
}

void verify_failure_and_abort() {
    Runtime runtime;
    const auto type = register_host(runtime);
    take(runtime.register_type(string_definition(runtime)));
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program program(runtime, type);
    publish(program, "debug-failure.source", R"(uint run(bool native, uint divisor) {
    String live = "live until error inspection finishes";
    self.set_value(1);
    if (native) { self.fail(); }
    uint quotient = 10 / divisor;
    self.set_value(2);
    return quotient;
}
)",
            {.diagnostics = true, .debugging = true});
    const auto division_site = breakpoint(take(program.pin()), "debug-failure.source", 5);
    for (const bool native : {true, false}) {
        std::size_t errors{};
        bool before_cleanup{};
        Debugger debugger(
            [&](const DebugSnapshot& stop) -> source::DebugCommand {
                ++errors;
                const auto* live = stop.frames.empty() ? nullptr : local(stop.frames[0], "live");
                before_cleanup = stop.reason == DebugStopReason::error && stop.error && stop.frames.size() == 1 &&
                                 stop.frames[0].location.function == "run" && live &&
                                 live->state == DebugValueState::available &&
                                 live->text == "live until error inspection finishes";
                throw std::runtime_error("The debugger callback must not replace the original failure.");
            },
            {.break_on_entry = false});
        source::Diagnostics capture;
        const std::array args{Scalar{native}, Scalar{std::uint32_t{0}}};
        const auto result = program.entry().invoke(host.id(), args, native ? &capture : nullptr, &debugger);
        const auto report = capture.snapshot();
        require(!result && result.error().code == ErrorCode::invocation_failed && errors == 1 && before_cleanup &&
                    debugger.status() == DebugStatus::failed && !debugger.active() &&
                    (native ? result.error().message == "Debugger native first failure."
                            : result.error().message.find("nonzero divisor") != std::string::npos) &&
                    (!native || (report.error && report.error->message == result.error().message &&
                                 report.resources_cleared && report.remaining_owned_values == 0)) &&
                    std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1,
                "Error stop replaced the first failure, ran after cleanup or resumed user side effects.");
        if (!native)
            require(result.error().source && result.error().source->path == division_site.path &&
                        result.error().source->version == division_site.source_version &&
                        result.error().source->line == division_site.line,
                    "Debugger-only execution lost the original file/version of its generated arithmetic error.");
    }

    Program aborted(runtime, type);
    publish(aborted, "abort.source", R"(uint run() {
    String live = "owned value on the aborted path";
    self.set_value(1);
    self.set_value(2);
    return 9;
}
)",
            {.diagnostics = true, .debugging = true});
    const auto at = breakpoint(take(aborted.pin()), "abort.source", 4);
    enum class CommandFailure { abort, stale, exception };
    for (const auto failure : {CommandFailure::abort, CommandFailure::stale, CommandFailure::exception}) {
        take(runtime.write(host.id(), field, std::uint32_t{0}));
        std::size_t stops{};
        Debugger debugger(
            [&](const DebugSnapshot& stop) {
                ++stops;
                if (failure == CommandFailure::exception)
                    throw std::runtime_error("Debugger handler first failure.");
                return failure == CommandFailure::stale
                           ? source::DebugCommand{stop.stop_id + 1, DebugAction::continue_execution}
                           : stop.command(DebugAction::abort);
            },
            {.break_on_entry = false});
        take(debugger.set_breakpoints({at}));
        source::Diagnostics capture;
        const auto result = aborted.entry().invoke(host.id(), {}, &capture, &debugger);
        const auto report = capture.snapshot();
        require(
            !result && stops == 1 && !debugger.active() &&
                (failure != CommandFailure::abort || (result.error().code == ErrorCode::execution_aborted &&
                                                      debugger.status() == DebugStatus::cancelled)) &&
                (failure != CommandFailure::exception || (result.error().code == ErrorCode::invocation_failed &&
                                                          result.error().message == "Debugger handler first failure." &&
                                                          debugger.status() == DebugStatus::failed)) &&
                report.error && report.error->message == result.error().message && report.resources_cleared &&
                report.remaining_owned_values == 0 &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1,
            "Abort, stale command or handler failure repeated the callback, lost its first cause or resumed the next statement.");
    }
}

void verify_async_debug_lifetime() {
    Runtime runtime;
    Program program(runtime);
    publish(program, "async-debug.source", R"(uint helper(uint value) { return value + 1; }
async uint run(uint value) {
    uint local = helper(value);
    yield;
    local = helper(local);
    return local;
}
)",
            {.diagnostics = true, .debugging = true}, "run");
    const auto old = take(program.pin());
    const auto entry = program.entry();
    const std::array args{Scalar{std::uint32_t{41}}};
    Coroutine* current{};
    std::vector<std::uint64_t> activations;
    std::uint64_t version{};
    bool saw_yield{}, crossed_yield{};
    std::size_t stops{};
    Debugger debugger([&](const DebugSnapshot& stop) {
        require(++stops < 64 && current && current->state() == CoroutineState::running &&
                    debugger.status() == DebugStatus::paused && !stop.frames.empty(),
                "Coroutine stop escaped the active resume or did not enter the paused state.");
        if (!version)
            version = stop.code_version;
        require(stop.code_version == version && stop.frames[0].location.source.path == "async-debug.source",
                "A resumed debugger switched to newly published source.");
        const auto& frame = stop.frames[0];
        if (frame.location.function == "run") {
            const auto* parameter = local(frame, "value");
            require(parameter && number(*parameter) == 41, "Async parameter storage changed across a suspension.");
            if (frame.location.source.line == 5 || frame.location.source.line == 6) {
                const auto* current_value = local(frame, "local");
                require(current_value && number(*current_value) == (frame.location.source.line == 5 ? 42u : 43u),
                        "Async debug local storage did not survive its suspended interval.");
            }
        }
        if (frame.location.function == "helper" && (activations.empty() || activations.back() != frame.activation_id))
            activations.push_back(frame.activation_id);
        if (frame.location.function == "run" && frame.location.source.line == 4) {
            const auto* value = local(frame, "local");
            require(value && number(*value) == 42, "Yield snapshot lost its coroutine-frame local.");
            saw_yield = true;
            return stop.command(DebugAction::step_over);
        }
        if (saw_yield && frame.location.function == "run" && frame.location.source.line == 5)
            crossed_yield = true;
        return stop.command(DebugAction::step_into);
    });
    source::Diagnostics capture;
    auto task = take(entry.start({}, args, &capture, &debugger));
    current = &task;
    require(debugger.status() == DebugStatus::ready && stops == 0, "Starting async code executed a debug stop.");
    require(take(task.resume()) == CoroutineState::suspended && debugger.status() == DebugStatus::suspended &&
                saw_yield,
            "Debug stepping did not preserve the language yield boundary.");
    publish(program, "async-replacement.source", "async uint run(uint value) { yield; return value + 100; }",
            {.debugging = true});
    program.close();
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 43 &&
                debugger.status() == DebugStatus::completed && !debugger.active() && crossed_yield &&
                activations.size() == 2 && activations[0] != activations[1] && capture.snapshot().resources_cleared,
            "Step-across-yield, dynamic helper identity or old-version execution failed after close.");
    const auto completed_stops = stops;
    task.cancel();
    task.cancel();
    require(stops == completed_stops && debugger.status() == DebugStatus::completed,
            "Repeated terminal cancellation ran debugger cleanup stops.");

    std::size_t cancel_stops{};
    Debugger cancelled([&](const DebugSnapshot& stop) {
        ++cancel_stops;
        return stop.command(DebugAction::continue_execution);
    });
    auto ready = take(old.start({}, args, nullptr, &cancelled));
    ready.cancel();
    ready.cancel();
    require(cancelled.status() == DebugStatus::cancelled && !cancelled.active() && cancel_stops == 0,
            "Ready cancellation entered source code or left its debug session active.");
    auto suspended = take(old.start({}, args, nullptr, &cancelled));
    take(suspended.resume());
    suspended.cancel();
    require(cancelled.status() == DebugStatus::cancelled && !cancelled.active() && cancel_stops == 1,
            "Suspended cancellation stopped during cleanup or lost its terminal state.");

    Debugger abort([](const DebugSnapshot& stop) { return stop.command(DebugAction::abort); });
    auto aborted = take(old.start({}, args, nullptr, &abort));
    require(take(aborted.resume()) == CoroutineState::cancelled && abort.status() == DebugStatus::cancelled &&
                !abort.active() && !aborted.result(),
            "Async abort did not resolve through Runtime cancellation without a result.");

    std::unique_ptr<Debugger> shell;
    shell = std::make_unique<Debugger>([&](const DebugSnapshot& stop) {
        shell.reset();
        return stop.command(DebugAction::continue_execution);
    });
    auto observer = *shell;
    auto detached = take(old.start({}, args, nullptr, shell.get()));
    take(detached.resume());
    take(detached.resume());
    require(!shell && observer.status() == DebugStatus::completed && !observer.active() && observer.last_stop(),
            "Destroying the public debugger inside its handler invalidated the retained session.");
}
}

void verify_source_debugger() {
    verify_compile_modes_and_stepping();
    verify_exact_breakpoints();
    const auto retained = verify_inspection_and_previews();
    const auto* text = local(retained.frames[0], "text");
    const auto* values = local(retained.frames[0], "values");
    require(text && text->text == "abcdefghijklmnop" && values && values->children[0].scalar &&
                std::get<float>(*values->children[0].scalar) == 1.0f,
            "An owning debug snapshot borrowed storage from destroyed native values or Runtime.");
    verify_failure_and_abort();
    verify_async_debug_lifetime();
    std::puts(
        "Source debugger: PASS (stepping, exact breakpoints, bounded snapshots, inspection guards, first errors, abort, coroutine lifetime)");
}
