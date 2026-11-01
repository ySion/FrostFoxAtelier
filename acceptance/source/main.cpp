#include "../helpers.hpp"
#include "source/program.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <utility>

void verify_source_shutdown();

void verify_source_session();

void verify_source_inheritance();

void verify_native_value_borrows();

void verify_source_owned_values();

void verify_source_modules();

void verify_source_types();

void verify_source_value_fields();

void verify_source_foundations();

void verify_source_plain_records();

void verify_source_diagnostics();

void verify_source_debugger();

void verify_source_materials();

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view name, std::string_view source_text) {
    auto candidate = take(Program::prepare(take(program.request(name, source_text))));
    take(program.try_publish(candidate));
}

void verify_public_program() {
    Runtime runtime;
    Program program(runtime, {});
    publish(program, "public-sync.source", "uint adjust(uint value) { return value + 1; }");
    const auto bound = take(program.bind<std::uint32_t, std::uint32_t>());
    const std::array arguments{Scalar{std::uint32_t{41}}};
    require(std::get<std::uint32_t>(*take(bound.invoke({}, arguments))) == 42,
            "Public source binding returned an incorrect scalar.");
    const auto old = take(program.pin());
    publish(program, "public-replacement.source", "uint adjust(uint value) { return value + 2; }");
    require(std::get<std::uint32_t>(*take(bound.invoke({}, arguments))) == 43 &&
                std::get<std::uint32_t>(*take(old.invoke({}, arguments))) == 42,
            "Public entry and lease did not select their respective versions.");

    auto incompatible = Program::prepare(
        take(program.request("public-wrong-kind.source", "async uint adjust(uint value) { yield; return value; }")));
    require(!incompatible, "Compatible publication accepted a different execution kind.");
    require(std::get<std::uint32_t>(*take(bound.invoke({}, arguments))) == 43,
            "Rejected compilation changed the published version.");

    Coroutine task;
    {
        Program asynchronous(runtime, {});
        publish(asynchronous, "public-async.source", "async uint delayed(uint value) { yield; return value; }");
        const auto entry = take(asynchronous.bind<std::uint32_t, std::uint32_t>(FunctionExecution::coroutine));
        std::array copied_arguments{Scalar{std::uint32_t{7}}};
        task = take(entry.start({}, copied_arguments));
        copied_arguments[0] = std::uint32_t{99};
        require(task.state() == CoroutineState::ready, "Creation executed an async body.");
        require(take(task.resume()) == CoroutineState::suspended, "Public async entry did not suspend.");
    }
    require(take(task.resume()) == CoroutineState::completed && std::get<std::uint32_t>(*take(task.result())) == 7,
            "Suspended public coroutine lost its parameters or code after program close.");

    Program void_program(runtime, {});
    publish(void_program, "public-void.source", "void complete() {}");
    require(!take(take(void_program.bind<void>()).invoke({}, {})), "Void source fabricated a scalar result.");

    Program floats(runtime, {});
    publish(floats, "public-float.source", "float identity(float value) { return value; }");
    const auto float_entry = take(floats.bind<float, float>());
    Program wide(runtime, {});
    publish(
        wide, "public-wide.source",
        "float last(float a, float b, float c, float d, float e, float f, float g, float h, float i, float j) { return j; }");
    for (const auto bits : {0x80000000u, 0x7fc54321u}) {
        const std::array value{Scalar{std::bit_cast<float>(bits)}};
        auto result = take(float_entry.invoke({}, value));
        require(result && std::holds_alternative<float>(*result) &&
                    std::bit_cast<std::uint32_t>(std::get<float>(*result)) == bits,
                "Public inline argument conversion changed float bits.");
        std::array<Scalar, 10> many_arguments{};
        many_arguments.back() = value.front();
        result = take(wide.entry().invoke({}, many_arguments));
        require(result && std::holds_alternative<float>(*result) &&
                    std::bit_cast<std::uint32_t>(std::get<float>(*result)) == bits,
                "Public expanded argument conversion changed float bits.");
    }
    take(runtime.check_code_publication());
}
}

void verify_source_math();

void verify_source_projects();

void measure_source_math();

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--materials") {
            verify_source_materials();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--projects") {
            verify_source_projects();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--math") {
            verify_source_math();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--math-measure") {
            measure_source_math();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--foundations") {
            verify_source_foundations();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--diagnostics") {
            verify_source_diagnostics();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--debugger") {
            verify_source_debugger();
            return 0;
        }
        verify_source_debugger();
        verify_source_diagnostics();
        verify_source_projects();
        verify_source_math();
        verify_source_plain_records();
        verify_public_program();
        verify_native_value_borrows();
        verify_source_owned_values();
        verify_source_modules();
        verify_source_types();
        verify_source_value_fields();
        verify_source_foundations();
        verify_source_inheritance();
        verify_source_shutdown();
        verify_source_session();
        std::puts("Source public consumer: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Source public consumer: FAIL: %s\n", error.what());
        return 1;
    }
}
