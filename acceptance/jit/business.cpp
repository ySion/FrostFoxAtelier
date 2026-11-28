#include "business.hpp"
#include "../helpers.hpp"
#include "math/math.hpp"
#include "runtime/foundation.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"
#include "source/program.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace atelier::jit_acceptance {
void verify_math_abi();

namespace {
using namespace acceptance::helpers;

void publish(source::Program& program, StringView text) {
    auto candidate = take(source::Program::prepare(take(program.request("abi.source", text))));
    take(program.try_publish(candidate));
}

struct Counter {
    float value{13};

    float bump(float amount) { return value += amount; }
};

struct Padding {
    virtual ~Padding() = default;

    std::uint64_t sentinel{0x123456789abcdef0};
};

struct Derived : Padding, Counter {};

enum class Mode : std::int32_t { idle = -3, active = 7 };

void scalar_abi() {
    Runtime runtime;
    take(NativeRegistration<Mode>{runtime, "abi.Mode", TypeKind::enumeration}
             .enumerator("idle", Mode::idle)
             .enumerator("active", Mode::active)
             .publish());
    auto program = take(source::Program::create(runtime));
    publish(*program, R"(
        double run(int signed_value, float narrow, double wide, uint high, bool enabled, int tail, abi.Mode mode) {
            double value = double(signed_value) * 2.0d + double(narrow) * 3.0d + wide * 5.0d
                         + double(high) + double(tail) * 7.0d;
            if (enabled && mode == abi.Mode::active) { value = value + 11.0d; }
            return value;
        })");
    const auto call =
        take(take(program->pin()).bind<double, std::int32_t, float, double, std::uint32_t, bool, std::int32_t, Mode>());
    // Independent arithmetic; the first three values use registers, the rest the stack.
    require(take(call.invoke({}, -17, 1.25f, -2.5, 0xf0000001u, true, 9, Mode::active)) == 4026531872.25,
            "Typed scalar ABI changed register/stack positions or numeric representation.");
}

void boolean_alias() {
    Runtime runtime;
    auto program = take(source::Program::create(runtime));
    publish(*program, "bool run(bool copy, const bool& read, bool& write) { write = copy; return read == write && "
                      "write != !copy; }");
    struct Guarded {
        bool value{};

        bool before{true};

        bool after{true};

        std::uint32_t guard{0xdeadbeef};
    } storage;
    const std::array args{Argument{Scalar{true}}, Argument::borrow(std::as_const(storage.value)),
                          Argument::borrow(storage.value)};
    const auto result = take(program->entry().invoke({}, args));
    require(result && std::get<bool>(*result) && storage.value && storage.before && storage.after &&
                storage.guard == 0xdeadbeef,
            "Bool ABI lost its borrowed alias or overwrote adjacent bytes.");
    const auto typed = take(program->entry().bind<bool, bool, const bool&, bool&>());
    require(take(typed.invoke({}, false, std::as_const(storage.value), storage.value)) && !storage.value &&
                storage.before && storage.after && storage.guard == 0xdeadbeef,
            "Typed reference ABI lost its borrowed alias or overwrote adjacent bytes.");
}

void reference_coroutine() {
    Runtime runtime;
    const auto base = take(
        NativeRegistration<Counter>{runtime, "abi.Counter", TypeKind::object}.method<&Counter::bump>("bump").publish());
    const auto derived =
        take(NativeRegistration<Derived>{runtime, "abi.Derived", TypeKind::object}.base<Counter>().publish());
    auto object = take(runtime.create(derived));
    const auto id = object.id();
    auto program = take(source::Program::create(runtime, base));
    publish(*program, R"(
        async float run(float amount) {
            abi.Counter held = self;
            weak abi.Counter watched = observe(held);
            yield;
            abi.Counter acquired = try_acquire(watched);
            if (acquired != self) { return 0.0; }
            return acquired.bump(amount);
        })");
    const std::array start_args{Scalar{2.f}};
    auto task = take(program->entry().start(id, start_args));
    require(take(task.resume()) == CoroutineState::suspended, "Reference coroutine did not suspend.");
    object.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(take(task.resume()) == CoroutineState::completed,
            "Suspended strong/weak references or adjusted receiver did not complete.");
    const auto task_result = take(task.result());
    require(task_result && std::get<float>(*task_result) == 15,
            "Suspended strong/weak references or adjusted receiver lost identity or data.");
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({16}));
    require(runtime.live_objects() == 0 && runtime.root_count() == 0, "Completed frame retained its receiver.");
}

void type_bound_stateless() {
    struct Unrelated {};
    Runtime runtime;
    const auto base = take(NativeRegistration<Counter>{runtime, "abi.Counter", TypeKind::object}.publish());
    const auto derived =
        take(NativeRegistration<Derived>{runtime, "abi.Derived", TypeKind::object}.base<Counter>().publish());
    const auto other_type = take(NativeRegistration<Unrelated>{runtime, "abi.Unrelated", TypeKind::object}.publish());
    auto object = take(runtime.create(derived));
    auto wrong = take(runtime.create(other_type));
    const auto object_id = object.id();
    auto program = take(source::Program::create(runtime, base));
    publish(*program, "int run(int x) { return x * 3; }");
    require(source::detail::ProgramAccess::stateless_direct(take(program->pin())),
            "A bound scalar entry without self must select stateless Typed dispatch.");
    const std::array invoke_args{Scalar{7}};
    const auto invoke_result = take(program->entry().invoke(object_id, invoke_args));
    require(invoke_result && std::get<std::int32_t>(*invoke_result) == 21,
            "Generic dispatch failed to finish with an unused optional receiver.");
    const auto typed = take(program->entry().bind<std::int32_t, std::int32_t>());
    require(take(typed.invoke(object_id, 7)) == 21 && take(typed.invoke({}, 7)) == 21,
            "Stateless Typed::invoke on a type-bound Program rejected a valid derived or empty receiver.");
    const auto mismatch = typed.invoke(wrong.id(), 7);
    require(!mismatch && mismatch.error().code == ErrorCode::type_mismatch,
            "Stateless Typed::invoke accepted an unrelated receiver type.");
    take(runtime.destroy(object_id));
    const auto destroyed = typed.invoke(object_id, 7);
    require(!destroyed && destroyed.error().code == ErrorCode::invalid_object,
            "Stateless Typed::invoke accepted a destroyed receiver.");
}

void weak_acquisition_during_mark() {
    Runtime runtime;
    const auto type = take(runtime.register_type({"abi.Observed"_s, TypeKind::object, {}, {}, {}}));
    auto program = take(source::Program::create(runtime, type));
    publish(*program, "bool run() { weak abi.Observed w = observe(self); return try_acquire(w) == self; }");
    const auto typed = take(program->entry().bind<bool>());
    for (const bool direct : {false, true}) {
        auto owner = take(runtime.create(type));
        const auto id = owner.id();
        const auto weak = take(runtime.observe(owner.reference()));
        owner.reset();
        take(runtime.begin_collection());
        while (runtime.collection_phase() == CollectionPhase::roots)
            take(runtime.collect_step({1}));
        bool acquired = false;
        if (direct)
            acquired = take(typed.invoke(id));
        else {
            const auto generic_res = take(program->entry().invoke(id, Span<const Scalar>{}));
            acquired = generic_res && std::get<bool>(*generic_res);
        }
        while (runtime.collection_phase() != CollectionPhase::idle)
            take(runtime.collect_step({1}));
        require(acquired && take(runtime.try_acquire(weak)).has_value(),
                "A successful synchronous weak acquisition must mark the object for the current cycle.");
        take(runtime.begin_collection());
        while (runtime.collection_phase() != CollectionPhase::idle)
            take(runtime.collect_step({1}));
        require(!take(runtime.try_acquire(weak)), "Synchronous weak acquisition retained a root after return.");
    }
}

void sequence_stdlib_execution() {
    Runtime runtime;
    Array<TypeDefinition> defs;
    defs.add(string_definition(runtime));
    defs.add(array_definition<std::int32_t>(runtime, "abi.IntArray"));
    defs.add(array_definition<std::uint32_t>(runtime, "abi.UintArray"));
    defs.add(array_definition<float>(runtime, "abi.FloatArray"));
    defs.add(array_definition<bool>(runtime, "abi.BoolArray"));
    defs.add(array_definition<String>(runtime, "abi.StringArray"));
    defs.add(array_definition<math::Vector3>(runtime, "abi.Vec3Array"));
    take(runtime.register_types(std::move(defs)));

    // 1. Stateless direct String literal & Array<float> swap/query verification:
    {
        auto program = take(source::Program::create(runtime));
        publish(*program, R"(
            int run(Array<float>& xs) {
                if ("alpha" >= "beta" || !"Hello".equals_ignore_case("hELLo")) { return -1; }
                if (xs.empty()) { return 0; }
                xs.swap(0, int(xs.size()) - 1);
                return xs.find(xs.back());
            })");
        require(source::detail::ProgramAccess::stateless_direct(take(program->pin())),
                "Pure inline Array.swap + view-only String literal comparisons should remain stateless_typed.");
        Array<float> xs{1.0f, 2.0f, 3.0f, 4.0f};
        const auto typed = take(program->entry().bind<std::int32_t, Array<float>&>());
        require(take(typed.invoke({}, xs)) == 3 && xs[0] == 4.0f && xs[3] == 1.0f,
                "Stateless Array.swap or Array.find produced wrong result.");
    }

    // 2. Comprehensive String formatting, parsing, UTF-8 boundary & self-alias safety:
    {
        auto program = take(source::Program::create(runtime));
        auto candidate = take(source::Program::prepare(take(program->request("abi.source", R"(
            uint grow_pool(Array<String>& pool) {
                pool[0] = "reallocated_long_heap_string_payload_0123456789";
                pool.reserve(64);
                pool.add("tail_element_forcing_array_reallocation_0123456789");
                return 1u;
            }
            uint grow_ints(Array<int>& xs) {
                xs.reserve(128);
                return 0u;
            }
            int append_and_ret(Array<int>& xs) {
                xs.add(100);
                return 5;
            }
            bool run(String& out, Array<String>& pool) {
                // Adversarial UAF check: pool[0] view taken before RHS reallocates both pool[0] and pool itself!
                const bool snapshot_ok = (pool[0] == pool[grow_pool(pool)]) == false;
                if (!snapshot_ok) { return false; }

                // Adversarial index-reallocation & .back() compound-assign stability check:
                Array<int> dyn;
                dyn.add(10);
                dyn.add(20);
                if (!"42".try_parse_int(dyn[grow_ints(dyn)]) || dyn[0] != 42) { return false; }
                dyn.back() += append_and_ret(dyn); // must update original back (index 1: 20 -> 25), leaving new back = 100
                if (dyn.size() != 3u || dyn[1] != 25 || dyn.back() != 100) { return false; }

                String s = "  Pt_";
                s.trim_start();
                s.to_lower();
                s.append(-42);
                s.append_ascii(0x3a); // ':'
                s.append_hex(255, 4, true);
                s.append_ascii(0x2f, 2); // "//"
                s.append_float(3.14159, 2);
                s += "_ok  ";
                s.trim_end();
                if (s != "pt_-42:00FF//3.14_ok") { return false; }
                if (!s.is_ascii() || !"火花".starts_with("火") || "火花".is_ascii()) { return false; }
                if (!s.starts_with_ignore_case("PT_") || !s.ends_with_ignore_case("_OK")) { return false; }
                if (s.find("//") != 11 || s.rfind("_") != 17 || s.find("missing") != -1) { return false; }
                if (s.count("0") != 2u || s.common_prefix_length("pt_-99") != 4u) { return false; }
                if (!s.strip_prefix("pt_") || !s.strip_suffix("_ok")) { return false; }
                s.replace_all("//", "|");
                s.replace(0, 3, s); // self-alias replace!
                s.pad_left(int(s.size()) + 2, 0x23); // '#'
                s.pad_right(int(s.size()) + 1);

                int parsed_i = 77;
                uint parsed_u = 88u;
                float parsed_f = 1.0f;
                if (!"-12345".try_parse_int(parsed_i) || parsed_i != -12345) { return false; }
                if ("12x".try_parse_int(parsed_i) || parsed_i != -12345) { return false; }
                if (!"4294967295".try_parse_uint(parsed_u) || parsed_u != 4294967295u) { return false; }
                if ("-1".try_parse_uint(parsed_u) || parsed_u != 4294967295u) { return false; }
                if (!"2.5e1".try_parse_float(parsed_f) || parsed_f != 25.0f) { return false; }
                if ("nan".try_parse_float(parsed_f) || "inf".try_parse_float(parsed_f) || parsed_f != 25.0f) {
                    return false;
                }

                out = s;
                return out.front() == 0x23u && out.back() == 0x20u;
            })",
                                                                             "run"))));
        take(program->try_publish(candidate));
        String out;
        Array<String> pool{String::from_utf8_validated("seed"), String::from_utf8_validated("other")};
        const std::array args{Argument::borrow(out), Argument::borrow(pool)};
        const auto res = take(program->entry().invoke({}, args));
        require(res && std::get<bool>(*res), "String stdlib execution returned false.");
        require(out == "##-42:00FF|3.14:00FF|3.14 ", "String stdlib produced unexpected text.");
    }

    // 3. UTF-8 mid-codepoint rejection & NaN Array ordering failure checks:
    {
        auto p1 = take(source::Program::create(runtime));
        publish(*p1, "void run(String& s) { s.slice_in_place(1, 2); }");
        String utf8 = String::from_utf8_validated("a火b");
        const std::array args{Argument::borrow(utf8)};
        const auto bad_utf8 = p1->entry().invoke({}, args);
        require(!bad_utf8 && StringView{bad_utf8.error().message}.contains("UTF-8") && utf8 == "a火b",
                "Mid-codepoint slice_in_place must fail without corrupting UTF-8 String.");

        auto p2 = take(source::Program::create(runtime));
        publish(*p2, "bool run(const Array<float>& xs) { return xs.is_sorted(); }");
        Array<float> nan_arr{1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f};
        const std::array nan_args{Argument::borrow(std::as_const(nan_arr))};
        const auto nan_sorted = p2->entry().invoke({}, nan_args);
        require(!nan_sorted && StringView{nan_sorted.error().message}.contains("non-NaN"),
                "Array<float>.is_sorted() with NaN must report floating_point_order failure.");

        auto p3 = take(source::Program::create(runtime));
        publish(*p3, "void run(Array<float>& xs) { xs.sort(); }");
        const std::array mut_nan_args{Argument::borrow(nan_arr)};
        const auto nan_sort = p3->entry().invoke({}, mut_nan_args);
        require(!nan_sort && StringView{nan_sort.error().message}.contains("non-NaN"),
                "Array<float>.sort() with NaN must report argument_mismatch failure.");

        // Host-entry aliasing: mutable String& a and const String& b borrowing the same String across reallocation:
        auto p4 = take(source::Program::create(runtime));
        publish(*p4, R"(
            uint run(String& a, const String& b) {
                a.append("_reallocated_long_heap_suffix_0123456789_abcdef");
                return b.size();
            })");
        String aliased = String::from_utf8_validated("short");
        const std::array alias_args{Argument::borrow(aliased), Argument::borrow(std::as_const(aliased))};
        const auto alias_res = take(p4->entry().invoke({}, alias_args));
        require(alias_res && std::get<std::uint32_t>(*alias_res) == aliased.size(),
                "Host-entry String& / const String& aliasing cached stale sequence view across reallocation.");
    }
}
}

void verify_business_execution() {
    scalar_abi();
    boolean_alias();
    reference_coroutine();
    type_bound_stateless();
    weak_acquisition_during_mark();
    sequence_stdlib_execution();
    verify_math_abi();
}
}
