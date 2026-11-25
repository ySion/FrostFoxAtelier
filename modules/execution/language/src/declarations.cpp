#include "language/declarations.hpp"
#include "language/lexer.hpp"

#include <algorithm>
#include <charconv>

namespace atelier::language {
std::expected<std::string, Diagnostic> detail::parse_presentation_text(Tokens& tokens, const Token& option) {
    ATELIER_TRY(tokens.expect("="));
    if (tokens.token_.kind != Token::Kind::string)
        return reject(tokens.token_.position, std::string(option.text) + " requires a string literal.");
    ATELIER_TRY_BIND(token, tokens.consume());
    ATELIER_TRY_BIND(text, string_value(*token));
    if (option.text == "DESC") {
        if (!normalize_description_text(*text))
            return reject(option.position, "DESC must not contain control characters other than line breaks and tabs.");
    } else if (std::ranges::any_of(*text, [](unsigned char c) { return c < 32 || c == 127; })) {
        return reject(option.position, std::string(option.text) + " must not contain control characters.");
    }
    return text;
}

std::expected<std::string_view, Diagnostic> detail::peek_attributed_keyword(const Tokens& tokens) {
    auto probe = tokens;
    while (probe.token_.text == "[") {
        ATELIER_TRY(probe.consume());
        while (probe.token_.text != "]") {
            if (probe.token_.kind == Token::Kind::end || probe.token_.text == "[")
                return reject(probe.token_.position, "Expected ']' to close attribute list.");
            ATELIER_TRY(probe.consume());
        }
        ATELIER_TRY(probe.consume());
    }
    return probe.token_.text;
}

std::expected<std::string, Diagnostic> detail::parse_description_attribute(Tokens& tokens,
                                                                           std::string_view target_kind) {
    std::string description;
    bool seen_desc = false;
    while (tokens.token_.text == "[") {
        ATELIER_TRY(tokens.consume());
        for (;;) {
            ATELIER_TRY_BIND(option, tokens.identifier());
            if (option->text != "DESC")
                return reject(option->position,
                              "Unknown " + std::string(target_kind) + " attribute: " + std::string(option->text));
            if (seen_desc)
                return reject(option->position, "Duplicate DESC attribute.");
            ATELIER_TRY_ASSIGN(description, parse_presentation_text(tokens, *option));
            seen_desc = true;
            ATELIER_TRY_BIND(more, tokens.accept(","));
            if (!*more)
                break;
        }
        ATELIER_TRY(tokens.expect("]"));
    }
    return description;
}

std::expected<void, Diagnostic> detail::skip_constant_initializer(Tokens& tokens) {
    std::size_t brace_depth = 0;
    for (;;) {
        if (brace_depth == 0) {
            ATELIER_TRY_BIND(closed, tokens.accept(";"));
            if (*closed)
                return {};
        }
        if (tokens.token_.kind == Token::Kind::end)
            return reject(tokens.token_.position, "Expected ';' to close a module constant.");
        if (tokens.token_.text == "{") {
            ++brace_depth;
        } else if (tokens.token_.text == "}") {
            if (brace_depth == 0)
                return reject(tokens.token_.position, "Expected ';' to close a module constant.");
            --brace_depth;
        }
        ATELIER_TRY(tokens.consume());
    }
}

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
    std::string_view target_keyword = tokens.token_.text;
    if (target_keyword == "[") {
        ATELIER_TRY_ASSIGN(target_keyword, peek_attributed_keyword(tokens));
    }
    bool seen_flags = false;
    bool seen_desc = false;
    while (tokens.token_.text == "[") {
        ATELIER_TRY(tokens.consume());
        for (;;) {
            ATELIER_TRY_BIND(option, tokens.identifier());
            if (option->text == "Flags") {
                if (seen_flags)
                    return reject(option->position, "Duplicate Flags attribute.");
                if (tokens.token_.text == "=")
                    return reject(option->position, "Flags is a flag without a value.");
                result.flags = true;
                seen_flags = true;
            } else if (option->text == "DESC") {
                if (seen_desc)
                    return reject(option->position, "Duplicate DESC attribute.");
                ATELIER_TRY_ASSIGN(result.description, parse_presentation_text(tokens, *option));
                seen_desc = true;
            } else {
                const std::string_view kind_label = target_keyword == "enum" ? "enum" : "struct";
                return reject(option->position,
                              "Unknown " + std::string(kind_label) + " attribute: " + std::string(option->text));
            }
            ATELIER_TRY_BIND(more, tokens.accept(","));
            if (!*more)
                break;
        }
        ATELIER_TRY(tokens.expect("]"));
    }
    if (result.flags && tokens.token_.text != "enum")
        return reject(result.position, "[Flags] applies to enum declarations only.");
    ATELIER_TRY_BIND(enumeration, tokens.accept("enum"));
    result.enumeration = *enumeration;
    if (!result.enumeration) {
        ATELIER_TRY(tokens.expect("struct"));
    }
    ATELIER_TRY_BIND(type_name, tokens.qualified_identifier());
    result.name = std::move(*type_name);
    if (result.name == "String" || result.name == "Array" || result.name.starts_with("core.") ||
        result.name.starts_with("math.") || result.name.starts_with("__source_"))
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
                    ATELIER_TRY_BIND(decoded, parse_presentation_text(tokens, *option));
                    if (option->text == "Label") {
                        attribute.label = std::move(*decoded);
                        seen_label = true;
                    } else {
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
                if (number->kind != Token::Kind::number)
                    return reject(number->position, "Enum values require a 32-bit integer literal.");
                auto text = number->text;
                const bool explicit_unsigned = !text.empty() && (text.back() == 'u' || text.back() == 'U');
                if (explicit_unsigned) {
                    if (*negative)
                        return reject(number->position,
                                      "Unary minus cannot be applied to an unsigned integer literal.");
                    text.remove_suffix(1);
                }
                int base = 10;
                if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                    text.remove_prefix(2);
                    base = 16;
                }
                std::uint64_t magnitude{};
                const auto converted = std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
                if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
                    magnitude > 4294967295ull)
                    return reject(number->position, "Enum values require a 32-bit integer literal.");
                if (explicit_unsigned && result.signed_enum)
                    return reject(number->position, "Unsigned integer literal requires a ': uint' enum.");
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
            ATELIER_TRY_BIND(field_desc, parse_description_attribute(tokens, "struct field"));
            ATELIER_TRY_BIND(type, parse_type_name(tokens));
            ATELIER_TRY_BIND(name, tokens.identifier());
            ATELIER_TRY(tokens.expect(";"));
            if (std::ranges::any_of(result.fields, [&](const auto& field) { return field.first == name->text; }))
                return reject(name->position, "Duplicate struct field name.");
            result.fields.emplace_back(name->text, std::move(*type));
            result.field_descriptions.push_back(std::move(*field_desc));
        }
    }
    if (!result.enumeration) {
        ATELIER_TRY(tokens.expect(";"));
    } else {
        ATELIER_TRY(tokens.accept(";"));
    }
    return result;
}

std::expected<void, Diagnostic>
detail::parse_parameter_attributes(Tokens& tokens, bool allow_attributes,
                                   std::vector<std::pair<std::string, std::string>>& metadata) {
    const auto open_pos = tokens.token_.position;
    ATELIER_TRY_BIND(attributed, tokens.accept("["));
    if (!*attributed)
        return {};
    if (!allow_attributes)
        return reject(open_pos, "Parameter attributes apply only to class methods.");
    ATELIER_TRY_BIND(role_tok, tokens.identifier());
    std::string role;
    if (role_tok->text == "Port")
        role = "port";
    else if (role_tok->text == "In")
        role = "in";
    else if (role_tok->text == "Out")
        role = "out";
    else if (role_tok->text == "InOut")
        role = "inout";
    else if (role_tok->text == "Group")
        role = "group";
    else
        return reject(role_tok->position, "Unknown parameter attribute: " + std::string(role_tok->text));

    bool has_name = false;
    std::string explicit_name;
    std::string from;
    std::string layer;
    std::string port;
    std::string description;
    bool optional = false;

    const auto parse_options = [&](std::string_view close_tok) -> std::expected<void, Diagnostic> {
        for (;;) {
            if (tokens.token_.kind == Token::Kind::string) {
                const auto pos = tokens.token_.position;
                if (role == "port")
                    return reject(pos, "Port does not accept arguments.");
                if (has_name || !from.empty())
                    return reject(pos, "Specify either a literal name or From, not both.");
                ATELIER_TRY_BIND(str_tok, tokens.consume());
                ATELIER_TRY_BIND(decoded, string_value(*str_tok));
                if (std::ranges::any_of(*decoded, [](unsigned char c) { return c < 32 || c == 127; }))
                    return reject(pos, "Parameter attribute strings must not contain control characters.");
                if (role != "group" && decoded->empty())
                    return reject(pos, "Attribute name cannot be empty.");
                has_name = true;
                explicit_name = std::move(*decoded);
            } else {
                ATELIER_TRY_BIND(opt, tokens.identifier());
                if (role == "port" && opt->text != "DESC")
                    return reject(opt->position, "Port only accepts a DESC option.");
                if (opt->text == "Optional") {
                    if (tokens.token_.text == "=")
                        return reject(opt->position, "Optional is a flag without a value.");
                    if (role != "in")
                        return reject(opt->position, "Optional applies only to In parameters.");
                    if (optional)
                        return reject(opt->position, "Duplicate parameter option: Optional");
                    optional = true;
                } else if (opt->text == "From" || opt->text == "Layer" || opt->text == "Port" || opt->text == "DESC") {
                    if (opt->text == "DESC" && !description.empty())
                        return reject(opt->position, "Duplicate parameter option: DESC");
                    if (opt->text == "From" && (has_name || !from.empty()))
                        return reject(opt->position, "Specify either a literal name or From, not both.");
                    if (opt->text == "Layer" && !layer.empty())
                        return reject(opt->position, "Duplicate parameter option: Layer");
                    if (opt->text == "Port") {
                        if (role != "in" && role != "group")
                            return reject(opt->position, "Port option applies only to In and Group parameters.");
                        if (!port.empty())
                            return reject(opt->position, "Duplicate parameter option: Port");
                    }
                    ATELIER_TRY(tokens.expect("="));
                    if (tokens.token_.kind != Token::Kind::string)
                        return reject(tokens.token_.position, std::string(opt->text) + " requires a string literal.");
                    ATELIER_TRY_BIND(str_tok, tokens.consume());
                    ATELIER_TRY_BIND(decoded, string_value(*str_tok));
                    if (decoded->empty() ||
                        std::ranges::any_of(*decoded, [](unsigned char c) { return c < 32 || c == 127; }))
                        return reject(opt->position, std::string(opt->text) +
                                                         " requires a nonempty string without control characters.");
                    if (opt->text == "From")
                        from = std::move(*decoded);
                    else if (opt->text == "Layer")
                        layer = std::move(*decoded);
                    else if (opt->text == "Port")
                        port = std::move(*decoded);
                    else
                        description = std::move(*decoded);
                } else {
                    return reject(opt->position, "Unknown parameter attribute option: " + std::string(opt->text));
                }
            }
            ATELIER_TRY_BIND(more, tokens.accept(","));
            if (!*more)
                break;
        }
        ATELIER_TRY(tokens.expect(close_tok));
        return {};
    };

    ATELIER_TRY_BIND(parens, tokens.accept("("));
    if (*parens) {
        if (role == "port")
            return reject(role_tok->position, "Port is a flag without arguments.");
        if (tokens.token_.text == ")")
            return reject(tokens.token_.position, "Expected an option inside parameter attribute parentheses.");
        ATELIER_TRY(parse_options(")"));
    }
    ATELIER_TRY_BIND(comma, tokens.accept(","));
    if (*comma) {
        ATELIER_TRY(parse_options("]"));
    } else {
        ATELIER_TRY(tokens.expect("]"));
    }

    metadata.emplace_back("procedural.role", std::move(role));
    if (has_name)
        metadata.emplace_back("procedural.name", std::move(explicit_name));
    if (!from.empty())
        metadata.emplace_back("procedural.from", std::move(from));
    if (!layer.empty())
        metadata.emplace_back("procedural.layer", std::move(layer));
    if (!port.empty())
        metadata.emplace_back("procedural.port", std::move(port));
    if (!description.empty())
        metadata.emplace_back("procedural.description", std::move(description));
    if (optional)
        metadata.emplace_back("procedural.optional", "true");
    std::ranges::sort(metadata);
    return {};
}

std::expected<std::vector<TypeDeclaration>, Diagnostic> collect_declarations(std::string_view source) {
    using namespace detail;
    ATELIER_TRY_BIND(created, Tokens::create(source));
    auto& tokens = *created;
    std::vector<TypeDeclaration> result;
    while (tokens.token_.kind != Token::Kind::end) {
        bool is_type = tokens.token_.text == "enum" || tokens.token_.text == "struct";
        if (tokens.token_.text == "[") {
            ATELIER_TRY_BIND(target, peek_attributed_keyword(tokens));
            if (*target == "const")
                return reject(tokens.token_.position, "Attributes do not apply to module constants.");
            is_type = *target == "enum" || *target == "struct";
        }
        if (is_type) {
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
            ATELIER_TRY(skip_constant_initializer(tokens));
            continue;
        }
        ATELIER_TRY(parse_description_attribute(tokens, "function"));
        // Function syntax and bodies are checked by the full typed parser.
        // The lexer skips comments, keeps strings whole and tracks author positions.
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

std::expected<std::vector<TypeName>, Diagnostic> collect_array_types(std::string_view source) {
    using namespace detail;
    ATELIER_TRY_BIND(created, Tokens::create(source));
    auto& tokens = *created;
    std::vector<TypeName> result;
    while (tokens.token_.kind != Token::Kind::end) {
        if (tokens.token_.text == "Array") {
            auto probe = tokens;
            if (auto parsed = parse_type_name(probe); parsed && parsed->array_depth > 0) {
                for (std::uint32_t depth = 1; depth <= parsed->array_depth; ++depth)
                    result.push_back({parsed->name, depth});
            }
        }
        ATELIER_TRY(tokens.consume());
    }
    std::ranges::sort(result, [](const TypeName& a, const TypeName& b) {
        return a.array_depth != b.array_depth ? a.array_depth < b.array_depth : a.name < b.name;
    });
    const auto duplicate = std::ranges::unique(result);
    result.erase(duplicate.begin(), duplicate.end());
    return result;
}
}
