#pragma once

#include "ui/session.hpp"

namespace atelier::ui {

// A scalar/vector row is a composition of ordinary controls. All keys belong
// to the caller; field keys remain stable when the row wraps or is regrouped.
struct NumberField {
    ControlKey key{};
    std::wstring label;
    double value{}, default_value{}, minimum{-10000}, maximum{10000};
    double drag_step{.01}, fine_step{.001};
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
// a 24 DIP row; callers append their ordinary controls to it.
void append_parameter_row(std::vector<Control>&, const ParameterRow&, float body_minimum=144);
// Append 1–4 fields. The caption uses group_key when supplied; its context menu
// and Ctrl+middle reset address that stable identity. The host owns group undo.
void append_number_row(std::vector<Control>&, ParameterRow, std::span<const NumberField>);

struct MenuItem {
    ControlKey key{};
    std::wstring text;
    bool enabled{true}, selected{};
    std::optional<EditCommand> edit_command;
};
// Append last: a root popup and its items. Coordinates and width are DIPs;
// Session clamps to the surface and owns modal input, scrolling and focus.
void append_menu(std::vector<Control>&, ControlKey key, ControlKey owner,
                 float x, float y, float width, std::span<const MenuItem>);

} // namespace atelier::ui
