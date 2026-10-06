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
void ControlWorkbench::append(std::vector<ui::Control>& controls,ui::ControlKey parent,ui_language::LiveDocument& document) const {
    using namespace workbench_control;
    using namespace ui_language;
    static const Values defaults;
    EditBinding position{position_reset,true,values_.position!=defaults.position,{}};
    EditBinding vector{vector4_reset,true,values_.vector4!=defaults.vector4,{}};
    EditBinding color{color_reset,true,values_.color!=defaults.color,{}};
    constexpr std::array names{L"X",L"Y",L"Z",L"W"};
    constexpr std::array colors{L"Red",L"Green",L"Blue"};
    for (std::size_t axis=0;axis<4;++axis) {
        const auto channel=static_cast<ui::VectorAxis>(axis+1);
        vector.fields.push_back({vector4_x+axis,names[axis],values_.vector4[axis],defaults.vector4[axis],-10000,10000,.01,.001,channel});
        if (axis==3) continue;
        position.fields.push_back({position_x+axis,names[axis],values_.position[axis],0,-10000,10000,.01,.001,channel});
        color.fields.push_back({red+axis,colors[axis],values_.color[axis],defaults.color[axis],0,1,.01,.001,channel});
    }
    EditBinding exposure{scalar_reset,true,values_.exposure!=1,
        {{scalar,L"",values_.exposure,1,0,32,.05,.005,ui::VectorAxis::none,ui::SliderRange{scalar_slider,0,16}}}};
    std::array<EditBinding,3> channels;
    for (std::size_t axis=0;axis<3;++axis) {
        auto field=color.fields[axis];field.slider=ui::SliderRange{480+axis,0,1};
        channels[axis]={0,true,field.value!=field.default_value,{std::move(field)}};
    }
    const std::array<InputValue,26> inputs{TextEditBinding{name,values_.name,!disabled_},values_.name!=defaults.name,
        disabled_,long_labels_,expanded_,page_==general_page,color_page_==rgb_page,color_expanded_,values_.enabled,values_.enabled!=defaults.enabled,
        std::wstring(mode_names[values_.mode]),values_.mode!=defaults.mode,position,exposure,vector,color,channels[0],channels[1],channels[2],
        values_.color[0],values_.color[1],values_.color[2],values_!=defaults,
        L"Sample history  \u00b7  "+std::to_wstring(undo_count())+L" undo / "+std::to_wstring(redo_count())+L" redo",std::wstring(long_labels_?L"Local position in metres":L"Position (m)"),
        std::wstring(long_labels_?L"Exposure compensation":L"Exposure")};
    const std::array keys{KeyBinding{"header",301},KeyBinding{"type",302},KeyBinding{"pages",445},KeyBinding{"tabs",pages},
        KeyBinding{"general",general_page},KeyBinding{"appearance",appearance_page},KeyBinding{"scroll",list},KeyBinding{"general_body",444},
        KeyBinding{"options",446},KeyBinding{"disable",disable},KeyBinding{"long_labels",long_labels},KeyBinding{"fold",fold},KeyBinding{"body",body},
        KeyBinding{"position",330,332,333},KeyBinding{"exposure",350,352,353},KeyBinding{"enabled_row",363,364,365},KeyBinding{"enabled",enabled},
        KeyBinding{"vector4",380,382,383},KeyBinding{"projection",417,419,418},KeyBinding{"mode",mode},KeyBinding{"appearance_body",appearance_body},
        KeyBinding{"color_tabs",color_pages},KeyBinding{"rgb",rgb_page},KeyBinding{"channels",channels_page},KeyBinding{"nested",455},
        KeyBinding{"swatch_row",456},KeyBinding{"swatch",swatch},KeyBinding{"color_fold",color_fold},KeyBinding{"color_body",color_body},
        KeyBinding{"rgb_fields.color",463,464,465},KeyBinding{"channel_fields.red",490,491,492},KeyBinding{"channel_fields.green",493,494,495},
        KeyBinding{"channel_fields.blue",496,497,498},KeyBinding{"preview",471},KeyBinding{"explanation",472},KeyBinding{"footer",447},
        KeyBinding{"reset",reset},KeyBinding{"history",371},KeyBinding{"gestures",372}};
    static_cast<void>(document.append("Controls",parent,keys,inputs,controls));
}
} // namespace atelier::editor
