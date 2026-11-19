#pragma once
#include "language/source.hpp"
#include "foundation/result.hpp"

namespace atelier::language::detail {
inline std::unexpected<Diagnostic> reject(SourcePosition position, std::string message) {
    return std::unexpected(Diagnostic{position, std::move(message)});
}

inline bool letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

inline bool digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

inline bool reserved(std::string_view value) noexcept {
    return value == "bool" || value == "true" || value == "false" || value == "for" || value == "break" ||
           value == "continue" || value == "void" || value == "float" || value == "double" || value == "uint" ||
           value == "int" || value == "const" || value == "if" || value == "else" || value == "while" ||
           value == "return" || value == "self" || value == "weak" || value == "null" || value == "new" ||
           value == "observe" || value == "try_acquire" || value == "async" || value == "yield" || value == "struct" ||
           value == "enum" || value == "class";
}

// Naming projection only; the parser also checks the accessor's signature.
inline std::string property_name(std::string_view name) {
    const auto start = name.rfind('.') == std::string_view::npos ? 0 : name.rfind('.') + 1;
    const auto member = name.substr(start);
    if ((!member.starts_with("Get") && !member.starts_with("Set")) || member.size() == 3)
        return {};
    const auto suffix = member.substr(3);
    if (!letter(suffix.front()) || reserved(suffix))
        return {};
    return std::string(name.substr(0, start)) + std::string(suffix);
}

// The project collector and typed parser use the same accessor eligibility rule.
inline std::string property_name(std::string_view name, std::size_t parameters, bool void_result,
                                 bool writable_parameter) {
    auto alias = property_name(name);
    if (alias.empty())
        return {};
    const auto start = name.rfind('.') == std::string_view::npos ? 0 : name.rfind('.') + 1;
    const bool getter = name.substr(start).starts_with("Get");
    if (getter ? (parameters != 0 || void_result) : (parameters != 1 || !void_result || writable_parameter))
        return {};
    return alias;
}

struct Token {
    enum class Kind { end, identifier, number, punctuation, string };

    Kind kind;

    std::string_view text;

    SourcePosition position;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::expected<Token, Diagnostic> next() {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n' || peek() == '\v' ||
                   peek() == '\f')
                advance();
            if (peek() == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n' && peek() != '\r')
                    advance();
            } else if (peek() == '/' && peek(1) == '*') {
                const auto start = position_;
                advance();
                advance();
                while (!at_end() && !(peek() == '*' && peek(1) == '/'))
                    advance();
                if (at_end())
                    return reject(start, "Unterminated block comment.");
                advance();
                advance();
            } else
                break;
        }
        const auto start = position_;
        if (at_end())
            return Token{Token::Kind::end, {}, start};
        const auto first = peek();
        if (first == '"') {
            advance();
            while (!at_end() && peek() != '"') {
                if (peek() == '\n' || peek() == '\r' || peek() == '\0')
                    return reject(start, "Invalid character in string literal.");
                if (peek() == '\\') {
                    advance();
                    if (at_end())
                        break;
                }
                advance();
            }
            if (at_end())
                return reject(start, "Unterminated string literal.");
            advance();
            return token(Token::Kind::string, start);
        }
        if (letter(first)) {
            do {
                advance();
            } while (letter(peek()) || digit(peek()));
            return token(Token::Kind::identifier, start);
        }
        if (digit(first)) {
            do {
                advance();
            } while (digit(peek()));
            if (peek() == '.') {
                advance();
                while (digit(peek()))
                    advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                advance();
                if (peek() == '+' || peek() == '-')
                    advance();
                if (!digit(peek()))
                    return reject(start, "A numeric exponent requires digits.");
                while (digit(peek()))
                    advance();
            }
            if (peek() == 'f' || peek() == 'F' || peek() == 'd' || peek() == 'D')
                advance();
            if (letter(peek()) || digit(peek()))
                return reject(start, "Invalid numeric literal suffix.");
            return token(Token::Kind::number, start);
        }
        if (first == '=' || first == '!' || first == '<' || first == '>') {
            advance();
            if (peek() == '=')
                advance();
            else if ((first == '<' || first == '>') && peek() == first)
                advance();
            return token(Token::Kind::punctuation, start);
        }
        if (first == '+' || first == '-') {
            advance();
            if (peek() == first)
                return reject(start, "Increment and decrement operators are not supported; use assignment.");
            return token(Token::Kind::punctuation, start);
        }
        if (first == '&' || first == '|') {
            advance();
            if (peek() == first)
                advance();
            return token(Token::Kind::punctuation, start);
        }
        if (first == ':') {
            advance();
            if (peek() == ':')
                advance();
            return token(Token::Kind::punctuation, start);
        }
        if (std::string_view("(){}[];.,*/%^~").find(first) != std::string_view::npos) {
            advance();
            return token(Token::Kind::punctuation, start);
        }
        return reject(start, "Unsupported source character.");
    }

private:
    bool at_end() const noexcept { return position_.offset == source_.size(); }

    char peek(std::size_t ahead = 0) const noexcept {
        return ahead < source_.size() - position_.offset ? source_[position_.offset + ahead] : '\0';
    }

    void advance() noexcept {
        const auto value = source_[position_.offset++];
        if (value == '\n' || (value == '\r' && peek() != '\n')) {
            ++position_.line;
            position_.column = 1;
        } else
            ++position_.column;
    }

    Token token(Token::Kind kind, SourcePosition start) const {
        return {kind, source_.substr(start.offset, position_.offset - start.offset), start};
    }

    std::string_view source_;

    SourcePosition position_;
};

// RFC 3629 UTF-8 validation matching foundation StringView::is_valid_utf8().
inline bool valid_utf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80)
            continue;
        if (first <= 0xdf) {
            if (first < 0xc2 || i == text.size() || (static_cast<unsigned char>(text[i++]) ^ 0x80u) >= 0x40u)
                return false;
        } else if (first <= 0xef) {
            if (text.size() - i < 2)
                return false;
            const auto b1 = static_cast<unsigned char>(text[i]);
            const auto b2 = static_cast<unsigned char>(text[i + 1]);
            if (((b1 ^ 0x80u) | (b2 ^ 0x80u)) >= 0x40u || (first == 0xe0 && b1 < 0xa0) || (first == 0xed && b1 >= 0xa0))
                return false;
            i += 2;
        } else if (first <= 0xf4) {
            if (text.size() - i < 3)
                return false;
            const auto b1 = static_cast<unsigned char>(text[i]);
            const auto b2 = static_cast<unsigned char>(text[i + 1]);
            const auto b3 = static_cast<unsigned char>(text[i + 2]);
            if (((b1 ^ 0x80u) | (b2 ^ 0x80u) | (b3 ^ 0x80u)) >= 0x40u || (first == 0xf0 && b1 < 0x90) ||
                (first == 0xf4 && b1 >= 0x90))
                return false;
            i += 3;
        } else {
            return false;
        }
    }
    return true;
}

// A string token's UTF-8 bytes, with the escapes \\ \" \n \r \t \0 decoded.
inline std::expected<std::string, Diagnostic> string_value(const Token& token) {
    std::string value;
    for (std::size_t i = 1; i + 1 < token.text.size(); ++i) {
        char c = token.text[i];
        if (c == '\\') {
            switch (c = token.text[++i]) {
            case '\\':
            case '"':
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '0':
                c = '\0';
                break;
            default:
                return reject(token.position, "Unsupported string escape.");
            }
        }
        value.push_back(c);
    }
    if (!valid_utf8(value))
        return reject(token.position, "String literal is not valid UTF-8.");
    return value;
}

inline bool normalize_description_text(std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127 ||
            (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
             static_cast<unsigned char>(text[i + 1]) <= 0x9f))
            return false;
    }
    auto output = text.find('\r');
    if (output == std::string::npos)
        return true;
    for (auto input = output; input < text.size(); ++input) {
        const char c = text[input];
        text[output++] = c == '\r' ? '\n' : c;
        if (c == '\r' && input + 1 < text.size() && text[input + 1] == '\n')
            ++input;
    }
    text.resize(output);
    return true;
}

class Tokens {
public:
    static std::expected<Tokens, Diagnostic> create(std::string_view source) {
        Lexer lexer(source);
        ATELIER_TRY_BIND(token, lexer.next());
        return Tokens{lexer, *token};
    }

    std::expected<Token, Diagnostic> consume() {
        auto previous = token_;
        ATELIER_TRY_BIND(next, lexer_.next());
        token_ = *next;
        return previous;
    }

    std::expected<bool, Diagnostic> accept(std::string_view text) {
        if (token_.text != text)
            return false;
        ATELIER_TRY(consume());
        return true;
    }

    std::expected<Token, Diagnostic> expect(std::string_view text) {
        if (text == ">" && token_.text == ">>") {
            auto first = token_;
            first.text = first.text.substr(0, 1);
            token_.text.remove_prefix(1);
            ++token_.position.offset;
            ++token_.position.column;
            return first;
        }
        if (token_.text != text)
            return reject(token_.position, "Expected '" + std::string(text) + "'.");
        return consume();
    }

    std::expected<Token, Diagnostic> identifier() {
        if (token_.kind != Token::Kind::identifier || reserved(token_.text))
            return reject(token_.position, "Expected a non-reserved identifier.");
        return consume();
    }

    std::expected<std::string, Diagnostic> qualified_identifier() {
        ATELIER_TRY_BIND(first, identifier());
        std::string name(first->text);
        for (;;) {
            ATELIER_TRY_BIND(dot, accept("."));
            if (!*dot)
                break;
            name += '.';
            ATELIER_TRY_BIND(part, identifier());
            name += part->text;
        }
        return name;
    }

    Lexer lexer_;

    Token token_;

private:
    Tokens(Lexer lexer, Token token) : lexer_(lexer), token_(token) {}
};
}
