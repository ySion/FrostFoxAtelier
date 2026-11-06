#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "reflection/fixture.hpp"
#include "runtime/native.hpp"
#include <array>
#include <cstdio>
#include <utility>

void verify_shared_calls(atelier::Runtime& runtime);

namespace {
using namespace atelier;

using namespace atelier::acceptance::reflection;

using namespace atelier::acceptance::helpers;

void verify_descriptions() {
    struct Manual {
        float value{};

        float read() const noexcept { return value; }
    };
    TypeInfo retained{};
    TypeInfo generated{};
    TypeInfo inherited{};
    {
        Runtime native;
        auto definition =
            NativeRegistration<Manual>{native, "reflection.ManualDescription", TypeKind::object, "手工 Type\r\n说明"}
                .field<&Manual::value>("value", {TypeReference::native<float>()}, offsetof(Manual, value), {}, {},
                                       "手工 Field\r说明")
                .metadata({{"test.note", std::string{"分组\0尾", 10}}, {"test.binding", "evaluate.values"}})
                .method<&Manual::read>("read", true, "手工 Method\n说明")
                .build();
        const auto id = take(native.register_type(definition));
        definition = {};
        retained = *take(native.describe(id));
        TypeDefinition child;
        child.name = "reflection.Child";
        child.base = id;
        inherited = *take(native.describe(take(native.register_type(std::move(child)))));
        require(inherited.fields[0].id == take(native.find_field(id, "value")),
                "Inherited metadata changed its declaring field identity.");
        generated =
            *take(native.describe(take(native.register_type(atelier::reflection::Definition<Sample>::build(native)))));
    }
    require(retained.description == "手工 Type\n说明" && retained.fields[0].description == "手工 Field\n说明" &&
                retained.methods[0].description == "手工 Method\n说明",
            "Manual producer or copied metadata failed to own normalized descriptions after Runtime shutdown.");
    const std::vector<std::pair<std::string, std::string>> expected{{"test.binding", "evaluate.values"},
                                                                    {"test.note", std::string{"分组\0尾", 10}}};
    require(retained.fields[0].metadata == expected && generated.fields[0].metadata == expected &&
                inherited.fields[0].metadata == expected,
            "Field Meta lost canonical ordering, decoded bytes, inheritance or owned publication lifetime.");
}

void verify_record_call(Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    auto object = take(runtime.create(type));
    const auto plain_type = take(runtime.native_type<PlainRecord>());
    static_assert(sizeof(PlainRecord) > 128 && alignof(PlainRecord) == 64);
    PlainRecord record{true, Mode::active, math::Matrix4d::identity(), 4};
    PlainRecord record_output{};
    take(runtime.call(object.id(), take(runtime.find_method(type, "record_alias")),
                      std::array{Argument::borrow(std::as_const(record)), Argument::borrow(record)},
                      ValueOutput{plain_type, &record_output}));
    require(record.count == 14 && record_output.count == 4 && !record_output.enabled &&
                record_output.mode == Mode::active,
            "Generated record thunk lost aligned value snapshots, bool/enum fields or typed large output.");
}

void verify_failed_borrow(Runtime& runtime) {
    const auto type = take(runtime.native_type<CallProbe>());
    auto object = take(runtime.create(type));
    struct Guarded {
        bool value{};

        bool before{true};

        bool after{true};

        std::uint32_t guard{0x12345678};
    } guarded;
    Scalar output{false};
    const auto failed = runtime.call(object.id(), take(runtime.find_method(type, "fail_bool")),
                                     std::array{Argument::borrow(guarded.value)}, &output);
    require(!failed && guarded.value && !std::get<bool>(output) && guarded.before && guarded.after &&
                guarded.guard == 0x12345678,
            "Generated bool failure lost committed reference writes or staged output.");
    const std::array arguments{Argument{Scalar{true}}, Argument::borrow(std::as_const(guarded.value)),
                               Argument::borrow(guarded.value)};
    take(runtime.call(object.id(), take(runtime.find_method(type, "bool_alias")), arguments, &output));
    require(std::get<bool>(output), "Generated bool failure left execution admission closed.");
}
}

int main() {
    atelier::install_fatal_handler();
    try {
        Runtime runtime;
        const auto types = take(runtime.register_types({atelier::reflection::Definition<Sample>::build(runtime),
                                                        atelier::reflection::Definition<Counter>::build(runtime),
                                                        atelier::reflection::Definition<CallProbe>::build(runtime),
                                                        atelier::reflection::Definition<Mode>::build(runtime),
                                                        atelier::reflection::Definition<PlainRecord>::build(runtime)}));
        verify_descriptions();
        auto object = take(runtime.create(types[1]));
        require(Counter::live == 1, "Generated native construction did not run exactly once.");
        verify_record_call(runtime);
        verify_failed_borrow(runtime);
        verify_shared_calls(runtime);
        take(runtime.destroy(object.id()));
        require(Counter::live == 0, "Generated native destruction failed.");
        std::puts("Reflection consumer: PASS (generated ABI, borrowed failure cleanup and owned metadata)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Reflection consumer: FAIL: %s\n", error.what());
        return 1;
    }
}
