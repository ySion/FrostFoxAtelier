#include "editor/world_panel.hpp"
#include "editor/commands.hpp"
#include <algorithm>
#include <format>

namespace atelier::editor {
namespace {
const ViewOptionInfo* info(ViewOption option) {
    const auto options=view_options();const auto found=std::ranges::find(options,option,&ViewOptionInfo::option);
    return found==options.end()?nullptr:&*found;
}
std::wstring title(std::string_view name) {
    std::wstring result;bool capital=true;
    for(char c:name) {if(c=='_') {result+=L' ';capital=true;} else {result+=wchar_t(capital && c>='a' && c<='z'?c-'a'+'A':c);capital=false;}}
    return result;
}
std::wstring choice_title(const ViewOptionInfo& option,double value) {
    if(option.option==ViewOption::antialiasing) {
        constexpr std::array names{L"Off",L"TAA",L"SMAA",L"SMAA T2x"};return names[static_cast<std::size_t>(value)];
    }
    if(option.option==ViewOption::probe_debug_volume) return value<0?L"All volumes":L"Volume "+std::to_wstring(unsigned(value));
    return title(option.choices[static_cast<std::size_t>(value)]);
}
bool same(const renderer::ProbeVolumeStats& a,const renderer::ProbeVolumeStats& b) {
    return a.enabled==b.enabled && a.rolling==b.rolling && a.minimum==b.minimum && a.maximum==b.maximum &&
        a.spacing==b.spacing && a.counts==b.counts && a.resident==b.resident && a.updated==b.updated && a.reset_probes==b.reset_probes;
}
std::wstring vector_text(const std::array<float,3>& v) {return std::format(L"{:.4g} / {:.4g} / {:.4g}",v[0],v[1],v[2]);}
}
double WorldPanel::displayed(const renderer::ViewSettings& view,ViewOption option) const {
    return edit_ && edit_->key==world_control::field(option)?edit_->value:view_value(view,option);
}
Result<bool> WorldPanel::apply(const ui::Action& action,renderer::ViewSettings& candidate) {
    using namespace world_control;
    if(action.kind==ui::ActionKind::activate && action.key>=slot_first && action.key<slot_first+renderer::probe_volume_limit) {
        cancel();slot_=static_cast<unsigned>(action.key-slot_first);return true;
    }
    if(action.kind==ui::ActionKind::activate && action.key>=choice_first &&
        action.key<choice_first+(static_cast<unsigned>(ViewOption::volume_last)+1)*32) {
        const auto offset=action.key-choice_first;
        const auto* option=info(static_cast<ViewOption>(offset/32));
        if(!option) return false;
        if(option->option>=ViewOption::volume_first &&
            (static_cast<unsigned>(option->option)-static_cast<unsigned>(ViewOption::volume_first))/static_cast<unsigned>(ProbeVolumeField::count)!=slot_) return true;
        const auto value=offset%32;
        if(option->option!=ViewOption::probe_debug_volume && (option->kind!=ViewValueKind::enumeration || value>=option->choices.size())) return false;
        if(option->option==ViewOption::probe_debug_volume && value>renderer::probe_volume_limit) return false;
        auto changed=set_view_option(candidate,option->option,double(value)-(option->option==ViewOption::probe_debug_volume?1:0));
        if(!changed) return std::unexpected(changed.error());return true;
    }
    if(action.key<field_first || action.key>=choice_first) return false;
    const auto* option=info(static_cast<ViewOption>(action.key-field_first));if(!option) return false;
    if(option->option>=ViewOption::volume_first &&
        (static_cast<unsigned>(option->option)-static_cast<unsigned>(ViewOption::volume_first))/static_cast<unsigned>(ProbeVolumeField::count)!=slot_) return true;
    if(action.kind==ui::ActionKind::activate && option->kind==ViewValueKind::boolean) {
        const auto* checked=std::get_if<bool>(&action.value);if(!checked) return false;
        auto changed=set_view_option(candidate,option->option,*checked?1:0);if(!changed) return std::unexpected(changed.error());return true;
    }
    if(option->kind==ViewValueKind::boolean || option->kind==ViewValueKind::enumeration) return false;
    if(action.kind==ui::ActionKind::edit_begin) {
        if(!edit_ || edit_->key!=action.key) edit_=Edit{action.key,view_value(candidate,option->option)};
        return true;
    }
    if(!edit_ || edit_->key!=action.key) return true; // A retired row cannot commit into a different volume.
    if(action.kind==ui::ActionKind::edit_cancel) {cancel();return true;}
    if(action.kind!=ui::ActionKind::edit_preview && action.kind!=ui::ActionKind::edit_commit) return true;
    const auto* value=std::get_if<double>(&action.value);if(!value) return false;
    auto proposed=candidate;
    auto changed=set_view_option(proposed,option->option,*value);if(!changed) {if(action.kind==ui::ActionKind::edit_commit)cancel();return std::unexpected(changed.error());}
    edit_->value=view_value(proposed,option->option);
    if(action.kind==ui::ActionKind::edit_commit) {
        cancel();candidate=proposed;
    }
    return true;
}
std::vector<ui::MenuItem> WorldPanel::menu(ui::ControlKey source,const renderer::ViewSettings& view) const {
    using namespace world_control;
    std::vector<ui::MenuItem> items;
    if(source==setup) {
        return {{static_cast<ui::ControlKey>(Command::fit_probe_scene),L"Fit Scene",true,false,{}},
                {static_cast<ui::ControlKey>(Command::fit_probe_scene_focus),L"Scene + Follow Focus",true,false,{}}};
    }
    if(source==slot) {
        for(unsigned i=0;i<renderer::probe_volume_limit;++i) {
            ui::MenuItem item{slot_first+i,L"Volume "+std::to_wstring(i)+(view.lit.probe_volumes[i].enabled?L"":L" (off)"),true,slot_==i,{}};
            item.behavior=ui::MenuBehavior::radio;item.radio_group=slot;items.push_back(std::move(item));
        }
        return items;
    }
    if(source<field_first || source>=choice_first) return items;
    const auto* option=info(static_cast<ViewOption>(source-field_first));if(!option) return items;
    const bool debug=option->option==ViewOption::probe_debug_volume;
    if(option->kind!=ViewValueKind::enumeration && !debug) return items;
    const auto count=debug?renderer::probe_volume_limit+1:option->choices.size();
    for(unsigned i=0;i<count;++i) {
        const auto value=double(i)-(debug?1:0);
        ui::MenuItem item{choice(option->option,i),choice_title(*option,value),true,view_value(view,option->option)==value,{}};
        item.behavior=ui::MenuBehavior::radio;item.radio_group=source;items.push_back(std::move(item));
    }
    return items;
}
void WorldPanel::observe(const renderer::FrameStats& stats) {
    for(std::size_t i=0;i<observed_.size();++i) {
        const auto& value=stats.scene_lit_probe_volumes[i];
        if(same(value,observed_[i]) && !readouts_[i][0].empty()) continue;
        observed_[i]=value;auto& text=readouts_[i];
        text[0]=value.enabled?(value.rolling?L"Following focus":L"Fixed"):L"No resident field";
        text[1]=value.enabled?vector_text(value.minimum):L"—";text[2]=value.enabled?vector_text(value.maximum):L"—";
        text[3]=value.enabled?vector_text(value.spacing):L"—";
        text[4]=value.enabled?std::format(L"{} × {} × {} · {} slots",value.counts[0],value.counts[1],value.counts[2],value.resident):L"—";
        text[5]=value.enabled?std::format(L"{} scheduled · {} reset",value.updated,value.reset_probes):L"—";
    }
}
void WorldPanel::append(std::vector<ui::Control>& controls,ui::ControlKey parent,ui_language::LiveDocument& document,const renderer::ViewSettings& view,bool editable,ui_language::ItemList lights) const {
    using namespace ui_language;namespace key=world_control;
    static const renderer::ViewSettings defaults;
    auto groups=std::make_shared<std::vector<Item>>();
    const auto group=[&](ui::ControlKey id,std::wstring name,std::shared_ptr<std::vector<Item>> rows) {
        groups->push_back({id,{std::move(name),0.,ItemList{std::move(rows)}}});
    };
    const auto row=[&](std::vector<Item>& rows,std::wstring label,std::span<const ViewOption> options) {
        const auto* option=info(options.front());const auto id=key::field(option->option);
        const bool boolean=option->kind==ViewValueKind::boolean;
        const bool enumeration=option->kind==ViewValueKind::enumeration || option->option==ViewOption::probe_debug_volume;
        const bool enabled=editable || !world_option(option->option);
        EditBinding edit{0,enabled,false,{}};
        if(!boolean && !enumeration) for(std::size_t i=0;i<options.size();++i) {
            const auto* item=info(options[i]);const auto value=displayed(view,item->option),initial=view_value(defaults,item->option);
            edit.modified|=value!=initial;const bool integer=item->kind==ViewValueKind::integer;
            constexpr std::array names{L"X",L"Y",L"Z"};
            edit.fields.push_back({key::field(item->option),options.size()==1?L"":names[i],value,initial,item->minimum,item->maximum,
                integer?1.:.05,integer?1.:.005,options.size()==1?ui::VectorAxis::none:static_cast<ui::VectorAxis>(i+1)});
            edit.fields.back().single_precision=!integer;
        }
        rows.push_back({key::root+8192+static_cast<unsigned>(option->option),
            {std::move(label),edit,boolean,view_value(view,option->option)!=0,enabled,ActionBinding{id,enabled},enumeration,
             enumeration?choice_title(*option,view_value(view,option->option)):std::wstring{},id,false,0.,0.,0.,false,false,ActionBinding{id,false}}});
    };
    const auto scalar=[&](std::vector<Item>& rows,std::wstring label,ViewOption option) {row(rows,std::move(label),{&option,1});};
    auto volume=std::make_shared<std::vector<Item>>();
    scalar(*volume,L"Enabled",probe_volume_option(slot_,ProbeVolumeField::enabled));
    scalar(*volume,L"Placement",probe_volume_option(slot_,ProbeVolumeField::placement));
    const bool rolling=view.lit.probe_volumes[slot_].placement==renderer::ProbePlacement::follow_focus;
    for(unsigned kind=0;kind<3;++kind) {
        const auto first=kind==0?ProbeVolumeField::min_x:kind==1?ProbeVolumeField::max_x:ProbeVolumeField::count_x;
        const std::array options{probe_volume_option(slot_,first),probe_volume_option(slot_,static_cast<ProbeVolumeField>(static_cast<unsigned>(first)+1)),
            probe_volume_option(slot_,static_cast<ProbeVolumeField>(static_cast<unsigned>(first)+2))};
        row(*volume,kind==2?L"Probe count":kind==0?(rolling?L"Min offset (m)":L"World min (m)"):(rolling?L"Max offset (m)":L"World max (m)"),options);
    }
    scalar(*volume,L"Blend (cells)",probe_volume_option(slot_,ProbeVolumeField::blend_cells));
    group(key::root+100,L"Volume "+std::to_wstring(slot_),std::move(volume));
    auto volume_groups=groups;groups=std::make_shared<std::vector<Item>>();
    auto global=std::make_shared<std::vector<Item>>();
    scalar(*global,L"DDGI",ViewOption::ddgi);scalar(*global,L"Updates / frame",ViewOption::probe_update_budget);
    scalar(*global,L"Rays / probe",ViewOption::probe_rays);scalar(*global,L"Feedback",ViewOption::probe_bounce);
    scalar(*global,L"Indirect gain",ViewOption::indirect_gain);
    group(key::root+101,L"Propagation",std::move(global));
    auto propagation=groups;groups=std::make_shared<std::vector<Item>>();
    auto debug=std::make_shared<std::vector<Item>>();
    scalar(*debug,L"Direct reference",ViewOption::direct_reference);
    scalar(*debug,L"Feedback visibility",ViewOption::probe_feedback_visibility);
    scalar(*debug,L"Probe spheres",ViewOption::probe_visualization);scalar(*debug,L"Debug volume",ViewOption::probe_debug_volume);
    scalar(*debug,L"Receiver coverage",ViewOption::probe_debug);scalar(*debug,L"Coverage metric",ViewOption::probe_coverage);
    scalar(*debug,L"Volume bounds",ViewOption::probe_volume);scalar(*debug,L"Relocation lines",ViewOption::probe_relocation);
    scalar(*debug,L"X-Ray",ViewOption::probe_xray);scalar(*debug,L"Sphere size",ViewOption::probe_radius);
    group(key::root+102,L"Diagnostics",std::move(debug));
    auto diagnostics=groups;groups=std::make_shared<std::vector<Item>>();
    auto lighting=std::make_shared<std::vector<Item>>();
    scalar(*lighting,L"Exposure (EV)",ViewOption::exposure_ev);
    scalar(*lighting,L"Shadows",ViewOption::shadows);scalar(*lighting,L"Direct candidates",ViewOption::direct_candidates);
    scalar(*lighting,L"Shadow bias (m)",ViewOption::shadow_bias);
    group(key::root+103,L"Lighting",std::move(lighting));
    auto lighting_groups=groups;groups=std::make_shared<std::vector<Item>>();
    auto effects=std::make_shared<std::vector<Item>>();
    scalar(*effects,L"Antialiasing",ViewOption::antialiasing);scalar(*effects,L"GTAO",ViewOption::gtao);
    scalar(*effects,L"Local GI (SSIL)",ViewOption::ssil);scalar(*effects,L"AO radius (m)",ViewOption::ao_radius);
    scalar(*effects,L"Reflections (SSSR)",ViewOption::sssr);scalar(*effects,L"Trace distance (m)",ViewOption::reflection_distance);
    scalar(*effects,L"Max roughness",ViewOption::reflection_roughness);scalar(*effects,L"Bloom",ViewOption::bloom);
    group(key::root+104,L"Effects",std::move(effects));
    auto actual=std::make_shared<std::vector<Item>>();
    constexpr std::array labels{L"Placement",L"World min (m)",L"World max (m)",L"Spacing (m)",L"Resident (CPU)",L"This frame (CPU)"};
    for(std::size_t i=0;i<labels.size();++i) actual->push_back({key::root+128+i,{std::wstring(labels[i]),readouts_[slot_][i].empty()?std::wstring(L"—"):readouts_[slot_][i]}});
    std::uint32_t total{};for(const auto& item:view.lit.probe_volumes) if(item.enabled) total+=item.counts[0]*item.counts[1]*item.counts[2];
    const std::array<InputValue,10> inputs{L"Volume "+std::to_wstring(slot_),ItemList{std::move(volume_groups)},ItemList{std::move(propagation)},ItemList{std::move(actual)},
        std::to_wstring(total)+L" / 4096 configured probes",ItemList{std::move(lighting_groups)},ItemList{std::move(groups)},ItemList{std::move(diagnostics)},editable,std::move(lights)};
    const std::array keys{KeyBinding{"slot",key::slot},KeyBinding{"setup",key::setup},KeyBinding{"scroll",key::root+80},KeyBinding{"lighting_tab",key::lighting_tab},KeyBinding{"ddgi_tab",key::ddgi_tab},KeyBinding{"effects_tab",key::effects_tab},KeyBinding{"diagnostics_tab",key::diagnostics_tab}};
    static_cast<void>(document.append("WorldPanel",parent,keys,inputs,controls));
}
}
