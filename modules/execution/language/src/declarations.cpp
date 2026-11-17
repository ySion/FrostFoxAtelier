#include "language/declarations.hpp"
#include "language/lexer.hpp"

#include <algorithm>
#include <charconv>

namespace atelier::language {
std::expected<TypeName, Diagnostic> detail::parse_type_name(Tokens& tokens) {
    TypeName result;
    for (;;) {
        ATELIER_TRY_BIND(array, tokens.accept("Array"));
        if (!*array)
            break;
        if (++result.array_depth > 128)
            return reject(tokens.token_.position, "Array type nesting exceeds the compiler limit of 128.");
        ATELIER_TRY(tokens.expect("<"));
    }
    if (tokens.token_.text == "bool" || tokens.token_.text == "float" || tokens.token_.text == "double" ||
        tokens.token_.text == "int" || tokens.token_.text == "uint") {
        ATELIER_TRY_BIND(name, tokens.consume());
        result.name = name->text;
    } else {
        ATELIER_TRY_BIND(name, tokens.qualified_identifier());
        result.name = std::move(*name);
    }
    for (std::uint32_t i = 0; i < result.array_depth; ++i) {
        ATELIER_TRY(tokens.expect(">"));
    }
    return result;
}

std::expected<TypeDeclaration, Diagnostic> detail::parse_type_declaration(Tokens& tokens) {
    TypeDeclaration result;
    result.position = tokens.token_.position;
    ATELIER_TRY_BIND(attributed, tokens.accept("["));
    if (*attributed) {
        ATELIER_TRY_BIND(option, tokens.identifier());
        if (option->text != "Flags")
            return reject(option->position, "Unknown enum attribute: " + std::string(option->text));
        if (tokens.token_.text == "=")
            return reject(option->position, "Flags is a flag without a value.");
        result.flags = true;
        ATELIER_TRY(tokens.expect("]"));
        if (tokens.token_.text != "enum")
            return reject(result.position, "[Flags] applies to enum declarations only.");
    }
    ATELIER_TRY_BIND(enumeration, tokens.accept("enum"));
    result.enumeration = *enumeration;
    if (!result.enumeration) {
        ATELIER_TRY(tokens.expect("struct"));
    }
    ATELIER_TRY_BIND(type_name, tokens.qualified_identifier());
    result.name = std::move(*type_name);
    if (result.name == "String" || result.name == "Array" || result.name.starts_with("core.") ||
        result.name.starts_with("math."))
        return reject(result.position, "Source declarations cannot replace built-in types.");
    if (editor_reserved(result.name))
        return reject(result.position,
                      "Source declarations cannot use the editor. prefix; it is reserved for editor types.");
    if (result.enumeration) {
        ATELIER_TRY_BIND(underlying, tokens.accept(":"));
        if (*underlying) {
            ATELIER_TRY_BIND(unsigned_type, tokens.accept("uint"));
            if (*unsigned_type)
                result.signed_enum = false;
            else {
                ATELIER_TRY(tokens.expect("int"));
            }
        }
        if (result.flags && result.signed_enum)
            return reject(result.position, "Flags enums require ': uint' underlying type.");
    }
    ATELIER_TRY(tokens.expect("{"));
    std::int64_t next = 0;
    for (;;) {
        ATELIER_TRY_BIND(closed, tokens.accept("}"));
        if (*closed)
            break;
        if (result.enumeration) {
            EnumeratorAttribute attribute;
            ATELIER_TRY_BIND(item_attributed, tokens.accept("["));
            if (*item_attributed) {
                bool seen_label = false;
                bool seen_desc = false;
                for (;;) {
                    ATELIER_TRY_BIND(option, tokens.identifier());
                    if ((option->text == "Label" && seen_label) || (option->text == "DESC" && seen_desc))
                        return reject(option->position, "Duplicate enumerator attribute: " + std::string(option->text));
                    if (option->text != "Label" && option->text != "DESC")
                        return reject(option->position, "Unknown enumerator attribute: " + std::string(option->text));
                    ATELIER_TRY(tokens.expect("="));
                    if (tokens.token_.kind != Token::Kind::string)
                        return reject(tokens.token_.position,
                                      std::string(option->text) + " requires a string literal.");
                    ATELIER_TRY_BIND(str_tok, tokens.consume());
                    ATELIER_TRY_BIND(decoded, string_value(*str_tok));
                    if (option->text == "Label") {
                        if (std::ranges::any_of(*decoded, [](unsigned char c) { return c < 32 || c == 127; }))
                            return reject(option->position, "Label must not contain control characters.");
                        attribute.label = std::move(*decoded);
                        seen_label = true;
                    } else {
                        if (!normalize_description_text(*decoded))
                            return reject(option->position,
                                          "DESC must not contain control characters other than line breaks and tabs.");
                        attribute.description = std::move(*decoded);
                        seen_desc = true;
                    }
                    ATELIER_TRY_BIND(more_attr, tokens.accept(","));
                    if (!*more_attr)
                        break;
                }
                ATELIER_TRY(tokens.expect("]"));
            }
            ATELIER_TRY_BIND(name, tokens.identifier());
            auto value = next;
            ATELIER_TRY_BIND(assigned, tokens.accept("="));
            if (*assigned) {
                ATELIER_TRY_BIND(negative, tokens.accept("-"));
                ATELIER_TRY_BIND(number, tokens.consume());
                std::uint64_t magnitude{};
                const auto converted =
                    std::from_chars(number->text.data(), number->text.data() + number->text.size(), magnitude);
                if (number->kind != Token::Kind::number || converted.ec != std::errc{} ||
                    converted.ptr != number->text.data() + number->text.size() || magnitude > 4294967295ull)
                    return reject(number->position, "Enum values require a 32-bit decimal integer.");
                value = *negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
            }
            if (value < (result.signed_enum ? -2147483648ll : 0ll) ||
                value > (result.signed_enum ? 2147483647ll : 4294967295ll))
                return reject(name->position, "Enum value exceeds its underlying type.");
            if (std::ranges::any_of(result.enumerators, [&](const auto& item) { return item.first == name->text; }))
                return reject(name->position, "Duplicate enum item name.");
            result.enumerators.emplace_back(name->text, static_cast<std::uint32_t>(value));
            result.enumerator_attributes.push_back(std::move(attribute));
            next = value + 1;
            if (tokens.token_.text != "}") {
                ATELIER_TRY(tokens.expect(","));
            }
        } else {
            ATELIER_TRY_BIND(type, parse_type_name(tokens));
            ATELIER_TRY_BIND(name, tokens.identifier());
            ATELIER_TRY(tokens.expect(";"));
            if (std::ranges::any_of(result.fields, [&](const auto& field) { return field.first == name->text; }))
                return reject(name->position, "Duplicate struct field name.");
            result.fields.emplace_back(name->text, std::move(*type));
        }
    }
    if (!result.enumeration) {
        ATELIER_TRY(tokens.expect(";"));
    } else {
        ATELIER_TRY(tokens.accept(";"));
    }
    return result;
}

std::expected<std::vector<TypeDeclaration>, Diagnostic> collect_declarations(std::string_view source) {
    using namespace detail;
    ATELIER_TRY_BIND(created, Tokens::create(source));
    auto& tokens = *created;
    std::vector<TypeDeclaration> result;
    while (tokens.token_.kind != Token::Kind::end) {
        if (tokens.token_.text == "[" || tokens.token_.text == "enum" || tokens.token_.text == "struct") {
            if (result.size() == 1024)
                return reject(tokens.token_.position, "A source module supports at most 1024 type declarations.");
            ATELIER_TRY_BIND(declaration, parse_type_declaration(tokens));
            if (std::ranges::any_of(result, [&](const auto& other) { return other.name == declaration->name; }))
                return reject(declaration->position, "Duplicate source type name.");
            result.push_back(std::move(*declaration));
            continue;
        }
        ATELIER_TRY_BIND(constant, tokens.accept("const"));
        if (*constant) {
            // Full typing and constant evaluation belong to the typed parser.
            for (;;) {
                ATELIER_TRY_BIND(closed, tokens.accept(";"));
                if (*closed)
                    break;
                if (tokens.token_.kind == Token::Kind::end || tokens.token_.text == "{" || tokens.token_.text == "}")
                    return reject(tokens.token_.position, "Expected ';' to close a module constant.");
                ATELIER_TRY(tokens.consume());
            }
            continue;
        }
        // Function syntax and bodies are checked by the full typed parser.
        // Tokenization preserves comments, strings and original positions.
        for (;;) {
            ATELIER_TRY_BIND(open, tokens.accept("{"));
            if (*open)
                break;
            if (tokens.token_.kind == Token::Kind::end)
                return reject(tokens.token_.position, "Expected a function body.");
            ATELIER_TRY(tokens.consume());
        }
        std::size_t depth = 1;
        while (depth) {
            if (tokens.token_.kind == Token::Kind::end)
                return reject(tokens.token_.position, "Expected '}' to close the function body.");
            if (tokens.token_.text == "{")
                ++depth;
            if (tokens.token_.text == "}")
                --depth;
            ATELIER_TRY(tokens.consume());
        }
    }
    std::ranges::sort(result, {}, &TypeDeclaration::name);
    return result;
}
}
