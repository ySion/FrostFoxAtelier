#pragma once

#include "language/source.hpp"
#include <string>
#include <vector>

namespace atelier::ui_language {

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
    bool operator==(const State&) const = default;
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
struct Binding {
    std::uint32_t node{}, operation{};
    Property property{};
    language::SourcePosition position;
};
struct Event {
    std::uint32_t node{}, operation{};
};
struct SourceMapping {
    std::size_t begin{}, end{};
    language::SourcePosition origin;
};

// UI structure and source positions only. Expressions and handlers are typed and
// compiled by the ordinary Source compiler, in one module per component type.
struct Declaration {
    std::string name, source_name, generated_source;
    std::vector<Input> inputs;
    std::vector<State> states;
    std::vector<Node> nodes;
    std::vector<Binding> bindings;
    std::vector<Event> events;
    std::vector<SourceMapping> mappings;
    std::string diagnostic(std::string_view compiler_message) const;
};

[[nodiscard]] std::expected<Declaration, language::Diagnostic> lower(std::string_view source_name,
                                                                     std::string_view source);
[[nodiscard]] std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(std::string_view source_name,
                                                                                         std::string_view source);

} // namespace atelier::ui_language
