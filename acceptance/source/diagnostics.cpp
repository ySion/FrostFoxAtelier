#include "../helpers.hpp"
#include "native_values.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/diagnostics.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

using atelier::acceptance::borrow::Particle;

using atelier::acceptance::borrow::Position;

using source::DiagnosticStatus;

using source::Diagnostics;

using source::Program;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view name, std::string_view text,
             source::CompileOptions options = {.diagnostics = true}, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request(name, text, entry, options))));
    take(program.try_publish(candidate));
}

struct Host {
    std::uint32_t value{};

    Coroutine* task{};

    void fail() {
        value = static_cast<std::uint32_t>(Particle::live);
        throw std::runtime_error("Diagnostic native failure.");
    }

    void cancel_fail() {
        task->cancel();
        value = 1;
        throw std::runtime_error("Diagnostic native failure.");
    }
};

TypeId register_host(Runtime& runtime) {
    return take(runtime.register_type(NativeRegistration<Host>{runtime, "diagnostic.Host", TypeKind::object}
                                          .field<&Host::value>("value")
                                          .method<&Host::fail>("fail", true)
                                          .method<&Host::cancel_fail>("cancel_fail", true)
                                          .build()));
}

void verify_gc_lifetime() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program program(runtime, type);
    publish(program, "gc.source", "void run() { self.value = self.value + 1; }");
    Diagnostics capture;
    take(runtime.create(type)).reset();
    take(runtime.begin_collection());
    take(program.entry().invoke(host.id(), {}, &capture));
    require(capture.snapshot().status == DiagnosticStatus::completed && !capture.active() &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1 && runtime.live_objects() == 2 &&
                runtime.root_count() == 1 && runtime.collection_phase() == CollectionPhase::roots,
            "Diagnostic execution advanced host GC or retained an invocation root.");
    std::size_t reclaimed{};
    while (runtime.collection_phase() != CollectionPhase::idle)
        reclaimed += take(runtime.collect_step({1})).reclaimed;
    require(reclaimed == 1 && runtime.live_objects() == 1,
            "Host collection did not reclaim the unrooted object after diagnostic execution.");
}

void verify_owned_failure_cleanup() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto particle = reflection::Definition<Particle>::build(runtime);
    particle.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({std::move(particle), reflection::Definition<Position>::build(runtime),
                                 string_definition(runtime),
                                 array_definition<Particle>(runtime, "diagnostic.Particles")}));
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program owned(runtime, type);
    publish(owned, "owned-diagnostic.source", R"(
uint leaf() {
    Array<borrow.Particle> local;
    local.resize(1);
    self.fail();
    return 0;
}
uint run() { return leaf(); }
)",
            {.diagnostics = true}, "run");
    require(Particle::live == 0, "Native failure fixture started with an unrelated live Particle.");
    Diagnostics capture;
    const auto failed = owned.entry().invoke(host.id(), {}, &capture);
    // The native callback observes the actual live value before it throws. A
    // skipped constructor cannot pass merely because the final live count is zero.
    require(!failed && capture.snapshot().status == DiagnosticStatus::failed && !capture.active() &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1 && Particle::live == 0 &&
                runtime.root_count() == 1,
            "Diagnostic native failure skipped the owned value or failed to release it and its invocation root.");
}

void verify_coroutine_lifetime() {
    Runtime runtime;
    const auto type = register_host(runtime);
    auto host = take(runtime.create(type));
    const auto field = take(runtime.find_field(type, "value"));
    Program program(runtime, type);
    publish(program, "suspend.source", "async uint run(uint value) { yield; return value + 1; }");
    const auto entry = program.entry();
    const std::array args{Scalar{std::uint32_t{41}}};
    Diagnostics capture;
    auto cancelled = take(entry.start(host.id(), args, &capture));
    require(take(cancelled.resume()) == CoroutineState::suspended, "Cancellation fixture did not yield.");
    cancelled.cancel();
    require(capture.snapshot().status == DiagnosticStatus::cancelled && !capture.active() && runtime.root_count() == 1,
            "Suspended cancellation retained a capture or receiver root.");

    Coroutine task;
    {
        Diagnostics temporary({.timing = true});
        capture = temporary;
        task = take(entry.start(host.id(), args, &temporary));
        require(take(task.resume()) == CoroutineState::suspended, "Diagnostic coroutine did not yield.");
    }
    publish(program, "replacement.source", "async uint run(uint value) { yield; return value + 100; }");
    program.close();
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 42 &&
                capture.snapshot().status == DiagnosticStatus::completed && !capture.active() &&
                runtime.root_count() == 1,
            "Old coroutine lost its code/capture after publication and close, or retained its receiver root.");

    Program callback(runtime, type);
    publish(callback, "cancel-callback.source", "async uint run(uint value) { self.cancel_fail(); return value; }");
    auto active = take(callback.entry().start(host.id(), args, &capture));
    take(runtime.with_native<Host>(host.id(), [&](Host& value) -> Result<void> {
        value.task = &active;
        return {};
    }));
    const auto state = active.resume();
    take(runtime.with_native<Host>(host.id(), [](Host& value) -> Result<void> {
        value.task = nullptr;
        return {};
    }));
    require(!state && capture.snapshot().status == DiagnosticStatus::failed && !capture.active() &&
                std::get<std::uint32_t>(take(runtime.read(host.id(), field))) == 1 && runtime.root_count() == 1,
            "Running cancellation followed by a native failure skipped the callback or retained execution roots.");
}

void verify_detached_reports_and_artifacts() {
    Diagnostics capture;
    source::CompilationArtifacts artifacts;
    {
        Runtime runtime;
        Program program(runtime);
        publish(program, "detached.source", "uint run(uint divisor) { return 9 / divisor; }",
                {.diagnostics = true, .artifacts = true});
        artifacts = take(take(program.pin()).artifacts());
        require(!program.entry().invoke({}, std::array{Scalar{std::uint32_t{0}}}, &capture),
                "Detached-report scenario did not fail.");
    }
    const auto retained = capture.snapshot();
    require(retained.status == DiagnosticStatus::failed && retained.error && retained.location &&
                retained.location->source.path == "detached.source" && !artifacts.object_code.empty() &&
                !artifacts.optimized_ir.empty(),
            "Diagnostic output borrowed data from a retired Runtime or executable version.");
    {
        Runtime runtime;
        Program program(runtime);
        publish(program, "second-runtime.source", "uint run(uint value) { return value + 2; }");
        const auto result = take(program.entry().invoke({}, std::array{Scalar{std::uint32_t{40}}}, &capture));
        const auto report = capture.snapshot();
        require(result && std::get<std::uint32_t>(*result) == 42 && report.status == DiagnosticStatus::completed &&
                    report.source_name == "second-runtime.source" && !report.error && !capture.active(),
                "Reusing a capture after Runtime retirement retained stale output or changed the new call.");
    }
}
}

void verify_source_diagnostics() {
    verify_gc_lifetime();
    verify_owned_failure_cleanup();
    verify_coroutine_lifetime();
    verify_detached_reports_and_artifacts();
    std::puts("Source diagnostics: PASS (GC, native failure cleanup, coroutine lifetime, detached reports/artifacts)");
}
