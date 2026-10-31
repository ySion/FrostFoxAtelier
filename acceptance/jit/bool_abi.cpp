#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"

#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
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
struct BoolAbiHost {
    bool active{true};
    bool value() const { return active; }
};
CodeOwner compile(const Binding& binding, std::string_view text, const BusinessServices& callbacks = services) {
    const EnvironmentView storage(binding);
    const std::string name(binding.name);
    const auto environment = storage.view(name.c_str());
    BusinessCode* output{};
    char diagnostic[1024]{};
    const auto status =
        atelier_compile_business(&callbacks, "bool-abi.source", text.data(), static_cast<std::uint32_t>(text.size()),
                                 &environment, &output, diagnostic, sizeof(diagnostic), "run");
    CodeOwner owner{output, &atelier_destroy_business};
    if (status != 0)
        throw std::runtime_error(diagnostic);
    return owner;
}
bool same_operand(const BusinessValue& a, const BusinessValue& b) {
    return a.kind == b.kind && a.bits == b.bits && a.storage == b.storage && a.data == b.data && a.slot == b.slot &&
           a.field == b.field;
}
struct Observation {
    std::uint64_t bits{};
    std::uint32_t begins{}, calls{}, finishes{};
    InvocationObserver observer() {
        return {this,
                [](void* context, Invocation&) -> Result<void> {
                    ++static_cast<Observation*>(context)->begins;
                    return {};
                },
                nullptr,
                [](void* context, Invocation&) -> Result<void> {
                    ++static_cast<Observation*>(context)->finishes;
                    return {};
                },
                nullptr};
    }
};
constexpr std::array invalid_bits{std::uint64_t{2}, (std::uint64_t{1} << 32) | 1};

void verify_arguments(Runtime& runtime, const Binding& binding) {
    auto code = compile(binding, "bool run(bool value) { return value; }");
    for (const auto bits : invalid_bits) {
        Observation observation;
        auto activation = take(ExecutionScope::enter(runtime));
        Invocation invocation(runtime, binding, atelier_business_layout(code.get()), observation.observer(), nullptr,
                              {}, &activation);
        const BusinessValue argument{ValueKind::boolean, bits};
        const BusinessValue sentinel{ValueKind::u32, 0xcafebabeu};
        auto output = sentinel;
        char diagnostic[512]{};
        const auto status =
            atelier_invoke_business(code.get(), &invocation, &argument, 1, &output, diagnostic, sizeof(diagnostic));
        require(status != 0 && diagnostic[0] && observation.begins == 0 && observation.finishes == 0 &&
                    same_operand(output, sentinel),
                "Invalid bool argument entered generated code or modified its result.");
    }
}
void verify_results(Runtime& runtime, const Binding& binding, ObjectId receiver) {
    auto callbacks = services;
    callbacks.call = [](void* context, std::uint32_t slot, std::uint32_t method, const BusinessValue* arguments,
                        std::uint32_t count, BusinessValue* output) noexcept -> std::uint32_t {
        auto& invocation = *static_cast<Invocation*>(context);
        auto& observation = *static_cast<Observation*>(invocation.observer.context);
        ++observation.calls;
        const auto status = call_method(context, slot, method, arguments, count, output);
        if (status == 0 && output)
            output->bits = observation.bits;
        return status;
    };
    auto code = compile(binding, "bool run() { return self.value(); }", callbacks);
    for (const auto bits : invalid_bits) {
        Observation observation{bits};
        auto activation = take(ExecutionScope::enter(runtime));
        Invocation invocation(runtime, binding, atelier_business_layout(code.get()), observation.observer(), nullptr,
                              {}, &activation);
        take(invocation.frame.hold(receiver_slot, receiver));
        const BusinessValue sentinel{ValueKind::u32, 0xcafebabeu};
        auto output = sentinel;
        char diagnostic[512]{};
        const auto status =
            atelier_invoke_business(code.get(), &invocation, nullptr, 0, &output, diagnostic, sizeof(diagnostic));
        require(status != 0 && observation.calls == 1 && observation.finishes == 0 && same_operand(output, sentinel),
                "Successful native callback with invalid bool bits committed a source result.");
        require(invocation.resources_cleared() && runtime.root_count() == 1 && !invocation.failed &&
                    std::string_view(diagnostic).find("Native call result contract violation") !=
                        std::string_view::npos,
                "Invalid bool native result lost its backend diagnostic or generated cleanup.");
    }
}
void verify_required_services(Runtime& runtime, const Binding& binding) {
    auto callbacks = services;
    callbacks.read_bool = nullptr;
    callbacks.write_bool = nullptr;
    auto retained = compile(binding, "bool run() { return true; }", callbacks);
    const auto value = take(execute_typed(runtime, binding, retained.get(), {}, {}));
    require(value.kind == ValueKind::boolean && value.bits == 1,
            "Unused bool field services prevented a bool source module from executing.");
    const EnvironmentView storage(binding);
    const std::string name(binding.name);
    const auto environment = storage.view(name.c_str());
    const std::array<std::string_view, 2> sources{"bool run() { return self.active; }",
                                                  "void run() { self.active = false; }"};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        callbacks = services;
        if (index == 0)
            callbacks.read_bool = nullptr;
        else
            callbacks.write_bool = nullptr;
        auto* candidate = retained.get();
        char diagnostic[1024]{};
        const auto status =
            atelier_compile_business(&callbacks, "bool-abi-missing-service.source", sources[index].data(),
                                     static_cast<std::uint32_t>(sources[index].size()), &environment, &candidate,
                                     diagnostic, sizeof(diagnostic));
        CodeOwner unexpected{candidate != retained.get() ? candidate : nullptr, &atelier_destroy_business};
        require(status != 0 && candidate == retained.get() && diagnostic[0],
                "Bool source field access with a missing service compiled or replaced retained code.");
    }
}
class GuardedBool {
public:
    explicit GuardedBool(bool initial) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        allocation_ = static_cast<std::byte*>(
            VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!allocation_)
            throw std::runtime_error("Bool guard page allocation failed.");
        DWORD previous{};
        if (!VirtualProtect(allocation_ + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous)) {
            VirtualFree(allocation_, 0, MEM_RELEASE);
            allocation_ = nullptr;
            throw std::runtime_error("Bool guard page protection failed.");
        }
        value = std::construct_at(reinterpret_cast<bool*>(allocation_ + info.dwPageSize - 1), initial);
    }
    ~GuardedBool() { VirtualFree(allocation_, 0, MEM_RELEASE); }
    GuardedBool(const GuardedBool&) = delete;
    GuardedBool& operator=(const GuardedBool&) = delete;
    bool* value{};

private:
    std::byte* allocation_{};
};
void verify_storage(Runtime& runtime, const Binding& binding) {
    auto code = compile(binding, R"(
        bool flip(bool& value) { value = !value; return value; }
        bool run(const bool& read, bool& write) { bool copy = read; write = copy; return flip(write); }
    )");
    GuardedBool read{true}, write{false};
    const std::array arguments{BusinessValue{ValueKind::boolean, 0, ArgumentStorage::read_only, read.value},
                               BusinessValue{ValueKind::boolean, 0, ArgumentStorage::reference, write.value}};
    const auto result = take(execute_typed(runtime, binding, code.get(), {}, arguments));
    require(result.kind == ValueKind::boolean && result.bits == 0 && *read.value && !*write.value,
            "JIT bool borrowing, local storage, or internal calls changed a one-byte bool value.");
}
}

void verify_bool_abi() {
    Runtime runtime;
    const auto type = take(NativeRegistration<BoolAbiHost>{runtime, "acceptance.BoolAbiHost", TypeKind::object}
                               .field<&BoolAbiHost::active>("active")
                               .method<&BoolAbiHost::value>("value", true)
                               .publish());
    const auto binding = bind_type(runtime, type);
    verify_required_services(runtime, binding);
    verify_arguments(runtime, binding);
    verify_storage(runtime, binding);
    auto receiver = take(runtime.create(type));
    verify_results(runtime, binding, receiver.id());
}
}
