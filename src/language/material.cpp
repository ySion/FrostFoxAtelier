#include "language/material.hpp"
#include "language/material_footprint.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace atelier::language {
namespace {
[[noreturn]] void reject(SourcePosition position, std::string message) {
    throw Diagnostic{position, std::move(message)};
}
std::string number(std::uint32_t bits) {
    return "asfloat(" + std::to_string(bits) + "u)";
}

// The material subset is acyclic and pure. Execute its typed value/assignment
// semantics into a field-sensitive DAG, then emit only the coverage root. A
// selection is lazy control flow, not an eager HLSL ternary: samples in skipped
// branches (including short-circuit operands and early returns) stay skipped.
class Coverage {
    using Value = std::vector<NodeId>;
    struct Node {
        Expression expression;
        NodeId condition{no_node};
    };
    struct Flow {
        std::vector<Value> locals;
        // Snapshot returned values before later assignments. Locals may keep
        // evolving on the fallthrough path; the final lazy selection excludes
        // that continuation on every path which has already returned.
        NodeId returned{no_node};
        Value result;
    };
    const MaterialModule& material_;
    const Program& program_;
    std::vector<Node> nodes_;
    std::map<std::pair<ValueType::Kind, std::uint64_t>, NodeId> literals_;
    std::vector<std::vector<std::uint64_t>> required_;

    bool record(ValueType type) const {
        return type.kind == ValueType::record && type.type_index != material_.texture_input;
    }
    std::size_t width(ValueType type) const {
        if (!record(type))
            return type == ValueType::void_value ? 0 : 1;
        std::size_t count{};
        for (const auto& field : program_.value_layouts[type.type_index].fields)
            count += width(field.type);
        return count;
    }
    std::size_t field_begin(ValueType owner, std::size_t offset) const {
        std::size_t begin{};
        for (const auto& field : program_.value_layouts[owner.type_index].fields) {
            if (field.offset == offset)
                return begin;
            begin += width(field.type);
        }
        throw std::logic_error("Missing typed coverage field.");
    }
    NodeId add(Expression expression) {
        nodes_.push_back({std::move(expression)});
        return nodes_.size() - 1;
    }
    NodeId literal(ValueType type, std::uint64_t bits) {
        const auto key = std::pair{type.kind, bits};
        if (const auto found = literals_.find(key); found != literals_.end())
            return found->second;
        Expression value{Expression::Kind::literal, type, {}};
        value.bits = bits;
        const auto id = add(std::move(value));
        literals_.emplace(key, id);
        return id;
    }
    NodeId boolean(bool value) { return literal(ValueType::boolean, value); }
    NodeId select(NodeId condition, NodeId yes, NodeId no) {
        if (yes == no)
            return yes;
        if (condition == boolean(true))
            return yes;
        if (condition == boolean(false))
            return no;
        if (yes == boolean(true) && no == boolean(false))
            return condition;
        nodes_.push_back({Expression{Expression::Kind::literal, nodes_[yes].expression.type, {}, yes, no}, condition});
        return nodes_.size() - 1;
    }
    Value select(NodeId condition, const Value& yes, const Value& no) {
        Value value;
        for (std::size_t i = 0; i < yes.size(); ++i)
            value.push_back(select(condition, yes[i], no[i]));
        return value;
    }
    Value initial(ValueType type, const std::byte* bytes = nullptr) {
        if (!record(type)) {
            std::uint32_t bits{};
            if (bytes) {
                if (type == ValueType::boolean)
                    bits = *bytes != std::byte{};
                else
                    std::memcpy(&bits, bytes, sizeof(bits));
            }
            return {literal(type, bits)};
        }
        const auto& layout = program_.value_layouts[type.type_index];
        if (!bytes && !layout.initial_value.empty())
            bytes = layout.initial_value.data();
        Value value;
        for (const auto& field : layout.fields) {
            const auto child = initial(field.type, bytes ? bytes + field.offset : nullptr);
            value.insert(value.end(), child.begin(), child.end());
        }
        return value;
    }
    Value unpack(NodeId id) {
        const auto type = nodes_[id].expression.type;
        if (!record(type))
            return {id};
        Value value;
        for (const auto& field : program_.value_layouts[type.type_index].fields) {
            Expression access{Expression::Kind::value_field, field.type, {}, id};
            access.offset = field.offset;
            const auto child = unpack(add(std::move(access)));
            value.insert(value.end(), child.begin(), child.end());
        }
        return value;
    }
    NodeId pack(ValueType type, const Value& value) {
        if (!record(type))
            return value[0];
        Expression construct{Expression::Kind::math_construct, type, {}};
        construct.arguments = value;
        return add(std::move(construct));
    }
    NodeId binary(Operation operation, NodeId left, NodeId right, ValueType type = ValueType::f32) {
        Expression expression{Expression::Kind::binary, type, {}, left, right};
        expression.operation = operation;
        return add(std::move(expression));
    }
    Value expression(const Function& function, NodeId id, const Flow& flow) {
        const auto& node = function.expressions[id];
        const auto operand = [&](NodeId child) { return expression(function, child, flow); };
        using Kind = Expression::Kind;
        switch (node.kind) {
        case Kind::literal:
            return {literal(node.type, node.bits)};
        case Kind::local:
            return flow.locals[node.value];
        case Kind::value_field: {
            const auto owner = operand(node.left);
            const auto begin = field_begin(function.expressions[node.left].type, node.offset);
            return Value(owner.begin() + begin, owner.begin() + begin + width(node.type));
        }
        case Kind::unary:
        case Kind::scalar_cast: {
            Value value;
            for (const auto child : operand(node.left)) {
                auto scalar = node;
                scalar.type = record(node.type) ? ValueType::f32 : node.type;
                scalar.left = child;
                value.push_back(add(std::move(scalar)));
            }
            return value;
        }
        case Kind::binary: {
            const auto left = operand(node.left), right = operand(node.right);
            if (node.operation == Operation::logical_and)
                return {select(left[0], right[0], boolean(false))};
            if (node.operation == Operation::logical_or)
                return {select(left[0], boolean(true), right[0])};
            Value value;
            const auto count = std::max(left.size(), right.size());
            const auto scalar = record(node.type) ? ValueType{ValueType::f32} : node.type;
            for (std::size_t i = 0; i < count; ++i)
                value.push_back(
                    binary(node.operation, left[left.size() == 1 ? 0 : i], right[right.size() == 1 ? 0 : i], scalar));
            if (node.type == ValueType::boolean && count > 1) {
                auto result = value[0];
                for (std::size_t i = 1; i < count; ++i)
                    result = binary(node.operation == Operation::equal ? Operation::logical_and : Operation::logical_or,
                                    result, value[i], ValueType::boolean);
                return {result};
            }
            return value;
        }
        case Kind::math_construct: {
            if (node.arguments.empty())
                return initial(node.type);
            Value value;
            for (const auto child : node.arguments) {
                const auto part = operand(child);
                value.insert(value.end(), part.begin(), part.end());
            }
            return value;
        }
        case Kind::source_call: {
            std::vector<Value> arguments;
            for (const auto child : node.arguments)
                arguments.push_back(operand(child));
            return invoke(program_.functions[node.value], std::move(arguments));
        }
        case Kind::math_intrinsic: {
            std::vector<Value> arguments;
            for (const auto child : node.arguments)
                arguments.push_back(operand(child));
            if (node.math_operation == MathOperation::cross) {
                Value value;
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto j = (i + 1) % 3, k = (i + 2) % 3;
                    value.push_back(binary(Operation::subtract,
                                           binary(Operation::multiply, arguments[0][j], arguments[1][k]),
                                           binary(Operation::multiply, arguments[0][k], arguments[1][j])));
                }
                return value;
            }
            if (node.math_operation >= MathOperation::square_root) {
                Value value;
                for (std::size_t i = 0; i < width(node.type); ++i) {
                    auto scalar = node;
                    scalar.type = ValueType::f32;
                    scalar.arguments.clear();
                    for (const auto& argument : arguments)
                        scalar.arguments.push_back(argument[argument.size() == 1 ? 0 : i]);
                    value.push_back(add(std::move(scalar)));
                }
                return value;
            }
            auto operation = node; // dot/length consume all lanes, unlike element-wise operations.
            for (std::size_t i = 0; i < arguments.size(); ++i)
                operation.arguments[i] = pack(function.expressions[node.arguments[i]].type, arguments[i]);
            return unpack(add(std::move(operation)));
        }
        case Kind::texture_sample:
        case Kind::texture_orientation: {
            auto sample = node;
            sample.left = pack(function.expressions[node.left].type, operand(node.left));
            sample.right = operand(node.right)[0];
            return unpack(add(std::move(sample)));
        }
        default:
            throw std::logic_error("Unvalidated coverage expression.");
        }
    }
    std::pair<std::uint32_t, std::size_t> target(const Function& function, NodeId id) const {
        const auto& node = function.expressions[id];
        if (node.kind == Expression::Kind::local)
            return {node.value, 0};
        auto result = target(function, node.left);
        result.second += field_begin(function.expressions[node.left].type, node.offset);
        return result;
    }
    void statement(const Function& function, NodeId id, Flow& flow) {
        const auto& node = function.statements[id];
        using Kind = Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            for (const auto child : node.children) {
                if (flow.returned == boolean(true))
                    break;
                statement(function, child, flow);
            }
            break;
        case Kind::declare:
            flow.locals[node.target] = node.expression == no_node ? initial(function.locals[node.target].type)
                                                                  : expression(function, node.expression, flow);
            break;
        case Kind::assign: {
            const auto value = expression(function, node.expression, flow);
            const auto [local, begin] = target(function, node.target);
            std::copy(value.begin(), value.end(), flow.locals[local].begin() + begin);
            break;
        }
        case Kind::call:
            break; // No observable side effects in the validated pure subset.
        case Kind::return_value: {
            const auto value = node.expression == no_node ? Value{} : expression(function, node.expression, flow);
            flow.result = flow.returned == no_node ? value : select(flow.returned, flow.result, value);
            flow.returned = boolean(true);
            break;
        }
        case Kind::condition: {
            const auto condition = expression(function, node.expression, flow)[0];
            Flow yes{flow.locals, no_node, {}}, no{flow.locals, no_node, {}};
            statement(function, node.body, yes);
            if (node.alternative != no_node)
                statement(function, node.alternative, no);
            for (std::size_t i = 0; i < flow.locals.size(); ++i) {
                // Branch-local declarations cannot escape their lexical scope.
                if (!flow.locals[i].empty())
                    flow.locals[i] = select(condition, yes.locals[i], no.locals[i]);
            }
            if (yes.returned == no_node && no.returned == no_node)
                break;
            auto value = yes.returned == no_node  ? no.result
                         : no.returned == no_node ? yes.result
                                                  : select(condition, yes.result, no.result);
            auto returned = select(condition, yes.returned == no_node ? boolean(false) : yes.returned,
                                   no.returned == no_node ? boolean(false) : no.returned);
            if (flow.returned != no_node) {
                value = select(flow.returned, flow.result, value);
                returned = select(flow.returned, boolean(true), returned);
            }
            flow.result = std::move(value);
            flow.returned = returned;
            break;
        }
        default:
            throw std::logic_error("Unvalidated coverage statement.");
        }
    }
    Value invoke(const Function& function, std::vector<Value> arguments) {
        Flow flow{std::move(arguments), no_node, {}};
        flow.locals.resize(function.locals.size());
        statement(function, function.body, flow);
        return flow.result;
    }
    const std::vector<std::uint64_t>& required(NodeId id) {
        auto& result = required_[id];
        if (!result.empty())
            return result;
        // Nodes are appended after their operands. Memoized bitsets describe
        // must-execute dependencies, avoiding repeated walks of the shared DAG.
        result.resize(id / 64 + 1);
        result[id / 64] |= std::uint64_t{1} << (id % 64);
        const auto include = [&](NodeId child) {
            if (child == no_node)
                return;
            const auto& dependencies = required(child);
            for (std::size_t i = 0; i < dependencies.size(); ++i)
                result[i] |= dependencies[i];
        };
        const auto& node = nodes_[id];
        if (node.condition == no_node) {
            include(node.expression.left);
            include(node.expression.right);
            for (const auto argument : node.expression.arguments)
                include(argument);
        } else {
            include(node.condition);
            const auto& yes = required(node.expression.left);
            const auto& no = required(node.expression.right);
            // A nested select requires its condition and only the INTERSECTION
            // of its arms. Union would make branch-only sampling eager.
            for (std::size_t i = 0; i < std::min(yes.size(), no.size()); ++i)
                result[i] |= yes[i] & no[i];
        }
        return result;
    }
    NodeId emit(NodeId id, Function& function, std::vector<Statement>& statements,
                std::vector<NodeId>& available) const {
        if (available[id] != no_node)
            return available[id];
        const auto& node = nodes_[id];
        auto expression = node.expression;
        const auto add_expression = [&](Expression value) {
            function.expressions.push_back(std::move(value));
            return function.expressions.size() - 1;
        };
        const auto add_block = [&](std::vector<Statement> children) {
            Statement block{Statement::Kind::block, {}, no_node, no_node, no_node, no_node, {}};
            for (auto& child : children) {
                block.children.push_back(function.statements.size());
                function.statements.push_back(std::move(child));
            }
            function.statements.push_back(std::move(block));
            return function.statements.size() - 1;
        };
        NodeId condition = no_node;
        if (node.condition != no_node) {
            condition = emit(node.condition, function, statements, available);
            const auto& yes = required_[node.expression.left];
            const auto& no = required_[node.expression.right];
            // Materialize shared must-execute values at this common dominator
            // before copying the scope. Emit consumers first so a shared select
            // retains its own lazy branches, rather than expanding in each arm.
            for (std::size_t i = std::min(yes.size(), no.size()); i-- > 0;) {
                auto common = yes[i] & no[i];
                while (common) {
                    const auto bit = static_cast<std::size_t>(std::bit_width(common) - 1);
                    emit(i * 64 + bit, function, statements, available);
                    common &= ~(std::uint64_t{1} << bit);
                }
            }
        } else {
            if (expression.left != no_node)
                expression.left = emit(expression.left, function, statements, available);
            if (expression.right != no_node)
                expression.right = emit(expression.right, function, statements, available);
            for (auto& argument : expression.arguments)
                argument = emit(argument, function, statements, available);
            if (expression.kind == Expression::Kind::literal || expression.kind == Expression::Kind::local)
                return available[id] = add_expression(std::move(expression));
        }
        const auto local = static_cast<std::uint32_t>(function.locals.size());
        function.locals.push_back({"coverage", expression.type});
        const auto reference =
            add_expression(Expression{Expression::Kind::local, expression.type, {}, no_node, no_node, local});
        Statement declare{Statement::Kind::declare, {}, no_node, no_node, no_node, no_node, {}};
        declare.target = local;
        if (condition == no_node)
            declare.expression = add_expression(std::move(expression));
        statements.push_back(std::move(declare));
        if (condition != no_node) {
            Statement branch{Statement::Kind::condition, {}, no_node, no_node, no_node, no_node, {}};
            branch.expression = condition;
            const auto arm = [&](NodeId value) {
                // Only definitions dominating this arm are reusable. Never hoist
                // a sample out of its controlling branch to share a temporary.
                auto scoped = available;
                std::vector<Statement> body;
                Statement assign{Statement::Kind::assign, {}, no_node, no_node, no_node, no_node, {}};
                assign.target = reference;
                assign.expression = emit(value, function, body, scoped);
                body.push_back(std::move(assign));
                return add_block(std::move(body));
            };
            branch.body = arm(node.expression.left);
            branch.alternative = arm(node.expression.right);
            statements.push_back(std::move(branch));
        }
        return available[id] = reference;
    }

public:
    explicit Coverage(const MaterialModule& material) : material_(material), program_(material.program) {}
    Function generate(std::size_t offset) {
        const auto& shade = program_.functions[program_.exports[0]];
        std::vector<Value> arguments;
        Function result;
        result.function_name = "coverage";
        result.result = ValueType::f32;
        result.parameters = shade.parameters;
        result.locals.assign(shade.locals.begin(), shade.locals.begin() + shade.parameters.size());
        for (std::size_t i = 0; i < shade.parameters.size(); ++i)
            arguments.push_back(unpack(add(Expression{Expression::Kind::local,
                                                      shade.parameters[i].type,
                                                      {},
                                                      no_node,
                                                      no_node,
                                                      static_cast<std::uint32_t>(i)})));
        const auto value = invoke(shade, std::move(arguments));
        const auto root = value[field_begin(shade.result, offset)];
        required_.resize(nodes_.size());
        required(root);
        std::vector<NodeId> available(nodes_.size(), no_node);
        std::vector<Statement> body;
        Statement returned{Statement::Kind::return_value, {}, no_node, no_node, no_node, no_node, {}};
        returned.expression = emit(root, result, body, available);
        body.push_back(std::move(returned));
        Statement block{Statement::Kind::block, {}, no_node, no_node, no_node, no_node, {}};
        for (auto& child : body) {
            block.children.push_back(result.statements.size());
            result.statements.push_back(std::move(child));
        }
        result.body = result.statements.size();
        result.statements.push_back(std::move(block));
        return result;
    }
};

class Hlsl {
    const MaterialModule& material_;
    const Program& program_;
    std::vector<bool> emitted_;
    std::string text_;
    std::size_t temporary_{};
    std::string type(ValueType value) const {
        switch (value.kind) {
        case ValueType::f32:
            return "float";
        case ValueType::boolean:
            return "bool";
        case ValueType::u32:
            return "uint";
        case ValueType::void_value:
            return "void";
        case ValueType::record: {
            if (value.type_index == material_.texture_input)
                return "uint";
            const auto& layout = program_.value_layouts[value.type_index];
            if (layout.math)
                return "float" + std::to_string(layout.math.components());
            if (value.type_index == material_.context)
                return "ms_Context";
            if (value.type_index == material_.output)
                return "ms_Output";
            if (value.type_index == material_.parameters)
                return "ms_Parameters";
            return "ms_Type" + std::to_string(value.type_index);
        }
        default:
            throw std::logic_error("Unvalidated material type.");
        }
    }
    std::string initial(ValueType value, const std::byte* bytes) const {
        if (value == ValueType{ValueType::record, material_.texture_input})
            return "0u";
        if (value.kind == ValueType::boolean)
            return bytes && *bytes != std::byte{} ? "true" : "false";
        if (value.kind == ValueType::f32) {
            std::uint32_t bits{};
            if (bytes)
                std::memcpy(&bits, bytes, sizeof(bits));
            return number(bits);
        }
        const auto& layout = program_.value_layouts[value.type_index];
        if (!layout.math)
            return "ms_initial" + std::to_string(value.type_index) + "()";
        std::string result = type(value) + "(";
        for (std::size_t i = 0; i < layout.math.components(); ++i) {
            if (i)
                result += ",";
            result += initial(ValueType::f32, bytes ? bytes + i * sizeof(float) : nullptr);
        }
        return result + ")";
    }
    void initialize_fields(ValueType value, const std::string& path, const std::byte* bytes) {
        if (value.kind != ValueType::record || value.type_index == material_.texture_input ||
            program_.value_layouts[value.type_index].math) {
            text_ += path + " = " + initial(value, bytes) + ";\n";
            return;
        }
        for (const auto& field : program_.value_layouts[value.type_index].fields)
            initialize_fields(field.type, path + ".ms_" + field.name, bytes ? bytes + field.offset : nullptr);
    }
    void declare_type(ValueType value) {
        if (value.kind != ValueType::record || emitted_[value.type_index])
            return;
        emitted_[value.type_index] = true;
        const auto& layout = program_.value_layouts[value.type_index];
        if (layout.math || value.type_index == material_.texture_input)
            return;
        for (const auto& field : layout.fields)
            declare_type(field.type);
        text_ += "struct " + type(value) + " {\n";
        for (const auto& field : layout.fields)
            text_ += type(field.type) + " ms_" + field.name + ";\n";
        text_ += "};\n" + type(value) + " ms_initial" + std::to_string(value.type_index) + "() {\n";
        text_ += type(value) + " v = (" + type(value) + ")0;\n";
        initialize_fields(value, "v", layout.initial_value.data());
        text_ += "return v;\n}\n";
    }
    static std::string operation(Operation operation) {
        switch (operation) {
        case Operation::add:
        case Operation::positive:
            return "+";
        case Operation::subtract:
        case Operation::negate:
            return "-";
        case Operation::multiply:
            return "*";
        case Operation::divide:
            return "/";
        case Operation::equal:
            return "==";
        case Operation::not_equal:
            return "!=";
        case Operation::less:
            return "<";
        case Operation::less_equal:
            return "<=";
        case Operation::greater:
            return ">";
        case Operation::greater_equal:
            return ">=";
        case Operation::logical_not:
            return "!";
        case Operation::logical_and:
            return "&&";
        case Operation::logical_or:
            return "||";
        default:
            throw std::logic_error("Unvalidated material operation.");
        }
    }
    static std::string intrinsic(MathOperation operation) {
        switch (operation) {
        case MathOperation::dot:
            return "dot";
        case MathOperation::cross:
            return "cross";
        case MathOperation::length:
            return "length";
        case MathOperation::square_root:
            return "sqrt";
        case MathOperation::sin:
            return "sin";
        case MathOperation::cos:
            return "cos";
        case MathOperation::floor:
            return "floor";
        case MathOperation::ceil:
            return "ceil";
        case MathOperation::trunc:
            return "trunc";
        case MathOperation::fract:
            return "ms_fract";
        case MathOperation::abs:
            return "abs";
        case MathOperation::min:
            return "min";
        case MathOperation::max:
            return "max";
        case MathOperation::clamp:
            return "ms_clamp";
        case MathOperation::lerp:
            return "ms_lerp";
        case MathOperation::step:
            return "step";
        default:
            return {};
        }
    }
    std::string expression(const Function& function, NodeId id) const {
        const auto& node = function.expressions[id];
        const auto operand = [&](NodeId value) { return expression(function, value); };
        using Kind = Expression::Kind;
        switch (node.kind) {
        case Kind::literal:
            if (node.type == ValueType::f32)
                return number(static_cast<std::uint32_t>(node.bits));
            if (node.type == ValueType::boolean)
                return node.bits ? "true" : "false";
            return std::to_string(node.bits) + "u";
        case Kind::local:
            return "ms_v" + std::to_string(node.value);
        case Kind::value_field: {
            const auto owner = function.expressions[node.left].type;
            const auto& layout = program_.value_layouts[owner.type_index];
            const auto field =
                std::ranges::find_if(layout.fields, [&](const auto& value) { return value.offset == node.offset; });
            if (field == layout.fields.end())
                throw std::logic_error("Missing typed material field.");
            return "(" + operand(node.left) + ")." + (layout.math ? "" : "ms_") + field->name;
        }
        case Kind::unary:
            return "(" + operation(node.operation) + operand(node.left) + ")";
        case Kind::binary: {
            auto result = "(" + operand(node.left) + operation(node.operation) + operand(node.right) + ")";
            const auto left = function.expressions[node.left].type;
            if (left.kind == ValueType::record &&
                (node.operation == Operation::equal || node.operation == Operation::not_equal))
                result = (node.operation == Operation::equal ? "all" : "any") + result;
            return result;
        }
        case Kind::texture_sample:
            return "::atelier_material_sample(ms_environment," + operand(node.right) + "," + operand(node.left) + ")";
        case Kind::texture_orientation:
            return "::atelier_material_sample_orientation(ms_environment," + operand(node.right) + "," +
                   operand(node.left) + ")";
        case Kind::scalar_cast:
            return type(node.type) + "(" + operand(node.left) + ")";
        case Kind::math_construct:
        case Kind::math_intrinsic:
        case Kind::source_call: {
            if (node.arguments.empty() && node.kind == Kind::math_construct)
                return initial(node.type, nullptr);
            std::string result = node.kind == Kind::source_call      ? "ms_f" + std::to_string(node.value)
                                 : node.kind == Kind::math_construct ? type(node.type)
                                                                     : intrinsic(node.math_operation);
            result += "(";
            if (node.kind == Kind::source_call)
                result += "ms_environment";
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                if (i || node.kind == Kind::source_call)
                    result += ",";
                result += operand(node.arguments[i]);
            }
            return result + ")";
        }
        default:
            throw std::logic_error("Unvalidated material expression.");
        }
    }
    void statement(const Function& function, NodeId id) {
        const auto& node = function.statements[id];
        using Kind = Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            text_ += "{\n";
            for (const auto child : node.children)
                statement(function, child);
            text_ += "}\n";
            break;
        case Kind::declare: {
            const auto& local = function.locals[node.target];
            text_ += type(local.type) + " ms_v" + std::to_string(node.target) + " = ";
            text_ += node.expression == no_node ? initial(local.type, nullptr) : expression(function, node.expression);
            text_ += ";\n";
            break;
        }
        case Kind::assign:
            text_ += expression(function, node.target) + " = " + expression(function, node.expression) + ";\n";
            break;
        case Kind::call:
            text_ += expression(function, node.expression) + ";\n";
            break;
        case Kind::return_value:
            text_ += "return" +
                     (node.expression == no_node ? std::string{} : " " + expression(function, node.expression)) + ";\n";
            break;
        case Kind::condition:
            text_ += "if (" + expression(function, node.expression) + ") {\n";
            statement(function, node.body);
            text_ += "}\n";
            if (node.alternative != no_node) {
                text_ += "else {\n";
                statement(function, node.alternative);
                text_ += "}\n";
            }
            break;
        default:
            throw std::logic_error("Unvalidated material statement.");
        }
    }
    bool continuous(ValueType value) const {
        return value.kind == ValueType::f32 ||
               (value.kind == ValueType::record && value.type_index != material_.texture_input);
    }
    std::string differential_type(ValueType value) const { return (continuous(value) ? "ms_D" : "") + type(value); }
    std::string field_name(const Function& function, const Expression& node) const {
        const auto& layout = program_.value_layouts[function.expressions[node.left].type.type_index];
        const auto field =
            std::ranges::find_if(layout.fields, [&](const auto& value) { return value.offset == node.offset; });
        if (field == layout.fields.end())
            throw std::logic_error("Missing typed material field.");
        return (layout.math ? "" : "ms_") + field->name;
    }
    void differential_helpers() {
        // Three ordinary values preserve component locality even in nested
        // records: projecting UV never inspects a sibling normal's changes.
        const auto declare = [&](const std::string& value, bool numeric) {
            const auto d = "ms_D" + value;
            text_ += "struct " + d + " { " + (numeric ? "precise " : "") + value + " v,d0,d1; };\n";
            text_ += d + " ms_seed(" + value + " v," + value + " d0," + value + " d1) { " + d +
                     " r; r.v=v; r.d0=d0; r.d1=d1; return r; }\n";
            text_ += d + " ms_constant(" + value + " v) { return ms_seed(v,(" + value + ")0,(" + value + ")0); }\n";
            text_ += value + " ms_value(" + d + " v) { return v.v; }\n";
        };
        for (std::size_t n = 1; n <= 4; ++n)
            declare("float" + (n == 1 ? std::string{} : std::to_string(n)), true);
        for (std::size_t i = 0; i < emitted_.size(); ++i) {
            if (emitted_[i] && !program_.value_layouts[i].math && i != material_.texture_input)
                declare(type({ValueType::record, static_cast<std::uint32_t>(i)}), false);
        }
        for (const auto* scalar : {"bool", "uint"}) {
            const std::string value = scalar;
            text_ += value + " ms_seed(" + value + " v," + value + " d0," + value + " d1) { return v; }\n";
            text_ += value + " ms_constant(" + value + " v) { return v; }\n";
            text_ += value + " ms_value(" + value + " v) { return v; }\n";
        }
        text_ += detail::material_footprint_scalar_hlsl;
        const auto lane = [](std::string_view value, char component) {
            const std::string a(value), c(1, component);
            return "ms_seed(" + a + ".v." + c + "," + a + ".d0." + c + "," + a + ".d1." + c + ")";
        };
        struct Element {
            const char* name;
            std::size_t arity;
        };
        constexpr Element elements[]{{"add", 2}, {"sub", 2},  {"mul", 2},   {"div", 2},  {"neg", 1},   {"sin", 1},
                                     {"cos", 1}, {"sqrt", 1}, {"floor", 1}, {"ceil", 1}, {"trunc", 1}, {"fract", 1},
                                     {"abs", 1}, {"min", 2},  {"max", 2},   {"step", 2}, {"clamp", 3}, {"lerp", 3}};
        constexpr std::string_view components = "xyzw";
        for (std::size_t n = 2; n <= 4; ++n) {
            const auto value = "float" + std::to_string(n), d = "ms_D" + value;
            for (const auto& element : elements) {
                const auto name = "ms_d" + std::string(element.name);
                text_ += d + " " + name + "(" + d + " a";
                if (element.arity >= 2)
                    text_ += "," + d + " b";
                if (element.arity >= 3)
                    text_ += "," + d + " c";
                text_ += ") { " + d + " r;\n";
                for (std::size_t i = 0; i < n; ++i) {
                    const auto c = components[i];
                    const std::string temporary = "t" + std::to_string(i);
                    text_ += "ms_Dfloat " + temporary + "=" + name + "(" + lane("a", c);
                    if (element.arity >= 2)
                        text_ += "," + lane("b", c);
                    if (element.arity >= 3)
                        text_ += "," + lane("c", c);
                    text_ += ");\n";
                    for (const auto* part : {"v", "d0", "d1"})
                        text_ += "r." + std::string(part) + "." + c + "=" + temporary + "." + part + ";\n";
                }
                text_ += "return r; }\n";
            }
            text_ += "ms_Dfloat ms_ddot(" + d + " a," + d + " b) { ms_Dfloat r=ms_dmul(" + lane("a", 'x') + "," +
                     lane("b", 'x') + ");\n";
            for (std::size_t i = 1; i < n; ++i)
                text_ += "r=ms_dadd(r,ms_dmul(" + lane("a", components[i]) + "," + lane("b", components[i]) + "));\n";
            text_ += "r.v=dot(a.v,b.v); return r; }\n";
            text_ += "float ms_length_change(" + value + " a," + value +
                     " d,float r) {\n"
                     "if (!ms_finite(r)) return ms_unavailable();\n"
                     "if (r==0.0) return all((asuint(d)&0x7fffffffu)==0u) ? 0.0 : ms_unavailable();\n"
                     "precise float sum=ms_change_scale(a.x,d.x);\n";
            for (std::size_t i = 1; i < n; ++i) {
                const std::string c(1, components[i]);
                text_ += "sum=ms_change_add(sum,ms_change_scale(a." + c + ",d." + c + "));\n";
            }
            text_ +=
                "if (!ms_finite(sum)) return ms_unavailable(); precise float change=sum/r; return ms_checked(change); }\n";
            text_ += "ms_Dfloat ms_dlength(" + d +
                     " a) { precise float r=length(a.v);\n"
                     "return ms_seed(r,ms_length_change(a.v,a.d0,r),ms_length_change(a.v,a.d1,r)); }\n";
        }
        text_ += "ms_Dfloat3 ms_dcross(ms_Dfloat3 a,ms_Dfloat3 b) { ms_Dfloat3 r;\n";
        for (std::size_t i = 0; i < 3; ++i) {
            const auto j = components[(i + 1) % 3], k = components[(i + 2) % 3];
            const auto t = "t" + std::to_string(i);
            text_ += "ms_Dfloat " + t + "=ms_dsub(ms_dmul(" + lane("a", j) + "," + lane("b", k) + "),ms_dmul(" +
                     lane("a", k) + "," + lane("b", j) + "));\n";
            for (const auto* part : {"d0", "d1"})
                text_ += "r." + std::string(part) + "." + components[i] + "=" + t + "." + part + ";\n";
        }
        text_ += "r.v=cross(a.v,b.v); return r; }\n";
        text_ += detail::material_footprint_sample_hlsl;
    }
    std::string differential_expression(const Function& function, NodeId id) {
        const auto& node = function.expressions[id];
        const auto emit = [&](const std::string& value) {
            if (node.type == ValueType::void_value) {
                text_ += value + ";\n";
                return std::string{};
            }
            const auto name = "ms_t" + std::to_string(temporary_++);
            text_ += differential_type(node.type) + " " + name + "=" + value + ";\n";
            return name;
        };
        const auto operand = [&](NodeId value) { return differential_expression(function, value); };
        const auto broadcast = [&](const std::string& value, ValueType from) {
            if (from == ValueType::f32 && node.type.kind == ValueType::record &&
                program_.value_layouts[node.type.type_index].math) {
                const auto target = type(node.type);
                return "ms_seed((" + target + ")" + value + ".v,(" + target + ")" + value + ".d0,(" + target + ")" +
                       value + ".d1)";
            }
            return value;
        };
        using Kind = Expression::Kind;
        switch (node.kind) {
        case Kind::literal:
            return emit("ms_constant(" + expression(function, id) + ")");
        case Kind::local:
            return emit("ms_v" + std::to_string(node.value));
        case Kind::value_field: {
            const auto owner = operand(node.left), field = field_name(function, node);
            if (!continuous(node.type))
                return emit(owner + ".v." + field);
            return emit("ms_seed(" + owner + ".v." + field + "," + owner + ".d0." + field + "," + owner + ".d1." +
                        field + ")");
        }
        case Kind::unary: {
            const auto value = operand(node.left);
            if (node.operation == Operation::positive)
                return value;
            return emit(node.operation == Operation::negate ? "ms_dneg(" + value + ")" : "(!" + value + ")");
        }
        case Kind::binary: {
            const auto a = operand(node.left);
            if (node.operation == Operation::logical_and || node.operation == Operation::logical_or) {
                const auto result = emit(a);
                text_ += "if (" + std::string(node.operation == Operation::logical_or ? "!" : "") + result + ") {\n";
                const auto b = operand(node.right);
                text_ += result + "=" + b + ";\n}\n";
                return result;
            }
            const auto b = operand(node.right);
            std::string name;
            switch (node.operation) {
            case Operation::add:
                name = "add";
                break;
            case Operation::subtract:
                name = "sub";
                break;
            case Operation::multiply:
                name = "mul";
                break;
            case Operation::divide:
                name = "div";
                break;
            default:
                break;
            }
            if (!name.empty())
                return emit("ms_d" + name + "(" + broadcast(a, function.expressions[node.left].type) + "," +
                            broadcast(b, function.expressions[node.right].type) + ")");
            auto result = "(ms_value(" + a + ")" + operation(node.operation) + "ms_value(" + b + "))";
            if (function.expressions[node.left].type.kind == ValueType::record)
                result = (node.operation == Operation::equal ? "all" : "any") + result;
            return emit(result);
        }
        case Kind::scalar_cast: {
            const auto value = operand(node.left);
            return function.expressions[node.left].type == ValueType::f32
                       ? value
                       : emit("ms_constant(float(ms_value(" + value + ")))");
        }
        case Kind::texture_sample:
        case Kind::texture_orientation: {
            const auto input = operand(node.right); // Same author argument order as CPU sample(input,uv).
            const auto uv = operand(node.left);
            if (node.kind == Kind::texture_orientation)
                return emit("ms_constant(::atelier_material_sample_orientation(ms_environment," + input + ",ms_value(" +
                            uv + ")))");
            return emit("ms_dsample(ms_environment," + input + "," + uv + ")");
        }
        case Kind::math_construct:
        case Kind::math_intrinsic:
        case Kind::source_call: {
            std::vector<std::string> arguments;
            for (const auto id : node.arguments)
                arguments.push_back(operand(id));
            if (node.kind == Kind::math_construct) {
                if (arguments.empty())
                    return emit("ms_constant(" + initial(node.type, nullptr) + ")");
                std::string result = "ms_seed(";
                for (const auto* part : {"v", "d0", "d1"}) {
                    if (part != std::string_view("v"))
                        result += ",";
                    result += type(node.type) + "(";
                    for (std::size_t i = 0; i < arguments.size(); ++i) {
                        if (i)
                            result += ",";
                        result += arguments[i] + "." + part;
                    }
                    result += ")";
                }
                return emit(result + ")");
            }
            auto name = intrinsic(node.math_operation);
            if (name.starts_with("ms_"))
                name.erase(0, 3);
            std::string result = node.kind == Kind::source_call
                                     ? "ms_f" + std::to_string(node.value) + "(ms_environment"
                                     : "ms_d" + name + "(";
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i || node.kind == Kind::source_call)
                    result += ",";
                result += node.kind == Kind::math_intrinsic
                              ? broadcast(arguments[i], function.expressions[node.arguments[i]].type)
                              : arguments[i];
            }
            return emit(result + ")");
        }
        default:
            throw std::logic_error("Unvalidated material footprint expression.");
        }
    }
    std::string differential_target(const Function& function, NodeId id, std::string_view part) const {
        const auto& node = function.expressions[id];
        if (node.kind == Expression::Kind::local)
            return "ms_v" + std::to_string(node.value) + (continuous(node.type) ? "." + std::string(part) : "");
        if (node.kind == Expression::Kind::value_field)
            return differential_target(function, node.left, part) + "." + field_name(function, node);
        throw std::logic_error("Unvalidated material footprint assignment target.");
    }
    void differential_statement(const Function& function, NodeId id) {
        const auto& node = function.statements[id];
        using Kind = Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            text_ += "{\n";
            for (const auto child : node.children)
                differential_statement(function, child);
            text_ += "}\n";
            break;
        case Kind::declare: {
            const auto& local = function.locals[node.target];
            const auto value = node.expression == no_node ? "ms_constant(" + initial(local.type, nullptr) + ")"
                                                          : differential_expression(function, node.expression);
            text_ += differential_type(local.type) + " ms_v" + std::to_string(node.target) + "=" + value + ";\n";
            break;
        }
        case Kind::assign: {
            const auto value = differential_expression(function, node.expression);
            const auto changes = continuous(function.expressions[node.target].type);
            for (const auto* part : {"v", "d0", "d1"}) {
                text_ += differential_target(function, node.target, part) + "=" + value +
                         (changes ? "." + std::string(part) : "") + ";\n";
                if (!changes)
                    break;
            }
            break;
        }
        case Kind::call:
            (void)differential_expression(function, node.expression);
            break;
        case Kind::return_value: {
            const auto result =
                node.expression == no_node ? std::string{} : " " + differential_expression(function, node.expression);
            text_ += "return" + result + ";\n";
            break;
        }
        case Kind::condition: {
            const auto condition = differential_expression(function, node.expression);
            text_ += "if (" + condition + ") {\n";
            differential_statement(function, node.body);
            text_ += "}\n";
            if (node.alternative != no_node) {
                text_ += "else {\n";
                differential_statement(function, node.alternative);
                text_ += "}\n";
            }
            break;
        }
        default:
            throw std::logic_error("Unvalidated material footprint statement.");
        }
    }
    std::string signature(std::size_t index, bool footprint = false) const {
        const auto& function = program_.functions[index];
        const auto name = [&](ValueType value) { return footprint ? differential_type(value) : type(value); };
        std::string result = name(function.result) + " ms_f" + std::to_string(index) + "(uint ms_environment";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            result += ",";
            result += name(function.parameters[i].type) + " ms_v" + std::to_string(i);
        }
        return result + ")";
    }
    void load_parameters(ValueType value, const std::string& path, std::size_t offset) {
        if (value == ValueType::f32) {
            text_ += path + " = asfloat(data.Load(base + " + std::to_string(offset) + "u));\n";
            return;
        }
        const auto& layout = program_.value_layouts[value.type_index];
        if (layout.math) {
            text_ += path + " = asfloat(data.Load" + std::to_string(layout.math.components()) + "(base + " +
                     std::to_string(offset) + "u));\n";
            return;
        }
        for (const auto& field : layout.fields)
            load_parameters(field.type, path + ".ms_" + field.name, offset + field.offset);
    }

public:
    explicit Hlsl(const MaterialModule& material)
        : material_(material), program_(material.program), emitted_(program_.value_layouts.size()) {}
    static bool supports(MathOperation operation) { return !intrinsic(operation).empty(); }
    std::string generate(bool footprint = false, bool coverage = false) {
        text_ = footprint ? "// Typed Source: center values and two local changes; no implicit derivatives.\n"
                          : "// Generated from typed Source. Sampling is linear-float4, explicit LOD-0.\n";
        std::set<std::pair<MathOperation, std::string>> helpers;
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            if (!program_.reachable[i])
                continue;
            const auto& function = program_.functions[i];
            declare_type(function.result);
            for (const auto& local : function.locals)
                declare_type(local.type);
            if (!footprint)
                for (const auto& node : function.expressions) {
                    const auto operation = node.math_operation;
                    if (node.kind != Expression::Kind::math_intrinsic ||
                        (operation != MathOperation::clamp && operation != MathOperation::lerp &&
                         operation != MathOperation::fract))
                        continue;
                    const auto value = type(node.type);
                    if (!helpers.emplace(operation, value).second)
                        continue;
                    // Typed value arguments evaluate each operand once. These Source
                    // contracts differ from HLSL's built-ins; precise prevents DXC
                    // from reassociating lerp or folding subtraction back into frac.
                    text_ += value + " " + intrinsic(operation) + "(" + value + " a";
                    if (operation == MathOperation::fract) {
                        text_ += ") { precise " + value +
                                 " result = a - floor(a); return min(result,asfloat(0x3f7fffffu)); }\n";
                    } else {
                        text_ += "," + value + " b," + value + " c) { ";
                        text_ += operation == MathOperation::clamp
                                     ? "return max(b,min(a,c)); }\n"
                                     : "precise " + value + " result = a * (1.0 - c) + b * c; return result; }\n";
                    }
                }
        }
        if (footprint)
            differential_helpers();
        for (std::size_t i = 0; i < program_.functions.size(); ++i)
            if (program_.reachable[i])
                text_ += signature(i, footprint) + ";\n";
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            if (!program_.reachable[i])
                continue;
            text_ += signature(i, footprint) + "\n";
            if (footprint)
                differential_statement(program_.functions[i], program_.functions[i].body);
            else
                statement(program_.functions[i], program_.functions[i].body);
        }
        text_ +=
            coverage ? "float ms_coverage(uint environment, ms_Context context, ms_Parameters parameters) {\n"
            : footprint
                ? "ms_Output ms_shade_footprint(uint environment, ms_Context context, ms_Context d0, ms_Context d1, ms_Parameters parameters) {\n"
                : "ms_Output ms_shade(uint environment, ms_Context context, ms_Parameters parameters) {\n";
        if (material_.resources != UINT32_MAX) {
            const auto resource_type = type({ValueType::record, material_.resources});
            text_ += resource_type + " resources = (" + resource_type + ")0;\n";
            for (const auto& input : material_.textures) {
                std::string path = "resources.ms_";
                for (const auto character : input.key)
                    path += character == '.' ? ".ms_" : std::string(1, character);
                text_ += path + " = " + std::to_string(input.slot) + "u;\n";
            }
        }
        text_ += "return " + std::string(footprint ? "ms_value(" : "") + "ms_f" + std::to_string(program_.exports[0]) +
                 (footprint ? "(environment,ms_seed(context,d0,d1),ms_constant(parameters)"
                            : "(environment,context,parameters");
        if (material_.resources != UINT32_MAX)
            text_ += footprint ? ",ms_constant(resources)" : ",resources";
        text_ += footprint ? ")); }\n" : "); }\n";
        if (!coverage)
            text_ += "ms_Parameters ms_defaults() { return " + std::string(footprint ? "ms_value(" : "") + "ms_f" +
                     std::to_string(program_.exports[1]) + (footprint ? "(0u)); }\n" : "(0u); }\n");
        text_ +=
            "ms_Parameters ms_load_parameters(ByteAddressBuffer data, uint base) {\nms_Parameters p = (ms_Parameters)0;\n";
        load_parameters({ValueType::record, material_.parameters}, "p", 0);
        text_ += "return p;\n}\n";
        return std::move(text_);
    }
};
}

std::expected<MaterialModule, Diagnostic> compile_material(std::string_view name, std::string_view source,
                                                           std::span<const ValueLayout> values, std::uint32_t context,
                                                           std::uint32_t output, std::uint32_t texture_input) {
    try {
        const ValueType token{ValueType::record, texture_input};
        const auto& token_layout = values[texture_input];
        if (!token_layout.plain_value || !token_layout.can_own || token_layout.math ||
            token_layout.fields.size() != 1 || token_layout.fields[0].name != "slot" ||
            token_layout.fields[0].type != ValueType::u32)
            reject({}, "Material TextureInput requires its registered plain uint slot field.");
        const auto slot_offset = token_layout.fields[0].offset;
        auto tree = compile_source(name, source, {{}, {}, {}, {}, values, true, token}, "shade");
        if (!tree)
            return std::unexpected(tree.error());
        const std::string_view entries[]{"shade", "defaults"};
        auto exported = export_functions(*tree, entries, false);
        if (!exported)
            return std::unexpected(exported.error());
        const auto& shade = tree->functions[tree->exports[0]];
        const auto& defaults = tree->functions[tree->exports[1]];
        if ((shade.parameters.size() != 2 && shade.parameters.size() != 3) ||
            shade.parameters[0].type != ValueType{ValueType::record, context} ||
            shade.result != ValueType{ValueType::record, output} || shade.parameters[1].type.kind != ValueType::record)
            reject(
                shade.position,
                "Material requires Output shade(Context, Params[, Resources]), with exact registered Context/Output types.");
        const auto params = shade.parameters[1].type;
        if (context == output || params.type_index == context || params.type_index == output ||
            values[params.type_index].math)
            reject(shade.position, "Material Context, Output and Params must be distinct plain structs.");
        if (!defaults.parameters.empty() || defaults.result != params)
            reject(defaults.position, "Material requires Params defaults() with no arguments.");
        std::function<bool(ValueType)> contains_texture = [&](ValueType type) {
            if (type == token)
                return true;
            if (type.kind != ValueType::record)
                return false;
            return std::ranges::any_of(values[type.type_index].fields,
                                       [&](const auto& field) { return contains_texture(field.type); });
        };
        if (contains_texture({ValueType::record, context}) || contains_texture({ValueType::record, output}))
            reject(shade.position, "Material Context/Output cannot contain texture inputs.");
        std::vector<bool> checked(values.size());
        std::function<void(ValueType, SourcePosition)> check_type = [&](ValueType type, SourcePosition position) {
            if (type == ValueType::f32 || type == ValueType::boolean || type == ValueType::void_value || type == token)
                return;
            if (type.kind != ValueType::record)
                reject(position, "Material values require float, bool, float vectors or plain structs.");
            const auto& layout = values[type.type_index];
            if (checked[type.type_index])
                return;
            checked[type.type_index] = true;
            if (!layout.plain_value || !layout.can_own ||
                (layout.math && (!layout.math.vector() || layout.math.precision != math::Precision::f32)))
                reject(position,
                       "Material values cannot contain Runtime resources, references or unsupported math types.");
            if (layout.fields.empty())
                reject(position, "Material structs must expose their value fields.");
            for (const auto& field : layout.fields)
                check_type(field.type, position);
        };
        check_type(params, shade.position);
        std::set<std::string> keys;
        std::function<void(ValueType, std::string)> check_parameters = [&](ValueType type, std::string path) {
            if (type == ValueType::f32 || (type.kind == ValueType::record && values[type.type_index].math)) {
                keys.insert(std::move(path));
                return;
            }
            if (type.kind != ValueType::record || type == token)
                reject(shade.position,
                       "Material parameters require float scalars, vectors or nested structs, not texture inputs.");
            for (const auto& field : values[type.type_index].fields)
                check_parameters(field.type, path.empty() ? field.name : path + "." + field.name);
        };
        check_parameters(params, {});
        std::uint32_t resources = UINT32_MAX;
        std::vector<MaterialTextureInput> textures;
        if (shade.parameters.size() == 3) {
            const auto type = shade.parameters[2].type;
            if (type.kind != ValueType::record || type == token || values[type.type_index].math)
                reject(shade.position, "Material Resources must be a struct of TextureInput leaves.");
            resources = type.type_index;
            check_type(type, shade.position);
            std::function<void(ValueType, std::string, std::size_t)> collect = [&](ValueType value, std::string path,
                                                                                   std::size_t offset) {
                if (value == token) {
                    if (!keys.insert(path).second)
                        reject(shade.position, "Material numeric/resource parameter key conflict: " + path);
                    textures.push_back(
                        {std::move(path), static_cast<std::uint32_t>(textures.size()), offset + slot_offset});
                    return;
                }
                if (value.kind != ValueType::record || values[value.type_index].math)
                    reject(shade.position, "Material Resources require TextureInput leaves, not numeric parameters.");
                for (const auto& field : values[value.type_index].fields)
                    collect(field.type, path.empty() ? field.name : path + "." + field.name, offset + field.offset);
            };
            collect(type, {}, 0);
        }
        std::vector<bool> visited(tree->functions.size());
        std::function<void(std::uint32_t)> check_defaults = [&](std::uint32_t index) {
            if (visited[index])
                return;
            visited[index] = true;
            const auto& function = tree->functions[index];
            if (contains_texture(function.result) ||
                std::ranges::any_of(function.locals, [&](const auto& local) { return contains_texture(local.type); }))
                reject(function.position, "Material defaults cannot contain texture inputs.");
            for (const auto& node : function.expressions) {
                if (node.kind == Expression::Kind::texture_sample)
                    reject(node.position, "Material defaults cannot sample textures.");
                if (node.kind == Expression::Kind::texture_orientation)
                    reject(node.position, "Material defaults cannot query texture orientation.");
                if (node.kind == Expression::Kind::source_call)
                    check_defaults(node.value);
            }
        };
        check_defaults(tree->exports[1]);
        for (std::size_t index = 0; index < tree->functions.size(); ++index) {
            if (!tree->reachable[index])
                continue;
            auto& function = tree->functions[index];
            if (function.is_coroutine || function.requires_receiver)
                reject(function.position, "Material functions cannot use objects or coroutines.");
            check_type(function.result, function.position);
            for (const auto& local : function.locals)
                check_type(local.type, function.position);
            for (const auto& parameter : function.parameters)
                if (parameter.passing != ParameterPassing::value)
                    reject(function.position, "Material function parameters are values, not borrows.");
            for (const auto& node : function.statements) {
                if (node.kind == Statement::Kind::loop || node.kind == Statement::Kind::yield ||
                    node.kind == Statement::Kind::break_loop || node.kind == Statement::Kind::continue_loop)
                    reject(node.position, "Material does not support loops or suspension.");
                if (node.kind == Statement::Kind::declare && node.expression == no_node &&
                    contains_texture(function.locals[node.target].type))
                    reject(node.position, "Texture inputs cannot be default constructed; copy an injected value.");
            }
            for (auto& node : function.expressions) {
                using Kind = Expression::Kind;
                if (node.kind == Kind::value_field && function.expressions[node.left].type == token)
                    reject(node.position,
                           "TextureInput members are compiler-owned, not accessible to material Source.");
                if (!(node.kind == Kind::literal && node.type == ValueType::u32))
                    check_type(node.type, node.position);
                switch (node.kind) {
                case Kind::literal:
                case Kind::local:
                case Kind::value_field:
                case Kind::math_construct:
                case Kind::source_call:
                    break;
                case Kind::unary:
                case Kind::binary:
                    if (node.operation == Operation::remainder ||
                        (node.operation >= Operation::bitwise_and && node.operation <= Operation::shift_right))
                        reject(node.position, "Material integer arithmetic is not supported.");
                    break;
                case Kind::scalar_cast:
                    if (node.type != ValueType::f32)
                        reject(node.position, "Material conversions only produce float.");
                    break;
                case Kind::math_intrinsic:
                    if (!Hlsl::supports(node.math_operation))
                        reject(node.position, "This math operation is not in the material subset.");
                    break;
                case Kind::texture_sample:
                case Kind::texture_orientation:
                    node.offset = slot_offset;
                    break;
                default:
                    reject(node.position, "Operation is not part of the pure material subset.");
                }
            }
        }
        tree->material = true;
        return MaterialModule{std::move(*tree), context,   params.type_index,  output,
                              texture_input,    resources, std::move(textures)};
    } catch (Diagnostic& diagnostic) {
        return std::unexpected(std::move(diagnostic));
    }
}
std::string material_hlsl(const MaterialModule& material) {
    // All variants travel through the existing owned HLSL text. No extra JIT
    // callback, Runtime type or DLL ABI is needed for a GPU-only lowering.
    std::string coverage = "#error Material coverage requires a top-level float coverage field in Output.\n";
    const auto& fields = material.program.value_layouts[material.output].fields;
    const auto field = std::ranges::find_if(
        fields, [](const auto& value) { return value.name == "coverage" && value.type == ValueType::f32; });
    if (field != fields.end()) {
        MaterialModule projected;
        projected.context = material.context;
        projected.parameters = material.parameters;
        projected.output = material.output;
        projected.texture_input = material.texture_input;
        projected.resources = material.resources;
        projected.textures = material.textures;
        projected.program.value_layouts = material.program.value_layouts;
        projected.program.functions.push_back(Coverage(material).generate(field->offset));
        projected.program.reachable = {true};
        projected.program.exports = {0};
        coverage = Hlsl(projected).generate(false, true);
    }
    return "#if ATELIER_MATERIAL_COVERAGE\n" + coverage + "#elif ATELIER_MATERIAL_FOOTPRINT\n" +
           Hlsl(material).generate(true) + "#else\n" + Hlsl(material).generate() + "#endif\n";
}
}
