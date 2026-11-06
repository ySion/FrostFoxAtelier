#include "../helpers.hpp"
#include "reflection/fixture.hpp"
#include "source/program.hpp"
#include <array>

namespace {
using namespace atelier;

using namespace atelier::acceptance::reflection;

using source::Program;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view text) {
    auto request = take(program.request("shared-calls.source", text));
    auto candidate = take(Program::prepare(request));
    take(program.try_publish(candidate));
}
}

void verify_shared_calls(atelier::Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    auto object = take(runtime.create(type));
    int value = 1;
    const std::array aliases{Argument::borrow(value), Argument::borrow(value)};
    auto source = take(Program::create(runtime, type));
    publish(*source, R"(
int run(const int& read, int& write) {
    self.aliases(read, write);
    return read;
})");
    auto entry = source->entry();
    auto result = take(entry.invoke(object.id(), aliases));
    require(result && std::get<int>(*result) == 20 && value == 20, "Host/JIT/native mixed alias chain failed.");
    auto evaluation = take(Program::create(runtime, type));
    publish(*evaluation, "int order() { self.value = 1; return self.mixed(self.value, self.mutate(), self.value); }");
    require(std::get<int>(*take(evaluation->entry().invoke(object.id(), {}))) == 8,
            "Nested call overwrote an earlier by-value operand.");
}
