#pragma once

#include "language/source.hpp"
#include "ui/detail/number_bits.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace atelier::ui_language {

namespace detail {
inline constexpr auto same_binding_value = []<class... T>(const std::variant<T...>& a, const std::variant<T...>& b) {
    if (const auto* number = std::get_if<double>(&a)) {
        const auto* other = std::get_if<double>(&b);
        return other && ui::detail::number_bits(*number) == ui::detail::number_bits(*other);
    }
    return a == b;
};
}

enum class InputKind { boolean, number, edit, text, text_edit, key, items, action, table, code, tooltip, tree_row };

struct Input {
    std::string name;

    InputKind kind;

    std::string item_type;

    bool operator==(const Input&) const = default;
};

struct State {
    std::string name;

    InputKind kind;

    // Non-monostate when initialized by a constant literal; otherwise initialized by JIT op 0.
    std::variant<std::monostate, bool, double> initial_value{};

    bool operator==(const State& other) const {
        return name == other.name && kind == other.kind &&
               detail::same_binding_value(initial_value, other.initial_value);
    }
};

enum class NodeKind {
    column,
    row,
    text,
    foldout,
    number_row,
    text_field,
    tree,
    tree_item,
    component,
    repeat,
    button,
    list,
    combo,
    choice_button,
    toggle_button,
    table,
    checkbox,
    parameter_row,
    viewport,
    tab,
    splitter,
    popup,
    menu_item,
    overlay,
    code_editor
};

enum class Property {
    text,
    classes,
    gap,
    height,
    width,
    padding_left,
    enabled,
    selected,
    collapsed,
    caption,
    edit,
    expand_height,
    icon,
    tree_row,
    tree_rows,
    mounted,
    items,
    padding_top,
    padding_right,
    padding_bottom,
    align_center,
    single_line,
    action,
    selection_group,
    content_width,
    content_height,
    expand_width,
    min_width,
    wrap,
    panel,
    model,
    context,
    shrink_width,
    view,
    row_height,
    header_height,
    tile_width,
    tile_height,
    item_gap,
    empty_text,
    modified,
    out_of_flow,
    offset_x,
    offset_y,
    anchor_x,
    anchor_y,
    pivot_x,
    pivot_y,
    flex_width,
    grow_width,
    red,
    green,
    blue,
    value,
    minimum,
    maximum,
    step,
    vertical,
    click_action,
    menu_branch,
    menu_behavior,
    radio_group,
    document,
    tooltip,
    tooltip_title,
    tooltip_identity,
    draggable,
    argument
};

struct Node {
    std::string name;

    NodeKind kind;

    std::uint32_t parent{}; // Parent slot + 1; zero is the host parent.

    std::string component_type;

    bool operator==(const Node&) const = default;
};

struct DirectSource {
    enum class Kind : std::uint8_t { input, state, node_key };

    Kind kind{};

    std::uint32_t slot{};

    InputKind input_kind{};

    bool operator==(const DirectSource&) const = default;
};

using LiteralValue = std::variant<std::monostate, bool, double, std::string>;

struct Binding {
    enum class Mode : std::uint8_t { jit, literal, direct };

    std::uint32_t node{};

    std::uint32_t operation{};

    Property property{};

    InputKind argument_kind{};

    std::uint32_t argument{};

    Mode mode{Mode::jit};

    LiteralValue literal{};

    DirectSource direct{};

    language::SourcePosition position;

    bool same_definition(const Binding& other) const noexcept {
        return node == other.node && operation == other.operation && property == other.property &&
               argument_kind == other.argument_kind && argument == other.argument && mode == other.mode &&
               detail::same_binding_value(literal, other.literal) && direct == other.direct;
    }
};

struct Event {
    std::uint32_t node{};

    std::uint32_t operation{};

    language::SourcePosition position;
};

struct SourceMapping {
    std::size_t begin{};

    std::size_t end{};

    language::SourcePosition origin;
};

struct PreparedHelpers {
    std::string generated_source;

    std::vector<SourceMapping> mappings;
};

// UI structure, static/direct bindings and source positions. Computed
// expressions and handlers are typed and compiled by the ordinary Source
// compiler when needs_jit is true.
struct Declaration {
    std::string name;

    std::string source_name;

    std::string generated_source;

    std::shared_ptr<const PreparedHelpers> helpers;

    bool has_jit_init{};

    bool needs_jit{};

    std::vector<Input> inputs;

    std::vector<State> states;

    std::vector<Node> nodes;

    std::vector<Binding> bindings;

    std::vector<Event> events;

    std::vector<SourceMapping> mappings;
};

[[nodiscard]] std::expected<Declaration, language::Diagnostic> lower(std::string_view source_name,
                                                                     std::string_view source);

[[nodiscard]] std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(std::string_view source_name,
                                                                                         std::string_view source);

} // namespace atelier::ui_language
