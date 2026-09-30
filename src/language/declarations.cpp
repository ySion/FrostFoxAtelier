#include "language/declarations.hpp"
#include "language/lexer.hpp"

#include <algorithm>
#include <charconv>

namespace atelier::language {
TypeDeclaration detail::parse_type_declaration(Tokens& tokens) {
    TypeDeclaration result;
    result.position = tokens.token_.position;
    result.enumeration = tokens.accept("enum");
    if (!result.enumeration) tokens.expect("struct");
    result.name = tokens.qualified_identifier();
    if (result.name == "String" || result.name == "Array" || result.name.starts_with("core."))
        reject(result.position, "Source declarations cannot replace built-in types.");
    if (result.enumeration && tokens.accept(":")) {
        if (tokens.accept("uint")) result.signed_enum = false;
        else tokens.expect("int");
    }
    tokens.expect("{");
    std::int64_t next = 0;
    while (!tokens.accept("}")) {
        if (result.enumeration) {
            const auto name = tokens.identifier();
            auto value = next;
            if (tokens.accept("=")) {
                const bool negative = tokens.accept("-");
                const auto number = tokens.consume();
                std::uint64_t magnitude{};
                const auto converted = std::from_chars(number.text.data(), number.text.data() + number.text.size(), magnitude);
                if (number.kind != Token::Kind::number || converted.ec != std::errc{} ||
                    converted.ptr != number.text.data() + number.text.size() || magnitude > 4294967295ull)
                    reject(number.position, "Enum values require a 32-bit decimal integer.");
                value = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
            }
            if (value < (result.signed_enum ? -2147483648ll : 0ll) || value > (result.signed_enum ? 2147483647ll : 4294967295ll))
                reject(name.position, "Enum value exceeds its underlying type.");
            if (std::ranges::any_of(result.enumerators, [&](const auto& item) { return item.first == name.text; }))
                reject(name.position, "Duplicate enum item name.");
            result.enumerators.emplace_back(name.text, static_cast<std::uint32_t>(value));
            next = value + 1;
            if (tokens.token_.text != "}") tokens.expect(",");
        } else {
            std::string type;
            if (tokens.token_.text == "float" || tokens.token_.text == "int" || tokens.token_.text == "uint")
                type = tokens.consume().text;
            else type = tokens.qualified_identifier();
            const auto name = tokens.identifier();
            tokens.expect(";");
            if (std::ranges::any_of(result.fields, [&](const auto& field) { return field.first == name.text; }))
                reject(name.position, "Duplicate struct field name.");
            result.fields.emplace_back(name.text, std::move(type));
        }
    }
    if (!result.enumeration) tokens.expect(";");
    else tokens.accept(";");
    return result;
}

std::expected<std::vector<TypeDeclaration>, Diagnostic> collect_declarations(std::string_view source) {
    using namespace detail;
    try {
        Tokens tokens(source);
        std::vector<TypeDeclaration> result;
        while (tokens.token_.kind != Token::Kind::end) {
            if (tokens.token_.text == "enum" || tokens.token_.text == "struct") {
                if (result.size() == 1024) reject(tokens.token_.position, "A source module supports at most 1024 type declarations.");
                auto declaration = parse_type_declaration(tokens);
                if (std::ranges::any_of(result, [&](const auto& other) { return other.name == declaration.name; }))
                    reject(declaration.position, "Duplicate source type name.");
                result.push_back(std::move(declaration));
                continue;
            }
            // Function syntax and bodies are checked by the full typed parser.
            // Tokenization preserves comments, strings and original positions.
            while (!tokens.accept("{")) {
                if (tokens.token_.kind == Token::Kind::end) reject(tokens.token_.position, "Expected a function body.");
                tokens.consume();
            }
            std::size_t depth = 1;
            while (depth) {
                if (tokens.token_.kind == Token::Kind::end) reject(tokens.token_.position, "Expected '}' to close the function body.");
                if (tokens.token_.text == "{") ++depth;
                if (tokens.token_.text == "}") --depth;
                tokens.consume();
            }
        }
        std::ranges::sort(result, {}, &TypeDeclaration::name);
        return result;
    } catch (Rejected& rejected) {
        return std::unexpected(std::move(rejected.diagnostic));
    }
}
}
