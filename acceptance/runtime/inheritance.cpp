#include "inheritance_fixture.hpp"
#include <cstdio>

using namespace atelier;

using namespace atelier::acceptance::inheritance;

void verify_runtime_inheritance() {
    Runtime runtime;
    TypeDefinition child{"inheritance.FailingChild", TypeKind::object, {}, {}, {}};
    child.base = TypeReference::native<Base>();
    child.fields = {{"resource", {TypeReference::native<Resource>()}}, {"failure", {TypeReference::native<Failing>()}}};
    auto failing = NativeRegistration<Failing>{runtime, "inheritance.Failing", TypeKind::record}.build();
    failing.native.construct = [](void* destination) noexcept -> Result<void> {
        if (Failing::fail)
            return std::unexpected(
                Error{ErrorCode::construction_failed, "Expected derived-field construction failure."});
        std::construct_at(static_cast<Failing*>(destination));
        return {};
    };
    const auto types = take(runtime.register_types({
        NativeRegistration<Base>{runtime, "inheritance.Base", TypeKind::object}.build(),
        NativeRegistration<Resource>{runtime, "inheritance.Resource", TypeKind::record}.build(),
        std::move(failing),
        std::move(child),
    }));
    Failing::fail = true;
    const auto before = Base::destroyed;
    require(!runtime.create(types[3]), "The derived constructor did not fail.");
    Failing::fail = false;
    require(Base::live == 0 && Base::destroyed == before + 1 && Resource::live == 0 && Resource::destroyed == 1 &&
                runtime.root_count() == 0,
            "Failed derived construction did not unwind exactly once.");
    std::puts("[pass] Runtime inheritance: partial construction unwinds base and fields");
}
