#pragma once

#include "language/source.hpp"
#include <string>
#include <vector>

namespace atelier::ui_language {

enum class InputKind { boolean, number, edit, text, text_edit, key, items };
struct Input { std::string name; InputKind kind; std::string item_type; };
struct State { std::string name; InputKind kind; };
enum class NodeKind { column, row, text, foldout, number_row, text_field, tree, tree_item, component, repeat };
enum class Property { text, classes, gap, height, width, padding_left, enabled, selected, collapsed, caption, edit,
    expand_height, icon, tree_parent, expanded, filter, mounted, items,
    padding_top, padding_right, padding_bottom, align_center, single_line, argument };
struct Node {
    std::string name;
    NodeKind kind;
    std::uint32_t parent{}; // Parent slot + 1; zero is the host parent.
    std::string component_type;
};
struct Binding { std::uint32_t node{}, operation{}; Property property{}; language::SourcePosition position; };
struct Event { std::uint32_t node{}, operation{}; };
struct SourceMapping { std::size_t begin{}, end{}; language::SourcePosition origin; };

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

[[nodiscard]] std::expected<Declaration, language::Diagnostic> lower(
    std::string_view source_name, std::string_view source);
[[nodiscard]] std::expected<std::vector<Declaration>, language::Diagnostic> lower_module(
    std::string_view source_name, std::string_view source);

} // namespace atelier::ui_language
