#pragma once
#include "world/world.hpp"
#include "editor/operator_parameters.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::editor::detail {
struct ScalarField {
    std::vector<std::string> names;

    world::FieldPath path;

    TypeId type;

    std::string label;

    std::string category;

    std::string description;

    ValueForm form{ValueForm::inline_value};

    // Inline leaves are scalars, enumerations, strings or whole Arrays of those; only the first two are Scalar values.
    TypeKind kind{TypeKind::scalar};

    // The declared range of the leaf, of its math vector for a lane, or of each element of an Array.
    FieldRange range{};

    bool material_entries_json{};
};

inline Result<std::vector<ScalarField>> author_fields(Runtime& runtime, TypeId type) {
    std::vector<ScalarField> result;
    const auto visit = [&](auto&& self, TypeId current, ScalarField prefix) -> Result<void> {
        if (prefix.path.size() > 16)
            return std::unexpected(Error{ErrorCode::invalid_definition, "Scene field nesting exceeds 16 levels."});
        ATELIER_TRY_BIND(info, runtime.describe(current));
        if ((*info)->kind == TypeKind::record &&
            current == runtime.native_type<MaterialEntriesJson>().value_or(TypeId{})) {
            prefix.path.push_back((*info)->fields.front().id);
            prefix.type = (*info)->fields.front().value.type;
            prefix.kind = TypeKind::string;
            prefix.material_entries_json = true;
            result.push_back(std::move(prefix));
            return {};
        }
        if ((*info)->kind == TypeKind::scalar || (*info)->kind == TypeKind::enumeration ||
            (*info)->kind == TypeKind::string) {
            prefix.type = current;
            prefix.kind = (*info)->kind;
            result.push_back(std::move(prefix));
            return {};
        }
        if (!prefix.path.empty() && (*info)->kind == TypeKind::array) {
            ATELIER_TRY_BIND(element, runtime.describe(*(*info)->element));
            if ((*element)->kind != TypeKind::scalar && (*element)->kind != TypeKind::enumeration &&
                (*element)->kind != TypeKind::string)
                return std::unexpected(
                    Error{ErrorCode::unsupported_operation, "Scene property Arrays hold scalars, enums or strings."});
            prefix.type = current;
            prefix.kind = TypeKind::array;
            result.push_back(std::move(prefix));
            return {};
        }
        if (!prefix.path.empty() && (*info)->kind != TypeKind::record)
            return std::unexpected(Error{
                ErrorCode::unsupported_operation,
                "Scene properties support scalars, enums, strings, Arrays of those and records of those values."});
        for (const auto& field : (*info)->fields) {
            auto child = prefix;
            child.names.push_back(field.name);
            child.path.push_back(field.id);
            // A vector's lanes share the aggregate caption. Other record children
            // use their own caption, while categories inherit through records.
            if (!(*info)->math.vector() && (*info)->math.shape != math::Shape::quaternion) {
                child.label = field.label;
                child.description = field.description;
            } else {
                if (!field.label.empty())
                    child.label = field.label;
                if (!field.description.empty())
                    child.description = field.description;
            }
            if (!field.category.empty())
                child.category = field.category;
            if (field.range != FieldRange{})
                child.range = field.range;
            if (field.value.form != ValueForm::inline_value) {
                child.type = field.value.type;
                child.form = field.value.form;
                result.push_back(std::move(child));
                continue;
            }
            ATELIER_TRY_BIND(added, self(self, field.value.type, std::move(child)));
        }
        return {};
    };
    ATELIER_TRY_BIND(visited, visit(visit, type, {}));
    return result;
}

inline Result<std::vector<ScalarField>> scalar_fields(Runtime& runtime, TypeId type) {
    auto result = author_fields(runtime, type);
    if (result)
        std::erase_if(*result, [](const auto& field) {
            return field.form != ValueForm::inline_value || field.kind == TypeKind::string ||
                   field.kind == TypeKind::array;
        });
    return result;
}

inline bool finite_scalar(const Scalar& value) {
    return std::visit(
        [](const auto& v) {
            if constexpr (std::is_floating_point_v<std::decay_t<decltype(v)>>)
                return std::isfinite(v);
            else
                return true;
        },
        value);
}

inline bool transform_path(const world::Types& types, std::span<const FieldId> path) {
    const auto contains = [&](const auto& paths) {
        return std::ranges::any_of(paths, [&](const auto& p) { return std::ranges::equal(p, path); });
    };
    return contains(types.position_fields) || contains(types.rotation_fields) || contains(types.scale_fields);
}
}
