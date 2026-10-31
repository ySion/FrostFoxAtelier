#include "math/math.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace atelier::jit_acceptance {
namespace {
using namespace source::detail;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

struct MathAbiHost {
    double precision{16777217.0};
    math::Vector3d position{1.0, 2.0, 3.0};
    math::Vector3d value() const { return position; }
    float single() const { return 1.25f; }
    std::int32_t signed_value() const { return -17; }
    std::uint32_t unsigned_value() const { return 19; }
    float fail() const { throw std::runtime_error("Math ABI native first failure."); }
};

CodeOwner compile(const Binding& binding, std::string_view source, const BusinessServices& callbacks = services) {
    const EnvironmentView storage(binding);
    const std::string type_name(binding.name);
    const auto environment = storage.view(type_name.c_str());
    BusinessCode* output{};
    char diagnostic[1024]{};
    const auto status = atelier_compile_business(&callbacks, "math-abi.source", source.data(),
                                                 static_cast<std::uint32_t>(source.size()), &environment, &output,
                                                 diagnostic, sizeof(diagnostic));
    CodeOwner owner{output, &atelier_destroy_business};
    if (status != 0)
        throw std::runtime_error(diagnostic);
    return owner;
}

void verify_required_services(Runtime& runtime, const Binding& binding) {
    auto callbacks = services;
    callbacks.read_double = nullptr;
    callbacks.write_double = nullptr;
    callbacks.read_value = nullptr;
    callbacks.write_value = nullptr;
    auto retained = compile(binding, "uint run() { return 7; }", callbacks);
    const auto scalar_result = take(execute_typed(runtime, binding, retained.get(), {}, {}));
    require(scalar_result.kind == ValueKind::u32 && scalar_result.bits == 7,
            "Unused double/math services prevented a scalar-only source module from executing.");

    const EnvironmentView storage(binding);
    const std::string type_name(binding.name);
    const auto environment = storage.view(type_name.c_str());
    const std::array<std::string_view, 4> sources{
        "double run() { return self.precision; }", "void run() { self.precision = 3.0d; }",
        "vector3d run() { return self.position; }", "void run() { self.position = vector3d(4.0d, 5.0d, 6.0d); }"};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        callbacks = services;
        switch (index) {
        case 0:
            callbacks.read_double = nullptr;
            break;
        case 1:
            callbacks.write_double = nullptr;
            break;
        case 2:
            callbacks.read_value = nullptr;
            break;
        case 3:
            callbacks.write_value = nullptr;
            break;
        default:
            std::unreachable();
        }
        auto* candidate = retained.get();
        char diagnostic[1024]{};
        const auto status =
            atelier_compile_business(&callbacks, "math-abi-missing-service.source", sources[index].data(),
                                     static_cast<std::uint32_t>(sources[index].size()), &environment, &candidate,
                                     diagnostic, sizeof(diagnostic));
        CodeOwner unexpected{candidate != retained.get() ? candidate : nullptr, &atelier_destroy_business};
        require(status != 0 && candidate == retained.get() && diagnostic[0] != '\0',
                "Source using a missing double/math field service compiled or replaced retained code.");
    }
}

enum class Fault { pointer, high_bits, native_failure };
struct Observation {
    Fault fault;
    std::uint32_t calls{}, finishes{};
    math::Vector3d replacement{91.0, 92.0, 93.0};
    InvocationObserver observer() {
        return {this, nullptr, nullptr,
                [](void* context, Invocation&) -> Result<void> {
                    ++static_cast<Observation*>(context)->finishes;
                    return {};
                },
                nullptr};
    }
};

BusinessServices corrupted_services() {
    auto callbacks = services;
    callbacks.call = [](void* context, std::uint32_t slot, std::uint32_t method, const BusinessValue* arguments,
                        std::uint32_t count, BusinessValue* output) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        auto& observation = *static_cast<Observation*>(invocation.observer.context);
        ++observation.calls;
        const auto status = call_method(context, slot, method, arguments, count, output);
        if (observation.fault == Fault::native_failure && status != 0) {
            invocation.fail(ErrorCode::invalid_storage, "Secondary math ABI failure.");
            if (output)
                output->bits |= std::uint64_t{1} << 48;
            return 9;
        }
        if (status != 0 || !output)
            return status;
        if (observation.fault == Fault::pointer)
            output->data = &observation.replacement;
        else if (observation.fault == Fault::high_bits)
            output->bits |= std::uint64_t{1} << 48;
        return 0;
    };
    return callbacks;
}

std::string source_call(std::string_view result_type, std::string_view method) {
    return std::string(result_type) + R"( run() {
        acceptance.MathAbiHost held = new acceptance.MathAbiHost();
        weak acceptance.MathAbiHost observed = observe(held);
        return self.)" +
           std::string(method) + "(); }";
}

bool same_operand(const BusinessValue& a, const BusinessValue& b) {
    return a.kind == b.kind && a.bits == b.bits && a.storage == b.storage && a.data == b.data && a.slot == b.slot &&
           a.field == b.field;
}

void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({64}));
}

void verify_result_rejection(Runtime& runtime, const Binding& binding, BusinessCode* code, ObjectId receiver,
                             Fault fault) {
    const auto signature = atelier_business_info(code).signature;
    math::Vector3d destination{-5.0, -6.0, -7.0};
    const auto original = destination;
    const BusinessValue sentinel{aggregate(signature.result) ? signature.result : BusinessType{ValueKind::u32},
                                 0xcafebabeu, ArgumentStorage::value,
                                 aggregate(signature.result) ? &destination : nullptr};
    auto output = sentinel;
    {
        Observation observation{fault};
        auto activation = take(ExecutionScope::enter(runtime));
        Invocation invocation(runtime, binding, atelier_business_layout(code), observation.observer(), nullptr, {},
                              &activation);
        take(invocation.frame.hold(receiver_slot, receiver));
        char diagnostic[512]{};
        const auto status =
            atelier_invoke_business(code, &invocation, nullptr, 0, &output, diagnostic, sizeof(diagnostic));
        require(status != 0 && observation.calls == 1 && observation.finishes == 0,
                "Corrupted math/scalar native result reached the successful source finish path.");
        require(invocation.resources_cleared() && runtime.root_count() == 1,
                "Math ABI result rejection relied on RAII cleanup or retained a strong/weak slot.");
        require(same_operand(output, sentinel) && destination == original,
                "Rejected math/scalar native result modified caller result storage.");
        if (fault == Fault::native_failure)
            require(status == 9 && invocation.failed &&
                        std::string_view(invocation.error) == "Math ABI native first failure." && diagnostic[0] == '\0',
                    "Native failure lost its first cause or was replaced by a result-contract error.");
        else
            require(!invocation.failed && std::string_view(diagnostic).find("Native call result contract violation") !=
                                              std::string_view::npos,
                    "Invalid native result lost its backend contract-violation diagnostic.");
    }
    collect(runtime);
    require(runtime.live_objects() == 1 && runtime.root_count() == 1,
            "Math ABI rejection retained its source-created temporary object.");

    if (fault == Fault::native_failure) {
        Observation observation{fault};
        auto activation = take(ExecutionScope::enter(runtime));
        const auto failed = execute_admitted(activation, runtime, binding, code, atelier_business_layout(code), true,
                                             receiver, {}, observation.observer());
        require(!failed && failed.error().message == "Math ABI native first failure." && observation.calls == 1 &&
                    observation.finishes == 0,
                "Source bridge replaced the native math ABI first failure.");
    }
    collect(runtime);
    require(runtime.live_objects() == 1 && runtime.root_count() == 1,
            "Source bridge first-error propagation retained its temporary object.");
}
}

void verify_math_abi() {
    Runtime runtime;
    const auto type = take(NativeRegistration<MathAbiHost>{runtime, "acceptance.MathAbiHost", TypeKind::object}
                               .field<&MathAbiHost::precision>("precision")
                               .field<&MathAbiHost::position>("position")
                               .method<&MathAbiHost::value>("value", true)
                               .method<&MathAbiHost::single>("single", true)
                               .method<&MathAbiHost::signed_value>("signed_value", true)
                               .method<&MathAbiHost::unsigned_value>("unsigned_value", true)
                               .method<&MathAbiHost::fail>("fail", true)
                               .publish());
    const auto binding = bind_type(runtime, type);
    verify_required_services(runtime, binding);
    auto receiver = take(runtime.create(type));
    const auto callbacks = corrupted_services();
    auto pointer = compile(binding, source_call("vector3d", "value"), callbacks);
    verify_result_rejection(runtime, binding, pointer.get(), receiver.id(), Fault::pointer);
    for (const auto& [result_type, method] : std::array{std::pair{"float", "single"}, std::pair{"int", "signed_value"},
                                                        std::pair{"uint", "unsigned_value"}}) {
        auto scalar = compile(binding, source_call(result_type, method), callbacks);
        verify_result_rejection(runtime, binding, scalar.get(), receiver.id(), Fault::high_bits);
    }
    auto failure = compile(binding, source_call("float", "fail"), callbacks);
    verify_result_rejection(runtime, binding, failure.get(), receiver.id(), Fault::native_failure);
}
}
