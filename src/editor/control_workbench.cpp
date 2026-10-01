#include "editor/control_workbench.hpp"
#include <algorithm>

namespace atelier::editor {
void ControlWorkbench::remember() {
    if (values_==history_[cursor_]) return;
    history_.erase(history_.begin()+static_cast<std::ptrdiff_t>(cursor_+1),history_.end());
    history_.push_back(values_);
    if (history_.size()>65) history_.erase(history_.begin());
    cursor_=history_.size()-1;
}
void ControlWorkbench::undo() {
    if (active_) { values_=std::move(active_->second); active_.reset(); }
    if (cursor_) values_=history_[--cursor_];
}
void ControlWorkbench::redo() {
    if (active_) { values_=std::move(active_->second); active_.reset(); }
    if (cursor_+1<history_.size()) values_=history_[++cursor_];
}
bool ControlWorkbench::apply(const ui::Action& action) {
    using namespace workbench_control;
    if (action.kind==ui::ActionKind::activate || action.kind==ui::ActionKind::reset) {
        if (action.key==general_page || action.key==appearance_page) { page_=action.key; return true; }
        if (action.key==rgb_page || action.key==channels_page) { color_page_=action.key; return true; }
        if (action.key==color_fold) { color_expanded_=std::get<bool>(action.value); return true; }
        if (action.key==disable) { disabled_=std::get<bool>(action.value); return true; }
        if (action.key==long_labels) { long_labels_=std::get<bool>(action.value); return true; }
        if (action.key==fold) { expanded_=std::get<bool>(action.value); return true; }
        if (action.key==color_reset) values_.color=Values{}.color;
        else if (action.key==position_reset) values_.position={};
        else if (action.key==vector4_reset) values_.vector4=Values{}.vector4;
        else if (action.key==scalar_reset) values_.exposure=1;
        else if (action.key==enabled) values_.enabled=std::get<bool>(action.value);
        else if (action.key==reset) values_=Values{};
        else if (action.key>=mode_first && action.key<mode_first+mode_names.size()) values_.mode=action.key-mode_first;
        else return false;
        remember(); return true;
    }
    const bool vector=action.key>=position_x && action.key<=position_z;
    const bool four=action.key>=vector4_x && action.key<=vector4_w;
    const bool color=action.key>=red && action.key<=blue;
    const bool channel_slider=action.key>=480 && action.key<=482;
    if (!vector && !four && !color && !channel_slider && action.key!=scalar && action.key!=scalar_slider && action.key!=name) return false;
    if (action.kind==ui::ActionKind::edit_begin) {
        active_.emplace(action.key,values_); return true;
    }
    if (!active_ || active_->first!=action.key) return true;
    if (action.kind==ui::ActionKind::edit_cancel) {
        values_=std::move(active_->second); active_.reset(); return true;
    }
    if (action.kind!=ui::ActionKind::edit_preview && action.kind!=ui::ActionKind::edit_commit) return true;
    if (color) values_.color[action.key-red]=std::get<double>(action.value);
    else if (channel_slider) values_.color[action.key-480]=std::get<double>(action.value);
    else if (vector) values_.position[action.key-position_x]=std::get<double>(action.value);
    else if (four) values_.vector4[action.key-vector4_x]=std::get<double>(action.value);
    else if (action.key==scalar || action.key==scalar_slider) values_.exposure=std::get<double>(action.value);
    else values_.name=std::get<std::wstring>(action.value);
    if (action.kind==ui::ActionKind::edit_commit) { remember(); active_.reset(); }
    return true;
}
void ControlWorkbench::append(std::vector<ui::Control>& controls, ui::ControlKey parent) const {
    static const Values defaults;
    using namespace ui_layout;
    using ui::ControlKind;
    using namespace workbench_control;
    const auto add=[&](ui::ControlKey key,ui::ControlKey owner,ControlKind kind,NodeInput layout,
                       std::wstring text={}) -> ui::Control& {
        ui::Control control;
        control.key=key; control.parent=owner; control.kind=kind;
        control.layout=std::move(layout); control.text=std::move(text);
        controls.push_back(std::move(control)); return controls.back();
    };
    const auto full=[](float height) { auto box=NodeInput::box(0,height); box.width=Axis::expand(); return box; };
    ui::append_object_header(controls,301,parent,302,name,L"Sample",values_.name,!disabled_);
    controls.back().modified=values_.name!=defaults.name;
    auto page_stack=NodeInput::column(); page_stack.height=Axis::expand();
    add(445,parent,ControlKind::container,page_stack).classes={"parameter-page"};
    const std::array tabs{ui::ParameterTab{general_page,L"Parameters"},ui::ParameterTab{appearance_page,L"Appearance"}};
    ui::append_parameter_tabs(controls,pages,445,page_,tabs);
    auto scroll=NodeInput::column(7); scroll.height=Axis::expand(); scroll.padding={0,0,0,8};
    add(list,445,ControlKind::list,scroll).classes={"parameter-list"};
    parent=list;
    auto paragraph=full(0); paragraph.height=Axis::content();
    auto general=NodeInput::column(7); general.collapsed=page_!=general_page;
    add(444,list,ControlKind::container,general);
    auto options=NodeInput::column(7); options.padding={8,7.333f,0,0};
    add(446,444,ControlKind::container,options);
    add(disable,446,ControlKind::checkbox,full(24),L"Disable sample fields").selected=disabled_;
    add(long_labels,446,ControlKind::checkbox,full(24),L"Show long labels").selected=long_labels_;
    auto& header=add(fold,444,ControlKind::foldout,full(28),L"Transform and display");
    header.selected=expanded_; header.classes={"section"};
    auto content=NodeInput::column(7); content.collapsed=!expanded_;
    auto& general_body=add(body,444,ControlKind::container,content);
    general_body.enabled=!disabled_; general_body.classes={"parameter-inset"};
    std::array<ui::NumberField,3> vector;
    constexpr std::array names{L"X",L"Y",L"Z"};
    for (std::size_t axis=0;axis<vector.size();++axis)
        vector[axis]={position_x+axis,names[axis],values_.position[axis],0,-10000,10000,.01,.001,static_cast<ui::VectorAxis>(axis+1)};
    ui::append_number_row(controls,{330,332,333,position_reset,body,
        long_labels_?L"Local position in metres":L"Position (m)"},vector);
    const std::array scalar_field{ui::NumberField{scalar,{},values_.exposure,1,0,32,.05,.005,ui::VectorAxis::none,ui::SliderRange{scalar_slider,0,16}}};
    ui::append_number_row(controls,{350,352,353,scalar_reset,body,
        long_labels_?L"Exposure compensation":L"Exposure"},scalar_field);
    ui::append_parameter_row(controls,{363,364,365,0,body,L""});
    auto& check=add(enabled,365,ControlKind::checkbox,full(24),L"Enabled");
    check.selected=values_.enabled; check.modified=values_.enabled!=defaults.enabled;
    std::array<ui::NumberField,4> vector4;
    constexpr std::array names4{L"X",L"Y",L"Z",L"W"};
    for (std::size_t axis=0;axis<vector4.size();++axis)
        vector4[axis]={vector4_x+axis,names4[axis],values_.vector4[axis],defaults.vector4[axis],-10000,10000,.01,.001,static_cast<ui::VectorAxis>(axis+1)};
    ui::append_number_row(controls,{380,382,383,vector4_reset,body,L"Vector4"},vector4);
    ui::append_parameter_row(controls,{417,419,418,0,body,L"Projection",true,values_.mode!=defaults.mode});
    auto& choice=add(mode,418,ControlKind::combo,full(24),mode_names[values_.mode]);
    choice.layout.height=Axis::fixed(26);
    choice.modified=values_.mode!=defaults.mode; choice.layout.width=Axis::content(120);
    auto appearance=NodeInput::column(); appearance.collapsed=page_!=appearance_page;
    auto& appearance_container=add(appearance_body,list,ControlKind::container,appearance);
    appearance_container.enabled=!disabled_; appearance_container.classes={"parameter-body"};
    const std::array color_tabs{ui::ParameterTab{rgb_page,L"Color"},ui::ParameterTab{channels_page,L"Channels"}};
    ui::append_parameter_tabs(controls,color_pages,appearance_body,color_page_,color_tabs);
    auto nested=NodeInput::column(7); nested.padding.top=7.333f;
    add(455,appearance_body,ControlKind::container,nested).classes={"parameter-nested-body"};
    auto swatch_row=NodeInput::column();
    add(456,455,ControlKind::container,swatch_row).classes={"parameter-inset"};
    auto& swatch_control=add(swatch,456,ControlKind::container,full(48));
    swatch_control.style.background=ui::Color{static_cast<float>(values_.color[0]),static_cast<float>(values_.color[1]),static_cast<float>(values_.color[2]),1};
    swatch_control.classes={"color-swatch"};
    auto& color_header=add(color_fold,455,ControlKind::foldout,full(28),L"Color components");
    color_header.selected=color_expanded_; color_header.classes={"section"};
    auto color_layout=NodeInput::column(7); color_layout.collapsed=!color_expanded_;
    add(color_body,455,ControlKind::container,color_layout).classes={"parameter-inset"};
    std::array<ui::NumberField,3> rgb;
    constexpr std::array color_names{L"Red",L"Green",L"Blue"};
    for (std::size_t axis=0;axis<rgb.size();++axis)
        rgb[axis]={red+axis,color_names[axis],values_.color[axis],defaults.color[axis],0,1,.01,.001,static_cast<ui::VectorAxis>(axis+1)};
    if (color_page_==rgb_page) ui::append_number_row(controls,{463,464,465,color_reset,color_body,L"Color"},rgb);
    else for (std::size_t axis=0;axis<rgb.size();++axis) {
        rgb[axis].slider=ui::SliderRange{480+axis,0,1};
        ui::append_number_row(controls,{490+axis*3,491+axis*3,492+axis*3,0,color_body,color_names[axis]},std::span(rgb).subspan(axis,1));
    }
    add(471,455,ControlKind::label,full(28),L"Preview").classes={"parameter-section"};
    paragraph.padding.left=8;
    add(472,455,ControlKind::label,paragraph,L"RGB values share one color. Switching tabs preserves edits; Undo restores the previous color.").role=ui::Role::caption;
    auto footer=NodeInput::column(7); footer.padding.left=8;
    add(447,parent,ControlKind::container,footer); parent=447;
    paragraph.padding.left=0;
    add(reset,parent,ControlKind::button,full(24),L"Reset sample values").enabled=values_!=defaults;
    add(371,parent,ControlKind::label,paragraph,
        L"Sample history  ·  "+std::to_wstring(undo_count())+L" undo / "+std::to_wstring(redo_count())+L" redo").role=ui::Role::caption;
    add(372,parent,ControlKind::label,paragraph,L"Middle drag: value ladder · Right click: menu\nCtrl + middle: defaults · Esc: cancel").role=ui::Role::caption;
}
} // namespace atelier::editor
