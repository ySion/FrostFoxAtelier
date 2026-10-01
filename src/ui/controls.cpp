#include "ui/controls.hpp"
#include <algorithm>
#include <stdexcept>

namespace atelier::ui {
void append_number_row(std::vector<Control>& controls, const NumberRow& row, std::span<const NumberField> fields) {
    if (fields.empty() || fields.size()>4) throw std::invalid_argument("A number row requires 1–4 fields.");
    using namespace ui_layout;
    const auto append = [&](ControlKey key, ControlKey parent, ControlKind kind, NodeInput layout,
                            std::wstring text={}) -> Control& {
        Control control;
        control.key=key; control.parent=parent; control.kind=kind;
        control.layout=std::move(layout); control.text=std::move(text);
        controls.push_back(std::move(control));
        return controls.back();
    };
    const bool modified=row.group_modified.value_or(
        std::any_of(fields.begin(),fields.end(),[](const auto& field){return field.value!=field.default_value;}));
    auto layout=NodeInput::row(8); layout.cross_gap=4;
    layout.flow=FlexSpec{}; layout.flow->wrap=Wrap::wrap;
    layout.flow->align_items=Alignment::fit(.5f);
    auto& root=append(row.key,row.parent,ControlKind::container,layout);
    root.enabled=row.enabled; root.classes={"parameter-row"};
    auto header=NodeInput::row(4); header.width=Axis::flex(Basis::fixed(132),1,0,132); header.height=Axis::content(28);
    append(row.header_key,row.key,ControlKind::container,header);
    auto caption=NodeInput::box(0,0); caption.width=Axis::expand(); caption.height=Axis::content(28);
    auto& label=append(row.label_key,row.header_key,ControlKind::label,caption,row.label);
    label.classes={"parameter-label"}; label.modified=modified;
    if (row.reset_key) {
        auto reset=NodeInput::box(42,28); reset.vertical_alignment=Alignment::fit(.5f);
        auto& button=append(row.reset_key,row.header_key,ControlKind::button,reset,L"Reset");
        button.enabled=modified; button.classes={"compact","quiet"};
    }
    constexpr float field_gap=4, label_width=12, field_minimum=24;
    auto body=NodeInput::row(field_gap); body.height=Axis::fixed(28);
    const auto labels=static_cast<std::size_t>(std::count_if(fields.begin(),fields.end(),[](const auto& field){return !field.label.empty();}));
    const auto minimum=static_cast<float>(fields.size())*field_minimum+static_cast<float>(labels)*label_width+
        static_cast<float>(fields.size()+labels-1)*field_gap;
    body.width=Axis::flex(Basis::fixed(std::max(168.f,minimum)),3,1,minimum);
    append(row.body_key,row.key,ControlKind::container,body);
    for (const auto& field:fields) {
        if (!field.label.empty()) {
            auto marker=NodeInput::box(label_width,28); marker.padding.top=5;
            auto& axis=append(field.label_key,row.body_key,ControlKind::label,marker,field.label);
            axis.classes={"axis-label"};
        }
        auto input=NodeInput::box(0,28); input.width=Axis::expand(1,field_minimum);
        auto& number=append(field.key,row.body_key,ControlKind::number,input);
        number.value=field.value; number.minimum=field.minimum; number.maximum=field.maximum;
        number.drag_step=field.drag_step; number.fine_step=field.fine_step;
        number.modified=field.value!=field.default_value; number.classes={"parameter-field"};
    }
}
} // namespace atelier::ui
