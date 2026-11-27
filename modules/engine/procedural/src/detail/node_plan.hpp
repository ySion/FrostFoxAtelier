#pragma once

#include "procedural/graph.hpp"

#include <array>

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

    Optional<Layer> fixed_layer{};

    bool optional{};

    std::string purpose{};
};

struct NodePlan {
    Ports ports;

    MethodId method{}; // Non-null when `native` is true.

    bool native{};

    bool uses_context{};

    bool uses_primary_stream{};

    FieldId run_over_field{};

    Array<ArgPlan> arguments;
};

Array<AttributeRule> attribute_rules(const NodePlan& plan, const TypeInfo& type);
void append_native_rules(Array<AttributeRule>& rules, std::string_view type);

Result<NodePlan> inspect_node(Runtime& runtime, TypeId type, Array<PortDescription>* port_descriptions = nullptr);

}
