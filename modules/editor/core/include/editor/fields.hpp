#pragma once
#include "world/world.hpp"
#include "editor/operator_parameters.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace atelier::editor::detail {
using FieldNames = std::vector<std::string>;

inline std::string dotted(std::span<const std::string> path) {
    std::string text;
    for (const auto& name : path) {
        if (!text.empty())
            text += '.';
        text += name;
    }
    return text;
}

struct ScalarField {
    FieldNames names;

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

    bool transient{};

    bool read_only{};

    MetadataList metadata{};
};

inline Result<std::vector<ScalarField>> author_fields(Runtime& runtime, TypeId type, bool include_transient = false,
                                                      std::vector<FieldNames>* skipped = nullptr) {
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
            child.transient |= field.transient;
            child.read_only |= field.read_only;
            if (!include_transient && child.transient) {
                if (skipped) {
                    child.names.push_back(field.name);
                    skipped->push_back(std::move(child.names));
                }
                continue;
            }
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
            for (const auto& entry : field.metadata) {
                const auto at =
                    std::ranges::find(child.metadata, entry.first, &std::pair<std::string, std::string>::first);
                if (at != child.metadata.end())
                    at->second = entry.second;
                else
                    child.metadata.push_back(entry);
            }
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
    auto result = author_fields(runtime, type, true);
    if (result)
        std::erase_if(*result, [](const auto& field) {
            return field.form != ValueForm::inline_value || field.kind == TypeKind::string ||
                   field.kind == TypeKind::array;
        });
    return result;
}

inline std::uint32_t declared_flag_mask(const TypeInfo::Enumeration& enumeration) noexcept {
    std::uint32_t mask = 0;
    for (const auto& [_, bits] : enumeration.values)
        mask |= bits;
    return mask;
}

inline bool valid_enum_bits(const TypeInfo::Enumeration& enumeration, std::uint32_t bits) noexcept {
    if (std::ranges::any_of(enumeration.values, [&](const auto& member) { return member.second == bits; }))
        return true;
    return enumeration.flags && (bits & declared_flag_mask(enumeration)) == bits;
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

inline bool scalar_less(const Scalar& a, const Scalar& b) noexcept {
    if (a.index() != b.index())
        return a.index() < b.index();
    return std::visit(
        [&](const auto& lhs) noexcept -> bool {
            using T = std::decay_t<decltype(lhs)>;
            const auto& rhs = std::get<T>(b);
            if constexpr (std::same_as<T, EnumValue>)
                return lhs.bits < rhs.bits;
            else
                return lhs < rhs;
        },
        a);
}

inline std::pair<Scalar, bool> clamp_scalar(Scalar value, const FieldRange& range) noexcept {
    bool clamped = false;
    if (range.hard_min && value.index() == range.hard_min->index() && scalar_less(value, *range.hard_min)) {
        value = *range.hard_min;
        clamped = true;
    }
    if (range.hard_max && value.index() == range.hard_max->index() && scalar_less(*range.hard_max, value)) {
        value = *range.hard_max;
        clamped = true;
    }
    return {value, clamped};
}

inline bool transform_field(const world::Types& types, FieldId field) noexcept {
    return field == types.position_fields[0][0] || field == types.rotation_fields[0][0] ||
           field == types.scale_fields[0][0];
}

inline bool transform_path(const world::Types& types, std::span<const FieldId> path) {
    const auto contains = [&](const auto& paths) {
        return std::ranges::any_of(paths, [&](const auto& p) { return std::ranges::equal(p, path); });
    };
    return contains(types.position_fields) || contains(types.rotation_fields) || contains(types.scale_fields);
}
}
