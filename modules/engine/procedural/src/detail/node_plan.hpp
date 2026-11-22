#pragma once

#include "procedural/graph.hpp"

#include <array>
#include <optional>
#include <vector>

namespace atelier::procedural::detail {

inline constexpr std::size_t column_type_count = std::variant_size_v<ColumnData> - 1; // Excludes detail-only String.

enum class ArgKind : std::uint8_t {
    context,
    port_input,
    port_inputs,
    port_spatial,
    port_output,
    column_in,
    column_inout,
    column_out,
    column_group
};

struct ArgPlan {
    ArgKind kind{};

    TypeId type{};

    std::uint32_t port_index{};

    std::size_t column_variant{};

    String static_name{};

    FieldId name_selector{};

    std::optional<Layer> fixed_layer{};

    bool optional{};
};

struct NodePlan {
    Ports ports;

    MethodId method{}; // Non-null when `native` is true.

    bool native{};

    bool uses_context{};

    bool uses_primary_stream{};

    FieldId run_over_field{};

    std::vector<ArgPlan> arguments;
};

Result<NodePlan> inspect_node(Runtime& runtime, TypeId type);

}
