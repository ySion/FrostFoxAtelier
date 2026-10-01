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
        if (action.key==disable) { disabled_=std::get<bool>(action.value); return true; }
        if (action.key==long_labels) { long_labels_=std::get<bool>(action.value); return true; }
        if (action.key==fold) { expanded_=std::get<bool>(action.value); return true; }
        if (action.key==position_reset) values_.position={};
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
    if (!vector && !four && action.key!=scalar && action.key!=name) return false;
    if (action.kind==ui::ActionKind::edit_begin) {
        active_.emplace(action.key,values_); return true;
    }
    if (!active_ || active_->first!=action.key) return true;
    if (action.kind==ui::ActionKind::edit_cancel) {
        values_=std::move(active_->second); active_.reset(); return true;
    }
    if (action.kind!=ui::ActionKind::edit_preview && action.kind!=ui::ActionKind::edit_commit) return true;
    if (vector) values_.position[action.key-position_x]=std::get<double>(action.value);
    else if (four) values_.vector4[action.key-vector4_x]=std::get<double>(action.value);
    else if (action.key==scalar) values_.exposure=std::get<double>(action.value);
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
    auto scroll=NodeInput::column(7); scroll.height=Axis::expand(); scroll.padding={0,2,2,6};
    add(list,parent,ControlKind::list,scroll).classes={"parameter-list"};
    parent=list;
    add(301,parent,ControlKind::label,full(26),L"Control workbench").role=ui::Role::title;
    auto paragraph=full(0); paragraph.height=Axis::content();
    add(302,parent,ControlKind::label,paragraph,L"Live samples · scene values stay separate").role=ui::Role::caption;
    add(disable,parent,ControlKind::checkbox,full(24),L"Disable sample fields").selected=disabled_;
    add(long_labels,parent,ControlKind::checkbox,full(24),L"Show long labels").selected=long_labels_;
    auto& header=add(fold,list,ControlKind::foldout,full(24),L"Parameters");
    header.selected=expanded_; header.classes={"section"};
    auto content=NodeInput::column(7); content.collapsed=!expanded_;
    add(body,list,ControlKind::container,content).enabled=!disabled_;
    std::array<ui::NumberField,3> vector;
    constexpr std::array names{L"X",L"Y",L"Z"};
    for (std::size_t axis=0;axis<vector.size();++axis)
        vector[axis]={position_x+axis,names[axis],values_.position[axis]};
    ui::append_number_row(controls,{330,332,333,position_reset,body,
        long_labels_?L"Local position in metres":L"Position (m)"},vector);
    const std::array scalar_field{ui::NumberField{scalar,{},values_.exposure,1,0,16,.05,.005}};
    ui::append_number_row(controls,{350,352,353,scalar_reset,body,
        long_labels_?L"Exposure compensation":L"Exposure"},scalar_field);
    ui::append_parameter_row(controls,{356,360,357,0,body,L"Name",true,values_.name!=defaults.name});
    auto& text=add(name,357,ControlKind::text_edit,full(24),values_.name);
    text.modified=values_.name!=defaults.name; text.context_target=name;
    ui::append_parameter_row(controls,{363,364,365,0,body,L""});
    auto& check=add(enabled,365,ControlKind::checkbox,full(24),L"Enabled");
    check.selected=values_.enabled; check.modified=values_.enabled!=defaults.enabled;
    std::array<ui::NumberField,4> vector4;
    constexpr std::array names4{L"X",L"Y",L"Z",L"W"};
    for (std::size_t axis=0;axis<vector4.size();++axis)
        vector4[axis]={vector4_x+axis,names4[axis],values_.vector4[axis],defaults.vector4[axis]};
    ui::append_number_row(controls,{380,382,383,vector4_reset,body,L"Vector4"},vector4);
    ui::append_parameter_row(controls,{417,419,418,0,body,L"Projection",true,values_.mode!=defaults.mode});
    auto& choice=add(mode,418,ControlKind::combo,full(24),mode_names[values_.mode]);
    choice.modified=values_.mode!=defaults.mode; choice.layout.width=Axis::content(120);
    add(reset,parent,ControlKind::button,full(24),L"Reset sample values").enabled=values_!=defaults;
    add(371,parent,ControlKind::label,paragraph,
        L"Sample history  ·  "+std::to_wstring(undo_count())+L" undo / "+std::to_wstring(redo_count())+L" redo").role=ui::Role::caption;
    add(372,parent,ControlKind::label,paragraph,L"Middle drag: value ladder · Right click: menu\nCtrl + middle: defaults · Esc: cancel").role=ui::Role::caption;
}
} // namespace atelier::editor
