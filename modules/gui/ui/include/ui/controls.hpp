#pragma once

#include "ui/session.hpp"

namespace atelier::ui {

enum class VectorAxis { none, x, y, z, w };

inline constexpr float single_line_height = 23;

inline constexpr float scalar_field_width = 140; // One tuple column in a default-width Parameters pane.

inline constexpr float stow_rail_width = 12;

inline constexpr float stow_collapsed_height = 12;

inline constexpr float stow_arrow_region_height = 12;

// Shared number/text editor geometry. Content may grow for larger fonts.
ui_layout::NodeInput single_line_layout(float width = 0);

// Slider endpoints are a navigation range; the number retains its legal range.
struct SliderRange {
    ControlKey key{};

    double minimum{};

    double maximum{1};

    bool operator==(const SliderRange&) const = default;
};

// A scalar/vector row is a composition of ordinary controls. All keys belong
// to the caller; field keys remain stable when the row wraps or is regrouped.
struct NumberField {
    ControlKey key{};

    std::wstring label;

    double value{};

    double default_value{};

    double minimum{-10000};

    double maximum{10000};

    double drag_step{.01};

    double fine_step{.001};

    VectorAxis axis{VectorAxis::none};

    std::optional<SliderRange> slider{};

    bool single_precision{};

    bool operator==(const NumberField&) const = default;
};

struct ParameterRow {
    ControlKey key{};

    ControlKey label_key{};

    ControlKey body_key{};

    ControlKey group_key{};

    ControlKey parent{};

    std::wstring label;

    bool enabled{true};

    // When the host filters out fields, retain the complete group's state.
    // Otherwise derive it from the supplied fields.
    std::optional<bool> group_modified{};
};

// Common caption/body composition for numbers, text and choices. body_key is
// a row with a 23 DIP minimum height; callers append their ordinary controls to it.
void append_parameter_row(std::vector<Control>&, const ParameterRow&, float body_minimum = 144);

// Append 1–4 fields. The caption uses group_key when supplied; its context menu
// and Ctrl+middle reset address that stable identity. The host owns group undo.
// Invalid fields return invalid_control with a static diagnostic. Failure may
// leave partially appended controls; the caller must discard that candidate.
Result<void> append_number_row(std::vector<Control>&, ParameterRow, std::span<const NumberField>);

struct MenuItem {
    ControlKey key{};

    std::wstring text;

    bool enabled{true};

    bool selected{};

    std::optional<EditCommand> edit_command;

    std::wstring shortcut{};

    bool separator_before{};

    std::vector<MenuItem> children{};

    MenuBehavior behavior{MenuBehavior::command};

    ControlKey radio_group{};

    ControlIcon icon{ControlIcon::none};

    std::wstring heading{}; // Optional group caption drawn above this item (Blender popover sections).

    bool operator==(const MenuItem&) const = default;
};

// Tree hosts flatten their own hierarchy; Session lays out only described rows.
inline constexpr std::uint32_t no_tree_node = std::numeric_limits<std::uint32_t>::max();

// Host nodes in depth-first preorder: a parent precedes its subtree. text is read only while filtering.
struct TreeNode {
    std::uint32_t parent{no_tree_node};

    bool expanded{};

    std::wstring_view text;
};

struct TreeRow {
    std::uint32_t node{};

    std::uint32_t parent{no_tree_node}; // parent: index of the parent row.

    std::uint32_t depth{};

    bool branch{};

    bool expanded{};

    std::bitset<128> lines; // Control::tree_lines.
};

// Visible rows of nodes, replacing rows. A nonempty filter (ASCII case folding)
// keeps matching nodes with their ancestors and subtrees, all expanded. Throws
// std::invalid_argument for a node outside preorder, deeper than 127 levels, or
// beyond maximum_tree_rows.
void flatten_tree(std::span<const TreeNode>, std::wstring_view filter, std::vector<TreeRow>& rows);

// Ascending row indices to describe: the viewport and one screen beyond either
// edge, the first and last rows, and each pinned row with its neighbours and ancestors.
std::vector<std::uint32_t> tree_window(std::span<const TreeRow>, TreeViewport, std::span<const std::uint32_t> pinned);
} // namespace atelier::ui
