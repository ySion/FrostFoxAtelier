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
void append_parameter_tabs(std::vector<Control>& controls,ControlKey key,ControlKey parent,
                           ControlKey selected,std::span<const ParameterTab> tabs) {
    Control bar; bar.key=key; bar.parent=parent; bar.layout=ui_layout::NodeInput::row();
    bar.layout.height=ui_layout::Axis::content(26+2.f/3); bar.layout.padding={0,0,0,2.f/3}; bar.layout.flow=ui_layout::FlexSpec{};
    bar.layout.flow->wrap=ui_layout::Wrap::wrap; bar.classes={"parameter-tabs"};
    controls.push_back(std::move(bar));
    bool first=true;
    for (const auto& tab:tabs) {
        Control item; item.key=tab.key; item.parent=key; item.kind=ControlKind::button; item.text=tab.text;
        item.layout=ui_layout::NodeInput::box(0,26); item.layout.width=ui_layout::Axis::content(0);
        item.selected=tab.key==selected; item.selection_group=key; item.classes={"parameter-tab"};
        if (!first) item.classes.push_back("joined");
        controls.push_back(std::move(item)); first=false;
    }
}
Control& append_object_header(std::vector<Control>& controls,ControlKey key,ControlKey parent,
                              ControlKey type_key,ControlKey name_key,std::wstring type,std::wstring name,bool enabled) {
    Control row; row.key=key; row.parent=parent; row.layout=ui_layout::NodeInput::row(6);
    row.layout.padding={6,3,6,3}; row.classes={"object-header"};
    row.layout.flow=ui_layout::FlexSpec{}; row.layout.flow->align_items=ui_layout::Alignment::fit(.5f);
    controls.push_back(std::move(row));
    Control caption; caption.key=type_key; caption.parent=key; caption.kind=ControlKind::label; caption.text=std::move(type);
    caption.layout=single_line_layout(); caption.layout.width=ui_layout::Axis::content(); caption.classes={"object-type"};
    controls.push_back(std::move(caption));
    Control field; field.key=name_key; field.parent=key; field.kind=ControlKind::text_edit; field.text=std::move(name);
    field.layout=single_line_layout(); field.layout.width=ui_layout::Axis::expand(1,32);
    field.enabled=enabled; field.context_target=name_key; field.classes={"object-name"};
    controls.push_back(std::move(field)); return controls.back();
}
void append_path_bar(std::vector<Control>& controls,const PathBar& bar) {
    Control row; row.key=bar.key; row.parent=bar.parent; row.layout=ui_layout::NodeInput::row(2);
    row.layout.height=ui_layout::Axis::fixed(40); row.layout.padding={4,4,6,4}; row.classes={"path-bar"};
    row.layout.flow=ui_layout::FlexSpec{}; row.layout.flow->align_items=ui_layout::Alignment::fit(.5f);
    controls.push_back(std::move(row));
    for (bool back:{true,false}) {
        Control button; button.key=back?bar.back_key:bar.forward_key; button.parent=bar.key; button.kind=ControlKind::button;
        button.icon=back?ControlIcon::back:ControlIcon::forward; button.enabled=back?bar.can_back:bar.can_forward;
        button.layout=ui_layout::NodeInput::box(26,28); button.classes={"path-navigation"}; controls.push_back(std::move(button));
    }
    Control label; label.key=bar.path_key; label.parent=bar.key; label.kind=ControlKind::combo; label.text=bar.path;
    label.icon=ControlIcon::world;
    label.layout=ui_layout::NodeInput::box(0,28); label.layout.width=ui_layout::Axis::expand(1,32); label.classes={"pane-path"};
    controls.push_back(std::move(label));
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
        number.default_value=field.default_value;
        number.context_target=field.key; number.context_group=row.group_key;
        number.drag_step=field.drag_step; number.fine_step=field.fine_step;
        number.modified=field.value!=field.default_value; number.classes={"parameter-field"};
        constexpr std::array axis_classes{"","axis-x","axis-y","axis-z","axis-w"};
        if (field.axis!=VectorAxis::none) number.classes.push_back(axis_classes.at(static_cast<std::size_t>(field.axis)));
        if (field.slider) number.layout.width=ui_layout::Axis::fixed(72);
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
void append_menu(std::vector<Control>& controls, ControlKey key, ControlKey owner,
                 float x, float y, float width, std::span<const MenuItem> items) {
    using namespace ui_layout;
    Control popup;
    popup.key=key; popup.kind=ControlKind::popup; popup.popup_owner=owner;
    popup.layout=NodeInput::column(); popup.layout.width=Axis::fixed(width);
    popup.layout.padding={1,2,1,2}; popup.layout.out_of_flow=true;
    popup.layout.offset_x=x; popup.layout.offset_y=y;
    controls.push_back(std::move(popup));
    auto auxiliary=std::numeric_limits<ControlKey>::max();
    const auto allocate=[&] {
        while (std::any_of(controls.begin(),controls.end(),[&](const auto& c){return c.key==auxiliary;}) ||
               std::any_of(items.begin(),items.end(),[&](const auto& item){return item.key==auxiliary;})) --auxiliary;
        return auxiliary--;
    };
    for (const auto& item:items) {
        if (item.separator_before) {
            Control separator; separator.key=allocate(); separator.parent=key;
            separator.layout=NodeInput::box(0,7); separator.layout.width=Axis::expand();
            separator.classes={"menu-separator"}; separator.enabled=false;
            controls.push_back(std::move(separator));
        }
        Control row; row.key=allocate(); row.parent=key;
        row.layout=NodeInput::column(); row.layout.height=Axis::content(24);
        const auto row_key=row.key; controls.push_back(std::move(row));
        Control control;
        control.key=item.key; control.parent=row_key; control.kind=ControlKind::menu_item;
        control.layout=NodeInput::box(0,24); control.layout.height=Axis::content(24); control.layout.width=Axis::expand();
        control.text=item.text; control.enabled=item.enabled; control.selected=item.selected;
        control.edit_command=item.edit_command;
        controls.push_back(std::move(control));
        if (!item.shortcut.empty()) {
            Control shortcut; shortcut.key=allocate(); shortcut.parent=row_key; shortcut.kind=ControlKind::label;
            shortcut.text=item.shortcut; shortcut.enabled=false; shortcut.classes={"menu-shortcut"};
            if (!item.enabled) shortcut.classes.push_back("unavailable");
            shortcut.layout=NodeInput::box(0,24); shortcut.layout.width=Axis::content();
            shortcut.layout.out_of_flow=true; shortcut.layout.anchor_x=shortcut.layout.pivot_x=1;
            shortcut.layout.offset_x=-9;
            controls.push_back(std::move(shortcut));
        }
    }
}
} // namespace atelier::ui
