#include "runtime/foundation.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace atelier;
using source::Program;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void take(Result<void> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
}
void publish(Program& program, std::string_view text, std::string_view entry = "run") {
    auto candidate = take(Program::prepare(take(program.request("foundations.source", text, entry))));
    take(program.try_publish(candidate));
}
template <class T>
T result(Result<std::optional<Scalar>> value) {
    return std::get<T>(*take(std::move(value)));
}
struct Work {
    static inline std::uint32_t live{}, destroyed{};
    String label;
    Work() { ++live; }
    Work(const Work& other) : label(other.label) { ++live; }
    Work& operator=(const Work&) = default;
    ~Work() noexcept {
        --live;
        ++destroyed;
    }
};
static_assert(std::is_standard_layout_v<Work>);
struct Host {
    bool enabled{};
    bool before{true};
    bool after{true};
    std::uint32_t guard{0xa55aa55a}, calls{};
    bool alias(bool copy, const bool& read, bool& write) {
        write = copy;
        return read;
    }
    bool touch(bool value) {
        ++calls;
        return value;
    }
    bool text(const String& value) {
        ++calls;
        return value.size() != 0;
    }
    bool fail() {
        ++calls;
        throw std::runtime_error("foundation first failure");
    }
    std::uint32_t live_count() const noexcept { return Work::live; }
    std::uint32_t get_next() noexcept { return ++calls; }
    std::uint32_t get_guard() const noexcept { return guard; }
    void set_guard(std::uint32_t value) {
        if (value > 100)
            throw std::runtime_error("guard rejected");
        guard = value;
    }
};
TypeId register_foundations(Runtime& runtime) {
    auto work = NativeRegistration<Work>{runtime, "foundation.Work", TypeKind::record}
                    .field<&Work::label>("label", {TypeReference::native<String>()}, offsetof(Work, label))
                    .build();
    work.native.value_operations_preserve_borrows = true;
    const auto types = take(runtime.register_types(
        {NativeRegistration<Host>{runtime, "foundation.Host", TypeKind::object}
             .field<&Host::enabled>("enabled")
             .field<&Host::before>("before")
             .field<&Host::after>("after")
             .field<&Host::guard>("guard")
             .method<&Host::alias>("alias", true)
             .method<&Host::touch>("touch", true)
             .method<&Host::text>("text", true)
             .method<&Host::fail>("fail", true)
             .method<&Host::get_next>("GetNext", true)
             .method<&Host::get_guard>("GetGuard", true)
             .method<&Host::set_guard>("SetGuard", true)
             .method<&Host::get_guard>("GetUnsafe")
             .method<&Host::live_count>("live_count", true)
             .build(),
         string_definition(runtime), std::move(work), array_definition<Work>(runtime, "foundation.Works"),
         array_definition<bool>(runtime, "foundation.Bools")}));
    return types[0];
}
const FieldInfo& field(const TypeInfo& type, std::string_view name) {
    for (const auto& value : type.fields)
        if (value.name == name)
            return value;
    throw std::runtime_error("Missing foundation source field.");
}
void collect(Runtime& runtime) {
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({4096}));
    take(runtime.begin_collection());
    while (take(runtime.collect_step({4096})).phase != CollectionPhase::idle) {
    }
}

void verify_execution_interval() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    auto temporary = take(runtime.create(host_type));
    const auto identity = temporary.id();
    temporary.reset();
    Program program(runtime);
    publish(program, R"(
        uint run(uint count) {
            uint sum = 0;
            for (uint i = 0; i < count; i = i + 1) { sum = sum + i; }
            return sum;
        }
    )");
    take(runtime.begin_collection());
    const auto phase = runtime.collection_phase();
    require(result<std::uint32_t>(program.entry().invoke({}, std::array{Scalar{std::uint32_t{256}}})) == 32640,
            "Protected synchronous execution returned an incorrect result.");
    require(runtime.collection_phase() == phase && runtime.type_of(identity),
            "GC advanced or invalidated an object inside synchronous execution.");
    collect(runtime);
    require(!runtime.type_of(identity), "Host could not collect after execution returned.");

    Program allocations(runtime, host_type);
    publish(allocations, R"(
        uint run(uint count) {
            uint total = 0;
            for (uint i = 0; i < count; i = i + 1) {
                weak foundation.Host temporary = observe(new foundation.Host());
                temporary.guard = i;
                total = total + temporary.guard;
            }
            return total;
        }
    )");
    require(result<std::uint32_t>(allocations.entry().invoke({}, std::array{Scalar{1024u}})) == 523776 &&
                runtime.live_objects() == 1024 && runtime.root_count() == 0,
            "A synchronous allocation loop collected implicitly or retained per-local roots.");
    collect(runtime);
    require(runtime.live_objects() == 0, "Allocation-loop garbage survived host collection.");
    for (unsigned batch = 0; batch < 16; ++batch) {
        require(result<std::uint32_t>(allocations.entry().invoke({}, std::array{Scalar{64u}})) == 2016 &&
                    runtime.live_objects() == 64 && runtime.root_count() == 0,
                "Batched execution retained objects or changed weak access semantics.");
        collect(runtime);
        require(runtime.live_objects() == 0, "Host collection left a backlog between batches.");
    }
    std::puts("Source execution interval: PASS (1024 temporary objects in one call; 64 per host-collected batch)");
}

void verify_properties_and_weakening() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    Program program(runtime, host_type);
    const auto object = take(runtime.create(host_type));
    publish(program, R"(
        uint run() {
            foundation.Host held = self;
            weak foundation.Host watched = held;
            watched = null;
            watched = held;
            watched.Guard = watched.Next * 10 + watched.Next;
            return watched.Guard + watched.Next * 100;
        }
    )");
    require(result<std::uint32_t>(program.entry().invoke(object.id(), {})) == 312,
            "Implicit weakening or native property evaluation order changed the result.");
    publish(program, R"(
        uint GetValue() { return self.Next; }
        void SetValue(uint value) { self.Guard = value; }
        void SetSink(uint value) { self.SetGuard(value); }
        uint run() {
            (Value) = Value * 10 + Value;
            Sink = self.Guard + GetValue();
            return self.GetGuard();
        }
    )");
    require(result<std::uint32_t>(program.entry().invoke(object.id(), {})) == 51,
            "Source property assignment read its getter, repeated an operand, or lost explicit accessor calls.");
    publish(program, "uint run() { self.Guard = 101; return 0; }");
    const auto rejected_write = program.entry().invoke(object.id(), {});
    require(!rejected_write && rejected_write.error().message.find("guard rejected") != std::string::npos &&
                take(runtime.with_native<Host>(object.id(),
                                               [](const Host& host) -> Result<bool> { return host.guard == 51; })),
            "Property assignment bypassed its native setter or swallowed rejection.");
    publish(program, "uint run() { weak foundation.Host absent; return self.Guard + absent.Guard; }");
    const auto null_read = program.entry().invoke(object.id(), {});
    require(!null_read && null_read.error().code == ErrorCode::invalid_object,
            "An ordinary property getter bypassed null-receiver validation.");
    Program values(runtime, host_type);
    publish(values, R"(
        struct foundation.Pair { uint x; uint y; };
        foundation.Pair GetPair() {
            foundation.Pair p;
            p.x = self.Guard; p.y = 9;
            return p;
        }
        void SetPair(const foundation.Pair& p) { self.Guard = p.x + p.y; }
        uint run() { Pair = Pair; return Pair.x + Pair.y; }
    )");
    require(result<std::uint32_t>(values.entry().invoke(object.id(), {})) == 69,
            "Plain-record property results, member reads or const-reference setters lost their value lifetime.");
    Program coroutine(runtime, host_type);
    publish(coroutine, R"(
        async uint run() {
            weak foundation.Host watched = new foundation.Host();
            yield;
            foundation.Host held = try_acquire(watched);
            if (held == null) { return 7; }
            return 0;
        }
    )");
    auto task = take(coroutine.entry().start({}, {}));
    require(take(task.resume()) == CoroutineState::suspended, "Implicit weakening coroutine did not suspend.");
    collect(runtime);
    require(take(task.resume()) == CoroutineState::completed && result<std::uint32_t>(task.result()) == 7,
            "Implicit strong-to-weak conversion retained the temporary across suspension.");
    constexpr std::array invalid{"void run() { weak foundation.Host w = self; foundation.Host h = w; }",
                                 "void run() { self.Next = 1; }",
                                 "void SetSink(uint v) {} uint run() { return Sink; }",
                                 "uint GetX() { return 1; } void SetX(float v) {} uint run() { return 0; }",
                                 "uint GetX() { return 1; } const uint X = 2; uint run() { return X; }",
                                 "uint GetX() { return 1; } uint X() { return 2; } uint run() { return X(); }",
                                 "uint GetX() { return 1; } uint X.child() { return 2; } uint run() { return 0; }",
                                 "uint GetX() { return 1; } uint run(uint X) { return X; }",
                                 "uint GetX() { return 1; } uint run() { uint X = 2; return X; }",
                                 "bool SetX(uint v) { return true; } void run() { X = 2; }",
                                 "void SetX(uint& v) {} void run() { X = 2; }",
                                 "void change(uint& v) { v = 2; } void run() { change(self.Guard); }",
                                 "vector3 GetPoint() { return vector3(); } void run() { Point.x = 1.0; }",
                                 "uint run() { String text = \"active borrow\"; return self.Unsafe; }"};
    for (const auto text : invalid) {
        const auto failed = Program::prepare(take(program.request("invalid-property.source", text, "run")));
        require(!failed && failed.error().message.find("invalid-property.source") != std::string::npos,
                "Invalid property or implicit strengthening was accepted or lost its source location.");
    }
    // A property is not an additional stored field and may not silently hide one.
    Runtime conflicts;
    auto conflicting = NativeRegistration<Host>{conflicts, "foundation.Conflict", TypeKind::object}
                           .field<&Host::guard>("Guard")
                           .method<&Host::get_guard>("GetGuard", true)
                           .build();
    Program collision(conflicts, take(conflicts.register_type(std::move(conflicting))));
    require(!Program::prepare(take(collision.request("collision.source", "uint run() { return self.Guard; }", "run"))),
            "A native property silently shadowed stored object data.");
}

void verify_boolean_storage() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    Program program(runtime, host_type);
    constexpr std::string_view schema =
        "struct foundation.State { bool before; bool enabled; bool after; uint guard; };";
    publish(program, std::string(schema) + R"(
        bool run(foundation.State& state, Array<bool>& flags, bool active) {
            bool local = false;
            if (!forward(active, local, local)) { return false; }
            if (!local) { return false; }
            state.enabled = local;
            flags[0] = false;
            flags[1] = self.alias(active, flags[1], flags[1]);
            flags[2] = !flags[1];
            Array<bool> scratch;
            scratch.resize(3);
            scratch[1] = true;
            scratch.add(!scratch[0]);
            return state.enabled && flags[1] && !flags[0] && !flags[2] &&
                scratch[1] && scratch[3] && !scratch[2] && state.before && state.after;
        }
        bool forward(bool copy, const bool& read, bool& write) {
            write = copy;
            return read;
        }
    )");
    const auto state = take(runtime.find_type("foundation.State"));
    const auto& info = *take(runtime.describe(state));
    require(info.size == 8 && info.alignment == 4 && *field(info, "before").offset == 0 &&
                *field(info, "enabled").offset == 1 && *field(info, "after").offset == 2 &&
                *field(info, "guard").offset == 4,
            "Source bool fields did not use byte storage and native alignment.");
    alignas(4) std::array<std::byte, 8> storage{};
    take(runtime.initialize_value(state, storage));
    const bool yes = true;
    const std::uint32_t guard = 0x12345678;
    std::memcpy(storage.data(), &yes, sizeof(bool));
    std::memcpy(storage.data() + 2, &yes, sizeof(bool));
    std::memcpy(storage.data() + 4, &guard, sizeof(guard));
    Array<bool> flags{true, false, true};
    static_assert(std::same_as<decltype(flags.data()), bool*>);
    auto* data = flags.data();
    const auto object = take(runtime.create(host_type));
    const std::array arguments{Argument::borrow(state, storage.data(), storage.data()), Argument::borrow(flags),
                               Argument{Scalar{true}}};
    require(result<bool>(program.entry().invoke(object.id(), arguments)) && flags.data() == data && !data[0] &&
                data[1] && !data[2] && &flags[2] == data + 2,
            "Source bool parameters, helper aliases or contiguous Array<bool> storage failed.");
    Program object_fields(runtime, host_type);
    publish(object_fields, "bool run(bool active) { self.enabled = active; return self.enabled; }");
    require(result<bool>(take(object_fields.bind<bool, bool>()).invoke(object.id(), std::array{Scalar{true}})),
            "Source bool object field read/write lost its scalar value.");
    std::uint32_t stored_guard{};
    std::memcpy(&stored_guard, storage.data() + 4, sizeof(stored_guard));
    require(storage[0] == std::byte{1} && storage[1] == std::byte{1} && storage[2] == std::byte{1} &&
                stored_guard == guard &&
                take(runtime.with_native<Host>(object.id(),
                                               [](const Host& host) -> Result<bool> {
                                                   return host.enabled && host.before && host.after &&
                                                          host.guard == 0xa55aa55a;
                                               })),
            "A bool field store overwrote adjacent bools or a numeric guard.");
    auto readonly = arguments;
    readonly[1] = Argument::borrow(std::as_const(flags));
    require(!program.entry().invoke(object.id(), readonly), "Mutable bool array accepted readonly host storage.");
    const auto old = take(program.pin());
    publish(program, std::string(schema) +
                         "bool run(foundation.State& state, Array<bool>& flags, bool active) { return !active; }");
    require(!result<bool>(program.entry().invoke(object.id(), arguments)) &&
                result<bool>(old.invoke(object.id(), arguments)),
            "Bool code replacement or old module lease changed its contract.");
    Program aliases(runtime, {});
    publish(
        aliases,
        "bool run(bool copy, const bool& read, bool& write) { write = copy; return read == write && write != !copy; }");
    const auto alias_entry = take(aliases.bind<bool, bool, const bool&, bool&>());
    struct Guarded {
        bool value{};
        bool before{true};
        bool after{true};
        std::uint32_t guard{0xdeadbeef};
    } guarded;
    const std::array alias_arguments{Argument{Scalar{true}}, Argument::borrow(std::as_const(guarded.value)),
                                     Argument::borrow(guarded.value)};
    require(result<bool>(alias_entry.invoke({}, alias_arguments)) && guarded.value && guarded.before && guarded.after &&
                guarded.guard == 0xdeadbeef,
            "Public source bool value/const-reference/reference ABI lost aliases or overwrote adjacent storage.");
    auto wrong_arguments = alias_arguments;
    wrong_arguments[0] = Argument{Scalar{std::uint32_t{1}}};
    require(!alias_entry.invoke({}, wrong_arguments), "Bool source parameter accepted an integer scalar.");
    take(runtime.destroy_value(state, storage));
}

void verify_flags_and_scopes() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    Program program(runtime, host_type);
    publish(program, R"(
        enum foundation.Mode : uint { Active = 7 }
        const uint MASK = LATER | 1;
        const uint LATER = uint(2);
        const foundation.Mode MODE = foundation.Mode::Active;
        const bool READY = true || (1 / 0 == 0);
        uint run(bool fail) {
            const uint limit = 6;
            uint flags = 0;
            uint visits = 0;
            for (uint i = 0; i < limit; i = i + 1) {
                Array<foundation.Work> outer;
                outer.resize(2);
                outer[0].label = "outer/heap/value/for/cleanup";
                if (self.live_count() != 2) { return 901; }
                if (i == 1) { continue; }
                for (uint j = 0; j < 4; j = j + 1) {
                    foundation.Work inner;
                    inner.label = "inner/heap/value/for/cleanup";
                    if (self.live_count() != 3) { return 902; }
                    if (j == 1) { continue; }
                    if (j == 3) { break; }
                    visits = visits + 1;
                    flags = flags | (1 << i);
                }
                if (self.live_count() != 2) { return 903; }
                if (fail && i == 2) { self.fail(); }
                if (i == 4) { break; }
            }
            if (self.live_count() != 0 || visits != 8 || !READY || MODE != foundation.Mode::Active) { return 904; }
            for (;;) { flags = flags ^ MASK; break; }
            uint countdown = 2;
            for (; countdown > 0;) { countdown = countdown - 1; }
            const int signed_value = -17;
            if (signed_value / 4 != -4 || signed_value % 4 != -1 || (signed_value >> 2) != -5) { return 905; }
            if ((signed_value & 3) != 3 || (signed_value ^ -1) != 16 || (~signed_value) != 16) { return 906; }
            if ((signed_value | 3) != -17 || (uint(8) >> int(2)) != 2) { return 907; }
            if ((uint(4294967295) + 1) != 0 || (uint(0) - 1) != 4294967295 || (uint(2147483648) * 2) != 0) { return 908; }
            const int minimum = -2147483648;
            if (minimum - 1 != 2147483647 || minimum + minimum != 0 || minimum * 2 != 0) { return 912; }
            if (int(uint(17)) != 17 || uint(int(17)) != 17 || int(float(-3.75)) != -3) { return 909; }
            if (uint(double(-0.5)) != 0 || int(-2147483648.5d) != -2147483648) { return 910; }
            if (double(float(uint(8))) != 8.0d || float(double(int(-8))) != -8.0) { return 911; }
            return ((flags & 31) + visits * 4) / 2 + 11 % 4;
        }
    )");
    const auto object = take(runtime.create(host_type));
    const auto entry = take(program.bind<std::uint32_t, bool>());
    const std::array arguments{Scalar{false}};
    require(result<std::uint32_t>(entry.invoke(object.id(), arguments)) == 34 && Work::live == 0,
            "Flags, numeric conversion, constants or nested loop scope cleanup failed.");
    const auto destroyed = Work::destroyed;
    const auto failure = entry.invoke(object.id(), std::array{Scalar{true}});
    require(!failure && failure.error().message.find("foundation first failure") != std::string::npos &&
                Work::live == 0 && Work::destroyed > destroyed &&
                result<std::uint32_t>(entry.invoke(object.id(), arguments)) == 34,
            "Loop failure lost the first cause, leaked owned values or blocked subsequent execution.");
    const auto old = take(program.pin());
    publish(
        program,
        "enum foundation.Mode : uint { Active = 7 } const uint RESULT = 35; uint run(bool fail) { return RESULT; }");
    require(result<std::uint32_t>(entry.invoke(object.id(), arguments)) == 35 &&
                result<std::uint32_t>(old.invoke(object.id(), arguments)) == 34 && Work::live == 0,
            "Top-level constants or loop cleanup lost the pinned module's code environment.");
}

void verify_short_circuit() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    Program owned(runtime, host_type), roots(runtime, host_type);
    publish(owned, R"(
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
    publish(roots, R"(
        bool run(bool fail) {
            foundation.Host held = self;
            if (false && held.fail()) { return false; }
            if (fail) { return held.fail() || held.touch(true); }
            return held.touch(true) && (true || held.fail());
        }
    )");
    const auto object = take(runtime.create(host_type));
    const auto baseline = runtime.root_count();
    const auto calls = [&] {
        return take(runtime.with_native<Host>(object.id(),
                                              [](const Host& host) -> Result<std::uint32_t> { return host.calls; }));
    };
    require(result<bool>(owned.entry().invoke(object.id(), std::array{Scalar{false}})) && calls() == 2 &&
                Work::live == 0 && runtime.root_count() == baseline,
            "Short circuit ran skipped RHSs or leaked helper/temporary owned values.");
    auto failure = owned.entry().invoke(object.id(), std::array{Scalar{true}});
    require(!failure && failure.error().message.find("foundation first failure") != std::string::npos && calls() == 5 &&
                Work::live == 0 && runtime.root_count() == baseline,
            "A failing logical LHS ran its RHS, replaced its first cause or leaked scoped storage.");
    require(result<bool>(roots.entry().invoke(object.id(), std::array{Scalar{false}})) && calls() == 6 &&
                runtime.root_count() == baseline,
            "Logical expressions leaked rooted reference locals.");
    failure = roots.entry().invoke(object.id(), std::array{Scalar{true}});
    require(!failure && calls() == 7 && runtime.root_count() == baseline &&
                result<bool>(owned.entry().invoke(object.id(), std::array{Scalar{false}})) && calls() == 9,
            "Root cleanup after logical failure did not restore execution admission.");
}

void verify_numeric_failures() {
    Runtime runtime;
    Program integers(runtime, {});
    constexpr std::array invalid_operations{"a / b", "a % b", "a << b", "a >> b"};
    for (const auto operation : invalid_operations) {
        publish(integers, "int run(int a, int b) { return " + std::string(operation) + "; }");
        const bool shift = std::string_view(operation).find('<') != std::string_view::npos ||
                           std::string_view(operation).find('>') != std::string_view::npos;
        const std::array bad =
            shift ? std::array{std::array<std::int32_t, 2>{1, -1}, std::array<std::int32_t, 2>{1, 32}}
                  : std::array{std::array<std::int32_t, 2>{1, 0},
                               std::array<std::int32_t, 2>{std::numeric_limits<std::int32_t>::min(), -1}};
        for (const auto input : bad) {
            const auto failed = integers.entry().invoke({}, std::array{Scalar{input[0]}, Scalar{input[1]}});
            require(!failed && failed.error().code == ErrorCode::invocation_failed,
                    "Invalid integer division/remainder/shift did not fail at execution.");
        }
        require(result<std::int32_t>(
                    integers.entry().invoke({}, std::array{Scalar{std::int32_t{8}}, Scalar{std::int32_t{2}}})) ==
                    (shift ? (std::string_view(operation).find('<') != std::string_view::npos ? 32 : 2)
                           : (std::string_view(operation).find('%') != std::string_view::npos ? 0 : 4)),
                "Arithmetic error left execution admission closed.");
    }
    Program unsigned_operations(runtime, {});
    for (const auto operation : {"/", "%"}) {
        publish(unsigned_operations, "uint run(uint a, uint b) { return a " + std::string(operation) + " b; }");
        require(
            !unsigned_operations.entry().invoke({}, std::array{Scalar{std::uint32_t{8}}, Scalar{std::uint32_t{0}}}) &&
                result<std::uint32_t>(unsigned_operations.entry().invoke(
                    {}, std::array{Scalar{std::uint32_t{9}}, Scalar{std::uint32_t{2}}})) ==
                    (std::string_view(operation) == "/" ? 4u : 1u),
            "Unsigned division/remainder failure or recovery changed its semantics.");
    }
    Program shifts(runtime, {});
    publish(shifts, "uint run(uint value, uint count) { return (value << count) >> count; }");
    require(!shifts.entry().invoke({}, std::array{Scalar{std::uint32_t{1}}, Scalar{std::uint32_t{32}}}) &&
                !shifts.entry().invoke({}, std::array{Scalar{std::uint32_t{1}}, Scalar{std::uint32_t{4294967295u}}}) &&
                result<std::uint32_t>(
                    shifts.entry().invoke({}, std::array{Scalar{std::uint32_t{1}}, Scalar{std::uint32_t{31}}})) == 1,
            "Unsigned shift counts escaped 0..31 validation or right shift was not logical.");
    Program conversion(runtime, {});
    publish(conversion, "vector3d run(double input) { return vector3d(double(int(input)), 2.0d, 3.0d); }");
    const auto type = take(runtime.native_type<math::Vector3d>());
    for (const double value : {2147483648.0, -2147483649.0, std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        math::Vector3d output{91.0, 92.0, 93.0};
        const auto failed = conversion.entry().invoke({}, std::array{Scalar{value}}, ValueOutput{type, &output});
        require(!failed && output == math::Vector3d{91.0, 92.0, 93.0},
                "Checked integer conversion succeeded out of range or overwrote failed output.");
    }
    math::Vector3d output;
    take(conversion.entry().invoke({}, std::array{Scalar{-2147483648.5}}, ValueOutput{type, &output}));
    require(output == math::Vector3d{-2147483648.0, 2.0, 3.0},
            "FP conversion checked its range before truncating toward zero.");
    Program unsigned_conversion(runtime, {});
    publish(unsigned_conversion, "uint run(double input) { return uint(input); }");
    for (const double value : {-1.0, 4294967296.0, std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(!unsigned_conversion.entry().invoke({}, std::array{Scalar{value}}),
                "Unsigned conversion admitted a negative, overflow or NaN.");
    require(result<std::uint32_t>(unsigned_conversion.entry().invoke({}, std::array{Scalar{-0.5}})) == 0 &&
                result<std::uint32_t>(unsigned_conversion.entry().invoke({}, std::array{Scalar{4294967295.5}})) ==
                    4294967295u,
            "Unsigned FP boundary conversion did not truncate toward zero.");
    Program narrowing(runtime, {});
    publish(narrowing, "int run(uint input) { return int(input); }");
    require(!narrowing.entry().invoke({}, std::array{Scalar{std::uint32_t{2147483648u}}}),
            "Unsigned-to-signed conversion reinterpreted the sign bit.");
    Program negative(runtime, {});
    publish(negative, "uint run(int input) { return uint(input); }");
    require(!negative.entry().invoke({}, std::array{Scalar{std::int32_t{-1}}}),
            "Signed-to-unsigned conversion wrapped a negative integer.");
    Program float_conversion(runtime, {});
    publish(float_conversion, "int run(float input) { return int(input); }");
    require(!float_conversion.entry().invoke({}, std::array{Scalar{2147483648.0f}}) &&
                !float_conversion.entry().invoke({}, std::array{Scalar{std::numeric_limits<float>::quiet_NaN()}}) &&
                !float_conversion.entry().invoke({}, std::array{Scalar{std::numeric_limits<float>::infinity()}}) &&
                result<std::int32_t>(float_conversion.entry().invoke({}, std::array{Scalar{-3.75f}})) == -3,
            "Float-to-integer execution lost range/finite checks or truncation toward zero.");
}

void verify_async_and_rejections() {
    Runtime runtime;
    const auto host_type = register_foundations(runtime);
    Program program(runtime, host_type);
    publish(program, R"(
        async bool run(bool active) {
            foundation.Host held = self;
            uint total = 0;
            for (uint i = 0; i < 4; i = i + 1) {
                if (i == 1) { continue; }
                for (uint j = 0; j < 3; j = j + 1) {
                    if (j == 1) { break; }
                    yield;
                    total = total + i;
                }
            }
            return active && total == 5 && held.touch(true);
        }
    )");
    auto object = take(runtime.create(host_type));
    const auto id = object.id();
    const auto entry = take(program.bind<bool, bool>(FunctionExecution::coroutine));
    const auto old = take(program.pin());
    auto task = take(entry.start(id, std::array{Scalar{true}}));
    require(take(task.resume()) == CoroutineState::suspended, "Bool for-loop coroutine did not yield.");
    publish(program, "async bool run(bool active) { yield; return !active; }");
    auto previous = take(old.start(id, std::array{Scalar{true}}));
    auto replacement = take(entry.start(id, std::array{Scalar{true}}));
    take(previous.resume());
    take(replacement.resume());
    object.reset();
    collect(runtime);
    require(runtime.type_of(id).has_value(), "Suspended for loop lost its receiver/rooted local.");
    for (auto* current : {&task, &previous}) {
        require(take(current->resume()) == CoroutineState::suspended &&
                    take(current->resume()) == CoroutineState::suspended &&
                    take(current->resume()) == CoroutineState::completed && result<bool>(current->result()),
                "Async nested for/break/continue lost its state, bool result or old code.");
    }
    require(take(replacement.resume()) == CoroutineState::completed && !result<bool>(replacement.result()),
            "New bool coroutine used the retired for-loop code.");
    collect(runtime);
    require(!runtime.type_of(id) && runtime.root_count() == 0, "Completed bool coroutine retained its roots.");
    Program rejected(runtime, host_type);
    constexpr std::array invalid{"bool run() { const bool value = true; value = false; return value; }",
                                 "bool run() { const bool value = true; return self.alias(true, value, value); }",
                                 "const uint VALUE = 1; uint run() { VALUE = 2; return VALUE; }",
                                 "bool run() { break; return true; }",
                                 "bool run() { continue; return true; }",
                                 "bool run() { if (1) { return true; } return false; }",
                                 "bool run() { while (1) {} return false; }",
                                 "bool run() { return true + false; }",
                                 "bool run() { return true < false; }",
                                 "bool run() { return bool(1); }",
                                 "int run() { return int(true); }",
                                 "bool run() { return true && 1; }",
                                 "uint run() { for (uint i = 0; i < 2; i++) {} return 0; }",
                                 "int run(int x) { return --x; }",
                                 "uint run() { for (uint i = 0; i < 2; i = i + 1) {} return i; }",
                                 "const uint A = B; const uint B = A; uint run() { return A; }",
                                 "const uint High = 2147483648; int run() { return -High; }",
                                 "enum Bad { Value } Bad run(Bad input) { return input | input; }"};
    for (const auto* text : invalid) {
        const auto failed = Program::prepare(take(rejected.request("invalid-foundations.source", text, "run")));
        require(!failed && failed.error().message.find("invalid-foundations.source") != std::string::npos,
                "Invalid const/loop/bool/integer code was accepted or lost its source diagnostic.");
    }
}
}

void verify_source_foundations() {
    verify_execution_interval();
    verify_properties_and_weakening();
    verify_boolean_storage();
    verify_flags_and_scopes();
    verify_short_circuit();
    verify_numeric_failures();
    verify_async_and_rejections();
    require(Work::live == 0, "Foundation acceptance retained owned work records.");
    std::puts(
        "Source foundations: PASS (bool ABI/storage, short circuit, checked integers/casts, constants, scoped for/async, properties and implicit weakening)");
}
