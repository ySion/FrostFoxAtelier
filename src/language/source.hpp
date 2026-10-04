#pragma once
#include "math/type.hpp"
#include "runtime/parameter.hpp"

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
struct ValueType {
    enum Kind { f32, u32, i32, boolean, void_value, strong_reference, weak_reference, null_reference, enumeration, record, array, string, f64 };
    Kind kind{void_value};
    std::uint32_t type_index{}; // index in the enum or value catalog, selected by kind
    constexpr ValueType(Kind value = void_value, std::uint32_t index = 0) : kind(value), type_index(index) {}
    bool operator==(const ValueType&) const = default;
};
struct ParameterType { ValueType type; ParameterPassing passing{ParameterPassing::value}; };
struct EnumItem { std::string_view name; std::uint32_t bits; };
struct EnumBinding { std::string_view name; ValueType underlying; std::span<const EnumItem> values; };
struct FieldBinding { std::string_view name; ValueType type; };
struct ValueField { std::string name; ValueType type; std::size_t offset; };
struct ValueLayout {
    std::string name;
    ValueType::Kind kind;
    std::size_t size, alignment;
    ValueType element;
    std::vector<ValueField> fields;
    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept{};
    bool can_own{}, can_mutate{};
    math::Type math{};
};
inline bool aggregate(ValueType type) noexcept {
    return type.kind == ValueType::record || type.kind == ValueType::array || type.kind == ValueType::string;
}
struct MethodBinding {
    std::string_view name;
    std::span<const ParameterType> parameters;
    ValueType result{ValueType::void_value};
    bool receiver_read_only{};
    bool preserves_borrows{};
};
struct Environment {
    std::span<const FieldBinding> fields;
    std::span<const MethodBinding> methods;
    std::string_view object_type_name{}; // exact bound Runtime type; optional for callers without object access
    std::span<const EnumBinding> enumerations{};
    std::span<const ValueLayout> values{};
};
struct SourcePosition { std::size_t offset{}, line{1}, column{1}; };
struct Diagnostic { SourcePosition position; std::string message; };

using NodeId = std::size_t;
inline constexpr NodeId no_node = std::numeric_limits<NodeId>::max();
enum class Operation {
    add, subtract, multiply, positive, negate, equal, not_equal, less, less_equal, greater, greater_equal, divide,
    remainder, bitwise_and, bitwise_or, bitwise_xor, bitwise_not, shift_left, shift_right,
    logical_not, logical_and, logical_or
};
enum class MathOperation { dot, cross, length, normalize_or_zero, conjugate, rotate, transpose, transform_point, transform_direction, square_root };
struct Expression {
    enum class Kind {
        literal, local, field, call, unary, binary,
        receiver, null_reference, create_reference, observe_reference, acquire_reference,
        value_field, index, length, string_literal, sequence_mutation, source_call,
        math_construct, math_intrinsic, scalar_cast
    };
    Kind kind;
    ValueType type;
    SourcePosition position;
    NodeId left{no_node}, right{no_node};
    std::uint32_t value{}; // local index or host catalog index
    Operation operation{Operation::add};
    NodeId receiver{no_node}; // field/call owner
    std::vector<ParameterPassing> argument_passing{};
    std::vector<NodeId> arguments{}; // call operands, evaluated left to right
    std::size_t offset{};
    SequenceMutation mutation{};
    std::uint64_t bits{}; // literal payload, including exact double representation
    MathOperation math_operation{MathOperation::dot};
    bool typed_constant{}; // module constant substitutions retain their declared type
};
struct Statement {
    enum class Kind { block, declare, assign, call, condition, loop, return_value, yield, break_loop, continue_loop };
    Kind kind;
    SourcePosition position;
    NodeId target{no_node}, expression{no_node}, body{no_node}, alternative{no_node};
    std::vector<NodeId> children;
    std::vector<std::uint32_t> locals{}; // declarations directly owned by this block
    NodeId step{no_node}; // optional loop step, also executed after continue
};
struct Local {
    std::string name;
    ValueType type;
    bool read_only{};
    // The first Function::parameters.size() locals are parameters;
    // all remaining locals initialize at their declaration.
};
struct Function {
    SourcePosition position;
    std::string function_name;
    std::vector<ParameterType> parameters;
    ValueType result{ValueType::void_value};
    std::vector<Local> locals;
    std::vector<Expression> expressions;
    std::vector<Statement> statements;
    NodeId body{no_node};
    bool is_coroutine{};
    bool uses_scoped_values{};
    bool requires_receiver{};
    bool preserves_borrows{};
    std::vector<std::string> strings;
};
struct Program {
    std::string source_name;
    std::string object_type_name;
    std::vector<ValueLayout> value_layouts;
    std::vector<Function> functions;
    std::uint32_t entry{};
    std::vector<bool> reachable;
};

// One module with a host-selected entry: scalar/enum/math values or borrowed value
// parameters, an optional scalar/math result and implicit self. Async cannot borrow
// caller storage or expose aggregate parameters/results.
// Environment names describe already validated Runtime members. No Runtime or
// LLVM object is owned here. Source/Environment storage is borrowed only during
// compilation; the returned typed tree owns everything needed for lowering.
[[nodiscard]] std::expected<Program, Diagnostic> compile_source(
    std::string_view source_name, std::string_view source, Environment environment, std::string_view entry_name = {});

}
