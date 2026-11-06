#include "../helpers.hpp"
#include "runtime/foundation.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view text) {
    auto candidate = take(Program::prepare(take(program.request("foundations.source", text, "run"))));
    take(program.try_publish(candidate));
}

template <class T>
T result(Result<std::optional<Scalar>> value) {
    return std::get<T>(*take(std::move(value)));
}

struct Work {
    static inline std::uint32_t live{};

    String label;

    Work() { ++live; }

    Work(const Work& other) : label(other.label) { ++live; }

    Work& operator=(const Work&) = default;

    ~Work() noexcept { --live; }
};

enum class Mode : std::uint32_t { idle = 7 };

struct Host {
    std::uint32_t guard{};

    std::uint32_t calls{};

    Mode mode{Mode::idle};

    String last_text;

    bool touch(bool value) {
        ++calls;
        return value;
    }

    Result<bool> text(const String& value) noexcept {
        ++calls;
        last_text = value;
        return value.size() != 0;
    }

    Result<bool> fail() noexcept {
        ++calls;
        return std::unexpected(Error{ErrorCode::invocation_failed, "foundation first failure"});
    }

    std::uint32_t get_next() noexcept { return ++calls; }

    std::uint32_t get_guard() const noexcept { return guard; }

    Mode get_mode() noexcept {
        ++calls;
        return Mode::idle;
    }

    Result<void> set_guard(std::uint32_t value) noexcept {
        if (value > 100)
            return std::unexpected(Error{ErrorCode::invocation_failed, "guard rejected"});
        guard = value;
        return {};
    }
};

TypeId register_foundations(Runtime& runtime) {
    auto work = NativeRegistration<Work>{runtime, "foundation.Work", TypeKind::record}
                    .field<&Work::label>("label", {TypeReference::native<String>()}, offsetof(Work, label))
                    .build();
    work.native.value_operations_preserve_borrows = true;
    return take(runtime.register_types({NativeRegistration<Host>{runtime, "foundation.Host", TypeKind::object}
                                            .field<&Host::mode>("mode")
                                            .method<&Host::touch>("touch", true)
                                            .method<&Host::text>("text", true)
                                            .method<&Host::fail>("fail", true)
                                            .method<&Host::get_next>("GetNext", true)
                                            .method<&Host::get_guard>("GetGuard", true)
                                            .method<&Host::get_mode>("get_mode", true)
                                            .method<&Host::set_guard>("SetGuard", true)
                                            .build(),
                                        string_definition(runtime), std::move(work),
                                        NativeRegistration<Mode>{runtime, "editor.Mode", TypeKind::enumeration}
                                            .enumerator("idle", Mode::idle)
                                            .build()}))[0];
}

void verify_native_result_projection() {
    Runtime runtime;
    const auto type = register_foundations(runtime);
    const auto object = take(runtime.create(type));
    Scalar mode;
    take(runtime.call(object.id(), take(runtime.find_method(type, "get_mode")), {}, &mode));
    require(std::get<EnumValue>(mode) == EnumValue{take(runtime.native_type<Mode>()), 7},
            "Native-only enum result lost its Runtime identity or value.");
    auto program = take(Program::create(runtime, type));
    publish(*program, "void run() { self.SetGuard(9); }");
    const auto entry = program->entry();
    auto rejected =
        Program::prepare(take(program->request("native-result.source", "void run() { self.get_mode(); }", "run")));
    require(!rejected, "Native-only result was exposed as a void call.");
    take(entry.invoke(object.id(), {}));
    require(take(runtime.with_native<Host>(
                object.id(), [](const Host& host) -> Result<bool> { return host.calls == 1 && host.guard == 9; })),
            "Rejected source executed the native-only method or lost the prior valid entry.");
    publish(*program, "void run() { self.SetGuard(13); }");
    rejected =
        Program::prepare(take(program->request("native-field.source", "void run() { self.mode = self.mode; }", "run")));
    take(entry.invoke(object.id(), {}));
    require(!rejected &&
                take(runtime.with_native<Host>(object.id(),
                                               [](const Host& host) -> Result<bool> {
                                                   return host.calls == 1 && host.guard == 13 &&
                                                          host.mode == Mode::idle;
                                               })) &&
                take(runtime.read(object.id(), take(runtime.find_field(type, "mode")))) == mode,
            "Native-only field projection changed host data or prevented compatible publication.");
    {
        auto literal = take(Program::create(runtime, type));
        publish(*literal, "bool run() { return self.text(\"jit-owned-text/0123456789/after-program-retirement\"); }");
        require(result<bool>(literal->entry().invoke(object.id(), {})), "Native text consumer rejected the literal.");
    }
    require(take(runtime.with_native<Host>(object.id(),
                                           [](const Host& host) -> Result<bool> {
                                               return host.last_text.view() ==
                                                      "jit-owned-text/0123456789/after-program-retirement";
                                           })),
            "String retained storage from a retired JIT constant pool.");
}

void verify_properties_and_weakening() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    auto program = take(Program::create(runtime, host_type));
    const auto object = take(runtime.create(host_type));
    publish(*program, R"(
        uint run() {
            foundation.Host held = self;
            weak foundation.Host watched = held;
            watched.Guard = watched.Next * 10 + watched.Next;
            return watched.Guard + watched.Next * 100;
        }
    )");
    require(result<std::uint32_t>(program->entry().invoke(object.id(), {})) == 312,
            "Native property evaluation order changed the result.");
    publish(*program, R"(
        uint GetValue() { return self.Next; }
        void SetValue(uint value) { self.Guard = value; }
        void SetSink(uint value) { self.SetGuard(value); }
        uint run() {
            (Value) = Value * 10 + Value;
            Sink = self.Guard + GetValue();
            return self.GetGuard();
        }
    )");
    require(result<std::uint32_t>(program->entry().invoke(object.id(), {})) == 51,
            "Source property assignment repeated an operand or lost accessor order.");
    publish(*program, "uint run() { self.Guard = 101; return 0; }");
    require(!program->entry().invoke(object.id(), {}) &&
                take(runtime.with_native<Host>(object.id(),
                                               [](const Host& host) -> Result<bool> { return host.guard == 51; })),
            "Failed property assignment changed its prior value.");
}

void verify_boolean_storage() {
    Runtime runtime;
    auto aliases = take(Program::create(runtime));
    publish(
        *aliases,
        "bool run(bool copy, const bool& read, bool& write) { write = copy; return read == write && write != !copy; }");
    const auto entry = aliases->entry();
    struct Guarded {
        bool value{};

        bool before{true};

        bool after{true};

        std::uint32_t guard{0xdeadbeef};
    } guarded;
    const std::array args{Argument{Scalar{true}}, Argument::borrow(std::as_const(guarded.value)),
                          Argument::borrow(guarded.value)};
    require(result<bool>(entry.invoke({}, args)) && guarded.value && guarded.before && guarded.after &&
                guarded.guard == 0xdeadbeef,
            "Source bool ABI lost aliases or overwrote adjacent storage.");
}

void verify_short_circuit() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    auto owned = take(Program::create(runtime, host_type));
    auto roots = take(Program::create(runtime, host_type));
    publish(*owned, R"(
        bool run(bool fail) {
            foundation.Work local;
            local.label = "short-circuit/owned/heap/string";
            bool value = false && self.fail();
            value = true || self.fail();
            if (!(value && (false || self.text("short-circuit/temporary/heap/string")))) { return false; }
            if (false && ""[0] == 0) { return false; }
            if (true || ""[0] == 0) { value = self.touch(true); }
            if (fail) { return self.fail() || self.text("later/temporary/heap/string"); }
            return value && helper();
        }
        bool helper() {
            foundation.Work temporary;
            temporary.label = "short-circuit/helper/heap/string";
            return true || self.fail();
        }
    )");
    publish(*roots, R"(
        bool run() {
            foundation.Host held = self;
            if (false && held.fail()) { return false; }
            return held.fail() || held.touch(true);
        }
    )");
    const auto object = take(runtime.create(host_type));
    const auto calls = [&] {
        return take(runtime.with_native<Host>(object.id(),
                                              [](const Host& host) -> Result<std::uint32_t> { return host.calls; }));
    };
    const auto owned_call = take(take(owned->pin()).bind<bool, bool>());
    require(!owned_call.invoke(object.id(), true) && calls() == 3 && Work::live == 0 && runtime.root_count() == 1,
            "A failing logical LHS ran its RHS or leaked scoped storage.");
    require(!roots->entry().invoke(object.id(), {}) && calls() == 4 && runtime.root_count() == 1 &&
                take(owned_call.invoke(object.id(), false)) && calls() == 6 && Work::live == 0,
            "Logical failure leaked rooted locals or blocked subsequent execution.");
}

void verify_numeric_failures() {
    Runtime runtime;
    auto integers = take(Program::create(runtime));
    publish(*integers, "int run(int a, int b) { return a / b; }");
    const auto divide = take(take(integers->pin()).bind<std::int32_t, std::int32_t, std::int32_t>());
    require(!divide.invoke({}, 1, 0) && take(divide.invoke({}, -8, 2)) == -4,
            "Arithmetic error left execution admission closed.");
    auto conversion = take(Program::create(runtime));
    publish(*conversion, "vector3d run(double input) { return vector3d(double(int(input)), 2.0d, 3.0d); }");
    const auto type = take(runtime.native_type<math::Vector3d>());
    math::Vector3d output{91.0, 92.0, 93.0};
    require(!conversion->entry().invoke({}, std::array{Scalar{2147483648.0}}, ValueOutput{type, &output}) &&
                output == math::Vector3d{91.0, 92.0, 93.0},
            "Checked integer conversion overwrote failed output.");
    take(conversion->entry().invoke({}, std::array{Scalar{-2147483648.5}}, ValueOutput{type, &output}));
    require(output == math::Vector3d{-2147483648.0, 2.0, 3.0},
            "FP conversion checked its range before truncating toward zero.");
}

void verify_typed_scalar_abi() {
    Runtime runtime;
    auto program = take(Program::create(runtime));
    publish(*program, R"(
        double run(int signed_value, float narrow, double wide, uint high, bool enabled, int tail) {
            double value = double(signed_value) * 2.0d + double(narrow) * 3.0d + wide * 5.0d
                         + double(high) + double(tail) * 7.0d;
            if (enabled) { value = value + 11.0d; }
            return value;
        }
    )");
    const auto call =
        take(take(program->pin()).bind<double, std::int32_t, float, double, std::uint32_t, bool, std::int32_t>());
    // RCX is Invocation; signed/narrow/wide occupy the other register positions.
    // high/enabled/tail and the hidden outputs cross into stack arguments.
    // Independent arithmetic: 4026531841 - 34 + 3.75 - 12.5 + 63 + 11.
    require(take(call.invoke({}, -17, 1.25f, -2.5, 0xf0000001u, true, 9)) == 4026531872.25,
            "Typed scalar ABI changed register/stack positions, signedness or floating-point bits.");
}
}

void verify_source_foundations() {
    verify_native_result_projection();
    verify_properties_and_weakening();
    verify_boolean_storage();
    verify_short_circuit();
    verify_numeric_failures();
    verify_typed_scalar_abi();
    std::puts("Source foundations: PASS (property order, bool ABI, logical cleanup, checked arithmetic recovery)");
}
