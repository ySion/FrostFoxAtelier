#include "language/source.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <utility>

namespace atelier::language {
namespace {
struct Rejected { Diagnostic diagnostic; };
[[noreturn]] void reject(SourcePosition position, std::string message) {
    throw Rejected{{position, std::move(message)}};
}

bool letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}
bool digit(char value) noexcept { return value >= '0' && value <= '9'; }
bool reserved(std::string_view value) noexcept {
    return value == "void" || value == "float" || value == "uint" || value == "if" ||
        value == "else" || value == "while" || value == "return" || value == "self" ||
        value == "weak" || value == "null" || value == "new" || value == "observe" || value == "try_acquire";
}
bool reference_type(ValueType type) noexcept {
    return type == ValueType::strong_reference || type == ValueType::weak_reference ||
        type == ValueType::null_reference;
}
struct Token {
    enum class Kind { end, identifier, number, punctuation };
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
            if (letter(peek())) reject(start, "Invalid numeric literal suffix.");
            return token(Token::Kind::number, start);
        }
        if (first == '=' || first == '!' || first == '<' || first == '>') {
            advance();
            if (peek() == '=') advance();
            if (first == '!' && position_.offset == start.offset + 1)
                reject(start, "Logical negation is not supported in this source slice.");
            return token(Token::Kind::punctuation, start);
        }
        if (std::string_view("(){};.,+-*").find(first) != std::string_view::npos) {
            advance();
            return token(Token::Kind::punctuation, start);
        }
        if (first == '/') reject(start, "Division is not supported in this source slice.");
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

struct BinaryOperator { int precedence; Operation operation; bool comparison; };
BinaryOperator binary_operator(std::string_view text) noexcept {
    if (text == "==") return {2, Operation::equal, true};
    if (text == "!=") return {2, Operation::not_equal, true};
    if (text == "<") return {3, Operation::less, true};
    if (text == "<=") return {3, Operation::less_equal, true};
    if (text == ">") return {3, Operation::greater, true};
    if (text == ">=") return {3, Operation::greater_equal, true};
    if (text == "+") return {4, Operation::add, false};
    if (text == "-") return {4, Operation::subtract, false};
    if (text == "*") return {5, Operation::multiply, false};
    return {-1, Operation::add, false};
}

class Parser {
public:
    Parser(std::string_view name, std::string_view source, Environment environment)
        : lexer_(source), environment_(environment), token_(lexer_.next()) { program_.source_name = name; }

    Program parse() {
        validate_environment();
        program_.object_type_name = environment_.object_type_name;
        program_.result = accept("void") ? ValueType::void_value : scalar_type();
        program_.function_name = identifier().text;
        expect("(");
        if (!accept(")")) {
            do {
                const auto type = scalar_type();
                local(identifier(), type);
                program_.parameters.push_back(type);
            } while (accept(","));
            expect(")");
        }
        program_.body = block();
        if (token_.kind != Token::Kind::end)
            reject(token_.position, "Only one host-bound entry function is supported.");
        if (program_.result != ValueType::void_value && !returns(program_.body))
            reject(token_.position, "Non-void entry can reach the end without a return value.");
        return std::move(program_);
    }
private:
    struct Depth {
        std::size_t& value;
        Depth(std::size_t& depth, SourcePosition position) : value(depth) {
            if (++value > 128) reject(position, "Source nesting exceeds the compiler limit of 128.");
        }
        ~Depth() { --value; }
    };
    void validate_name(std::string_view name, bool optional) const {
        if (name.empty() && !optional) reject({}, "Host member name cannot be empty.");
        for (std::size_t start = 0; start < name.size();) {
            auto end = name.find('.', start);
            if (end == std::string_view::npos) end = name.size();
            const auto segment = name.substr(start, end - start);
            if (segment.empty() || !letter(segment.front()) || reserved(segment))
                reject({}, "Host binding name requires non-reserved identifier segments.");
            for (const auto character : segment)
                if (!letter(character) && !digit(character))
                    reject({}, "Host binding name requires non-reserved identifier segments.");
            if (end == name.size()) break;
            start = end + 1;
            if (start == name.size()) reject({}, "Host binding name cannot end with '.'.");
        }
    }
    void validate_environment() const {
        validate_name(environment_.object_type_name, true);
        if (environment_.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
            environment_.methods.size() > std::numeric_limits<std::uint32_t>::max())
            reject({}, "Host member catalog exceeds its index range.");
        for (std::size_t i = 0; i < environment_.fields.size(); ++i) {
            const auto& field = environment_.fields[i];
            validate_name(field.name, false);
            if (field.type != ValueType::f32 && field.type != ValueType::u32)
                reject({}, "Host field has an unsupported scalar type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.fields[j].name == field.name) reject({}, "Ambiguous host field binding.");
        }
        for (std::size_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            validate_name(method.name, false);
            if (method.parameters.size() > std::numeric_limits<std::uint32_t>::max())
                reject({}, "Host parameter list exceeds its index range.");
            for (const auto type : method.parameters)
                if (type != ValueType::f32 && type != ValueType::u32)
                    reject({}, "Host method has an unsupported parameter type.");
            if (method.result != ValueType::void_value && method.result != ValueType::f32 && method.result != ValueType::u32)
                reject({}, "Host method has an unsupported result type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.methods[j].name == method.name) reject({}, "Ambiguous host method binding.");
        }
    }
    Token consume() { auto previous = token_; token_ = lexer_.next(); return previous; }
    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        consume();
        return true;
    }
    Token expect(std::string_view text) {
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
    void object_type() {
        const auto position = token_.position;
        const auto name = qualified_identifier();
        if (environment_.object_type_name.empty())
            reject(position, "The host environment does not expose a named object type.");
        if (name != environment_.object_type_name)
            reject(position, "Unknown object type '" + name + "'.");
    }
    bool reference_declaration() const {
        if (token_.kind != Token::Kind::identifier || reserved(token_.text)) return false;
        auto lookahead = lexer_;
        auto next = lookahead.next();
        while (next.text == ".") {
            const auto segment = lookahead.next();
            if (segment.kind != Token::Kind::identifier || reserved(segment.text)) return false;
            next = lookahead.next();
        }
        return next.kind == Token::Kind::identifier && !reserved(next.text);
    }
    NodeId add(Expression expression) {
        std::size_t height = 1;
        for (const auto child : {expression.left, expression.right, expression.receiver})
            if (child != no_node && expression_heights_[child] + 1 > height)
                height = expression_heights_[child] + 1;
        for (const auto argument : expression.arguments)
            if (expression_heights_[argument] + 1 > height) height = expression_heights_[argument] + 1;
        if (height > 128) reject(expression.position, "Expression tree exceeds the compiler limit of 128.");
        program_.expressions.push_back(std::move(expression));
        expression_heights_.push_back(height);
        return program_.expressions.size() - 1;
    }
    NodeId add(Statement statement) {
        program_.statements.push_back(std::move(statement));
        return program_.statements.size() - 1;
    }
    void unused_name(const Token& name) const {
        for (const auto index : visible_)
            if (program_.locals[index].name == name.text)
                reject(name.position, "A local or parameter with this name is already visible; shadowing is not supported.");
    }
    std::uint32_t local(const Token& name, ValueType type) {
        unused_name(name);
        if (program_.locals.size() >= std::numeric_limits<std::uint32_t>::max())
            reject(name.position, "Too many source locals.");
        const auto index = static_cast<std::uint32_t>(program_.locals.size());
        program_.locals.push_back({std::string(name.text), type});
        visible_.push_back(index);
        return index;
    }
    ValueType scalar_type() {
        if (accept("float")) return ValueType::f32;
        if (accept("uint")) return ValueType::u32;
        reject(token_.position, "Expected 'float' or 'uint' scalar type.");
    }
    bool returns(NodeId id) const {
        const auto& statement = program_.statements[id];
        switch (statement.kind) {
        case Statement::Kind::return_value: return true;
        case Statement::Kind::block:
            for (const auto child : statement.children) if (returns(child)) return true;
            return false;
        case Statement::Kind::condition:
            return statement.alternative != no_node && returns(statement.body) && returns(statement.alternative);
        default: return false; // A loop may execute zero times.
        }
    }
    void same_type(NodeId expression, ValueType required) const {
        const auto actual = program_.expressions[expression].type;
        if (actual == required || (reference_type(required) && actual == ValueType::null_reference)) return;
        if (reference_type(required) || reference_type(actual))
            reject(program_.expressions[expression].position,
                "Type mismatch; references require the same strong/weak form or null and cannot mix with scalars.");
        reject(program_.expressions[expression].position, "Type mismatch; float and uint do not convert implicitly.");
    }
    NodeId block() {
        Depth guard(depth_, token_.position);
        Statement result{Statement::Kind::block, expect("{").position, no_node, no_node, no_node, no_node, {}};
        const auto visible_before = visible_.size();
        while (token_.text != "}") {
            if (token_.kind == Token::Kind::end) reject(token_.position, "Expected '}' before end of source.");
            result.children.push_back(statement());
        }
        consume();
        result.locals.assign(visible_.begin() + visible_before, visible_.end());
        visible_.resize(visible_before);
        return add(std::move(result));
    }
    NodeId statement() {
        Depth guard(depth_, token_.position);
        const auto position = token_.position;
        if (token_.text == "{") return block();
        if (token_.text == "float" || token_.text == "uint" || token_.text == "weak" || reference_declaration()) {
            ValueType type;
            if (accept("float")) type = ValueType::f32;
            else if (accept("uint")) type = ValueType::u32;
            else {
                const auto weak = accept("weak");
                object_type();
                type = weak ? ValueType::weak_reference : ValueType::strong_reference;
            }
            const auto name = identifier();
            unused_name(name);
            const auto initialized = accept("=");
            if (!initialized && !reference_type(type)) reject(token_.position, "Expected '='.");
            const auto initializer = initialized ? expression() :
                add(Expression{Expression::Kind::null_reference, ValueType::null_reference, name.position});
            same_type(initializer, type);
            expect(";");
            const auto index = local(name, type);
            return add(Statement{Statement::Kind::declare, position, index, initializer, no_node, no_node, {}});
        }
        if (accept("if")) {
            expect("(");
            const auto condition = expression();
            if (program_.expressions[condition].type != ValueType::predicate)
                reject(position, "An if condition requires a comparison, not numeric truthiness.");
            expect(")");
            const auto body = block();
            NodeId alternative = no_node;
            if (accept("else")) alternative = token_.text == "if" ? statement() : block();
            return add(Statement{Statement::Kind::condition, position, no_node, condition, body, alternative, {}});
        }
        if (accept("while")) {
            expect("(");
            const auto condition = expression();
            if (program_.expressions[condition].type != ValueType::predicate)
                reject(position, "A while condition requires a comparison, not numeric truthiness.");
            expect(")");
            const auto body = block();
            return add(Statement{Statement::Kind::loop, position, no_node, condition, body, no_node, {}});
        }
        if (accept("return")) {
            NodeId value = no_node;
            if (program_.result == ValueType::void_value) {
                if (token_.text != ";") reject(token_.position, "This void entry cannot return a value.");
            } else {
                if (token_.text == ";") reject(token_.position, "Non-void entry requires a return value.");
                value = expression();
                same_type(value, program_.result);
            }
            expect(";");
            return add(Statement{Statement::Kind::return_value, position, no_node, value, no_node, no_node, {}});
        }
        const auto target = expression();
        if (accept("=")) {
            const auto kind = program_.expressions[target].kind;
            if (kind != Expression::Kind::local && kind != Expression::Kind::field)
                reject(position, "Assignment requires a local or a scalar field.");
            const auto value = expression();
            same_type(value, program_.expressions[target].type);
            expect(";");
            return add(Statement{Statement::Kind::assign, position, target, value, no_node, no_node, {}});
        }
        if (program_.expressions[target].kind != Expression::Kind::call)
            reject(position, "Only a native method call can be used as an expression statement.");
        expect(";");
        return add(Statement{Statement::Kind::call, position, no_node, target, no_node, no_node, {}});
    }
    NodeId expression(int minimum = 0) {
        Depth guard(depth_, token_.position);
        auto left = unary();
        for (;;) {
            const auto operation = binary_operator(token_.text);
            if (operation.precedence < minimum) return left;
            const auto symbol = consume();
            const auto right = expression(operation.precedence + 1);
            const auto type = program_.expressions[left].type;
            const auto right_type = program_.expressions[right].type;
            if (reference_type(type) || reference_type(right_type)) {
                if (!reference_type(type) || !reference_type(right_type) ||
                    (operation.operation != Operation::equal && operation.operation != Operation::not_equal))
                    reject(symbol.position, "References support only == and != with another reference or null.");
            } else if ((type != ValueType::f32 && type != ValueType::u32) || right_type != type) {
                reject(symbol.position, "Binary operands require matching float or uint types.");
            }
            left = add(Expression{Expression::Kind::binary,
                operation.comparison ? ValueType::predicate : type, symbol.position, left, right, 0, operation.operation});
        }
    }
    NodeId unary() {
        Depth guard(depth_, token_.position);
        if (token_.text == "+" || token_.text == "-") {
            const auto symbol = consume();
            const auto operand = unary();
            const auto type = program_.expressions[operand].type;
            if (type != ValueType::f32 && type != ValueType::u32)
                reject(symbol.position, "Unary arithmetic requires a numeric operand.");
            if (symbol.text == "-" && type != ValueType::f32)
                reject(symbol.position, "Unary minus requires float; use uint subtraction for modulo arithmetic.");
            return add(Expression{Expression::Kind::unary, type, symbol.position, operand, no_node, 0,
                symbol.text == "+" ? Operation::positive : Operation::negate});
        }
        return postfix();
    }
    NodeId postfix() {
        auto receiver = primary();
        while (accept(".")) {
            const auto position = program_.expressions[receiver].position;
            if (program_.expressions[receiver].type != ValueType::strong_reference)
                reject(position, "Member access requires a strong reference; acquire weak references first.");
            const auto member = qualified_identifier();
            if (accept("(")) {
                const MethodBinding* method{};
                for (const auto& candidate : environment_.methods) if (candidate.name == member) method = &candidate;
                if (!method) reject(position, "Unknown native method '" + member + "'.");
                std::vector<NodeId> arguments;
                if (!accept(")")) {
                    do {
                        if (arguments.size() >= method->parameters.size())
                            reject(token_.position, "Native method argument count mismatch.");
                        const auto argument = expression();
                        same_type(argument, method->parameters[arguments.size()]);
                        arguments.push_back(argument);
                    } while (accept(","));
                    expect(")");
                }
                if (arguments.size() != method->parameters.size())
                    reject(position, "Native method argument count mismatch.");
                Expression call{Expression::Kind::call, method->result, position,
                    no_node, no_node, static_cast<std::uint32_t>(method - environment_.methods.data()), Operation::add, receiver};
                call.arguments = std::move(arguments);
                receiver = add(std::move(call));
            } else {
                const FieldBinding* field{};
                for (const auto& candidate : environment_.fields) if (candidate.name == member) field = &candidate;
                if (!field) reject(position, "Unknown receiver field '" + member + "'.");
                receiver = add(Expression{Expression::Kind::field, field->type, position,
                    no_node, no_node, static_cast<std::uint32_t>(field - environment_.fields.data()), Operation::add, receiver});
            }
        }
        return receiver;
    }
    NodeId primary() {
        const auto token = token_;
        if (accept("(")) {
            const auto result = expression();
            expect(")");
            return result;
        }
        if (token.kind == Token::Kind::number) {
            consume();
            if (token.text.find_first_of(".eE") != std::string_view::npos) {
                float value{};
                const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != token.text.data() + token.text.size() || !std::isfinite(value))
                    reject(token.position, "Float literal is out of range or invalid.");
                return add(Expression{Expression::Kind::literal, ValueType::f32, token.position,
                    no_node, no_node, std::bit_cast<std::uint32_t>(value)});
            }
            std::uint32_t value{};
            const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.text.data() + token.text.size())
                reject(token.position, "Uint literal is outside 0..4294967295.");
            return add(Expression{Expression::Kind::literal, ValueType::u32, token.position, no_node, no_node, value});
        }
        if (token.kind != Token::Kind::identifier) reject(token.position, "Expected an expression.");
        consume();
        if (token.text == "self")
            return add(Expression{Expression::Kind::receiver, ValueType::strong_reference, token.position});
        if (token.text == "null")
            return add(Expression{Expression::Kind::null_reference, ValueType::null_reference, token.position});
        if (token.text == "new") {
            object_type();
            expect("(");
            if (token_.text != ")") reject(token_.position, "Object creation does not accept constructor arguments.");
            consume();
            return add(Expression{Expression::Kind::create_reference, ValueType::strong_reference, token.position});
        }
        if (token.text == "observe" || token.text == "try_acquire") {
            const auto observe = token.text == "observe";
            expect("(");
            if (token_.text == ")") reject(token_.position, "Reference operation requires one argument.");
            const auto argument = expression();
            const auto required = observe ? ValueType::strong_reference : ValueType::weak_reference;
            if (program_.expressions[argument].type != required)
                reject(program_.expressions[argument].position, observe ?
                    "observe requires a strong reference argument." : "try_acquire requires a weak reference argument.");
            if (token_.text == ",") reject(token_.position, "Reference operation requires exactly one argument.");
            expect(")");
            return add(Expression{observe ? Expression::Kind::observe_reference : Expression::Kind::acquire_reference,
                observe ? ValueType::weak_reference : ValueType::strong_reference, token.position, argument});
        }
        for (auto iterator = visible_.rbegin(); iterator != visible_.rend(); ++iterator) {
            const auto& local_value = program_.locals[*iterator];
            if (local_value.name == token.text)
                return add(Expression{Expression::Kind::local, local_value.type, token.position,
                    no_node, no_node, *iterator});
        }
        reject(token.position, "Unknown identifier '" + std::string(token.text) + "'.");
    }

    Lexer lexer_;
    Environment environment_;
    Token token_;
    Program program_;
    std::vector<std::uint32_t> visible_;
    std::vector<std::size_t> expression_heights_;
    std::size_t depth_{};
};
}

std::expected<Program, Diagnostic> compile_source(
    std::string_view source_name, std::string_view source, Environment environment) {
    try {
        return Parser(source_name, source, environment).parse();
    } catch (Rejected& rejected) {
        return std::unexpected(std::move(rejected.diagnostic));
    }
}

}
