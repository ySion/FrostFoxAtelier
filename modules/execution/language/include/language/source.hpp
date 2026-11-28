#pragma once
#include "math/type.hpp"
#include "runtime/parameter.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::language {

// Compiler classifications, not a separate registry of business types.
struct ValueType {
    enum Kind {
        f32,
        u32,
        i32,
        boolean,
        void_value,
        strong_reference,
        weak_reference,
        null_reference,
        enumeration,
        record,
        array,
        string,
        f64
    };

    Kind kind{void_value};

    std::uint32_t type_index{}; // index in the enum or value catalog, selected by kind

    constexpr ValueType(Kind value = void_value, std::uint32_t index = 0) : kind(value), type_index(index) {}

    bool operator==(const ValueType&) const = default;
};

struct ParameterType {
    ValueType type;

    ParameterPassing passing{ParameterPassing::value};

    bool has_default{};

    std::uint64_t default_bits{};

    bool mutates_structure{};

    bool changes_length{};
};

struct EnumItem {
    std::string_view name;

    std::uint32_t bits;
};

struct EnumBinding {
    std::string_view name;

    ValueType underlying;

    std::span<const EnumItem> values;

    bool flags{};
};

struct FieldBinding {
    std::string_view name;

    ValueType type;

    bool constrained{};
};

struct ValueField {
    std::string name;

    ValueType type;

    std::size_t offset;
};

struct ValueLayout {
    std::string name;

    ValueType::Kind kind;

    std::size_t size;

    std::size_t alignment;

    ValueType element;

    std::vector<ValueField> fields;

    void (*sequence_view)(const void*, const void**, std::size_t*) noexcept {};

    bool can_own{};

    bool can_mutate{};

    bool standard_array_layout{};

    math::Type math{};

    bool plain_value{};

    bool zero_default{};

    std::vector<std::byte> initial_value{}; // Material compilation captures plain native defaults, never callbacks.
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

    bool material{}; // Enables controlled sample/orientation intrinsics; validation follows typing.

    ValueType texture_input{}; // Exact registered value type, only meaningful for materials.
};

struct SourcePosition {
    std::size_t offset{};

    std::size_t line{1};

    std::size_t column{1};
};

struct Diagnostic {
    SourcePosition position;

    std::string message;
};

using NodeId = std::size_t;

inline constexpr NodeId no_node = std::numeric_limits<NodeId>::max();

enum class Operation {
    add,
    subtract,
    multiply,
    positive,
    negate,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    divide,
    remainder,
    bitwise_and,
    bitwise_or,
    bitwise_xor,
    bitwise_not,
    shift_left,
    shift_right,
    logical_not,
    logical_and,
    logical_or
};

// square_root and every later operation are element-wise over scalars and vectors.
enum class MathOperation {
    dot,
    cross,
    length,
    length_squared,
    distance,
    distance_squared,
    normalize_or_zero,
    fast_normalize_or_zero,
    conjugate,
    slerp01,
    fast_slerp01,
    rotate,
    transpose,
    transform_point,
    transform_direction,
    is_finite,
    as_uint,
    as_int,
    square_root,
    fast_rsqrt,
    floor,
    ceil,
    trunc,
    round,
    fract,
    abs,
    sign,
    min,
    max,
    clamp,
    cwise_mul,
    cwise_div,
    radians,
    degrees,
    lerp,
    step,
    smoothstep,
    sin,
    cos,
    atan2,
    exp,
    log,
    pow
};

enum class SequenceOperation : std::uint8_t {
    str_empty,
    str_is_ascii,
    str_starts_with,
    str_ends_with,
    str_contains,
    str_equals_ignore_case,
    str_starts_with_ignore_case,
    str_ends_with_ignore_case,
    str_find,
    str_rfind,
    str_count,
    str_common_prefix_length,
    str_try_parse_int,
    str_try_parse_uint,
    str_try_parse_float,
    str_try_parse_double,
    str_append_scalar,
    str_append_ascii,
    str_append_hex,
    str_append_float_fixed,
    str_pad_left,
    str_pad_right,
    str_trim,
    str_trim_start,
    str_trim_end,
    str_to_lower,
    str_to_upper,
    str_strip_prefix,
    str_strip_suffix,
    str_slice_in_place,
    str_erase,
    str_replace,
    str_replace_all,
    arr_empty,
    arr_contains,
    arr_find,
    arr_rfind,
    arr_count,
    arr_is_sorted,
    arr_fill,
    arr_reverse,
    arr_swap,
    arr_sort,
    arr_resize_fill,
    arr_truncate,
    arr_pop,
    arr_insert,
    arr_remove_at,
    arr_remove_at_swap,
    arr_remove,
    arr_remove_swap
};

[[nodiscard]] constexpr bool is_try_parse_sequence_operation(SequenceOperation op) noexcept {
    return op >= SequenceOperation::str_try_parse_int && op <= SequenceOperation::str_try_parse_double;
}

[[nodiscard]] constexpr bool is_readonly_sequence_operation(SequenceOperation op) noexcept {
    return (op >= SequenceOperation::str_empty && op <= SequenceOperation::str_common_prefix_length) ||
           (op >= SequenceOperation::arr_empty && op <= SequenceOperation::arr_is_sorted);
}

[[nodiscard]] constexpr bool is_length_preserving_array_mutation(SequenceOperation op) noexcept {
    return op >= SequenceOperation::arr_fill && op <= SequenceOperation::arr_sort;
}

[[nodiscard]] constexpr bool mutates_sequence_structure(SequenceOperation op) noexcept {
    return (op >= SequenceOperation::str_append_scalar && op <= SequenceOperation::str_replace_all) ||
           (op >= SequenceOperation::arr_resize_fill && op <= SequenceOperation::arr_remove_swap);
}

[[nodiscard]] constexpr bool can_be_statement_sequence_operation(SequenceOperation op) noexcept {
    return !is_readonly_sequence_operation(op);
}

struct Expression {
    enum class Kind {
        literal,
        local,
        field,
        call,
        unary,
        binary,
        receiver,
        null_reference,
        create_reference,
        observe_reference,
        acquire_reference,
        value_field,
        index,
        length,
        string_literal,
        sequence_mutation,
        sequence_operation,
        source_call,
        math_construct,
        math_intrinsic,
        scalar_cast,
        texture_sample,
        texture_orientation,
        conditional,
        constant_data
    };

    Kind kind;

    ValueType type;

    SourcePosition position;

    NodeId left{no_node};

    NodeId right{no_node};

    std::uint32_t value{}; // local index or host catalog index

    Operation operation{Operation::add};

    NodeId receiver{no_node}; // field/call owner

    std::vector<ParameterPassing> argument_passing{};

    std::vector<NodeId> arguments{}; // call operands, evaluated left to right

    std::size_t offset{};

    SequenceMutation mutation{};

    SequenceOperation sequence_operation{SequenceOperation::str_empty};

    std::uint64_t bits{}; // literal payload, including exact double representation

    MathOperation math_operation{MathOperation::dot};

    bool typed_constant{}; // module constant substitutions retain their declared type
};

struct Statement {
    enum class Kind {
        block,
        declare,
        assign,
        call,
        condition,
        loop,
        parallel_loop,
        return_value,
        fail,
        yield,
        break_loop,
        continue_loop
    };

    Kind kind;

    SourcePosition position;

    NodeId target{no_node}; // local, lvalue, or fail message index

    NodeId expression{no_node}; // rhs, condition, return value, or optional fail element index

    NodeId body{no_node}; // block/branch body, or optional fail layer enum expression

    NodeId alternative{no_node};

    std::vector<NodeId> children;

    std::vector<std::uint32_t> locals{}; // declarations directly owned by this block

    NodeId step{no_node}; // optional loop step, also executed after continue

    std::optional<Operation> compound{};
};

struct Local {
    std::string name;

    ValueType type;

    bool read_only{};

    bool fixed_length{};
    // The first Function::parameters.size() locals are parameters;
    // all remaining locals initialize at their declaration.
};

struct Function {
    SourcePosition position;

    std::size_t end_offset{};

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

    bool parallel_safe{};

    bool used_in_parallel{};

    std::string description{};

    std::vector<std::string> strings;
};

struct ConstantData {
    ValueType type;

    std::size_t element_count{};

    std::vector<std::byte> bytes;
};

struct Program {
    std::string source_name;

    std::string object_type_name;

    std::vector<ValueLayout> value_layouts;

    std::vector<ConstantData> constants;

    std::vector<Function> functions;

    std::vector<std::string> failure_messages;

    std::uint32_t entry{};

    std::vector<bool> reachable;

    bool material{}; // Set only after validating the complete material subset.

    // Public wrappers share internal typed function bodies in one JIT module.
    std::vector<std::uint32_t> exports;

    bool exported(std::uint32_t index) const {
        if (exports.empty())
            return index == entry;
        for (auto item : exports)
            if (item == index)
                return true;
        return false;
    }
};

// One module with a host-selected entry: scalar/enum/plain record values or borrowed
// parameters, an optional scalar/plain record result and implicit self. Async cannot borrow
// caller storage or expose aggregate parameters/results.
// Environment names describe already validated Runtime members. No Runtime or
// LLVM object is owned here. Source/Environment storage is borrowed only during
// compilation; the returned typed tree owns everything needed for lowering.
// allow_declarations checks a module that selects no entry, with or without functions.
[[nodiscard]] std::expected<Program, Diagnostic> compile_source(std::string_view source_name, std::string_view source,
                                                                Environment environment,
                                                                std::string_view entry_name = {},
                                                                bool allow_declarations = false,
                                                                bool allow_parameter_attributes = false);

[[nodiscard]] std::expected<void, Diagnostic> export_functions(Program&, std::span<const std::string_view>,
                                                               bool receiver);

}
