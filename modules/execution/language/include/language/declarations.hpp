#pragma once
#include "language/source.hpp"

namespace atelier::language {
struct TypeName {
    std::string name;

    std::uint32_t array_depth{};

    bool operator==(const TypeName&) const = default;
};

struct EnumeratorAttribute {
    std::string label{};

    std::string description{};

    bool operator==(const EnumeratorAttribute&) const = default;
};

struct MethodParameter {
    std::string name;

    TypeName type;

    ParameterPassing passing{ParameterPassing::value};

    SourcePosition position;

    std::vector<std::pair<std::string, std::string>> metadata;
};

// Unresolved source definitions. Runtime alone resolves identities and layouts.
struct TypeDeclaration {
    SourcePosition position;

    std::string name;

    bool enumeration{};

    bool signed_enum{true};

    bool flags{};

    std::string description;

    std::vector<std::pair<std::string, std::uint32_t>> enumerators;

    std::vector<EnumeratorAttribute> enumerator_attributes;

    std::vector<std::pair<std::string, TypeName>> fields;

    std::vector<std::string> field_descriptions;

    bool same_definition(const TypeDeclaration& other) const noexcept {
        return name == other.name && description == other.description && enumeration == other.enumeration &&
               signed_enum == other.signed_enum && flags == other.flags && enumerators == other.enumerators &&
               enumerator_attributes == other.enumerator_attributes && fields == other.fields &&
               field_descriptions == other.field_descriptions;
    }
};

[[nodiscard]] std::expected<std::vector<TypeDeclaration>, Diagnostic> collect_declarations(std::string_view source);

[[nodiscard]] std::expected<std::vector<TypeName>, Diagnostic> collect_array_types(std::string_view source);

// The editor publishes its own types under this prefix. Until Programs have their own visibility
// domains, Source cannot declare such types and its host catalog does not contain them.
[[nodiscard]] inline bool editor_reserved(std::string_view name) noexcept {
    return name.starts_with("editor.");
}

[[nodiscard]] inline bool synthesized_array_name(std::string_view name) noexcept {
    return name.starts_with("__source_array.");
}

[[nodiscard]] inline std::string make_synthesized_array_name(std::string_view element_name, std::uint32_t depth = 1) {
    std::string result;
    result.reserve(depth * 15 + element_name.size());
    for (std::uint32_t i = 0; i < depth; ++i)
        result += "__source_array.";
    result += element_name;
    return result;
}

namespace detail {
class Tokens;
struct Token;

std::expected<std::string, Diagnostic> parse_presentation_text(Tokens&, const Token& option);

std::expected<std::string_view, Diagnostic> peek_attributed_keyword(const Tokens&);

std::expected<std::string, Diagnostic> parse_description_attribute(Tokens&, std::string_view target_kind);

std::expected<void, Diagnostic> skip_constant_initializer(Tokens&);

std::expected<TypeName, Diagnostic> parse_type_name(Tokens&);

std::expected<TypeDeclaration, Diagnostic> parse_type_declaration(Tokens&);

std::expected<void, Diagnostic> parse_parameter_attributes(Tokens&, bool allow_attributes,
                                                           std::vector<std::pair<std::string, std::string>>& metadata);
}
}
