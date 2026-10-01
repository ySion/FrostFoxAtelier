#pragma once

#include "ui/session.hpp"

namespace atelier::ui {

// A scalar/vector row is a composition of ordinary controls. All keys belong
// to the caller; field keys remain stable when the row wraps or is regrouped.
struct NumberField {
    ControlKey key{}, label_key{};
    std::wstring label;
    double value{}, default_value{}, minimum{-10000}, maximum{10000};
    double drag_step{.01}, fine_step{.001};
};
struct NumberRow {
    ControlKey key{}, header_key{}, label_key{}, body_key{}, reset_key{}, parent{};
    std::wstring label;
    bool enabled{true};
    // When the host filters out fields, retain the complete group's state.
    // Otherwise derive it from the supplied fields.
    std::optional<bool> group_modified{};
};
// Append 1–4 fields. Reset emits an ordinary activation of reset_key; the host
// applies the group change through its own transaction/undo mechanism.
void append_number_row(std::vector<Control>&, const NumberRow&, std::span<const NumberField>);

} // namespace atelier::ui
