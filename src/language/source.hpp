#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::language {

// Compiler classifications, not a separate registry of business types.
// A predicate is internal to conditions, not a new Runtime value type.
enum class ValueType { f32, u32, predicate, void_value, strong_reference, weak_reference, null_reference };
struct FieldBinding { std::string_view name; ValueType type; std::uint32_t selector; };
struct MethodBinding { std::string_view name; ValueType parameter; std::uint32_t selector; };
struct Environment {
    std::span<const FieldBinding> fields;
    std::span<const MethodBinding> methods;
    std::string_view object_type_name{}; // exact bound Runtime type; optional for scalar-only callers
};
struct SourcePosition { std::size_t offset{}, line{1}, column{1}; };
struct Diagnostic { SourcePosition position; std::string message; };

using NodeId = std::size_t;
inline constexpr NodeId no_node = std::numeric_limits<NodeId>::max();
enum class Operation { add, subtract, multiply, positive, negate, equal, not_equal, less, less_equal, greater, greater_equal };
struct Expression {
    enum class Kind {
        literal, local, field, call, unary, binary,
        receiver, null_reference, create_reference, observe_reference, acquire_reference
    };
    Kind kind;
    ValueType type;
    SourcePosition position;
    NodeId left{no_node}, right{no_node};
    std::uint32_t value{}; // literal bits, local index, or host member selector
    Operation operation{Operation::add};
    NodeId receiver{no_node}; // field/call owner; call.left remains the argument
};
struct Statement {
    enum class Kind { block, declare, assign, call, condition, loop, return_value };
    Kind kind;
    SourcePosition position;
    NodeId target{no_node}, expression{no_node}, body{no_node}, alternative{no_node};
    std::vector<NodeId> children;
    std::vector<std::uint32_t> locals{}; // declarations directly owned by this block
};
struct Local {
    std::string name;
    ValueType type;
    // First two locals are the checked float/uint parameters; others initialize
    // at their declaration. All storage remains private to generated execution.
};
struct Program {
    std::string source_name;
    std::string function_name;
    std::vector<Local> locals;
    std::vector<Expression> expressions;
    std::vector<Statement> statements;
    NodeId body{no_node};
    std::string object_type_name;
};

// One host-bound void function with float/uint parameters and an implicit self.
// Environment names describe already validated Runtime members. No Runtime or
// LLVM object is owned here. Source/Environment storage is borrowed only during
// compilation; the returned typed tree owns everything needed for lowering.
[[nodiscard]] std::expected<Program, Diagnostic> compile_source(
    std::string_view source_name, std::string_view source, Environment environment);

}
