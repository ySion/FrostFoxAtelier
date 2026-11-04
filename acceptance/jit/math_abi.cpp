#include "../helpers.hpp"
#include "math/math.hpp"
#include "runtime/native.hpp"
#include "source/detail/bridge.hpp"

#include <string>

namespace atelier::jit_acceptance {
namespace {
using namespace source::detail;

using namespace atelier::acceptance::helpers;

struct MathAbiHost {
    math::Vector3d value() const { return {1.0, 2.0, 3.0}; }
};

bool same_operand(const BusinessValue& a, const BusinessValue& b) {
    return a.kind == b.kind && a.bits == b.bits && a.storage == b.storage && a.data == b.data;
}
}

void verify_math_abi() {
    Runtime runtime;
    const auto type = take(NativeRegistration<MathAbiHost>{runtime, "acceptance.MathAbiHost", TypeKind::object}
                               .method<&MathAbiHost::value>("value", true)
                               .publish());
    auto binding = bind_type(runtime, type);
    using Entry = std::uint32_t (*)(NativeContext*, void*, void*, NativeFailure*) noexcept;
    static const auto entry = reinterpret_cast<Entry>(native_method<&MathAbiHost::value>("value", true).native_entry);
    binding.methods.front().entry = reinterpret_cast<std::uintptr_t>(
        +[](NativeContext* context, void* receiver, void* output, NativeFailure* failure) noexcept -> std::uint32_t {
            const auto status = entry(context, receiver, output, failure);
            if (status != 0)
                return status;
            // Output has fixed type/address now; a failed call still must not
            // commit its already-written aggregate scratch to the caller.
            *failure = {static_cast<std::uint32_t>(ErrorCode::invocation_failed), "Aggregate result failure."};
            return 1;
        });
    constexpr std::string_view source = R"(
        vector3d run() {
            acceptance.MathAbiHost held = new acceptance.MathAbiHost();
            weak acceptance.MathAbiHost observed = observe(held);
            return self.value();
        })";
    const EnvironmentView storage(binding);
    const std::string name(binding.name);
    const auto environment = storage.view(name.c_str());
    BusinessCode* compiled{};
    char diagnostic[1024]{};
    const auto status =
        atelier_compile_business(&services, "math-abi.source", source.data(), static_cast<std::uint32_t>(source.size()),
                                 &environment, &compiled, diagnostic, sizeof(diagnostic));
    CodeOwner code{compiled, &atelier_destroy_business};
    require(status == 0, diagnostic);
    auto receiver = take(runtime.create(type));
    math::Vector3d destination{-5.0, -6.0, -7.0};
    const BusinessValue sentinel{atelier_business_info(code.get()).signature.result, 0xcafebabeu,
                                 ArgumentStorage::value, &destination};
    auto output = sentinel;
    {
        auto activation = take(ExecutionScope::enter(runtime));
        Invocation invocation(runtime, binding, atelier_business_layout(code.get()), {}, nullptr, {}, &activation);
        take(invocation.frame.hold(receiver_slot, receiver.id()));
        require(atelier_invoke_business(code.get(), &invocation, nullptr, 0, &output, diagnostic, sizeof(diagnostic)) !=
                    0,
                "Failed native result was accepted.");
        require(invocation.resources_cleared() && runtime.root_count() == 1,
                "Aggregate result rejection relied on RAII cleanup.");
        require(same_operand(output, sentinel) && destination == math::Vector3d{-5, -6, -7},
                "Failed native result changed caller storage.");
    }
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({64}));
    require(runtime.live_objects() == 1 && runtime.root_count() == 1,
            "Aggregate result rejection retained its source-created object.");
}
}
