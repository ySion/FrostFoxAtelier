#pragma once
#include "world/world.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::editor::detail {
struct ScalarField {
    std::vector<std::string> names;
    world::FieldPath path;
    TypeId type;
};
inline Result<std::vector<ScalarField>> scalar_fields(Runtime& runtime, TypeId type) {
    std::vector<ScalarField> result;
    const auto visit = [&](auto&& self, TypeId current, ScalarField prefix) -> Result<void> {
        if (prefix.path.size() > 16) return std::unexpected(Error{ErrorCode::invalid_definition,"Scene field nesting exceeds 16 levels."});
        auto info = runtime.describe(current);
        if (!info) return std::unexpected(info.error());
        if ((*info)->kind == TypeKind::scalar || (*info)->kind == TypeKind::enumeration) {
            prefix.type = current; result.push_back(std::move(prefix)); return {};
        }
        if (!prefix.path.empty() && (*info)->kind != TypeKind::record)
            return std::unexpected(Error{ErrorCode::unsupported_operation,"Scene properties support scalars, enums and records of those values."});
        for (const auto& field : (*info)->fields) {
            if (field.value.form != ValueForm::inline_value)
                return std::unexpected(Error{ErrorCode::unsupported_operation,"Scene business references are not supported: " + field.name});
            auto child = prefix; child.names.push_back(field.name); child.path.push_back(field.id);
            auto added = self(self, field.value.type, std::move(child)); if (!added) return added;
        }
        return {};
    };
    auto visited = visit(visit, type, {}); if (!visited) return std::unexpected(visited.error());
    return result;
}
inline bool finite_scalar(const Scalar& value) {
    return std::visit([](const auto& v) {
        if constexpr (std::is_floating_point_v<std::decay_t<decltype(v)>>) return std::isfinite(v);
        else return true;
    },value);
}
inline bool transform_path(const world::Types& types, std::span<const FieldId> path) {
    const auto contains = [&](const auto& paths) {
        return std::ranges::any_of(paths,[&](const auto& p){ return std::ranges::equal(p,path); });
    };
    return contains(types.position_fields) || contains(types.rotation_fields) || contains(types.scale_fields);
}
}
