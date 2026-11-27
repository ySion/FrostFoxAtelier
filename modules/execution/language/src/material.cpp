#include "language/material.hpp"
#include "language/lexer.hpp"
#include "detail/material_footprint.hpp"
#include "foundation/result.hpp"
#include "memory/fatal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace atelier::language {
namespace {
using detail::reject;

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

    static constexpr std::size_t max_nodes = 65536;

    std::vector<Node> nodes_;

    std::map<std::pair<ValueType::Kind, std::uint64_t>, NodeId> literals_;

    std::vector<std::vector<std::uint64_t>> required_;

    bool overflowed_{};

    bool record(ValueType type) const {
        return type.kind == ValueType::record && type.type_index != material_.texture_input &&
               (material_.normal_texture_input == UINT32_MAX || type.type_index != material_.normal_texture_input);
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
        fatal("Missing typed coverage field.");
    }

    NodeId add(Expression expression) {
        if (overflowed_ || nodes_.size() >= max_nodes) {
            overflowed_ = true;
            return 0;
        }
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
        if (!overflowed_)
            literals_.emplace(key, id);
        return id;
    }

    NodeId boolean(bool value) { return literal(ValueType::boolean, value); }

    NodeId select(NodeId condition, NodeId yes, NodeId no) {
        if (overflowed_)
            return 0;
        if (yes == no)
            return yes;
        if (condition == boolean(true))
            return yes;
        if (condition == boolean(false))
            return no;
        if (yes == boolean(true) && no == boolean(false))
            return condition;
        if (nodes_.size() >= max_nodes) {
            overflowed_ = true;
            return 0;
        }
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

    Value unpack(ValueType type, NodeId id) {
        if (overflowed_)
            return Value(width(type), 0);
        if (!record(type))
            return {id};
        Value value;
        for (const auto& field : program_.value_layouts[type.type_index].fields) {
            Expression access{Expression::Kind::value_field, field.type, {}, id};
            access.offset = field.offset;
            const auto child = unpack(field.type, add(std::move(access)));
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
        if (overflowed_)
            return Value(width(node.type), 0);
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
            const auto left = operand(node.left);
            const auto right = operand(node.right);
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
        case Kind::conditional:
            return select(operand(node.receiver)[0], operand(node.left), operand(node.right));
        case Kind::math_construct: {
            if (node.arguments.empty())
                return initial(node.type);
            Value value;
            for (const auto child : node.arguments) {
                const auto part = operand(child);
                value.insert(value.end(), part.begin(), part.end());
            }
            if (value.size() == 1 && width(node.type) > 1) {
                const auto splat = value[0];
                value.assign(width(node.type), splat);
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
                    const auto j = (i + 1) % 3;
                    const auto k = (i + 2) % 3;
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
            return unpack(node.type, add(std::move(operation)));
        }
        case Kind::texture_sample:
        case Kind::normal_sample:
        case Kind::texture_orientation: {
            auto sample = node;
            sample.left = pack(function.expressions[node.left].type, operand(node.left));
            sample.right = operand(node.right)[0];
            return unpack(node.type, add(std::move(sample)));
        }
        default:
            fatal("Unvalidated coverage expression.");
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
        if (overflowed_)
            return;
        const auto& node = function.statements[id];
        using Kind = Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            for (const auto child : node.children) {
                if (overflowed_ || flow.returned == boolean(true))
                    break;
                statement(function, child, flow);
            }
            break;
        case Kind::declare:
            flow.locals[node.target] = node.expression == no_node ? initial(function.locals[node.target].type)
                                                                  : expression(function, node.expression, flow);
            break;
        case Kind::assign: {
            auto value = expression(function, node.expression, flow);
            if (overflowed_)
                return;
            const auto [local, begin] = target(function, node.target);
            if (node.compound) {
                const auto target_type = function.expressions[node.target].type;
                const auto target_width = width(target_type);
                const Value current(flow.locals[local].begin() + begin,
                                    flow.locals[local].begin() + begin + target_width);
                const auto scalar = record(target_type) ? ValueType{ValueType::f32} : target_type;
                Value combined;
                combined.reserve(target_width);
                for (std::size_t i = 0; i < target_width; ++i)
                    combined.push_back(binary(*node.compound, current[i], value[value.size() == 1 ? 0 : i], scalar));
                value = std::move(combined);
            }
            std::copy(value.begin(), value.end(), flow.locals[local].begin() + begin);
            break;
        }
        case Kind::call:
            break; // No observable side effects in the validated pure subset.
        case Kind::return_value: {
            const auto value = node.expression == no_node ? Value{} : expression(function, node.expression, flow);
            if (overflowed_)
                return;
            flow.result = flow.returned == no_node ? value : select(flow.returned, flow.result, value);
            flow.returned = boolean(true);
            break;
        }
        case Kind::condition: {
            const auto condition = expression(function, node.expression, flow)[0];
            if (overflowed_)
                return;
            Flow yes{flow.locals, no_node, {}};
            Flow no{flow.locals, no_node, {}};
            statement(function, node.body, yes);
            if (!overflowed_ && node.alternative != no_node)
                statement(function, node.alternative, no);
            if (overflowed_)
                return;
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
            fatal("Unvalidated coverage statement.");
        }
    }

    Value invoke(const Function& function, std::vector<Value> arguments) {
        if (overflowed_)
            return Value(width(function.result), 0);
        Flow flow{std::move(arguments), no_node, {}};
        flow.locals.resize(function.locals.size());
        statement(function, function.body, flow);
        if (overflowed_)
            return Value(width(function.result), 0);
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

    NodeId emit(NodeId id, Function& function, std::vector<Statement>& statements, std::vector<NodeId>& available) {
        if (overflowed_)
            return 0;
        if (available[id] != no_node)
            return available[id];
        if (function.expressions.size() >= max_nodes || function.statements.size() >= max_nodes) {
            overflowed_ = true;
            return 0;
        }
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

    std::optional<Function> generate(std::size_t offset) {
        const auto& shade = program_.functions[program_.exports[0]];
        std::vector<Value> arguments;
        Function result;
        result.function_name = "coverage";
        result.result = ValueType::f32;
        result.parameters = shade.parameters;
        result.locals.assign(shade.locals.begin(), shade.locals.begin() + shade.parameters.size());
        for (std::size_t i = 0; i < shade.parameters.size(); ++i)
            arguments.push_back(unpack(shade.parameters[i].type, add(Expression{Expression::Kind::local,
                                                                                shade.parameters[i].type,
                                                                                {},
                                                                                no_node,
                                                                                no_node,
                                                                                static_cast<std::uint32_t>(i)})));
        const auto value = invoke(shade, std::move(arguments));
        if (overflowed_)
            return std::nullopt;
        const auto root = value[field_begin(shade.result, offset)];
        required_.resize(nodes_.size());
        required(root);
        std::vector<NodeId> available(nodes_.size(), no_node);
        std::vector<Statement> body;
        Statement returned{Statement::Kind::return_value, {}, no_node, no_node, no_node, no_node, {}};
        returned.expression = emit(root, result, body, available);
        if (overflowed_)
            return std::nullopt;
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

    // Projects only GI-relevant surface roots (pruning normal_world, normal_texture, sample_orientation, and TBN).
    std::optional<Function> generate_gi_hit(std::span<const ValueField> gi_fields) {
        const auto& shade = program_.functions[program_.exports[0]];
        std::vector<Value> arguments;
        Function result;
        result.function_name = "gi_hit";
        result.result = shade.result;
        result.parameters = shade.parameters;
        result.locals.assign(shade.locals.begin(), shade.locals.begin() + shade.parameters.size());
        for (std::size_t i = 0; i < shade.parameters.size(); ++i)
            arguments.push_back(unpack(shade.parameters[i].type, add(Expression{Expression::Kind::local,
                                                                                shade.parameters[i].type,
                                                                                {},
                                                                                no_node,
                                                                                no_node,
                                                                                static_cast<std::uint32_t>(i)})));
        const auto value = invoke(shade, std::move(arguments));
        if (overflowed_)
            return std::nullopt;
        required_.resize(nodes_.size());
        std::vector<NodeId> lane_roots;
        for (const auto& field : gi_fields) {
            const std::size_t begin = field_begin(shade.result, field.offset);
            const std::size_t w = width(field.type);
            for (std::size_t k = 0; k < w; ++k) {
                required(value[begin + k]);
                lane_roots.push_back(value[begin + k]);
            }
        }
        std::vector<NodeId> available(nodes_.size(), no_node);
        std::vector<Statement> body;

        const auto add_expr = [&](Expression expr) -> NodeId {
            result.expressions.push_back(std::move(expr));
            return result.expressions.size() - 1;
        };
        const auto add_block = [&](std::vector<Statement> children) {
            Statement block{Statement::Kind::block, {}, no_node, no_node, no_node, no_node, {}};
            for (auto& child : children) {
                block.children.push_back(result.statements.size());
                result.statements.push_back(std::move(child));
            }
            result.statements.push_back(std::move(block));
            return result.statements.size() - 1;
        };

        // Group lane roots sharing the same top-level early-return condition so both arms share `scoped`
        // (avoiding duplicate texture fetches across `.x/.y/.z` lanes after an early return!).
        for (std::size_t idx = 0; idx < lane_roots.size() && !overflowed_; ++idx) {
            const NodeId root_id = lane_roots[idx];
            if (available[root_id] != no_node)
                continue;
            const NodeId cond_id = nodes_[root_id].condition;
            if (cond_id == no_node) {
                (void)emit(root_id, result, body, available);
                continue;
            }
            std::vector<NodeId> group;
            for (std::size_t j = idx; j < lane_roots.size(); ++j) {
                const NodeId cand = lane_roots[j];
                if (available[cand] == no_node && nodes_[cand].condition == cond_id &&
                    std::ranges::find(group, cand) == group.end()) {
                    group.push_back(cand);
                }
            }
            const NodeId cond_expr = emit(cond_id, result, body, available);
            for (const NodeId g : group) {
                const auto& yes = required_[nodes_[g].expression.left];
                const auto& no = required_[nodes_[g].expression.right];
                for (std::size_t i = std::min(yes.size(), no.size()); i-- > 0;) {
                    auto common = yes[i] & no[i];
                    while (common) {
                        const auto bit = static_cast<std::size_t>(std::bit_width(common) - 1);
                        (void)emit(i * 64 + bit, result, body, available);
                        common &= ~(std::uint64_t{1} << bit);
                    }
                }
            }
            std::vector<NodeId> refs;
            refs.reserve(group.size());
            for (const NodeId g : group) {
                const auto vtype = nodes_[g].expression.type;
                const auto local = static_cast<std::uint32_t>(result.locals.size());
                result.locals.push_back({"coverage", vtype});
                const NodeId ref = add_expr(Expression{Expression::Kind::local, vtype, {}, no_node, no_node, local});
                Statement decl{Statement::Kind::declare, {}, no_node, no_node, no_node, no_node, {}};
                decl.target = local;
                body.push_back(std::move(decl));
                refs.push_back(ref);
            }
            auto emit_shared_arm = [&](bool yes_side) {
                auto scoped = available;
                std::vector<Statement> arm_body;
                for (std::size_t g_i = 0; g_i < group.size() && !overflowed_; ++g_i) {
                    const NodeId arm_val =
                        yes_side ? nodes_[group[g_i]].expression.left : nodes_[group[g_i]].expression.right;
                    Statement assign{Statement::Kind::assign, {}, no_node, no_node, no_node, no_node, {}};
                    assign.target = refs[g_i];
                    assign.expression = emit(arm_val, result, arm_body, scoped);
                    arm_body.push_back(std::move(assign));
                }
                return add_block(std::move(arm_body));
            };
            Statement branch{Statement::Kind::condition, {}, no_node, no_node, no_node, no_node, {}};
            branch.expression = cond_expr;
            branch.body = emit_shared_arm(true);
            branch.alternative = emit_shared_arm(false);
            body.push_back(std::move(branch));
            for (std::size_t g_i = 0; g_i < group.size(); ++g_i)
                available[group[g_i]] = refs[g_i];
        }
        if (overflowed_)
            return std::nullopt;

        const auto out_local = static_cast<std::uint32_t>(result.locals.size());
        result.locals.push_back({"gi_out", shade.result});
        Statement declare_out{Statement::Kind::declare, {}, no_node, no_node, no_node, no_node, {}};
        declare_out.target = out_local;
        body.push_back(std::move(declare_out));

        for (const auto& field : gi_fields) {
            const std::size_t begin = field_begin(shade.result, field.offset);
            const std::size_t w = width(field.type);
            NodeId rhs = no_node;
            if (!record(field.type)) {
                rhs = emit(value[begin], result, body, available);
            } else {
                Expression construct{Expression::Kind::math_construct, field.type, {}};
                construct.arguments.reserve(w);
                for (std::size_t k = 0; k < w; ++k)
                    construct.arguments.push_back(emit(value[begin + k], result, body, available));
                rhs = add_expr(std::move(construct));
            }
            if (overflowed_)
                return std::nullopt;
            const NodeId out_ref =
                add_expr(Expression{Expression::Kind::local, shade.result, {}, no_node, no_node, out_local});
            Expression field_ref{Expression::Kind::value_field, field.type, {}, out_ref};
            field_ref.offset = field.offset;
            const NodeId lhs = add_expr(std::move(field_ref));

            Statement assign{Statement::Kind::assign, {}, no_node, no_node, no_node, no_node, {}};
            assign.target = lhs;
            assign.expression = rhs;
            body.push_back(std::move(assign));
        }

        const NodeId ret_ref =
            add_expr(Expression{Expression::Kind::local, shade.result, {}, no_node, no_node, out_local});
        Statement returned{Statement::Kind::return_value, {}, no_node, no_node, no_node, no_node, {}};
        returned.expression = ret_ref;
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

    bool has_chained_sample_uv_{false};

    bool detect_chained_sample_uv() const {
        const std::size_t fn_count = program_.functions.size();
        std::vector<bool> fn_may_have_sample(fn_count, false);
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t fi = 0; fi < fn_count; ++fi) {
                if (!program_.reachable[fi] || fn_may_have_sample[fi])
                    continue;
                for (const auto& e : program_.functions[fi].expressions) {
                    if (e.kind == Expression::Kind::texture_sample || e.kind == Expression::Kind::normal_sample ||
                        (e.kind == Expression::Kind::source_call && fn_may_have_sample[e.value])) {
                        fn_may_have_sample[fi] = true;
                        changed = true;
                        break;
                    }
                }
            }
        }

        for (std::size_t fi = 0; fi < fn_count; ++fi) {
            if (!program_.reachable[fi])
                continue;
            const auto& fn = program_.functions[fi];
            // Conservatively seed parameters as potentially carrying a sample if any caller passes one;
            // first check with parameters = false, then propagate across calls.
            std::vector<bool> local_sample(fn.locals.size(), false);
            for (std::size_t caller_i = 0; caller_i < fn_count; ++caller_i) {
                if (!program_.reachable[caller_i])
                    continue;
                for (const auto& ce : program_.functions[caller_i].expressions) {
                    if (ce.kind == Expression::Kind::source_call && ce.value == fi && fn_may_have_sample[caller_i]) {
                        for (std::size_t p = 0; p < fn.parameters.size(); ++p)
                            local_sample[p] = true;
                    }
                }
            }

            std::function<bool(NodeId)> expr_sample = [&](NodeId id) -> bool {
                if (id == no_node)
                    return false;
                const auto& e = fn.expressions[id];
                using Kind = Expression::Kind;
                switch (e.kind) {
                case Kind::texture_sample:
                case Kind::normal_sample:
                    return true;
                case Kind::local:
                    return local_sample[e.value];
                case Kind::source_call:
                    if (fn_may_have_sample[e.value])
                        return true;
                    return std::ranges::any_of(e.arguments, expr_sample);
                case Kind::scalar_cast:
                    return fn.expressions[e.left].type == ValueType::f32 && expr_sample(e.left);
                case Kind::texture_orientation:
                    return false;
                case Kind::math_intrinsic:
                    if (e.math_operation == MathOperation::floor || e.math_operation == MathOperation::ceil ||
                        e.math_operation == MathOperation::trunc || e.math_operation == MathOperation::round ||
                        e.math_operation == MathOperation::step) {
                        return false;
                    }
                    return std::ranges::any_of(e.arguments, expr_sample);
                default:
                    if (!continuous(e.type))
                        return false;
                    return expr_sample(e.left) || expr_sample(e.right) || std::ranges::any_of(e.arguments, expr_sample);
                }
            };

            std::function<std::uint32_t(NodeId)> target_local = [&](NodeId id) -> std::uint32_t {
                const auto& e = fn.expressions[id];
                return e.kind == Expression::Kind::local ? e.value : target_local(e.left);
            };

            std::function<void(NodeId)> walk_stmt = [&](NodeId sid) {
                if (sid == no_node)
                    return;
                const auto& st = fn.statements[sid];
                switch (st.kind) {
                case Statement::Kind::block:
                    for (const auto c : st.children)
                        walk_stmt(c);
                    break;
                case Statement::Kind::declare:
                    if (st.expression != no_node && expr_sample(st.expression))
                        local_sample[st.target] = true;
                    break;
                case Statement::Kind::assign:
                    if (expr_sample(st.expression))
                        local_sample[target_local(st.target)] = true;
                    break;
                case Statement::Kind::condition:
                    walk_stmt(st.body);
                    walk_stmt(st.alternative);
                    break;
                default:
                    break;
                }
            };
            walk_stmt(fn.body);

            for (const auto& e : fn.expressions) {
                if ((e.kind == Expression::Kind::texture_sample || e.kind == Expression::Kind::normal_sample) &&
                    expr_sample(e.left))
                    return true;
            }
        }
        return false;
    }

    bool is_texture_type(std::uint32_t type_index) const noexcept {
        return type_index == material_.texture_input ||
               (material_.normal_texture_input != UINT32_MAX && type_index == material_.normal_texture_input);
    }

    bool is_texture_value(ValueType value) const noexcept {
        return value.kind == ValueType::record && is_texture_type(value.type_index);
    }

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
            if (is_texture_type(value.type_index))
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
            fatal("Unvalidated material type.");
        }
    }

    std::string initial(ValueType value, const std::byte* bytes) const {
        if (is_texture_value(value))
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
        if (value.kind != ValueType::record || is_texture_type(value.type_index) ||
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
        if (layout.math || is_texture_type(value.type_index))
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
            fatal("Unvalidated material operation.");
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
        case MathOperation::length_squared:
            return "ms_length_squared";
        case MathOperation::distance:
            return "distance";
        case MathOperation::distance_squared:
            return "ms_distance_squared";
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
        case MathOperation::round:
            return "round";
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
        case MathOperation::cwise_mul:
            return "ms_cwise_mul";
        case MathOperation::cwise_div:
            return "ms_cwise_div";
        case MathOperation::radians:
            return "radians";
        case MathOperation::degrees:
            return "degrees";
        case MathOperation::lerp:
            return "ms_lerp";
        case MathOperation::step:
            return "step";
        default:
            return {};
        }
    }

    static bool contains_conditional(const Function& function, NodeId id) {
        if (id == no_node)
            return false;
        const auto& node = function.expressions[id];
        if (node.kind == Expression::Kind::conditional)
            return true;
        if (contains_conditional(function, node.receiver) || contains_conditional(function, node.left) ||
            contains_conditional(function, node.right))
            return true;
        return std::ranges::any_of(node.arguments, [&](NodeId arg) { return contains_conditional(function, arg); });
    }

    std::string expression(const Function& function, NodeId id, bool group_binary = true) {
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
                fatal("Missing typed material field.");
            return "(" + operand(node.left) + ")." + (layout.math ? "" : "ms_") + field->name;
        }
        case Kind::unary:
            return "(" + operation(node.operation) + operand(node.left) + ")";
        case Kind::binary: {
            if ((node.operation == Operation::logical_and || node.operation == Operation::logical_or) &&
                contains_conditional(function, node.right)) {
                const auto a = operand(node.left);
                const auto result = "ms_t" + std::to_string(temporary_++);
                text_ += "bool " + result + " = " + a + ";\n";
                text_ += "if (" + std::string(node.operation == Operation::logical_or ? "!" : "") + result + ") {\n";
                const auto b = operand(node.right);
                text_ += result + " = " + b + ";\n}\n";
                return result;
            }
            const auto lhs = operand(node.left);
            const auto rhs = operand(node.right);
            auto result = lhs + operation(node.operation) + rhs;
            const auto left = function.expressions[node.left].type;
            if (left.kind == ValueType::record &&
                (node.operation == Operation::equal || node.operation == Operation::not_equal))
                return (node.operation == Operation::equal ? "all(" : "any(") + result + ")";
            return group_binary ? "(" + result + ")" : result;
        }
        case Kind::conditional: {
            const auto cond = operand(node.receiver);
            const auto result = "ms_t" + std::to_string(temporary_++);
            text_ += type(node.type) + " " + result + ";\n";
            text_ += "if (" + cond + ") {\n";
            const auto yes = operand(node.left);
            text_ += result + " = " + yes + ";\n} else {\n";
            const auto no = operand(node.right);
            text_ += result + " = " + no + ";\n}\n";
            return result;
        }
        case Kind::texture_sample:
            return "::atelier_material_sample(ms_environment," + operand(node.right) + "," + operand(node.left) + ")";
        case Kind::normal_sample:
            return "::atelier_material_sample_normal(ms_environment," + operand(node.right) + "," +
                   operand(node.left) + ")";
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
            if (node.kind == Kind::math_construct && node.arguments.size() == 1 &&
                function.expressions[node.arguments[0]].type == ValueType::f32)
                return "((" + type(node.type) + ")(" + operand(node.arguments[0]) + "))";
            std::string result = node.kind == Kind::source_call      ? "ms_f" + std::to_string(node.value)
                                 : node.kind == Kind::math_construct ? type(node.type)
                                                                     : intrinsic(node.math_operation);
            result += "(";
            if (node.kind == Kind::source_call)
                result += "ms_environment";
            const bool vector_intrinsic = node.kind == Kind::math_intrinsic && node.type.kind == ValueType::record &&
                                          program_.value_layouts[node.type.type_index].math;
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                if (i || node.kind == Kind::source_call)
                    result += ",";
                const auto arg_id = node.arguments[i];
                if (vector_intrinsic && function.expressions[arg_id].type == ValueType::f32)
                    result += "((" + type(node.type) + ")(" + operand(arg_id) + "))";
                else
                    result += operand(arg_id);
            }
            return result + ")";
        }
        default:
            fatal("Unvalidated material expression.");
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
            const auto value =
                node.expression == no_node ? initial(local.type, nullptr) : expression(function, node.expression);
            text_ += type(local.type) + " ms_v" + std::to_string(node.target) + " = " + value + ";\n";
            break;
        }
        case Kind::assign: {
            const auto target_str = expression(function, node.target);
            const auto rhs_str = expression(function, node.expression);
            if (node.compound)
                text_ += target_str + " = (" + target_str + operation(*node.compound) + rhs_str + ");\n";
            else
                text_ += target_str + " = " + rhs_str + ";\n";
            break;
        }
        case Kind::call: {
            const auto expr_str = expression(function, node.expression);
            text_ += expr_str + ";\n";
            break;
        }
        case Kind::return_value: {
            const auto ret_str =
                node.expression == no_node ? std::string{} : " " + expression(function, node.expression);
            text_ += "return" + ret_str + ";\n";
            break;
        }
        case Kind::condition: {
            const auto cond_str = expression(function, node.expression, false);
            text_ += "if (" + cond_str + ") {\n";
            statement(function, node.body);
            text_ += "}\n";
            if (node.alternative != no_node) {
                text_ += "else {\n";
                statement(function, node.alternative);
                text_ += "}\n";
            }
            break;
        }
        default:
            fatal("Unvalidated material statement.");
        }
    }

    bool continuous(ValueType value) const {
        return value.kind == ValueType::f32 || (value.kind == ValueType::record && !is_texture_type(value.type_index));
    }

    std::string differential_type(ValueType value) const { return (continuous(value) ? "ms_D" : "") + type(value); }

    std::string field_name(const Function& function, const Expression& node) const {
        const auto& layout = program_.value_layouts[function.expressions[node.left].type.type_index];
        const auto field =
            std::ranges::find_if(layout.fields, [&](const auto& value) { return value.offset == node.offset; });
        if (field == layout.fields.end())
            fatal("Missing typed material field.");
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
            if (emitted_[i] && !program_.value_layouts[i].math && !is_texture_type(static_cast<std::uint32_t>(i)))
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
            const std::string a(value);
            const std::string c(1, component);
            return "ms_seed(" + a + ".v." + c + "," + a + ".d0." + c + "," + a + ".d1." + c + ")";
        };
        struct Element {
            const char* name;

            std::size_t arity;
        };
        constexpr Element elements[]{
            {"add", 2},  {"sub", 2},   {"mul", 2},  {"div", 2},     {"neg", 1},     {"sin", 1},   {"cos", 1},
            {"sqrt", 1}, {"floor", 1}, {"ceil", 1}, {"trunc", 1},   {"round", 1},   {"fract", 1}, {"abs", 1},
            {"min", 2},  {"max", 2},   {"step", 2}, {"radians", 1}, {"degrees", 1}, {"clamp", 3}, {"lerp", 3}};
        constexpr std::string_view components = "xyzw";
        for (std::size_t n = 2; n <= 4; ++n) {
            const auto value = "float" + std::to_string(n);
            const auto d = "ms_D" + value;
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
            text_ += "ms_Dfloat ms_dlength_squared(" + d + " a) { return ms_ddot(a,a); }\n";
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
            text_ += "ms_Dfloat ms_ddistance(" + d + " a," + d + " b) { return ms_dlength(ms_dsub(b,a)); }\n";
            text_ += "ms_Dfloat ms_ddistance_squared(" + d + " a," + d + " b) { " + d +
                     " diff=ms_dsub(b,a); return ms_ddot(diff,diff); }\n";
        }
        text_ += "ms_Dfloat3 ms_dcross(ms_Dfloat3 a,ms_Dfloat3 b) { ms_Dfloat3 r;\n";
        for (std::size_t i = 0; i < 3; ++i) {
            const auto j = components[(i + 1) % 3];
            const auto k = components[(i + 2) % 3];
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
            const auto owner = operand(node.left);
            const auto field = field_name(function, node);
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
        case Kind::conditional: {
            const auto cond = operand(node.receiver);
            const auto result = "ms_t" + std::to_string(temporary_++);
            text_ += differential_type(node.type) + " " + result + ";\n";
            text_ += "if (" + cond + ") {\n";
            const auto yes = operand(node.left);
            text_ += result + "=" + yes + ";\n} else {\n";
            const auto no = operand(node.right);
            text_ += result + "=" + no + ";\n}\n";
            return result;
        }
        case Kind::scalar_cast: {
            const auto value = operand(node.left);
            return function.expressions[node.left].type == ValueType::f32
                       ? value
                       : emit("ms_constant(float(ms_value(" + value + ")))");
        }
        case Kind::texture_sample:
        case Kind::normal_sample:
        case Kind::texture_orientation: {
            const auto input = operand(node.right); // Same author argument order as CPU sample(input,uv).
            const auto uv = operand(node.left);
            if (node.kind == Kind::texture_orientation)
                return emit("ms_constant(::atelier_material_sample_orientation(ms_environment," + input + ",ms_value(" +
                            uv + ")))");
            if (node.kind == Kind::normal_sample)
                return emit(std::string(has_chained_sample_uv_ ? "ms_dsample_normal(" : "ms_dsample_normal_value(") +
                            "ms_environment," + input + "," + uv + ")");
            return emit(std::string(has_chained_sample_uv_ ? "ms_dsample(" : "ms_dsample_value(") + "ms_environment," +
                        input + "," + uv + ")");
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
                const bool splat_scalar =
                    arguments.size() == 1 && function.expressions[node.arguments[0]].type == ValueType::f32;
                std::string result = "ms_seed(";
                for (const auto* part : {"v", "d0", "d1"}) {
                    if (part != std::string_view("v"))
                        result += ",";
                    if (splat_scalar) {
                        result += "(" + type(node.type) + ")(" + arguments[0] + "." + part + ")";
                    } else {
                        result += type(node.type) + "(";
                        for (std::size_t i = 0; i < arguments.size(); ++i) {
                            if (i)
                                result += ",";
                            result += arguments[i] + "." + part;
                        }
                        result += ")";
                    }
                }
                return emit(result + ")");
            }
            auto name = node.math_operation == MathOperation::cwise_mul   ? std::string("mul")
                        : node.math_operation == MathOperation::cwise_div ? std::string("div")
                                                                          : intrinsic(node.math_operation);
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
            fatal("Unvalidated material footprint expression.");
        }
    }

    std::string differential_target(const Function& function, NodeId id, std::string_view part) const {
        const auto& node = function.expressions[id];
        if (node.kind == Expression::Kind::local)
            return "ms_v" + std::to_string(node.value) + (continuous(node.type) ? "." + std::string(part) : "");
        if (node.kind == Expression::Kind::value_field)
            return differential_target(function, node.left, part) + "." + field_name(function, node);
        fatal("Unvalidated material footprint assignment target.");
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
            auto value = differential_expression(function, node.expression);
            if (node.compound) {
                const auto lhs = differential_expression(function, node.target);
                const auto target_type = function.expressions[node.target].type;
                const auto rhs_type = function.expressions[node.expression].type;
                const char* op_name = *node.compound == Operation::add        ? "add"
                                      : *node.compound == Operation::subtract ? "sub"
                                      : *node.compound == Operation::multiply ? "mul"
                                                                              : "div";
                const auto rhs_cast = (rhs_type == ValueType::f32 && target_type.kind == ValueType::record &&
                                       program_.value_layouts[target_type.type_index].math)
                                          ? "ms_seed((" + type(target_type) + ")" + value + ".v,(" + type(target_type) +
                                                ")" + value + ".d0,(" + type(target_type) + ")" + value + ".d1)"
                                          : value;
                const auto combined = "ms_t" + std::to_string(temporary_++);
                text_ += differential_type(target_type) + " " + combined + "=ms_d" + op_name + "(" + lhs + "," +
                         rhs_cast + ");\n";
                value = combined;
            }
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
            fatal("Unvalidated material footprint statement.");
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

    enum class SliceMode : std::uint8_t { shade, footprint, coverage, gi_hit };

    std::string generate(SliceMode mode = SliceMode::shade) {
        const bool footprint = (mode == SliceMode::footprint);
        const bool coverage = (mode == SliceMode::coverage);
        const bool gi_hit = (mode == SliceMode::gi_hit);
        has_chained_sample_uv_ = footprint && detect_chained_sample_uv();
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
                         operation != MathOperation::fract && operation != MathOperation::cwise_mul &&
                         operation != MathOperation::cwise_div && operation != MathOperation::length_squared &&
                         operation != MathOperation::distance_squared))
                        continue;
                    const bool vector_reduction =
                        operation == MathOperation::length_squared || operation == MathOperation::distance_squared;
                    const auto value =
                        vector_reduction ? type(function.expressions[node.arguments[0]].type) : type(node.type);
                    if (!helpers.emplace(operation, value).second)
                        continue;
                    // Typed value arguments evaluate each operand once. These Source
                    // contracts differ from HLSL's built-ins; precise prevents DXC
                    // from reassociating lerp or folding subtraction back into frac.
                    if (operation == MathOperation::length_squared) {
                        text_ += "float ms_length_squared(" + value +
                                 " a) { precise float result = dot(a,a); return result; }\n";
                        continue;
                    }
                    if (operation == MathOperation::distance_squared) {
                        text_ += "float ms_distance_squared(" + value + " a," + value + " b) { precise " + value +
                                 " d = b - a; precise float result = dot(d,d); return result; }\n";
                        continue;
                    }
                    text_ += value + " " + intrinsic(operation) + "(" + value + " a";
                    if (operation == MathOperation::fract) {
                        text_ += ") { precise " + value +
                                 " result = a - floor(a); return min(result,asfloat(0x3f7fffffu)); }\n";
                    } else if (operation == MathOperation::cwise_mul || operation == MathOperation::cwise_div) {
                        text_ += "," + value + " b) { precise " + value + " result = a " +
                                 (operation == MathOperation::cwise_mul ? "*" : "/") + " b; return result; }\n";
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
        if (gi_hit) {
            text_ += "struct ms_GiHit {\n"
                     "float3 ms_effective_albedo;\n"
                     "float ms_roughness;\n"
                     "float3 ms_emissive;\n"
                     "float ms_metallic;\n"
                     "float3 ms_transmission;\n"
                     "};\n"
                     "ms_GiHit ms_shade_gi_hit(uint environment, ms_Context context, ms_Parameters parameters) {\n";
        } else {
            text_ +=
                coverage ? "float ms_coverage(uint environment, ms_Context context, ms_Parameters parameters) {\n"
                : footprint
                    ? "ms_Output ms_shade_footprint(uint environment, ms_Context context, ms_Context d0, ms_Context d1, ms_Parameters parameters) {\n"
                    : "ms_Output ms_shade(uint environment, ms_Context context, ms_Parameters parameters) {\n";
        }
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
        if (gi_hit) {
            text_ += "ms_Output raw = ms_f" + std::to_string(program_.exports[0]) + "(environment,context,parameters";
            if (material_.resources != UINT32_MAX)
                text_ += ",resources";
            text_ += ");\n";
            const auto& out_fields = program_.value_layouts[material_.output].fields;
            const auto has_out_field = [&](std::string_view fname) {
                return std::ranges::any_of(out_fields, [&](const ValueField& f) { return f.name == fname; });
            };
            text_ += "ms_GiHit hit = (ms_GiHit)0;\n";
            text_ += "float3 base = " +
                     std::string(has_out_field("base_color") ? "saturate(raw.ms_base_color)" : "float3(1.0,1.0,1.0)") +
                     ";\n";
            text_ +=
                "float rough = " + std::string(has_out_field("roughness") ? "saturate(raw.ms_roughness)" : "0.65") +
                ";\n";
            text_ +=
                "float metal = " + std::string(has_out_field("metallic") ? "saturate(raw.ms_metallic)" : "0.0") + ";\n";
            text_ += "float spec_w = " +
                     std::string(has_out_field("specular_weight") ? "saturate(raw.ms_specular_weight)" : "1.0") + ";\n";
            text_ += "float spec_ior = " +
                     std::string(has_out_field("specular_ior") ? "clamp(raw.ms_specular_ior, 1.0, 3.0)" : "1.5") +
                     ";\n";
            text_ += "float f0_d = (spec_ior - 1.0) / (spec_ior + 1.0);\n";
            text_ += "f0_d = f0_d * f0_d * spec_w;\n";
            // OpenPBR v1.1 §3.10 directional-hemispherical albedo folding across dielectric/conductor/coat/fuzz/foliage:
            text_ += "float e_diel = saturate(1.0 - f0_d * (1.125 - 0.25 * rough));\n";
            text_ += "float3 cond_col = base" +
                     std::string(has_out_field("specular_color")
                                     ? " * lerp(float3(1.0,1.0,1.0), saturate(raw.ms_specular_color), 0.15)"
                                     : "") +
                     ";\n";
            if (has_out_field("subsurface_weight") && has_out_field("subsurface_color")) {
                text_ += "float ssw = saturate(raw.ms_subsurface_weight) * (1.0 - metal) * 0.35;\n";
                text_ += "base = lerp(base, saturate(raw.ms_subsurface_color), ssw);\n";
            }
            text_ += "float3 rho = lerp(base * e_diel, cond_col * (0.92 + 0.08 * rough), metal);\n";
            if (has_out_field("thin_film_weight")) {
                text_ += "rho *= lerp(1.0, 0.94, saturate(raw.ms_thin_film_weight) * (1.0 - metal));\n";
            }
            if (has_out_field("coat_weight") && has_out_field("coat_darkening")) {
                text_ += "float cw = saturate(raw.ms_coat_weight);\n";
                text_ += "rho *= lerp(1.0, lerp(1.0, 0.82, saturate(raw.ms_coat_darkening)), cw);\n";
            }
            if (has_out_field("fuzz_weight") && has_out_field("fuzz_color")) {
                text_ += "float fw = saturate(raw.ms_fuzz_weight) * 0.25;\n";
                text_ += "rho = lerp(rho, saturate(raw.ms_fuzz_color), fw);\n";
            }
            if (has_out_field("transmission_scatter") && has_out_field("transmission_color")) {
                text_ += "float ts = saturate(raw.ms_transmission_scatter) * (1.0 - metal);\n";
                text_ += "hit.ms_transmission = rho * saturate(raw.ms_transmission_color) * (0.5 * ts);\n";
                text_ += "rho *= (1.0 - 0.5 * ts);\n";
            }
            text_ += "hit.ms_effective_albedo = rho;\n";
            text_ +=
                "hit.ms_emissive = " +
                std::string(
                    has_out_field("emissive")
                        ? "((all((asuint(raw.ms_emissive) & 0x7f800000u) != 0x7f800000u)) ? max(raw.ms_emissive, 0.0) : float3(0.0,0.0,0.0))"
                        : "float3(0.0,0.0,0.0)") +
                ";\n";
            text_ += "hit.ms_roughness = rough;\n";
            text_ += "hit.ms_metallic = metal;\n";
            text_ += "return hit;\n}\n";
        } else {
            text_ += "return " + std::string(footprint ? "ms_value(" : "") + "ms_f" +
                     std::to_string(program_.exports[0]) +
                     (footprint ? "(environment,ms_seed(context,d0,d1),ms_constant(parameters)"
                                : "(environment,context,parameters");
            if (material_.resources != UINT32_MAX)
                text_ += footprint ? ",ms_constant(resources)" : ",resources";
            text_ += footprint ? ")); }\n" : "); }\n";
        }
        if (!coverage && !gi_hit)
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
                                                           std::uint32_t output, std::uint32_t texture_input,
                                                           std::uint32_t normal_texture_input) {
    const ValueType token{ValueType::record, texture_input};
    const auto& token_layout = values[texture_input];
    if (!token_layout.plain_value || !token_layout.can_own || token_layout.math || token_layout.fields.size() != 1 ||
        token_layout.fields[0].name != "slot" || token_layout.fields[0].type != ValueType::u32)
        return reject({}, "Material TextureInput requires its registered plain uint slot field.");
    const auto slot_offset = token_layout.fields[0].offset;

    ValueType normal_token{};
    std::size_t normal_slot_offset = slot_offset;
    if (normal_texture_input != UINT32_MAX) {
        if (normal_texture_input == texture_input)
            return reject({}, "Material NormalTextureInput must be distinct from TextureInput.");
        normal_token = {ValueType::record, normal_texture_input};
        const auto& n_layout = values[normal_texture_input];
        if (!n_layout.plain_value || !n_layout.can_own || n_layout.math || n_layout.fields.size() != 1 ||
            n_layout.fields[0].name != "slot" || n_layout.fields[0].type != ValueType::u32)
            return reject({}, "Material NormalTextureInput requires its registered plain uint slot field.");
        normal_slot_offset = n_layout.fields[0].offset;
    }
    const auto is_any_token = [&](ValueType t) noexcept {
        return t == token || (normal_texture_input != UINT32_MAX && t == normal_token);
    };

    ATELIER_TRY_BIND(tree, compile_source(name, source, {{}, {}, {}, {}, values, true, token, normal_token}, "shade"));
    const std::string_view entries[]{"shade", "defaults"};
    ATELIER_TRY(export_functions(*tree, entries, false));
    const auto& shade = tree->functions[tree->exports[0]];
    const auto& defaults = tree->functions[tree->exports[1]];
    if ((shade.parameters.size() != 2 && shade.parameters.size() != 3) ||
        shade.parameters[0].type != ValueType{ValueType::record, context} ||
        shade.result != ValueType{ValueType::record, output} || shade.parameters[1].type.kind != ValueType::record)
        return reject(
            shade.position,
            "Material requires Output shade(Context, Params[, Resources]), with exact registered Context/Output types.");
    const auto params = shade.parameters[1].type;
    if (context == output || params.type_index == context || params.type_index == output ||
        values[params.type_index].math)
        return reject(shade.position, "Material Context, Output and Params must be distinct plain structs.");
    if (!defaults.parameters.empty() || defaults.result != params)
        return reject(defaults.position, "Material requires Params defaults() with no arguments.");
    std::function<bool(ValueType)> contains_texture = [&](ValueType type) {
        if (is_any_token(type))
            return true;
        if (type.kind != ValueType::record)
            return false;
        return std::ranges::any_of(values[type.type_index].fields,
                                   [&](const auto& field) { return contains_texture(field.type); });
    };
    if (contains_texture({ValueType::record, context}) || contains_texture({ValueType::record, output}))
        return reject(shade.position, "Material Context/Output cannot contain texture inputs.");
    std::vector<bool> checked(values.size());
    std::function<std::expected<void, Diagnostic>(ValueType, SourcePosition)> check_type =
        [&](ValueType type, SourcePosition position) -> std::expected<void, Diagnostic> {
        if (type == ValueType::f32 || type == ValueType::boolean || type == ValueType::void_value || is_any_token(type))
            return {};
        if (type.kind != ValueType::record)
            return reject(position, "Material values require float, bool, float vectors or plain structs.");
        const auto& layout = values[type.type_index];
        if (checked[type.type_index])
            return {};
        checked[type.type_index] = true;
        if (!layout.plain_value || !layout.can_own ||
            (layout.math && (!layout.math.vector() || layout.math.precision != math::Precision::f32)))
            return reject(position,
                          "Material values cannot contain Runtime resources, references or unsupported math types.");
        if (layout.fields.empty())
            return reject(position, "Material structs must expose their value fields.");
        for (const auto& field : layout.fields) {
            ATELIER_TRY(check_type(field.type, position));
        }
        return {};
    };
    ATELIER_TRY(check_type(params, shade.position));
    std::set<std::string> keys;
    std::function<std::expected<void, Diagnostic>(ValueType, std::string)> check_parameters =
        [&](ValueType type, std::string path) -> std::expected<void, Diagnostic> {
        if (type == ValueType::f32 || (type.kind == ValueType::record && values[type.type_index].math)) {
            keys.insert(std::move(path));
            return {};
        }
        if (type.kind != ValueType::record || is_any_token(type))
            return reject(shade.position,
                          "Material parameters require float scalars, vectors or nested structs, not texture inputs.");
        for (const auto& field : values[type.type_index].fields) {
            ATELIER_TRY(check_parameters(field.type, path.empty() ? field.name : path + "." + field.name));
        }
        return {};
    };
    ATELIER_TRY(check_parameters(params, {}));
    std::uint32_t resources = UINT32_MAX;
    std::vector<MaterialTextureInput> textures;
    if (shade.parameters.size() == 3) {
        const auto type = shade.parameters[2].type;
        if (type.kind != ValueType::record || is_any_token(type) || values[type.type_index].math)
            return reject(shade.position,
                          "Material Resources must be a struct of TextureInput/NormalTextureInput leaves.");
        resources = type.type_index;
        ATELIER_TRY(check_type(type, shade.position));
        std::function<std::expected<void, Diagnostic>(ValueType, std::string, std::size_t)> collect =
            [&](ValueType value, std::string path, std::size_t offset) -> std::expected<void, Diagnostic> {
            if (is_any_token(value)) {
                if (!keys.insert(path).second)
                    return reject(shade.position, "Material numeric/resource parameter key conflict: " + path);
                const bool is_norm = (normal_texture_input != UINT32_MAX && value == normal_token);
                textures.push_back({std::move(path), static_cast<std::uint32_t>(textures.size()),
                                    offset + (is_norm ? normal_slot_offset : slot_offset), is_norm});
                return {};
            }
            if (value.kind != ValueType::record || values[value.type_index].math)
                return reject(shade.position,
                              "Material Resources require TextureInput or NormalTextureInput leaves, not numeric parameters.");
            for (const auto& field : values[value.type_index].fields) {
                ATELIER_TRY(
                    collect(field.type, path.empty() ? field.name : path + "." + field.name, offset + field.offset));
            }
            return {};
        };
        ATELIER_TRY(collect(type, {}, 0));
    }
    std::vector<bool> visited(tree->functions.size());
    std::function<std::expected<void, Diagnostic>(std::uint32_t)> check_defaults =
        [&](std::uint32_t index) -> std::expected<void, Diagnostic> {
        if (visited[index])
            return {};
        visited[index] = true;
        const auto& function = tree->functions[index];
        if (contains_texture(function.result) ||
            std::ranges::any_of(function.locals, [&](const auto& local) { return contains_texture(local.type); }))
            return reject(function.position, "Material defaults cannot contain texture inputs.");
        for (const auto& node : function.expressions) {
            if (node.kind == Expression::Kind::texture_sample || node.kind == Expression::Kind::normal_sample)
                return reject(node.position, "Material defaults cannot sample textures.");
            if (node.kind == Expression::Kind::texture_orientation)
                return reject(node.position, "Material defaults cannot query texture orientation.");
            if (node.kind == Expression::Kind::source_call) {
                ATELIER_TRY(check_defaults(node.value));
            }
        }
        return {};
    };
    ATELIER_TRY(check_defaults(tree->exports[1]));
    for (std::size_t index = 0; index < tree->functions.size(); ++index) {
        if (!tree->reachable[index])
            continue;
        auto& function = tree->functions[index];
        if (function.is_coroutine || function.requires_receiver)
            return reject(function.position, "Material functions cannot use objects or coroutines.");
        ATELIER_TRY(check_type(function.result, function.position));
        for (const auto& local : function.locals) {
            ATELIER_TRY(check_type(local.type, function.position));
        }
        for (const auto& parameter : function.parameters)
            if (parameter.passing != ParameterPassing::value)
                return reject(function.position, "Material function parameters are values, not borrows.");
        for (const auto& node : function.statements) {
            if (node.kind == Statement::Kind::loop || node.kind == Statement::Kind::parallel_loop ||
                node.kind == Statement::Kind::fail || node.kind == Statement::Kind::yield ||
                node.kind == Statement::Kind::break_loop || node.kind == Statement::Kind::continue_loop)
                return reject(node.position, "Material does not support loops, explicit fail or suspension.");
            if (node.kind == Statement::Kind::declare && node.expression == no_node &&
                contains_texture(function.locals[node.target].type))
                return reject(node.position, "Texture inputs cannot be default constructed; copy an injected value.");
        }
        for (auto& node : function.expressions) {
            using Kind = Expression::Kind;
            if (node.kind == Kind::value_field && is_any_token(function.expressions[node.left].type))
                return reject(node.position,
                              "TextureInput members are compiler-owned, not accessible to material Source.");
            if (!(node.kind == Kind::literal && node.type == ValueType::u32)) {
                ATELIER_TRY(check_type(node.type, node.position));
            }
            switch (node.kind) {
            case Kind::literal:
            case Kind::local:
            case Kind::value_field:
            case Kind::math_construct:
            case Kind::source_call:
            case Kind::conditional:
                break;
            case Kind::unary:
            case Kind::binary:
                if (node.operation == Operation::remainder ||
                    (node.operation >= Operation::bitwise_and && node.operation <= Operation::shift_right))
                    return reject(node.position, "Material integer arithmetic is not supported.");
                break;
            case Kind::scalar_cast:
                if (node.type != ValueType::f32)
                    return reject(node.position, "Material conversions only produce float.");
                break;
            case Kind::math_intrinsic:
                if (!Hlsl::supports(node.math_operation))
                    return reject(node.position, "This math operation is not in the material subset.");
                break;
            case Kind::texture_sample:
            case Kind::normal_sample:
            case Kind::texture_orientation: {
                const auto in_type = function.expressions[node.right].type;
                node.offset = (normal_texture_input != UINT32_MAX && in_type == normal_token) ? normal_slot_offset
                                                                                              : slot_offset;
                break;
            }
            case Kind::constant_data:
                return reject(node.position, "Material constants require scalar or float vector types.");
            default:
                return reject(node.position, "Operation is not part of the pure material subset.");
            }
        }
    }
    tree->material = true;
    return MaterialModule{std::move(*tree),   context,   params.type_index,  output,
                          texture_input,      normal_texture_input, resources, std::move(textures)};
}

MaterialHlsl material_hlsl(const MaterialModule& material) {
    MaterialHlsl result{Hlsl(material).generate(Hlsl::SliceMode::shade),
                        Hlsl(material).generate(Hlsl::SliceMode::footprint),
                        {},
                        {},
                        MaterialCoverageStatus::missing_field,
                        MaterialCoverageStatus::missing_field};
    const auto& fields = material.program.value_layouts[material.output].fields;

    // 1. Pruned scalar coverage slice for Masked VisBuffer PSOs:
    const auto cov_field = std::ranges::find_if(
        fields, [](const auto& value) { return value.name == "coverage" && value.type == ValueType::f32; });
    if (cov_field != fields.end()) {
        auto generated = Coverage(material).generate(cov_field->offset);
        if (!generated) {
            result.coverage_status = MaterialCoverageStatus::node_budget_exceeded;
        } else {
            MaterialModule projected;
            projected.context = material.context;
            projected.parameters = material.parameters;
            projected.output = material.output;
            projected.texture_input = material.texture_input;
            projected.normal_texture_input = material.normal_texture_input;
            projected.resources = material.resources;
            projected.textures = material.textures;
            projected.program.value_layouts = material.program.value_layouts;
            projected.program.functions.push_back(std::move(*generated));
            projected.program.reachable = {true};
            projected.program.exports = {0};
            result.coverage = Hlsl(projected).generate(Hlsl::SliceMode::coverage);
            result.coverage_status = MaterialCoverageStatus::available;
        }
    }

    // 2. Pruned GI hit slice (excludes coverage & normal_world/normal_texture/TBN, folds OpenPBR §3.10 effective albedo):
    constexpr std::string_view gi_names[] = {
        "base_color",         "emissive",         "roughness",         "metallic",
        "specular_weight",    "specular_ior",     "specular_color",    "coat_weight",
        "coat_darkening",     "fuzz_weight",      "fuzz_color",        "transmission_scatter",
        "transmission_color", "thin_film_weight", "subsurface_weight", "subsurface_color"};
    std::vector<ValueField> gi_fields;
    for (const auto& f : fields) {
        if (std::ranges::find(gi_names, f.name) != std::end(gi_names))
            gi_fields.push_back(f);
    }
    if (!gi_fields.empty()) {
        if (auto gi_fn = Coverage(material).generate_gi_hit(gi_fields)) {
            MaterialModule projected;
            projected.context = material.context;
            projected.parameters = material.parameters;
            projected.output = material.output;
            projected.texture_input = material.texture_input;
            projected.normal_texture_input = material.normal_texture_input;
            projected.resources = material.resources;
            projected.textures = material.textures;
            projected.program.value_layouts = material.program.value_layouts;
            projected.program.functions.push_back(std::move(*gi_fn));
            projected.program.reachable = {true};
            projected.program.exports = {0};
            result.gi_hit = Hlsl(projected).generate(Hlsl::SliceMode::gi_hit);
        } else {
            result.gi_hit = Hlsl(material).generate(Hlsl::SliceMode::gi_hit);
        }
        result.gi_hit_status = MaterialCoverageStatus::available;
    }
    return result;
}
}
