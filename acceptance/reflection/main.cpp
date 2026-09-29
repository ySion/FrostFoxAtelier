#include "reflection/fixture.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;
using namespace atelier::acceptance::reflection;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void invoke(Runtime& runtime, ObjectId object, TypeId type, const char* name,
    std::span<const Scalar> args, Scalar expected) {
    Scalar output;
    const auto called = runtime.call(object, take(runtime.find_method(type, name)), args, &output);
    if (!called) throw std::runtime_error(called.error().message);
    require(output == expected, "Generated member invoker changed its signature or receiver.");
}
}
int main() {
    try {
        Runtime runtime;
        const auto types = take(runtime.register_types({
            atelier::reflection::Definition<Sample>::build(runtime),
            atelier::reflection::Definition<Counter>::build(runtime)}));
        auto object = take(runtime.create(types[1]));
        require(Counter::live == 1, "Generated native construction did not run exactly once.");
        invoke(runtime, object.id(), types[1], "read", {}, 3.f);
        const std::array count{Scalar{std::uint32_t{7}}}, scale{Scalar{2.f}};
        invoke(runtime, object.id(), types[1], "count", count, std::uint32_t{7});
        invoke(runtime, object.id(), types[1], "scale", scale, 6.f);
        require(!runtime.find_field(types[1], "sample_"), "Generated hidden record became public.");
        const auto info = take(runtime.describe(types[1]));
        require(info->methods.front().signature.receiver->read_only, "Generated const method lost receiver metadata.");
#if defined(_DEBUG)
        require(runtime.find_field(types[1], "debug_only").has_value() && !runtime.find_field(types[1], "release_only"), "Generator lost dev preprocessing context.");
#else
        require(runtime.find_field(types[1], "release_only").has_value() && !runtime.find_field(types[1], "debug_only"), "Generator lost release preprocessing context.");
#endif
        const auto destroyed = runtime.destroy(object.id());
        if (!destroyed) throw std::runtime_error(destroyed.error().message);
        require(Counter::live == 0 && !runtime.type_of(object.id()), "Generated native destruction or identity invalidation failed.");
        std::puts("Reflection consumer: PASS (private records, scalar overloads, const/noexcept, target macros, lifecycle)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Reflection consumer: FAIL: %s\n", error.what());
        return 1;
    }
}
