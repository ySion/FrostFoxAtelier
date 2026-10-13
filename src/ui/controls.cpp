#include "ui/controls.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace atelier::ui {
ui_layout::NodeInput single_line_layout(float width) {
    auto layout=ui_layout::NodeInput::box(width,single_line_height);
    layout.height=ui_layout::Axis::content(single_line_height);
    return layout;
}
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
    auto caption=single_line_layout(160);
    auto& label=append(row.group_key?row.group_key:row.label_key,row.key,ControlKind::label,caption,row.label);
    label.classes={"parameter-label"}; label.modified=modified;
    label.context_target=row.group_key;
    auto body=NodeInput::row(2); body.height=Axis::content(single_line_height);
    body.width=Axis::flex(Basis::fixed(std::max(144.f,body_minimum)),1,1,body_minimum);
    append(row.body_key,row.key,ControlKind::container,body);
}
void append_number_row(std::vector<Control>& controls, ParameterRow row, std::span<const NumberField> fields) {
    if (fields.empty() || fields.size()>4) throw std::invalid_argument("A number row requires 1–4 fields.");
    for (const auto& field:fields) if (static_cast<unsigned>(field.axis)>static_cast<unsigned>(VectorAxis::w))
        throw std::invalid_argument("A number field requires a valid vector axis.");
    row.group_modified=row.group_modified.value_or(
        std::any_of(fields.begin(),fields.end(),[](const auto& field){return field.value!=field.default_value;}));
    constexpr float field_minimum=24;
    append_parameter_row(controls,row,fields.front().slider?114.f:static_cast<float>(fields.size())*field_minimum+static_cast<float>(fields.size()-1)*2);
    for (const auto& field:fields) {
        if (field.slider && (fields.size()!=1 || !field.slider->key || field.slider->minimum>=field.slider->maximum ||
            field.slider->minimum<field.minimum || field.slider->maximum>field.maximum))
            throw std::invalid_argument("A scalar slider requires a unique key and a range within the number's legal range.");
        Control number;
        number.key=field.key; number.parent=row.body_key; number.kind=ControlKind::number; number.text=field.label;
        number.layout=single_line_layout(); number.layout.width=ui_layout::Axis::expand(1,field_minimum);
        number.value=field.value; number.minimum=field.minimum; number.maximum=field.maximum;
        number.single_precision=field.single_precision;
        number.default_value=field.default_value;
        number.context_target=field.key; number.context_group=row.group_key;
        number.drag_step=field.drag_step; number.fine_step=field.fine_step;
        number.modified=field.value!=field.default_value; number.classes={"parameter-field"};
        constexpr std::array axis_classes{"","axis-x","axis-y","axis-z","axis-w"};
        if (field.axis!=VectorAxis::none) number.classes.push_back(axis_classes.at(static_cast<std::size_t>(field.axis)));
        if (field.slider) number.layout.width=ui_layout::Axis::fixed(72);
        // Houdini: a lone scalar is one tuple column wide (about a third of the
        // body), never a row-wide strip; tuples and slider pairs keep expanding.
        else if (fields.size()==1) number.layout.width=ui_layout::Axis::flex(ui_layout::Basis::fixed(scalar_field_width),0,1,field_minimum);
        controls.push_back(std::move(number));
        if (field.slider) {
            Control slider;
            slider.key=field.slider->key; slider.parent=row.body_key; slider.kind=ControlKind::slider;
            slider.layout=single_line_layout(); slider.layout.width=ui_layout::Axis::expand(1,40);
            slider.value=field.value; slider.minimum=field.slider->minimum; slider.maximum=field.slider->maximum;
            slider.drag_step=field.drag_step; slider.fine_step=field.fine_step;
            slider.classes={"parameter-slider"}; controls.push_back(std::move(slider));
        }
    }
}
} // namespace atelier::ui
