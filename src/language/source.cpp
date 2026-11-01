#include "language/source.hpp"
#include "language/lexer.hpp"
#include "language/declarations.hpp"

#include <bit>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <type_traits>
#include <utility>

namespace atelier::language {
namespace {
using namespace detail;

bool reference_type(ValueType type) noexcept {
    return type == ValueType::strong_reference || type == ValueType::weak_reference ||
           type == ValueType::null_reference;
}
struct BinaryOperator {
    int precedence;
    Operation operation;
    bool comparison;
};
BinaryOperator binary_operator(std::string_view text) noexcept {
    if (text == "||")
        return {1, Operation::logical_or, false};
    if (text == "&&")
        return {2, Operation::logical_and, false};
    if (text == "|")
        return {3, Operation::bitwise_or, false};
    if (text == "^")
        return {4, Operation::bitwise_xor, false};
    if (text == "&")
        return {5, Operation::bitwise_and, false};
    if (text == "==")
        return {6, Operation::equal, true};
    if (text == "!=")
        return {6, Operation::not_equal, true};
    if (text == "<")
        return {7, Operation::less, true};
    if (text == "<=")
        return {7, Operation::less_equal, true};
    if (text == ">")
        return {7, Operation::greater, true};
    if (text == ">=")
        return {7, Operation::greater_equal, true};
    if (text == "<<")
        return {8, Operation::shift_left, false};
    if (text == ">>")
        return {8, Operation::shift_right, false};
    if (text == "+")
        return {9, Operation::add, false};
    if (text == "-")
        return {9, Operation::subtract, false};
    if (text == "*")
        return {10, Operation::multiply, false};
    if (text == "/")
        return {10, Operation::divide, false};
    if (text == "%")
        return {10, Operation::remainder, false};
    return {-1, Operation::add, false};
}

class Parser : private Tokens {
public:
    Parser(std::string_view name, std::string_view source, Environment environment)
        : Tokens(source), environment_(environment) {
        module_.source_name = name;
    }

    Program parse(std::string_view entry_name, bool allow_declarations) {
        validate_environment();
        module_.object_type_name = environment_.object_type_name;
        module_.value_layouts.assign(environment_.values.begin(), environment_.values.end());
        struct Body {
            Lexer lexer;
            Token token;
        };
        std::vector<Body> bodies;
        std::vector<std::string> type_names;
        while (token_.kind != Token::Kind::end) {
            if (token_.text == "struct" || token_.text == "enum") {
                auto declaration = parse_type_declaration(*this);
                if (type_names.size() == 1024)
                    reject(declaration.position, "A source module supports at most 1024 type declarations.");
                if (std::ranges::find(type_names, declaration.name) != type_names.end())
                    reject(declaration.position, "Duplicate source type name.");
                validate_declaration(declaration);
                type_names.push_back(std::move(declaration.name));
                continue;
            }
            if (accept("const")) {
                const auto position = token_.position;
                const auto type = parameter_type();
                if (!scalar(type))
                    reject(position, "Module constants require a scalar or enum type.");
                const auto name_position = token_.position;
                const auto name = qualified_identifier();
                if (math_reserved_name(name))
                    reject(name_position, "Module constants cannot replace built-in math names.");
                if (std::ranges::any_of(constants_, [&](const auto& value) { return value.name == name; }))
                    reject(name_position, "Duplicate module constant name.");
                if (constants_.size() == 1024)
                    reject(position, "A source module supports at most 1024 constants.");
                expect("=");
                constants_.push_back({name, type, name_position, lexer_, token_});
                while (!accept(";")) {
                    if (token_.kind == Token::Kind::end || token_.text == "{" || token_.text == "}")
                        reject(token_.position, "Expected ';' to close a module constant.");
                    consume();
                }
                continue;
            }
            if (module_.functions.size() >= 1024)
                reject(token_.position, "A source module supports at most 1024 functions.");
            function_ = {};
            function_.position = token_.position;
            visible_.clear();
            signature();
            for (const auto& previous : module_.functions)
                if (previous.function_name == function_.function_name)
                    reject(function_.position, "Duplicate source function name.");
            bodies.push_back({lexer_, token_});
            module_.functions.push_back(std::move(function_));
            expect("{");
            std::size_t braces = 1;
            while (braces != 0) {
                if (token_.kind == Token::Kind::end)
                    reject(token_.position, "Expected '}' to close the function body.");
                if (token_.text == "{")
                    ++braces;
                if (token_.text == "}")
                    --braces;
                consume();
            }
        }
        if (module_.functions.empty() && !allow_declarations)
            reject(token_.position, "Source module requires an entry function.");
        for (const auto& function : module_.functions)
            if (std::ranges::find(type_names, function.function_name) != type_names.end())
                reject(function.position, "A source function and type cannot have the same name.");
        for (const auto& value : constants_) {
            if (std::ranges::find(type_names, value.name) != type_names.end() ||
                std::ranges::any_of(module_.functions,
                                    [&](const auto& function) { return function.function_name == value.name; }))
                reject(value.position, "A module constant cannot share a name with a source type or function.");
            for (const auto& function : module_.functions)
                for (const auto& parameter : function.locals)
                    if (parameter.name == value.name)
                        reject(function.position, "Parameters cannot shadow a module constant.");
        }
        collect_properties();
        for (std::size_t i = 0; i < constants_.size(); ++i)
            resolve_constant(i);
        if (module_.functions.empty())
            return std::move(module_);
        if (entry_name.empty() && module_.functions.size() != 1 && !allow_declarations)
            reject({}, "A multi-function module requires an explicit host entry name.");
        bool found = entry_name.empty();
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            function_ = module_.functions[i];
            if (function_.function_name == entry_name) {
                module_.entry = static_cast<std::uint32_t>(i);
                found = true;
            }
            lexer_ = bodies[i].lexer;
            token_ = bodies[i].token;
            visible_.clear();
            expression_heights_.clear();
            for (std::uint32_t j = 0; j < function_.parameters.size(); ++j)
                visible_.push_back(j);
            function_.body = block();
            finish_properties();
            if (function_.result != ValueType::void_value && !returns(function_.body))
                reject(function_.position, "Non-void function can reach the end without a return value.");
            module_.functions[i] = std::move(function_);
        }
        if (!found)
            reject({}, "Selected entry function was not declared in this module.");
        analyze_calls();
        return std::move(module_);
    }

private:
    math::Type math_type(ValueType type) const noexcept {
        return type.kind == ValueType::record && type.type_index < environment_.values.size()
                   ? environment_.values[type.type_index].math
                   : math::Type{};
    }
    bool plain_record(ValueType type) const noexcept {
        return type.kind == ValueType::record && type.type_index < environment_.values.size() &&
               (environment_.values[type.type_index].plain_value || bool(math_type(type)));
    }
    static bool floating(ValueType type) noexcept { return type == ValueType::f32 || type == ValueType::f64; }
    static ValueType math_scalar(math::Type type) noexcept {
        return type.precision == math::Precision::f64 ? ValueType::f64 : ValueType::f32;
    }
    ValueType math_named_type(std::string_view name) const noexcept {
        for (std::uint32_t i = 0; i < environment_.values.size(); ++i) {
            const auto& value = environment_.values[i];
            if (value.math && (value.name == name || math::short_name(value.math) == name))
                return {ValueType::record, i};
        }
        return ValueType::void_value;
    }
    bool math_reserved_name(std::string_view name) const noexcept {
        return name == "math" || name.starts_with("math.") || math_named_type(name) != ValueType::void_value;
    }
    ValueType named_type(std::string_view name, SourcePosition position) const {
        if (name == "bool")
            return ValueType::boolean;
        if (name == "float")
            return ValueType::f32;
        if (name == "double")
            return ValueType::f64;
        if (name == "uint")
            return ValueType::u32;
        if (name == "int")
            return ValueType::i32;
        if (name == environment_.object_type_name)
            return ValueType::strong_reference;
        for (std::uint32_t i = 0; i < environment_.values.size(); ++i)
            if (environment_.values[i].name == name ||
                (name == "String" && environment_.values[i].kind == ValueType::string))
                return {environment_.values[i].kind, i};
        for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i)
            if (environment_.enumerations[i].name == name)
                return {ValueType::enumeration, i};
        const auto math = math_named_type(name);
        if (math != ValueType::void_value)
            return math;
        reject(position, "Unknown registered value type '" + std::string(name) + "'.");
    }
    ValueType declared_type(const TypeName& name, SourcePosition position) const {
        auto type = named_type(name.name, position);
        for (std::uint32_t depth = 0; depth < name.array_depth; ++depth) {
            const auto array = std::ranges::find_if(environment_.values, [&](const auto& value) {
                return value.kind == ValueType::array && value.element == type;
            });
            if (array == environment_.values.end())
                reject(position, "This Array element type has not been registered in the Runtime.");
            type = {ValueType::array, static_cast<std::uint32_t>(array - environment_.values.begin())};
        }
        return type;
    }
    void validate_declaration(const TypeDeclaration& declaration) const {
        const auto type = named_type(declaration.name, declaration.position);
        if (declaration.enumeration) {
            if (type.kind != ValueType::enumeration)
                reject(declaration.position, "Source enum conflicts with its Runtime type.");
            const auto& layout = environment_.enumerations[type.type_index];
            if (layout.underlying != (declaration.signed_enum ? ValueType::i32 : ValueType::u32) ||
                layout.values.size() != declaration.enumerators.size())
                reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
            for (std::size_t i = 0; i < layout.values.size(); ++i)
                if (layout.values[i].name != declaration.enumerators[i].first ||
                    layout.values[i].bits != declaration.enumerators[i].second)
                    reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
        } else {
            if (type.kind != ValueType::record)
                reject(declaration.position, "Source struct conflicts with its Runtime type.");
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
        function_.result = accept("void") ? ValueType::void_value : result_type();
        if (function_.is_coroutine && aggregate(function_.result))
            reject(function_.position, "async aggregate results are not supported by the task result contract.");
        if (plain_record(function_.result) && !math_type(function_.result))
            function_.uses_scoped_values = true;
        const auto name_position = token_.position;
        const auto name = qualified_identifier();
        if (math_reserved_name(name))
            reject(name_position, "Source functions cannot replace math constructors or the math namespace.");
        function_.function_name = name;
        expect("(");
        if (!accept(")")) {
            do {
                const auto position = token_.position;
                const bool read_only = accept("const");
                const auto type = parameter_type();
                if (reference_type(type))
                    reject(position, "Object reference parameters are not supported.");
                const bool reference = accept("&");
                if (read_only && !reference)
                    reject(position, "const parameters require a reference in this source slice.");
                const auto passing = reference ? (read_only ? ParameterPassing::read_only : ParameterPassing::reference)
                                               : ParameterPassing::value;
                if (function_.is_coroutine && reference)
                    reject(position, "async parameters cannot borrow caller storage.");
                if (aggregate(type)) {
                    if (function_.is_coroutine)
                        reject(position, "async aggregate parameters are not supported by the task argument contract.");
                    if (!reference && !plain_record(type))
                        reject(
                            position,
                            "Only plain records support value parameters; other native values require Type& or const Type&.");
                    if (!reference && !math_type(type) && !environment_.values[type.type_index].can_own)
                        reject(
                            position,
                            "Plain record value parameters require borrow-preserving copy and lifecycle operations.");
                    if (type.kind == ValueType::string && !read_only)
                        reject(position, "String borrowing requires const String&.");
                    if (reference || !math_type(type))
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
            // A self node preserves borrows only if every use is a BorrowSafe call or a by-value field
            // read; `(self.v).x = 1` shares its self node with the unused `self.v` read it was projected from.
            std::vector<bool> allowed(function.expressions.size());
            std::vector<bool> blocked(function.expressions.size());
            std::vector<bool> stored(function.expressions.size());
            function.preserves_borrows = true;
            for (const auto& statement : function.statements)
                if (statement.kind == Statement::Kind::assign)
                    stored[statement.target] = true;
            for (const auto& node : function.expressions)
                if (node.kind == Expression::Kind::call)
                    for (std::size_t i = 0; i < node.arguments.size(); ++i)
                        if (node.argument_passing[i] != ParameterPassing::value)
                            stored[node.arguments[i]] = true;
            for (std::size_t i = 0; i < function.expressions.size(); ++i) {
                const auto& node = function.expressions[i];
                if (node.kind == Expression::Kind::receiver)
                    function.requires_receiver = true;
                if (node.kind == Expression::Kind::field &&
                    function.expressions[node.receiver].kind == Expression::Kind::receiver)
                    (stored[i] ? blocked : allowed)[node.receiver] = true;
                if (node.kind != Expression::Kind::call)
                    continue;
                if (!environment_.methods[node.value].preserves_borrows ||
                    function.expressions[node.receiver].kind != Expression::Kind::receiver)
                    function.preserves_borrows = false;
                else
                    allowed[node.receiver] = true;
            }
            for (std::size_t i = 0; i < function.expressions.size(); ++i)
                if (reference_type(function.expressions[i].type) && (!allowed[i] || blocked[i]))
                    function.preserves_borrows = false;
            for (const auto& local : function.locals)
                if (reference_type(local.type))
                    function.preserves_borrows = false;
        }
        enum class State { fresh, active, complete };
        std::vector<State> states(module_.functions.size());
        struct Cursor {
            std::uint32_t function;
            std::size_t next{};
        };
        std::vector<Cursor> stack;
        for (std::uint32_t root = 0; root < module_.functions.size(); ++root) {
            if (states[root] != State::fresh)
                continue;
            states[root] = State::active;
            stack.push_back({root});
            while (!stack.empty()) {
                auto& cursor = stack.back();
                auto& function = module_.functions[cursor.function];
                while (cursor.next < function.expressions.size() &&
                       function.expressions[cursor.next].kind != Expression::Kind::source_call)
                    ++cursor.next;
                if (cursor.next < function.expressions.size()) {
                    const auto& call = function.expressions[cursor.next];
                    if (states[call.value] == State::active)
                        reject(call.position, "Recursive source calls are not supported in this module stage.");
                    if (states[call.value] == State::fresh) {
                        states[call.value] = State::active;
                        stack.push_back({call.value});
                        continue;
                    }
                    const auto& callee = module_.functions[call.value];
                    function.uses_scoped_values |= callee.uses_scoped_values;
                    function.requires_receiver |= callee.requires_receiver;
                    function.preserves_borrows &= callee.preserves_borrows;
                    ++cursor.next;
                } else {
                    if (function.uses_scoped_values && (function.is_coroutine || !function.preserves_borrows))
                        reject(
                            function.position,
                            "Scoped value execution in '" + function.function_name +
                                "' reaches suspension, an object operation other than reading a self field, or a native call without BorrowSafe.");
                    states[cursor.function] = State::complete;
                    stack.pop_back();
                }
            }
        }
        module_.reachable.resize(module_.functions.size());
        std::vector<std::uint32_t> pending{module_.entry};
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            if (module_.reachable[index])
                continue;
            module_.reachable[index] = true;
            for (const auto& call : module_.functions[index].expressions)
                if (call.kind == Expression::Kind::source_call)
                    pending.push_back(call.value);
        }
    }
    std::vector<NodeId> call_arguments(std::span<const ParameterType> parameters) {
        std::vector<NodeId> arguments;
        if (!accept(")")) {
            do {
                if (arguments.size() >= parameters.size())
                    reject(token_.position, "Call argument count mismatch.");
                arguments.push_back(expression());
            } while (accept(","));
            expect(")");
        }
        if (arguments.size() != parameters.size())
            reject(token_.position, "Call argument count mismatch.");
        return arguments;
    }
    Expression call_expression(bool native, std::uint32_t index, SourcePosition position, NodeId receiver,
                               std::vector<NodeId> arguments) {
        const auto parameters = native ? environment_.methods[index].parameters
                                       : std::span<const ParameterType>{module_.functions[index].parameters};
        const auto result = native ? environment_.methods[index].result : module_.functions[index].result;
        if (!native && module_.functions[index].is_coroutine)
            reject(position, "Source calls require a synchronous function; async entries are started by the host.");
        if (plain_record(result) && !math_type(result)) {
            if (!environment_.values[result.type_index].can_own)
                reject(position, "Plain record results require borrow-preserving copy and lifecycle operations.");
            function_.uses_scoped_values = true;
        }
        Expression call{native ? Expression::Kind::call : Expression::Kind::source_call,
                        result,
                        position,
                        no_node,
                        no_node,
                        index,
                        Operation::add,
                        receiver};
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& parameter = parameters[i];
            auto argument = converted(arguments[i], parameter.type);
            const auto& node = function_.expressions[argument];
            if (parameter.passing == ParameterPassing::value && plain_record(parameter.type) &&
                !math_type(parameter.type) && !environment_.values[parameter.type.type_index].can_own)
                reject(node.position,
                       "Plain record value arguments require borrow-preserving copy and lifecycle operations.");
            if (parameter.passing == ParameterPassing::reference && !writable_storage(argument))
                reject(node.position, "Writable reference requires a mutable lvalue.");
            if (parameter.passing != ParameterPassing::value && node.kind == Expression::Kind::field) {
                if (!native)
                    reject(node.position, "Runtime object fields may only be borrowed by protected native calls.");
                if (parameter.passing == ParameterPassing::reference && environment_.fields[node.value].constrained)
                    reject(node.position, "Hard-bounded fields cannot be borrowed writably; assign the field instead.");
            }
            call.arguments.push_back(argument);
            call.argument_passing.push_back(parameter.passing);
        }
        return call;
    }
    struct Property {
        std::string name;
        ValueType type;
        bool native;
        std::uint32_t getter{UINT32_MAX};
        std::uint32_t setter{UINT32_MAX};
    };
    struct PropertyUse {
        NodeId node;
        std::size_t property;
    };
    std::vector<Property> properties_;
    std::vector<PropertyUse> property_uses_;
    const Property* find_property(std::string_view name, bool native) const {
        for (const auto& property : properties_)
            if (property.native == native && property.name == name)
                return &property;
        return nullptr;
    }
    void collect_properties() {
        const auto collect = [&](std::string_view name, std::span<const ParameterType> parameters, ValueType result,
                                 bool native, std::uint32_t index, SourcePosition position) {
            auto alias = property_name(name, parameters.size(), result == ValueType::void_value,
                                       parameters.size() == 1 && parameters[0].passing == ParameterPassing::reference);
            if (alias.empty())
                return;
            const auto start = name.rfind('.') == std::string_view::npos ? 0 : name.rfind('.') + 1;
            const bool getter = name.substr(start).starts_with("Get");
            const auto type = getter ? result : parameters[0].type;
            auto property =
                std::ranges::find_if(properties_, [&](const auto& p) { return p.native == native && p.name == alias; });
            if (property == properties_.end()) {
                properties_.push_back({std::move(alias), type, native});
                property = properties_.end() - 1;
            }
            if (property->type != type)
                reject(position, "Property getter and setter types differ: " + property->name);
            (getter ? property->getter : property->setter) = index;
        };
        for (std::uint32_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            collect(method.name, method.parameters, method.result, true, i, {});
        }
        for (std::uint32_t i = 0; i < module_.functions.size(); ++i) {
            const auto& function = module_.functions[i];
            if (!function.is_coroutine)
                collect(function.function_name, function.parameters, function.result, false, i, function.position);
        }
        for (const auto& property : properties_) {
            const auto conflict = [&] {
                reject({}, "Property conflicts with a visible declaration: " + property.name);
            };
            const auto hidden = [&](std::string_view name) {
                return name == property.name ||
                       (name.starts_with(property.name) && name.size() > property.name.size() &&
                        name[property.name.size()] == '.');
            };
            for (const auto& other : properties_)
                if (&other != &property && other.native == property.native && hidden(other.name))
                    conflict();
            for (const auto& field : environment_.fields)
                if (hidden(field.name))
                    conflict();
            if (property.native) {
                for (const auto& method : environment_.methods)
                    if (hidden(method.name))
                        conflict();
                continue;
            }
            if (math_reserved_name(property.name) || hidden(environment_.object_type_name))
                conflict();
            for (const auto& type : environment_.values)
                if (hidden(type.name))
                    conflict();
            for (const auto& type : environment_.enumerations)
                if (hidden(type.name))
                    conflict();
            for (const auto& constant : constants_)
                if (hidden(constant.name))
                    conflict();
            for (const auto& function : module_.functions) {
                if (hidden(function.function_name))
                    conflict();
                for (const auto& parameter : function.locals)
                    if (parameter.name == property.name)
                        conflict();
            }
        }
    }
    NodeId property_access(const Property& property, SourcePosition position, NodeId receiver = no_node) {
        // Keep only a call-shaped placeholder until assignment vs. read is known.
        // No speculative getter call may enter effect analysis or the call graph.
        const auto node = add(Expression{property.native ? Expression::Kind::call : Expression::Kind::source_call,
                                         property.type, position, no_node, no_node, 0, Operation::add, receiver});
        property_uses_.push_back({node, static_cast<std::size_t>(&property - properties_.data())});
        return node;
    }
    void finish_properties() {
        for (const auto& use : property_uses_) {
            const auto& property = properties_[use.property];
            const auto node = function_.expressions[use.node];
            if (property.getter == UINT32_MAX)
                reject(node.position, "Property is write-only: " + property.name);
            function_.expressions[use.node] =
                call_expression(property.native, property.getter, node.position, node.receiver, {});
        }
        property_uses_.clear();
    }
    std::string expression_name(bool native) {
        std::string name(identifier().text);
        while (token_.text == "." && !find_property(name, native)) {
            consume();
            name += '.';
            name += identifier().text;
        }
        return name;
    }
    struct Depth {
        std::size_t& value;
        Depth(std::size_t& depth, SourcePosition position) : value(depth) {
            if (++value > 128)
                reject(position, "Source nesting exceeds the compiler limit of 128.");
        }
        ~Depth() { --value; }
    };
    void validate_name(std::string_view name, bool optional) const {
        if (name.empty() && !optional)
            reject({}, "Host member name cannot be empty.");
        for (std::size_t start = 0; start < name.size();) {
            auto end = name.find('.', start);
            if (end == std::string_view::npos)
                end = name.size();
            const auto segment = name.substr(start, end - start);
            if (segment.empty() || !letter(segment.front()) || reserved(segment))
                reject({}, "Host binding name requires non-reserved identifier segments.");
            for (const auto character : segment)
                if (!letter(character) && !digit(character))
                    reject({}, "Host binding name requires non-reserved identifier segments.");
            if (end == name.size())
                break;
            start = end + 1;
            if (start == name.size())
                reject({}, "Host binding name cannot end with '.'.");
        }
    }
    bool scalar(ValueType type) const {
        return type == ValueType::boolean || floating(type) || type == ValueType::u32 || type == ValueType::i32 ||
               (type.kind == ValueType::enumeration && type.type_index < environment_.enumerations.size());
    }
    bool supported_value(ValueType type) const {
        return scalar(type) || (aggregate(type) && type.type_index < environment_.values.size() &&
                                environment_.values[type.type_index].kind == type.kind);
    }
    std::size_t value_size(ValueType type) const {
        return aggregate(type)              ? environment_.values[type.type_index].size
               : type == ValueType::boolean ? 1
               : type == ValueType::f64     ? 8
                                            : 4;
    }
    std::size_t value_alignment(ValueType type) const {
        return aggregate(type)              ? environment_.values[type.type_index].alignment
               : type == ValueType::boolean ? 1
               : type == ValueType::f64     ? 8
                                            : 4;
    }
    void validate_environment() const {
        validate_name(environment_.object_type_name, true);
        for (std::size_t i = 0; i < environment_.enumerations.size(); ++i) {
            const auto& enumeration = environment_.enumerations[i];
            validate_name(enumeration.name, false);
            if (enumeration.underlying != ValueType::i32 && enumeration.underlying != ValueType::u32)
                reject({}, "Enum requires i32/u32 representation.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.enumerations[j].name == enumeration.name)
                    reject({}, "Duplicate enum type name.");
            for (std::size_t j = 0; j < enumeration.values.size(); ++j) {
                validate_name(enumeration.values[j].name, false);
                if (enumeration.values[j].name.find('.') != std::string_view::npos)
                    reject({}, "Enum item must be a simple identifier.");
                for (std::size_t k = 0; k < j; ++k)
                    if (enumeration.values[k].name == enumeration.values[j].name)
                        reject({}, "Duplicate enum item name.");
            }
        }
        if (environment_.values.size() > std::numeric_limits<std::uint32_t>::max())
            reject({}, "Value catalog is too large.");
        for (std::size_t i = 0; i < environment_.values.size(); ++i) {
            const auto& value = environment_.values[i];
            validate_name(value.name, false);
            if (!aggregate({value.kind, static_cast<std::uint32_t>(i)}) || !value.size ||
                !std::has_single_bit(value.alignment) || value.size % value.alignment ||
                value.size > static_cast<std::size_t>(PTRDIFF_MAX))
                reject({}, "Invalid native value layout.");
            if ((value.kind == ValueType::array || value.kind == ValueType::string) && !value.sequence_view)
                reject({}, "Sequence layout requires its native view operation.");
            if (value.kind == ValueType::array && !supported_value(value.element))
                reject({}, "Invalid array element type.");
            if (value.math) {
                if (value.kind != ValueType::record || value.name != math::name(value.math) ||
                    value.math.components() == 0 || value.size != value.math.size() ||
                    value.alignment != value.math.scalar_size() || value.fields.size() != value.math.components() ||
                    (value.math.precision != math::Precision::f32 && value.math.precision != math::Precision::f64))
                    reject({}, "Invalid canonical math value layout.");
                for (std::size_t component = 0; component < value.fields.size(); ++component)
                    if (value.fields[component].type != math_scalar(value.math) ||
                        value.fields[component].offset != component * value.math.scalar_size())
                        reject({}, "Math components require contiguous scalar layout.");
                const auto alias = math::short_name(value.math);
                if (environment_.object_type_name == alias)
                    reject({}, "Object type conflicts with a built-in math type alias.");
                for (const auto& other : environment_.values)
                    if (&other != &value && (other.math == value.math || other.name == alias))
                        reject({}, "Ambiguous built-in math type or alias.");
                for (const auto& enumeration : environment_.enumerations)
                    if (enumeration.name == alias || enumeration.name == value.name)
                        reject({}, "Enum type conflicts with a built-in math type or alias.");
            }
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.values[j].name == value.name)
                    reject({}, "Duplicate native value name.");
            for (std::size_t j = 0; j < value.fields.size(); ++j) {
                const auto& field = value.fields[j];
                validate_name(field.name, false);
                if (value.kind != ValueType::record || field.name.find('.') != std::string::npos ||
                    !supported_value(field.type) || field.offset > value.size ||
                    value_size(field.type) > value.size - field.offset || field.offset % value_alignment(field.type) ||
                    value.alignment < value_alignment(field.type))
                    reject({}, "Invalid direct native field layout.");
                for (std::size_t k = 0; k < j; ++k)
                    if (value.fields[k].name == field.name)
                        reject({}, "Duplicate native value field.");
            }
        }
        if (environment_.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
            environment_.methods.size() > std::numeric_limits<std::uint32_t>::max())
            reject({}, "Host member catalog exceeds its index range.");
        for (std::size_t i = 0; i < environment_.fields.size(); ++i) {
            const auto& field = environment_.fields[i];
            validate_name(field.name, false);
            if (!scalar(field.type) && !math_type(field.type))
                reject({}, "Host fields require scalar or built-in math value types.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.fields[j].name == field.name)
                    reject({}, "Ambiguous host field binding.");
        }
        for (std::size_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            validate_name(method.name, false);
            if (method.parameters.size() > std::numeric_limits<std::uint32_t>::max())
                reject({}, "Host parameter list exceeds its index range.");
            for (const auto& parameter : method.parameters)
                if (!supported_value(parameter.type) ||
                    (aggregate(parameter.type) && parameter.passing == ParameterPassing::value &&
                     !plain_record(parameter.type)) ||
                    (parameter.passing != ParameterPassing::value && parameter.passing != ParameterPassing::read_only &&
                     parameter.passing != ParameterPassing::reference))
                    reject({}, "Host method has an unsupported parameter type or passing mode.");
            if (method.result != ValueType::void_value && !scalar(method.result) && !plain_record(method.result))
                reject({}, "Host method has an unsupported result type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.methods[j].name == method.name)
                    reject({}, "Ambiguous host method binding.");
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
        if (token_.kind != Token::Kind::identifier || reserved(token_.text))
            return false;
        auto lookahead = lexer_;
        auto next = lookahead.next();
        if (token_.text == "Array" && next.text == "<")
            return true;
        while (next.text == ".") {
            const auto segment = lookahead.next();
            if (segment.kind != Token::Kind::identifier || reserved(segment.text))
                return false;
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
            if (expression_heights_[argument] + 1 > height)
                height = expression_heights_[argument] + 1;
        if (height > 128)
            reject(expression.position, "Expression tree exceeds the compiler limit of 128.");
        function_.expressions.push_back(std::move(expression));
        expression_heights_.push_back(height);
        return function_.expressions.size() - 1;
    }
    NodeId literal(ValueType type, SourcePosition position, std::uint64_t bits) {
        Expression expression{Expression::Kind::literal, type, position};
        expression.bits = bits;
        return add(std::move(expression));
    }
    NodeId add(Statement statement) {
        function_.statements.push_back(std::move(statement));
        return function_.statements.size() - 1;
    }
    void unused_name(const Token& name) const {
        if (math_reserved_name(name.text))
            reject(name.position, "Locals cannot replace math constructors or the math namespace.");
        if (find_property(name.text, false))
            reject(name.position, "Locals cannot shadow a property.");
        for (const auto& value : constants_)
            if (value.name == name.text)
                reject(name.position,
                       "A module constant with this name is already visible; shadowing is not supported.");
        for (const auto index : visible_)
            if (function_.locals[index].name == name.text)
                reject(name.position,
                       "A local or parameter with this name is already visible; shadowing is not supported.");
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
    ValueType result_type() {
        const auto position = token_.position;
        const auto type = parameter_type();
        if (scalar(type) || math_type(type))
            return type;
        if (plain_record(type)) {
            if (!environment_.values[type.type_index].can_own)
                reject(position, "Plain record results require borrow-preserving copy and lifecycle operations.");
            return type;
        }
        reject(position, "Function results require a scalar, enum or plain record value type.");
    }
    ValueType parameter_type() {
        Depth guard(depth_, token_.position);
        const auto position = token_.position;
        return declared_type(parse_type_name(*this), position);
    }
    bool returns(NodeId id) const {
        const auto& statement = function_.statements[id];
        switch (statement.kind) {
        case Statement::Kind::return_value:
            return true;
        case Statement::Kind::block:
            for (const auto child : statement.children)
                if (returns(child))
                    return true;
            return false;
        case Statement::Kind::condition:
            return statement.alternative != no_node && returns(statement.body) && returns(statement.alternative);
        default:
            return false; // A loop may execute zero times.
        }
    }
    bool writable(NodeId id) const {
        const auto& expression = function_.expressions[id];
        if (aggregate(expression.type) && !math_type(expression.type))
            return owned_storage(id) && environment_.values[expression.type.type_index].can_own;
        return writable_storage(id);
    }
    bool writable_storage(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::field)
            return true;
        if (node.kind == Expression::Kind::local)
            return !function_.locals[node.value].read_only;
        if (node.kind == Expression::Kind::index && function_.expressions[node.left].type.kind == ValueType::string)
            return false;
        // Whole object math reads are copies. A mutable component must resolve to
        // its published object field rather than point into that temporary copy.
        if (node.kind == Expression::Kind::value_field &&
            function_.expressions[node.left].kind == Expression::Kind::field)
            return false;
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index)
            return writable_storage(node.left);
        return false;
    }
    bool owned_storage(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::local)
            return !function_.locals[node.value].read_only &&
                   (node.value >= function_.parameters.size() ||
                    function_.parameters[node.value].passing == ParameterPassing::value);
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index)
            return owned_storage(node.left);
        return false;
    }
    bool integer_constant(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.type != ValueType::u32 || node.typed_constant)
            return false;
        if (node.kind == Expression::Kind::literal)
            return node.bits <= 2147483647u;
        return (node.kind == Expression::Kind::unary && integer_constant(node.left)) ||
               (node.kind == Expression::Kind::binary && integer_constant(node.left) && integer_constant(node.right));
    }
    void signed_constant(NodeId id) {
        auto& node = function_.expressions[id];
        node.type = ValueType::i32;
        if (node.left != no_node)
            signed_constant(node.left);
        if (node.right != no_node)
            signed_constant(node.right);
    }
    NodeId converted(NodeId expression, ValueType required) {
        if (required == ValueType::i32 && integer_constant(expression))
            signed_constant(expression);
        const auto actual = function_.expressions[expression].type;
        if (actual == required || (reference_type(required) && actual == ValueType::null_reference))
            return expression;
        if (required == ValueType::weak_reference && actual == ValueType::strong_reference)
            return add(Expression{Expression::Kind::observe_reference, required,
                                  function_.expressions[expression].position, expression});
        if (reference_type(required) || reference_type(actual))
            reject(
                function_.expressions[expression].position,
                "Type mismatch; only strong-to-weak reference conversion is implicit; use try_acquire to strengthen.");
        reject(function_.expressions[expression].position,
               "Type mismatch; typed numeric values and distinct enum types do not convert implicitly.");
    }
    static long double numeric_value(ValueType type, std::uint64_t bits) {
        if (type == ValueType::f64)
            return std::bit_cast<double>(bits);
        if (type == ValueType::f32)
            return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        if (type == ValueType::i32)
            return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
        return static_cast<std::uint32_t>(bits);
    }
    static std::uint64_t convert_constant(ValueType from, ValueType to, std::uint64_t bits, SourcePosition position) {
        if (from == to)
            return bits;
        auto value = numeric_value(from, bits);
        if (to == ValueType::f32)
            return std::bit_cast<std::uint32_t>(static_cast<float>(value));
        if (to == ValueType::f64)
            return std::bit_cast<std::uint64_t>(static_cast<double>(value));
        value = std::trunc(value);
        const auto minimum = to == ValueType::i32 ? -2147483648.0L : 0.0L;
        const auto maximum = to == ValueType::i32 ? 2147483647.0L : 4294967295.0L;
        if (!std::isfinite(value) || value < minimum || value > maximum)
            reject(position, "Constant numeric conversion exceeds its integer destination range.");
        return to == ValueType::i32 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(value))
                                    : static_cast<std::uint32_t>(value);
    }
    template <class T>
    static std::uint64_t fold_arithmetic(Operation operation, T left, T right, SourcePosition position) {
        if (operation == Operation::equal)
            return left == right;
        if (operation == Operation::not_equal)
            return left != right;
        if (operation == Operation::less)
            return left < right;
        if (operation == Operation::less_equal)
            return left <= right;
        if (operation == Operation::greater)
            return left > right;
        if (operation == Operation::greater_equal)
            return left >= right;
        if constexpr (std::is_floating_point_v<T>) {
            T result{};
            switch (operation) {
            case Operation::add:
                result = left + right;
                break;
            case Operation::subtract:
                result = left - right;
                break;
            case Operation::multiply:
                result = left * right;
                break;
            case Operation::divide:
                result = left / right;
                break;
            default:
                reject(position, "Unsupported constant floating operation.");
            }
            if constexpr (sizeof(T) == 8)
                return std::bit_cast<std::uint64_t>(result);
            else
                return std::bit_cast<std::uint32_t>(result);
        } else {
            const auto a = static_cast<std::uint32_t>(left);
            const auto b = static_cast<std::uint32_t>(right);
            switch (operation) {
            case Operation::add:
                return a + b;
            case Operation::subtract:
                return a - b;
            case Operation::multiply:
                return a * b;
            case Operation::bitwise_and:
                return a & b;
            case Operation::bitwise_or:
                return a | b;
            case Operation::bitwise_xor:
                return a ^ b;
            case Operation::divide:
            case Operation::remainder:
                if (right == 0)
                    reject(position, "Constant integer division or remainder by zero.");
                if constexpr (std::is_signed_v<T>)
                    if (left == std::numeric_limits<std::int32_t>::min() && right == -1)
                        reject(position, "Constant signed division or remainder overflows.");
                return static_cast<std::uint32_t>(operation == Operation::divide ? left / right : left % right);
            default:
                reject(position, "Unsupported constant integer operation.");
            }
        }
    }
    void validate_constant(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind != Expression::Kind::literal && node.kind != Expression::Kind::unary &&
            node.kind != Expression::Kind::binary && node.kind != Expression::Kind::scalar_cast)
            reject(node.position, "Module constant initializers require compile-time pure scalar expressions.");
        if (node.left != no_node)
            validate_constant(node.left);
        if (node.right != no_node)
            validate_constant(node.right);
    }
    std::uint64_t fold_constant(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::literal)
            return node.bits;
        const auto left = fold_constant(node.left);
        const auto input = function_.expressions[node.left].type;
        if (node.kind == Expression::Kind::scalar_cast)
            return convert_constant(input, node.type, left, node.position);
        if (node.kind == Expression::Kind::unary) {
            switch (node.operation) {
            case Operation::positive:
                return left;
            case Operation::logical_not:
                return !left;
            case Operation::bitwise_not:
                return ~static_cast<std::uint32_t>(left);
            case Operation::negate:
                if (input == ValueType::f32)
                    return std::bit_cast<std::uint32_t>(-std::bit_cast<float>(static_cast<std::uint32_t>(left)));
                if (input == ValueType::f64)
                    return std::bit_cast<std::uint64_t>(-std::bit_cast<double>(left));
                return 0u - static_cast<std::uint32_t>(left);
            default:
                reject(node.position, "Unsupported constant unary operation.");
            }
        }
        if (node.operation == Operation::logical_and && !left)
            return 0;
        if (node.operation == Operation::logical_or && left)
            return 1;
        const auto right = fold_constant(node.right);
        if (node.operation == Operation::logical_and || node.operation == Operation::logical_or)
            return right != 0;
        if (node.operation == Operation::shift_left || node.operation == Operation::shift_right) {
            const auto count_type = function_.expressions[node.right].type;
            if (right > 31 ||
                (count_type == ValueType::i32 && std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(right)) < 0))
                reject(node.position, "Constant shift count is outside 0..31.");
            const auto value = static_cast<std::uint32_t>(left);
            const auto count = static_cast<std::uint32_t>(right);
            if (node.operation == Operation::shift_left)
                return value << count;
            if (input == ValueType::i32)
                return static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(value) >> count);
            return value >> count;
        }
        if (input == ValueType::f32)
            return fold_arithmetic(node.operation, std::bit_cast<float>(static_cast<std::uint32_t>(left)),
                                   std::bit_cast<float>(static_cast<std::uint32_t>(right)), node.position);
        if (input == ValueType::f64)
            return fold_arithmetic(node.operation, std::bit_cast<double>(left), std::bit_cast<double>(right),
                                   node.position);
        if (input == ValueType::i32)
            return fold_arithmetic(node.operation, std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(left)),
                                   std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(right)), node.position);
        return fold_arithmetic(node.operation, static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(right),
                               node.position);
    }
    void resolve_constant(std::size_t index) {
        auto& value = constants_[index];
        if (value.state == Constant::State::complete)
            return;
        if (value.state == Constant::State::active)
            reject(value.position, "Circular module constant dependency.");
        value.state = Constant::State::active;
        const auto saved_lexer = lexer_;
        const auto saved_token = token_;
        auto saved_function = std::move(function_);
        auto saved_heights = std::move(expression_heights_);
        auto saved_visible = std::move(visible_);
        function_ = {};
        expression_heights_.clear();
        visible_.clear();
        lexer_ = value.lexer;
        token_ = value.token;
        const auto initializer = converted(expression(), value.type);
        expect(";");
        validate_constant(initializer);
        value.bits = fold_constant(initializer);
        value.state = Constant::State::complete;
        lexer_ = saved_lexer;
        token_ = saved_token;
        function_ = std::move(saved_function);
        expression_heights_ = std::move(saved_heights);
        visible_ = std::move(saved_visible);
    }
    NodeId block() {
        Depth guard(depth_, token_.position);
        Statement result{Statement::Kind::block, expect("{").position, no_node, no_node, no_node, no_node, {}};
        const auto visible_before = visible_.size();
        while (token_.text != "}") {
            if (token_.kind == Token::Kind::end)
                reject(token_.position, "Expected '}' before end of source.");
            result.children.push_back(statement());
        }
        consume();
        result.locals.assign(visible_.begin() + visible_before, visible_.end());
        visible_.resize(visible_before);
        return add(std::move(result));
    }
    bool starts_declaration() const {
        return token_.text == "const" || token_.text == "bool" || token_.text == "float" || token_.text == "double" ||
               token_.text == "uint" || token_.text == "int" || token_.text == "weak" || reference_declaration();
    }
    NodeId simple_statement(std::string_view terminator, bool allow_declaration = true) {
        const auto position = token_.position;
        if (starts_declaration()) {
            if (!allow_declaration)
                reject(position, "A for step accepts assignment, call or an empty clause.");
            const bool read_only = accept("const");
            const bool weak = accept("weak");
            auto type = parameter_type();
            if (weak) {
                if (type != ValueType::strong_reference)
                    reject(position, "weak requires an object type.");
                type = ValueType::weak_reference;
            }
            if (read_only && reference_type(type))
                reject(position, "const object reference locals are not supported.");
            if (aggregate(type) && !math_type(type)) {
                if (!environment_.values[type.type_index].can_own)
                    reject(position,
                           "Owned locals require values with borrow-preserving copy and lifecycle operations.");
                function_.uses_scoped_values = true;
            }
            const auto name = identifier();
            unused_name(name);
            const auto initialized = accept("=");
            if (read_only && !initialized)
                reject(token_.position, "const locals require an initializer.");
            if (!initialized && !reference_type(type) && !aggregate(type))
                reject(token_.position, "Expected '='.");
            NodeId initializer = no_node;
            if (initialized)
                initializer = expression();
            else if (reference_type(type))
                initializer =
                    add(Expression{Expression::Kind::null_reference, ValueType::null_reference, name.position});
            if (initializer != no_node)
                initializer = converted(initializer, type);
            expect(terminator);
            const auto index = local(name, type);
            function_.locals[index].read_only = read_only;
            return add(Statement{Statement::Kind::declare, position, index, initializer, no_node, no_node, {}});
        }
        const auto target = expression();
        if (accept("=")) {
            const auto use = std::ranges::find(property_uses_, target, &PropertyUse::node);
            if (use != property_uses_.end()) {
                const auto property = properties_[use->property];
                const auto node = function_.expressions[target];
                if (property.setter == UINT32_MAX)
                    reject(position, "Property is read-only: " + property.name);
                property_uses_.erase(use);
                auto call =
                    call_expression(property.native, property.setter, node.position, node.receiver, {expression()});
                // The RHS can be deeper than the original property placeholder.
                if (expression_heights_[call.arguments[0]] >= 128)
                    reject(position, "Expression tree exceeds the compiler limit of 128.");
                function_.expressions[target] = std::move(call);
                expect(terminator);
                return add(Statement{Statement::Kind::call, position, no_node, target, no_node, no_node, {}});
            }
            if (!writable(target))
                reject(position, "Assignment requires a writable local or field.");
            const auto value = converted(expression(), function_.expressions[target].type);
            expect(terminator);
            return add(Statement{Statement::Kind::assign, position, target, value, no_node, no_node, {}});
        }
        if (function_.expressions[target].kind != Expression::Kind::call &&
            function_.expressions[target].kind != Expression::Kind::source_call &&
            function_.expressions[target].kind != Expression::Kind::sequence_mutation)
            reject(position, "Only calls and sequence modifications can be used as expression statements.");
        expect(terminator);
        return add(Statement{Statement::Kind::call, position, no_node, target, no_node, no_node, {}});
    }
    NodeId statement() {
        Depth guard(depth_, token_.position);
        const auto position = token_.position;
        if (token_.text == "{")
            return block();
        if (starts_declaration())
            return simple_statement(";");
        if (accept("if")) {
            expect("(");
            const auto condition = expression();
            if (function_.expressions[condition].type != ValueType::boolean)
                reject(position, "An if condition requires bool.");
            expect(")");
            const auto body = block();
            NodeId alternative = no_node;
            if (accept("else"))
                alternative = token_.text == "if" ? statement() : block();
            return add(Statement{Statement::Kind::condition, position, no_node, condition, body, alternative, {}});
        }
        if (accept("while")) {
            expect("(");
            const auto condition = expression();
            if (function_.expressions[condition].type != ValueType::boolean)
                reject(position, "A while condition requires bool.");
            expect(")");
            ++loop_depth_;
            const auto body = block();
            --loop_depth_;
            return add(Statement{Statement::Kind::loop, position, no_node, condition, body, no_node, {}});
        }
        if (accept("for")) {
            expect("(");
            const auto visible_before = visible_.size();
            Statement scope{Statement::Kind::block, position, no_node, no_node, no_node, no_node, {}};
            if (!accept(";"))
                scope.children.push_back(simple_statement(";"));
            const auto condition = token_.text == ";" ? literal(ValueType::boolean, token_.position, 1) : expression();
            if (function_.expressions[condition].type != ValueType::boolean)
                reject(position, "A for condition requires bool.");
            expect(";");
            const auto step = accept(")") ? no_node : simple_statement(")", false);
            ++loop_depth_;
            const auto body = block();
            --loop_depth_;
            Statement loop{Statement::Kind::loop, position, no_node, condition, body, no_node, {}};
            loop.step = step;
            scope.children.push_back(add(std::move(loop)));
            scope.locals.assign(visible_.begin() + visible_before, visible_.end());
            visible_.resize(visible_before);
            return add(std::move(scope));
        }
        if (token_.text == "break" || token_.text == "continue") {
            const auto keyword = consume();
            if (loop_depth_ == 0)
                reject(position, "break and continue require an enclosing loop.");
            expect(";");
            return add(Statement{keyword.text == "break" ? Statement::Kind::break_loop : Statement::Kind::continue_loop,
                                 position,
                                 no_node,
                                 no_node,
                                 no_node,
                                 no_node,
                                 {}});
        }
        if (accept("yield")) {
            if (!function_.is_coroutine)
                reject(position, "yield requires an async entry.");
            expect(";");
            return add(Statement{Statement::Kind::yield, position, no_node, no_node, no_node, no_node, {}});
        }
        if (accept("return")) {
            NodeId value = no_node;
            if (function_.result == ValueType::void_value) {
                if (token_.text != ";")
                    reject(token_.position, "This void entry cannot return a value.");
            } else {
                if (token_.text == ";")
                    reject(token_.position, "Non-void entry requires a return value.");
                value = converted(expression(), function_.result);
            }
            expect(";");
            return add(Statement{Statement::Kind::return_value, position, no_node, value, no_node, no_node, {}});
        }
        return simple_statement(";");
    }
    NodeId expression(int minimum = 0) {
        Depth guard(depth_, token_.position);
        auto left = unary();
        for (;;) {
            const auto operation = binary_operator(token_.text);
            if (operation.precedence < minimum)
                return left;
            const auto symbol = consume();
            const auto right = expression(operation.precedence + 1);
            const bool shift =
                operation.operation == Operation::shift_left || operation.operation == Operation::shift_right;
            if (!shift && function_.expressions[left].type == ValueType::i32 && integer_constant(right))
                signed_constant(right);
            if (!shift && function_.expressions[right].type == ValueType::i32 && integer_constant(left))
                signed_constant(left);
            const auto type = function_.expressions[left].type;
            const auto right_type = function_.expressions[right].type;
            auto result_type = operation.comparison ? ValueType{ValueType::boolean} : type;
            if (operation.operation == Operation::logical_and || operation.operation == Operation::logical_or) {
                if (type != ValueType::boolean || right_type != ValueType::boolean)
                    reject(symbol.position, "Logical operators require bool operands.");
                result_type = ValueType::boolean;
            } else if (shift) {
                if ((type != ValueType::i32 && type != ValueType::u32) ||
                    (right_type != ValueType::i32 && right_type != ValueType::u32))
                    reject(symbol.position, "Shift operands require int or uint.");
            } else if (reference_type(type) || reference_type(right_type)) {
                if (!reference_type(type) || !reference_type(right_type) ||
                    (operation.operation != Operation::equal && operation.operation != Operation::not_equal))
                    reject(symbol.position, "References support only == and != with another reference or null.");
            } else if (math_type(type) || math_type(right_type)) {
                const auto left_math = math_type(type);
                const auto right_math = math_type(right_type);
                const auto op = operation.operation;
                if ((op == Operation::equal || op == Operation::not_equal) && type == right_type) {
                    // Mathematical equality compares all components, with scalar IEEE semantics.
                } else if (left_math.vector() && type == right_type &&
                           (op == Operation::add || op == Operation::subtract)) {
                    result_type = type;
                } else if (op == Operation::multiply && type == right_type &&
                           (left_math.shape == math::Shape::quaternion || left_math.shape == math::Shape::matrix4)) {
                    result_type = type;
                } else if (left_math.vector() && right_type == math_scalar(left_math) &&
                           (op == Operation::multiply || op == Operation::divide)) {
                    result_type = type;
                } else if (right_math.vector() && type == math_scalar(right_math) && op == Operation::multiply) {
                    result_type = right_type;
                } else
                    reject(symbol.position, "Unsupported math operation or mismatched component precision.");
            } else if (!scalar(type) || right_type != type ||
                       ((type.kind == ValueType::enumeration || type == ValueType::boolean) &&
                        operation.operation != Operation::equal && operation.operation != Operation::not_equal)) {
                reject(symbol.position,
                       "Binary operands require matching numeric types; bool and enums support equality only.");
            } else if ((operation.operation == Operation::remainder || operation.operation == Operation::bitwise_and ||
                        operation.operation == Operation::bitwise_or ||
                        operation.operation == Operation::bitwise_xor) &&
                       type != ValueType::i32 && type != ValueType::u32)
                reject(symbol.position, "Remainder and bitwise operators require int or uint.");
            left = add(Expression{Expression::Kind::binary, result_type, symbol.position, left, right, 0,
                                  operation.operation});
        }
    }
    NodeId unary() {
        Depth guard(depth_, token_.position);
        if (token_.text == "!" || token_.text == "~") {
            const auto symbol = consume();
            const auto operand = unary();
            const auto type = function_.expressions[operand].type;
            if (symbol.text == "!" && type != ValueType::boolean)
                reject(symbol.position, "Logical negation requires bool.");
            if (symbol.text == "~" && type != ValueType::i32 && type != ValueType::u32)
                reject(symbol.position, "Bitwise negation requires int or uint.");
            return add(Expression{Expression::Kind::unary, type, symbol.position, operand, no_node, 0,
                                  symbol.text == "!" ? Operation::logical_not : Operation::bitwise_not});
        }
        if (token_.text == "+" || token_.text == "-") {
            const auto symbol = consume();
            const auto operand = unary();
            if (symbol.text == "-" && function_.expressions[operand].kind == Expression::Kind::literal &&
                !function_.expressions[operand].typed_constant &&
                function_.expressions[operand].type == ValueType::u32 &&
                function_.expressions[operand].bits == 2147483648u)
                return literal(ValueType::i32, symbol.position, 2147483648u);
            if (symbol.text == "-" && integer_constant(operand))
                signed_constant(operand);
            const auto type = function_.expressions[operand].type;
            if (!floating(type) && type != ValueType::u32 && type != ValueType::i32 && !math_type(type))
                reject(symbol.position, "Unary arithmetic requires a numeric operand.");
            if (symbol.text == "-" && type == ValueType::u32)
                reject(symbol.position,
                       "Unary minus requires a signed numeric value; use uint subtraction for modulo arithmetic.");
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
                if (type.kind != ValueType::array && type.kind != ValueType::string)
                    reject(position, "Indexing requires Array or String.");
                const auto index = expression();
                expect("]");
                if (function_.expressions[index].type != ValueType::u32 &&
                    function_.expressions[index].type != ValueType::i32)
                    reject(position, "A sequence index requires uint or int.");
                const auto element = type.kind == ValueType::string ? ValueType{ValueType::u32}
                                                                    : environment_.values[type.type_index].element;
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
                        if (!owned_storage(receiver) || !environment_.values[type.type_index].can_own ||
                            !environment_.values[type.type_index].can_mutate)
                            reject(member.position, "Structural mutation requires a locally owned sequence.");
                        SequenceMutation operation;
                        if (member.text == "reserve")
                            operation = SequenceMutation::reserve;
                        else if (member.text == "resize" && type.kind == ValueType::array)
                            operation = SequenceMutation::resize;
                        else if (member.text == "clear")
                            operation = SequenceMutation::clear;
                        else if (member.text == "add" && type.kind == ValueType::array)
                            operation = SequenceMutation::add;
                        else if (member.text == "append")
                            operation = SequenceMutation::append;
                        else
                            reject(member.position, "Unknown native sequence operation.");
                        NodeId input = no_node;
                        if (operation != SequenceMutation::clear) {
                            input = expression();
                            if (operation == SequenceMutation::reserve || operation == SequenceMutation::resize) {
                                const auto input_type = function_.expressions[input].type;
                                if (input_type != ValueType::u32 && input_type != ValueType::i32)
                                    reject(member.position, "Sequence capacity requires uint or int.");
                            } else
                                input = converted(input, operation == SequenceMutation::add
                                                             ? environment_.values[type.type_index].element
                                                             : type);
                        }
                        expect(")");
                        Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position,
                                            receiver, input};
                        mutation.mutation = operation;
                        receiver = add(std::move(mutation));
                    }
                } else {
                    const auto& fields = environment_.values[type.type_index].fields;
                    const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& candidate) {
                        return candidate.name == member.text;
                    });
                    if (field == fields.end())
                        reject(member.position, "Field has no published direct native layout.");
                    const auto& owner = function_.expressions[receiver];
                    if (owner.kind == Expression::Kind::field) {
                        const auto path = std::string(environment_.fields[owner.value].name) + "." + field->name;
                        const auto projection = std::ranges::find_if(environment_.fields, [&](const auto& candidate) {
                            return candidate.name == path && candidate.type == field->type;
                        });
                        if (projection != environment_.fields.end()) {
                            receiver =
                                add(Expression{Expression::Kind::field, projection->type, position, no_node, no_node,
                                               static_cast<std::uint32_t>(projection - environment_.fields.begin()),
                                               Operation::add, owner.receiver});
                            continue;
                        }
                    }
                    Expression access{Expression::Kind::value_field, field->type, position, receiver};
                    access.offset = field->offset;
                    receiver = add(std::move(access));
                }
                continue;
            }
            if (function_.expressions[receiver].type != ValueType::strong_reference &&
                function_.expressions[receiver].type != ValueType::weak_reference)
                reject(position, "Member access requires a typed object reference.");
            const auto member = expression_name(true);
            if (accept("(")) {
                const MethodBinding* method{};
                for (const auto& candidate : environment_.methods)
                    if (candidate.name == member)
                        method = &candidate;
                if (!method)
                    reject(position, "Unknown native method '" + member + "'.");
                receiver = add(call_expression(true, static_cast<std::uint32_t>(method - environment_.methods.data()),
                                               position, receiver, call_arguments(method->parameters)));
            } else if (const auto* property = find_property(member, true)) {
                receiver = property_access(*property, position, receiver);
            } else {
                const FieldBinding* field{};
                for (const auto& candidate : environment_.fields)
                    if (candidate.name == member)
                        field = &candidate;
                if (!field)
                    reject(position, "Unknown receiver field '" + member + "'.");
                receiver = add(Expression{Expression::Kind::field, field->type, position, no_node, no_node,
                                          static_cast<std::uint32_t>(field - environment_.fields.data()),
                                          Operation::add, receiver});
            }
        }
        return receiver;
    }
    NodeId numeric_literal(const Token& token) {
        auto text = token.text;
        const auto suffix = text.back();
        const bool is_double = suffix == 'd' || suffix == 'D';
        const bool is_float = suffix == 'f' || suffix == 'F';
        if (is_float || is_double)
            text.remove_suffix(1);
        if (is_double) {
            double value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                reject(token.position, "Double literal is out of range or invalid.");
            return literal(ValueType::f64, token.position, std::bit_cast<std::uint64_t>(value));
        }
        if (is_float || text.find_first_of(".eE") != std::string_view::npos) {
            float value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                reject(token.position, "Float literal is out of range or invalid.");
            return literal(ValueType::f32, token.position, std::bit_cast<std::uint32_t>(value));
        }
        std::uint32_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            reject(token.position, "Uint literal is outside 0..4294967295.");
        return literal(ValueType::u32, token.position, value);
    }
    NodeId math_call(std::string_view name, SourcePosition position, ValueType constructed) {
        std::vector<NodeId> arguments;
        if (!accept(")")) {
            do {
                if (arguments.size() == 16)
                    reject(token_.position, "A math call accepts at most 16 scalar components or operands.");
                arguments.push_back(expression());
            } while (accept(","));
            expect(")");
        }
        if (constructed != ValueType::void_value) {
            const auto descriptor = math_type(constructed);
            if (!arguments.empty() && arguments.size() != descriptor.components() &&
                !(descriptor.shape == math::Shape::matrix4 && arguments.size() == 1))
                reject(
                    position,
                    "Math construction requires no arguments, one scalar per component, or one diagonal scalar for a matrix.");
            for (auto& argument : arguments)
                argument = converted(argument, math_scalar(descriptor));
            Expression result{Expression::Kind::math_construct, constructed, position};
            result.arguments = std::move(arguments);
            return add(std::move(result));
        }
        MathOperation operation;
        std::size_t arity = 1;
        if (name == "math.dot") {
            operation = MathOperation::dot;
            arity = 2;
        } else if (name == "math.cross") {
            operation = MathOperation::cross;
            arity = 2;
        } else if (name == "math.length")
            operation = MathOperation::length;
        else if (name == "math.normalize_or_zero")
            operation = MathOperation::normalize_or_zero;
        else if (name == "math.conjugate")
            operation = MathOperation::conjugate;
        else if (name == "math.rotate") {
            operation = MathOperation::rotate;
            arity = 2;
        } else if (name == "math.transpose")
            operation = MathOperation::transpose;
        else if (name == "math.transform_point") {
            operation = MathOperation::transform_point;
            arity = 2;
        } else if (name == "math.transform_direction") {
            operation = MathOperation::transform_direction;
            arity = 2;
        } else if (name == "math.sqrt")
            operation = MathOperation::square_root;
        else if (name == "math.floor")
            operation = MathOperation::floor;
        else if (name == "math.ceil")
            operation = MathOperation::ceil;
        else if (name == "math.trunc")
            operation = MathOperation::trunc;
        else if (name == "math.fract")
            operation = MathOperation::fract;
        else if (name == "math.abs")
            operation = MathOperation::abs;
        else if (name == "math.sign")
            operation = MathOperation::sign;
        else if (name == "math.min") {
            operation = MathOperation::min;
            arity = 2;
        } else if (name == "math.max") {
            operation = MathOperation::max;
            arity = 2;
        } else if (name == "math.step") {
            operation = MathOperation::step;
            arity = 2;
        } else if (name == "math.clamp") {
            operation = MathOperation::clamp;
            arity = 3;
        } else if (name == "math.lerp") {
            operation = MathOperation::lerp;
            arity = 3;
        } else if (name == "math.smoothstep") {
            operation = MathOperation::smoothstep;
            arity = 3;
        } else if (name == "math.sin")
            operation = MathOperation::sin;
        else if (name == "math.cos")
            operation = MathOperation::cos;
        else if (name == "math.atan2") {
            operation = MathOperation::atan2;
            arity = 2;
        } else if (name == "math.exp")
            operation = MathOperation::exp;
        else if (name == "math.log")
            operation = MathOperation::log;
        else if (name == "math.pow") {
            operation = MathOperation::pow;
            arity = 2;
        } else
            reject(position, "Unknown built-in math operation '" + std::string(name) + "'.");
        if (arguments.size() != arity)
            reject(position, "Math operation argument count mismatch.");
        const auto first = function_.expressions[arguments[0]].type;
        const auto descriptor = math_type(first);
        auto result_type = first;
        if (operation >= MathOperation::square_root) {
            // Element-wise: the result is the vector argument's type, or the scalar
            // type when every argument is scalar; scalar arguments apply to every lane.
            for (const auto argument : arguments) {
                const auto type = function_.expressions[argument].type;
                if (math_type(type).vector()) {
                    result_type = type;
                    break;
                }
            }
            const auto lane = math_type(result_type).vector() ? math_scalar(math_type(result_type)) : result_type;
            if (!floating(lane))
                reject(position, std::string(name) + " requires float or double scalars or vectors.");
            for (const auto argument : arguments) {
                const auto type = function_.expressions[argument].type;
                if (type != result_type && type != lane)
                    reject(
                        function_.expressions[argument].position,
                        std::string(name) +
                            " arguments must have the result type or its component type; numeric types and precision do not convert implicitly.");
            }
        } else
            switch (operation) {
            case MathOperation::dot:
            case MathOperation::cross:
                if (!descriptor.vector() ||
                    (operation == MathOperation::cross && descriptor.shape != math::Shape::vector3))
                    reject(position, "math.dot requires vectors; math.cross requires vector3.");
                arguments[1] = converted(arguments[1], first);
                if (operation == MathOperation::dot)
                    result_type = math_scalar(descriptor);
                break;
            case MathOperation::length:
            case MathOperation::normalize_or_zero:
                if (!descriptor.vector() && descriptor.shape != math::Shape::quaternion)
                    reject(position, "Length and normalization require a vector or quaternion.");
                if (operation == MathOperation::length)
                    result_type = math_scalar(descriptor);
                break;
            case MathOperation::conjugate:
                if (descriptor.shape != math::Shape::quaternion)
                    reject(position, "math.conjugate requires a quaternion.");
                break;
            case MathOperation::transpose:
                if (descriptor.shape != math::Shape::matrix4)
                    reject(position, "math.transpose requires a matrix4.");
                break;
            case MathOperation::rotate: {
                const auto second = function_.expressions[arguments[1]].type;
                const auto vector = math_type(second);
                if (descriptor.shape != math::Shape::quaternion || vector.shape != math::Shape::vector3 ||
                    descriptor.precision != vector.precision)
                    reject(position, "math.rotate requires a quaternion followed by a vector3 of matching precision.");
                result_type = second;
                break;
            }
            case MathOperation::transform_point:
            case MathOperation::transform_direction: {
                const auto second = function_.expressions[arguments[1]].type;
                const auto matrix = math_type(second);
                if (descriptor.shape != math::Shape::vector3 || matrix.shape != math::Shape::matrix4 ||
                    descriptor.precision != matrix.precision)
                    reject(position,
                           "Transform operations require a vector3 followed by a matrix4 of matching precision.");
                break;
            }
            default:
                break;
            }
        Expression result{Expression::Kind::math_intrinsic, result_type, position};
        result.math_operation = operation;
        result.arguments = std::move(arguments);
        return add(std::move(result));
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
            auto value = string_value(token);
            for (std::uint32_t i = 0; i < environment_.values.size(); ++i) {
                if (environment_.values[i].kind != ValueType::string || !environment_.values[i].can_own ||
                    !environment_.values[i].can_mutate)
                    continue;
                function_.uses_scoped_values = true;
                const auto index = static_cast<std::uint32_t>(function_.strings.size());
                function_.strings.push_back(std::move(value));
                return add(Expression{
                    Expression::Kind::string_literal, {ValueType::string, i}, token.position, no_node, no_node, index});
            }
            reject(token.position, "String literals require the registered foundation String.");
        }
        if (token.kind == Token::Kind::number) {
            consume();
            return numeric_literal(token);
        }
        if (token.kind != Token::Kind::identifier)
            reject(token.position, "Expected an expression.");
        if (environment_.material && (token.text == "sample" || token.text == "sample_orientation")) {
            const bool orientation = token.text == "sample_orientation";
            const auto label = "Material " + std::string(token.text);
            consume();
            expect("(");
            const auto input = expression();
            if (function_.expressions[input].type != environment_.texture_input)
                reject(token.position, label + " requires the registered TextureInput value, not a numeric slot.");
            expect(",");
            const auto uv = expression();
            expect(")");
            if (function_.expressions[uv].type != math_named_type("vector2"))
                reject(token.position, label + " coordinates require vector2.");
            return add(
                Expression{orientation ? Expression::Kind::texture_orientation : Expression::Kind::texture_sample,
                           math_named_type(orientation ? "vector2" : "vector4"), token.position, uv, input});
        }
        if (token.text == "float" || token.text == "double" || token.text == "int" || token.text == "uint") {
            consume();
            expect("(");
            const auto argument = expression();
            expect(")");
            const auto type = function_.expressions[argument].type;
            if (!floating(type) && type != ValueType::i32 && type != ValueType::u32)
                reject(token.position, "Numeric conversion requires float, double, int or uint.");
            return add(Expression{Expression::Kind::scalar_cast, named_type(token.text, token.position), token.position,
                                  argument});
        }
        {
            auto lookahead = lexer_;
            auto next = lookahead.next();
            std::string name(token.text);
            while (next.text == ".") {
                const auto segment = lookahead.next();
                name += ".";
                name += segment.text;
                next = lookahead.next();
            }
            if (next.text == "(" && (name.starts_with("math.") || math_named_type(name) != ValueType::void_value)) {
                const auto constructed = math_named_type(name);
                const auto actual_name = qualified_identifier();
                expect("(");
                return math_call(actual_name, token.position, constructed);
            }
            if (next.text == "::") {
                const auto name_actual = qualified_identifier();
                expect("::");
                const auto item = identifier();
                for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i) {
                    const auto& enumeration = environment_.enumerations[i];
                    if (enumeration.name != name_actual)
                        continue;
                    for (const auto& value : enumeration.values)
                        if (value.name == item.text)
                            return literal({ValueType::enumeration, i}, token.position, value.bits);
                    reject(item.position, "Unknown enumerator.");
                }
                reject(token.position, "Unknown enum type.");
            }
            // Any other dotted name not led by a local or self is a qualified module function or constant.
            if (name.size() != token.text.size() && token.text != "self" &&
                std::ranges::none_of(visible_,
                                     [&](auto local) { return function_.locals[local].name == token.text; })) {
                const auto qualified = expression_name(false);
                if (accept("("))
                    return source_call(qualified, token.position);
                return module_constant(qualified, token.position);
            }
        }
        consume();
        if (token.text == "true" || token.text == "false")
            return literal(ValueType::boolean, token.position, token.text == "true" ? 1 : 0);
        if (token.text == "self")
            return add(Expression{Expression::Kind::receiver, ValueType::strong_reference, token.position});
        if (token.text == "null")
            return add(Expression{Expression::Kind::null_reference, ValueType::null_reference, token.position});
        if (token.text == "new") {
            object_type();
            expect("(");
            if (token_.text != ")")
                reject(token_.position, "Object creation does not accept constructor arguments.");
            consume();
            return add(Expression{Expression::Kind::create_reference, ValueType::strong_reference, token.position});
        }
        if (token.text == "observe" || token.text == "try_acquire") {
            const auto observe = token.text == "observe";
            expect("(");
            if (token_.text == ")")
                reject(token_.position, "Reference operation requires one argument.");
            const auto argument = expression();
            const auto required = observe ? ValueType::strong_reference : ValueType::weak_reference;
            if (function_.expressions[argument].type != required)
                reject(function_.expressions[argument].position,
                       observe ? "observe requires a strong reference argument."
                               : "try_acquire requires a weak reference argument.");
            if (token_.text == ",")
                reject(token_.position, "Reference operation requires exactly one argument.");
            expect(")");
            return add(Expression{observe ? Expression::Kind::observe_reference : Expression::Kind::acquire_reference,
                                  observe ? ValueType::weak_reference : ValueType::strong_reference, token.position,
                                  argument});
        }
        for (auto iterator = visible_.rbegin(); iterator != visible_.rend(); ++iterator) {
            const auto& local_value = function_.locals[*iterator];
            if (local_value.name == token.text)
                return add(
                    Expression{Expression::Kind::local, local_value.type, token.position, no_node, no_node, *iterator});
        }
        if (accept("("))
            return source_call(token.text, token.position);
        return module_constant(token.text, token.position);
    }
    NodeId module_constant(std::string_view name, SourcePosition position) {
        if (const auto* property = find_property(name, false))
            return property_access(*property, position);
        for (std::size_t i = 0; i < constants_.size(); ++i) {
            if (constants_[i].name != name)
                continue;
            resolve_constant(i);
            const auto node = literal(constants_[i].type, position, constants_[i].bits);
            function_.expressions[node].typed_constant = true;
            return node;
        }
        reject(position, "Unknown identifier '" + std::string(name) + "'.");
    }
    // Called after the opening parenthesis.
    NodeId source_call(std::string_view name, SourcePosition position) {
        for (std::uint32_t i = 0; i < module_.functions.size(); ++i) {
            const auto& target = module_.functions[i];
            if (target.function_name != name)
                continue;
            return add(call_expression(false, i, position, no_node, call_arguments(target.parameters)));
        }
        reject(position, "Unknown source function '" + std::string(name) + "'.");
    }

    Environment environment_;
    struct Constant {
        std::string name;
        ValueType type;
        SourcePosition position;
        Lexer lexer;
        Token token;
        enum class State { fresh, active, complete } state{};
        std::uint64_t bits{};
    };
    std::vector<Constant> constants_;
    Function function_;
    Program module_;
    std::vector<std::uint32_t> visible_;
    std::vector<std::size_t> expression_heights_;
    std::size_t depth_{};
    std::size_t loop_depth_{};
};
}

std::expected<Program, Diagnostic> compile_source(std::string_view source_name, std::string_view source,
                                                  Environment environment, std::string_view entry_name,
                                                  bool allow_declarations) {
    try {
        return Parser(source_name, source, environment).parse(entry_name, allow_declarations);
    } catch (Rejected& rejected) {
        return std::unexpected(std::move(rejected.diagnostic));
    }
}

std::expected<void, Diagnostic> export_functions(Program& program, std::span<const std::string_view> names,
                                                 bool receiver) {
    if (names.empty() || !program.exports.empty())
        return std::unexpected(Diagnostic{{}, "Source exports require a nonempty, previously unexported module."});
    const auto count = program.functions.size();
    for (const auto name : names) {
        if (std::ranges::any_of(program.exports, [&](auto i) { return program.functions[i].function_name == name; }))
            return std::unexpected(Diagnostic{{}, "Duplicate source export: " + std::string(name)});
        const auto found = std::find_if(program.functions.begin(), program.functions.begin() + count,
                                        [&](const auto& f) { return f.function_name == name; });
        if (found == program.functions.begin() + count)
            return std::unexpected(Diagnostic{{}, "Exported source function was not declared: " + std::string(name)});
        if (found->is_coroutine)
            return std::unexpected(Diagnostic{found->position, "Project exports must be synchronous."});
        const auto target = static_cast<std::uint32_t>(found - program.functions.begin());
        Function wrapper;
        wrapper.function_name = found->function_name;
        wrapper.position = found->position;
        wrapper.parameters = found->parameters;
        wrapper.result = found->result;
        wrapper.locals.assign(found->locals.begin(), found->locals.begin() + found->parameters.size());
        wrapper.uses_scoped_values = found->uses_scoped_values;
        wrapper.preserves_borrows = found->preserves_borrows;
        wrapper.requires_receiver = receiver || found->requires_receiver;
        Expression call{Expression::Kind::source_call, wrapper.result, wrapper.position};
        call.value = target;
        for (std::uint32_t i = 0; i < wrapper.parameters.size(); ++i) {
            Expression argument{Expression::Kind::local, wrapper.parameters[i].type, wrapper.position};
            argument.value = i;
            wrapper.expressions.push_back(std::move(argument));
            call.arguments.push_back(i);
            call.argument_passing.push_back(wrapper.parameters[i].passing);
        }
        const auto call_id = wrapper.expressions.size();
        wrapper.expressions.push_back(std::move(call));
        Statement action{};
        action.kind = wrapper.result == ValueType::void_value ? Statement::Kind::call : Statement::Kind::return_value;
        action.position = wrapper.position;
        action.expression = call_id;
        wrapper.statements.push_back(std::move(action));
        Statement block{};
        block.kind = Statement::Kind::block;
        block.position = wrapper.position;
        block.children = {0};
        wrapper.body = 1;
        wrapper.statements.push_back(std::move(block));
        program.exports.push_back(static_cast<std::uint32_t>(program.functions.size()));
        program.functions.push_back(std::move(wrapper));
    }
    program.reachable.assign(program.functions.size(), false);
    auto pending = program.exports;
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (program.reachable[index])
            continue;
        program.reachable[index] = true;
        for (const auto& node : program.functions[index].expressions)
            if (node.kind == Expression::Kind::source_call)
                pending.push_back(node.value);
    }
    program.entry = program.exports.front();
    return {};
}

}
