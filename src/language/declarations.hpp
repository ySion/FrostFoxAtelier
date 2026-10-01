#pragma once
#include "language/source.hpp"

namespace atelier::language {
struct TypeName {
    std::string name;
    std::uint32_t array_depth{};
    bool operator==(const TypeName&) const = default;
};
// Unresolved source definitions. Runtime alone resolves identities and layouts.
struct TypeDeclaration {
    SourcePosition position;
    std::string name;
    bool enumeration{};
    bool signed_enum{true};
    std::vector<std::pair<std::string, std::uint32_t>> enumerators;
    std::vector<std::pair<std::string, TypeName>> fields;
    bool same_definition(const TypeDeclaration& other) const noexcept {
        return name == other.name && enumeration == other.enumeration && signed_enum == other.signed_enum &&
            enumerators == other.enumerators && fields == other.fields;
    }
};
[[nodiscard]] std::expected<std::vector<TypeDeclaration>, Diagnostic> collect_declarations(std::string_view source);
namespace detail {
class Tokens;
TypeName parse_type_name(Tokens&);
TypeDeclaration parse_type_declaration(Tokens&);
}
}
