#include "language/source.hpp"
#include "language/lexer.hpp"
#include "language/declarations.hpp"

#include <bit>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace atelier::language {
namespace {
using namespace detail;

bool reference_type(ValueType type) noexcept {
    return type == ValueType::strong_reference || type == ValueType::weak_reference ||
        type == ValueType::null_reference;
}
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

class Parser : private Tokens {
public:
    Parser(std::string_view name, std::string_view source, Environment environment)
        : Tokens(source), environment_(environment) { module_.source_name = name; }

    Program parse(std::string_view entry_name) {
        validate_environment();
        module_.object_type_name = environment_.object_type_name;
        module_.value_layouts.assign(environment_.values.begin(), environment_.values.end());
        struct Body { Lexer lexer; Token token; };
        std::vector<Body> bodies;
        std::vector<std::string> type_names;
        while (token_.kind != Token::Kind::end) {
            if (token_.text == "struct" || token_.text == "enum") {
                auto declaration = parse_type_declaration(*this);
                if (type_names.size() == 1024) reject(declaration.position, "A source module supports at most 1024 type declarations.");
                if (std::ranges::find(type_names, declaration.name) != type_names.end())
                    reject(declaration.position, "Duplicate source type name.");
                validate_declaration(declaration);
                type_names.push_back(std::move(declaration.name));
                continue;
            }
            if (module_.functions.size() >= 1024) reject(token_.position, "A source module supports at most 1024 functions.");
            function_ = {};
            function_.position = token_.position;
            visible_.clear();
            signature();
            for (const auto& previous : module_.functions)
                if (previous.function_name == function_.function_name) reject(function_.position, "Duplicate source function name.");
            bodies.push_back({lexer_, token_});
            module_.functions.push_back(std::move(function_));
            expect("{");
            std::size_t braces = 1;
            while (braces != 0) {
                if (token_.kind == Token::Kind::end) reject(token_.position, "Expected '}' to close the function body.");
                if (token_.text == "{") ++braces;
                if (token_.text == "}") --braces;
                consume();
            }
        }
        if (module_.functions.empty()) reject(token_.position, "Source module requires an entry function.");
        for (const auto& function : module_.functions)
            if (std::ranges::find(type_names, function.function_name) != type_names.end())
                reject(function.position, "A source function and type cannot have the same name.");
        if (entry_name.empty() && module_.functions.size() != 1)
            reject({}, "A multi-function module requires an explicit host entry name.");
        bool found = entry_name.empty();
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            function_ = module_.functions[i];
            if (function_.function_name == entry_name) { module_.entry = static_cast<std::uint32_t>(i); found = true; }
            lexer_ = bodies[i].lexer; token_ = bodies[i].token;
            visible_.clear(); expression_heights_.clear();
            for (std::uint32_t j = 0; j < function_.parameters.size(); ++j) visible_.push_back(j);
            function_.body = block();
            if (function_.result != ValueType::void_value && !returns(function_.body))
                reject(function_.position, "Non-void function can reach the end without a return value.");
            module_.functions[i] = std::move(function_);
        }
        if (!found) reject({}, "Selected entry function was not declared in this module.");
        analyze_calls();
        return std::move(module_);
    }

private:

    ValueType declared_type(std::string_view name, SourcePosition position) const {
        if (name == "float") return ValueType::f32;
        if (name == "uint") return ValueType::u32;
        if (name == "int") return ValueType::i32;
        for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i)
            if (environment_.enumerations[i].name == name) return {ValueType::enumeration, i};
        for (std::uint32_t i = 0; i < environment_.values.size(); ++i)
            if (environment_.values[i].name == name) return {environment_.values[i].kind, i};
        reject(position, "Source type declaration has no prepared Runtime layout: " + std::string(name));
    }
    void validate_declaration(const TypeDeclaration& declaration) const {
        const auto type = declared_type(declaration.name, declaration.position);
        if (declaration.enumeration) {
            if (type.kind != ValueType::enumeration) reject(declaration.position, "Source enum conflicts with its Runtime type.");
            const auto& layout = environment_.enumerations[type.type_index];
            if (layout.underlying != (declaration.signed_enum ? ValueType::i32 : ValueType::u32) ||
                layout.values.size() != declaration.enumerators.size())
                reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
            for (std::size_t i = 0; i < layout.values.size(); ++i)
                if (layout.values[i].name != declaration.enumerators[i].first || layout.values[i].bits != declaration.enumerators[i].second)
                    reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
        } else {
            if (type.kind != ValueType::record) reject(declaration.position, "Source struct conflicts with its Runtime type.");
            const auto& layout = environment_.values[type.type_index];
            if (layout.fields.size() != declaration.fields.size())
                reject(declaration.position, "Source struct differs from its prepared Runtime definition.");
            for (std::size_t i = 0; i < layout.fields.size(); ++i)
                if (layout.fields[i].name != declaration.fields[i].first ||
                    layout.fields[i].type != declared_type(declaration.fields[i].second, declaration.position))
                    reject(declaration.position, "Source struct differs from its prepared Runtime definition.");
        }
    }
    void signature() {
        function_.is_coroutine = accept("async");
        function_.result = accept("void") ? ValueType::void_value : scalar_type();
        function_.function_name = identifier().text;
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
                if (function_.is_coroutine && reference) reject(position, "async parameters cannot borrow caller storage.");
                if (aggregate(type)) {
                    if (!reference) reject(position, "Native values require a borrowed parameter; use Type& or const Type&.");
                    if (type.kind == ValueType::string && !read_only) reject(position, "String borrowing requires const String&.");
                    function_.uses_scoped_values = true;
                }
                const auto index = local(identifier(), type);
                function_.locals[index].read_only = read_only;
                function_.parameters.push_back({type, passing});
            } while (accept(","));
            expect(")");
        }
    }
    void analyze_calls() {
        // Summarize direct effects before propagating callee effects bottom-up.
        for (auto& function : module_.functions) {
            std::vector<bool> allowed(function.expressions.size());
            function.preserves_borrows = true;
            for (const auto& node : function.expressions) {
                if (node.kind == Expression::Kind::receiver) function.requires_receiver = true;
                if (node.kind != Expression::Kind::call) continue;
                if (!environment_.methods[node.value].preserves_borrows ||
                    function.expressions[node.receiver].kind != Expression::Kind::receiver)
                    function.preserves_borrows = false;
                else allowed[node.receiver] = true;
            }
            for (std::size_t i = 0; i < function.expressions.size(); ++i)
                if (reference_type(function.expressions[i].type) && !allowed[i]) function.preserves_borrows = false;
            for (const auto& local : function.locals)
                if (reference_type(local.type)) function.preserves_borrows = false;
        }
        enum class State { fresh, active, complete };
        std::vector<State> states(module_.functions.size());
        struct Cursor { std::uint32_t function; std::size_t next{}; };
        std::vector<Cursor> stack;
        for (std::uint32_t root = 0; root < module_.functions.size(); ++root) {
            if (states[root] != State::fresh) continue;
            states[root] = State::active; stack.push_back({root});
            while (!stack.empty()) {
                auto& cursor = stack.back();
                auto& function = module_.functions[cursor.function];
                while (cursor.next < function.expressions.size() &&
                    function.expressions[cursor.next].kind != Expression::Kind::source_call) ++cursor.next;
                if (cursor.next < function.expressions.size()) {
                    const auto& call = function.expressions[cursor.next];
                    if (states[call.value] == State::active)
                        reject(call.position, "Recursive source calls are not supported in this module stage.");
                    if (states[call.value] == State::fresh) {
                        states[call.value] = State::active; stack.push_back({call.value}); continue;
                    }
                    const auto& callee = module_.functions[call.value];
                    function.uses_scoped_values |= callee.uses_scoped_values;
                    function.requires_receiver |= callee.requires_receiver;
                    function.preserves_borrows &= callee.preserves_borrows;
                    ++cursor.next;
                } else {
                    if (function.uses_scoped_values && (function.is_coroutine || !function.preserves_borrows))
                        reject(function.position, "Scoped value execution in '" + function.function_name +
                            "' reaches suspension, object operations or a native call without BorrowSafe.");
                    states[cursor.function] = State::complete; stack.pop_back();
                }
            }
        }
        module_.reachable.resize(module_.functions.size());
        std::vector<std::uint32_t> pending{module_.entry};
        while (!pending.empty()) {
            const auto index = pending.back(); pending.pop_back();
            if (module_.reachable[index]) continue;
            module_.reachable[index] = true;
            for (const auto& call : module_.functions[index].expressions)
                if (call.kind == Expression::Kind::source_call) pending.push_back(call.value);
        }
    }
    std::vector<NodeId> call_arguments(std::span<const ParameterType> parameters) {
        std::vector<NodeId> arguments;
        if (!accept(")")) {
            do {
                if (arguments.size() >= parameters.size()) reject(token_.position, "Call argument count mismatch.");
                const auto argument = expression();
                const auto& parameter = parameters[arguments.size()];
                same_type(argument, parameter.type);
                if (parameter.passing == ParameterPassing::reference && !writable_storage(argument))
                    reject(function_.expressions[argument].position, "Writable reference requires a mutable lvalue.");
                arguments.push_back(argument);
            } while (accept(","));
            expect(")");
        }
        if (arguments.size() != parameters.size()) reject(token_.position, "Call argument count mismatch.");
        return arguments;
    }
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
        function_.expressions.push_back(std::move(expression));
        expression_heights_.push_back(height);
        return function_.expressions.size() - 1;
    }
    NodeId add(Statement statement) {
        function_.statements.push_back(std::move(statement));
        return function_.statements.size() - 1;
    }
    void unused_name(const Token& name) const {
        for (const auto index : visible_)
            if (function_.locals[index].name == name.text)
                reject(name.position, "A local or parameter with this name is already visible; shadowing is not supported.");
    }
    std::uint32_t local(const Token& name, ValueType type) {
        unused_name(name);
        if (function_.locals.size() >= std::numeric_limits<std::uint32_t>::max())
            reject(name.position, "Too many source locals.");
        const auto index = static_cast<std::uint32_t>(function_.locals.size());
        function_.locals.push_back({std::string(name.text), type});
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
        const auto& statement = function_.statements[id];
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
        const auto& expression = function_.expressions[id];
        if (aggregate(expression.type)) return owned_storage(id) &&
            (expression.kind == Expression::Kind::local || expression.type.kind == ValueType::record);
        return writable_storage(id);
    }
    bool writable_storage(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::field) return true;
        if (node.kind == Expression::Kind::local) return !function_.locals[node.value].read_only;
        if (node.kind == Expression::Kind::index && function_.expressions[node.left].type.kind == ValueType::string) return false;
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index) return writable_storage(node.left);
        return false;
    }
    bool owned_storage(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::local) return node.value >= function_.parameters.size() && !function_.locals[node.value].read_only;
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index) return owned_storage(node.left);
        return false;
    }
    bool integer_constant(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.type != ValueType::u32) return false;
        if (node.kind == Expression::Kind::literal) return node.value <= 2147483647u;
        return (node.kind == Expression::Kind::unary && integer_constant(node.left)) ||
            (node.kind == Expression::Kind::binary && integer_constant(node.left) && integer_constant(node.right));
    }
    void signed_constant(NodeId id) {
        auto& node = function_.expressions[id];
        node.type = ValueType::i32;
        if (node.left != no_node) signed_constant(node.left);
        if (node.right != no_node) signed_constant(node.right);
    }
    void same_type(NodeId expression, ValueType required) {
        if (required == ValueType::i32 && integer_constant(expression)) signed_constant(expression);
        const auto actual = function_.expressions[expression].type;
        if (actual == required || (reference_type(required) && actual == ValueType::null_reference)) return;
        if (reference_type(required) || reference_type(actual))
            reject(function_.expressions[expression].position,
                "Type mismatch; references require the same strong/weak form or null and cannot mix with scalars.");
        reject(function_.expressions[expression].position, "Type mismatch; typed numeric values and distinct enum types do not convert implicitly.");
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
                if (!environment_.values[type.type_index].can_own)
                    reject(position, "Owned locals require values with borrow-preserving copy and lifecycle operations.");
                function_.uses_scoped_values = true;
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
            if (function_.expressions[condition].type != ValueType::predicate)
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
            if (function_.expressions[condition].type != ValueType::predicate)
                reject(position, "A while condition requires a comparison, not numeric truthiness.");
            expect(")");
            const auto body = block();
            return add(Statement{Statement::Kind::loop, position, no_node, condition, body, no_node, {}});
        }
        if (accept("yield")) {
            if (!function_.is_coroutine) reject(position, "yield requires an async entry.");
            expect(";");
            return add(Statement{Statement::Kind::yield, position, no_node, no_node, no_node, no_node, {}});
        }
        if (accept("return")) {
            NodeId value = no_node;
            if (function_.result == ValueType::void_value) {
                if (token_.text != ";") reject(token_.position, "This void entry cannot return a value.");
            } else {
                if (token_.text == ";") reject(token_.position, "Non-void entry requires a return value.");
                value = expression();
                same_type(value, function_.result);
            }
            expect(";");
            return add(Statement{Statement::Kind::return_value, position, no_node, value, no_node, no_node, {}});
        }
        const auto target = expression();
        if (accept("=")) {
            if (!writable(target))
                reject(position, "Assignment requires a writable local or field.");
            const auto value = expression();
            same_type(value, function_.expressions[target].type);
            expect(";");
            return add(Statement{Statement::Kind::assign, position, target, value, no_node, no_node, {}});
        }
        if (function_.expressions[target].kind != Expression::Kind::call && function_.expressions[target].kind != Expression::Kind::source_call && function_.expressions[target].kind != Expression::Kind::sequence_mutation)
            reject(position, "Only calls and sequence modifications can be used as expression statements.");
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
            if (function_.expressions[left].type == ValueType::i32 && integer_constant(right)) signed_constant(right);
            if (function_.expressions[right].type == ValueType::i32 && integer_constant(left)) signed_constant(left);
            const auto type = function_.expressions[left].type;
            const auto right_type = function_.expressions[right].type;
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
            if (symbol.text == "-" && function_.expressions[operand].kind == Expression::Kind::literal &&
                function_.expressions[operand].type == ValueType::u32 && function_.expressions[operand].value == 2147483648u)
                return add(Expression{Expression::Kind::literal, ValueType::i32, symbol.position, no_node, no_node, 2147483648u});
            if (symbol.text == "-" && integer_constant(operand)) signed_constant(operand);
            const auto type = function_.expressions[operand].type;
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
            const auto position = function_.expressions[receiver].position;
            const auto type = function_.expressions[receiver].type;
            if (accept("[")) {
                if (type.kind != ValueType::array && type.kind != ValueType::string) reject(position, "Indexing requires Array or String.");
                const auto index = expression(); expect("]");
                if (function_.expressions[index].type != ValueType::u32 && function_.expressions[index].type != ValueType::i32)
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
                                const auto input_type = function_.expressions[input].type;
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
            if (function_.expressions[receiver].type != ValueType::strong_reference)
                reject(position, "Member access requires a strong reference; acquire weak references first.");
            const auto member = qualified_identifier();
            if (accept("(")) {
                const MethodBinding* method{};
                for (const auto& candidate : environment_.methods) if (candidate.name == member) method = &candidate;
                if (!method) reject(position, "Unknown native method '" + member + "'.");
                auto arguments = call_arguments(method->parameters);
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
                function_.uses_scoped_values = true;
                const auto index = static_cast<std::uint32_t>(function_.strings.size());
                function_.strings.push_back(std::move(value));
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
            if (function_.expressions[argument].type != required)
                reject(function_.expressions[argument].position, observe ?
                    "observe requires a strong reference argument." : "try_acquire requires a weak reference argument.");
            if (token_.text == ",") reject(token_.position, "Reference operation requires exactly one argument.");
            expect(")");
            return add(Expression{observe ? Expression::Kind::observe_reference : Expression::Kind::acquire_reference,
                observe ? ValueType::weak_reference : ValueType::strong_reference, token.position, argument});
        }
        for (auto iterator = visible_.rbegin(); iterator != visible_.rend(); ++iterator) {
            const auto& local_value = function_.locals[*iterator];
            if (local_value.name == token.text)
                return add(Expression{Expression::Kind::local, local_value.type, token.position,
                    no_node, no_node, *iterator});
        }
        if (accept("(")) {
            for (std::uint32_t i = 0; i < module_.functions.size(); ++i) {
                const auto& target = module_.functions[i];
                if (target.function_name != token.text) continue;
                if (target.is_coroutine) reject(token.position, "Source calls require a synchronous function; async entries are started by the host.");
                Expression call{Expression::Kind::source_call, target.result, token.position, no_node, no_node, i};
                call.arguments = call_arguments(target.parameters);
                for (std::size_t parameter = 0; parameter < target.parameters.size(); ++parameter) {
                    const auto mode = target.parameters[parameter].passing;
                    if (mode != ParameterPassing::value && function_.expressions[call.arguments[parameter]].kind == Expression::Kind::field)
                        reject(token.position, "Runtime object fields may only be borrowed by protected native calls.");
                    call.argument_passing.push_back(mode);
                }
                return add(std::move(call));
            }
            reject(token.position, "Unknown source function '" + std::string(token.text) + "'.");
        }
        reject(token.position, "Unknown identifier '" + std::string(token.text) + "'.");
    }

    Environment environment_;
    Function function_;
    Program module_;
    std::vector<std::uint32_t> visible_;
    std::vector<std::size_t> expression_heights_;
    std::size_t depth_{};
};
}

std::expected<Program, Diagnostic> compile_source(
    std::string_view source_name, std::string_view source, Environment environment, std::string_view entry_name) {
    try {
        return Parser(source_name, source, environment).parse(entry_name);
    } catch (Rejected& rejected) {
        return std::unexpected(std::move(rejected.diagnostic));
    }
}

}
