#include "language/source.hpp"
#include "language/lexer.hpp"
#include "language/declarations.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <type_traits>
#include <unordered_map>
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

std::optional<Operation> compound_assignment_operator(std::string_view text) noexcept {
    if (text == "+=")
        return Operation::add;
    if (text == "-=")
        return Operation::subtract;
    if (text == "*=")
        return Operation::multiply;
    if (text == "/=")
        return Operation::divide;
    if (text == "%=")
        return Operation::remainder;
    if (text == "&=")
        return Operation::bitwise_and;
    if (text == "|=")
        return Operation::bitwise_or;
    if (text == "^=")
        return Operation::bitwise_xor;
    if (text == "<<=")
        return Operation::shift_left;
    if (text == ">>=")
        return Operation::shift_right;
    return std::nullopt;
}

class Parser : private Tokens {
public:
    Parser(std::string_view name, Tokens tokens, Environment environment)
        : Tokens(std::move(tokens)), environment_(environment) {
        module_.source_name = name;
    }

    std::expected<Program, Diagnostic> parse(std::string_view entry_name, bool allow_declarations,
                                             bool allow_parameter_attributes = false) {
        ATELIER_TRY(validate_environment());
        module_.object_type_name = environment_.object_type_name;
        module_.value_layouts.assign(environment_.values.begin(), environment_.values.end());
        struct Body {
            Lexer lexer;

            Token token;
        };
        std::vector<Body> bodies;
        std::vector<std::string> type_names;
        while (token_.kind != Token::Kind::end) {
            ATELIER_TRY_BIND(keyword, peek_declaration_keyword(*this));
            if (*keyword == "struct" || *keyword == "enum") {
                ATELIER_TRY_BIND(declaration, parse_type_declaration(*this, *keyword));
                if (type_names.size() == 1024)
                    return reject(declaration->position, "A source module supports at most 1024 type declarations.");
                if (std::ranges::find(type_names, declaration->name) != type_names.end())
                    return reject(declaration->position, "Duplicate source type name.");
                ATELIER_TRY(validate_declaration(*declaration));
                type_names.push_back(std::move(declaration->name));
                continue;
            }
            std::string function_description;
            if (token_.text == "[")
                ATELIER_TRY_ASSIGN(function_description, parse_description_attribute(*this, "function"));
            ATELIER_TRY_BIND(constant, accept("const"));
            if (*constant) {
                const auto position = token_.position;
                ATELIER_TRY_BIND(type, parameter_type());
                if (environment_.material) {
                    const auto mtype = math_type(*type);
                    if (*type != ValueType::f32 && *type != ValueType::boolean &&
                        !(mtype.vector() && mtype.precision == math::Precision::f32))
                        return reject(position, "Material constants require float, bool, or float vector types.");
                }
                const bool valid_constant_type =
                    scalar(*type) || bool(math_type(*type)) || (plain_record(*type) && parallel_value_type(*type)) ||
                    (type->kind == ValueType::array && environment_.values[type->type_index].standard_array_layout &&
                     environment_.values[type->type_index].can_own &&
                     environment_.values[type->type_index].can_mutate && parallel_sequence_type(*type));
                if (!valid_constant_type)
                    return reject(
                        position,
                        "Module constants require a scalar, enum, built-in math, plain struct, or Array of plain values.");
                const auto name_position = token_.position;
                ATELIER_TRY_BIND(name, qualified_identifier());
                if (math_reserved_name(*name))
                    return reject(name_position, "Module constants cannot replace built-in math names.");
                if (constant_indices_.contains(*name))
                    return reject(name_position, "Duplicate module constant name.");
                if (constants_.size() == 1024)
                    return reject(position, "A source module supports at most 1024 constants.");
                ATELIER_TRY(expect("="));
                constant_indices_.emplace(*name, static_cast<std::uint32_t>(constants_.size()));
                constants_.push_back({std::move(*name), *type, name_position, lexer_, token_});
                ATELIER_TRY(skip_constant_initializer(*this));
                continue;
            }
            if (module_.functions.size() >= 1024)
                return reject(token_.position, "A source module supports at most 1024 functions.");
            function_ = {};
            function_.position = token_.position;
            function_.description = std::move(function_description);
            visible_.clear();
            ATELIER_TRY(signature(allow_parameter_attributes));
            for (const auto& previous : module_.functions)
                if (previous.function_name == function_.function_name)
                    return reject(function_.position, "Duplicate source function name.");
            bodies.push_back({lexer_, token_});
            module_.functions.push_back(std::move(function_));
            ATELIER_TRY_ASSIGN(module_.functions.back().end_offset, skip_function_body(*this));
        }
        if (module_.functions.empty() && !allow_declarations)
            return reject(token_.position, "Source module requires an entry function.");
        for (const auto& function : module_.functions)
            if (std::ranges::find(type_names, function.function_name) != type_names.end())
                return reject(function.position, "A source function and type cannot have the same name.");
        for (const auto& value : constants_) {
            if (std::ranges::find(type_names, value.name) != type_names.end() ||
                std::ranges::any_of(module_.functions,
                                    [&](const auto& function) { return function.function_name == value.name; }))
                return reject(value.position, "A module constant cannot share a name with a source type or function.");
            for (const auto& function : module_.functions)
                for (const auto& parameter : function.locals)
                    if (parameter.name == value.name)
                        return reject(function.position, "Parameters cannot shadow a module constant.");
        }
        ATELIER_TRY(collect_properties());
        for (std::size_t i = 0; i < constants_.size(); ++i) {
            ATELIER_TRY(resolve_constant(i));
        }
        if (module_.functions.empty())
            return std::move(module_);
        if (entry_name.empty() && module_.functions.size() != 1 && !allow_declarations)
            return reject({}, "A multi-function module requires an explicit host entry name.");
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
            ATELIER_TRY_BIND(body, block());
            function_.body = *body;
            ATELIER_TRY(finish_properties());
            if (function_.result != ValueType::void_value && !returns(function_.body))
                return reject(function_.position, "Non-void function can reach the end without a return value.");
            module_.functions[i] = std::move(function_);
        }
        if (!found)
            return reject({}, "Selected entry function was not declared in this module.");
        ATELIER_TRY(analyze_calls());
        return std::move(module_);
    }

    static void mark_reachable_parallel_callees(Program& program) {
        for (auto& function : program.functions)
            function.used_in_parallel = false;
        std::vector<std::uint32_t> pending;
        for (std::size_t i = 0; i < program.functions.size(); ++i) {
            if (i >= program.reachable.size() || !program.reachable[i])
                continue;
            const auto& function = program.functions[i];
            auto scan_expr = [&](auto&& self, NodeId id) -> void {
                if (id == no_node)
                    return;
                const auto& expr = function.expressions[id];
                if (expr.kind == Expression::Kind::source_call)
                    pending.push_back(expr.value);
                self(self, expr.left);
                self(self, expr.right);
                self(self, expr.receiver);
                for (const auto arg : expr.arguments)
                    self(self, arg);
            };
            auto scan_stmt = [&](auto&& self, NodeId id) -> void {
                if (id == no_node)
                    return;
                const auto& stmt = function.statements[id];
                using Kind = Statement::Kind;
                switch (stmt.kind) {
                case Kind::block:
                    for (const auto child : stmt.children)
                        self(self, child);
                    return;
                case Kind::declare:
                case Kind::call:
                    scan_expr(scan_expr, stmt.expression);
                    return;
                case Kind::assign:
                    scan_expr(scan_expr, stmt.target);
                    scan_expr(scan_expr, stmt.expression);
                    return;
                case Kind::condition:
                    scan_expr(scan_expr, stmt.expression);
                    self(self, stmt.body);
                    self(self, stmt.alternative);
                    return;
                case Kind::loop:
                    scan_expr(scan_expr, stmt.expression);
                    self(self, stmt.body);
                    self(self, stmt.step);
                    return;
                case Kind::fail:
                    scan_expr(scan_expr, stmt.body);
                    scan_expr(scan_expr, stmt.expression);
                    return;
                default:
                    return;
                }
            };
            for (const auto& root : function.statements)
                if (root.kind == Statement::Kind::parallel_loop)
                    scan_stmt(scan_stmt, root.body);
        }
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            auto& callee = program.functions[index];
            if (callee.used_in_parallel)
                continue;
            callee.used_in_parallel = true;
            for (const auto& expr : callee.expressions)
                if (expr.kind == Expression::Kind::source_call)
                    pending.push_back(expr.value);
        }
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

    bool parallel_value_type(ValueType type) const noexcept {
        if (scalar(type) || bool(math_type(type)))
            return true;
        if (type.kind != ValueType::record || type.type_index >= environment_.values.size())
            return false;
        const auto& layout = environment_.values[type.type_index];
        return layout.plain_value && layout.can_own && (layout.zero_default || !layout.initial_value.empty());
    }

    bool parallel_sequence_type(ValueType type) const noexcept {
        if (type.kind == ValueType::string && type.type_index < environment_.values.size())
            return true;
        return type.kind == ValueType::array && type.type_index < environment_.values.size() &&
               parallel_value_type(environment_.values[type.type_index].element);
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

    std::expected<ValueType, Diagnostic> named_type(std::string_view name, SourcePosition position) const {
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
            if (!synthesized_array_name(environment_.values[i].name) &&
                (environment_.values[i].name == name ||
                 (name == "String" && environment_.values[i].kind == ValueType::string)))
                return ValueType{environment_.values[i].kind, i};
        for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i)
            if (environment_.enumerations[i].name == name)
                return ValueType{ValueType::enumeration, i};
        const auto math = math_named_type(name);
        if (math != ValueType::void_value)
            return math;
        return reject(position, "Unknown registered value type '" + std::string(name) + "'.");
    }

    std::expected<ValueType, Diagnostic> declared_type(const TypeName& name, SourcePosition position) const {
        ATELIER_TRY_BIND(type, named_type(name.name, position));
        for (std::uint32_t depth = 0; depth < name.array_depth; ++depth) {
            const auto array = std::ranges::find_if(environment_.values, [&](const auto& value) {
                return value.kind == ValueType::array && value.standard_array_layout && value.element == *type;
            });
            if (array == environment_.values.end())
                return reject(position, "This Array element type has not been registered in the Runtime.");
            *type = {ValueType::array, static_cast<std::uint32_t>(array - environment_.values.begin())};
        }
        return type;
    }

    std::expected<void, Diagnostic> validate_declaration(const TypeDeclaration& declaration) const {
        ATELIER_TRY_BIND(type, named_type(declaration.name, declaration.position));
        if (declaration.enumeration) {
            if (type->kind != ValueType::enumeration)
                return reject(declaration.position, "Source enum conflicts with its Runtime type.");
            const auto& layout = environment_.enumerations[type->type_index];
            if (layout.underlying != (declaration.signed_enum ? ValueType::i32 : ValueType::u32) ||
                layout.flags != declaration.flags || layout.values.size() != declaration.enumerators.size())
                return reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
            for (std::size_t i = 0; i < layout.values.size(); ++i)
                if (layout.values[i].name != declaration.enumerators[i].first ||
                    layout.values[i].bits != declaration.enumerators[i].second)
                    return reject(declaration.position, "Source enum differs from its prepared Runtime definition.");
        } else {
            if (type->kind != ValueType::record)
                return reject(declaration.position, "Source struct conflicts with its Runtime type.");
            const auto& layout = environment_.values[type->type_index];
            if (layout.fields.size() != declaration.fields.size())
                return reject(declaration.position, "Source struct differs from its prepared Runtime definition.");
            for (std::size_t i = 0; i < layout.fields.size(); ++i) {
                if (layout.fields[i].name != declaration.fields[i].first)
                    return reject(declaration.position, "Source struct differs from its prepared Runtime definition.");
                ATELIER_TRY_BIND(field_type, declared_type(declaration.fields[i].second, declaration.position));
                if (layout.fields[i].type != *field_type)
                    return reject(declaration.position, "Source struct differs from its prepared Runtime definition.");
            }
        }
        return {};
    }

    std::expected<void, Diagnostic> signature(bool allow_parameter_attributes) {
        ATELIER_TRY_BIND(asynchronous, accept("async"));
        function_.is_coroutine = *asynchronous;
        ATELIER_TRY_BIND(void_result, accept("void"));
        if (*void_result)
            function_.result = ValueType::void_value;
        else {
            ATELIER_TRY_BIND(result, result_type());
            function_.result = *result;
        }
        if (function_.is_coroutine && aggregate(function_.result))
            return reject(function_.position, "async aggregate results are not supported by the task result contract.");
        if (plain_record(function_.result) && !math_type(function_.result))
            function_.uses_scoped_values = true;
        const auto name_position = token_.position;
        ATELIER_TRY_BIND(name, qualified_identifier());
        if (math_reserved_name(*name))
            return reject(name_position, "Source functions cannot replace math constructors or the math namespace.");
        function_.function_name = std::move(*name);
        ATELIER_TRY(expect("("));
        ATELIER_TRY_BIND(empty, accept(")"));
        if (!*empty) {
            for (;;) {
                std::vector<std::pair<std::string, std::string>> metadata;
                ATELIER_TRY(parse_parameter_attributes(*this, allow_parameter_attributes, metadata));
                const auto position = token_.position;
                ATELIER_TRY_BIND(read_only, accept("const"));
                ATELIER_TRY_BIND(type, parameter_type());
                if (reference_type(*type))
                    return reject(position, "Object reference parameters are not supported.");
                ATELIER_TRY_BIND(reference, accept("&"));
                if (*read_only && !*reference)
                    return reject(position, "const parameters require a reference in this source slice.");
                const auto passing = *reference
                                         ? (*read_only ? ParameterPassing::read_only : ParameterPassing::reference)
                                         : ParameterPassing::value;
                if (function_.is_coroutine && *reference)
                    return reject(position, "async parameters cannot borrow caller storage.");
                if (aggregate(*type)) {
                    if (function_.is_coroutine)
                        return reject(position,
                                      "async aggregate parameters are not supported by the task argument contract.");
                    if (!*reference && !plain_record(*type))
                        return reject(
                            position,
                            "Only plain records support value parameters; other native values require Type& or const Type&.");
                    if (!*reference && !math_type(*type) && !environment_.values[type->type_index].can_own)
                        return reject(
                            position,
                            "Plain record value parameters require borrow-preserving copy and lifecycle operations.");
                    if (*reference || !math_type(*type))
                        function_.uses_scoped_values = true;
                }
                ATELIER_TRY_BIND(parameter_name, identifier());
                ATELIER_TRY_BIND(index, local(*parameter_name, *type));
                function_.locals[*index].read_only = *read_only;
                function_.locals[*index].fixed_length = std::ranges::any_of(metadata, [](const auto& item) {
                    return item.first == "procedural.role" && (item.second == "inout" || item.second == "out");
                });
                function_.parameters.push_back({*type, passing});
                ATELIER_TRY_BIND(more, accept(","));
                if (!*more)
                    break;
            }
            ATELIER_TRY(expect(")"));
        }
        return {};
    }

    static std::optional<std::uint32_t> root_local(const Function& function, NodeId id) noexcept {
        while (id != no_node) {
            const auto& expr = function.expressions[id];
            if (expr.kind == Expression::Kind::local)
                return expr.value;
            if (expr.kind == Expression::Kind::value_field || expr.kind == Expression::Kind::index) {
                id = expr.left;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    bool borrows_constant_data(NodeId id) const noexcept {
        while (id != no_node) {
            const auto& expr = function_.expressions[id];
            if (expr.kind == Expression::Kind::constant_data)
                return true;
            if (expr.kind == Expression::Kind::value_field) {
                id = expr.left;
                continue;
            }
            if (expr.kind == Expression::Kind::index &&
                function_.expressions[expr.left].type.kind != ValueType::string) {
                id = expr.left;
                continue;
            }
            return false;
        }
        return false;
    }

    static void mark_structure_mutation(Function& function, NodeId id) noexcept {
        if (const auto root = root_local(function, id);
            root && *root < function.parameters.size() &&
            function.parameters[*root].passing == ParameterPassing::reference)
            function.parameters[*root].mutates_structure = true;
    }

    static void mark_length_change(Function& function, NodeId id) noexcept {
        const auto& expr = function.expressions[id];
        if (expr.kind == Expression::Kind::local && expr.value < function.parameters.size() &&
            function.parameters[expr.value].passing == ParameterPassing::reference)
            function.parameters[expr.value].changes_length = true;
    }

    bool parallel_outer_sequence(const Expression& seq, std::uint32_t local_limit) const noexcept {
        return (seq.kind == Expression::Kind::constant_data ||
                (seq.kind == Expression::Kind::local && seq.value < local_limit)) &&
               parallel_sequence_type(seq.type);
    }

    bool element_equatable(ValueType elem) const noexcept {
        return scalar(elem) || bool(math_type(elem)) || (elem.kind == ValueType::string && supported_value(elem));
    }

    bool element_ordered(ValueType elem) const noexcept {
        return elem == ValueType::i32 || elem == ValueType::u32 || elem == ValueType::f32 || elem == ValueType::f64 ||
               (elem.kind == ValueType::enumeration && elem.type_index < environment_.enumerations.size()) ||
               (elem.kind == ValueType::string && supported_value(elem));
    }

    static std::vector<bool> compute_view_only_string_literals(const Function& function) {
        std::vector<bool> view_only(function.expressions.size(), false);
        for (std::size_t i = 0; i < function.expressions.size(); ++i)
            view_only[i] = function.expressions[i].kind == Expression::Kind::string_literal;
        const auto clear = [&](NodeId id) noexcept {
            if (id != no_node && id < view_only.size())
                view_only[id] = false;
        };
        for (const auto& stmt : function.statements) {
            if (stmt.kind == Statement::Kind::assign)
                clear(stmt.target);
            clear(stmt.expression);
            if (stmt.kind == Statement::Kind::fail)
                clear(stmt.body);
        }
        for (const auto& expr : function.expressions) {
            using Kind = Expression::Kind;
            clear(expr.receiver);
            const bool left_is_view =
                expr.kind == Kind::length || expr.kind == Kind::index ||
                (expr.kind == Kind::binary && expr.left != no_node &&
                 function.expressions[expr.left].type.kind == ValueType::string) ||
                (expr.kind == Kind::sequence_operation &&
                 (is_readonly_sequence_operation(expr.sequence_operation) ||
                  is_try_parse_sequence_operation(expr.sequence_operation)));
            if (!left_is_view)
                clear(expr.left);
            const bool right_is_view = expr.kind == Kind::binary && expr.right != no_node &&
                                       function.expressions[expr.right].type.kind == ValueType::string;
            if (!right_is_view)
                clear(expr.right);
            for (const auto arg : expr.arguments) {
                const bool arg_is_view =
                    expr.kind == Kind::sequence_operation && arg != no_node &&
                    function.expressions[arg].type.kind == ValueType::string &&
                    expr.sequence_operation != SequenceOperation::arr_fill &&
                    expr.sequence_operation != SequenceOperation::arr_resize_fill &&
                    expr.sequence_operation != SequenceOperation::arr_insert;
                if (!arg_is_view)
                    clear(arg);
            }
        }
        return view_only;
    }

    std::expected<void, Diagnostic> analyze_calls() {
        // Summarize direct effects before propagating callee effects bottom-up.
        for (auto& function : module_.functions) {
            // A self node preserves borrows only if every use is a BorrowSafe call or a by-value field
            // read; `(self.v).x = 1` shares its self node with the unused `self.v` read it was projected from.
            std::vector<bool> allowed(function.expressions.size());
            std::vector<bool> blocked(function.expressions.size());
            std::vector<bool> stored(function.expressions.size());
            const auto view_only_literals = compute_view_only_string_literals(function);
            function.preserves_borrows = true;
            for (const auto& statement : function.statements) {
                if (statement.kind == Statement::Kind::assign) {
                    stored[statement.target] = true;
                    const auto target_type = function.expressions[statement.target].type;
                    if (aggregate(target_type) && !plain_record(target_type)) {
                        mark_structure_mutation(function, statement.target);
                        mark_length_change(function, statement.target);
                    }
                }
            }
            for (const auto& node : function.expressions) {
                if (node.kind == Expression::Kind::sequence_mutation) {
                    mark_structure_mutation(function, node.left);
                    mark_length_change(function, node.left);
                }
                if (node.kind == Expression::Kind::sequence_operation) {
                    if (mutates_sequence_structure(node.sequence_operation)) {
                        mark_structure_mutation(function, node.left);
                        mark_length_change(function, node.left);
                    } else if (is_length_preserving_array_mutation(node.sequence_operation)) {
                        const auto seq_type = function.expressions[node.left].type;
                        const auto elem = environment_.values[seq_type.type_index].element;
                        if (aggregate(elem) && !plain_record(elem))
                            mark_structure_mutation(function, node.left);
                    }
                    if (is_try_parse_sequence_operation(node.sequence_operation) && !node.arguments.empty())
                        stored[node.arguments[0]] = true;
                }
                if (node.kind == Expression::Kind::call)
                    for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                        const auto arg_type = function.expressions[node.arguments[i]].type;
                        if (node.argument_passing[i] == ParameterPassing::reference ||
                            (node.argument_passing[i] == ParameterPassing::read_only &&
                             arg_type.kind != ValueType::string))
                            stored[node.arguments[i]] = true;
                        if (node.argument_passing[i] == ParameterPassing::reference && aggregate(arg_type) &&
                            !plain_record(arg_type)) {
                            mark_structure_mutation(function, node.arguments[i]);
                            mark_length_change(function, node.arguments[i]);
                        }
                    }
            }
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
            function.parallel_safe = !function.is_coroutine && !function.requires_receiver &&
                                     (function.result == ValueType::void_value || parallel_value_type(function.result));
            for (std::size_t p = 0; function.parallel_safe && p < function.parameters.size(); ++p) {
                const auto& param = function.parameters[p];
                if (param.passing == ParameterPassing::read_only)
                    function.parallel_safe = parallel_value_type(param.type) || param.type.kind == ValueType::record ||
                                             parallel_sequence_type(param.type);
                else
                    function.parallel_safe = parallel_value_type(param.type);
            }
            for (std::size_t loc = function.parameters.size(); function.parallel_safe && loc < function.locals.size();
                 ++loc)
                function.parallel_safe = parallel_value_type(function.locals[loc].type);
            for (std::size_t s = 0; function.parallel_safe && s < function.statements.size(); ++s) {
                const auto& stmt = function.statements[s];
                if (stmt.kind == Statement::Kind::parallel_loop || stmt.kind == Statement::Kind::yield)
                    function.parallel_safe = false;
                else if (stmt.kind == Statement::Kind::assign && aggregate(function.expressions[stmt.target].type) &&
                         !math_type(function.expressions[stmt.target].type) &&
                         !parallel_value_type(function.expressions[stmt.target].type))
                    function.parallel_safe = false;
            }
            const auto local_limit = static_cast<std::uint32_t>(function.locals.size());
            const auto valid_parallel_seq_view = [&](NodeId seq_id) noexcept {
                const auto& seq = function.expressions[seq_id];
                return parallel_outer_sequence(seq, local_limit) ||
                       (seq.kind == Expression::Kind::string_literal && view_only_literals[seq_id]);
            };
            for (std::size_t e = 0; function.parallel_safe && e < function.expressions.size(); ++e) {
                const auto& expr = function.expressions[e];
                using Kind = Expression::Kind;
                if (expr.kind == Kind::call || expr.kind == Kind::receiver || expr.kind == Kind::field ||
                    expr.kind == Kind::null_reference || expr.kind == Kind::create_reference ||
                    expr.kind == Kind::observe_reference || expr.kind == Kind::acquire_reference ||
                    expr.kind == Kind::sequence_mutation || expr.kind == Kind::texture_sample ||
                    expr.kind == Kind::texture_orientation)
                    function.parallel_safe = false;
                else if (expr.kind == Kind::string_literal && !view_only_literals[e])
                    function.parallel_safe = false;
                else if (expr.kind == Kind::value_field &&
                         (expr.type.kind == ValueType::array || expr.type.kind == ValueType::string))
                    function.parallel_safe = false;
                else if ((expr.kind == Kind::index || expr.kind == Kind::length) &&
                         !valid_parallel_seq_view(expr.left))
                    function.parallel_safe = false;
                else if (expr.kind == Kind::binary &&
                         (function.expressions[expr.left].type.kind == ValueType::array ||
                          function.expressions[expr.left].type.kind == ValueType::string) &&
                         (!valid_parallel_seq_view(expr.left) || !valid_parallel_seq_view(expr.right)))
                    function.parallel_safe = false;
                else if (expr.kind == Kind::sequence_operation) {
                    if (!is_readonly_sequence_operation(expr.sequence_operation) &&
                        !is_try_parse_sequence_operation(expr.sequence_operation)) {
                        function.parallel_safe = false;
                    } else if (!valid_parallel_seq_view(expr.left)) {
                        function.parallel_safe = false;
                    } else {
                        for (const auto arg_id : expr.arguments) {
                            const auto& arg = function.expressions[arg_id];
                            if ((arg.type.kind == ValueType::array || arg.type.kind == ValueType::string) &&
                                !valid_parallel_seq_view(arg_id)) {
                                function.parallel_safe = false;
                                break;
                            }
                        }
                    }
                } else if (expr.kind == Kind::source_call) {
                    for (const auto arg_id : expr.arguments) {
                        const auto& arg = function.expressions[arg_id];
                        if ((arg.type.kind == ValueType::array || arg.type.kind == ValueType::string) &&
                            !parallel_outer_sequence(arg, static_cast<std::uint32_t>(function.parameters.size()))) {
                            function.parallel_safe = false;
                            break;
                        }
                    }
                }
            }
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
                        return reject(call.position, "Recursive source calls are not supported in this module stage.");
                    if (states[call.value] == State::fresh) {
                        states[call.value] = State::active;
                        stack.push_back({call.value});
                        continue;
                    }
                    const auto& callee = module_.functions[call.value];
                    function.uses_scoped_values |= callee.uses_scoped_values;
                    function.requires_receiver |= callee.requires_receiver;
                    function.preserves_borrows &= callee.preserves_borrows;
                    function.parallel_safe &= callee.parallel_safe;
                    for (std::size_t a = 0; a < call.arguments.size(); ++a)
                        if (call.argument_passing[a] == ParameterPassing::reference) {
                            if (callee.parameters[a].mutates_structure)
                                mark_structure_mutation(function, call.arguments[a]);
                            if (callee.parameters[a].changes_length)
                                mark_length_change(function, call.arguments[a]);
                        }
                    ++cursor.next;
                } else {
                    if (function.uses_scoped_values && (function.is_coroutine || !function.preserves_borrows))
                        return reject(
                            function.position,
                            "Scoped value execution in '" + function.function_name +
                                "' reaches suspension, an object operation other than reading a self field, or a native call without BorrowSafe.");
                    states[cursor.function] = State::complete;
                    stack.pop_back();
                }
            }
        }
        for (const auto& function : module_.functions) {
            for (const auto& call : function.expressions) {
                if (call.kind != Expression::Kind::source_call && call.kind != Expression::Kind::call)
                    continue;
                for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                    if (call.argument_passing[i] != ParameterPassing::reference)
                        continue;
                    const auto& arg_expr = function.expressions[call.arguments[i]];
                    const auto arg_type = arg_expr.type;
                    const bool changes_len = call.kind == Expression::Kind::source_call
                                                 ? module_.functions[call.value].parameters[i].changes_length
                                                 : (aggregate(arg_type) && !plain_record(arg_type));
                    if (changes_len && arg_expr.kind == Expression::Kind::local &&
                        function.locals[arg_expr.value].fixed_length)
                        return reject(
                            arg_expr.position,
                            "SOP [InOut] and [Out] attribute columns have fixed element count and cannot change structure.");
                    const bool mutates = call.kind == Expression::Kind::source_call
                                             ? module_.functions[call.value].parameters[i].mutates_structure
                                             : (aggregate(arg_type) && !plain_record(arg_type));
                    if (!mutates)
                        continue;
                    const auto root = root_local(function, call.arguments[i]);
                    if (!root)
                        continue;
                    for (std::size_t j = 0; j < call.arguments.size(); ++j) {
                        if (j == i || call.argument_passing[j] == ParameterPassing::value)
                            continue;
                        if (root_local(function, call.arguments[j]) == root)
                            return reject(
                                function.expressions[call.arguments[j]].position,
                                "Cannot borrow '" + function.locals[*root].name +
                                    "' through another reference parameter in the same call when its sequence structure may change.");
                    }
                }
            }
        }
        ATELIER_TRY(validate_parallel_loops());
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
        mark_reachable_parallel_callees(module_);
        return {};
    }

    std::expected<void, Diagnostic> validate_parallel_loops() {
        for (auto& function : module_.functions) {
            const bool has_parallel_loop = std::ranges::any_of(
                function.statements, [](const auto& s) { return s.kind == Statement::Kind::parallel_loop; });
            if (!has_parallel_loop)
                continue;
            const auto view_only_literals = compute_view_only_string_literals(function);
            const auto unwrap_value_fields = [&](NodeId id) noexcept {
                while (function.expressions[id].kind == Expression::Kind::value_field)
                    id = function.expressions[id].left;
                return id;
            };
            for (const auto& root : function.statements) {
                if (root.kind != Statement::Kind::parallel_loop)
                    continue;
                const std::uint32_t ivar = static_cast<std::uint32_t>(root.target);
                std::vector<std::uint32_t> written_arrays;
                std::vector<std::uint32_t> arbitrary_sequences;
                const auto mark_unique = [](std::vector<std::uint32_t>& list, std::uint32_t value) {
                    if (std::ranges::find(list, value) == list.end())
                        list.push_back(value);
                };
                const auto check_mutable_lvalue = [&](NodeId target_id,
                                                      SourcePosition pos) -> std::expected<void, Diagnostic> {
                    const auto& target_expr = function.expressions[target_id];
                    if (aggregate(target_expr.type) && !math_type(target_expr.type) &&
                        !parallel_value_type(target_expr.type))
                        return reject(pos, "parallel_for cannot assign non-plain aggregate values.");
                    const NodeId base_id = unwrap_value_fields(target_id);
                    const auto& base = function.expressions[base_id];
                    if (base.kind == Expression::Kind::local && base.value > ivar &&
                        !function.locals[base.value].read_only)
                        return {};
                    if (base.kind == Expression::Kind::index) {
                        const auto& seq = function.expressions[base.left];
                        if (seq.kind == Expression::Kind::local && seq.value < ivar &&
                            seq.type.kind == ValueType::array && parallel_sequence_type(seq.type) &&
                            !function.locals[seq.value].read_only) {
                            if (base.right == no_node ||
                                function.expressions[base.right].kind != Expression::Kind::local ||
                                function.expressions[base.right].value != ivar)
                                return reject(base.right == no_node ? base.position
                                                                    : function.expressions[base.right].position,
                                              "parallel_for element writes must index directly by the loop variable.");
                            mark_unique(written_arrays, seq.value);
                            return {};
                        }
                    }
                    return reject(
                        pos,
                        "parallel_for can only mutate locals declared inside the loop or outer Array[i] elements.");
                };
                auto check_expr = [&](auto&& self, NodeId id) -> std::expected<void, Diagnostic> {
                    if (id == no_node)
                        return {};
                    const auto& expr = function.expressions[id];
                    using Kind = Expression::Kind;
                    const auto check_seq_view = [&](NodeId seq_id, SourcePosition pos,
                                                    bool reads_elements) -> std::expected<void, Diagnostic> {
                        const auto& seq = function.expressions[seq_id];
                        if (seq.kind == Kind::string_literal && view_only_literals[seq_id])
                            return {};
                        if (!parallel_outer_sequence(seq, ivar))
                            return reject(pos, "parallel_for sequence access requires a top-level outer Array or String.");
                        if (reads_elements && seq.kind == Kind::local)
                            mark_unique(arbitrary_sequences, seq.value);
                        return self(self, seq_id);
                    };
                    switch (expr.kind) {
                    case Kind::literal:
                    case Kind::constant_data:
                        return {};
                    case Kind::string_literal:
                        if (view_only_literals[id])
                            return {};
                        return reject(
                            expr.position,
                            "parallel_for does not allow native calls, object operations, String literals or sequence structural mutations.");
                    case Kind::local: {
                        const auto type = function.locals[expr.value].type;
                        if (expr.value < ivar && !parallel_value_type(type) && type.kind != ValueType::record &&
                            !parallel_sequence_type(type))
                            return reject(expr.position,
                                          "parallel_for cannot capture object references or nested/managed sequences.");
                        return {};
                    }
                    case Kind::field:
                        if (function.expressions[expr.receiver].kind != Kind::receiver)
                            return reject(expr.position, "parallel_for can only read fields on self.");
                        if (expr.type.kind == ValueType::string)
                            return reject(
                                expr.position,
                                "parallel_for cannot access String fields on self; copy to a local String before the loop.");
                        return {};
                    case Kind::value_field:
                        if (expr.type.kind == ValueType::array || expr.type.kind == ValueType::string)
                            return reject(expr.position,
                                          "parallel_for cannot access Array or String fields inside a record.");
                        return self(self, expr.left);
                    case Kind::binary:
                        if (function.expressions[expr.left].type.kind == ValueType::array ||
                            function.expressions[expr.left].type.kind == ValueType::string) {
                            ATELIER_TRY(check_seq_view(expr.left, expr.position, true));
                            return check_seq_view(expr.right, expr.position, true);
                        }
                        ATELIER_TRY(self(self, expr.left));
                        return self(self, expr.right);
                    case Kind::unary:
                    case Kind::scalar_cast:
                    case Kind::conditional:
                        ATELIER_TRY(self(self, expr.receiver));
                        ATELIER_TRY(self(self, expr.left));
                        return self(self, expr.right);
                    case Kind::length: {
                        const auto& seq = function.expressions[expr.left];
                        if (seq.kind == Kind::string_literal && view_only_literals[expr.left])
                            return {};
                        if (!parallel_outer_sequence(seq, ivar))
                            return reject(expr.position,
                                          "parallel_for sequence size requires a top-level outer Array or String.");
                        return self(self, expr.left);
                    }
                    case Kind::index: {
                        const auto& seq = function.expressions[expr.left];
                        if (!(seq.kind == Kind::string_literal && view_only_literals[expr.left]) &&
                            !parallel_outer_sequence(seq, ivar))
                            return reject(expr.position,
                                          "parallel_for indexing requires a top-level outer Array or String.");
                        if (seq.kind == Kind::local &&
                            (expr.right == no_node || function.expressions[expr.right].kind != Kind::local ||
                             function.expressions[expr.right].value != ivar))
                            mark_unique(arbitrary_sequences, seq.value);
                        ATELIER_TRY(self(self, expr.left));
                        return self(self, expr.right);
                    }
                    case Kind::sequence_operation:
                        if (is_readonly_sequence_operation(expr.sequence_operation)) {
                            const bool reads_elems = expr.sequence_operation != SequenceOperation::str_empty &&
                                                     expr.sequence_operation != SequenceOperation::arr_empty;
                            ATELIER_TRY(check_seq_view(expr.left, expr.position, reads_elems));
                            for (const auto arg_id : expr.arguments) {
                                const auto& arg = function.expressions[arg_id];
                                if (arg.type.kind == ValueType::array || arg.type.kind == ValueType::string) {
                                    ATELIER_TRY(check_seq_view(arg_id, arg.position, true));
                                } else {
                                    ATELIER_TRY(self(self, arg_id));
                                }
                            }
                            return {};
                        }
                        if (is_try_parse_sequence_operation(expr.sequence_operation)) {
                            ATELIER_TRY(check_seq_view(expr.left, expr.position, true));
                            const NodeId out_id = expr.arguments[0];
                            ATELIER_TRY(check_mutable_lvalue(out_id, function.expressions[out_id].position));
                            return self(self, out_id);
                        }
                        return reject(
                            expr.position,
                            "parallel_for does not allow native calls, object operations, String literals or sequence structural mutations.");
                    case Kind::math_construct:
                    case Kind::math_intrinsic:
                        for (const auto arg : expr.arguments) {
                            ATELIER_TRY(self(self, arg));
                        }
                        return {};
                    case Kind::source_call: {
                        const auto& callee = module_.functions[expr.value];
                        if (!callee.parallel_safe)
                            return reject(expr.position,
                                          "Function '" + callee.function_name +
                                              "' is not parallel-safe and cannot be called in parallel_for.");
                        for (std::size_t a = 0; a < expr.arguments.size(); ++a) {
                            const NodeId arg_id = expr.arguments[a];
                            const auto& arg = function.expressions[arg_id];
                            if (expr.argument_passing[a] == ParameterPassing::reference) {
                                ATELIER_TRY(check_mutable_lvalue(arg_id, arg.position));
                            } else if (expr.argument_passing[a] == ParameterPassing::read_only &&
                                       (arg.type.kind == ValueType::array || arg.type.kind == ValueType::string)) {
                                if (!parallel_outer_sequence(arg, ivar))
                                    return reject(
                                        arg.position,
                                        "parallel_for helper sequence arguments require a top-level outer Array or String.");
                                if (arg.kind == Kind::local)
                                    mark_unique(arbitrary_sequences, arg.value);
                            }
                            ATELIER_TRY(self(self, arg_id));
                        }
                        return {};
                    }
                    default:
                        return reject(
                            expr.position,
                            "parallel_for does not allow native calls, object operations, String literals or sequence structural mutations.");
                    }
                };
                auto check_stmt = [&](auto&& self, NodeId id) -> std::expected<void, Diagnostic> {
                    if (id == no_node)
                        return {};
                    const auto& stmt = function.statements[id];
                    using Kind = Statement::Kind;
                    switch (stmt.kind) {
                    case Kind::block:
                        for (const auto child : stmt.children) {
                            ATELIER_TRY(self(self, child));
                        }
                        return {};
                    case Kind::declare:
                        if (!parallel_value_type(function.locals[stmt.target].type))
                            return reject(
                                stmt.position,
                                "parallel_for locals require scalar, enum, built-in math or zero-default plain struct types.");
                        return check_expr(check_expr, stmt.expression);
                    case Kind::assign:
                        ATELIER_TRY(check_mutable_lvalue(stmt.target, stmt.position));
                        ATELIER_TRY(check_expr(check_expr, stmt.target));
                        return check_expr(check_expr, stmt.expression);
                    case Kind::call:
                        return check_expr(check_expr, stmt.expression);
                    case Kind::condition:
                        ATELIER_TRY(check_expr(check_expr, stmt.expression));
                        ATELIER_TRY(self(self, stmt.body));
                        return self(self, stmt.alternative);
                    case Kind::loop:
                        ATELIER_TRY(check_expr(check_expr, stmt.expression));
                        ATELIER_TRY(self(self, stmt.body));
                        return self(self, stmt.step);
                    case Kind::fail:
                        ATELIER_TRY(check_expr(check_expr, stmt.body));
                        return check_expr(check_expr, stmt.expression);
                    case Kind::break_loop:
                    case Kind::continue_loop:
                        return {};
                    default:
                        return reject(stmt.position, "Unsupported control flow inside parallel_for.");
                    }
                };
                ATELIER_TRY(check_stmt(check_stmt, root.body));
                for (const auto written : written_arrays) {
                    if (std::ranges::find(arbitrary_sequences, written) != arbitrary_sequences.end())
                        return reject(
                            root.position,
                            "Array '" + function.locals[written].name +
                                "' is written inside parallel_for and cannot also be indexed by an expression other than the loop variable.");
                }
            }
        }
        return {};
    }

    std::expected<std::vector<NodeId>, Diagnostic> call_arguments(std::span<const ParameterType> parameters) {
        std::vector<NodeId> arguments;
        ATELIER_TRY_BIND(empty, accept(")"));
        if (!*empty) {
            for (;;) {
                if (arguments.size() >= parameters.size())
                    return reject(token_.position, "Call argument count mismatch.");
                ATELIER_TRY_BIND(argument, expression());
                arguments.push_back(*argument);
                ATELIER_TRY_BIND(more, accept(","));
                if (!*more)
                    break;
            }
            ATELIER_TRY(expect(")"));
        }
        if (arguments.size() < parameters.size()) {
            if (!parameters[arguments.size()].has_default)
                return reject(token_.position, "Call argument count mismatch.");
            for (std::size_t i = arguments.size(); i < parameters.size(); ++i) {
                ATELIER_TRY_BIND(value, literal(parameters[i].type, token_.position, parameters[i].default_bits));
                arguments.push_back(*value);
            }
        }
        if (arguments.size() != parameters.size())
            return reject(token_.position, "Call argument count mismatch.");
        return arguments;
    }

    std::expected<Expression, Diagnostic> call_expression(bool native, std::uint32_t index, SourcePosition position,
                                                          NodeId receiver, std::vector<NodeId> arguments) {
        const auto parameters = native ? environment_.methods[index].parameters
                                       : std::span<const ParameterType>{module_.functions[index].parameters};
        const auto result = native ? environment_.methods[index].result : module_.functions[index].result;
        if (!native && module_.functions[index].is_coroutine)
            return reject(position,
                          "Source calls require a synchronous function; async entries are started by the host.");
        if (plain_record(result) && !math_type(result)) {
            if (!environment_.values[result.type_index].can_own)
                return reject(position,
                              "Plain record results require borrow-preserving copy and lifecycle operations.");
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
            ATELIER_TRY_BIND(argument, converted(arguments[i], parameter.type));
            const auto& node = function_.expressions[*argument];
            if (parameter.passing == ParameterPassing::value && plain_record(parameter.type) &&
                !math_type(parameter.type) && !environment_.values[parameter.type.type_index].can_own)
                return reject(node.position,
                              "Plain record value arguments require borrow-preserving copy and lifecycle operations.");
            if ((aggregate(parameter.type) && !math_type(parameter.type)) ||
                (parameter.passing != ParameterPassing::value && borrows_constant_data(*argument)))
                function_.uses_scoped_values = true;
            if (parameter.passing == ParameterPassing::reference && !writable_storage(*argument))
                return reject(node.position, "Writable reference requires a mutable lvalue.");
            if (parameter.passing != ParameterPassing::value && node.kind == Expression::Kind::field) {
                const bool readonly_self_string =
                    node.type.kind == ValueType::string && parameter.passing == ParameterPassing::read_only &&
                    function_.expressions[node.receiver].kind == Expression::Kind::receiver;
                if (!native && !readonly_self_string)
                    return reject(node.position,
                                  "Runtime object fields may only be borrowed by protected native calls.");
                if (parameter.passing == ParameterPassing::reference && environment_.fields[node.value].constrained)
                    return reject(node.position,
                                  "Hard-bounded fields cannot be borrowed writably; assign the field instead.");
            }
            call.arguments.push_back(*argument);
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

    std::expected<void, Diagnostic> collect_properties() {
        const auto collect = [&](std::string_view name, std::span<const ParameterType> parameters, ValueType result,
                                 bool native, std::uint32_t index,
                                 SourcePosition position) -> std::expected<void, Diagnostic> {
            auto alias = property_name(name, parameters.size(), result == ValueType::void_value,
                                       parameters.size() == 1 && parameters[0].passing == ParameterPassing::reference);
            if (alias.empty())
                return {};
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
                return reject(position, "Property getter and setter types differ: " + property->name);
            (getter ? property->getter : property->setter) = index;
            return {};
        };
        for (std::uint32_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            ATELIER_TRY(collect(method.name, method.parameters, method.result, true, i, {}));
        }
        for (std::uint32_t i = 0; i < module_.functions.size(); ++i) {
            const auto& function = module_.functions[i];
            if (!function.is_coroutine) {
                ATELIER_TRY(
                    collect(function.function_name, function.parameters, function.result, false, i, function.position));
            }
        }
        for (const auto& property : properties_) {
            const auto conflict = [&] {
                return reject({}, "Property conflicts with a visible declaration: " + property.name);
            };
            const auto hidden = [&](std::string_view name) {
                return name == property.name ||
                       (name.starts_with(property.name) && name.size() > property.name.size() &&
                        name[property.name.size()] == '.');
            };
            for (const auto& other : properties_)
                if (&other != &property && other.native == property.native && hidden(other.name))
                    return conflict();
            for (const auto& field : environment_.fields)
                if (hidden(field.name))
                    return conflict();
            if (property.native) {
                for (const auto& method : environment_.methods)
                    if (hidden(method.name))
                        return conflict();
                continue;
            }
            if (math_reserved_name(property.name) || hidden(environment_.object_type_name))
                return conflict();
            for (const auto& type : environment_.values)
                if (hidden(type.name))
                    return conflict();
            for (const auto& type : environment_.enumerations)
                if (hidden(type.name))
                    return conflict();
            for (const auto& constant : constants_)
                if (hidden(constant.name))
                    return conflict();
            for (const auto& function : module_.functions) {
                if (hidden(function.function_name))
                    return conflict();
                for (const auto& parameter : function.locals)
                    if (parameter.name == property.name)
                        return conflict();
            }
        }
        return {};
    }

    std::expected<NodeId, Diagnostic> property_access(const Property& property, SourcePosition position,
                                                      NodeId receiver = no_node) {
        // Keep only a call-shaped placeholder until assignment vs. read is known.
        // No speculative getter call may enter effect analysis or the call graph.
        ATELIER_TRY_BIND(node, add(Expression{property.native ? Expression::Kind::call : Expression::Kind::source_call,
                                              property.type, position, no_node, no_node, 0, Operation::add, receiver}));
        property_uses_.push_back({*node, static_cast<std::size_t>(&property - properties_.data())});
        return node;
    }

    std::expected<void, Diagnostic> finish_properties() {
        for (const auto& use : property_uses_) {
            const auto& property = properties_[use.property];
            const auto node = function_.expressions[use.node];
            if (property.getter == UINT32_MAX)
                return reject(node.position, "Property is write-only: " + property.name);
            ATELIER_TRY_BIND(call, call_expression(property.native, property.getter, node.position, node.receiver, {}));
            function_.expressions[use.node] = std::move(*call);
        }
        property_uses_.clear();
        return {};
    }

    std::expected<std::string, Diagnostic> expression_name(bool native) {
        ATELIER_TRY_BIND(first, identifier());
        std::string name(first->text);
        const auto string_field = [&](std::string_view candidate_name) {
            return native && std::ranges::any_of(environment_.fields, [&](const auto& field) {
                       return field.name == candidate_name && field.type.kind == ValueType::string;
                   });
        };
        const auto aggregate_constant = [&](std::string_view candidate_name) {
            if (native)
                return false;
            const auto found = constant_indices_.find(candidate_name);
            return found != constant_indices_.end() && aggregate(constants_[found->second].type);
        };
        while (token_.text == "." && !find_property(name, native) && !string_field(name) && !aggregate_constant(name)) {
            ATELIER_TRY(consume());
            name += '.';
            ATELIER_TRY_BIND(part, identifier());
            name += part->text;
        }
        return name;
    }

    struct Depth {
        std::size_t& value;

        explicit Depth(std::size_t& depth) : value(depth) { ++value; }

        ~Depth() { --value; }
    };

    std::expected<void, Diagnostic> validate_name(std::string_view name, bool optional) const {
        if (name.empty() && !optional)
            return reject({}, "Host member name cannot be empty.");
        for (std::size_t start = 0; start < name.size();) {
            auto end = name.find('.', start);
            if (end == std::string_view::npos)
                end = name.size();
            const auto segment = name.substr(start, end - start);
            if (segment.empty() || !letter(segment.front()) || reserved(segment))
                return reject({}, "Host binding name requires non-reserved identifier segments.");
            for (const auto character : segment)
                if (!letter(character) && !digit(character))
                    return reject({}, "Host binding name requires non-reserved identifier segments.");
            if (end == name.size())
                break;
            start = end + 1;
            if (start == name.size())
                return reject({}, "Host binding name cannot end with '.'.");
        }
        return {};
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

    bool flags_enum(ValueType type) const noexcept {
        return type.kind == ValueType::enumeration && type.type_index < environment_.enumerations.size() &&
               environment_.enumerations[type.type_index].flags;
    }

    std::expected<void, Diagnostic> validate_environment() const {
        ATELIER_TRY(validate_name(environment_.object_type_name, true));
        for (std::size_t i = 0; i < environment_.enumerations.size(); ++i) {
            const auto& enumeration = environment_.enumerations[i];
            ATELIER_TRY(validate_name(enumeration.name, false));
            if (enumeration.underlying != ValueType::i32 && enumeration.underlying != ValueType::u32)
                return reject({}, "Enum requires i32/u32 representation.");
            if (enumeration.flags && enumeration.underlying != ValueType::u32)
                return reject({}, "Flags enum requires u32 representation.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.enumerations[j].name == enumeration.name)
                    return reject({}, "Duplicate enum type name.");
            for (std::size_t j = 0; j < enumeration.values.size(); ++j) {
                ATELIER_TRY(validate_name(enumeration.values[j].name, false));
                if (enumeration.values[j].name.find('.') != std::string_view::npos)
                    return reject({}, "Enum item must be a simple identifier.");
                for (std::size_t k = 0; k < j; ++k)
                    if (enumeration.values[k].name == enumeration.values[j].name)
                        return reject({}, "Duplicate enum item name.");
            }
        }
        if (environment_.values.size() > std::numeric_limits<std::uint32_t>::max())
            return reject({}, "Value catalog is too large.");
        for (std::size_t i = 0; i < environment_.values.size(); ++i) {
            const auto& value = environment_.values[i];
            ATELIER_TRY(validate_name(value.name, false));
            if (!aggregate({value.kind, static_cast<std::uint32_t>(i)}) || !value.size ||
                !std::has_single_bit(value.alignment) || value.size % value.alignment ||
                value.size > static_cast<std::size_t>(PTRDIFF_MAX))
                return reject({}, "Invalid native value layout.");
            if ((value.kind == ValueType::array || value.kind == ValueType::string) && !value.sequence_view)
                return reject({}, "Sequence layout requires its native view operation.");
            if (value.kind == ValueType::array && !supported_value(value.element))
                return reject({}, "Invalid array element type.");
            if (value.standard_array_layout && (value.kind != ValueType::array || value.size != 3 * sizeof(void*) ||
                                                value.alignment != alignof(void*) || !value.can_mutate))
                return reject({}, "Invalid standard array layout.");
            if (value.math) {
                if (value.kind != ValueType::record || value.name != math::name(value.math) ||
                    value.math.components() == 0 || value.size != value.math.size() ||
                    value.alignment != value.math.scalar_size() || value.fields.size() != value.math.components() ||
                    (value.math.precision != math::Precision::f32 && value.math.precision != math::Precision::f64))
                    return reject({}, "Invalid canonical math value layout.");
                for (std::size_t component = 0; component < value.fields.size(); ++component)
                    if (value.fields[component].type != math_scalar(value.math) ||
                        value.fields[component].offset != component * value.math.scalar_size())
                        return reject({}, "Math components require contiguous scalar layout.");
                const auto alias = math::short_name(value.math);
                if (environment_.object_type_name == alias)
                    return reject({}, "Object type conflicts with a built-in math type alias.");
                for (const auto& other : environment_.values)
                    if (&other != &value && (other.math == value.math || other.name == alias))
                        return reject({}, "Ambiguous built-in math type or alias.");
                for (const auto& enumeration : environment_.enumerations)
                    if (enumeration.name == alias || enumeration.name == value.name)
                        return reject({}, "Enum type conflicts with a built-in math type or alias.");
            }
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.values[j].name == value.name)
                    return reject({}, "Duplicate native value name.");
            for (std::size_t j = 0; j < value.fields.size(); ++j) {
                const auto& field = value.fields[j];
                ATELIER_TRY(validate_name(field.name, false));
                if (value.kind != ValueType::record || field.name.find('.') != std::string::npos ||
                    !supported_value(field.type) || field.offset > value.size ||
                    value_size(field.type) > value.size - field.offset || field.offset % value_alignment(field.type) ||
                    value.alignment < value_alignment(field.type))
                    return reject({}, "Invalid direct native field layout.");
                for (std::size_t k = 0; k < j; ++k)
                    if (value.fields[k].name == field.name)
                        return reject({}, "Duplicate native value field.");
            }
        }
        if (environment_.fields.size() > std::numeric_limits<std::uint32_t>::max() ||
            environment_.methods.size() > std::numeric_limits<std::uint32_t>::max())
            return reject({}, "Host member catalog exceeds its index range.");
        for (std::size_t i = 0; i < environment_.fields.size(); ++i) {
            const auto& field = environment_.fields[i];
            ATELIER_TRY(validate_name(field.name, false));
            const bool valid_string_field =
                field.type.kind == ValueType::string && supported_value(field.type) && !field.constrained;
            if (!scalar(field.type) && !math_type(field.type) && !valid_string_field)
                return reject({}, "Host fields require scalar, built-in math or String value types.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.fields[j].name == field.name)
                    return reject({}, "Ambiguous host field binding.");
        }
        for (std::size_t i = 0; i < environment_.methods.size(); ++i) {
            const auto& method = environment_.methods[i];
            ATELIER_TRY(validate_name(method.name, false));
            if (method.parameters.size() > std::numeric_limits<std::uint32_t>::max())
                return reject({}, "Host parameter list exceeds its index range.");
            bool seen_default = false;
            for (const auto& parameter : method.parameters) {
                if (!supported_value(parameter.type) ||
                    (aggregate(parameter.type) && parameter.passing == ParameterPassing::value &&
                     !plain_record(parameter.type)) ||
                    (parameter.passing != ParameterPassing::value && parameter.passing != ParameterPassing::read_only &&
                     parameter.passing != ParameterPassing::reference))
                    return reject({}, "Host method has an unsupported parameter type or passing mode.");
                if (!parameter.has_default) {
                    if (seen_default || parameter.default_bits != 0)
                        return reject({}, "Host method parameter defaults must cover a trailing suffix.");
                } else {
                    seen_default = true;
                    if (!scalar(parameter.type) || parameter.passing == ParameterPassing::reference ||
                        (parameter.type == ValueType::boolean && parameter.default_bits > 1) ||
                        (parameter.type != ValueType::f64 &&
                         parameter.default_bits > std::numeric_limits<std::uint32_t>::max()))
                        return reject({}, "Host method has an invalid parameter default.");
                }
            }
            if (method.result != ValueType::void_value && !scalar(method.result) && !plain_record(method.result))
                return reject({}, "Host method has an unsupported result type.");
            for (std::size_t j = 0; j < i; ++j)
                if (environment_.methods[j].name == method.name)
                    return reject({}, "Ambiguous host method binding.");
        }
        return {};
    }

    std::expected<void, Diagnostic> object_type() {
        const auto position = token_.position;
        ATELIER_TRY_BIND(name, qualified_identifier());
        if (environment_.object_type_name.empty())
            return reject(position, "The host environment does not expose a named object type.");
        if (*name != environment_.object_type_name)
            return reject(position, "Unknown object type '" + *name + "'.");
        return {};
    }

    std::expected<bool, Diagnostic> reference_declaration() const {
        if (token_.kind != Token::Kind::identifier || reserved(token_.text))
            return false;
        auto lookahead = lexer_;
        ATELIER_TRY_BIND(next, lookahead.next());
        if (token_.text == "Array" && next->text == "<")
            return true;
        while (next->text == ".") {
            ATELIER_TRY_BIND(segment, lookahead.next());
            if (segment->kind != Token::Kind::identifier || reserved(segment->text))
                return false;
            next = lookahead.next();
            if (!next)
                return std::unexpected(std::move(next.error()));
        }
        return next->kind == Token::Kind::identifier && !reserved(next->text);
    }

    std::expected<NodeId, Diagnostic> add(Expression expression) {
        std::size_t height = 1;
        for (const auto child : {expression.left, expression.right, expression.receiver})
            if (child != no_node && expression_heights_[child] + 1 > height)
                height = expression_heights_[child] + 1;
        for (const auto argument : expression.arguments)
            if (expression_heights_[argument] + 1 > height)
                height = expression_heights_[argument] + 1;
        if (height > 128)
            return reject(expression.position, "Expression tree exceeds the compiler limit of 128.");
        function_.expressions.push_back(std::move(expression));
        expression_heights_.push_back(height);
        return function_.expressions.size() - 1;
    }

    std::expected<NodeId, Diagnostic> literal(ValueType type, SourcePosition position, std::uint64_t bits,
                                              bool typed_constant = false) {
        Expression expression{Expression::Kind::literal, type, position};
        expression.bits = bits;
        expression.typed_constant = typed_constant;
        return add(std::move(expression));
    }

    NodeId add(Statement statement) {
        function_.statements.push_back(std::move(statement));
        return function_.statements.size() - 1;
    }

    std::expected<void, Diagnostic> unused_name(const Token& name) const {
        if (math_reserved_name(name.text))
            return reject(name.position, "Locals cannot replace math constructors or the math namespace.");
        if (find_property(name.text, false))
            return reject(name.position, "Locals cannot shadow a property.");
        if (constant_indices_.contains(name.text))
            return reject(name.position,
                          "A module constant with this name is already visible; shadowing is not supported.");
        for (const auto index : visible_)
            if (function_.locals[index].name == name.text)
                return reject(name.position,
                              "A local or parameter with this name is already visible; shadowing is not supported.");
        return {};
    }

    std::expected<std::uint32_t, Diagnostic> local(const Token& name, ValueType type) {
        ATELIER_TRY(unused_name(name));
        if (function_.locals.size() >= std::numeric_limits<std::uint32_t>::max())
            return reject(name.position, "Too many source locals.");
        const auto index = static_cast<std::uint32_t>(function_.locals.size());
        function_.locals.push_back({std::string(name.text), type});
        visible_.push_back(index);
        return index;
    }

    std::expected<ValueType, Diagnostic> result_type() {
        const auto position = token_.position;
        ATELIER_TRY_BIND(type, parameter_type());
        if (scalar(*type) || math_type(*type))
            return type;
        if (plain_record(*type)) {
            if (!environment_.values[type->type_index].can_own)
                return reject(position,
                              "Plain record results require borrow-preserving copy and lifecycle operations.");
            return type;
        }
        return reject(position, "Function results require a scalar, enum or plain record value type.");
    }

    std::expected<ValueType, Diagnostic> parameter_type() {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        const auto position = token_.position;
        ATELIER_TRY_BIND(name, parse_type_name(*this));
        return declared_type(*name, position);
    }

    bool returns(NodeId id) const {
        const auto& statement = function_.statements[id];
        switch (statement.kind) {
        case Statement::Kind::return_value:
        case Statement::Kind::fail:
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
            return mutable_local_storage(id) && environment_.values[expression.type.type_index].can_own;
        return writable_storage(id);
    }

    bool writable_storage(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::field)
            return node.type.kind != ValueType::string;
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

    bool mutable_local_storage(NodeId id) const {
        return writable_storage(id) && root_local(function_, id).has_value();
    }

    bool integer_constant(NodeId id) const {
        const auto& node = function_.expressions[id];
        if (node.type != ValueType::u32 || node.typed_constant)
            return false;
        if (node.kind == Expression::Kind::literal)
            return node.bits <= 2147483647u;
        return (node.kind == Expression::Kind::unary && integer_constant(node.left)) ||
               (node.kind == Expression::Kind::binary && integer_constant(node.left) && integer_constant(node.right)) ||
               (node.kind == Expression::Kind::conditional && integer_constant(node.left) &&
                integer_constant(node.right));
    }

    void signed_constant(NodeId id) {
        auto& node = function_.expressions[id];
        node.type = ValueType::i32;
        if (node.left != no_node)
            signed_constant(node.left);
        if (node.right != no_node)
            signed_constant(node.right);
    }

    std::expected<NodeId, Diagnostic> converted(NodeId expression, ValueType required) {
        if (required == ValueType::i32 && integer_constant(expression))
            signed_constant(expression);
        const auto actual = function_.expressions[expression].type;
        if (actual == required || (reference_type(required) && actual == ValueType::null_reference))
            return expression;
        if (required == ValueType::weak_reference && actual == ValueType::strong_reference)
            return add(Expression{Expression::Kind::observe_reference, required,
                                  function_.expressions[expression].position, expression});
        if (reference_type(required) || reference_type(actual))
            return reject(
                function_.expressions[expression].position,
                "Type mismatch; only strong-to-weak reference conversion is implicit; use try_acquire to strengthen.");
        return reject(function_.expressions[expression].position,
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

    static std::expected<std::uint64_t, Diagnostic> convert_constant(ValueType from, ValueType to, std::uint64_t bits,
                                                                     SourcePosition position) {
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
            return reject(position, "Constant numeric conversion exceeds its integer destination range.");
        return to == ValueType::i32 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(value))
                                    : static_cast<std::uint32_t>(value);
    }

    template <class T>
    static std::expected<std::uint64_t, Diagnostic> fold_arithmetic(Operation operation, T left, T right,
                                                                    SourcePosition position) {
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
                return reject(position, "Unsupported constant floating operation.");
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
                    return reject(position, "Constant integer division or remainder by zero.");
                if constexpr (std::is_signed_v<T>)
                    if (left == std::numeric_limits<std::int32_t>::min() && right == -1)
                        return reject(position, "Constant signed division or remainder overflows.");
                return static_cast<std::uint32_t>(operation == Operation::divide ? left / right : left % right);
            default:
                return reject(position, "Unsupported constant integer operation.");
            }
        }
    }

    static void write_scalar_bytes(ValueType type, std::uint64_t bits, std::byte* dst) noexcept {
        if (type == ValueType::boolean) {
            *dst = static_cast<std::byte>(bits != 0 ? 1u : 0u);
        } else if (type == ValueType::f64) {
            std::memcpy(dst, &bits, sizeof(double));
        } else {
            const auto value = static_cast<std::uint32_t>(bits);
            std::memcpy(dst, &value, sizeof(value));
        }
    }

    static std::uint64_t read_scalar_bits(ValueType type, const std::byte* src) noexcept {
        if (type == ValueType::boolean)
            return *src != std::byte{0} ? 1u : 0u;
        if (type == ValueType::f64) {
            std::uint64_t bits{};
            std::memcpy(&bits, src, sizeof(double));
            return bits;
        }
        std::uint32_t bits{};
        std::memcpy(&bits, src, sizeof(bits));
        return bits;
    }

    std::expected<std::optional<std::span<const std::byte>>, Diagnostic> try_constant_slice(NodeId id) {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(function_.expressions[id].position, "Source nesting exceeds the compiler limit of 128.");
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::constant_data) {
            const auto& constant = module_.constants[node.value];
            return std::span<const std::byte>{constant.bytes.data(), constant.bytes.size()};
        }
        if (node.kind == Expression::Kind::value_field) {
            ATELIER_TRY_BIND(base, try_constant_slice(node.left));
            if (!*base)
                return std::optional<std::span<const std::byte>>{};
            return (*base)->subspan(node.offset, value_size(node.type));
        }
        if (node.kind == Expression::Kind::index) {
            const auto& seq = function_.expressions[node.left];
            if (seq.kind != Expression::Kind::constant_data)
                return reject(node.position, "Module constant initializers require compile-time constant expressions.");
            const auto& constant = module_.constants[seq.value];
            std::size_t idx = 0;
            if (node.right == no_node) {
                if (constant.element_count == 0)
                    return reject(node.position, "Constant array index is out of bounds.");
                idx = constant.element_count - 1;
            } else {
                ATELIER_TRY_BIND(idx_bits, fold_constant(node.right));
                const auto idx_type = function_.expressions[node.right].type;
                if (idx_type == ValueType::i32 &&
                    std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*idx_bits)) < 0)
                    return reject(node.position, "Constant array index is out of bounds.");
                idx = static_cast<std::size_t>(static_cast<std::uint32_t>(*idx_bits));
                if (idx >= constant.element_count)
                    return reject(node.position, "Constant array index is out of bounds.");
            }
            const std::size_t elem_size = value_size(node.type);
            return std::span<const std::byte>{constant.bytes}.subspan(idx * elem_size, elem_size);
        }
        if (node.kind == Expression::Kind::conditional) {
            ATELIER_TRY_BIND(cond, fold_constant(node.receiver));
            return try_constant_slice(*cond ? node.left : node.right);
        }
        return std::optional<std::span<const std::byte>>{};
    }

    std::expected<void, Diagnostic> fold_projection_or_intrinsic(const Expression& node, std::span<std::byte> out) {
        if (node.kind == Expression::Kind::value_field) {
            ATELIER_TRY_BIND(slice, try_constant_slice(node.left));
            if (*slice) {
                std::memcpy(out.data(), (*slice)->data() + node.offset, out.size());
                return {};
            }
            const auto owner_type = function_.expressions[node.left].type;
            const std::size_t owner_size = value_size(owner_type);
            if (owner_size <= 256) {
                std::array<std::byte, 256> owner_bytes{};
                ATELIER_TRY(fold_value_bytes(node.left, std::span{owner_bytes.data(), owner_size}));
                std::memcpy(out.data(), owner_bytes.data() + node.offset, out.size());
                return {};
            }
            std::vector<std::byte> owner_bytes(owner_size);
            ATELIER_TRY(fold_value_bytes(node.left, owner_bytes));
            std::memcpy(out.data(), owner_bytes.data() + node.offset, out.size());
            return {};
        }
        if (node.kind == Expression::Kind::index) {
            const auto& seq = function_.expressions[node.left];
            if (seq.kind != Expression::Kind::constant_data)
                return reject(node.position, "Module constant initializers require compile-time constant expressions.");
            const auto& constant = module_.constants[seq.value];
            std::size_t idx = 0;
            if (node.right == no_node) {
                if (constant.element_count == 0)
                    return reject(node.position, "Constant array index is out of bounds.");
                idx = constant.element_count - 1;
            } else {
                ATELIER_TRY_BIND(idx_bits, fold_constant(node.right));
                const auto idx_type = function_.expressions[node.right].type;
                if (idx_type == ValueType::i32 &&
                    std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*idx_bits)) < 0)
                    return reject(node.position, "Constant array index is out of bounds.");
                idx = static_cast<std::size_t>(static_cast<std::uint32_t>(*idx_bits));
                if (idx >= constant.element_count)
                    return reject(node.position, "Constant array index is out of bounds.");
            }
            std::memcpy(out.data(), constant.bytes.data() + idx * out.size(), out.size());
            return {};
        }
        return fold_math_intrinsic(node, out);
    }

    std::expected<std::uint64_t, Diagnostic> fold_constant(NodeId id) {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(function_.expressions[id].position, "Source nesting exceeds the compiler limit of 128.");
        const auto& node = function_.expressions[id];
        if (node.kind == Expression::Kind::literal)
            return node.bits;
        if (node.kind == Expression::Kind::conditional) {
            ATELIER_TRY_BIND(cond, fold_constant(node.receiver));
            return *cond ? fold_constant(node.left) : fold_constant(node.right);
        }
        if (node.kind == Expression::Kind::length) {
            const auto& seq = function_.expressions[node.left];
            if (seq.kind != Expression::Kind::constant_data)
                return reject(node.position, "Module constant initializers require compile-time constant expressions.");
            return static_cast<std::uint32_t>(module_.constants[seq.value].element_count);
        }
        if (node.kind == Expression::Kind::sequence_operation &&
            node.sequence_operation == SequenceOperation::arr_empty) {
            const auto& seq = function_.expressions[node.left];
            if (seq.kind != Expression::Kind::constant_data)
                return reject(node.position, "Module constant initializers require compile-time constant expressions.");
            return module_.constants[seq.value].element_count == 0 ? 1u : 0u;
        }
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index ||
            node.kind == Expression::Kind::math_intrinsic) {
            std::array<std::byte, 8> bytes{};
            ATELIER_TRY(fold_projection_or_intrinsic(node, std::span<std::byte>{bytes.data(), value_size(node.type)}));
            return read_scalar_bits(node.type, bytes.data());
        }
        if (node.kind != Expression::Kind::unary && node.kind != Expression::Kind::binary &&
            node.kind != Expression::Kind::scalar_cast)
            return reject(node.position, "Module constant initializers require compile-time constant expressions.");
        if (node.kind == Expression::Kind::binary && math_type(function_.expressions[node.left].type)) {
            const auto mtype = math_type(function_.expressions[node.left].type);
            std::array<std::byte, 128> lhs_bytes{};
            std::array<std::byte, 128> rhs_bytes{};
            ATELIER_TRY(fold_value_bytes(node.left, std::span<std::byte>{lhs_bytes.data(), mtype.size()}));
            ATELIER_TRY(fold_value_bytes(node.right, std::span<std::byte>{rhs_bytes.data(), mtype.size()}));
            return mtype.precision == math::Precision::f64
                       ? compare_math_lanes<double>(node.operation, lhs_bytes.data(), rhs_bytes.data(),
                                                    mtype.components())
                       : compare_math_lanes<float>(node.operation, lhs_bytes.data(), rhs_bytes.data(),
                                                   mtype.components());
        }
        ATELIER_TRY_BIND(left_result, fold_constant(node.left));
        const auto left = *left_result;
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
                return reject(node.position, "Unsupported constant unary operation.");
            }
        }
        if (node.operation == Operation::logical_and && !left)
            return 0;
        if (node.operation == Operation::logical_or && left)
            return 1;
        ATELIER_TRY_BIND(right_result, fold_constant(node.right));
        const auto right = *right_result;
        if (node.operation == Operation::logical_and || node.operation == Operation::logical_or)
            return right != 0;
        if (node.operation == Operation::shift_left || node.operation == Operation::shift_right) {
            const auto count_type = function_.expressions[node.right].type;
            if (right > 31 ||
                (count_type == ValueType::i32 && std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(right)) < 0))
                return reject(node.position, "Constant shift count is outside 0..31.");
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

    template <class T>
    static void eval_norm_lanes(std::span<const T> input, bool normalize, std::span<T> out) noexcept {
        T maximum = T{0};
        bool finite = true;
        bool has_nan = false;
        for (const T raw : input) {
            const T x = std::abs(raw);
            if (x > maximum)
                maximum = x;
            finite = finite && (x <= std::numeric_limits<T>::max());
            has_nan = has_nan || std::isnan(x);
        }
        const bool valid = finite && (maximum > T{0});
        const T divisor = valid ? maximum : T{1};
        std::array<T, 16> scaled{};
        T sum = T{0};
        for (std::size_t i = 0; i < input.size(); ++i) {
            scaled[i] = (valid ? input[i] : T{0}) / divisor;
            sum += scaled[i] * scaled[i];
        }
        const T magnitude = std::sqrt(sum);
        if (normalize) {
            const T norm_divisor = valid ? magnitude : T{1};
            for (std::size_t i = 0; i < input.size(); ++i)
                out[i] = scaled[i] / norm_divisor;
            return;
        }
        T result = finite ? (maximum * magnitude) : std::numeric_limits<T>::infinity();
        if (has_nan && maximum != std::numeric_limits<T>::infinity())
            result = std::numeric_limits<T>::quiet_NaN();
        out[0] = result;
    }

    template <class T>
    static std::expected<void, Diagnostic> eval_math_lanes(MathOperation op, std::size_t lanes,
                                                           std::span<const std::array<T, 16>> args,
                                                           std::span<std::byte> out, SourcePosition pos) {
        using Op = MathOperation;
        std::array<T, 16> res{};
        const auto write_out = [&](std::size_t count) { std::memcpy(out.data(), res.data(), count * sizeof(T)); };
        if (op >= Op::square_root) {
            for (std::size_t i = 0; i < lanes; ++i) {
                const T a = args[0][i];
                const T b = args.size() > 1 ? args[1][i] : T{};
                const T c = args.size() > 2 ? args[2][i] : T{};
                switch (op) {
                case Op::square_root:
                    res[i] = std::sqrt(a);
                    break;
                case Op::floor:
                    res[i] = std::floor(a);
                    break;
                case Op::ceil:
                    res[i] = std::ceil(a);
                    break;
                case Op::trunc:
                    res[i] = std::trunc(a);
                    break;
                case Op::round:
                    res[i] = std::rint(a);
                    break;
                case Op::fract: {
                    const T diff = a - std::floor(a);
                    const T max_under_one = std::nextafter(T{1}, T{0});
                    res[i] = (max_under_one < diff) ? max_under_one : diff;
                    break;
                }
                case Op::abs:
                    res[i] = std::abs(a);
                    break;
                case Op::sign:
                    res[i] = a > T{0} ? T{1} : (a < T{0} ? T{-1} : a);
                    break;
                case Op::min:
                    res[i] = (b < a) ? b : a;
                    break;
                case Op::max:
                    res[i] = (a < b) ? b : a;
                    break;
                case Op::clamp: {
                    const T m = (c < a) ? c : a;
                    res[i] = (b < m) ? m : b;
                    break;
                }
                case Op::cwise_mul:
                    res[i] = a * b;
                    break;
                case Op::cwise_div:
                    res[i] = a / b;
                    break;
                case Op::radians:
                    res[i] = a * static_cast<T>(std::numbers::pi_v<double> / 180.0);
                    break;
                case Op::degrees:
                    res[i] = a * static_cast<T>(180.0 / std::numbers::pi_v<double>);
                    break;
                case Op::lerp:
                    res[i] = a * (T{1} - c) + b * c;
                    break;
                case Op::step:
                    res[i] = (b < a) ? T{0} : T{1};
                    break;
                case Op::smoothstep: {
                    const T raw = (c - a) / (b - a);
                    const T m = (T{1} < raw) ? T{1} : raw;
                    const T t = (T{0} < m) ? m : T{0};
                    res[i] = t * t * (T{3} - T{2} * t);
                    break;
                }
                case Op::sin:
                    res[i] = std::sin(a);
                    break;
                case Op::cos:
                    res[i] = std::cos(a);
                    break;
                case Op::atan2:
                    res[i] = std::atan2(a, b);
                    break;
                case Op::exp:
                    res[i] = std::exp(a);
                    break;
                case Op::log:
                    res[i] = std::log(a);
                    break;
                case Op::pow:
                    if (b == T{0} || a == T{1})
                        res[i] = T{1};
                    else if (std::isnan(a) || std::isnan(b) || a < T{0})
                        res[i] = std::numeric_limits<T>::quiet_NaN();
                    else
                        res[i] = std::pow(std::abs(a), b);
                    break;
                default:
                    return reject(pos, "This math operation is not supported in compile-time constant initializers.");
                }
            }
            write_out(lanes);
            return {};
        }
        switch (op) {
        case Op::dot: {
            T sum{};
            for (std::size_t i = 0; i < lanes; ++i)
                sum += args[0][i] * args[1][i];
            res[0] = sum;
            write_out(1);
            return {};
        }
        case Op::cross:
            res[0] = args[0][1] * args[1][2] - args[0][2] * args[1][1];
            res[1] = args[0][2] * args[1][0] - args[0][0] * args[1][2];
            res[2] = args[0][0] * args[1][1] - args[0][1] * args[1][0];
            write_out(3);
            return {};
        case Op::length_squared: {
            T sum{};
            for (std::size_t i = 0; i < lanes; ++i)
                sum += args[0][i] * args[0][i];
            res[0] = sum;
            write_out(1);
            return {};
        }
        case Op::length:
            eval_norm_lanes<T>(std::span<const T>{args[0].data(), lanes}, false, std::span<T>{res.data(), 1});
            write_out(1);
            return {};
        case Op::distance_squared: {
            T sum{};
            for (std::size_t i = 0; i < lanes; ++i) {
                const T d = args[1][i] - args[0][i];
                sum += d * d;
            }
            res[0] = sum;
            write_out(1);
            return {};
        }
        case Op::distance: {
            std::array<T, 16> diff{};
            for (std::size_t i = 0; i < lanes; ++i)
                diff[i] = args[1][i] - args[0][i];
            eval_norm_lanes<T>(std::span<const T>{diff.data(), lanes}, false, std::span<T>{res.data(), 1});
            write_out(1);
            return {};
        }
        case Op::normalize_or_zero:
            eval_norm_lanes<T>(std::span<const T>{args[0].data(), lanes}, true, std::span<T>{res.data(), lanes});
            write_out(lanes);
            return {};
        case Op::conjugate:
            res[0] = -args[0][0];
            res[1] = -args[0][1];
            res[2] = -args[0][2];
            res[3] = args[0][3];
            write_out(4);
            return {};
        case Op::rotate: {
            const T qx = args[0][0];
            const T qy = args[0][1];
            const T qz = args[0][2];
            const T qw = args[0][3];
            const T vx = args[1][0];
            const T vy = args[1][1];
            const T vz = args[1][2];
            const T tx = (qy * vz - qz * vy) * T{2};
            const T ty = (qz * vx - qx * vz) * T{2};
            const T tz = (qx * vy - qy * vx) * T{2};
            res[0] = (vx + tx * qw) + (qy * tz - qz * ty);
            res[1] = (vy + ty * qw) + (qz * tx - qx * tz);
            res[2] = (vz + tz * qw) + (qx * ty - qy * tx);
            write_out(3);
            return {};
        }
        case Op::transpose:
            for (std::size_t r = 0; r < 4; ++r)
                for (std::size_t c = 0; c < 4; ++c)
                    res[r * 4 + c] = args[0][c * 4 + r];
            write_out(16);
            return {};
        case Op::transform_point:
        case Op::transform_direction: {
            const T vx = args[0][0];
            const T vy = args[0][1];
            const T vz = args[0][2];
            for (std::size_t c = 0; c < 3; ++c) {
                res[c] = (vx * args[1][c] + vy * args[1][4 + c]) + vz * args[1][8 + c];
                if (op == Op::transform_point)
                    res[c] += args[1][12 + c];
            }
            write_out(3);
            return {};
        }
        default:
            return reject(pos, "This math operation is not supported in compile-time constant initializers.");
        }
    }

    std::expected<void, Diagnostic> fold_math_intrinsic(const Expression& node, std::span<std::byte> out) {
        using Op = MathOperation;
        if (node.math_operation == Op::as_uint || node.math_operation == Op::as_int) {
            ATELIER_TRY_BIND(bits, fold_constant(node.arguments[0]));
            write_scalar_bytes(node.type, static_cast<std::uint32_t>(*bits), out.data());
            return {};
        }
        if ((node.math_operation == Op::abs || node.math_operation == Op::min || node.math_operation == Op::max ||
             node.math_operation == Op::clamp) &&
            (node.type == ValueType::i32 || node.type == ValueType::u32)) {
            ATELIER_TRY_BIND(a_bits, fold_constant(node.arguments[0]));
            if (node.math_operation == Op::abs) {
                const auto a = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*a_bits));
                if (a == std::numeric_limits<std::int32_t>::min())
                    return reject(node.position, "Constant integer abs overflows INT_MIN.");
                write_scalar_bytes(node.type, static_cast<std::uint32_t>(a < 0 ? -a : a), out.data());
                return {};
            }
            ATELIER_TRY_BIND(b_bits, fold_constant(node.arguments[1]));
            std::uint64_t c_raw{};
            if (node.math_operation == Op::clamp) {
                ATELIER_TRY_BIND(c_bits, fold_constant(node.arguments[2]));
                c_raw = *c_bits;
            }
            const auto eval_int = [&](auto a, auto b, auto c) {
                const auto res = node.math_operation == Op::min   ? std::min(a, b)
                                 : node.math_operation == Op::max ? std::max(a, b)
                                                                  : std::max(b, std::min(a, c));
                write_scalar_bytes(node.type, static_cast<std::uint32_t>(res), out.data());
            };
            if (node.type == ValueType::i32)
                eval_int(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*a_bits)),
                         std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(*b_bits)),
                         std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(c_raw)));
            else
                eval_int(static_cast<std::uint32_t>(*a_bits), static_cast<std::uint32_t>(*b_bits),
                         static_cast<std::uint32_t>(c_raw));
            return {};
        }
        if (node.math_operation == Op::is_finite) {
            const auto arg_type = function_.expressions[node.arguments[0]].type;
            const auto m = math_type(arg_type);
            std::array<std::byte, 128> arg_bytes{};
            const std::size_t sz = value_size(arg_type);
            ATELIER_TRY(fold_value_bytes(node.arguments[0], std::span<std::byte>{arg_bytes.data(), sz}));
            const std::size_t lanes = m ? m.components() : 1;
            const bool f64 = m ? m.precision == math::Precision::f64 : arg_type == ValueType::f64;
            const bool ok =
                f64 ? lanes_finite<double>(arg_bytes.data(), lanes) : lanes_finite<float>(arg_bytes.data(), lanes);
            write_scalar_bytes(ValueType::boolean, ok ? 1u : 0u, out.data());
            return {};
        }
        const auto first_type = function_.expressions[node.arguments[0]].type;
        const auto first_math = math_type(first_type);
        const auto res_math = math_type(node.type);
        const std::size_t lanes = res_math ? res_math.components() : (first_math ? first_math.components() : 1);
        const bool is_f64 = (res_math && res_math.precision == math::Precision::f64) ||
                            (first_math && first_math.precision == math::Precision::f64) ||
                            first_type == ValueType::f64 || node.type == ValueType::f64;
        return is_f64 ? fold_float_math_intrinsic<double>(node, lanes, out)
                      : fold_float_math_intrinsic<float>(node, lanes, out);
    }

    template <class T>
    static std::uint64_t compare_math_lanes(Operation op, const std::byte* lhs, const std::byte* rhs,
                                            std::size_t lanes) noexcept {
        bool all_equal = true;
        bool any_unequal = false;
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            T a{};
            T b{};
            std::memcpy(&a, lhs + lane * sizeof(T), sizeof(T));
            std::memcpy(&b, rhs + lane * sizeof(T), sizeof(T));
            all_equal &= (a == b);
            any_unequal |= !(a == b);
        }
        return op == Operation::equal ? (all_equal ? 1u : 0u) : (any_unequal ? 1u : 0u);
    }

    template <class T>
    static bool lanes_finite(const std::byte* bytes, std::size_t lanes) noexcept {
        bool ok = true;
        for (std::size_t i = 0; i < lanes; ++i) {
            T v{};
            std::memcpy(&v, bytes + i * sizeof(T), sizeof(T));
            ok &= std::isfinite(v);
        }
        return ok;
    }

    template <class T>
    static void negate_lanes(std::byte* bytes, std::size_t lanes) noexcept {
        for (std::size_t i = 0; i < lanes; ++i) {
            T v{};
            std::memcpy(&v, bytes + i * sizeof(T), sizeof(T));
            v = -v;
            std::memcpy(bytes + i * sizeof(T), &v, sizeof(T));
        }
    }

    template <class T>
    static std::array<T, 16> unpack_lanes(math::Type mtype, const std::byte* raw) noexcept {
        std::array<T, 16> lanes{};
        if (mtype) {
            std::memcpy(lanes.data(), raw, mtype.components() * sizeof(T));
        } else {
            T scalar_val{};
            std::memcpy(&scalar_val, raw, sizeof(T));
            lanes.fill(scalar_val);
        }
        return lanes;
    }

    template <class T>
    std::expected<void, Diagnostic> fold_float_math_intrinsic(const Expression& node, std::size_t lanes,
                                                              std::span<std::byte> out) {
        std::array<std::array<T, 16>, 3> unpacked{};
        for (std::size_t a = 0; a < node.arguments.size(); ++a) {
            const auto atype = function_.expressions[node.arguments[a]].type;
            std::array<std::byte, 128> raw{};
            ATELIER_TRY(fold_value_bytes(node.arguments[a], std::span<std::byte>{raw.data(), value_size(atype)}));
            unpacked[a] = unpack_lanes<T>(math_type(atype), raw.data());
        }
        return eval_math_lanes<T>(node.math_operation, lanes, std::span{unpacked.data(), node.arguments.size()}, out,
                                  node.position);
    }

    template <class T>
    static void fold_math_binary_lanes(Operation op, math::Type left_math, math::Type right_math,
                                       const std::byte* lhs_raw, const std::byte* rhs_raw, std::byte* out) noexcept {
        const auto mtype = left_math ? left_math : right_math;
        const auto lhs = unpack_lanes<T>(left_math, lhs_raw);
        const auto rhs = unpack_lanes<T>(right_math, rhs_raw);
        std::array<T, 16> res{};
        if (op == Operation::multiply && left_math && right_math) {
            if (left_math.shape == math::Shape::quaternion) {
                res[0] = lhs[3] * rhs[0] + lhs[0] * rhs[3] + lhs[1] * rhs[2] - lhs[2] * rhs[1];
                res[1] = lhs[3] * rhs[1] - lhs[0] * rhs[2] + lhs[1] * rhs[3] + lhs[2] * rhs[0];
                res[2] = lhs[3] * rhs[2] + lhs[0] * rhs[1] - lhs[1] * rhs[0] + lhs[2] * rhs[3];
                res[3] = lhs[3] * rhs[3] - lhs[0] * rhs[0] - lhs[1] * rhs[1] - lhs[2] * rhs[2];
                std::memcpy(out, res.data(), 4 * sizeof(T));
                return;
            }
            if (left_math.shape == math::Shape::matrix4) {
                for (std::size_t r = 0; r < 4; ++r)
                    for (std::size_t c = 0; c < 4; ++c) {
                        T sum{};
                        for (std::size_t k = 0; k < 4; ++k)
                            sum += lhs[r * 4 + k] * rhs[k * 4 + c];
                        res[r * 4 + c] = sum;
                    }
                std::memcpy(out, res.data(), 16 * sizeof(T));
                return;
            }
        }
        for (std::size_t i = 0; i < mtype.components(); ++i) {
            switch (op) {
            case Operation::add:
                res[i] = lhs[i] + rhs[i];
                break;
            case Operation::subtract:
                res[i] = lhs[i] - rhs[i];
                break;
            case Operation::multiply:
                res[i] = lhs[i] * rhs[i];
                break;
            case Operation::divide:
                res[i] = lhs[i] / rhs[i];
                break;
            default:
                break;
            }
        }
        std::memcpy(out, res.data(), mtype.components() * sizeof(T));
    }

    std::expected<void, Diagnostic> fold_value_bytes(NodeId id, std::span<std::byte> out) {
        const auto& node = function_.expressions[id];
        if (scalar(node.type)) {
            ATELIER_TRY_BIND(bits, fold_constant(id));
            write_scalar_bytes(node.type, *bits, out.data());
            return {};
        }
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(node.position, "Source nesting exceeds the compiler limit of 128.");
        if (node.kind == Expression::Kind::conditional) {
            ATELIER_TRY_BIND(cond, fold_constant(node.receiver));
            return fold_value_bytes(*cond ? node.left : node.right, out);
        }
        if (node.kind == Expression::Kind::constant_data) {
            const auto& constant = module_.constants[node.value];
            std::memcpy(out.data(), constant.bytes.data(), out.size());
            return {};
        }
        if (node.kind == Expression::Kind::value_field || node.kind == Expression::Kind::index ||
            node.kind == Expression::Kind::math_intrinsic)
            return fold_projection_or_intrinsic(node, out);
        if (const auto mtype = math_type(node.type)) {
            const bool f64 = mtype.precision == math::Precision::f64;
            const std::size_t lane_bytes = mtype.scalar_size();
            if (node.kind == Expression::Kind::math_construct) {
                std::memset(out.data(), 0, out.size());
                if (node.arguments.empty()) {
                    if (mtype.shape == math::Shape::quaternion) {
                        if (f64) {
                            const double one = 1.0;
                            std::memcpy(out.data() + 3 * 8, &one, 8);
                        } else {
                            const float one = 1.0f;
                            std::memcpy(out.data() + 3 * 4, &one, 4);
                        }
                    }
                    return {};
                }
                if (node.arguments.size() == 1) {
                    const auto arg_type = function_.expressions[node.arguments[0]].type;
                    if (const auto arg_math = math_type(arg_type)) {
                        std::array<std::byte, 128> src{};
                        ATELIER_TRY(fold_value_bytes(node.arguments[0], std::span{src.data(), arg_math.size()}));
                        for (std::size_t i = 0; i < mtype.components(); ++i) {
                            if (arg_math.precision == mtype.precision) {
                                std::memcpy(out.data() + i * lane_bytes, src.data() + i * lane_bytes, lane_bytes);
                            } else if (f64) {
                                float v{};
                                std::memcpy(&v, src.data() + i * 4, 4);
                                const double w = static_cast<double>(v);
                                std::memcpy(out.data() + i * 8, &w, 8);
                            } else {
                                double v{};
                                std::memcpy(&v, src.data() + i * 8, 8);
                                const float w = static_cast<float>(v);
                                std::memcpy(out.data() + i * 4, &w, 4);
                            }
                        }
                        return {};
                    }
                    ATELIER_TRY_BIND(bits, fold_constant(node.arguments[0]));
                    if (mtype.vector()) {
                        for (std::size_t i = 0; i < mtype.components(); ++i)
                            write_scalar_bytes(math_scalar(mtype), *bits, out.data() + i * lane_bytes);
                    } else {
                        for (std::size_t i = 0; i < 16; i += 5)
                            write_scalar_bytes(math_scalar(mtype), *bits, out.data() + i * lane_bytes);
                    }
                    return {};
                }
                for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                    ATELIER_TRY_BIND(bits, fold_constant(node.arguments[i]));
                    write_scalar_bytes(math_scalar(mtype), *bits, out.data() + i * lane_bytes);
                }
                return {};
            }
            if (node.kind == Expression::Kind::unary) {
                ATELIER_TRY(fold_value_bytes(node.left, out));
                if (node.operation == Operation::negate) {
                    if (f64)
                        negate_lanes<double>(out.data(), mtype.components());
                    else
                        negate_lanes<float>(out.data(), mtype.components());
                }
                return {};
            }
            if (node.kind == Expression::Kind::binary) {
                const auto ltype = function_.expressions[node.left].type;
                const auto rtype = function_.expressions[node.right].type;
                std::array<std::byte, 128> lhs{};
                std::array<std::byte, 128> rhs{};
                ATELIER_TRY(fold_value_bytes(node.left, std::span{lhs.data(), value_size(ltype)}));
                ATELIER_TRY(fold_value_bytes(node.right, std::span{rhs.data(), value_size(rtype)}));
                if (f64)
                    fold_math_binary_lanes<double>(node.operation, math_type(ltype), math_type(rtype), lhs.data(),
                                                   rhs.data(), out.data());
                else
                    fold_math_binary_lanes<float>(node.operation, math_type(ltype), math_type(rtype), lhs.data(),
                                                  rhs.data(), out.data());
                return {};
            }
        }
        return reject(node.position, "Module constant initializers require compile-time constant expressions.");
    }

    std::expected<void, Diagnostic> parse_constant_bytes(ValueType target_type, std::span<std::byte> out) {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        if (token_.text == "{") {
            const auto brace_pos = token_.position;
            if (!plain_record(target_type) || math_type(target_type) || !parallel_value_type(target_type))
                return reject(brace_pos, "Brace initializers require a plain struct or constant Array type.");
            ATELIER_TRY(consume());
            const auto& layout = environment_.values[target_type.type_index];
            if (!layout.initial_value.empty())
                std::memcpy(out.data(), layout.initial_value.data(), layout.size);
            else
                std::memset(out.data(), 0, layout.size);
            ATELIER_TRY_BIND(empty, accept("}"));
            if (*empty)
                return {};
            std::size_t field_index = 0;
            for (;;) {
                if (field_index >= layout.fields.size())
                    return reject(token_.position, "Too many initializers for struct '" + layout.name + "'.");
                const auto& field = layout.fields[field_index];
                ATELIER_TRY(parse_constant_bytes(field.type, out.subspan(field.offset, value_size(field.type))));
                ++field_index;
                ATELIER_TRY_BIND(comma, accept(","));
                if (!*comma)
                    break;
                if (token_.text == "}")
                    break;
            }
            if (field_index != layout.fields.size())
                return reject(token_.position, "Struct constant initializer for '" + layout.name +
                                                   "' requires either '{}' or all fields in order.");
            ATELIER_TRY(expect("}"));
            return {};
        }
        ATELIER_TRY_BIND(parsed, expression());
        ATELIER_TRY_BIND(initializer, converted(*parsed, target_type));
        ATELIER_TRY(finish_properties());
        return fold_value_bytes(*initializer, out);
    }

    std::expected<void, Diagnostic> resolve_constant(std::size_t index) {
        auto& value = constants_[index];
        if (value.state == Constant::State::complete)
            return {};
        if (value.state == Constant::State::active)
            return reject(value.position, "Circular module constant dependency.");
        value.state = Constant::State::active;
        struct ConstantScope {
            Parser& parser;

            Lexer lexer;

            Token token;

            Function function;

            std::vector<std::size_t> heights;

            std::vector<std::uint32_t> visible;

            std::vector<PropertyUse> property_uses;

            explicit ConstantScope(Parser& owner)
                : parser(owner), lexer(owner.lexer_), token(owner.token_), function(std::move(owner.function_)),
                  heights(std::move(owner.expression_heights_)), visible(std::move(owner.visible_)),
                  property_uses(std::move(owner.property_uses_)) {
                parser.function_ = {};
                parser.expression_heights_.clear();
                parser.visible_.clear();
                parser.property_uses_.clear();
            }

            ~ConstantScope() {
                parser.lexer_ = lexer;
                parser.token_ = token;
                parser.function_ = std::move(function);
                parser.expression_heights_ = std::move(heights);
                parser.visible_ = std::move(visible);
                parser.property_uses_ = std::move(property_uses);
            }
        } scope{*this};
        lexer_ = value.lexer;
        token_ = value.token;

        if (value.type.kind == ValueType::array) {
            const auto elem_type = environment_.values[value.type.type_index].element;
            const std::size_t stride = value_size(elem_type);
            ATELIER_TRY_BIND(open_brace, accept("{"));
            if (*open_brace) {
                ConstantData data{value.type, 0, {}};
                if (token_.text != "}") {
                    for (;;) {
                        if (data.element_count >= 65536 || data.bytes.size() + stride > 4 * 1024 * 1024)
                            return reject(token_.position,
                                          "Constant array exceeds the 65536-element or 4 MiB compile-time limit.");
                        const std::size_t offset = data.bytes.size();
                        data.bytes.resize(offset + stride);
                        function_.expressions.clear();
                        expression_heights_.clear();
                        ATELIER_TRY(parse_constant_bytes(elem_type, std::span{data.bytes.data() + offset, stride}));
                        ++data.element_count;
                        ATELIER_TRY_BIND(comma, accept(","));
                        if (!*comma || token_.text == "}")
                            break;
                    }
                }
                ATELIER_TRY(expect("}"));
                ATELIER_TRY(expect(";"));
                value.data_index = static_cast<std::uint32_t>(module_.constants.size());
                module_.constants.push_back(std::move(data));
            } else {
                ATELIER_TRY_BIND(parsed, expression());
                ATELIER_TRY_BIND(init, converted(*parsed, value.type));
                ATELIER_TRY(finish_properties());
                const auto& node = function_.expressions[*init];
                if (node.kind != Expression::Kind::constant_data)
                    return reject(node.position,
                                  "Constant Array requires a '{ ... }' initializer or another constant Array.");
                ATELIER_TRY(expect(";"));
                value.data_index = node.value;
            }
            value.state = Constant::State::complete;
            return {};
        }

        if (scalar(value.type) || math_type(value.type)) {
            ATELIER_TRY(parse_constant_bytes(value.type, std::span{value.inline_bytes.data(), value_size(value.type)}));
            ATELIER_TRY(expect(";"));
            if (environment_.material && value.type != ValueType::boolean) {
                const auto mtype = math_type(value.type);
                if (!lanes_finite<float>(value.inline_bytes.data(), mtype ? mtype.components() : 1))
                    return reject(value.position, "Material constants must be finite.");
            }
            value.state = Constant::State::complete;
            return {};
        }

        ConstantData data{value.type, 1, std::vector<std::byte>(value_size(value.type))};
        ATELIER_TRY(parse_constant_bytes(value.type, data.bytes));
        ATELIER_TRY(expect(";"));
        value.data_index = static_cast<std::uint32_t>(module_.constants.size());
        module_.constants.push_back(std::move(data));
        value.state = Constant::State::complete;
        return {};
    }

    std::expected<NodeId, Diagnostic> block() {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        ATELIER_TRY_BIND(open, expect("{"));
        Statement result{Statement::Kind::block, open->position, no_node, no_node, no_node, no_node, {}};
        const auto visible_before = visible_.size();
        while (token_.text != "}") {
            if (token_.kind == Token::Kind::end)
                return reject(token_.position, "Expected '}' before end of source.");
            ATELIER_TRY_BIND(child, statement());
            result.children.push_back(*child);
        }
        ATELIER_TRY(consume());
        result.locals.assign(visible_.begin() + visible_before, visible_.end());
        visible_.resize(visible_before);
        return add(std::move(result));
    }

    std::expected<bool, Diagnostic> starts_declaration() const {
        if (token_.text == "const" || token_.text == "bool" || token_.text == "float" || token_.text == "double" ||
            token_.text == "uint" || token_.text == "int" || token_.text == "weak")
            return true;
        return reference_declaration();
    }

    std::expected<NodeId, Diagnostic> simple_statement(std::string_view terminator, bool allow_declaration = true) {
        const auto position = token_.position;
        ATELIER_TRY_BIND(declaration, starts_declaration());
        if (*declaration) {
            if (!allow_declaration)
                return reject(position, "A for step accepts assignment, call or an empty clause.");
            ATELIER_TRY_BIND(read_only, accept("const"));
            ATELIER_TRY_BIND(weak, accept("weak"));
            ATELIER_TRY_BIND(parsed_type, parameter_type());
            auto type = *parsed_type;
            if (*weak) {
                if (type != ValueType::strong_reference)
                    return reject(position, "weak requires an object type.");
                type = ValueType::weak_reference;
            }
            if (*read_only && reference_type(type))
                return reject(position, "const object reference locals are not supported.");
            if (aggregate(type) && !math_type(type)) {
                if (!environment_.values[type.type_index].can_own)
                    return reject(position,
                                  "Owned locals require values with borrow-preserving copy and lifecycle operations.");
                function_.uses_scoped_values = true;
            }
            ATELIER_TRY_BIND(name, identifier());
            ATELIER_TRY(unused_name(*name));
            ATELIER_TRY_BIND(initialized, accept("="));
            if (*read_only && !*initialized)
                return reject(token_.position, "const locals require an initializer.");
            if (!*initialized && !reference_type(type) && !aggregate(type))
                return reject(token_.position, "Expected '='.");
            NodeId initializer = no_node;
            if (*initialized) {
                ATELIER_TRY_BIND(parsed, expression());
                initializer = *parsed;
            } else if (reference_type(type)) {
                ATELIER_TRY_BIND(
                    null, add(Expression{Expression::Kind::null_reference, ValueType::null_reference, name->position}));
                initializer = *null;
            }
            if (initializer != no_node) {
                ATELIER_TRY_BIND(value, converted(initializer, type));
                initializer = *value;
            }
            ATELIER_TRY(expect(terminator));
            ATELIER_TRY_BIND(index, local(*name, type));
            function_.locals[*index].read_only = *read_only;
            return add(Statement{Statement::Kind::declare, position, *index, initializer, no_node, no_node, {}});
        }
        ATELIER_TRY_BIND(parsed_target, expression());
        const auto target = *parsed_target;
        ATELIER_TRY_BIND(assigned, accept("="));
        if (*assigned) {
            const auto use = std::ranges::find(property_uses_, target, &PropertyUse::node);
            if (use != property_uses_.end()) {
                const auto property = properties_[use->property];
                const auto node = function_.expressions[target];
                if (property.setter == UINT32_MAX)
                    return reject(position, "Property is read-only: " + property.name);
                property_uses_.erase(use);
                ATELIER_TRY_BIND(value, expression());
                ATELIER_TRY_BIND(
                    call, call_expression(property.native, property.setter, node.position, node.receiver, {*value}));
                // The RHS can be deeper than the original property placeholder.
                if (expression_heights_[call->arguments[0]] >= 128)
                    return reject(position, "Expression tree exceeds the compiler limit of 128.");
                function_.expressions[target] = std::move(*call);
                ATELIER_TRY(expect(terminator));
                return add(Statement{Statement::Kind::call, position, no_node, target, no_node, no_node, {}});
            }
            if (!writable(target))
                return reject(position, "Assignment requires a writable local or field.");
            if (const auto& tnode = function_.expressions[target];
                tnode.kind == Expression::Kind::local && function_.locals[tnode.value].fixed_length)
                return reject(
                    position,
                    "SOP [InOut] and [Out] attribute columns have fixed element count and cannot change structure.");
            const auto type = function_.expressions[target].type;
            ATELIER_TRY_BIND(parsed, expression());
            ATELIER_TRY_BIND(value, converted(*parsed, type));
            ATELIER_TRY(expect(terminator));
            return add(Statement{Statement::Kind::assign, position, target, *value, no_node, no_node, {}});
        }
        if (const auto compound = compound_assignment_operator(token_.text)) {
            ATELIER_TRY_BIND(symbol, consume());
            if (std::ranges::find(property_uses_, target, &PropertyUse::node) != property_uses_.end())
                return reject(symbol->position,
                              "Compound assignment is not supported on Get/Set properties; use explicit assignment.");
            const auto target_type = function_.expressions[target].type;
            if (target_type.kind == ValueType::string && *compound == Operation::add) {
                if (const auto& tnode = function_.expressions[target];
                    tnode.kind == Expression::Kind::local && function_.locals[tnode.value].fixed_length)
                    return reject(
                        symbol->position,
                        "SOP [InOut] and [Out] attribute columns have fixed element count and cannot change structure.");
                if (!mutable_local_storage(target) || !environment_.values[target_type.type_index].can_own ||
                    !environment_.values[target_type.type_index].can_mutate)
                    return reject(symbol->position, "Structural mutation requires a mutable owned or borrowed sequence.");
                ATELIER_TRY_BIND(parsed_rhs, expression());
                ATELIER_TRY_BIND(rhs, converted(*parsed_rhs, target_type));
                ATELIER_TRY(expect(terminator));
                Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, symbol->position,
                                    target, *rhs};
                mutation.mutation = SequenceMutation::append;
                ATELIER_TRY_BIND(mutated, add(std::move(mutation)));
                return add(Statement{Statement::Kind::call, position, no_node, *mutated, no_node, no_node, {}});
            }
            if (!writable(target))
                return reject(position, "Assignment requires a writable local or field.");
            ATELIER_TRY_BIND(parsed_rhs, expression());
            auto rhs = *parsed_rhs;
            const bool shift = *compound == Operation::shift_left || *compound == Operation::shift_right;
            if (!shift && target_type == ValueType::i32 && integer_constant(rhs))
                signed_constant(rhs);
            ATELIER_TRY_BIND(result_type, check_binary_types(*compound, false, symbol->position, target_type,
                                                             function_.expressions[rhs].type));
            if (*result_type != target_type)
                return reject(symbol->position, "Compound assignment result type does not match the target type.");
            ATELIER_TRY(expect(terminator));
            Statement stmt{Statement::Kind::assign, position, target, rhs, no_node, no_node, {}};
            stmt.compound = *compound;
            return add(std::move(stmt));
        }
        const auto& target_expr = function_.expressions[target];
        if (target_expr.kind != Expression::Kind::call && target_expr.kind != Expression::Kind::source_call &&
            target_expr.kind != Expression::Kind::sequence_mutation &&
            !(target_expr.kind == Expression::Kind::sequence_operation &&
              can_be_statement_sequence_operation(target_expr.sequence_operation)))
            return reject(position, "Only calls and sequence modifications can be used as expression statements.");
        ATELIER_TRY(expect(terminator));
        return add(Statement{Statement::Kind::call, position, no_node, target, no_node, no_node, {}});
    }

    std::expected<NodeId, Diagnostic> statement() {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        const auto position = token_.position;
        if (token_.text == "{")
            return block();
        ATELIER_TRY_BIND(declaration, starts_declaration());
        if (*declaration)
            return simple_statement(";");
        ATELIER_TRY_BIND(conditional, accept("if"));
        if (*conditional) {
            ATELIER_TRY(expect("("));
            ATELIER_TRY_BIND(condition, expression());
            if (function_.expressions[*condition].type != ValueType::boolean)
                return reject(position, "An if condition requires bool.");
            ATELIER_TRY(expect(")"));
            ATELIER_TRY_BIND(body, block());
            NodeId alternative = no_node;
            ATELIER_TRY_BIND(otherwise, accept("else"));
            if (*otherwise) {
                ATELIER_TRY_BIND(parsed, token_.text == "if" ? statement() : block());
                alternative = *parsed;
            }
            return add(Statement{Statement::Kind::condition, position, no_node, *condition, *body, alternative, {}});
        }
        ATELIER_TRY_BIND(while_loop, accept("while"));
        if (*while_loop) {
            ATELIER_TRY(expect("("));
            ATELIER_TRY_BIND(condition, expression());
            if (function_.expressions[*condition].type != ValueType::boolean)
                return reject(position, "A while condition requires bool.");
            ATELIER_TRY(expect(")"));
            ++loop_depth_;
            ATELIER_TRY_BIND(body, block());
            --loop_depth_;
            return add(Statement{Statement::Kind::loop, position, no_node, *condition, *body, no_node, {}});
        }
        ATELIER_TRY_BIND(for_loop, accept("for"));
        if (*for_loop) {
            ATELIER_TRY(expect("("));
            const auto visible_before = visible_.size();
            Statement scope{Statement::Kind::block, position, no_node, no_node, no_node, no_node, {}};
            ATELIER_TRY_BIND(empty, accept(";"));
            if (!*empty) {
                ATELIER_TRY_BIND(initializer, simple_statement(";"));
                scope.children.push_back(*initializer);
            }
            ATELIER_TRY_BIND(condition,
                             token_.text == ";" ? literal(ValueType::boolean, token_.position, 1) : expression());
            if (function_.expressions[*condition].type != ValueType::boolean)
                return reject(position, "A for condition requires bool.");
            ATELIER_TRY(expect(";"));
            ATELIER_TRY_BIND(closed, accept(")"));
            NodeId step = no_node;
            if (!*closed) {
                ATELIER_TRY_BIND(parsed, simple_statement(")", false));
                step = *parsed;
            }
            ++loop_depth_;
            ATELIER_TRY_BIND(body, block());
            --loop_depth_;
            Statement loop{Statement::Kind::loop, position, no_node, *condition, *body, no_node, {}};
            loop.step = step;
            scope.children.push_back(add(std::move(loop)));
            scope.locals.assign(visible_.begin() + visible_before, visible_.end());
            visible_.resize(visible_before);
            return add(std::move(scope));
        }
        ATELIER_TRY_BIND(parallel_loop, accept("parallel_for"));
        if (*parallel_loop) {
            if (function_.is_coroutine)
                return reject(position, "parallel_for cannot be used in an async function.");
            if (parallel_loop_depth_ > 0)
                return reject(position, "parallel_for cannot be nested.");
            ATELIER_TRY(expect("("));
            ATELIER_TRY_BIND(explicit_const, accept("const"));
            (void)explicit_const;
            ATELIER_TRY(expect("uint"));
            ATELIER_TRY_BIND(ivar_name, identifier());
            ATELIER_TRY(unused_name(*ivar_name));
            ATELIER_TRY(expect(":"));
            ATELIER_TRY_BIND(count, expression());
            const auto count_type = function_.expressions[*count].type;
            if (count_type != ValueType::u32 && count_type != ValueType::i32)
                return reject(function_.expressions[*count].position, "parallel_for count requires uint or int.");
            ATELIER_TRY(expect(")"));
            const auto visible_before = visible_.size();
            ATELIER_TRY_BIND(ivar, local(*ivar_name, ValueType::u32));
            function_.locals[*ivar].read_only = true;
            const auto saved_loop_depth = std::exchange(loop_depth_, 0);
            ++parallel_loop_depth_;
            ATELIER_TRY_BIND(body, block());
            --parallel_loop_depth_;
            loop_depth_ = saved_loop_depth;
            visible_.resize(visible_before);
            return add(Statement{Statement::Kind::parallel_loop, position, *ivar, *count, *body, no_node, {}});
        }
        if (token_.text == "break" || token_.text == "continue") {
            ATELIER_TRY_BIND(keyword, consume());
            if (keyword->text == "break") {
                if (loop_depth_ == 0)
                    return reject(position, parallel_loop_depth_ > 0
                                                ? "break cannot exit parallel_for; use fail(...) for errors."
                                                : "break and continue require an enclosing loop.");
            } else if (loop_depth_ == 0 && parallel_loop_depth_ == 0) {
                return reject(position, "break and continue require an enclosing loop.");
            }
            ATELIER_TRY(expect(";"));
            return add(
                Statement{keyword->text == "break" ? Statement::Kind::break_loop : Statement::Kind::continue_loop,
                          position,
                          no_node,
                          no_node,
                          no_node,
                          no_node,
                          {}});
        }
        ATELIER_TRY_BIND(failed, accept("fail"));
        if (*failed) {
            ATELIER_TRY(expect("("));
            if (token_.kind != Token::Kind::string)
                return reject(token_.position, "fail requires a string literal message.");
            ATELIER_TRY_BIND(message_token, consume());
            ATELIER_TRY_BIND(decoded, string_value(*message_token));
            NodeId fail_layer = no_node;
            NodeId fail_index = no_node;
            ATELIER_TRY_BIND(has_extra, accept(","));
            if (*has_extra) {
                ATELIER_TRY_BIND(first_arg, expression());
                ATELIER_TRY_BIND(has_third, accept(","));
                if (*has_third) {
                    const auto layer_type = function_.expressions[*first_arg].type;
                    if (layer_type.kind != ValueType::enumeration || layer_type.type_index >= 0xffffu ||
                        environment_.enumerations[layer_type.type_index].flags ||
                        environment_.enumerations[layer_type.type_index].underlying != ValueType::u32 ||
                        environment_.enumerations[layer_type.type_index].values.size() >= 0xffffu) {
                        return reject(function_.expressions[*first_arg].position,
                                      "fail element layer requires a uint-backed non-flags enum.");
                    }
                    fail_layer = *first_arg;
                    ATELIER_TRY_BIND(second_arg, expression());
                    fail_index = *second_arg;
                } else {
                    fail_index = *first_arg;
                }
                const auto idx_type = function_.expressions[fail_index].type;
                if (idx_type != ValueType::u32 && idx_type != ValueType::i32)
                    return reject(function_.expressions[fail_index].position,
                                  "fail element index requires uint or int.");
            }
            ATELIER_TRY(expect(")"));
            ATELIER_TRY(expect(";"));
            std::uint32_t message_index = 0;
            const auto existing = std::ranges::find(module_.failure_messages, *decoded);
            if (existing != module_.failure_messages.end())
                message_index = static_cast<std::uint32_t>(existing - module_.failure_messages.begin());
            else {
                message_index = static_cast<std::uint32_t>(module_.failure_messages.size());
                module_.failure_messages.push_back(std::move(*decoded));
            }
            return add(Statement{Statement::Kind::fail, position, message_index, fail_index, fail_layer, no_node, {}});
        }
        ATELIER_TRY_BIND(yield, accept("yield"));
        if (*yield) {
            if (!function_.is_coroutine)
                return reject(position, "yield requires an async entry.");
            ATELIER_TRY(expect(";"));
            return add(Statement{Statement::Kind::yield, position, no_node, no_node, no_node, no_node, {}});
        }
        ATELIER_TRY_BIND(return_value, accept("return"));
        if (*return_value) {
            if (parallel_loop_depth_ > 0)
                return reject(position, "return cannot exit parallel_for.");
            NodeId value = no_node;
            if (function_.result == ValueType::void_value) {
                if (token_.text != ";")
                    return reject(token_.position, "This void entry cannot return a value.");
            } else {
                if (token_.text == ";")
                    return reject(token_.position, "Non-void entry requires a return value.");
                ATELIER_TRY_BIND(parsed, expression());
                ATELIER_TRY_BIND(converted_value, converted(*parsed, function_.result));
                value = *converted_value;
            }
            ATELIER_TRY(expect(";"));
            return add(Statement{Statement::Kind::return_value, position, no_node, value, no_node, no_node, {}});
        }
        return simple_statement(";");
    }

    std::expected<ValueType, Diagnostic> check_binary_types(Operation op, bool comparison, SourcePosition position,
                                                            ValueType type, ValueType right_type) const {
        auto result_type = comparison ? ValueType{ValueType::boolean} : type;
        const bool shift = op == Operation::shift_left || op == Operation::shift_right;
        if (op == Operation::logical_and || op == Operation::logical_or) {
            if (type != ValueType::boolean || right_type != ValueType::boolean)
                return reject(position, "Logical operators require bool operands.");
            result_type = ValueType::boolean;
        } else if (shift) {
            if ((type != ValueType::i32 && type != ValueType::u32) ||
                (right_type != ValueType::i32 && right_type != ValueType::u32))
                return reject(position, "Shift operands require int or uint.");
        } else if (reference_type(type) || reference_type(right_type)) {
            if (!reference_type(type) || !reference_type(right_type) ||
                (op != Operation::equal && op != Operation::not_equal))
                return reject(position, "References support only == and != with another reference or null.");
        } else if (type.kind == ValueType::string || right_type.kind == ValueType::string) {
            if (type != right_type || !comparison)
                return reject(
                    position,
                    "Strings support ==, !=, <, <=, >, >= comparisons; use += or .append(...) for concatenation.");
            result_type = ValueType::boolean;
        } else if (type.kind == ValueType::array || right_type.kind == ValueType::array) {
            if (type != right_type || (op != Operation::equal && op != Operation::not_equal))
                return reject(position, "Arrays support == and != with a matching Array type.");
            if (!element_equatable(environment_.values[type.type_index].element))
                return reject(position, "Array equality requires scalar, enum, built-in math or String elements.");
            result_type = ValueType::boolean;
        } else if (math_type(type) || math_type(right_type)) {
            const auto left_math = math_type(type);
            const auto right_math = math_type(right_type);
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
                return reject(position, "Unsupported math operation or mismatched component precision.");
        } else if (flags_enum(type) && right_type == type &&
                   (op == Operation::bitwise_and || op == Operation::bitwise_or || op == Operation::bitwise_xor)) {
            result_type = type;
        } else if (!scalar(type) || right_type != type ||
                   ((type.kind == ValueType::enumeration || type == ValueType::boolean) && op != Operation::equal &&
                    op != Operation::not_equal)) {
            return reject(
                position,
                "Binary operands require matching numeric types; bool and non-Flags enums support equality only.");
        } else if ((op == Operation::remainder || op == Operation::bitwise_and || op == Operation::bitwise_or ||
                    op == Operation::bitwise_xor) &&
                   type != ValueType::i32 && type != ValueType::u32)
            return reject(position, "Remainder and bitwise operators require int or uint.");
        return result_type;
    }

    std::expected<NodeId, Diagnostic> expression(int minimum = 0) {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        ATELIER_TRY_BIND(parsed_left, unary());
        auto left = *parsed_left;
        for (;;) {
            const auto operation = binary_operator(token_.text);
            if (operation.precedence < minimum)
                break;
            ATELIER_TRY_BIND(symbol, consume());
            ATELIER_TRY_BIND(parsed_right, expression(operation.precedence + 1));
            const auto right = *parsed_right;
            const bool shift =
                operation.operation == Operation::shift_left || operation.operation == Operation::shift_right;
            if (!shift && function_.expressions[left].type == ValueType::i32 && integer_constant(right))
                signed_constant(right);
            if (!shift && function_.expressions[right].type == ValueType::i32 && integer_constant(left))
                signed_constant(left);
            ATELIER_TRY_BIND(result_type,
                             check_binary_types(operation.operation, operation.comparison, symbol->position,
                                                function_.expressions[left].type, function_.expressions[right].type));
            ATELIER_TRY_BIND(combined, add(Expression{Expression::Kind::binary, *result_type, symbol->position, left,
                                                      right, 0, operation.operation}));
            left = *combined;
        }
        if (minimum == 0) {
            const auto qmark_pos = token_.position;
            ATELIER_TRY_BIND(ternary, accept("?"));
            if (*ternary) {
                if (function_.expressions[left].type != ValueType::boolean)
                    return reject(qmark_pos, "Conditional expression requires a bool condition.");
                ATELIER_TRY_BIND(parsed_then, expression(0));
                const auto then_id = *parsed_then;
                ATELIER_TRY(expect(":"));
                ATELIER_TRY_BIND(parsed_else, expression(0));
                const auto else_id = *parsed_else;
                if (function_.expressions[then_id].type == ValueType::i32 && integer_constant(else_id))
                    signed_constant(else_id);
                if (function_.expressions[else_id].type == ValueType::i32 && integer_constant(then_id))
                    signed_constant(then_id);
                const auto then_type = function_.expressions[then_id].type;
                const auto else_type = function_.expressions[else_id].type;
                if (then_type != else_type)
                    return reject(qmark_pos, "Conditional branches require matching value types.");
                if (!scalar(then_type) && !math_type(then_type) &&
                    !(plain_record(then_type) && environment_.values[then_type.type_index].can_own))
                    return reject(
                        qmark_pos,
                        "Conditional branches require matching scalar, enum, built-in math or plain struct value types.");
                if (plain_record(then_type) && !math_type(then_type))
                    function_.uses_scoped_values = true;
                return add(Expression{Expression::Kind::conditional, then_type, qmark_pos, then_id, else_id, 0,
                                      Operation::add, left});
            }
        }
        return left;
    }

    std::expected<NodeId, Diagnostic> unary() {
        Depth guard(depth_);
        if (depth_ > 128)
            return reject(token_.position, "Source nesting exceeds the compiler limit of 128.");
        if (token_.text == "!" || token_.text == "~") {
            ATELIER_TRY_BIND(symbol, consume());
            ATELIER_TRY_BIND(parsed_operand, unary());
            const auto operand = *parsed_operand;
            const auto type = function_.expressions[operand].type;
            if (symbol->text == "!" && type != ValueType::boolean)
                return reject(symbol->position, "Logical negation requires bool.");
            if (symbol->text == "~" && type != ValueType::i32 && type != ValueType::u32 && !flags_enum(type))
                return reject(symbol->position, "Bitwise negation requires int, uint or a Flags enum.");
            return add(Expression{Expression::Kind::unary, type, symbol->position, operand, no_node, 0,
                                  symbol->text == "!" ? Operation::logical_not : Operation::bitwise_not});
        }
        if (token_.text == "+" || token_.text == "-") {
            ATELIER_TRY_BIND(symbol, consume());
            ATELIER_TRY_BIND(parsed_operand, unary());
            const auto operand = *parsed_operand;
            if (symbol->text == "-" && function_.expressions[operand].kind == Expression::Kind::literal &&
                !function_.expressions[operand].typed_constant &&
                function_.expressions[operand].type == ValueType::u32 &&
                function_.expressions[operand].bits == 2147483648u)
                return literal(ValueType::i32, symbol->position, 2147483648u);
            if (symbol->text == "-" && integer_constant(operand))
                signed_constant(operand);
            const auto type = function_.expressions[operand].type;
            if (!floating(type) && type != ValueType::u32 && type != ValueType::i32 && !math_type(type))
                return reject(symbol->position, "Unary arithmetic requires a numeric operand.");
            if (symbol->text == "-" && type == ValueType::u32)
                return reject(
                    symbol->position,
                    "Unary minus requires a signed numeric value; use uint subtraction for modulo arithmetic.");
            return add(Expression{Expression::Kind::unary, type, symbol->position, operand, no_node, 0,
                                  symbol->text == "+" ? Operation::positive : Operation::negate});
        }
        ATELIER_TRY_BIND(receiver, primary());
        return postfix(*receiver);
    }

    std::expected<NodeId, Diagnostic> sequence_method(NodeId receiver, ValueType type, const Token& member,
                                                      SourcePosition position) {
        std::vector<NodeId> args;
        ATELIER_TRY_BIND(empty_args, accept(")"));
        if (!*empty_args) {
            for (;;) {
                if (args.size() >= 4)
                    return reject(token_.position, "Sequence method argument count mismatch.");
                ATELIER_TRY_BIND(parsed_arg, expression());
                args.push_back(*parsed_arg);
                ATELIER_TRY_BIND(more, accept(","));
                if (!*more)
                    break;
            }
            ATELIER_TRY(expect(")"));
        }
        const auto expect_arity = [&](std::size_t min_n, std::size_t max_n) -> std::expected<void, Diagnostic> {
            if (args.size() < min_n || args.size() > max_n)
                return reject(member.position, "Sequence method argument count mismatch.");
            return {};
        };
        const auto check_index_or_size = [&](NodeId arg, const char* what) -> std::expected<void, Diagnostic> {
            const auto t = function_.expressions[arg].type;
            if (t != ValueType::u32 && t != ValueType::i32)
                return reject(function_.expressions[arg].position, std::string(what) + " requires uint or int.");
            return {};
        };
        const auto check_mutable_sequence = [&](bool allow_fixed_length) -> std::expected<void, Diagnostic> {
            if (!allow_fixed_length) {
                if (const auto& rnode = function_.expressions[receiver];
                    rnode.kind == Expression::Kind::local && function_.locals[rnode.value].fixed_length)
                    return reject(
                        member.position,
                        "SOP [InOut] and [Out] attribute columns have fixed element count and cannot change structure.");
            }
            if (!mutable_local_storage(receiver) || !environment_.values[type.type_index].can_own ||
                !environment_.values[type.type_index].can_mutate)
                return reject(member.position, "Structural mutation requires a mutable owned or borrowed sequence.");
            return {};
        };
        const auto make_seq_op = [&](SequenceOperation op, ValueType result_type) -> std::expected<NodeId, Diagnostic> {
            Expression node{Expression::Kind::sequence_operation, result_type, position, receiver};
            node.sequence_operation = op;
            node.arguments = std::move(args);
            return add(std::move(node));
        };
        const bool is_string = type.kind == ValueType::string;
        const ValueType element = is_string ? ValueType{ValueType::u32} : environment_.values[type.type_index].element;
        if (member.text == "size") {
            ATELIER_TRY(expect_arity(0, 0));
            return add(Expression{Expression::Kind::length, ValueType::u32, position, receiver});
        }
        if (member.text == "empty") {
            ATELIER_TRY(expect_arity(0, 0));
            return make_seq_op(is_string ? SequenceOperation::str_empty : SequenceOperation::arr_empty,
                               ValueType::boolean);
        }
        if (member.text == "front") {
            ATELIER_TRY(expect_arity(0, 0));
            ATELIER_TRY_BIND(zero, literal(ValueType::u32, member.position, 0));
            return add(Expression{Expression::Kind::index, element, member.position, receiver, *zero});
        }
        if (member.text == "back") {
            ATELIER_TRY(expect_arity(0, 0));
            return add(Expression{Expression::Kind::index, element, member.position, receiver, no_node});
        }
        if (is_string) {
            if (member.text == "is_ascii") {
                ATELIER_TRY(expect_arity(0, 0));
                return make_seq_op(SequenceOperation::str_is_ascii, ValueType::boolean);
            }
            if (member.text == "starts_with" || member.text == "ends_with" || member.text == "contains" ||
                member.text == "equals_ignore_case" || member.text == "starts_with_ignore_case" ||
                member.text == "ends_with_ignore_case") {
                ATELIER_TRY(expect_arity(1, 1));
                ATELIER_TRY_BIND(sub, converted(args[0], type));
                args[0] = *sub;
                const auto op = member.text == "starts_with"        ? SequenceOperation::str_starts_with
                                : member.text == "ends_with"        ? SequenceOperation::str_ends_with
                                : member.text == "contains"         ? SequenceOperation::str_contains
                                : member.text == "equals_ignore_case" ? SequenceOperation::str_equals_ignore_case
                                : member.text == "starts_with_ignore_case"
                                    ? SequenceOperation::str_starts_with_ignore_case
                                    : SequenceOperation::str_ends_with_ignore_case;
                return make_seq_op(op, ValueType::boolean);
            }
            if (member.text == "find" || member.text == "rfind") {
                ATELIER_TRY(expect_arity(1, 2));
                ATELIER_TRY_BIND(sub, converted(args[0], type));
                args[0] = *sub;
                if (args.size() == 2)
                    ATELIER_TRY(check_index_or_size(args[1], "String search start offset"));
                return make_seq_op(member.text == "find" ? SequenceOperation::str_find : SequenceOperation::str_rfind,
                                   ValueType::i32);
            }
            if (member.text == "count" || member.text == "common_prefix_length") {
                ATELIER_TRY(expect_arity(1, 1));
                ATELIER_TRY_BIND(sub, converted(args[0], type));
                args[0] = *sub;
                return make_seq_op(member.text == "count" ? SequenceOperation::str_count
                                                          : SequenceOperation::str_common_prefix_length,
                                   ValueType::u32);
            }
            if (member.text == "try_parse_int" || member.text == "try_parse_uint" || member.text == "try_parse_float" ||
                member.text == "try_parse_double") {
                ATELIER_TRY(expect_arity(1, 1));
                const ValueType expected_out = member.text == "try_parse_int"     ? ValueType{ValueType::i32}
                                               : member.text == "try_parse_uint"  ? ValueType{ValueType::u32}
                                               : member.text == "try_parse_float" ? ValueType{ValueType::f32}
                                                                                  : ValueType{ValueType::f64};
                const auto& out_expr = function_.expressions[args[0]];
                if (out_expr.type != expected_out || !mutable_local_storage(args[0]))
                    return reject(out_expr.position,
                                  "String." + std::string(member.text) +
                                      " requires a writable local, reference parameter, struct field or Array element of matching numeric type.");
                const auto op = member.text == "try_parse_int"     ? SequenceOperation::str_try_parse_int
                                : member.text == "try_parse_uint"  ? SequenceOperation::str_try_parse_uint
                                : member.text == "try_parse_float" ? SequenceOperation::str_try_parse_float
                                                                   : SequenceOperation::str_try_parse_double;
                return make_seq_op(op, ValueType::boolean);
            }
            ATELIER_TRY(check_mutable_sequence(false));
            if (member.text == "clear") {
                ATELIER_TRY(expect_arity(0, 0));
                Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                    no_node};
                mutation.mutation = SequenceMutation::clear;
                return add(std::move(mutation));
            }
            if (member.text == "reserve") {
                ATELIER_TRY(expect_arity(1, 1));
                ATELIER_TRY(check_index_or_size(args[0], "Sequence capacity"));
                Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                    args[0]};
                mutation.mutation = SequenceMutation::reserve;
                return add(std::move(mutation));
            }
            if (member.text == "append") {
                ATELIER_TRY(expect_arity(1, 1));
                const auto arg_type = function_.expressions[args[0]].type;
                if (arg_type == type) {
                    Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                        args[0]};
                    mutation.mutation = SequenceMutation::append;
                    return add(std::move(mutation));
                }
                if (arg_type == ValueType::i32 || arg_type == ValueType::u32 || arg_type == ValueType::f32 ||
                    arg_type == ValueType::f64 || arg_type == ValueType::boolean)
                    return make_seq_op(SequenceOperation::str_append_scalar, ValueType::void_value);
                return reject(function_.expressions[args[0]].position,
                              "String.append requires String, int, uint, float, double or bool.");
            }
            if (member.text == "append_ascii") {
                ATELIER_TRY(expect_arity(1, 2));
                ATELIER_TRY(check_index_or_size(args[0], "ASCII byte value"));
                if (args.size() == 2)
                    ATELIER_TRY(check_index_or_size(args[1], "Repeat count"));
                return make_seq_op(SequenceOperation::str_append_ascii, ValueType::void_value);
            }
            if (member.text == "append_hex") {
                ATELIER_TRY(expect_arity(1, 3));
                ATELIER_TRY_BIND(val, converted(args[0], ValueType::u32));
                args[0] = *val;
                if (args.size() >= 2)
                    ATELIER_TRY(check_index_or_size(args[1], "Hex minimum digit count"));
                if (args.size() == 3) {
                    ATELIER_TRY_BIND(upper, converted(args[2], ValueType::boolean));
                    args[2] = *upper;
                }
                return make_seq_op(SequenceOperation::str_append_hex, ValueType::void_value);
            }
            if (member.text == "append_float") {
                ATELIER_TRY(expect_arity(2, 2));
                const auto val_type = function_.expressions[args[0]].type;
                if (!floating(val_type))
                    return reject(function_.expressions[args[0]].position,
                                  "String.append_float requires float or double.");
                ATELIER_TRY(check_index_or_size(args[1], "Fixed float precision"));
                return make_seq_op(SequenceOperation::str_append_float_fixed, ValueType::void_value);
            }
            if (member.text == "pad_left" || member.text == "pad_right") {
                ATELIER_TRY(expect_arity(1, 2));
                ATELIER_TRY(check_index_or_size(args[0], "Padding width"));
                if (args.size() == 2)
                    ATELIER_TRY(check_index_or_size(args[1], "Padding ASCII byte"));
                return make_seq_op(member.text == "pad_left" ? SequenceOperation::str_pad_left
                                                             : SequenceOperation::str_pad_right,
                                   ValueType::void_value);
            }
            if (member.text == "trim" || member.text == "trim_start" || member.text == "trim_end" ||
                member.text == "to_lower" || member.text == "to_upper") {
                ATELIER_TRY(expect_arity(0, 0));
                const auto op = member.text == "trim"         ? SequenceOperation::str_trim
                                : member.text == "trim_start" ? SequenceOperation::str_trim_start
                                : member.text == "trim_end"   ? SequenceOperation::str_trim_end
                                : member.text == "to_lower"   ? SequenceOperation::str_to_lower
                                                              : SequenceOperation::str_to_upper;
                return make_seq_op(op, ValueType::void_value);
            }
            if (member.text == "strip_prefix" || member.text == "strip_suffix") {
                ATELIER_TRY(expect_arity(1, 1));
                ATELIER_TRY_BIND(sub, converted(args[0], type));
                args[0] = *sub;
                return make_seq_op(member.text == "strip_prefix" ? SequenceOperation::str_strip_prefix
                                                                 : SequenceOperation::str_strip_suffix,
                                   ValueType::boolean);
            }
            if (member.text == "slice_in_place" || member.text == "erase") {
                ATELIER_TRY(expect_arity(1, 2));
                ATELIER_TRY(check_index_or_size(args[0], "String byte offset"));
                if (args.size() == 2)
                    ATELIER_TRY(check_index_or_size(args[1], "String byte count"));
                return make_seq_op(member.text == "slice_in_place" ? SequenceOperation::str_slice_in_place
                                                                   : SequenceOperation::str_erase,
                                   ValueType::void_value);
            }
            if (member.text == "insert") {
                ATELIER_TRY(expect_arity(2, 2));
                ATELIER_TRY(check_index_or_size(args[0], "String byte offset"));
                ATELIER_TRY_BIND(rep, converted(args[1], type));
                ATELIER_TRY_BIND(zero, literal(ValueType::u32, member.position, 0));
                args = {args[0], *zero, *rep};
                return make_seq_op(SequenceOperation::str_replace, ValueType::void_value);
            }
            if (member.text == "replace") {
                ATELIER_TRY(expect_arity(3, 3));
                ATELIER_TRY(check_index_or_size(args[0], "String byte offset"));
                ATELIER_TRY(check_index_or_size(args[1], "String byte count"));
                ATELIER_TRY_BIND(rep, converted(args[2], type));
                args[2] = *rep;
                return make_seq_op(SequenceOperation::str_replace, ValueType::void_value);
            }
            if (member.text == "replace_all") {
                ATELIER_TRY(expect_arity(2, 2));
                ATELIER_TRY_BIND(search, converted(args[0], type));
                ATELIER_TRY_BIND(rep, converted(args[1], type));
                args[0] = *search;
                args[1] = *rep;
                return make_seq_op(SequenceOperation::str_replace_all, ValueType::u32);
            }
            return reject(member.position, "Unknown native String operation.");
        }
        if (member.text == "contains" || member.text == "count") {
            ATELIER_TRY(expect_arity(1, 1));
            if (!element_equatable(element))
                return reject(member.position,
                              "Array." + std::string(member.text) +
                                  " requires scalar, enum, built-in math or String elements.");
            ATELIER_TRY_BIND(val, converted(args[0], element));
            args[0] = *val;
            return make_seq_op(member.text == "contains" ? SequenceOperation::arr_contains
                                                         : SequenceOperation::arr_count,
                               member.text == "contains" ? ValueType::boolean : ValueType::u32);
        }
        if (member.text == "find" || member.text == "rfind") {
            ATELIER_TRY(expect_arity(1, 2));
            if (!element_equatable(element))
                return reject(member.position,
                              "Array." + std::string(member.text) +
                                  " requires scalar, enum, built-in math or String elements.");
            ATELIER_TRY_BIND(val, converted(args[0], element));
            args[0] = *val;
            if (args.size() == 2)
                ATELIER_TRY(check_index_or_size(args[1], "Array search start index"));
            return make_seq_op(member.text == "find" ? SequenceOperation::arr_find : SequenceOperation::arr_rfind,
                               ValueType::i32);
        }
        if (member.text == "is_sorted") {
            ATELIER_TRY(expect_arity(0, 0));
            if (!element_ordered(element))
                return reject(member.position,
                              "Array.is_sorted requires int, uint, float, double, enum or String elements.");
            return make_seq_op(SequenceOperation::arr_is_sorted, ValueType::boolean);
        }
        if (member.text == "fill" || member.text == "reverse" || member.text == "swap" || member.text == "sort") {
            ATELIER_TRY(check_mutable_sequence(true));
            if (member.text == "fill") {
                ATELIER_TRY(expect_arity(1, 1));
                ATELIER_TRY_BIND(val, converted(args[0], element));
                args[0] = *val;
                return make_seq_op(SequenceOperation::arr_fill, ValueType::void_value);
            }
            if (member.text == "reverse") {
                ATELIER_TRY(expect_arity(0, 0));
                return make_seq_op(SequenceOperation::arr_reverse, ValueType::void_value);
            }
            if (member.text == "swap") {
                ATELIER_TRY(expect_arity(2, 2));
                ATELIER_TRY(check_index_or_size(args[0], "Array index"));
                ATELIER_TRY(check_index_or_size(args[1], "Array index"));
                return make_seq_op(SequenceOperation::arr_swap, ValueType::void_value);
            }
            ATELIER_TRY(expect_arity(0, 0));
            if (!element_ordered(element))
                return reject(member.position,
                              "Array.sort requires int, uint, float, double, enum or String elements.");
            return make_seq_op(SequenceOperation::arr_sort, ValueType::void_value);
        }
        ATELIER_TRY(check_mutable_sequence(false));
        if (member.text == "clear") {
            ATELIER_TRY(expect_arity(0, 0));
            Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                no_node};
            mutation.mutation = SequenceMutation::clear;
            return add(std::move(mutation));
        }
        if (member.text == "reserve") {
            ATELIER_TRY(expect_arity(1, 1));
            ATELIER_TRY(check_index_or_size(args[0], "Sequence capacity"));
            Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                args[0]};
            mutation.mutation = SequenceMutation::reserve;
            return add(std::move(mutation));
        }
        if (member.text == "resize") {
            ATELIER_TRY(expect_arity(1, 2));
            ATELIER_TRY(check_index_or_size(args[0], "Sequence size"));
            if (args.size() == 1) {
                Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver,
                                    args[0]};
                mutation.mutation = SequenceMutation::resize;
                return add(std::move(mutation));
            }
            ATELIER_TRY_BIND(fill_val, converted(args[1], element));
            args[1] = *fill_val;
            return make_seq_op(SequenceOperation::arr_resize_fill, ValueType::void_value);
        }
        if (member.text == "truncate") {
            ATELIER_TRY(expect_arity(1, 1));
            ATELIER_TRY(check_index_or_size(args[0], "Array truncate size"));
            return make_seq_op(SequenceOperation::arr_truncate, ValueType::void_value);
        }
        if (member.text == "pop") {
            ATELIER_TRY(expect_arity(0, 0));
            return make_seq_op(SequenceOperation::arr_pop, ValueType::void_value);
        }
        if (member.text == "add") {
            ATELIER_TRY(expect_arity(1, 1));
            ATELIER_TRY_BIND(val, converted(args[0], element));
            Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver, *val};
            mutation.mutation = SequenceMutation::add;
            return add(std::move(mutation));
        }
        if (member.text == "append") {
            ATELIER_TRY(expect_arity(1, 1));
            ATELIER_TRY_BIND(other, converted(args[0], type));
            Expression mutation{Expression::Kind::sequence_mutation, ValueType::void_value, position, receiver, *other};
            mutation.mutation = SequenceMutation::append;
            return add(std::move(mutation));
        }
        if (member.text == "insert") {
            ATELIER_TRY(expect_arity(2, 2));
            ATELIER_TRY(check_index_or_size(args[0], "Array insert index"));
            ATELIER_TRY_BIND(val, converted(args[1], element));
            args[1] = *val;
            return make_seq_op(SequenceOperation::arr_insert, ValueType::void_value);
        }
        if (member.text == "remove_at") {
            ATELIER_TRY(expect_arity(1, 2));
            ATELIER_TRY(check_index_or_size(args[0], "Array remove index"));
            if (args.size() == 2)
                ATELIER_TRY(check_index_or_size(args[1], "Array remove count"));
            return make_seq_op(SequenceOperation::arr_remove_at, ValueType::void_value);
        }
        if (member.text == "remove_at_swap") {
            ATELIER_TRY(expect_arity(1, 1));
            ATELIER_TRY(check_index_or_size(args[0], "Array remove index"));
            return make_seq_op(SequenceOperation::arr_remove_at_swap, ValueType::void_value);
        }
        if (member.text == "remove" || member.text == "remove_swap") {
            ATELIER_TRY(expect_arity(1, 1));
            if (!element_equatable(element))
                return reject(member.position,
                              "Array." + std::string(member.text) +
                                  " requires scalar, enum, built-in math or String elements.");
            ATELIER_TRY_BIND(val, converted(args[0], element));
            args[0] = *val;
            return make_seq_op(member.text == "remove" ? SequenceOperation::arr_remove
                                                       : SequenceOperation::arr_remove_swap,
                               ValueType::boolean);
        }
        return reject(member.position, "Unknown native Array operation.");
    }

    std::expected<NodeId, Diagnostic> postfix(NodeId receiver) {
        while (token_.text == "." || token_.text == "[") {
            const auto position = function_.expressions[receiver].position;
            const auto type = function_.expressions[receiver].type;
            ATELIER_TRY_BIND(indexing, accept("["));
            if (*indexing) {
                if (type.kind != ValueType::array && type.kind != ValueType::string)
                    return reject(position, "Indexing requires Array or String.");
                ATELIER_TRY_BIND(parsed_index, expression());
                const auto index = *parsed_index;
                ATELIER_TRY(expect("]"));
                if (function_.expressions[index].type != ValueType::u32 &&
                    function_.expressions[index].type != ValueType::i32)
                    return reject(position, "A sequence index requires uint or int.");
                const auto element = type.kind == ValueType::string ? ValueType{ValueType::u32}
                                                                    : environment_.values[type.type_index].element;
                ATELIER_TRY_BIND(indexed, add(Expression{Expression::Kind::index, element, position, receiver, index}));
                receiver = *indexed;
                continue;
            }
            ATELIER_TRY(expect("."));
            if (aggregate(type)) {
                ATELIER_TRY_BIND(member, identifier());
                if (type.kind == ValueType::array || type.kind == ValueType::string) {
                    ATELIER_TRY(expect("("));
                    ATELIER_TRY_BIND(updated, sequence_method(receiver, type, *member, position));
                    receiver = *updated;
                } else {
                    if (environment_.material && type == environment_.texture_input)
                        return reject(member->position,
                                      "TextureInput members are compiler-owned, not accessible to material Source.");
                    const auto& fields = environment_.values[type.type_index].fields;
                    const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& candidate) {
                        return candidate.name == member->text;
                    });
                    if (field == fields.end())
                        return reject(member->position, "Field has no published direct native layout.");
                    const auto& owner = function_.expressions[receiver];
                    if (const auto mtype = math_type(type);
                        mtype && field->type == math_scalar(mtype) && owner.kind == Expression::Kind::math_construct &&
                        (owner.arguments.empty() || owner.arguments.size() == mtype.components() ||
                         (owner.arguments.size() == 1 && mtype.vector())) &&
                        std::ranges::all_of(owner.arguments, [&](NodeId arg) {
                            return function_.expressions[arg].kind == Expression::Kind::literal;
                        })) {
                        const std::size_t lane = field->offset / mtype.scalar_size();
                        std::uint64_t lane_bits = 0;
                        if (owner.arguments.size() == mtype.components()) {
                            lane_bits = function_.expressions[owner.arguments[lane]].bits;
                        } else if (owner.arguments.size() == 1) {
                            lane_bits = function_.expressions[owner.arguments[0]].bits;
                        } else if (mtype.shape == math::Shape::quaternion && lane == 3) {
                            lane_bits = mtype.precision == math::Precision::f64 ? std::bit_cast<std::uint64_t>(1.0)
                                                                                : std::bit_cast<std::uint32_t>(1.0f);
                        }
                        ATELIER_TRY_BIND(extracted, literal(field->type, position, lane_bits, true));
                        receiver = *extracted;
                        continue;
                    }
                    if (owner.kind == Expression::Kind::field) {
                        const auto path = std::string(environment_.fields[owner.value].name) + "." + field->name;
                        const auto projection = std::ranges::find_if(environment_.fields, [&](const auto& candidate) {
                            return candidate.name == path && candidate.type == field->type;
                        });
                        if (projection != environment_.fields.end()) {
                            auto projected =
                                add(Expression{Expression::Kind::field, projection->type, position, no_node, no_node,
                                               static_cast<std::uint32_t>(projection - environment_.fields.begin()),
                                               Operation::add, owner.receiver});
                            if (!projected)
                                return std::unexpected(std::move(projected).error());
                            receiver = *projected;
                            continue;
                        }
                    }
                    Expression access{Expression::Kind::value_field, field->type, position, receiver};
                    access.offset = field->offset;
                    ATELIER_TRY_BIND(accessed, add(std::move(access)));
                    receiver = *accessed;
                }
                continue;
            }
            if (function_.expressions[receiver].type != ValueType::strong_reference &&
                function_.expressions[receiver].type != ValueType::weak_reference)
                return reject(position, "Member access requires a typed object reference.");
            ATELIER_TRY_BIND(member, expression_name(true));
            ATELIER_TRY_BIND(calling, accept("("));
            if (*calling) {
                const MethodBinding* method{};
                for (const auto& candidate : environment_.methods)
                    if (candidate.name == *member)
                        method = &candidate;
                if (!method)
                    return reject(position, "Unknown native method '" + *member + "'.");
                ATELIER_TRY_BIND(arguments, call_arguments(method->parameters));
                ATELIER_TRY_BIND(node,
                                 call_expression(true, static_cast<std::uint32_t>(method - environment_.methods.data()),
                                                 position, receiver, std::move(*arguments)));
                ATELIER_TRY_BIND(call, add(std::move(*node)));
                receiver = *call;
            } else if (const auto* property = find_property(*member, true)) {
                ATELIER_TRY_BIND(access, property_access(*property, position, receiver));
                receiver = *access;
            } else {
                const FieldBinding* field{};
                for (const auto& candidate : environment_.fields)
                    if (candidate.name == *member)
                        field = &candidate;
                if (!field)
                    return reject(position, "Unknown receiver field '" + *member + "'.");
                if (field->type.kind == ValueType::string) {
                    if (function_.expressions[receiver].kind != Expression::Kind::receiver)
                        return reject(position, "String fields may only be borrowed from self.");
                    function_.uses_scoped_values = true;
                }
                auto access = add(Expression{Expression::Kind::field, field->type, position, no_node, no_node,
                                             static_cast<std::uint32_t>(field - environment_.fields.data()),
                                             Operation::add, receiver});
                if (!access)
                    return std::unexpected(std::move(access).error());
                receiver = *access;
            }
        }
        return receiver;
    }

    std::expected<NodeId, Diagnostic> numeric_literal(const Token& token) {
        auto text = token.text;
        const bool is_hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        if (!is_hex) {
            const auto suffix = text.back();
            const bool is_double = suffix == 'd' || suffix == 'D';
            const bool is_float = suffix == 'f' || suffix == 'F';
            if (is_float || is_double)
                text.remove_suffix(1);
            if (is_double) {
                double value{};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                    return reject(token.position, "Double literal is out of range or invalid.");
                return literal(ValueType::f64, token.position, std::bit_cast<std::uint64_t>(value));
            }
            if (is_float || text.find_first_of(".eE") != std::string_view::npos) {
                float value{};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
                    return reject(token.position, "Float literal is out of range or invalid.");
                return literal(ValueType::f32, token.position, std::bit_cast<std::uint32_t>(value));
            }
        }
        ParsedUint32Literal parsed{};
        if (!parse_uint32_literal(token.text, parsed))
            return reject(token.position, "Uint literal is outside 0..4294967295.");
        return literal(ValueType::u32, token.position, parsed.value, parsed.explicit_unsigned);
    }

    std::expected<NodeId, Diagnostic> math_call(std::string_view name, SourcePosition position, ValueType constructed) {
        std::vector<NodeId> arguments;
        ATELIER_TRY_BIND(empty, accept(")"));
        if (!*empty) {
            for (;;) {
                if (arguments.size() == 16)
                    return reject(token_.position, "A math call accepts at most 16 scalar components or operands.");
                ATELIER_TRY_BIND(argument, expression());
                arguments.push_back(*argument);
                ATELIER_TRY_BIND(more, accept(","));
                if (!*more)
                    break;
            }
            ATELIER_TRY(expect(")"));
        }
        if (constructed != ValueType::void_value) {
            const auto descriptor = math_type(constructed);
            if (arguments.size() == 1) {
                const auto arg_math = math_type(function_.expressions[arguments[0]].type);
                if (arg_math && arg_math.shape == descriptor.shape) {
                    Expression result{Expression::Kind::math_construct, constructed, position};
                    result.arguments = std::move(arguments);
                    return add(std::move(result));
                }
                if (descriptor.vector() || descriptor.shape == math::Shape::matrix4) {
                    ATELIER_TRY_BIND(value, converted(arguments[0], math_scalar(descriptor)));
                    arguments[0] = *value;
                    Expression result{Expression::Kind::math_construct, constructed, position};
                    result.arguments = std::move(arguments);
                    return add(std::move(result));
                }
            }
            if (!arguments.empty() && arguments.size() != descriptor.components())
                return reject(
                    position,
                    "Math construction requires no arguments, a same-shape math value, one broadcast/diagonal scalar for a vector or matrix, or one scalar per component.");
            for (auto& argument : arguments) {
                ATELIER_TRY_BIND(value, converted(argument, math_scalar(descriptor)));
                argument = *value;
            }
            Expression result{Expression::Kind::math_construct, constructed, position};
            result.arguments = std::move(arguments);
            return add(std::move(result));
        }
        struct MathIntrinsicEntry {
            std::string_view name;

            MathOperation operation;

            std::uint8_t arity;
        };
        static constexpr std::array<MathIntrinsicEntry, 43> intrinsics{{
            {"math.dot", MathOperation::dot, 2},
            {"math.cross", MathOperation::cross, 2},
            {"math.length", MathOperation::length, 1},
            {"math.length_squared", MathOperation::length_squared, 1},
            {"math.distance", MathOperation::distance, 2},
            {"math.distance_squared", MathOperation::distance_squared, 2},
            {"math.normalize_or_zero", MathOperation::normalize_or_zero, 1},
            {"math.fast_normalize_or_zero", MathOperation::fast_normalize_or_zero, 1},
            {"math.conjugate", MathOperation::conjugate, 1},
            {"math.slerp01", MathOperation::slerp01, 3},
            {"math.fast_slerp01", MathOperation::fast_slerp01, 3},
            {"math.rotate", MathOperation::rotate, 2},
            {"math.transpose", MathOperation::transpose, 1},
            {"math.transform_point", MathOperation::transform_point, 2},
            {"math.transform_direction", MathOperation::transform_direction, 2},
            {"math.sqrt", MathOperation::square_root, 1},
            {"math.fast_rsqrt", MathOperation::fast_rsqrt, 1},
            {"math.floor", MathOperation::floor, 1},
            {"math.ceil", MathOperation::ceil, 1},
            {"math.trunc", MathOperation::trunc, 1},
            {"math.round", MathOperation::round, 1},
            {"math.fract", MathOperation::fract, 1},
            {"math.abs", MathOperation::abs, 1},
            {"math.sign", MathOperation::sign, 1},
            {"math.min", MathOperation::min, 2},
            {"math.max", MathOperation::max, 2},
            {"math.step", MathOperation::step, 2},
            {"math.clamp", MathOperation::clamp, 3},
            {"math.cwise_mul", MathOperation::cwise_mul, 2},
            {"math.cwise_div", MathOperation::cwise_div, 2},
            {"math.radians", MathOperation::radians, 1},
            {"math.degrees", MathOperation::degrees, 1},
            {"math.lerp", MathOperation::lerp, 3},
            {"math.smoothstep", MathOperation::smoothstep, 3},
            {"math.sin", MathOperation::sin, 1},
            {"math.cos", MathOperation::cos, 1},
            {"math.atan2", MathOperation::atan2, 2},
            {"math.exp", MathOperation::exp, 1},
            {"math.log", MathOperation::log, 1},
            {"math.pow", MathOperation::pow, 2},
            {"math.is_finite", MathOperation::is_finite, 1},
            {"math.as_uint", MathOperation::as_uint, 1},
            {"math.as_int", MathOperation::as_int, 1},
        }};
        const auto match = std::ranges::find(intrinsics, name, &MathIntrinsicEntry::name);
        if (match == intrinsics.end())
            return reject(position, "Unknown built-in math operation '" + std::string(name) + "'.");
        const auto operation = match->operation;
        const std::size_t arity = match->arity;
        if (arguments.size() != arity)
            return reject(position, "Math operation argument count mismatch.");
        const auto first = function_.expressions[arguments[0]].type;
        const auto descriptor = math_type(first);
        auto result_type = first;
        if (operation >= MathOperation::square_root) {
            const bool integer_eligible = (operation == MathOperation::min || operation == MathOperation::max ||
                                           operation == MathOperation::clamp || operation == MathOperation::abs) &&
                                          std::ranges::all_of(arguments, [&](NodeId arg) {
                                              const auto t = function_.expressions[arg].type;
                                              return t == ValueType::i32 || t == ValueType::u32;
                                          });
            if (integer_eligible) {
                if (operation == MathOperation::abs) {
                    if (integer_constant(arguments[0]))
                        signed_constant(arguments[0]);
                    if (function_.expressions[arguments[0]].type != ValueType::i32)
                        return reject(position, "math.abs requires float, double, float/double vectors, or int.");
                    result_type = ValueType::i32;
                } else {
                    const bool any_signed = std::ranges::any_of(
                        arguments, [&](NodeId arg) { return function_.expressions[arg].type == ValueType::i32; });
                    if (any_signed) {
                        for (const auto arg : arguments)
                            if (integer_constant(arg))
                                signed_constant(arg);
                    }
                    result_type = function_.expressions[arguments[0]].type;
                    for (const auto arg : arguments) {
                        if (function_.expressions[arg].type != result_type)
                            return reject(function_.expressions[arg].position,
                                          std::string(name) + " integer arguments require matching int or uint types.");
                    }
                }
            } else {
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
                    return reject(position, std::string(name) + " requires float or double scalars or vectors.");
                for (const auto argument : arguments) {
                    const auto type = function_.expressions[argument].type;
                    if (type != result_type && type != lane)
                        return reject(
                            function_.expressions[argument].position,
                            std::string(name) +
                                " arguments must have the result type or its component type; numeric types and precision do not convert implicitly.");
                }
            }
        } else
            switch (operation) {
            case MathOperation::dot:
            case MathOperation::cross:
            case MathOperation::distance:
            case MathOperation::distance_squared: {
                if (!descriptor.vector() ||
                    (operation == MathOperation::cross && descriptor.shape != math::Shape::vector3))
                    return reject(position, std::string(name) + " requires matching vector operands.");
                ATELIER_TRY_BIND(value, converted(arguments[1], first));
                arguments[1] = *value;
                if (operation != MathOperation::cross)
                    result_type = math_scalar(descriptor);
                break;
            }
            case MathOperation::length:
            case MathOperation::length_squared:
            case MathOperation::normalize_or_zero:
            case MathOperation::fast_normalize_or_zero:
                if (!descriptor.vector() && descriptor.shape != math::Shape::quaternion)
                    return reject(position, "Length and normalization require a vector or quaternion.");
                if (operation == MathOperation::length || operation == MathOperation::length_squared)
                    result_type = math_scalar(descriptor);
                break;
            case MathOperation::conjugate:
                if (descriptor.shape != math::Shape::quaternion)
                    return reject(position, "math.conjugate requires a quaternion.");
                break;
            case MathOperation::slerp01:
            case MathOperation::fast_slerp01: {
                if (descriptor.shape != math::Shape::quaternion)
                    return reject(position, std::string(name) +
                                                " requires two matching quaternions followed by a matching scalar.");
                ATELIER_TRY_BIND(second, converted(arguments[1], first));
                arguments[1] = *second;
                ATELIER_TRY_BIND(third, converted(arguments[2], math_scalar(descriptor)));
                arguments[2] = *third;
                break;
            }
            case MathOperation::transpose:
                if (descriptor.shape != math::Shape::matrix4)
                    return reject(position, "math.transpose requires a matrix4.");
                break;
            case MathOperation::rotate: {
                const auto second = function_.expressions[arguments[1]].type;
                const auto vector = math_type(second);
                if (descriptor.shape != math::Shape::quaternion || vector.shape != math::Shape::vector3 ||
                    descriptor.precision != vector.precision)
                    return reject(position,
                                  "math.rotate requires a quaternion followed by a vector3 of matching precision.");
                result_type = second;
                break;
            }
            case MathOperation::transform_point:
            case MathOperation::transform_direction: {
                const auto second = function_.expressions[arguments[1]].type;
                const auto matrix = math_type(second);
                if (descriptor.shape != math::Shape::vector3 || matrix.shape != math::Shape::matrix4 ||
                    descriptor.precision != matrix.precision)
                    return reject(
                        position,
                        "Transform operations require a vector3 followed by a matrix4 of matching precision.");
                break;
            }
            case MathOperation::is_finite:
                if (!floating(first) && !descriptor)
                    return reject(position,
                                  "math.is_finite requires a floating-point scalar, vector, quaternion or matrix.");
                result_type = ValueType::boolean;
                break;
            case MathOperation::as_uint:
            case MathOperation::as_int:
                if (first != ValueType::i32 && first != ValueType::u32 && first != ValueType::f32)
                    return reject(position, std::string(name) + " requires int, uint or float.");
                result_type = operation == MathOperation::as_uint ? ValueType::u32 : ValueType::i32;
                break;
            default:
                break;
            }
        Expression result{Expression::Kind::math_intrinsic, result_type, position};
        result.math_operation = operation;
        result.arguments = std::move(arguments);
        return add(std::move(result));
    }

    std::expected<NodeId, Diagnostic> primary() {
        // Keep non-grouping temporaries off the stack while parsing nested parentheses.
        ATELIER_TRY_BIND(parenthesized, accept("("));
        if (*parenthesized) {
            ATELIER_TRY_BIND(result, expression());
            ATELIER_TRY(expect(")"));
            return result;
        }
        return atom();
    }

    std::expected<NodeId, Diagnostic> material_sample(const Token& token) {
        const bool orientation = token.text == "sample_orientation";
        const auto label = "Material " + std::string(token.text);
        ATELIER_TRY(consume());
        ATELIER_TRY(expect("("));
        ATELIER_TRY_BIND(parsed_input, expression());
        const auto input = *parsed_input;
        if (function_.expressions[input].type != environment_.texture_input)
            return reject(token.position, label + " requires the registered TextureInput value, not a numeric slot.");
        ATELIER_TRY(expect(","));
        ATELIER_TRY_BIND(parsed_uv, expression());
        const auto uv = *parsed_uv;
        ATELIER_TRY(expect(")"));
        if (function_.expressions[uv].type != math_named_type("vector2"))
            return reject(token.position, label + " coordinates require vector2.");
        return add(Expression{orientation ? Expression::Kind::texture_orientation : Expression::Kind::texture_sample,
                              math_named_type(orientation ? "vector2" : "vector4"), token.position, uv, input});
    }

    std::expected<NodeId, Diagnostic> scalar_cast(const Token& token) {
        ATELIER_TRY(consume());
        ATELIER_TRY(expect("("));
        ATELIER_TRY_BIND(parsed_argument, expression());
        const auto argument = *parsed_argument;
        ATELIER_TRY(expect(")"));
        const auto type = function_.expressions[argument].type;
        if (!floating(type) && type != ValueType::i32 && type != ValueType::u32)
            return reject(token.position, "Numeric conversion requires float, double, int or uint.");
        ATELIER_TRY_BIND(target_type, named_type(token.text, token.position));
        return add(Expression{Expression::Kind::scalar_cast, *target_type, token.position, argument});
    }

    std::expected<NodeId, Diagnostic> reference_operation(const Token& token) {
        const auto observe = token.text == "observe";
        ATELIER_TRY(expect("("));
        if (token_.text == ")")
            return reject(token_.position, "Reference operation requires one argument.");
        ATELIER_TRY_BIND(parsed_argument, expression());
        const auto argument = *parsed_argument;
        const auto required = observe ? ValueType::strong_reference : ValueType::weak_reference;
        if (function_.expressions[argument].type != required)
            return reject(function_.expressions[argument].position,
                          observe ? "observe requires a strong reference argument."
                                  : "try_acquire requires a weak reference argument.");
        if (token_.text == ",")
            return reject(token_.position, "Reference operation requires exactly one argument.");
        ATELIER_TRY(expect(")"));
        return add(Expression{observe ? Expression::Kind::observe_reference : Expression::Kind::acquire_reference,
                              observe ? ValueType::weak_reference : ValueType::strong_reference, token.position,
                              argument});
    }

    std::expected<NodeId, Diagnostic> atom() {
        const auto token = token_;
        if (token.kind == Token::Kind::string) {
            ATELIER_TRY(consume());
            ATELIER_TRY_BIND(value, string_value(token));
            for (std::uint32_t i = 0; i < environment_.values.size(); ++i) {
                if (environment_.values[i].kind != ValueType::string || !environment_.values[i].can_own ||
                    !environment_.values[i].can_mutate)
                    continue;
                function_.uses_scoped_values = true;
                const auto index = static_cast<std::uint32_t>(function_.strings.size());
                function_.strings.push_back(std::move(*value));
                return add(Expression{
                    Expression::Kind::string_literal, {ValueType::string, i}, token.position, no_node, no_node, index});
            }
            return reject(token.position, "String literals require the registered foundation String.");
        }
        if (token.kind == Token::Kind::number) {
            ATELIER_TRY(consume());
            return numeric_literal(token);
        }
        if (token.kind != Token::Kind::identifier)
            return reject(token.position, "Expected an expression.");
        if (environment_.material && (token.text == "sample" || token.text == "sample_orientation"))
            return material_sample(token);
        if (token.text == "float" || token.text == "double" || token.text == "int" || token.text == "uint")
            return scalar_cast(token);
        {
            auto lookahead = lexer_;
            ATELIER_TRY_BIND(next, lookahead.next());
            std::string name(token.text);
            while (next->text == ".") {
                ATELIER_TRY_BIND(segment, lookahead.next());
                name += ".";
                name += segment->text;
                next = lookahead.next();
                ATELIER_TRY(next);
            }
            if (next->text == "(" && (name.starts_with("math.") || math_named_type(name) != ValueType::void_value)) {
                const auto constructed = math_named_type(name);
                ATELIER_TRY_BIND(actual_name, qualified_identifier());
                ATELIER_TRY(expect("("));
                return math_call(*actual_name, token.position, constructed);
            }
            if (next->text != "(" && next->text != "::" && name.starts_with("math.")) {
                ATELIER_TRY_BIND(actual_name, qualified_identifier());
                if (*actual_name == "math.PI")
                    return literal(ValueType::f32, token.position,
                                   std::bit_cast<std::uint32_t>(std::numbers::pi_v<float>), true);
                if (*actual_name == "math.TAU")
                    return literal(ValueType::f32, token.position,
                                   std::bit_cast<std::uint32_t>(2.0f * std::numbers::pi_v<float>), true);
                if (*actual_name == "math.PI_D")
                    return literal(ValueType::f64, token.position,
                                   std::bit_cast<std::uint64_t>(std::numbers::pi_v<double>), true);
                if (*actual_name == "math.TAU_D")
                    return literal(ValueType::f64, token.position,
                                   std::bit_cast<std::uint64_t>(2.0 * std::numbers::pi_v<double>), true);
                return reject(token.position, "Unknown built-in math constant '" + *actual_name + "'.");
            }
            if (next->text == "::") {
                ATELIER_TRY_BIND(name_actual, qualified_identifier());
                ATELIER_TRY(expect("::"));
                ATELIER_TRY_BIND(item, identifier());
                for (std::uint32_t i = 0; i < environment_.enumerations.size(); ++i) {
                    const auto& enumeration = environment_.enumerations[i];
                    if (enumeration.name != *name_actual)
                        continue;
                    for (const auto& value : enumeration.values)
                        if (value.name == item->text)
                            return literal({ValueType::enumeration, i}, token.position, value.bits);
                    return reject(item->position, "Unknown enumerator.");
                }
                return reject(token.position, "Unknown enum type.");
            }
            // Any other dotted name not led by a local or self is a qualified module function or constant.
            if (name.size() != token.text.size() && token.text != "self" &&
                std::ranges::none_of(visible_,
                                     [&](auto local) { return function_.locals[local].name == token.text; })) {
                ATELIER_TRY_BIND(qualified, expression_name(false));
                ATELIER_TRY_BIND(calling, accept("("));
                if (*calling)
                    return source_call(*qualified, token.position);
                return module_constant(*qualified, token.position);
            }
        }
        ATELIER_TRY(consume());
        if (token.text == "true" || token.text == "false")
            return literal(ValueType::boolean, token.position, token.text == "true" ? 1 : 0);
        if (token.text == "self")
            return add(Expression{Expression::Kind::receiver, ValueType::strong_reference, token.position});
        if (token.text == "null")
            return add(Expression{Expression::Kind::null_reference, ValueType::null_reference, token.position});
        if (token.text == "new") {
            ATELIER_TRY(object_type());
            ATELIER_TRY(expect("("));
            if (token_.text != ")")
                return reject(token_.position, "Object creation does not accept constructor arguments.");
            ATELIER_TRY(consume());
            return add(Expression{Expression::Kind::create_reference, ValueType::strong_reference, token.position});
        }
        if (token.text == "observe" || token.text == "try_acquire")
            return reference_operation(token);
        for (auto iterator = visible_.rbegin(); iterator != visible_.rend(); ++iterator) {
            const auto& local_value = function_.locals[*iterator];
            if (local_value.name == token.text)
                return add(
                    Expression{Expression::Kind::local, local_value.type, token.position, no_node, no_node, *iterator});
        }
        ATELIER_TRY_BIND(calling, accept("("));
        if (*calling)
            return source_call(token.text, token.position);
        return module_constant(token.text, token.position);
    }

    std::expected<NodeId, Diagnostic> module_constant(std::string_view name, SourcePosition position) {
        if (const auto* property = find_property(name, false))
            return property_access(*property, position);
        if (const auto found = constant_indices_.find(name); found != constant_indices_.end()) {
            const std::size_t i = found->second;
            ATELIER_TRY(resolve_constant(i));
            const auto& constant = constants_[i];
            if (scalar(constant.type)) {
                ATELIER_TRY_BIND(node, literal(constant.type, position,
                                               read_scalar_bits(constant.type, constant.inline_bytes.data()), true));
                return node;
            }
            if (const auto mtype = math_type(constant.type)) {
                Expression result{Expression::Kind::math_construct, constant.type, position};
                result.arguments.reserve(mtype.components());
                for (std::size_t lane_idx = 0; lane_idx < mtype.components(); ++lane_idx) {
                    const auto lane_bits = read_scalar_bits(math_scalar(mtype), constant.inline_bytes.data() +
                                                                                    lane_idx * mtype.scalar_size());
                    ATELIER_TRY_BIND(lane, literal(math_scalar(mtype), position, lane_bits, true));
                    result.arguments.push_back(*lane);
                }
                return add(std::move(result));
            }
            return add(Expression{Expression::Kind::constant_data, constant.type, position, no_node, no_node,
                                  constant.data_index});
        }
        return reject(position, "Unknown identifier '" + std::string(name) + "'.");
    }

    // Called after the opening parenthesis.
    std::expected<NodeId, Diagnostic> source_call(std::string_view name, SourcePosition position) {
        for (std::uint32_t i = 0; i < module_.functions.size(); ++i) {
            const auto& target = module_.functions[i];
            if (target.function_name != name)
                continue;
            ATELIER_TRY_BIND(arguments, call_arguments(target.parameters));
            ATELIER_TRY_BIND(call, call_expression(false, i, position, no_node, std::move(*arguments)));
            return add(std::move(*call));
        }
        return reject(position, "Unknown source function '" + std::string(name) + "'.");
    }

    Environment environment_;

    struct TransparentStringHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
    };

    struct Constant {
        std::string name;

        ValueType type;

        SourcePosition position;

        Lexer lexer;

        Token token;

        enum class State { fresh, active, complete } state{};

        std::array<std::byte, 128> inline_bytes{};

        std::uint32_t data_index{UINT32_MAX};
    };

    std::vector<Constant> constants_;

    std::unordered_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>> constant_indices_;

    Function function_;

    Program module_;

    std::vector<std::uint32_t> visible_;

    std::vector<std::size_t> expression_heights_;

    std::size_t depth_{};

    std::size_t loop_depth_{};

    std::size_t parallel_loop_depth_{};
};
}

std::expected<Program, Diagnostic> compile_source(std::string_view source_name, std::string_view source,
                                                  Environment environment, std::string_view entry_name,
                                                  bool allow_declarations, bool allow_parameter_attributes) {
    ATELIER_TRY_BIND(tokens, Tokens::create(source));
    return Parser(source_name, std::move(*tokens), environment)
        .parse(entry_name, allow_declarations, allow_parameter_attributes);
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
        wrapper.parallel_safe = found->parallel_safe;
        wrapper.description = found->description;
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
    Parser::mark_reachable_parallel_callees(program);
    return {};
}

}
