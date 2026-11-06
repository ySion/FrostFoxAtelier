#include "../helpers.hpp"
#include "source/native_values.hpp"
#include "source/debugger.hpp"
#include "source/diagnostics.hpp"
#include "source/program.hpp"
#include "memory/fatal.hpp"

#include <array>
#include <cstdio>

namespace {
using namespace atelier;

using namespace atelier::source;

using namespace atelier::acceptance::helpers;

using acceptance::borrow::Particle;

void publish(Program& program, std::string_view path, std::string_view text,
             CompileOptions options = {.debugging = true}, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request(path, text, entry, options))));
    take(program.try_publish(candidate));
}

DebugBreakpoint breakpoint(const Program::Lease& lease, std::string_view path, std::size_t line) {
    for (const auto& site : take(lease.debug_sites()))
        if (site.source.path == path && site.source.line == line)
            return {site.source.path, site.source.version, site.source.line, site.source.column};
    fatal("Requested source line has no executable debug site.");
}

const DebugValue* local(const DebugFrame& frame, std::string_view name) {
    for (const auto& value : frame.locals)
        if (value.name == name)
            return &value;
    return nullptr;
}

std::uint32_t number(const DebugValue& value) noexcept {
    if (value.state != DebugValueState::available || !value.scalar)
        fatal("Debug uint local is unavailable.");
    const auto* result = std::get_if<std::uint32_t>(&*value.scalar);
    if (!result)
        fatal("Debug uint local has the wrong scalar type.");
    return *result;
}

struct Host {
    std::uint32_t value{};

    void set_value(std::uint32_t next) noexcept { value = next; }

    Result<void> fail() noexcept {
        return std::unexpected(Error{ErrorCode::invocation_failed, "Debugger native first failure."});
    }
};

void verify_compile_modes_and_stepping() {
    Runtime runtime;
    auto calls = take(Program::create(runtime));
    publish(*calls, "steps.source", R"(uint leaf(uint value) {
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
    bool entered_leaf{};
    bool returned_to_caller{};
    bool initialized_result{};
    Debugger into([&](const DebugSnapshot& stop) noexcept {
        if (stop.frames.empty())
            fatal("Debugger stop has no active frame.");
        const auto& frame = stop.frames[0];
        if (frame.location.function == "leaf") {
            const auto* parameter = local(frame, "value");
            const auto* initializing = stop.frames.size() > 1 ? local(stop.frames[1], "result") : nullptr;
            if (!parameter || number(*parameter) != 21 || !initializing ||
                initializing->state != DebugValueState::uninitialized || initializing->scalar)
                fatal("Helper entry read an unfinished caller initializer or lost its parameter.");
            entered_leaf = true;
            return stop.command(DebugAction::step_out);
        }
        if (frame.location.function != "run")
            fatal("Step-out lost the caller frame.");
        if (entered_leaf) {
            returned_to_caller = true;
            if (const auto* value = local(frame, "result"); value && value->state == DebugValueState::available)
                initialized_result = number(*value) == 42;
            return stop.command(DebugAction::step_over);
        }
        return stop.command(DebugAction::step_into);
    });
    require(std::get<std::uint32_t>(*take(calls->entry().invoke({}, std::array{Scalar{21u}}, nullptr, &into))) == 43 &&
                entered_leaf && returned_to_caller && initialized_result,
            "Step-into/out did not resume the caller and publish its initialized result.");
}

DebugSnapshot verify_inspection_and_previews() {
    Runtime runtime;
    take(runtime.register_types({string_definition(runtime), array_definition<float>(runtime, "debugger.Floats")}));
    auto program = take(Program::create(runtime));
    publish(*program, "preview.source", R"(float run(const Array<float>& values, const String& text) {
    String owned = text;
    uint checkpoint = 0;
    return values[0];
}
)");
    Array<float> values{1.0f};
    const auto text = String::literal<"abcdefghijklmnop">();
    DebugSnapshot retained;
    Debugger debugger(
        [&](const DebugSnapshot& stop) noexcept {
            retained = stop;
            return stop.command(DebugAction::continue_execution);
        },
        {.break_on_entry = false});
    take(debugger.set_breakpoints({breakpoint(take(program->pin()), "preview.source", 3)}));
    take(program->entry().invoke(
        {}, std::array{Argument::borrow(std::as_const(values)), Argument::borrow(std::as_const(text))}, nullptr,
        &debugger));
    return retained;
}

void verify_failure_and_abort() {
    Runtime runtime;
    auto particle = reflection::Definition<Particle>::build(runtime);
    particle.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({std::move(particle),
                                 reflection::Definition<acceptance::borrow::Position>::build(runtime),
                                 string_definition(runtime),
                                 NativeRegistration<Host>{runtime, "debugger.Host", TypeKind::object}
                                     .field<&Host::value>("value")
                                     .method<&Host::set_value>("set_value", true)
                                     .method<&Host::fail>("fail", true)
                                     .build()}));
    const auto type = take(runtime.native_type<Host>());
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    auto program = take(Program::create(runtime, type));
    publish(*program, "debug-failure.source", R"(void run() {
    borrow.Particle live;
    live.name = "live until error inspection finishes";
    self.set_value(1);
    self.fail();
    self.set_value(2);
}
)",
            {.diagnostics = true, .debugging = true});
    bool before_cleanup{};
    Debugger failed(
        [&](const DebugSnapshot& stop) noexcept -> Result<DebugCommand> {
            before_cleanup = stop.reason == DebugStopReason::error && Particle::live == 1;
            return std::unexpected(Error{ErrorCode::capacity_exceeded, "Debugger error handler failure."});
        },
        {.break_on_entry = false});
    Diagnostics capture;
    const auto failure = program->entry().invoke(host.id(), {}, &capture, &failed);
    require(!failure && failure.error().code == ErrorCode::invocation_failed && before_cleanup && Particle::live == 0 &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1,
            "Error stop ran after cleanup, leaked its owned value or resumed user side effects.");

    auto aborted = take(Program::create(runtime, type));
    publish(*aborted, "abort.source", R"(uint run() {
    borrow.Particle live;
    self.set_value(1);
    self.set_value(2);
    return 9;
}
)",
            {.diagnostics = true, .debugging = true});
    const auto at = breakpoint(take(aborted->pin()), "abort.source", 4);
    const auto call = take(take(aborted->pin()).bind<std::uint32_t>());
    for (const bool handler_fails : {false, true}) {
        take(runtime.write(host.id(), field, std::uint32_t{0}));
        bool reached{};
        bool refuse = true;
        unsigned stops{};
        Debugger debugger(
            [&](const DebugSnapshot& stop) noexcept -> Result<DebugCommand> {
                reached = Particle::live == 1;
                ++stops;
                if (!refuse)
                    return stop.command(DebugAction::continue_execution);
                if (handler_fails)
                    return std::unexpected(Error{ErrorCode::capacity_exceeded, "Debugger handler failure."});
                return stop.command(DebugAction::abort);
            },
            {.break_on_entry = false});
        take(debugger.set_breakpoints({at}));
        Diagnostics aborted_capture;
        const auto outcome = call.invoke(host.id(), &aborted_capture, &debugger);
        require(!outcome &&
                    outcome.error().code ==
                        (handler_fails ? ErrorCode::capacity_exceeded : ErrorCode::execution_aborted) &&
                    reached && Particle::live == 0 && !debugger.active() &&
                    std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1,
                "Abort or handler failure leaked the owned value or resumed the next statement.");
        refuse = false;
        require(take(call.invoke(host.id(), &aborted_capture, &debugger)) == 9 && stops == 2 && Particle::live == 0 &&
                    debugger.status() == DebugStatus::completed && !debugger.active() &&
                    std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 2,
                "Debugger failure did not release inspection or reset the session for recovery.");
    }
}

void verify_async_debug_lifetime() {
    Runtime runtime;
    auto program = take(Program::create(runtime));
    publish(*program, "async-debug.source", R"(uint helper(uint value) { return value + 1; }
async uint run(uint value) {
    uint local = helper(value);
    yield;
    local = helper(local);
    return local;
}
)",
            {.diagnostics = true, .debugging = true}, "run");
    const auto old = take(program->pin());
    const std::array args{Scalar{41u}};
    bool saw_yield{};
    bool crossed_yield{};
    Debugger debugger([&](const DebugSnapshot& stop) noexcept {
        if (stop.frames.empty())
            fatal("Debugger stop has no active frame.");
        const auto& frame = stop.frames[0];
        if (frame.location.source.path != "async-debug.source")
            fatal("Resumed debugger switched source versions.");
        if (frame.location.function == "run" && frame.location.source.line == 4) {
            const auto* value = local(frame, "local");
            if (!value || number(*value) != 42)
                fatal("Yield snapshot lost its coroutine-frame local.");
            saw_yield = true;
            return stop.command(DebugAction::step_over);
        }
        if (saw_yield && frame.location.function == "run" && frame.location.source.line == 5)
            crossed_yield = true;
        return stop.command(DebugAction::step_into);
    });
    Diagnostics capture;
    auto task = take(program->entry().start({}, args, &capture, &debugger));
    require(take(task.resume()) == CoroutineState::suspended && saw_yield,
            "Debug stepping did not preserve the language yield boundary.");
    publish(*program, "async-replacement.source", "async uint run(uint value) { yield; return value + 100; }");
    program->close();
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 43 &&
                debugger.status() == DebugStatus::completed && !debugger.active() && crossed_yield,
            "Step-across-yield or old-version execution failed after close.");
    std::unique_ptr<Debugger> shell;
    shell = std::make_unique<Debugger>([&](const DebugSnapshot& stop) noexcept {
        shell.reset();
        return stop.command(DebugAction::continue_execution);
    });
    auto observer = *shell;
    auto detached = take(old.start({}, args, nullptr, shell.get()));
    take(detached.resume());
    take(detached.resume());
    require(!shell && observer.status() == DebugStatus::completed && !observer.active() && observer.last_stop() &&
                observer.last_stop()->frames.at(0).location.source.path == "async-debug.source",
            "Releasing the public debugger inside its handler invalidated the retained session.");
}
}

void verify_source_debugger() {
    verify_compile_modes_and_stepping();
    const auto retained = verify_inspection_and_previews();
    const auto& frame = retained.frames.at(0);
    const auto* text = local(frame, "text");
    const auto* owned = local(frame, "owned");
    const auto* values = local(frame, "values");
    require(text && text->text == "abcdefghijklmnop" && owned && owned->text == "abcdefghijklmnop" && values &&
                values->children.at(0).scalar && std::get<float>(*values->children[0].scalar) == 1.0f,
            "Owning debug snapshot borrowed storage from destroyed native values or Runtime.");
    verify_failure_and_abort();
    verify_async_debug_lifetime();
    std::puts("Source debugger: PASS (step into/out, owning previews, failure cleanup, async lifetime)");
}
