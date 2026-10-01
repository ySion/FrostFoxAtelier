#include "ui/controls.hpp"
#include <algorithm>
#include <stdexcept>

namespace atelier::ui {
void append_parameter_row(std::vector<Control>& controls, const ParameterRow& row, float body_minimum) {
    using namespace ui_layout;
    const auto append = [&](ControlKey key, ControlKey parent, ControlKind kind, NodeInput layout,
                            std::wstring text={}) -> Control& {
        Control control;
        control.key=key; control.parent=parent; control.kind=kind;
        control.layout=std::move(layout); control.text=std::move(text);
        controls.push_back(std::move(control));
        return controls.back();
    };
    const bool modified=row.group_modified.value_or(false);
    auto layout=NodeInput::row(8); layout.cross_gap=3;
    layout.flow=FlexSpec{}; layout.flow->wrap=Wrap::wrap;
    layout.flow->align_items=Alignment::fit(.5f);
    auto& root=append(row.key,row.parent,ControlKind::container,layout);
    root.enabled=row.enabled; root.classes={"parameter-row"};
    auto caption=NodeInput::box(160,24); caption.height=Axis::content(24);
    auto& label=append(row.group_key?row.group_key:row.label_key,row.key,ControlKind::label,caption,row.label);
    label.classes={"parameter-label"}; label.modified=modified;
    label.context_target=row.group_key;
    auto body=NodeInput::row(2); body.height=Axis::content(24);
    body.width=Axis::flex(Basis::fixed(std::max(144.f,body_minimum)),1,1,body_minimum);
    append(row.body_key,row.key,ControlKind::container,body);
}
void append_number_row(std::vector<Control>& controls, ParameterRow row, std::span<const NumberField> fields) {
    if (fields.empty() || fields.size()>4) throw std::invalid_argument("A number row requires 1–4 fields.");
    row.group_modified=row.group_modified.value_or(
        std::any_of(fields.begin(),fields.end(),[](const auto& field){return field.value!=field.default_value;}));
    constexpr float field_minimum=24;
    append_parameter_row(controls,row,static_cast<float>(fields.size())*field_minimum+static_cast<float>(fields.size()-1)*2);
    for (const auto& field:fields) {
        Control number;
        number.key=field.key; number.parent=row.body_key; number.kind=ControlKind::number; number.text=field.label;
        number.layout=ui_layout::NodeInput::box(0,24); number.layout.height=ui_layout::Axis::content(24); number.layout.width=ui_layout::Axis::expand(1,field_minimum);
        number.value=field.value; number.minimum=field.minimum; number.maximum=field.maximum;
        number.default_value=field.default_value;
        number.context_target=field.key; number.context_group=row.group_key;
        number.drag_step=field.drag_step; number.fine_step=field.fine_step;
        number.modified=field.value!=field.default_value; number.classes={"parameter-field"};
        controls.push_back(std::move(number));
    }
}
void append_menu(std::vector<Control>& controls, ControlKey key, ControlKey owner,
                 float x, float y, float width, std::span<const MenuItem> items) {
    using namespace ui_layout;
    Control popup;
    popup.key=key; popup.kind=ControlKind::popup; popup.popup_owner=owner;
    popup.layout=NodeInput::column(); popup.layout.width=Axis::fixed(width);
    popup.layout.padding={1,2,1,2}; popup.layout.out_of_flow=true;
    popup.layout.offset_x=x; popup.layout.offset_y=y;
    controls.push_back(std::move(popup));
    for (const auto& item:items) {
        Control control;
        control.key=item.key; control.parent=key; control.kind=ControlKind::menu_item;
        control.layout=NodeInput::box(0,24); control.layout.width=Axis::expand();
        control.text=item.text; control.enabled=item.enabled; control.selected=item.selected;
        control.edit_command=item.edit_command;
        controls.push_back(std::move(control));
    }
}
} // namespace atelier::ui
