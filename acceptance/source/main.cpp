#include "source/program.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <utility>

void verify_source_shutdown();
void verify_source_session();
void verify_source_inheritance();

namespace {
using namespace atelier;
using source::Program;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void take(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

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

    auto incompatible = Program::prepare(take(program.request("public-wrong-kind.source",
        "async uint adjust(uint value) { yield; return value; }")));
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
    require(take(task.resume()) == CoroutineState::completed &&
        std::get<std::uint32_t>(*take(task.result())) == 7,
        "Suspended public coroutine lost its parameters or code after program close.");

    Program void_program(runtime, {});
    publish(void_program, "public-void.source", "void complete() {}");
    require(!take(take(void_program.bind<void>()).invoke({}, {})), "Void source fabricated a scalar result.");

    Program floats(runtime, {});
    publish(floats, "public-float.source", "float identity(float value) { return value; }");
    const auto float_entry = take(floats.bind<float, float>());
    Program wide(runtime, {});
    publish(wide, "public-wide.source",
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

int main() {
    try {
        verify_public_program();
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
