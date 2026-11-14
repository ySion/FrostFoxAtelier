#include "../helpers.hpp"
#include "reflection/fixture.hpp"
#include "runtime/native.hpp"
#include <cstdio>

void verify_reflection() {
    using namespace atelier;
    using namespace acceptance::helpers;
    using namespace acceptance::reflection;
    TypeInfo retained;
    {
        Runtime runtime;
        const auto types = take(runtime.register_types({atelier::reflection::Definition<Sample>::build(runtime),
                                                        atelier::reflection::Definition<Counter>::build(runtime)}));
        retained = *take(runtime.describe(types[0]));
        auto object = take(runtime.create(types[1]));
        require(Counter::live == 1, "Generated constructor did not run.");
        take(runtime.destroy(object.id()));
        require(Counter::live == 0, "Generated destructor did not run.");
    }
    const std::vector<std::pair<std::string, std::string>> expected{{"test.binding", "evaluate.values"},
                                                                    {"test.note", std::string{"分组\0尾", 10}}};
    require(retained.fields[0].metadata == expected, "Generated metadata did not outlive Runtime.");
    std::puts("Reflection consumer: PASS (generated producer and owned metadata)");
}
