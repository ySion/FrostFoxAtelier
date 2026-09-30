#pragma once
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
// A predicate is internal to conditions, not a new Runtime value type.
struct ValueType {
    enum Kind { f32, u32, i32, predicate, void_value, strong_reference, weak_reference, null_reference, enumeration, record, array, string };
    Kind kind{void_value};
    std::uint32_t type_index{}; // index in the enum or native-value catalog, selected by kind
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
enum class Operation { add, subtract, multiply, positive, negate, equal, not_equal, less, less_equal, greater, greater_equal };
struct Expression {
    enum class Kind {
        literal, local, field, call, unary, binary,
        receiver, null_reference, create_reference, observe_reference, acquire_reference,
        value_field, index, length, string_literal, sequence_mutation
    };
    Kind kind;
    ValueType type;
    SourcePosition position;
    NodeId left{no_node}, right{no_node};
    std::uint32_t value{}; // literal bits, local index, or host catalog index
    Operation operation{Operation::add};
    NodeId receiver{no_node}; // field/call owner
    std::vector<ParameterPassing> argument_passing{};
    std::vector<NodeId> arguments{}; // native call operands, evaluated left to right
    std::size_t offset{};
    SequenceMutation mutation{};
};
struct Statement {
    enum class Kind { block, declare, assign, call, condition, loop, return_value, yield };
    Kind kind;
    SourcePosition position;
    NodeId target{no_node}, expression{no_node}, body{no_node}, alternative{no_node};
    std::vector<NodeId> children;
    std::vector<std::uint32_t> locals{}; // declarations directly owned by this block
};
struct Local {
    std::string name;
    ValueType type;
    bool read_only{};
    // The first Program::parameters.size() locals are parameters;
    // all remaining locals initialize at their declaration.
};
struct Program {
    std::string source_name;
    std::string function_name;
    std::vector<ParameterType> parameters;
    ValueType result{ValueType::void_value};
    std::vector<Local> locals;
    std::vector<Expression> expressions;
    std::vector<Statement> statements;
    NodeId body{no_node};
    std::string object_type_name;
    bool is_coroutine{};
    bool uses_native_values{};
    std::vector<ValueLayout> value_layouts;
    std::vector<std::string> strings;
};

// One host-bound function: scalar/enum values or native borrowed parameters,
// an optional scalar result and implicit self. Async cannot borrow caller storage.
// Environment names describe already validated Runtime members. No Runtime or
// LLVM object is owned here. Source/Environment storage is borrowed only during
// compilation; the returned typed tree owns everything needed for lowering.
[[nodiscard]] std::expected<Program, Diagnostic> compile_source(
    std::string_view source_name, std::string_view source, Environment environment);

}
