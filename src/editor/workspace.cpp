#include "editor/workspace.hpp"
#include "editor/assets.hpp"
#include "editor/fields.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <cwctype>

namespace atelier::editor {
namespace {
struct LightingOption {std::wstring_view title;bool renderer::SceneLitSettings::*field;};
constexpr std::array lighting_options{
    LightingOption{L"Shadows",&renderer::SceneLitSettings::shadows},
    LightingOption{L"DDGI",&renderer::SceneLitSettings::ddgi},
    LightingOption{L"GTAO",&renderer::SceneLitSettings::gtao},
    LightingOption{L"SSSR",&renderer::SceneLitSettings::sssr},
    LightingOption{L"TAA",&renderer::SceneLitSettings::taa},
    LightingOption{L"Bloom",&renderer::SceneLitSettings::bloom}};
}
Workspace::Workspace() : dock(1ULL<<48,{
    {control::scene_tab,control::scene_content,L"Scene View",{240,160}},
    {control::parameters_tab,50,L"Parameters",{220,260}},
    {control::workbench,300,L"Controls",{220,280}},
    {control::actors_tab,21,L"Scene Tree",{180,140}},
    {control::structure_tab,control::structure_content,L"Actor Structure",{200,140}},
    {control::assets_tab,control::assets_content,L"Asset Browser",{240,140}},
    {source_control::tab,source_control::content,L"Code",{300,180}},
    {control::output_tab,control::output_content,L"Project Output",{240,120}}}) {
    const auto registered=ui_language::register_types(ui_runtime_);
    if (!registered) throw std::runtime_error(registered.error().message);
    auto messages=std::make_shared<ui::TableData>();messages->columns.push_back({1,L"Message",1000,160,ui::TextAlign::left,false});output=std::move(messages);
    reset_layout();
}
void Workspace::log(std::wstring text) {
    auto messages=std::make_shared<ui::TableData>(*output);
    std::size_t start{};
    while(start<text.size()) {
        const auto end=text.find(L'\n',start);auto line=text.substr(start,end==text.npos?text.size()-start:end-start);
        if(!line.empty() && line.back()==L'\r') line.pop_back();
        if(line.size()>2048) {line.resize(2048);if(line.back()>=0xd800 && line.back()<=0xdbff) line.pop_back();line+=L"…";}
        if(!line.empty()) messages->rows.push_back({next_output_++,{{1,std::move(line)}}});
        if(end==text.npos) break;start=end+1;
    }
    if(messages->rows.size()>256) messages->rows.erase(messages->rows.begin(),messages->rows.end()-256);
    output=std::move(messages);
}
void Workspace::reset_layout() {
    dock.reset();
    dock.move(control::scene_tab,control::parameters_tab,ui::DockSide::left,.62f);
    dock.move(control::actors_tab,control::parameters_tab,ui::DockSide::bottom,.7f);
    dock.move(control::structure_tab,control::actors_tab,ui::DockSide::tabs);
    dock.select(control::actors_tab);
    dock.move(control::assets_tab,control::scene_tab,ui::DockSide::bottom,.73f);
    dock.move(source_control::tab,control::scene_tab,ui::DockSide::tabs);
    dock.move(control::output_tab,control::assets_tab,ui::DockSide::tabs);
    dock.select(control::assets_tab);
    dock.select(control::scene_tab);
    dock.select(control::parameters_tab); menu.reset(); menu_bar_collapsed=shelf_collapsed=false;
}
void Workspace::reset_document() {
    if (ui_document_ && ui_document_->committed_source()) ui_checkpoint_=ui_document_->committed_source();
    ui_document_.reset();
    bindings_.clear(); binding_cursor_=0; tree_expansion_.clear(); tree_frame_=0;
    navigation_={ObjectId{}}; navigation_cursor_=0; query.clear(); tree_query.clear(); structure_query.clear(); parameter_search_visible=false;
    inspecting_asset=false;
    menu.reset(); assets.reset(project_open);
    component_choices_.clear(); property_choices_.clear(); menu_selection_={};
}
std::optional<TypeId> Workspace::component_choice(ui::ControlKey key) const noexcept {
    if(key<control::component_choice_first || key-control::component_choice_first>=component_choices_.size()) return {};
    return component_choices_[static_cast<std::size_t>(key-control::component_choice_first)].type;
}
std::optional<Workspace::PropertyChoice> Workspace::property_choice(ui::ControlKey key) const {
    if(key<control::property_choice_first || key-control::property_choice_first>=property_choices_.size()) return {};
    return property_choices_[static_cast<std::size_t>(key-control::property_choice_first)];
}
Result<bool> Workspace::apply_ui(const ui::Action& action) {
    return ui_document_?ui_document_->event(action):Result<bool>{false};
}
void Workspace::commit_ui_controls() {
    if (!ui_document_) return;
    const auto revision=ui_document_->revision();ui_document_->commit_controls();
    if (ui_document_->revision()!=revision && revision)
        status=L"UI updated: revision "+std::to_wstring(ui_document_->revision())+L" ("+
            std::to_wstring(static_cast<unsigned>(ui_document_->compile_milliseconds()))+L" ms compile)";
    if (ui_runtime_.collection_phase()==CollectionPhase::idle) {
        const auto started=ui_runtime_.begin_collection();
        if (!started) throw std::runtime_error(started.error().message);
    }
    const auto collected=ui_runtime_.collect_step({32});
    if (!collected) throw std::runtime_error(collected.error().message);
}
bool Workspace::reject_ui_controls(std::string message) {
    return ui_document_ && ui_document_->reject_controls(std::move(message));
}
void Workspace::set_ui_source(std::filesystem::path path) {
    if (ui_document_) throw std::logic_error("Choose the authoring file before opening the UI document.");
    ui_source_=std::move(path);
}
std::filesystem::path Workspace::ui_source() const {
    if (!ui_source_.empty()) return ui_source_;
    const std::filesystem::path authoring{ATELIER_UI_AUTHORING_PATH};std::error_code error;
    return std::filesystem::is_regular_file(authoring,error)?authoring:asset_directory()/"editor.ui.source";
}
const Workspace::Binding* Workspace::binding(ui::ControlKey key) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [key](const auto& item) { return item.key == key; });
    return found == bindings_.end() ? nullptr : &*found;
}
ui::ControlKey Workspace::object_control(ObjectId object) const noexcept { return field_control(object, {}); }
ui::ControlKey Workspace::field_control(ObjectId object, std::span<const FieldId> path) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
        return (item.kind==(path.empty()?Binding::Kind::object:Binding::Kind::field) || (!path.empty() && item.kind==Binding::Kind::rotation)) &&
            item.object == object && std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
    });
    return found == bindings_.end() ? 0 : found->key;
}
ui::ControlKey Workspace::reset_control(ObjectId object) const noexcept {
    const auto found=std::find_if(bindings_.begin(),bindings_.end(),[&](const auto& item) { return item.kind==Binding::Kind::position_default && item.object==object; });
    return found==bindings_.end()?0:found->key;
}
ui::ControlKey Workspace::name_control(ObjectId object) const noexcept {
    const auto found=std::find_if(bindings_.begin(),bindings_.end(),[&](const auto& item) { return item.kind==Binding::Kind::name && item.object==object; });
    return found==bindings_.end()?0:found->key;
}
bool Workspace::can_navigate(int direction) const noexcept {
    return direction<0?navigation_cursor_>0:navigation_cursor_+1<navigation_.size();
}
Result<void> Workspace::navigate(Session& session,int direction) {
    if (!can_navigate(direction)) return {};
    const auto next=direction<0?navigation_cursor_-1:navigation_cursor_+1;
    auto changed=session.select(navigation_[next]);
    if (changed) {navigation_cursor_=next;inspecting_asset=false;}
    return changed;
}
Result<void> Workspace::select_tree(Session& session,ui::ControlKey key,ui::SelectionModifiers modifiers,const ui::UiFrame* frame) {
    inspecting_asset=false;
    if (key==control::tree_world) return session.select({});
    const auto* target=binding(key);
    if(target && target->kind==Binding::Kind::object && (modifiers.shift || modifiers.control)) {
        std::vector<ObjectId> selected;
        if(modifiers.control) selected.assign(session.selected_actors().begin(),session.selected_actors().end());
        if(modifiers.shift) {
            const auto members=session.world().members();if(!members) return std::unexpected(members.error());
            std::vector<ObjectId> visible;
            for(const auto& actor:*members) if(!frame || frame->find(object_control(actor.id()))) visible.push_back(actor.id());
            const auto first=std::ranges::find(visible,session.selection().actor),last=std::ranges::find(visible,target->object);
            if(first!=visible.end() && last!=visible.end()) {
                for(auto at=std::min(first,last);at<=std::max(first,last);++at)
                    if(std::ranges::find(selected,*at)==selected.end()) selected.push_back(*at);
            } else selected.push_back(target->object);
        } else if(std::ranges::find(selected,target->object)!=selected.end()) std::erase(selected,target->object);
        else selected.push_back(target->object);
        return session.select_actors(selected,target->object);
    }
    return target && (target->kind==Binding::Kind::object || target->kind==Binding::Kind::structure)?session.select(target->object):Result<void>{};
}
bool Workspace::command_enabled(Command command,const Session& session) const {
    switch (command) {
    case Command::new_source: case Command::new_project_ui: case Command::build_project: return project_open;
    case Command::rebuild_project:return project_open && !project_running;
    case Command::run_project:return project_open && scene_available && !project_running && project_build && project_build->state==BuildState::ready;
    case Command::stop_project:return project_running && !project_stopping;
    case Command::edit_project_ui:return project_open && project_ui_configured;
    case Command::show_project_ui:return project_open && project_ui_configured && !project_running;
    case Command::save_scene: case Command::save_scene_as:return project_open && scene_available;
    case Command::open_scene:
    case Command::import_mesh: case Command::refresh_assets: return project_open;
    case Command::save_document: return source_history()?source_editor.active()!=nullptr:project_open && scene_available;
    case Command::close_source: case Command::reload_source: return source_editor.active()!=nullptr;
    case Command::undo: if (source_history()) return source_editor.active() && source_editor.active()->document->edit().can_undo();return (sample_history()?workbench.undo_count():session.undo_count())!=0;
    case Command::redo: if (source_history()) return source_editor.active() && source_editor.active()->document->edit().can_redo();return (sample_history()?workbench.redo_count():session.redo_count())!=0;
    case Command::create_actor: case Command::create_static_mesh: {
        const auto members=session.world().members(); return scene_available && members && members->size()<Session::maximum_actors;
    }
    case Command::selection_grid:case Command::selection_cursor:case Command::cursor_selected:
    case Command::erase_selected: case Command::focus: {
        if (session.selection().actor==ObjectId{}) return false;
        const auto state=world::inspect_actor(session.world().runtime(),session.selection().actor);
        if (!state || !state->can_edit) return false;
        if (command==Command::erase_selected) return true;
        const auto root=world::root_component(session.world().runtime(),session.selection().actor);
        return root && root->has_value();
    }
    case Command::previous_tab: case Command::next_tab: case Command::close_tab: case Command::maximize_pane:
        return dock.focused_tab()!=0;
    default: return true;
    }
}
ui::ControlKey Workspace::bind(ObjectId object, std::span<const FieldId> path, Binding::Kind kind) {
    if (!path.empty() && kind==Binding::Kind::object) kind=Binding::Kind::field;
    const auto first=bindings_.begin()+static_cast<std::ptrdiff_t>(binding_cursor_);
    const auto found=std::find_if(first,bindings_.end(),[&](const auto& item) {
        return item.kind==kind && item.object==object && std::equal(item.path.begin(),item.path.end(),path.begin(),path.end());
    });
    if (found!=bindings_.end()) { std::iter_swap(first,found); return bindings_[binding_cursor_++].key; }
    if (next_binding_==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("Editor control identities exhausted.");
    bindings_.push_back({next_binding_++,object,{path.begin(),path.end()},kind});
    std::iter_swap(bindings_.begin()+static_cast<std::ptrdiff_t>(binding_cursor_),bindings_.end()-1);
    return bindings_[binding_cursor_++].key;
}
bool Workspace::apply_view(const ui::Action& action) {
    if (action.kind!=ui::ActionKind::activate) return false;
    const auto key=action.key;
    if (key>=control::shading_first && key<control::shading_first+3)
        view.shading=static_cast<renderer::Shading>(key-control::shading_first);
    else if(key==control::shading_scene_lit) view.shading=renderer::Shading::scene_lit;
    else if(key>=control::lit_effects_first && key<control::lit_effects_first+lighting_options.size()) {
        const auto* checked=std::get_if<bool>(&action.value);if(!checked) return false;
        view.lit.*lighting_options[key-control::lit_effects_first].field=*checked;
    } else if(key>=control::lit_exposure_first && key<control::lit_exposure_first+5)
        view.lit.exposure_ev=float(int(key-control::lit_exposure_first)-2);
    else if(key>=control::lit_shadow_budget_first && key<control::lit_shadow_budget_first+3)
        view.lit.shadow_light_budget=4u<<static_cast<unsigned>(key-control::lit_shadow_budget_first);
    else if (key>=control::edges_first && key<control::edges_first+4) {
        const auto option=key-control::edges_first;
        view.wireframe=option<2?static_cast<renderer::Wireframe>(option):renderer::Wireframe::only;
        view.hidden_lines=option!=3;
    } else if (key>=control::topology_first && key<control::topology_first+2)
        view.triangle_edges=key!=control::topology_first;
    else if (key==control::guide_background || key==control::guide_grid || key==control::guide_axes) {
        const auto* checked=std::get_if<bool>(&action.value);
        if (!checked) return false;
        if (key==control::guide_background) view.background=*checked?renderer::Background::gradient:renderer::Background::none;
        else if (key==control::guide_grid) view.reference_grid=*checked;
        else view.origin_axes=*checked;
    }
    else return false;
    status=L"Viewport display updated";
    return true;
}
namespace {
// Houdini-style parameter label from a reflected field path: "material.base_color"
// becomes "Base Color". The path still identifies the field; only the caption changes.
std::wstring parameter_label(std::string_view path) {
    if(const auto dot=path.rfind('.');dot!=path.npos) path=path.substr(dot+1);
    std::wstring label;bool start=true;
    for(const char c:path) {
        if(c=='_') {label+=L' ';start=true;continue;}
        label+=static_cast<wchar_t>(start && c>='a' && c<='z'?c-'a'+'A':c);start=false;
    }
    return label;
}
}
std::vector<ui::Control> Workspace::describe(const Session& session, ui_layout::Size size, const ui::UiFrame* frame,const Project* project) {
    const auto take = []<class T>(Result<T> value) -> T {
        if (!value) throw std::runtime_error(value.error().message);
        return std::move(*value);
    };
    using namespace ui_layout;
    using ui::ControlKind;
    constexpr std::array shading_titles{L"Smooth Shaded",L"Flat Shaded",L"MatCap Shaded"};
    constexpr std::array edge_titles{L"Shading Only",L"Shading + Wireframe",L"Wireframe · Visible Edges",L"Wireframe · All Edges"};
    constexpr std::array topology_titles{L"Polygon Edges",L"Triangle Edges"};
    const auto edge_option=view.wireframe==renderer::Wireframe::only?
        (view.hidden_lines?2u:3u):static_cast<unsigned>(view.wireframe);
    auto& runtime = session.world().runtime();
    if (!ui_document_) {
        auto recovery=ui_checkpoint_;
        if (!recovery) if (auto bundled=read_file(asset_directory()/"editor.ui.source",1024*1024);bundled) recovery=std::move(*bundled);
        ui_document_=std::make_unique<ui_language::LiveDocument>(ui_runtime_,ui_source(),next_binding_,std::move(recovery));
    }
    source_editor.poll(frame && frame->input_anchor && frame->input_anchor->token.key==source_control::buffer && frame->input_anchor->composition_id);
    ui_document_->poll();
    project_ui_configured=project && !project->ui_source().empty();
    const auto members = take(session.world().members());
    std::unordered_set<ObjectId> live; live.reserve(members.size()*3);
    for (const auto& member:members) {
        live.insert(member.id());
        for (const auto& component:take(world::components(runtime,member.id()))) live.insert(component.id());
    }
    // A stalled accepted frame must not reinsert keys retired on a prior projection.
    if (frame && frame->drawing->id!=tree_frame_) {
        for (const auto& row:frame->tree_rows) tree_expansion_[row.key]=row.expanded;
        tree_frame_=frame->drawing->id;
    }
    // Closing/hiding a view is not an object lifetime change. Retire bindings and
    // their view state together; undo later restores identity with fresh keys.
    std::erase_if(bindings_,[&](const auto& item) {
        if (live.contains(item.object)) return false;
        tree_expansion_.erase(item.key);
        return true;
    });
    binding_cursor_=0;
    std::vector<ui::Control> result;
    result.reserve(64 + members.size());
    const auto requested_selection=session.selection().actor;
    const auto selected=std::ranges::any_of(members,[&](const auto& member){return member.id()==requested_selection;})?
        requested_selection:ObjectId{};
    const auto inspected=selected!=ObjectId{} && live.contains(session.selection().object)?session.selection().object:ObjectId{};
    const auto old_cursor=navigation_cursor_;
    std::size_t destination{};
    for (std::size_t i=0;i<navigation_.size();++i) {
        const auto id=navigation_[i];
        if (id!=ObjectId{} && !live.contains(id)) continue;
        navigation_[destination]=id;
        if (i<=old_cursor) navigation_cursor_=destination;
        ++destination;
    }
    navigation_.resize(destination); // The World entry always survives.
    if (navigation_[navigation_cursor_]!=inspected) {
        navigation_.resize(navigation_cursor_+1); navigation_.push_back(inspected);
        if (navigation_.size()>64) navigation_.erase(navigation_.begin()+1); // Keep World reachable.
        navigation_cursor_=navigation_.size()-1;
    }
    const auto actor_status = selected == ObjectId{} ? world::ActorStatus{} : take(world::inspect_actor(runtime, selected));
    const auto transform = selected == ObjectId{} ? std::optional<ObjectId>{} : take(world::root_component(runtime, selected));
    auto status_text=status;
    if(!render_notice.empty()) status_text+=L"  |  "+render_notice;
    if (!ui_error().empty()) {
        auto diagnostic=wide(std::string(ui_error()));
        if (diagnostic.size()>512) {diagnostic.resize(512);if (diagnostic.back()>=0xd800 && diagnostic.back()<=0xdbff) diagnostic.pop_back();}
        status_text=L"UI: "+diagnostic;
    }
    auto menus=std::make_shared<std::vector<ui_language::Item>>();
    for (const auto& group:command_groups)
        menus->push_back({group.key,{std::wstring(group.title),menu && menu->source==group.key,group.key}});
    const std::array<ui_language::InputValue,9> shell_inputs{menu_bar_collapsed,ui_language::ItemList{std::move(menus)},
        status_text+L"  |  "+std::to_wstring(members.size())+L" Actors",ui_language::ActionBinding{control::menu_bar_handle},
        double(ui::stow_rail_width),double(ui::stow_collapsed_height),double(ui::stow_arrow_region_height),
        double(ui::ControlIcon::up),double(ui::ControlIcon::down)};
    const std::array<ui_language::KeyBinding,8> shell_keys{{{"root",1},{"menubar",2},{"stow",control::menu_bar_handle},
        {"menu_body",control::menu_bar_body},{"shelf_slot",3},{"workspace",20},{"footer",100},{"status",101}}};
    static_cast<void>(ui_document_->append("EditorShell",0,shell_keys,shell_inputs,result));
    auto create_tools=std::make_shared<std::vector<ui_language::Item>>();
    auto view_tools=std::make_shared<std::vector<ui_language::Item>>();
    for (std::size_t index=0;index<command_definitions.size();++index) {
        const auto& definition=command_definitions[index];
        if (definition.group!=CommandGroup::create && definition.group!=CommandGroup::view) continue;
        if (definition.group==CommandGroup::view && definition.shelf_title.empty()) continue;
        if (!shelf_items_[index]) shelf_items_[index]=next_binding_++;
        const auto title=definition.shelf_title.empty()?definition.title:definition.shelf_title;
        const auto icon=definition.id>=Command::create_point_light && definition.id<=Command::create_directional_light?ui::ControlIcon::component:
            definition.id==Command::create_static_mesh?ui::ControlIcon::mesh:
            definition.id==Command::create_actor?ui::ControlIcon::object:ui::ControlIcon::axes;
        auto& items=definition.group==CommandGroup::create?create_tools:view_tools;
        items->push_back({shelf_items_[index],{std::wstring(title),static_cast<double>(icon),
            ui_language::ActionBinding{static_cast<ui::ControlKey>(definition.id),command_enabled(definition.id,session)}}});
    }
    const std::array<ui_language::InputValue,12> shelf_inputs{shelf_collapsed,shelf_page==control::shelf_create,
        ui_language::ItemList{std::move(create_tools)},ui_language::ItemList{std::move(view_tools)},
        ui_language::ActionBinding{control::shelf_handle},ui_language::ActionBinding{control::shelf_create},ui_language::ActionBinding{control::shelf_view},
        static_cast<double>(ui::stow_rail_width),static_cast<double>(ui::stow_collapsed_height),static_cast<double>(ui::stow_arrow_region_height),
        static_cast<double>(ui::ControlIcon::up),static_cast<double>(ui::ControlIcon::down)};
    const std::array<ui_language::KeyBinding,7> shelf_keys{{{"shelf",control::shelf},{"stow",control::shelf_handle},
        {"contents",control::shelf_contents},{"tabs",control::shelf_tabs},{"create",control::shelf_create},
        {"view",control::shelf_view},{"body",control::shelf_body}}};
    static_cast<void>(ui_document_->append("ToolShelf",3,shelf_keys,shelf_inputs,result));
    // The three authored fixed-height bands define the native dock host extent.
    float chrome_height{};
    for (const auto key:{ui::ControlKey{2},control::shelf,ui::ControlKey{100}}) {
        const auto at=std::ranges::find(result,key,&ui::Control::key);
        if (at==result.end() || at->layout.height.rule!=ui_layout::SizeRule::fixed) {
            if (reject_ui_controls("Editor chrome bands require fixed heights for the dock host."))
                return describe(session,size,frame,project);
            throw std::runtime_error(std::string(ui_error()));
        }
        chrome_height+=at->layout.height.value;
    }
    const auto placement=dock.presentation(20,{size.width,std::max(0.f,size.height-chrome_height)});
    auto panes=std::make_shared<std::vector<ui_language::Item>>();
    for (const auto& pane:placement.panes) {
        auto tabs=std::make_shared<std::vector<ui_language::Item>>();
        for (const auto& tab:pane.tabs) {
            std::wstring style=L"dock-tab";
            for (const auto& name:tab.view->classes) style+=L" "+std::wstring(name.begin(),name.end());
            tabs->push_back({tab.wrapper,{tab.view->title,tab.view==pane.active,std::move(style),tab.view->key,double(ui::ControlIcon::close)},
                {{"title",tab.view->key},{"close",tab.close}}});
        }
        auto slots=std::make_shared<std::vector<ui_language::Item>>();
        if (pane.active) slots->push_back({pane.active->content,{pane.active->key!=control::scene_tab && pane.active->key!=control::parameters_tab}});
        const bool h=pane.horizontal,first=pane.collapsed==1,second=pane.collapsed==2;
        panes->push_back({pane.key,{double(pane.bounds.width),double(pane.bounds.height),double(pane.bounds.x),double(pane.bounds.y),pane.split,
            ui_language::ItemList{std::move(tabs)},ui_language::ItemList{std::move(slots)},pane.active!=nullptr,
            double(pane.maximized?ui::ControlIcon::restore:ui::ControlIcon::maximize),double(ui::ControlIcon::plus),double(ui::ControlIcon::down),
            double(pane.divider.x),double(pane.divider.y),double(pane.divider.width),double(pane.divider.height),!h,
            pane.value,pane.minimum,pane.maximum,pane.step,
            double(h?(first?ui::ControlIcon::right:ui::ControlIcon::left):(first?ui::ControlIcon::down:ui::ControlIcon::up)),
            double(h?(second?ui::ControlIcon::left:ui::ControlIcon::right):(second?ui::ControlIcon::up:ui::ControlIcon::down)),
            double(h?ui::ControlIcon::grip_vertical:ui::ControlIcon::grip_horizontal),first,second,
            std::wstring(h?L"dock-divider vertical":L"dock-divider horizontal"),std::wstring(h?L"dock-grip vertical":L"dock-grip horizontal")},
            {{"tabs.strip",pane.key+2},{"tabs.tabs",pane.key+9},{"tabs.add",pane.key+11},{"tabs.gap",pane.key+15},
             {"tabs.maximize",pane.key+3},{"tabs.menu",pane.key+4},{"tabs.empty",pane.key+10},
             {"divider.rail",pane.key+15},{"divider.track",pane.key+1},{"divider.first",pane.key+12},{"divider.second",pane.key+13},{"divider.grip",pane.key+14}},pane.parent});
    }
    const std::array<ui_language::InputValue,1> dock_inputs{ui_language::ItemList{std::move(panes)}};
    static_cast<void>(ui_document_->append("DockWorkspace",20,{},dock_inputs,result));
    for (const auto& pane:placement.panes) {
    if (!pane.active) continue;
    const auto& view=*pane.active;
    if (view.key==control::scene_tab) {
        auto modes=std::make_shared<std::vector<ui_language::Item>>();
        constexpr std::array icons{ui::ControlIcon::pointer,ui::ControlIcon::axes,ui::ControlIcon::translate,ui::ControlIcon::rotate,ui::ControlIcon::scale};
        for(std::size_t i=0;i<icons.size();++i) {
            const auto command=static_cast<ui::ControlKey>(Command::tool_select)+i;
            modes->push_back({command,{ui_language::ActionBinding{command,true},double(icons[i]),std::size_t(viewport_settings.tool)==i,control::viewport_modes,
                std::wstring(i==1?L"viewport-mode grouped":L"viewport-mode")}});
        }
        constexpr std::array mode_names{L"Select",L"Transform",L"Move",L"Rotate",L"Scale"};
        constexpr std::array view_names{L"Persp",L"Top",L"Front",L"Right"};
        // Bindings as actually routed by Application::viewport_input and ViewportTool.
        constexpr std::array tool_hints{
            L"Left click places the 3D cursor. Right click selects, drag box-selects. Middle tumbles, Shift+Middle pans.",
            L"Drag a handle to move, rotate or scale. G / R / S for modal transforms. Middle tumbles, Shift+Middle pans.",
            L"Drag an arrow or plane to move. G then X/Y/Z constrains, type a value, Enter confirms.",
            L"Drag a ring to rotate. R then X/Y/Z constrains, type degrees, Enter confirms.",
            L"Drag a box to scale an axis, the centre for uniform scale. S then X/Y/Z constrains."};
        const auto step=viewport_settings.grid_spacing;
        const std::wstring step_name=step==.1f?L"0.1 m":step==.2f?L"0.2 m":step==.5f?L"0.5 m":L"1 m";
        const std::array<ui_language::InputValue,33> inputs{ui_language::ActionBinding{control::scene_back,can_navigate(-1)},
            ui_language::ActionBinding{control::scene_forward,can_navigate(1)},double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world),
            double(ui::ControlIcon::mesh),double(ui::ControlIcon::axes),menu && menu->source==control::scene_shading,menu && menu->source==control::scene_guides,
            ui_language::ItemList{std::move(modes)},double(ui::ControlIcon::cursor3d),menu && menu->source==control::viewport_pivot,
            std::wstring(mode_names[std::size_t(viewport_mode)]),double(icons[std::size_t(viewport_mode)]),
            std::wstring(viewport_mode==TransformTool::scale?L"Local axes":L"World axes"),
            std::wstring(viewport_settings.pivot==PivotMode::median?L"Selection Center":L"3D Cursor"),
            ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::snap_toggle)},double(ui::ControlIcon::magnet),viewport_settings.grid_snap,
            step_name,menu && menu->source==control::viewport_snap,viewport_settings.cursor_surface,
            ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::cursor_surface)},std::wstring(view_names[std::size_t(camera_view)]),
            menu && menu->source==control::viewport_camera,ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::viewport_options)},
            viewport_options_collapsed,double(ui::ControlIcon::up),double(ui::ControlIcon::down),viewport_readout,!viewport_readout.empty(),
            ui_language::ActionBinding{control::viewport_rail_snap},std::wstring(tool_hints[std::size_t(viewport_mode)])};
        const std::array<ui_language::KeyBinding,14> keys{{{"navigation",641},{"path",control::scene_path},{"back",control::scene_back},{"readout",control::viewport_readout},
            {"forward",control::scene_forward},{"tools.display",control::scene_shading},{"tools.guides",control::scene_guides},
            {"viewport",control::viewport},{"pivot",control::viewport_pivot},{"edit_tools",control::viewport_modes},
            {"actions",control::viewport_tools_menu},{"step",control::viewport_snap},{"camera",control::viewport_camera},{"mode",control::viewport_hud}}};
        static_cast<void>(ui_document_->append("SceneViewport",control::scene_content,keys,inputs,result));
        continue;
    }
    if (view.key==source_control::tab) {source_editor.append(result,view.content,*ui_document_,project,project_build.get(),project_running);continue;}
    if (view.key==control::output_tab) {
        const std::array<ui_language::InputValue,1> inputs{ui_language::TableBinding{output,{}}};
        const std::array<ui_language::KeyBinding,1> keys{{{"messages",control::output_table}}};
        static_cast<void>(ui_document_->append("ProjectOutput",view.content,keys,inputs,result));continue;
    }
    if (view.key==control::workbench) { workbench.append(result,view.content,*ui_document_); continue; }
    if (view.key==control::assets_tab) {
        assets.append(result,view.content,project,command_enabled(Command::create_static_mesh,session),frame,*ui_document_);
        continue;
    }
    if (view.key==control::actors_tab) {
        auto rows=std::make_shared<std::vector<ui_language::Item>>();rows->reserve(members.size());
        for (const auto& member:members) {
            const auto state=take(world::inspect_actor(runtime,member.id()));
            const auto actor_key=bind(member.id());
            const auto type=take(runtime.describe(state.type));auto type_name=type->name;
            if (const auto dot=type_name.find_last_of('.');dot!=type_name.npos) type_name=type_name.substr(dot+1);
            auto title=std::wstring(session.name(member.id()))+L"   ("+std::wstring(type_name.begin(),type_name.end())+L")";
            const bool mesh_actor=take(runtime.is_a(state.type,session.types().static_mesh_actor));
            const auto expansion=tree_expansion_.find(actor_key);
            rows->push_back({actor_key,{std::move(title),
                state.state==world::ActorState::attached_inactive || state.state==world::ActorState::active,
                session.is_selected(member.id()),static_cast<double>(mesh_actor?ui::ControlIcon::mesh:ui::ControlIcon::object),
                control::tree_world,expansion!=tree_expansion_.end() && expansion->second,std::wstring(mesh_actor?L"mesh-actor":L"actor")}});
        }
        const auto expansion=tree_expansion_.find(control::tree_world);
        const std::array<ui_language::InputValue,10> inputs{ui_language::ItemList{std::move(rows)},
            ui_language::TextEditBinding{control::tree_filter,tree_query},selected==ObjectId{},
            expansion==tree_expansion_.end() || expansion->second,static_cast<double>(ui::ControlIcon::world),
            ui_language::ActionBinding{control::actor_back,can_navigate(-1)},ui_language::ActionBinding{control::actor_forward,can_navigate(1)},
            double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world)};
        const std::array<ui_language::KeyBinding,6> keys{{{"tree",control::actor_tree},{"world",control::tree_world},
            {"path.bar",24},{"path.path",control::actor_path},{"path.back",control::actor_back},{"path.forward",control::actor_forward}}};
        static_cast<void>(ui_document_->append("SceneTree",21,keys,inputs,result));
        continue;
    }
    const auto short_type=[&](ObjectId object) {
        auto name=take(runtime.describe(take(runtime.type_of(object))))->name;
        if(const auto dot=name.find_last_of('.');dot!=name.npos) name=name.substr(dot+1);
        return wide(name);
    };
    if(view.key==control::structure_tab) {
        auto rows=std::make_shared<std::vector<ui_language::Item>>();
        if(selected!=ObjectId{}) {
            const auto actor_key=bind(selected,{},Binding::Kind::structure);
            const auto expansion=tree_expansion_.find(actor_key);
            rows->push_back({actor_key,{std::wstring(session.name(selected)),true,inspected==selected,double(ui::ControlIcon::object),
                ui::ControlKey{},expansion==tree_expansion_.end() || expansion->second,std::wstring(L"actor")}});
            for(const auto& component:take(world::components(runtime,selected))) {
                const auto key=bind(component.id(),{},Binding::Kind::structure);
                const auto label=short_type(component.id())+(transform==component.id()?L"  (Root)":L"");
                rows->push_back({key,{label,actor_status.can_edit,inspected==component.id(),double(ui::ControlIcon::component),
                    actor_key,false,std::wstring(L"component")}});
            }
        }
        const std::array<ui_language::InputValue,8> inputs{selected==ObjectId{}?std::wstring(L"Actor Structure"):std::wstring(session.name(selected)),
            ui_language::ItemList{std::move(rows)},ui_language::TextEditBinding{control::structure_filter,structure_query},selected!=ObjectId{},
            selected!=ObjectId{} && actor_status.can_edit,control::add_component,
            ui_language::ActionBinding{control::remove_selected,command_enabled(Command::erase_selected,session)},double(ui::ControlIcon::close)};
        const std::array<ui_language::KeyBinding,2> keys{{{"tree",control::structure_tree},{"add",control::add_component}}};
        static_cast<void>(ui_document_->append("ActorStructure",view.content,keys,inputs,result));
        continue;
    }
    if(inspecting_asset) {
        const auto& asset=assets.selected();auto entries=std::make_shared<std::vector<ui_language::Item>>();
        const auto add=[&](std::size_t index,std::wstring name,std::wstring value) {
            auto& key=resource_properties_[index];if(!key) key=next_binding_++;
            entries->push_back({key,{std::move(name),std::move(value)}});
        };
        std::wstring kind=L"Resource";
        if(asset) {
            kind=asset->kind==AssetKind::mesh?L"Mesh":asset->kind==AssetKind::scene?L"Scene":asset->kind==AssetKind::source?L"Source":asset->kind==AssetKind::folder?L"Folder":L"File";
            add(0,L"Type",kind);add(1,L"Resource",wide(asset->key));
            add(2,L"Location",asset->path.empty()?L"Engine":asset->path.generic_wstring());
            if(!asset->path.empty() && asset->kind!=AssetKind::folder) add(3,L"Size",std::to_wstring(asset->bytes)+L" bytes");
            if(const auto* mesh=mesh_asset(asset->key)) {
                add(4,L"Triangles",std::to_wstring(mesh->mesh->vertices().size()/3));
            }
        }
        const std::array<ui_language::InputValue,4> inputs{kind,asset?asset->name:std::wstring(L"No resource selected"),
            double(asset && asset->kind==AssetKind::mesh?ui::ControlIcon::mesh:ui::ControlIcon::folder),ui_language::ItemList{std::move(entries)}};
        static_cast<void>(ui_document_->append("ResourceParameters",view.content,{},inputs,result));continue;
    }
    const auto lowercase=[](std::wstring value) {for(auto& c:value)c=static_cast<wchar_t>(std::towlower(c));return value;};
    const auto folded=parameter_search_visible?lowercase(query):std::wstring{};
    const bool show_position=transform && (inspected==selected || inspected==*transform);
    auto groups=std::make_shared<std::vector<ui_language::Item>>();
    const auto append_properties=[&](ObjectId object) {
        const auto type=take(runtime.type_of(object));
        const auto title=short_type(object);
        const bool group_matches=lowercase(title).find(folded)!=std::wstring::npos;
        auto properties=std::make_shared<std::vector<ui_language::Item>>();
        const auto schema=take(session.properties(object));
        const auto light=take(runtime.is_a(type,session.types().light_component))?
            std::optional(take(world::read_light(runtime,object))):std::nullopt;
        for(std::size_t index=0;index<schema.size();++index) {
            const auto& property=schema[index];
            if(show_position && object==*transform && detail::transform_path(session.types(),property.path)) continue;
            if(light) {
                const auto& name=property.name;
                if(light->kind!=world::LightKind::rectangle && (name=="settings.width" || name=="settings.height" || name=="settings.two_sided")) continue;
                if(light->kind==world::LightKind::rectangle && name=="settings.source_radius") continue;
                if(light->kind==world::LightKind::directional && name=="settings.range") continue;
            }
            auto parent_type=type;TypeId aggregate_type{};
            for(std::size_t depth=0;depth+1<property.path.size();++depth) {
                const auto parent=take(runtime.describe(parent_type));
                aggregate_type=parent_type;
                parent_type=std::ranges::find(parent->fields,property.path[depth],&FieldInfo::id)->value.type;
            }
            const auto parent=take(runtime.describe(parent_type));
            if(property.path.size()>1 && (parent->math.vector() || parent->math.shape==math::Shape::quaternion)) {
                const auto count=parent->math.components();
                const auto name=property.name.substr(0,property.name.rfind('.'));
                const auto prefix=std::span(property.path).first(property.path.size()-1);
                const bool spatial_position=std::ranges::equal(property.path,session.types().position_fields[0]);
                const bool spatial_rotation=std::ranges::equal(property.path,session.types().rotation_fields[0]);
                const bool spatial_scale=std::ranges::equal(property.path,session.types().scale_fields[0]);
                const bool base_color=aggregate_type==session.types().material &&
                    property.path[property.path.size()-2]==take(runtime.find_field(session.types().material,"base_color"));
                const bool emission=aggregate_type==session.types().material &&
                    property.path[property.path.size()-2]==take(runtime.find_field(session.types().material,"emissive"));
                const bool light_color=aggregate_type==session.types().light_settings &&
                    property.path[property.path.size()-2]==take(runtime.find_field(session.types().light_settings,"color"));
                const bool color=base_color || emission || light_color;
                const auto euler=spatial_rotation?euler_degrees(take(world::read_transform(runtime,object)).rotation):math::Vector3{};
                std::vector<ui::NumberField> tuple;bool matches=group_matches || lowercase(wide(name)).find(folded)!=std::wstring::npos;
                for(std::size_t lane=0;lane<count && index+lane<schema.size();++lane) {
                    const auto& field=schema[index+lane];
                    if(field.path.size()!=property.path.size() || !std::equal(prefix.begin(),prefix.end(),field.path.begin())) break;
                    matches|=lowercase(wide(field.name)).find(folded)!=std::wstring::npos;
                    if(spatial_rotation && lane==3) continue;
                    const auto value=spatial_rotation?double(euler[lane]):std::visit([](const auto& v)->double {
                        if constexpr(std::same_as<std::decay_t<decltype(v)>,EnumValue>) return v.bits;
                        else return double(v);
                    },field.value);
                    const double limit=parent->math.precision==math::Precision::f32?double(std::numeric_limits<float>::max()):std::numeric_limits<double>::max();
                    const double minimum=color?0:spatial_position?session.parameters()[lane].minimum:spatial_scale?Session::minimum_scale:spatial_rotation?-360000:-limit;
                    const double maximum=base_color?1:spatial_position?session.parameters()[lane].maximum:spatial_scale?Session::maximum_scale:spatial_rotation?360000:limit;
                    const auto key=bind(object,field.path,spatial_rotation?Binding::Kind::rotation:Binding::Kind::field);
                    tuple.push_back({key,std::wstring(1,(color?L"RGBW":L"XYZW")[lane]),value,spatial_rotation?0:value,
                        minimum,maximum,spatial_rotation?1:.01,spatial_rotation?.1:.001,static_cast<ui::VectorAxis>(lane+1)});
                    tuple.back().single_precision=parent->math.precision==math::Precision::f32;
                }
                index+=count-1;
                if(matches) {
                    const auto field_key=tuple.front().key;
                    // Swatch shows the display colour; HDR emission is clamped for the chip only.
                    const auto channel=[&](std::size_t lane){return tuple.size()>lane?std::clamp(tuple[lane].value,0.0,1.0):0.0;};
                    const double r=channel(0),g=channel(1),b=channel(2);
                    properties->push_back({bind(object,prefix,Binding::Kind::property_row),
                        {spatial_rotation?std::wstring(L"Rotation (°)"):parameter_label(name),ui_language::EditBinding{0,actor_status.can_edit,false,std::move(tuple)},
                        false,false,actor_status.can_edit,ui_language::ActionBinding{field_key,actor_status.can_edit},false,std::wstring{},field_key,
                        color,r,g,b}});
                }
                continue;
            }
            auto caption=parameter_label(property.name);
            if(light && property.name.starts_with("settings.")) {
                const auto field=std::string_view(property.name).substr(9);
                if(field=="kind") caption=L"Light Type";
                else if(field=="intensity") caption=light->kind==world::LightKind::point?L"Intensity (cd)":
                    light->kind==world::LightKind::rectangle?L"Intensity (cd/m²)":L"Illuminance (lux)";
                else if(field=="source_radius") caption=light->kind==world::LightKind::directional?L"Angular Radius (rad)":L"Source Radius (m)";
                else if(field=="range") caption=L"Range (m)";
                else if(field=="width") caption=L"Width (m)";
                else if(field=="height") caption=L"Height (m)";
                else if(field=="two_sided") caption=L"Two Sided";
                else if(field=="casts_shadow") caption=L"Cast Shadows";
            }
            if(!group_matches && lowercase(caption).find(folded)==std::wstring::npos) continue;
            std::optional<Scalar> default_value;auto owner=type;
            for(const auto id:property.path) {
                const auto info=take(runtime.describe(owner));const auto at=std::ranges::find(info->fields,id,&FieldInfo::id);
                if(at==info->fields.end()) break;
                default_value=at->default_value;owner=at->value.type;
            }
            const bool boolean=std::holds_alternative<bool>(property.value);
            const auto* enumeration=std::get_if<EnumValue>(&property.value);
            const bool integral=std::holds_alternative<std::int32_t>(property.value) || std::holds_alternative<std::uint32_t>(property.value);
            const auto number=[](const Scalar& value) {return std::visit([](const auto& scalar)->double {
                if constexpr(std::same_as<std::decay_t<decltype(scalar)>,EnumValue>) return scalar.bits;
                else return static_cast<double>(scalar);
            },value);};
            double minimum=-std::numeric_limits<double>::max(),maximum=std::numeric_limits<double>::max();
            if(std::holds_alternative<float>(property.value)) {minimum=-std::numeric_limits<float>::max();maximum=std::numeric_limits<float>::max();}
            if(integral) {minimum=std::holds_alternative<std::int32_t>(property.value)?std::numeric_limits<std::int32_t>::lowest():0;
                maximum=std::holds_alternative<std::int32_t>(property.value)?std::numeric_limits<std::int32_t>::max():std::numeric_limits<std::uint32_t>::max();}
            if(parent_type==session.types().material && (property.name=="material.roughness" || property.name=="material.metallic")) {minimum=0;maximum=1;}
            if(light && parent_type==session.types().light_settings && !boolean && !enumeration) {
                minimum=property.name=="settings.range" || property.name=="settings.width" || property.name=="settings.height"?
                    double(std::numeric_limits<float>::denorm_min()):0;
                if(property.name=="settings.source_radius" && light->kind==world::LightKind::directional) maximum=double(1.5707963267948966f);
            }
            const auto key=bind(object,property.path);const double value=number(property.value),initial=default_value?number(*default_value):value;
            ui_language::EditBinding edit{0,actor_status.can_edit,value!=initial,{{key,L"",value,initial,minimum,maximum,integral?1:.01,integral?1:.001}}};
            edit.fields.front().single_precision=std::holds_alternative<float>(property.value);
            std::wstring choice;
            if(enumeration) for(const auto& member:take(runtime.describe(enumeration->type))->enumeration->values)
                if(member.second==enumeration->bits) {choice=wide(member.first);break;}
            properties->push_back({bind(object,property.path,Binding::Kind::property_row),
                {caption,std::move(edit),boolean,boolean && std::get<bool>(property.value),actor_status.can_edit,
                 ui_language::ActionBinding{key,actor_status.can_edit},enumeration!=nullptr,std::move(choice),key,false,0.0,0.0,0.0}});
        }
        if(!properties->empty()) groups->push_back({bind(object,{},Binding::Kind::property_group),
            {title,double(object==selected?ui::ControlIcon::object:ui::ControlIcon::component),ui_language::ItemList{std::move(properties)}}});
    };
    if(inspected!=ObjectId{}) {
        append_properties(inspected);
        if(inspected==selected) for(const auto& component:take(world::components(runtime,selected))) append_properties(component.id());
    }
    constexpr std::array aliases{L"transform scene root position 变换 位置 x", L"transform scene root position 变换 位置 y", L"transform scene root position 变换 位置 z"};
    constexpr std::array names{L"X", L"Y", L"Z"};
    const auto definitions=session.parameters();
    std::vector<ui::NumberField> fields;bool group_modified{};
    for(std::size_t axis=0;axis<definitions.size();++axis) {
        const auto& definition=definitions[axis];
        const double value=show_position?std::get<float>(take(session.world().read(*transform,definition.path))):definition.default_value;
        group_modified|=value!=definition.default_value;
        if(!show_position || std::wstring_view(aliases[axis]).find(folded)==std::wstring_view::npos) continue;
        fields.push_back({bind(*transform,definition.path),names[axis],value,definition.default_value,definition.minimum,definition.maximum,definition.drag_step,.001,static_cast<ui::VectorAxis>(axis+1)});
        fields.back().single_precision=true;
    }
    const bool position_matches=!fields.empty();
    const bool rotation_matches=show_position && std::wstring_view(L"rotation transform angle euler x y z 旋转 角度").find(folded)!=std::wstring_view::npos;
    const bool scale_matches=show_position && std::wstring_view(L"scale transform x y z 缩放").find(folded)!=std::wstring_view::npos;
    ui_language::EditBinding rotation_edit,scale_edit;
    if(show_position) {
        const auto trs=take(world::read_transform(runtime,*transform));const auto angles=euler_degrees(trs.rotation);
        rotation_edit={bind(*transform,{},Binding::Kind::rotation_default),actor_status.can_edit,angles!=math::Vector3{}, {}};
        scale_edit={bind(*transform,{},Binding::Kind::scale_default),actor_status.can_edit,trs.scale!=math::Vector3{1,1,1}, {}};
        for(std::size_t axis=0;axis<3;++axis) {
            rotation_edit.fields.push_back({bind(*transform,session.types().rotation_fields[axis],Binding::Kind::rotation),names[axis],angles[axis],0,-360000,360000,1,.1,static_cast<ui::VectorAxis>(axis+1)});
            scale_edit.fields.push_back({bind(*transform,session.types().scale_fields[axis]),names[axis],trs.scale[axis],1,Session::minimum_scale,Session::maximum_scale,.01,.001,static_cast<ui::VectorAxis>(axis+1)});
            rotation_edit.fields.back().single_precision=scale_edit.fields.back().single_precision=true;
        }
    }
    const bool matches=position_matches || rotation_matches || scale_matches,empty=groups->empty() && !matches;
    ui_language::EditBinding position{show_position?bind(*transform,{},Binding::Kind::position_default):ui::ControlKey{74},
        actor_status.can_edit && show_position,group_modified,std::move(fields)};
    const bool actor_selected=inspected!=ObjectId{} && inspected==selected;
    const std::array<ui_language::InputValue,22> inputs{inspected==ObjectId{}?std::wstring(L"Object"):short_type(inspected),
        ui_language::TextEditBinding{actor_selected?bind(selected,{},Binding::Kind::name):ui::ControlKey{77},
            inspected==ObjectId{}?L"No selection":actor_selected?std::wstring(session.name(selected)):short_type(inspected),actor_selected && actor_status.can_edit},
        ui_language::ActionBinding{control::parameter_back,can_navigate(-1)},ui_language::ActionBinding{control::parameter_forward,can_navigate(1)},
        double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world),
        inspected!=ObjectId{},ui_language::ItemList{std::move(groups)},matches,std::move(position),ui_language::TextEditBinding{control::parameter_search,query},empty,
        double(actor_selected?(take(runtime.is_a(actor_status.type,session.types().static_mesh_actor))?ui::ControlIcon::mesh:ui::ControlIcon::object):ui::ControlIcon::component),double(ui::ControlIcon::search),
        parameter_search_visible && inspected!=ObjectId{},ui_language::ActionBinding{control::parameter_search_toggle,inspected!=ObjectId{}},
        std::move(rotation_edit),std::move(scale_edit),position_matches,rotation_matches,scale_matches};
    const std::array<ui_language::KeyBinding,12> keys{{{"path.bar",75},
        {"path.path",control::parameter_path},{"path.back",control::parameter_back},{"path.forward",control::parameter_forward},
        {"header.row",51},{"header.type",52},{"header.search",control::parameter_search_toggle},
        {"scroll",control::parameter_list},{"transform",85},
        {"position.section",control::position_fold},{"position.contents",58},{"position.position",70,72,73}}};
    static_cast<void>(ui_document_->append("Parameters",view.content,keys,inputs,result));
    }
    const auto preview=placement.preview.value_or(ui::Rect{});
    const std::array<ui_language::InputValue,5> preview_inputs{placement.preview.has_value(),double(preview.x),double(preview.y),double(preview.width),double(preview.height)};
    const std::array<ui_language::KeyBinding,1> preview_keys{{{"preview",placement.preview_key}}};
    static_cast<void>(ui_document_->append("DockPreview",0,preview_keys,preview_inputs,result));
    menu_items.clear(); component_choices_.clear(); property_choices_.clear(); menu_selection_=session.selection();
    if (menu) {
        const auto find=[&](ui::ControlKey key) -> const ui::Control* {
            const auto at=std::find_if(result.begin(),result.end(),[&](const auto& item){return item.key==key;});
            return at==result.end()?nullptr:&*at;
        };
        const auto* source=find(menu->source);
        bool available=source!=nullptr;
        for (auto* item=source;item;item=find(item->parent))
            available&=item->enabled && !item->layout.collapsed;
        if (!available) menu.reset();
        else {
            const auto command_item=[&](Command command) {
                const auto key=static_cast<ui::ControlKey>(command);const auto* definition=command_definition(key);
                ui::MenuItem item{key,std::wstring(definition->title),command_enabled(command,session),false,{},std::wstring(definition->shortcut),definition->separator};
                const auto radio=[&](Command first,bool selected) {item.behavior=ui::MenuBehavior::radio;item.radio_group=static_cast<ui::ControlKey>(first);item.selected=selected;};
                if(command>=Command::tool_select && command<=Command::tool_scale)
                    radio(Command::tool_select,key-static_cast<ui::ControlKey>(Command::tool_select)==std::size_t(viewport_settings.tool));
                else if(command==Command::pivot_median || command==Command::pivot_cursor)
                    radio(Command::pivot_median,(command==Command::pivot_cursor)==(viewport_settings.pivot==PivotMode::cursor));
                else if(command>=Command::snap_grid_01 && command<=Command::snap_grid_1) {
                    constexpr std::array steps{.1f,.2f,.5f,1.f};radio(Command::snap_grid_01,viewport_settings.grid_spacing==steps[key-static_cast<ui::ControlKey>(Command::snap_grid_01)]);
                } else if(command>=Command::view_perspective && command<=Command::view_right)
                    radio(Command::view_perspective,key-static_cast<ui::ControlKey>(Command::view_perspective)==std::size_t(camera_view));
                else if(command==Command::snap_toggle || command==Command::cursor_surface || command==Command::viewport_options) {
                    item.behavior=ui::MenuBehavior::checkbox;
                    item.selected=command==Command::snap_toggle?viewport_settings.grid_snap:command==Command::cursor_surface?viewport_settings.cursor_surface:!viewport_options_collapsed;
                }
                return item;
            };
            const auto commands=[&](std::initializer_list<Command> commands) {
                std::vector<ui::MenuItem> result;for(const auto command:commands) result.push_back(command_item(command));return result;
            };
            const auto viewport_commands=[&](ui::ControlKey key) {
                using enum Command;
                switch(key) {
                case control::viewport_tools_menu: return commands({tool_select,tool_transform,tool_move,tool_rotate,tool_scale});
                case control::viewport_snap: return commands({snap_toggle,snap_grid_01,snap_grid_02,snap_grid_05,snap_grid_1,
                    selection_grid,selection_cursor,cursor_selected,cursor_origin,cursor_grid,cursor_surface});
                case control::viewport_camera: return commands({view_perspective,view_top,view_front,view_right,focus,reset_camera});
                case control::viewport_pivot: return commands({pivot_median,pivot_cursor,cursor_selected,cursor_origin,cursor_grid});
                default: return std::vector<ui::MenuItem>{};
                }
            };
            std::vector<ui::MenuItem> items=dock.menu_items(source->key,frame);
            if (const auto* group=command_group(source->key)) {
                if(group->id==CommandGroup::view) {
                    items.push_back(command_item(Command::focus));
                    for(const auto& [key,title]:std::array<std::pair<ui::ControlKey,std::wstring_view>,4>{{
                        {control::viewport_tools_menu,L"Interaction Mode"},{control::viewport_pivot,L"Pivot"},
                        {control::viewport_snap,L"Snapping"},{control::viewport_camera,L"Camera View"}}})
                        items.push_back({.key=key,.text=std::wstring(title),.edit_command={},.children=viewport_commands(key)});
                    items.push_back(command_item(Command::viewport_options));
                } else for (const auto& definition:command_definitions) if (definition.group==group->id)
                    items.push_back(command_item(definition.id));
            } else if(source->key==control::viewport || source->key==control::viewport_tools_menu) {
                using enum Command;
                items=viewport_commands(control::viewport_tools_menu);
                if(source->key==control::viewport) {
                    items.push_back({.key=control::viewport_snap,.text=L"Snap",.edit_command={},.children=viewport_commands(control::viewport_snap)});
                    for(auto item:commands({focus,create_static_mesh,erase_selected})) items.push_back(std::move(item));
                }
            } else if(source->key==control::viewport_snap || source->key==control::viewport_camera || source->key==control::viewport_pivot) {
                items=viewport_commands(source->key);
            } else if (control::is_path(source->key)) {
                items.push_back({control::world_context,L"World",true,selected==ObjectId{},{}});
                for (const auto& member:members) items.push_back({bind(member.id(),{},Binding::Kind::navigation),
                    std::wstring(session.name(member.id())),true,selected==member.id(),{},L"",items.size()==1});
                for (auto& item:items) {item.behavior=ui::MenuBehavior::radio;item.radio_group=source->key;}
            } else if(const auto* target=binding(source->context_target);source->kind==ControlKind::combo && target && target->kind==Binding::Kind::field) {
                auto value=session.read_property(target->object,target->path);
                if(value) if(const auto* enumeration=std::get_if<EnumValue>(&*value)) {
                    for(const auto& member:take(runtime.describe(enumeration->type))->enumeration->values) {
                        const auto key=control::property_choice_first+property_choices_.size();
                        property_choices_.push_back({target->object,target->path,{enumeration->type,member.second},target->key});
                        ui::MenuItem item{key,wide(member.first),actor_status.can_edit,member.second==enumeration->bits,{}};
                        item.behavior=ui::MenuBehavior::radio;item.radio_group=source->key;items.push_back(std::move(item));
                    }
                }
            } else if (auto asset_items=assets.menu_items(source->key);!asset_items.empty()) items=std::move(asset_items);
            else if (source->key==control::scene_shading) {
                const auto group=[&](ui::ControlKey first,const auto& titles,std::size_t selected) {
                    for (std::size_t i=0;i<titles.size();++i) {
                        ui::MenuItem item{first+i,titles[i],true,selected==i,{}};
                        item.behavior=ui::MenuBehavior::radio;item.radio_group=first;
                        item.separator_before=i==0 && !items.empty();items.push_back(std::move(item));
                    }
                };
                group(control::shading_first,shading_titles,static_cast<unsigned>(this->view.shading));
                ui::MenuItem lit{control::shading_scene_lit,L"Scene Lit",true,this->view.shading==renderer::Shading::scene_lit,{}};
                lit.behavior=ui::MenuBehavior::radio;lit.radio_group=control::shading_first;items.push_back(std::move(lit));
                group(control::edges_first,edge_titles,edge_option);
                group(control::topology_first,topology_titles,this->view.triangle_edges?1u:0u);
                ui::MenuItem lighting{control::lit_menu,L"Scene Lighting",true,false,{},L"",true};
                for(std::size_t i=0;i<lighting_options.size();++i) {
                    ui::MenuItem option{control::lit_effects_first+i,std::wstring(lighting_options[i].title),true,this->view.lit.*lighting_options[i].field,{}};
                    option.behavior=ui::MenuBehavior::checkbox;lighting.children.push_back(std::move(option));
                }
                ui::MenuItem exposure{control::lit_exposure_menu,L"Exposure",true,false,{},L"",true};
                for(int i=0;i<5;++i) {
                    const auto ev=i-2;
                    ui::MenuItem option{control::lit_exposure_first+std::uint64_t(i),(ev>0?L"+":L"")+std::to_wstring(ev)+L" EV",true,this->view.lit.exposure_ev==float(ev),{}};
                    option.behavior=ui::MenuBehavior::radio;option.radio_group=control::lit_exposure_first;
                    exposure.children.push_back(std::move(option));
                }
                lighting.children.push_back(std::move(exposure));
                ui::MenuItem budget{control::lit_shadow_budget_menu,L"Shadow Budget",true,false,{}};
                for(unsigned i=0;i<3;++i) {
                    const auto limit=4u<<i;
                    ui::MenuItem option{control::lit_shadow_budget_first+i,std::to_wstring(limit)+L" lights",true,this->view.lit.shadow_light_budget==limit,{}};
                    option.behavior=ui::MenuBehavior::radio;option.radio_group=control::lit_shadow_budget_first;budget.children.push_back(std::move(option));
                }
                lighting.children.push_back(std::move(budget));items.push_back(std::move(lighting));
            } else if (source->key==source_control::files) {
                items=source_editor.menu_items();
            } else if(source->key==control::add_component) {
                component_choices_=take(session.component_types());
                for(std::size_t i=0;i<component_choices_.size();++i)
                    items.push_back({control::component_choice_first+i,wide(component_choices_[i].name),selected!=ObjectId{},false,{}});
            } else if (source->key==control::scene_guides) {
                items.push_back({control::guide_background,L"Gradient Background",true,view.background==renderer::Background::gradient,{}});
                items.push_back({control::guide_grid,L"Reference Grid",true,view.reference_grid,{}});
                items.push_back({control::guide_axes,L"Origin Axes",true,view.origin_axes,{}});
                for (auto& item:items) item.behavior=ui::MenuBehavior::checkbox;
            } else if (!items.empty()) {}
            else if (source->kind==ControlKind::combo && source->key==workbench_control::mode) {
                for (std::size_t i=0;i<ControlWorkbench::mode_names.size();++i)
                    items.push_back({workbench_control::mode_first+i,ControlWorkbench::mode_names[i],true,workbench.values().mode==i,{}});
                for (auto& item:items) {item.behavior=ui::MenuBehavior::radio;item.radio_group=source->key;}
            } else {
                if (source->kind==ControlKind::number)
                    items.push_back({control::menu_reset,L"Revert to Default",source->modified,false,ui::EditCommand{ui::EditCommandKind::reset,source->key}});
                const auto* group=menu->group?find(menu->group):(source->kind==ControlKind::label?source:nullptr);
                if (group) items.push_back({control::menu_group_reset,L"Revert Tuple to Defaults",group->modified,false,{}});
                if (source->kind==ControlKind::number || source->kind==ControlKind::text_edit) {
                    items.push_back({control::menu_copy,L"Copy Value",true,false,ui::EditCommand{ui::EditCommandKind::copy,source->key}});
                    items.push_back({control::menu_paste,L"Paste Value",true,false,ui::EditCommand{ui::EditCommandKind::paste,source->key}});
                }
            }
            if (items.empty()) menu.reset();
            else menu_items=std::move(items);
        }
    }
    if (ui_document_->candidate_failed()) {
        reject_ui_controls(std::string(ui_error()));
        return describe(session,size,frame,project);
    }
    return result;
}
}
