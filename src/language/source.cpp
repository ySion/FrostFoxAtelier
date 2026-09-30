#include "language/source.hpp"

#include <bit>
#include <algorithm>
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
    return value == "void" || value == "float" || value == "uint" || value == "int" || value == "const" || value == "if" ||
        value == "else" || value == "while" || value == "return" || value == "self" ||
        value == "weak" || value == "null" || value == "new" || value == "observe" || value == "try_acquire" ||
        value == "async" || value == "yield";
}
bool reference_type(ValueType type) noexcept {
    return type == ValueType::strong_reference || type == ValueType::weak_reference ||
        type == ValueType::null_reference;
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
        if (first == ':') {
            advance();
            if (peek() != ':') reject(start, "Enum qualification requires '::'.");
            advance();
            return token(Token::Kind::punctuation, start);
        }
        if (std::string_view("(){}[];.,+-*&").find(first) != std::string_view::npos) {
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
        program_.value_layouts.assign(environment_.values.begin(), environment_.values.end());
        program_.is_coroutine = accept("async");
        program_.result = accept("void") ? ValueType::void_value : scalar_type();
        program_.function_name = identifier().text;
        expect("(");
        if (!accept(")")) {
            do {
                const auto position = token_.position;
                const bool read_only = accept("const");
                const auto type = parameter_type();
                if (reference_type(type)) reject(position, "Object reference parameters are not supported.");
                const bool reference = accept("&");
                if (read_only && !reference) reject(position, "const parameters require a reference in this source slice.");
                const auto passing = reference ? (read_only ? ParameterPassing::read_only : ParameterPassing::reference) : ParameterPassing::value;
                if (program_.is_coroutine && reference) reject(position, "async parameters cannot borrow caller storage.");
                if (aggregate(type)) {
                    if (!reference) reject(position, "Native values require a borrowed parameter; use Type& or const Type&.");
                    if (type.kind == ValueType::string && !read_only) reject(position, "String borrowing requires const String&.");
                    program_.uses_native_values = true;
                }
                const auto index = local(identifier(), type);
                program_.locals[index].read_only = read_only;
                program_.parameters.push_back({type, passing});
            } while (accept(","));
            expect(")");
        }
        program_.body = block();
        if (program_.uses_native_values) {
            if (program_.is_coroutine) reject({}, "Owning native values currently require synchronous execution.");
            std::vector<bool> allowed_receivers(program_.expressions.size());
            for (const auto& node : program_.expressions) {
                if (node.kind != Expression::Kind::call) continue;
                if (!environment_.methods[node.value].preserves_borrows ||
                    program_.expressions[node.receiver].kind != Expression::Kind::receiver)
                    reject(node.position, "Native value scopes require a BorrowSafe method on self.");
                allowed_receivers[node.receiver] = true;
            }
            for (std::size_t i = 0; i < program_.expressions.size(); ++i) {
                const auto& node = program_.expressions[i];
                if (reference_type(node.type) && !allowed_receivers[i])
                    reject(node.position, "Native value scopes cannot perform object operations.");
            }
            for (const auto& value : program_.locals)
                if (reference_type(value.type)) reject({}, "A native value scope cannot own Runtime object references.");
        }
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
    bool scalar(ValueType type) const {
        return type == ValueType::f32 || type == ValueType::u32 || type == ValueType::i32 ||
            (type.kind == ValueType::enumeration && type.type_index < environment_.enumerations.size());
    }
    bool supported_value(ValueType type) const {
        return scalar(type) || (aggregate(type) && type.type_index < environment_.values.size() &&
            environment_.values[type.type_index].kind == type.kind);
    }
    std::size_t value_size(ValueType type) const {
        return aggregate(type) ? environment_.values[type.type_index].size : 4;
    }
    std::size_t value_alignment(ValueType type) const {
        return aggregate(type) ? environment_.values[type.type_index].alignment : 4;
    }
    void validate_environment() const {
        validate_name(environment_.object_type_name, true);
        for (std::size_t i = 0; i < environment_.enumerations.size(); ++i) {
            const auto& enumeration = environment_.enumerations[i];
            validate_name(enumeration.name, false);
            if (enumeration.underlying != ValueType::i32 && enumeration.underlying != ValueType::u32)
                reject({}, "Enum requires i32/u32 representation.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.enumerations[j].name == enumeration.name) reject({}, "Duplicate enum type name.");
            for (std::size_t j = 0; j < enumeration.values.size(); ++j) {
                validate_name(enumeration.values[j].name, false);
                if (enumeration.values[j].name.find('.') != std::string_view::npos) reject({}, "Enum item must be a simple identifier.");
                for (std::size_t k = 0; k < j; ++k)
                    if (enumeration.values[k].name == enumeration.values[j].name) reject({}, "Duplicate enum item name.");
            }
        }
        if (environment_.values.size() > std::numeric_limits<std::uint32_t>::max()) reject({}, "Value catalog is too large.");
        for (std::size_t i = 0; i < environment_.values.size(); ++i) {
            const auto& value = environment_.values[i];
            validate_name(value.name, false);
            if (!aggregate({value.kind, static_cast<std::uint32_t>(i)}) || !value.size ||
                !std::has_single_bit(value.alignment) || value.size % value.alignment || value.size > static_cast<std::size_t>(PTRDIFF_MAX))
                reject({}, "Invalid native value layout.");
            if ((value.kind == ValueType::array || value.kind == ValueType::string) && !value.sequence_view)
                reject({}, "Sequence layout requires its native view operation.");
            if (value.kind == ValueType::array && !supported_value(value.element)) reject({}, "Invalid array element type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.values[j].name == value.name) reject({}, "Duplicate native value name.");
            for (std::size_t j = 0; j < value.fields.size(); ++j) {
                const auto& field = value.fields[j];
                validate_name(field.name, false);
                if (value.kind != ValueType::record || field.name.find('.') != std::string::npos || !supported_value(field.type) ||
                    field.offset > value.size || value_size(field.type) > value.size - field.offset ||
                    field.offset % value_alignment(field.type) || value.alignment < value_alignment(field.type))
                    reject({}, "Invalid direct native field layout.");
                for (std::size_t k = 0; k < j; ++k)
                    if (value.fields[k].name == field.name) reject({}, "Duplicate native value field.");
            }
        }
        if (environment_.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
            environment_.methods.size() > std::numeric_limits<std::uint32_t>::max())
            reject({}, "Host member catalog exceeds its index range.");
        for (std::size_t i = 0; i < environment_.fields.size(); ++i) {
            const auto& field = environment_.fields[i];
            validate_name(field.name, false);
            if (!scalar(field.type))
                reject({}, "Host field has an unsupported scalar type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.fields[j].name == field.name) reject({}, "Ambiguous host field binding.");
        }
        for (std::size_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            validate_name(method.name, false);
            if (method.parameters.size() > std::numeric_limits<std::uint32_t>::max())
                reject({}, "Host parameter list exceeds its index range.");
            for (const auto& parameter : method.parameters)
                if (!supported_value(parameter.type) || (aggregate(parameter.type) && parameter.passing == ParameterPassing::value) || (parameter.passing != ParameterPassing::value &&
                    parameter.passing != ParameterPassing::read_only && parameter.passing != ParameterPassing::reference))
                    reject({}, "Host method has an unsupported parameter type or passing mode.");
            if (method.result != ValueType::void_value && !scalar(method.result))
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
        if (token_.text == "Array" && next.text == "<") return true;
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
        if (accept("int")) return ValueType::i32;
        const auto position = token_.position;
        const auto name = qualified_identifier();
        for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i)
            if (environment_.enumerations[i].name == name) return {ValueType::enumeration, i};
        reject(position, "Expected float, uint, int or a registered enum type.");
    }
    ValueType parameter_type() {
        Depth guard(depth_, token_.position);
        if (token_.text == "float" || token_.text == "uint" || token_.text == "int") return scalar_type();
        const auto position = token_.position;
        if (accept("Array")) {
            expect("<"); const auto element = parameter_type(); expect(">");
            for (std::uint32_t i = 0; i < environment_.values.size(); ++i)
                if (environment_.values[i].kind == ValueType::array && environment_.values[i].element == element)
                    return {ValueType::array, i};
            reject(position, "This Array element type has not been registered in the Runtime.");
        }
        const auto name = qualified_identifier();
        if (name == environment_.object_type_name) return ValueType::strong_reference;
        for (std::uint32_t i = 0; i < environment_.values.size(); ++i) {
            const auto& value = environment_.values[i];
            if (value.name == name || (name == "String" && value.kind == ValueType::string)) return {value.kind, i};
        }
        for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i)
            if (environment_.enumerations[i].name == name) return {ValueType::enumeration, i};
        reject(position, "Unknown registered value type '" + name + "'.");
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
    bool writable(NodeId id) const {
        const auto& expression = program_.expressions[id];
        if (aggregate(expression.type)) return expression.kind == Expression::Kind::local &&
            expression.value >= program_.parameters.size() && !program_.locals[expression.value].read_only;
        return writable_storage(id);
    }
    bool writable_storage(NodeId id) const {
        const auto& node = program_.expressions[id];
        if (node.kind == Expression::Kind::field) return true;
        if (node.kind == Expression::Kind::local) return !program_.locals[node.value].read_only;
        if (node.kind == Expression::Kind::index && program_.expressions[node.left].type.kind == ValueType::string) return false;
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index) return writable_storage(node.left);
        return false;
    }
    bool owned_storage(NodeId id) const {
        const auto& node = program_.expressions[id];
        if (node.kind == Expression::Kind::local) return node.value >= program_.parameters.size() && !program_.locals[node.value].read_only;
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index) return owned_storage(node.left);
        return false;
    }
    bool integer_constant(NodeId id) const {
        const auto& node = program_.expressions[id];
        if (node.type != ValueType::u32) return false;
        if (node.kind == Expression::Kind::literal) return node.value <= 2147483647u;
        return (node.kind == Expression::Kind::unary && integer_constant(node.left)) ||
            (node.kind == Expression::Kind::binary && integer_constant(node.left) && integer_constant(node.right));
    }
    void signed_constant(NodeId id) {
        auto& node = program_.expressions[id];
        node.type = ValueType::i32;
        if (node.left != no_node) signed_constant(node.left);
        if (node.right != no_node) signed_constant(node.right);
    }
    void same_type(NodeId expression, ValueType required) {
        if (required == ValueType::i32 && integer_constant(expression)) signed_constant(expression);
        const auto actual = program_.expressions[expression].type;
        if (actual == required || (reference_type(required) && actual == ValueType::null_reference)) return;
        if (reference_type(required) || reference_type(actual))
            reject(program_.expressions[expression].position,
                "Type mismatch; references require the same strong/weak form or null and cannot mix with scalars.");
        reject(program_.expressions[expression].position, "Type mismatch; typed numeric values and distinct enum types do not convert implicitly.");
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
        if (token_.text == "float" || token_.text == "uint" || token_.text == "int" || token_.text == "weak" || reference_declaration()) {
            const bool weak = accept("weak");
            auto type = parameter_type();
            if (weak) {
                if (type != ValueType::strong_reference) reject(position, "weak requires an object type.");
                type = ValueType::weak_reference;
            }
            if (aggregate(type)) {
                if (type.kind == ValueType::record || !environment_.values[type.type_index].can_own)
                    reject(position, "Owned locals require String or Array with borrow-preserving value operations.");
                program_.uses_native_values = true;
            }
            const auto name = identifier();
            unused_name(name);
            const auto initialized = accept("=");
            if (!initialized && !reference_type(type) && !aggregate(type)) reject(token_.position, "Expected '='.");
            NodeId initializer = no_node;
            if (initialized) initializer = expression();
            else if (reference_type(type)) initializer = add(Expression{Expression::Kind::null_reference, ValueType::null_reference, name.position});
            if (initializer != no_node) same_type(initializer, type);
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
        if (accept("yield")) {
            if (!program_.is_coroutine) reject(position, "yield requires an async entry.");
            expect(";");
            return add(Statement{Statement::Kind::yield, position, no_node, no_node, no_node, no_node, {}});
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
            if (!writable(target))
                reject(position, "Assignment requires a writable local or field.");
            const auto value = expression();
            same_type(value, program_.expressions[target].type);
            expect(";");
            return add(Statement{Statement::Kind::assign, position, target, value, no_node, no_node, {}});
        }
        if (program_.expressions[target].kind != Expression::Kind::call && program_.expressions[target].kind != Expression::Kind::sequence_mutation)
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
            if (program_.expressions[left].type == ValueType::i32 && integer_constant(right)) signed_constant(right);
            if (program_.expressions[right].type == ValueType::i32 && integer_constant(left)) signed_constant(left);
            const auto type = program_.expressions[left].type;
            const auto right_type = program_.expressions[right].type;
            if (reference_type(type) || reference_type(right_type)) {
                if (!reference_type(type) || !reference_type(right_type) ||
                    (operation.operation != Operation::equal && operation.operation != Operation::not_equal))
                    reject(symbol.position, "References support only == and != with another reference or null.");
            } else if (!scalar(type) || right_type != type ||
                (type.kind == ValueType::enumeration && operation.operation != Operation::equal && operation.operation != Operation::not_equal)) {
                reject(symbol.position, "Binary operands require matching numeric types; enums support equality only.");
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
            if (symbol.text == "-" && program_.expressions[operand].kind == Expression::Kind::literal &&
                program_.expressions[operand].type == ValueType::u32 && program_.expressions[operand].value == 2147483648u)
                return add(Expression{Expression::Kind::literal, ValueType::i32, symbol.position, no_node, no_node, 2147483648u});
            if (symbol.text == "-" && integer_constant(operand)) signed_constant(operand);
            const auto type = program_.expressions[operand].type;
            if (type != ValueType::f32 && type != ValueType::u32 && type != ValueType::i32)
                reject(symbol.position, "Unary arithmetic requires a numeric operand.");
            if (symbol.text == "-" && type != ValueType::f32 && type != ValueType::i32)
                reject(symbol.position, "Unary minus requires float or int; use uint subtraction for modulo arithmetic.");
            return add(Expression{Expression::Kind::unary, type, symbol.position, operand, no_node, 0,
                symbol.text == "+" ? Operation::positive : Operation::negate});
        }
        return postfix();
    }
    NodeId postfix() {
        auto receiver = primary();
        while (token_.text == "." || token_.text == "[") {
            const auto position = program_.expressions[receiver].position;
            const auto type = program_.expressions[receiver].type;
            if (accept("[")) {
                if (type.kind != ValueType::array && type.kind != ValueType::string) reject(position, "Indexing requires Array or String.");
                const auto index = expression(); expect("]");
                if (program_.expressions[index].type != ValueType::u32 && program_.expressions[index].type != ValueType::i32)
                    reject(position, "A sequence index requires uint or int.");
                const auto element = type.kind == ValueType::string ? ValueType{ValueType::u32} : environment_.values[type.type_index].element;
                receiver = add(Expression{Expression::Kind::index, element, position, receiver, index});
                continue;
            }
            expect(".");
            if (aggregate(type)) {
                const auto member = identifier();
                if (type.kind == ValueType::array || type.kind == ValueType::string) {
                    expect("(");
                    if (member.text == "size") {
                        expect(")");
                        receiver = add(Expression{Expression::Kind::length, ValueType::u32, position, receiver});
                    } else {
                        if (!owned_storage(receiver) || !environment_.values[type.type_index].can_own || !environment_.values[type.type_index].can_mutate)
                            reject(member.position, "Structural mutation requires a locally owned sequence.");
                        SequenceMutation operation;
                        if (member.text == "reserve") operation = SequenceMutation::reserve;
                        else if (member.text == "resize" && type.kind == ValueType::array) operation = SequenceMutation::resize;
                        else if (member.text == "clear") operation = SequenceMutation::clear;
                        else if (member.text == "add" && type.kind == ValueType::array) operation = SequenceMutation::add;
                        else if (member.text == "append") operation = SequenceMutation::append;
                        else reject(member.position, "Unknown native sequence operation.");
                        NodeId input = no_node;
                        if (operation != SequenceMutation::clear) {
                            input = expression();
                            if (operation == SequenceMutation::reserve || operation == SequenceMutation::resize) {
                                const auto input_type = program_.expressions[input].type;
                                if (input_type != ValueType::u32 && input_type != ValueType::i32) reject(member.position, "Sequence capacity requires uint or int.");
                            } else same_type(input, operation == SequenceMutation::add ? environment_.values[type.type_index].element : type);
                        }
                        expect(")");
                        Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver, input};
                        mutation.mutation = operation;
                        receiver = add(std::move(mutation));
                    }
                } else {
                    const auto& fields = environment_.values[type.type_index].fields;
                    const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& candidate) { return candidate.name == member.text; });
                    if (field == fields.end()) reject(member.position, "Field has no published direct native layout.");
                    Expression access{Expression::Kind::value_field, field->type, position, receiver};
                    access.offset = field->offset;
                    receiver = add(std::move(access));
                }
                continue;
            }
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
                        const auto& parameter = method->parameters[arguments.size()];
                        same_type(argument, parameter.type);
                        if (parameter.passing == ParameterPassing::reference && !writable_storage(argument))
                            reject(program_.expressions[argument].position, "Writable reference requires a mutable lvalue.");
                        arguments.push_back(argument);
                    } while (accept(","));
                    expect(")");
                }
                if (arguments.size() != method->parameters.size())
                    reject(position, "Native method argument count mismatch.");
                Expression call{Expression::Kind::call, method->result, position,
                    no_node, no_node, static_cast<std::uint32_t>(method - environment_.methods.data()), Operation::add, receiver};
                call.arguments = std::move(arguments);
                for (const auto& parameter : method->parameters) call.argument_passing.push_back(parameter.passing);
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
        if (token.kind == Token::Kind::string) {
            consume();
            std::string value;
            for (std::size_t i = 1; i + 1 < token.text.size(); ++i) {
                char c = token.text[i];
                if (c == '\\') {
                    c = token.text[++i];
                    switch (c) {
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
            for (std::uint32_t i = 0; i < environment_.values.size(); ++i) {
                if (environment_.values[i].kind != ValueType::string || !environment_.values[i].can_own || !environment_.values[i].can_mutate) continue;
                program_.uses_native_values = true;
                const auto index = static_cast<std::uint32_t>(program_.strings.size());
                program_.strings.push_back(std::move(value));
                return add(Expression{Expression::Kind::string_literal, {ValueType::string, i}, token.position, no_node, no_node, index});
            }
            reject(token.position, "String literals require the registered foundation String.");
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
        {
            auto lookahead = lexer_;
            auto next = lookahead.next();
            std::string name(token.text);
            while (next.text == ".") {
                const auto segment = lookahead.next();
                name += "."; name += segment.text;
                next = lookahead.next();
            }
            if (next.text == "::") {
                const auto name_actual = qualified_identifier();
                expect("::");
                const auto item = identifier();
                for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i) {
                    const auto& enumeration = environment_.enumerations[i];
                    if (enumeration.name != name_actual) continue;
                    for (const auto& value : enumeration.values)
                        if (value.name == item.text) return add(Expression{Expression::Kind::literal,
                            {ValueType::enumeration, i}, token.position, no_node, no_node, value.bits});
                    reject(item.position, "Unknown enumerator.");
                }
                reject(token.position, "Unknown enum type.");
            }
        }
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
