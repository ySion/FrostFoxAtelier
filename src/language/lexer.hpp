#pragma once
#include "language/source.hpp"

namespace atelier::language::detail {
struct Rejected { Diagnostic diagnostic; };
[[noreturn]] inline void reject(SourcePosition position, std::string message) {
    throw Rejected{{position, std::move(message)}};
}

inline bool letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}
inline bool digit(char value) noexcept { return value >= '0' && value <= '9'; }
inline bool reserved(std::string_view value) noexcept {
    return value == "bool" || value == "true" || value == "false" || value == "for" || value == "break" || value == "continue" ||
        value == "void" || value == "float" || value == "double" || value == "uint" || value == "int" || value == "const" || value == "if" ||
        value == "else" || value == "while" || value == "return" || value == "self" ||
        value == "weak" || value == "null" || value == "new" || value == "observe" || value == "try_acquire" ||
        value == "async" || value == "yield" || value == "struct" || value == "enum" || value == "class";
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
    Token next() {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n' ||
                   peek() == '\v' || peek() == '\f') advance();
            if (peek() == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
            } else if (peek() == '/' && peek(1) == '*') {
                const auto start = position_;
                advance(); advance();
                while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
                if (at_end()) reject(start, "Unterminated block comment.");
                advance(); advance();
            } else break;
        }
        const auto start = position_;
        if (at_end()) return {Token::Kind::end, {}, start};
        const auto first = peek();
        if (first == '"') {
            advance();
            while (!at_end() && peek() != '"') {
                if (peek() == '\n' || peek() == '\r' || peek() == '\0') reject(start, "Invalid character in string literal.");
                if (peek() == '\\') { advance(); if (at_end()) break; }
                advance();
            }
            if (at_end()) reject(start, "Unterminated string literal.");
            advance();
            return token(Token::Kind::string, start);
        }
        if (letter(first)) {
            do { advance(); } while (letter(peek()) || digit(peek()));
            return token(Token::Kind::identifier, start);
        }
        if (digit(first)) {
            do { advance(); } while (digit(peek()));
            if (peek() == '.') {
                advance();
                while (digit(peek())) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                advance();
                if (peek() == '+' || peek() == '-') advance();
                if (!digit(peek())) reject(start, "A numeric exponent requires digits.");
                while (digit(peek())) advance();
            }
            if (peek() == 'f' || peek() == 'F' || peek() == 'd' || peek() == 'D') advance();
            if (letter(peek()) || digit(peek())) reject(start, "Invalid numeric literal suffix.");
            return token(Token::Kind::number, start);
        }
        if (first == '=' || first == '!' || first == '<' || first == '>') {
            advance();
            if (peek() == '=') advance();
            else if ((first == '<' || first == '>') && peek() == first) advance();
            return token(Token::Kind::punctuation, start);
        }
        if (first == '+' || first == '-') {
            advance();
            if (peek() == first) reject(start, "Increment and decrement operators are not supported; use assignment.");
            return token(Token::Kind::punctuation, start);
        }
        if (first == '&' || first == '|') {
            advance();
            if (peek() == first) advance();
            return token(Token::Kind::punctuation, start);
        }
        if (first == ':') {
            advance();
            if (peek() == ':') advance();
            return token(Token::Kind::punctuation, start);
        }
        if (std::string_view("(){}[];.,*/%^~").find(first) != std::string_view::npos) {
            advance();
            return token(Token::Kind::punctuation, start);
        }
        reject(start, "Unsupported source character.");
    }
private:
    bool at_end() const noexcept { return position_.offset == source_.size(); }
    char peek(std::size_t ahead = 0) const noexcept {
        return ahead < source_.size() - position_.offset ? source_[position_.offset + ahead] : '\0';
    }
    void advance() noexcept {
        const auto value = source_[position_.offset++];
        if (value == '\n') { ++position_.line; position_.column = 1; }
        else ++position_.column;
    }
    Token token(Token::Kind kind, SourcePosition start) const {
        return {kind, source_.substr(start.offset, position_.offset - start.offset), start};
    }
    std::string_view source_;
    SourcePosition position_;
};

// A string token's UTF-8 bytes, with the escapes \\ \" \n \r \t \0 decoded.
inline std::string string_value(const Token& token) {
    std::string value;
    for (std::size_t i = 1; i + 1 < token.text.size(); ++i) {
        char c = token.text[i];
        if (c == '\\') {
            switch (c = token.text[++i]) {
            case '\\': case '"': break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '0': c = '\0'; break;
            default: reject(token.position, "Unsupported string escape.");
            }
        }
        value.push_back(c);
    }
    return value;
}

class Tokens {
public:
    explicit Tokens(std::string_view source) : lexer_(source), token_(lexer_.next()) {}
    Token consume() { auto previous = token_; token_ = lexer_.next(); return previous; }
    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        consume();
        return true;
    }
    Token expect(std::string_view text) {
        if (text == ">" && token_.text == ">>") {
            auto first = token_;
            first.text = first.text.substr(0, 1);
            token_.text.remove_prefix(1);
            ++token_.position.offset;
            ++token_.position.column;
            return first;
        }
        if (token_.text != text) reject(token_.position, "Expected '" + std::string(text) + "'.");
        return consume();
    }
    Token identifier() {
        if (token_.kind != Token::Kind::identifier || reserved(token_.text))
            reject(token_.position, "Expected a non-reserved identifier.");
        return consume();
    }
    std::string qualified_identifier() {
        std::string name(identifier().text);
        while (accept(".")) { name += '.'; name += identifier().text; }
        return name;
    }
    Lexer lexer_;
    Token token_;
};
}
