#pragma once

#include "ui/session.hpp"

namespace atelier::ui {

enum class VectorAxis { none, x, y, z, w };
inline constexpr float single_line_height=23;
inline constexpr float stow_rail_width=12, stow_collapsed_height=12, stow_arrow_region_height=12;
// Shared number/text editor geometry. Content may grow for larger fonts.
ui_layout::NodeInput single_line_layout(float width=0);
// Slider endpoints are a navigation range; the number retains its legal range.
struct SliderRange { ControlKey key{}; double minimum{}, maximum{1}; bool operator==(const SliderRange&) const = default; };
// A scalar/vector row is a composition of ordinary controls. All keys belong
// to the caller; field keys remain stable when the row wraps or is regrouped.
struct NumberField {
    ControlKey key{};
    std::wstring label;
    double value{}, default_value{}, minimum{-10000}, maximum{10000};
    double drag_step{.01}, fine_step{.001};
    VectorAxis axis{VectorAxis::none};
    std::optional<SliderRange> slider{};
    bool single_precision{};
    bool operator==(const NumberField&) const = default;
};
struct ParameterRow {
    ControlKey key{}, label_key{}, body_key{}, group_key{}, parent{};
    std::wstring label;
    bool enabled{true};
    // When the host filters out fields, retain the complete group's state.
    // Otherwise derive it from the supplied fields.
    std::optional<bool> group_modified{};
};
// Common caption/body composition for numbers, text and choices. body_key is
// a row with a 23 DIP minimum height; callers append their ordinary controls to it.
void append_parameter_row(std::vector<Control>&, const ParameterRow&, float body_minimum=144);
// Append 1–4 fields. The caption uses group_key when supplied; its context menu
// and Ctrl+middle reset address that stable identity. The host owns group undo.
void append_number_row(std::vector<Control>&, ParameterRow, std::span<const NumberField>);

struct MenuItem {
    ControlKey key{};
    std::wstring text;
    bool enabled{true}, selected{};
    std::optional<EditCommand> edit_command;
    std::wstring shortcut{};
    bool separator_before{};
    std::vector<MenuItem> children{};
    MenuBehavior behavior{MenuBehavior::command};
    ControlKey radio_group{};
    ControlIcon icon{ControlIcon::none};
    bool operator==(const MenuItem&) const = default;
};
} // namespace atelier::ui
