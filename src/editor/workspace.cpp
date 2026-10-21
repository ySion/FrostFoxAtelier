#include "editor/workspace.hpp"
#include "editor/examples.hpp"
#include "editor/assets.hpp"
#include "editor/fields.hpp"
#include "editor/view_options.hpp"
#include "foundation/set.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cwctype>
#include <format>

namespace atelier::editor {
namespace {
struct CavityOption {std::wstring_view title;ViewOption option;};
constexpr std::array cavity_options{
    CavityOption{L"Ridge",ViewOption::cavity_ridge},CavityOption{L"Valley",ViewOption::cavity_valley},
    CavityOption{L"Radius",ViewOption::cavity_radius}};
constexpr std::array<std::array<double,5>,3> cavity_values{{{0,.5,1,1.5,2},{0,.5,1,1.5,2},{.5,1,2,3,4}}};
}
Workspace::Workspace() : dock(1ULL<<48,{
    {control::scene_tab,control::scene_content,L"Scene View",{240,160}},
    {control::parameters_tab,50,L"Parameters",{220,260}},
    {control::workbench,300,L"Controls",{220,280}},
    {control::entities_tab,21,L"Scene Tree",{180,140}},
    {control::structure_tab,control::structure_content,L"Entity Structure",{200,140}},
    {control::assets_tab,control::assets_content,L"Asset Browser",{240,140}},
    {source_control::tab,source_control::content,L"Code",{300,180}},
    {control::output_tab,control::output_content,L"Project Output",{240,120}},
    {world_control::tab,world_control::content,L"World",{280,240}}}) {
    const auto registered=ui_language::register_types(ui_runtime_);
    if (!registered) throw std::runtime_error(registered.error().message);
    auto messages=std::make_shared<ui::TableData>();messages->columns.push_back({1,L"Message",1000,160,ui::TextAlign::left,false});output=std::move(messages);
    register_builtin_operators(operators);
    reset_layout();
}
namespace {
constexpr std::array<std::pair<RegionKind,ui::ControlKey>,9> region_tabs{{
    {RegionKind::viewport,control::scene_tab},{RegionKind::scene_tree,control::entities_tab},{RegionKind::structure,control::structure_tab},
    {RegionKind::parameters,control::parameters_tab},{RegionKind::assets,control::assets_tab},{RegionKind::code,source_control::tab},
    {RegionKind::output,control::output_tab},{RegionKind::world,world_control::tab},{RegionKind::controls,control::workbench}}};
}
RegionKind region_of(ui::ControlKey tab) noexcept {
    const auto at=std::ranges::find(region_tabs,tab,&std::pair<RegionKind,ui::ControlKey>::second);
    return at==region_tabs.end()?RegionKind::none:at->first;
}
ui::ControlKey region_tab(RegionKind region) noexcept {
    const auto at=std::ranges::find(region_tabs,region,&std::pair<RegionKind,ui::ControlKey>::first);
    return at==region_tabs.end()?ui::ControlKey{}:at->second;
}
std::string_view operator_for(Command command) noexcept {
    switch(command) {
    case Command::frame_all:return "view.frame_all";
    case Command::focus:return "view.frame_selected";
    case Command::reset_camera:return "view.reset";
    case Command::reset_layout:return "layout.reset";
    case Command::close_tab:return "layout.close_tab";
    case Command::maximize_pane:return "layout.maximize";
    case Command::undo:return "edit.undo";
    case Command::redo:return "edit.redo";
    case Command::add_transform:return "scene.transform_add";
    case Command::remove_transform:return "scene.transform_remove";
    case Command::discard_saved_data:return "scene.discard_saved_data";
    default:return {};
    }
}
Context Workspace::operator_context(Session& session,Project* project,bool interactive) const {
    const auto* source=source_editor.active();
    const auto source_history=source?HistoryState{source->document->edit().can_undo(),source->document->edit().can_redo()}:HistoryState{};
    return {session,project,
        {session.selection().entity,session.selection().object,{session.selected_entities().begin(),session.selected_entities().end()}},
        region_of(dock.focused_tab()),inspecting_asset?material_inspection:std::string{},
        source_history,{workbench.undo_count()!=0,workbench.redo_count()!=0},scene_available,interactive};
}
void Workspace::log(std::wstring text) {
    auto messages=std::make_shared<ui::TableData>(*output);
    std::size_t start{};
    while(start<text.size()) {
        const auto end=text.find(L'\n',start);auto line=text.substr(start,end==text.npos?text.size()-start:end-start);
        if(!line.empty() && line.back()==L'\r') line.pop_back();
        if(line.size()>2048) {line.resize(2048);if(line.back()>=0xd800 && line.back()<=0xdbff) line.pop_back();line+=L"…";}
        if(!line.empty()) {messages->rows.push_back({next_output_,{{next_output_,std::move(line)}}});++next_output_;}
        if(end==text.npos) break;start=end+1;
    }
    if(messages->rows.size()>256) messages->rows.erase(messages->rows.begin(),messages->rows.end()-256);
    // The Message column spans the longest retained line, so details view scrolls instead of clipping it.
    float width=1000;
    for(const auto& row:messages->rows) {
        float estimate=24;
        for(const auto c:row.cells.front().text) estimate+=c<0x2e80?7.5f:13.f;
        width=std::max(width,std::min(estimate,32768.f));
    }
    messages->columns.front().width=width;
    output=std::move(messages);
}
void Workspace::log_repeated(std::wstring line) {
    if(line==repeated_line_ && !output->rows.empty() && output->rows.back().key==repeated_row_) {
        auto messages=std::make_shared<ui::TableData>(*output);
        messages->rows.back().cells.front().text=line+L" (×"+std::to_wstring(++repeated_count_)+L")";
        output=std::move(messages);return;
    }
    log(line);
    repeated_line_=std::move(line);repeated_row_=output->rows.back().key;repeated_count_=1;
}
void Workspace::reset_layout() {
    lighting.cancel();
    dock.reset();
    dock.move(world_control::tab,control::parameters_tab,ui::DockSide::tabs);
    dock.move(control::scene_tab,control::parameters_tab,ui::DockSide::left,.62f);
    dock.move(control::entities_tab,control::parameters_tab,ui::DockSide::bottom,.7f);
    dock.move(control::structure_tab,control::entities_tab,ui::DockSide::tabs);
    dock.select(control::entities_tab);
    dock.move(control::assets_tab,control::scene_tab,ui::DockSide::bottom,.73f);
    dock.move(source_control::tab,control::scene_tab,ui::DockSide::tabs);
    dock.move(control::output_tab,control::assets_tab,ui::DockSide::tabs);
    dock.select(control::assets_tab);
    dock.select(control::scene_tab);
    dock.select(control::parameters_tab); menu.reset(); menu_bar_collapsed=shelf_collapsed=false;
}
void Workspace::reset_document() {
    camera_flying=false;
    color_edit.reset();
    category_keys_.clear();
    property_help_.clear();
    lighting.cancel();
    lighting.observe(renderer::FrameStats{});
    lighting.performance().reset();
    if (ui_document_ && ui_document_->committed_source()) ui_checkpoint_=ui_document_->committed_source();
    ui_document_.reset();
    bindings_.clear(); binding_cursor_=0; scene_tree_={}; structure_tree_={};
    navigation_={ObjectId{}}; navigation_cursor_=0; query.clear(); tree_query.clear(); structure_query.clear(); parameter_search_visible=false;
    inspecting_asset=false;material_inspection.clear();
    menu.reset(); assets.reset(project_open);
    component_choices_.clear(); property_choices_.clear();material_choices_.clear(); menu_selection_={};
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
Result<void> Workspace::select_tree(Session& session,ui::ControlKey key,ui::SelectionModifiers modifiers) {
    inspecting_asset=false;
    if (key==control::tree_world) return session.select({});
    const auto* target=binding(key);
    if(target && target->kind==Binding::Kind::object && (modifiers.shift || modifiers.control)) {
        std::vector<ObjectId> selected;
        if(modifiers.control) selected.assign(session.selected_entities().begin(),session.selected_entities().end());
        if(modifiers.shift) {
            // The range runs over the rows the Scene Tree shows, described or scrolled away.
            const auto row_of=[&](ObjectId entity) {
                return std::ranges::find_if(scene_tree_.rows,[&](const auto& row){return scene_tree_.objects[row.node]==entity;});
            };
            const auto first=row_of(session.selection().entity),last=row_of(target->object);
            if(first!=scene_tree_.rows.end() && last!=scene_tree_.rows.end()) {
                for(auto at=std::min(first,last);at<=std::max(first,last);++at)
                    if(const auto entity=scene_tree_.objects[at->node];entity!=ObjectId{} && std::ranges::find(selected,entity)==selected.end())
                        selected.push_back(entity);
            } else selected.push_back(target->object);
        } else if(std::ranges::find(selected,target->object)!=selected.end()) std::erase(selected,target->object);
        else selected.push_back(target->object);
        return session.select_entities(selected,target->object);
    }
    return target && (target->kind==Binding::Kind::object || target->kind==Binding::Kind::structure)?session.select(target->object):Result<void>{};
}
void Workspace::build_structure_tree(const Session& session,ObjectId entity) {
    // The Entity, then each Entity's Components before its children, in preorder.
    auto& tree=structure_tree_;tree.nodes.clear();tree.objects.clear();
    if(entity==ObjectId{}) return;
    auto& runtime=session.world().runtime();
    struct Pending {ObjectId entity;std::uint32_t parent;};
    std::vector<Pending> pending{{entity,ui::no_tree_node}};
    while(!pending.empty()) {
        const auto entry=pending.back();pending.pop_back();
        const auto node=static_cast<std::uint32_t>(tree.nodes.size());
        tree.nodes.push_back({entry.parent,tree.expanded(entry.entity,true),{}});tree.objects.push_back(entry.entity);
        auto components=world::components(runtime,entry.entity);if(!components) throw std::runtime_error(components.error().message);
        for(const auto& component:*components) {tree.nodes.push_back({node,false,{}});tree.objects.push_back(component.id());}
        auto children=world::children(runtime,entry.entity);if(!children) throw std::runtime_error(children.error().message);
        for(auto child=children->rbegin();child!=children->rend();++child) pending.push_back({child->id(),node});
    }
}
Result<void> Workspace::set_tree_expanded(const Session& session,EntityTree tree,std::span<const ObjectId> objects,bool expanded) {
    auto& view=tree==EntityTree::scene_tree?scene_tree_:structure_tree_;
    if(tree==EntityTree::structure) {
        const auto entity=session.selection().entity;
        build_structure_tree(session,entity!=ObjectId{} && session.world().contains(entity)?entity:ObjectId{});
    }
    for(const auto object:objects)
        if(tree==EntityTree::scene_tree?object!=ObjectId{} && !session.world().contains(object):view.node(object)==ui::no_tree_node)
            return std::unexpected(Error{ErrorCode::invalid_member,"The identity is not a row of this tree."});
    for(const auto object:objects) view.expansion[object]=expanded;
    return {};
}
Result<bool> Workspace::reveal_in_tree(const Session& session,ObjectId entity) {
    if(!session.world().contains(entity)) return std::unexpected(Error{ErrorCode::invalid_member,"The Entity is not a member of this World."});
    std::vector<ObjectId> ancestors{ObjectId{}};
    for(auto parent=world::parent_entity(session.world().runtime(),entity);;parent=world::parent_entity(session.world().runtime(),*parent)) {
        if(!parent) return std::unexpected(parent.error());
        if(*parent==ObjectId{}) break;
        ancestors.push_back(*parent);
    }
    if(auto expanded=set_tree_expanded(session,EntityTree::scene_tree,ancestors,true);!expanded) return std::unexpected(expanded.error());
    return tree_query.empty() || std::ranges::any_of(scene_tree_.rows,[&](const auto& row){return scene_tree_.objects[row.node]==entity;});
}
std::vector<std::uint32_t> Workspace::window_tree(EntityTreeView& tree,ObjectId primary,std::wstring_view filter,
    ui::TreeViewport viewport,std::span<const ObjectId> pinned) {
    // A new primary row, or the primary row under a new parent, expands its ancestors once.
    if(const auto node=tree.node(primary);node!=ui::no_tree_node) {
        const auto parent=tree.nodes[node].parent;
        const std::pair current{primary,parent==ui::no_tree_node?ObjectId{}:tree.objects[parent]};
        if(tree.primary!=current) {
            for(auto at=parent;at!=ui::no_tree_node;at=tree.nodes[at].parent) {tree.expansion[tree.objects[at]]=true;tree.nodes[at].expanded=true;}
            tree.primary=current;
        }
    } else tree.primary.reset();
    ui::flatten_tree(tree.nodes,filter,tree.rows);
    std::vector<std::uint32_t> rows;
    for(const auto object:pinned)
        if(const auto node=tree.node(object);node!=ui::no_tree_node)
            if(const auto row=std::ranges::find(tree.rows,node,&ui::TreeRow::node);row!=tree.rows.end())
                rows.push_back(static_cast<std::uint32_t>(row-tree.rows.begin()));
    return ui::tree_window(tree.rows,viewport,rows);
}
bool Workspace::command_enabled(Command command,Session& session,Project* project) const {
    if(const auto id=operator_for(command);!id.empty()) {
        const auto* info=operators.find(id);
        return info && info->poll(operator_context(session,project,true)).has_value();
    }
    switch (command) {
    case Command::erase_entities: return scene_available && !session.selected_entities().empty();
    case Command::select_all: case Command::select_invert: case Command::select_box: return scene_available;
    case Command::select_none: return scene_available && !session.selected_entities().empty();
    case Command::rename_selected: return scene_available && session.selection().entity!=ObjectId{};
    case Command::unparent_selected: {
        if(!scene_available) return false;
        for(const auto entity:session.selected_entities()) {
            const auto parent=world::parent_entity(session.world().runtime(),entity);
            if(parent && *parent!=ObjectId{}) return true;
        }
        return false;
    }
    case Command::clear_location: case Command::clear_rotation: case Command::clear_scale:
        return scene_available && !session.selected_entities().empty();
    case Command::duplicate_selected: {
        const auto members=session.world().members();
        return scene_available && !session.selected_entities().empty() && members &&
            members->size()+session.selected_entities().size()<=Session::maximum_entities;
    }
    case Command::new_source: case Command::new_project_ui: case Command::build_project: return project_open;
    case Command::rebuild_project:return project_open && !project_running;
    case Command::run_project:return project_open && scene_available && !project_running && project_build && project_build->state==BuildState::ready;
    case Command::stop_project:return project_running && !project_stopping;
    case Command::restart_project:return project_running && !project_stopping && play_pending;
    case Command::edit_project_ui:return project_open && project_ui_configured;
    case Command::show_project_ui:return project_open && project_ui_configured && !project_running;
    case Command::save_scene: case Command::save_scene_as:return project_open && scene_available;
    case Command::open_scene:
    case Command::import_mesh:case Command::import_texture:case Command::create_material: case Command::refresh_assets: return project_open;
    case Command::save_document: return source_history()?source_editor.active()!=nullptr:project_open && scene_available;
    case Command::close_source: case Command::reload_source: return source_editor.active()!=nullptr;
    case Command::create_entity: case Command::create_static_mesh:
    case Command::create_point_light: case Command::create_rectangle_light: case Command::create_directional_light: {
        const auto members=session.world().members();
        return scene_available && members && members->size()<Session::maximum_entities;
    }
    case Command::selection_grid:case Command::selection_cursor:case Command::cursor_selected:
    case Command::erase_selected: {
        if (session.selection().entity==ObjectId{}) return false;
        const auto state=world::inspect_entity(session.world().runtime(),session.selection().entity);
        if (!state || !state->can_edit) return false;
        if (command==Command::erase_selected) return true;
        const auto spatial=world::has_transform(session.world().runtime(),session.selection().entity);
        return spatial && *spatial;
    }
    case Command::previous_tab: case Command::next_tab:
        return dock.focused_tab()!=0;
    default: return true;
    }
}
ui::ControlKey Workspace::bind(ObjectId object, std::span<const FieldId> path, Binding::Kind kind) {
    if (!path.empty() && kind==Binding::Kind::object) kind=Binding::Kind::field;
    const auto first=bindings_.begin()+static_cast<std::ptrdiff_t>(binding_cursor_);
    const auto found=std::find_if(first,bindings_.end(),[&](const auto& item) {
        return item.kind==kind && item.object==object && item.component==ObjectId{} && item.slot.empty() &&
            std::equal(item.path.begin(),item.path.end(),path.begin(),path.end());
    });
    if (found!=bindings_.end()) { std::iter_swap(first,found); return bindings_[binding_cursor_++].key; }
    if (next_binding_==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("Editor control identities exhausted.");
    bindings_.push_back({next_binding_++,object,{path.begin(),path.end()},kind});
    std::iter_swap(bindings_.begin()+static_cast<std::ptrdiff_t>(binding_cursor_),bindings_.end()-1);
    return bindings_[binding_cursor_++].key;
}
Result<bool> Workspace::apply_view(Session& session,const ui::Action& action,std::array<float,3> focus) {
    if(action.kind==ui::ActionKind::activate && (action.key==control::group_components || action.key==control::group_categories)) {
        if(auto cancelled=session.cancel_edit();!cancelled) return std::unexpected(cancelled.error());
        parameter_by_category_=action.key==control::group_categories;return true;
    }
    sync_world(session);
    auto candidate=view;
    const auto edited=lighting.apply(action,candidate);
    if(!edited) return std::unexpected(edited.error());
    if(*edited) {
        if(auto published=set_view_settings(session,candidate,focus);!published) return std::unexpected(published.error());
        return true;
    }
    if (action.kind!=ui::ActionKind::activate) return false;
    const auto key=action.key;
    const auto set=[&](ViewOption option,double value) {
        auto changed=set_view_option(candidate,option,value);
        if(!changed) throw std::runtime_error(changed.error().message);
    };
    if (key>=control::shading_preset_first && key<control::shading_preset_first+4) {
        const auto preset=key-control::shading_preset_first;
        set(ViewOption::wireframe,preset==0?double(renderer::Wireframe::only):double(renderer::Wireframe::off));
        if (preset) set(ViewOption::shading,preset==1?double(renderer::Shading::smooth):
            preset==2?double(renderer::Shading::matcap):double(renderer::Shading::scene_lit));
    } else if(key>=control::display_grid && key<=control::display_lit && key!=control::display_fps) {
        const auto* checked=std::get_if<bool>(&action.value);if(!checked) return false;
        if(key==control::display_grid) set(ViewOption::reference_grid,*checked?1:0);
        else if(key==control::display_axes) set(ViewOption::origin_axes,*checked?1:0);
        else if(key==control::display_wire) set(ViewOption::wireframe,double(*checked?renderer::Wireframe::overlay:renderer::Wireframe::off));
        else if(key==control::display_wire_only) set(ViewOption::wireframe,double(*checked?renderer::Wireframe::only:renderer::Wireframe::off));
        else if(key==control::display_triangles) set(ViewOption::triangle_edges,*checked?1:0);
        else if(key==control::display_cavity) set(ViewOption::cavity,*checked?1:0);
        else set(ViewOption::shading,double(*checked?renderer::Shading::scene_lit:renderer::Shading::smooth));
    } else if(key==control::cavity_enabled) {
        const auto* checked=std::get_if<bool>(&action.value);if(!checked) return false;
        set(ViewOption::cavity,*checked?1:0);
    } else if(key>=control::cavity_value_first && key<control::cavity_value_first+15) {
        const auto index=key-control::cavity_value_first;
        set(cavity_options[index/5].option,cavity_values[index/5][index%5]);
    } else if (key>=control::shading_first && key<control::shading_first+3)
        set(ViewOption::shading,double(key-control::shading_first));
    else if(key==control::shading_scene_lit) set(ViewOption::shading,double(renderer::Shading::scene_lit));
    else if (key>=control::edges_first && key<control::edges_first+4) {
        const auto option=key-control::edges_first;
        set(ViewOption::wireframe,double(option<2?option:2));
        set(ViewOption::hidden_lines,option!=3?1:0);
    } else if (key>=control::topology_first && key<control::topology_first+2)
        set(ViewOption::triangle_edges,key!=control::topology_first?1:0);
    else if (key==control::guide_background || key==control::guide_grid || key==control::guide_axes) {
        const auto* checked=std::get_if<bool>(&action.value);
        if (!checked) return false;
        if (key==control::guide_background) {
            if(view.shading==renderer::Shading::scene_lit) return false;
            set(ViewOption::background,*checked?1:0);
        }
        else if (key==control::guide_grid) set(ViewOption::reference_grid,*checked?1:0);
        else set(ViewOption::origin_axes,*checked?1:0);
    }
    else return false;
    if(auto updated=set_view_settings(session,candidate,focus);!updated) return std::unexpected(updated.error());
    status=L"Viewport display updated";
    return true;
}
std::optional<Workspace::TreeDrop> Workspace::tree_drop(const Session& session,const ui::UiFrame& frame,ObjectId dragged,ui::DragPoint at) const {
    const auto* tree=frame.find(control::entity_tree);
    if(!tree || at.x<tree->bounds.x || at.x>=tree->bounds.x+tree->bounds.width || at.y<tree->bounds.y || at.y>=tree->bounds.y+tree->bounds.height) return {};
    for(const auto& box:frame.controls) {
        // Rows scrolled out of the tree are outside its bounds, so the tree test above already excludes them.
        if(box.kind!=ui::ControlKind::tree_item || box.parent!=control::entity_tree || at.y<box.bounds.y || at.y>=box.bounds.y+box.bounds.height) continue;
        if(box.key==control::tree_world) return TreeDrop{{},box.key};
        const auto* target=binding(box.key);
        if(!target || target->kind!=Binding::Kind::object || !target->path.empty()) return {};
        // An Entity cannot become a child of itself or of its own descendant.
        auto& runtime=session.world().runtime();
        for(auto ancestor=target->object;ancestor!=ObjectId{};) {
            if(ancestor==dragged) return {};
            const auto parent=world::parent_entity(runtime,ancestor);if(!parent) return {};
            ancestor=*parent;
        }
        return TreeDrop{target->object,box.key};
    }
    // Empty space below the rows is World, as in Houdini's tree view.
    return TreeDrop{{},control::tree_world};
}
bool Workspace::sync_world(const Session& session) {
    auto& author=static_cast<rendering::SceneLightingSettings&>(view.lit);
    const auto current=session.presentation().environment();
    if(!current || author==*current) return false;
    lighting.cancel();author=*current;++view_revision_;return true;
}
Result<void> Workspace::set_view_settings(Session& session,const renderer::ViewSettings& candidate,std::array<float,3> focus) {
    if(auto valid=validate_view_options(candidate,focus);!valid) return valid;
    if(auto changed=session.set_environment(candidate.lit);!changed) return changed;
    if(view!=candidate) {lighting.cancel();view=candidate;++view_revision_;}
    return {};
}
namespace {
double scalar_number(const Scalar& value) {
    return std::visit([](const auto& scalar)->double {
        if constexpr(std::same_as<std::decay_t<decltype(scalar)>,EnumValue>) return scalar.bits;
        else return static_cast<double>(scalar);
    },value);
}
// Typed and dragged values stay within the hard bounds, as Runtime writes clamp to them.
// A stored value outside them (a trusted native write) widens the range so it still displays.
std::pair<double,double> legal_range(const Scalar& value,const FieldRange& range) {
    auto limits=std::visit([](const auto& scalar)->std::pair<double,double> {
        using T=std::decay_t<decltype(scalar)>;
        if constexpr(std::same_as<T,EnumValue> || std::same_as<T,bool>) return {-std::numeric_limits<double>::max(),std::numeric_limits<double>::max()};
        else return {double(std::numeric_limits<T>::lowest()),double(std::numeric_limits<T>::max())};
    },value);
    if(range.hard_min) limits.first=scalar_number(*range.hard_min);
    if(range.hard_max) limits.second=scalar_number(*range.hard_max);
    const auto current=scalar_number(value);
    return {std::min(limits.first,current),std::max(limits.second,current)};
}
// Slider endpoints are the UI range; a side with only a hard bound uses it.
std::optional<std::pair<double,double>> slider_range(const FieldRange& range) {
    const auto& low=range.ui_min?range.ui_min:range.hard_min;const auto& high=range.ui_max?range.ui_max:range.hard_max;
    if(!low || !high || !(scalar_number(*low)<scalar_number(*high))) return std::nullopt;
    return std::pair{scalar_number(*low),scalar_number(*high)};
}
std::string range_note(const FieldRange& range) {
    const auto text=[](const Scalar& value) {
        char buffer[32];const auto written=std::to_chars(buffer,buffer+sizeof(buffer),scalar_number(value));
        return std::string(buffer,written.ptr);
    };
    std::string note;
    const auto add=[&](const std::string& part){note+=(note.empty()?"":"，")+part;};
    if(range.ui_min || range.ui_max) add("范围 "+(range.ui_min?text(*range.ui_min):"-∞")+" – "+(range.ui_max?text(*range.ui_max):"∞"));
    const bool lock_min=range.hard_min && range.hard_min==range.ui_min,lock_max=range.hard_max && range.hard_max==range.ui_max;
    if(lock_min && lock_max) add("上下限锁定");
    else if(lock_min) add("下限锁定");
    else if(lock_max) add("上限锁定");
    if(range.hard_min && !lock_min) add("硬下限 "+text(*range.hard_min));
    if(range.hard_max && !lock_max) add("硬上限 "+text(*range.hard_max));
    return note;
}
std::string described(std::string description,const FieldRange& range) {
    if(auto note=range_note(range);!note.empty()) description+=(description.empty()?"":"\n")+note;
    return description;
}
// Houdini-style parameter label from a reflected field path: "material.base_color"
// becomes "Base Color". The path still identifies the field; only the caption changes.
ui::ControlIcon light_icon(presentation::LightKind kind) {
    switch(kind) {
    case presentation::LightKind::point:return ui::ControlIcon::point_light;
    case presentation::LightKind::rectangle:return ui::ControlIcon::area_light;
    case presentation::LightKind::directional:return ui::ControlIcon::sun_light;
    }
    return ui::ControlIcon::point_light;
}
// One glyph per Entity, as Houdini's network/tree icons: the type is carried by
// the icon so rows can show only the name.
ui::ControlIcon entity_icon(const Session& session,ObjectId entity) {
    auto& runtime=session.world().runtime();
    const auto state=world::inspect_entity(runtime,entity);
    if(state && runtime.is_a(state->type,session.presentation_types().static_mesh_entity).value_or(false)) return ui::ControlIcon::mesh;
    if(const auto parts=world::components(runtime,entity)) for(const auto& part:*parts) {
        const auto type=runtime.type_of(part.id());
        if(type && runtime.is_a(*type,session.presentation_types().light_component).value_or(false))
            if(const auto light=presentation::read_light(runtime,part.id())) return light_icon(light->kind);
    }
    return ui::ControlIcon::object;
}
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
std::vector<ui::Control> Workspace::describe(Session& session, ui_layout::Size size, const ui::UiFrame* frame,Project* project,const ui::Session* ui) {
    property_help_.begin();
    sync_world(session);
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
    // The World already indexes all attached components, including disabled
    // ones. UI liveness only needs their IDs, not a new Root for each member.
    const auto components=take(session.world().query(session.types().component));
    Set<ObjectId> live;live.reserve(members.size()+components.size()+1);
    for(const auto& member:members) live.add(member.id());
    for(const auto component:components) live.add(component);
    const auto material_object=inspecting_asset?session.loaded_material(material_inspection):ObjectId{};
    if(material_object!=ObjectId{}) live.add(material_object);
    // Only material objects can extend the membership directory. Most frames
    // reference Entity/Component bindings exclusively, so avoid reading every
    // mesh's material slots unless an existing UI reference needs that lookup.
    const auto unresolved=[&](ObjectId object) {return object!=ObjectId{} && !live.contains(object);};
    const bool needs_materials=unresolved(session.selection().object) ||
        std::ranges::any_of(navigation_,unresolved) ||
        std::ranges::any_of(bindings_,[&](const auto& binding){return unresolved(binding.object);});
    if(needs_materials) for(const auto component:components) {
        const auto bindings=presentation::read_material_bindings(runtime,component);
        if(bindings) for(const auto& binding:*bindings) live.add(binding.resource.id());
    }
    // Closing/hiding a view is not an object lifetime change. Retire bindings and
    // tree view state together; undo later restores identity with fresh keys.
    std::erase_if(bindings_,[&](const auto& item) {return !live.contains(item.object);});
    for(auto* tree:{&scene_tree_,&structure_tree_})
        std::erase_if(tree->expansion,[&](const auto& entry){return entry.first!=ObjectId{} && !live.contains(entry.first);});
    binding_cursor_=0;
    std::vector<ui::Control> result;
    result.reserve(64 + members.size());
    const auto requested_selection=session.selection().entity;
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
    const auto entity_status = selected == ObjectId{} ? world::EntityStatus{} : take(world::inspect_entity(runtime, selected));
    const auto transform = selected!=ObjectId{} && take(world::has_transform(runtime,selected))?std::optional(selected):std::nullopt;
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
    // Houdini status line: the message, then muted scene facts separated by space, not a rule.
    auto facts=std::to_wstring(members.size())+(members.size()==1?L" Entity":L" Entities");
    if(play_pending) facts+=play_diverged?L"   Play diverged":*play_pending?L"   Play: "+std::to_wstring(*play_pending)+L" pending":std::wstring(L"   Play in sync");
    const std::array<ui_language::InputValue,12> shell_inputs{menu_bar_collapsed,ui_language::ItemList{std::move(menus)},
        status_text,ui_language::ActionBinding{control::menu_bar_handle},
        double(ui::stow_rail_width),double(ui::stow_collapsed_height),double(ui::stow_arrow_region_height),
        double(ui::ControlIcon::up),double(ui::ControlIcon::down),
        std::move(facts),play_diverged,ui_language::ActionBinding{control::command_key(Command::restart_project),command_enabled(Command::restart_project,session,project)}};
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
        const auto icon=definition.id==Command::create_point_light?ui::ControlIcon::point_light:
            definition.id==Command::create_rectangle_light?ui::ControlIcon::area_light:
            definition.id==Command::create_directional_light?ui::ControlIcon::sun_light:
            definition.id==Command::create_static_mesh?ui::ControlIcon::mesh:
            definition.id==Command::create_entity?ui::ControlIcon::object:ui::ControlIcon::axes;
        auto& items=definition.group==CommandGroup::create?create_tools:view_tools;
        items->push_back({shelf_items_[index],{std::wstring(title),static_cast<double>(icon),
            ui_language::ActionBinding{static_cast<ui::ControlKey>(definition.id),command_enabled(definition.id,session,project)}}});
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
                return describe(session,size,frame,project,ui);
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
        // Bindings as actually routed by Application::viewport_input and ViewportTool.
        constexpr std::array tool_hints{
            L"Left click places the 3D cursor. Right click selects, drag box-selects. Middle tumbles, Shift+Middle pans.",
            L"Drag a handle to move, rotate or scale. G / R / S for modal transforms. Middle tumbles, Shift+Middle pans.",
            L"Drag an arrow or plane to move. G then X/Y/Z constrains, type a value, Enter confirms.",
            L"Drag a ring to rotate. R then X/Y/Z constrains, type degrees, Enter confirms.",
            L"Drag a box to scale an axis, the centre for uniform scale. S then X/Y/Z constrains."};
        const auto open=[&](ui::ControlKey key){return menu && menu->source==key;};
        // Blender's shading buttons: Wireframe / Solid / MatCap / Scene Lit, routed as the Z pie presets.
        auto shading_modes=std::make_shared<std::vector<ui_language::Item>>();
        const auto shading_selected=this->view.wireframe==renderer::Wireframe::only?0u:this->view.shading==renderer::Shading::matcap?2u:
            this->view.shading==renderer::Shading::scene_lit?3u:1u;
        constexpr std::array shading_icons{ui::ControlIcon::shade_wire,ui::ControlIcon::shade_solid,ui::ControlIcon::shade_matcap,ui::ControlIcon::shade_lit};
        for(std::size_t i=0;i<shading_icons.size();++i) {
            const auto key=control::shading_preset_first+i;
            shading_modes->push_back({930+i,{ui_language::ActionBinding{key,true},double(shading_icons[i]),shading_selected==i,
                control::viewport_shading_modes,std::wstring(i==0?L"viewport-segment-item first":i+1==shading_icons.size()?L"viewport-segment-item last":L"viewport-segment-item")}});
        }
        // Blender's select-mode segment: Set / Extend / Subtract / Invert / Intersect.
        auto select_modes=std::make_shared<std::vector<ui_language::Item>>();
        constexpr std::array select_icons{ui::ControlIcon::select_new,ui::ControlIcon::select_extend,ui::ControlIcon::select_subtract,
            ui::ControlIcon::select_invert,ui::ControlIcon::select_intersect};
        for(std::size_t i=0;i<select_icons.size();++i) {
            const auto command=static_cast<ui::ControlKey>(Command::select_mode_set)+i;
            select_modes->push_back({990+i,{ui_language::ActionBinding{command,true},double(select_icons[i]),
                std::size_t(viewport_settings.select_mode)==i,control::viewport_select_modes,
                std::wstring(i==0?L"viewport-segment-item first":i+1==select_icons.size()?L"viewport-segment-item last":L"viewport-segment-item")}});
        }
        // Houdini display bar: each toggle mirrors the one ViewSettings value it edits.
        auto display_toggles=std::make_shared<std::vector<ui_language::Item>>();
        const auto lit=this->view.shading==renderer::Shading::scene_lit;
        const std::array<std::tuple<ui::ControlKey,ui::ControlIcon,bool,bool>,8> display{{
            {control::display_grid,ui::ControlIcon::grid,this->view.reference_grid,false},
            {control::display_axes,ui::ControlIcon::axes_origin,this->view.origin_axes,false},
            {control::display_wire,ui::ControlIcon::wire_overlay,this->view.wireframe==renderer::Wireframe::overlay,true},
            {control::display_wire_only,ui::ControlIcon::shade_wire,this->view.wireframe==renderer::Wireframe::only,false},
            {control::display_triangles,ui::ControlIcon::mesh,this->view.triangle_edges,false},
            {control::display_cavity,ui::ControlIcon::shade_matcap,this->view.cavity.enabled && !lit,true},
            {control::display_lit,ui::ControlIcon::shade_lit,lit,false},
            {control::display_fps,ui::ControlIcon::fps,show_fps,true}}};
        for(std::size_t index=0;const auto& [key,icon,selected,grouped]:display) {
            const auto target=key==control::display_fps?control::command_key(Command::show_fps):key;
            display_toggles->push_back({control::display_toggle_first+index++,{ui_language::ActionBinding{target,true},double(icon),selected,
                std::wstring(grouped?L"display-toggle grouped":L"display-toggle")}});
        }
        const auto step=viewport_settings.grid_spacing;
        const std::wstring step_name=step==.1f?L"0.1 m":step==.2f?L"0.2 m":step==.5f?L"0.5 m":L"1 m";
        const std::array<ui_language::InputValue,49> inputs{ui_language::ActionBinding{control::scene_back,can_navigate(-1)},
            ui_language::ActionBinding{control::scene_forward,can_navigate(1)},double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world),
            double(ui::ControlIcon::down),double(ui::ControlIcon::overlays),menu && menu->source==control::scene_shading,menu && menu->source==control::scene_guides,
            ui_language::ItemList{std::move(modes)},double(ui::ControlIcon::cursor3d),menu && menu->source==control::viewport_pivot,
            std::wstring(camera_flying?L"Fly":mode_names[std::size_t(viewport_mode)]),double(camera_flying?ui::ControlIcon::fly:icons[std::size_t(viewport_mode)]),
            std::wstring(viewport_mode==TransformTool::scale || viewport_settings.orientation==Orientation::local?L"Local":L"Global"),
            std::wstring(viewport_settings.pivot==PivotMode::median?L"Selection Center":L"3D Cursor"),
            ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::snap_toggle)},double(ui::ControlIcon::magnet),viewport_settings.grid_snap,
            step_name,menu && menu->source==control::viewport_snap,viewport_settings.cursor_surface,
            ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::cursor_surface)},std::wstring(camera_view_name(camera_view)),
            menu && menu->source==control::viewport_camera,ui_language::ActionBinding{static_cast<ui::ControlKey>(Command::viewport_options)},
            viewport_options_collapsed,double(ui::ControlIcon::up),double(ui::ControlIcon::down),viewport_readout,!viewport_readout.empty(),
            ui_language::ActionBinding{control::viewport_rail_snap},std::wstring(camera_flying?L"WASD move · Q/E down/up · Wheel speed · Shift fast · Enter/LMB confirm · Esc/RMB cancel":tool_hints[std::size_t(viewport_mode)]),
            std::wstring(this->view.shading==renderer::Shading::scene_lit || this->view.background==renderer::Background::none?
                L"viewport-tool-hint dark-background":L"viewport-tool-hint"),menu && menu->source==control::viewport_cavity,
            show_fps,viewport_fps.empty()?std::wstring(L"FPS —"):viewport_fps,camera_flying,
            ui_language::ItemList{std::move(shading_modes)},ui_language::ItemList{std::move(select_modes)},
            open(control::viewport_view_menu),open(control::viewport_select_menu),open(control::viewport_header_add),open(control::viewport_object_menu),
            open(control::viewport_tools_menu),pane.bounds.width<660,ui_language::ItemList{std::move(display_toggles)},
            double(ui::ControlIcon::axes_origin),open(control::viewport_orientation)};
        const std::array<ui_language::KeyBinding,27> keys{{{"navigation",641},{"path",control::scene_path},{"back",control::scene_back},{"readout",control::viewport_readout},
            {"fps",control::viewport_fps},{"fly_reticle",control::viewport_fly_reticle},{"tool_hint",control::viewport_hint},
            {"cavity",control::viewport_cavity},
            {"forward",control::scene_forward},{"display",control::scene_shading},{"guides",control::scene_guides},
            {"viewport",control::viewport},{"pivot",control::viewport_pivot},{"edit_tools",control::viewport_modes},
            {"actions",control::viewport_tools_menu},{"step",control::viewport_snap},{"camera",control::viewport_camera},{"mode",control::viewport_hud},
            {"view_menu",control::viewport_view_menu},{"select_menu",control::viewport_select_menu},{"add_menu",control::viewport_header_add},
            {"object_menu",control::viewport_object_menu},{"shading_modes",control::viewport_shading_modes},{"selection_modes",control::viewport_select_modes},
            {"snapping",control::viewport_snapping},{"display_bar",control::display_bar},{"basis",control::viewport_orientation}}};
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
    if (view.key==world_control::tab) {
        lighting.append(result,view.content,*ui_document_,this->view,!session.world().running());continue;
    }
    if (view.key==control::assets_tab) {
        assets.append(result,view.content,project,command_enabled(Command::create_static_mesh,session,project),frame,*ui_document_);
        continue;
    }
    if (view.key==control::entities_tab) {
        // Every member is flattened each frame; only rows near the viewport are described.
        auto& tree=scene_tree_;
        tree.nodes.clear();tree.objects.clear();
        tree.nodes.push_back({ui::no_tree_node,tree.expanded(ObjectId{},true),L"World"});tree.objects.push_back({});
        std::vector<std::uint32_t> path{0}; // Members are in composition preorder.
        for (const auto& member:members) {
            const auto parent=take(world::parent_entity(runtime,member.id()));
            while (path.size()>1 && tree.objects[path.back()]!=parent) path.pop_back();
            path.push_back(static_cast<std::uint32_t>(tree.nodes.size()));
            tree.nodes.push_back({path[path.size()-2],tree.expanded(member.id(),false),
                tree_query.empty()?std::wstring_view{}:session.name(member.id())});
            tree.objects.push_back(member.id());
        }
        const auto* focused=ui?binding(ui->focused_control()):nullptr;
        const std::array pinned{selected,focused && focused->kind==Binding::Kind::object?focused->object:selected,
            row_rename && row_rename->object!=ObjectId{}?row_rename->object:selected};
        auto window=window_tree(tree,selected,tree_query,ui?ui->tree_viewport(control::entity_tree):ui::TreeViewport{0,size.height,12},pinned);
        // World stays when the filter matches nothing.
        if (tree.rows.empty()) {tree.rows.push_back({0,ui::no_tree_node,0,false,false,{}});window={0};}
        auto rows=std::make_shared<std::vector<ui_language::Item>>();rows->reserve(window.size());
        Set<ObjectId> selected_entities;selected_entities.reserve(session.selected_entities().size());
        for(const auto entity:session.selected_entities()) selected_entities.add(entity);
        ui_language::TreeRowBinding world_row;
        for (const auto index:window) {
            const auto& row=tree.rows[index];
            const auto entity=tree.objects[row.node];
            const ui_language::TreeRowBinding position{index,row.depth,row.branch,row.expanded,entity==selected,row.lines};
            if (!row.node) {world_row=position;continue;}
            const auto state=take(world::inspect_entity(runtime,entity));
            const auto entity_key=bind(entity);
            const bool mesh_entity=take(runtime.is_a(state.type,session.presentation_types().static_mesh_entity));
            const bool retained=session.has_retained(entity) || std::ranges::any_of(take(world::components(runtime,entity)),
                [&](const auto& component){return session.has_retained(component.id());});
            rows->push_back({entity_key,{std::wstring(session.name(entity)),
                state.state==world::EntityState::attached_inactive || state.state==world::EntityState::active,
                selected_entities.contains(entity),static_cast<double>(entity_icon(session,entity)),position,
                std::wstring(mesh_entity?L"mesh-entity":L"entity")+(retained?L" retained":L"")+(tree_drop_row==entity_key?L" drop-target":L""),state.can_edit,entity_key}});
        }
        const std::array<ui_language::InputValue,12> inputs{ui_language::ItemList{std::move(rows)},static_cast<double>(tree.rows.size()),
            ui_language::TextEditBinding{control::tree_filter,tree_query},selected==ObjectId{},
            world_row,static_cast<double>(ui::ControlIcon::world),
            ui_language::ActionBinding{control::entity_back,can_navigate(-1)},ui_language::ActionBinding{control::entity_forward,can_navigate(1)},
            double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world),
            std::wstring(tree_drop_row==control::tree_world?L"drop-target":L"")};
        const std::array<ui_language::KeyBinding,6> keys{{{"tree",control::entity_tree},{"world",control::tree_world},
            {"path.bar",24},{"path.path",control::entity_path},{"path.back",control::entity_back},{"path.forward",control::entity_forward}}};
        static_cast<void>(ui_document_->append("SceneTree",21,keys,inputs,result));
        continue;
    }
    const auto short_type=[&](ObjectId object) {
        auto name=take(runtime.describe(take(runtime.type_of(object))))->name;
        if(const auto dot=name.find_last_of('.');dot!=name.npos) name=name.substr(dot+1);
        return wide(name);
    };
    // Placeholders show the saved type they stand in for.
    const auto object_title=[&](ObjectId object) {
        if(session.has_retained(object)) if(const auto saved=session.retained(object);saved && !saved->type.empty()) {
            auto name=saved->type;if(const auto dot=name.find_last_of('.');dot!=name.npos) name=name.substr(dot+1);
            return wide(name)+L" (missing type)";
        }
        return short_type(object);
    };
    if(view.key==control::structure_tab) {
        auto& tree=structure_tree_;
        build_structure_tree(session,selected);
        tree.texts.clear();
        if(!structure_query.empty()) {
            tree.texts.reserve(tree.objects.size());
            for(const auto object:tree.objects) tree.texts.push_back(session.world().contains(object)?std::wstring(session.name(object)):object_title(object));
            for(std::size_t i=0;i<tree.nodes.size();++i) tree.nodes[i].text=tree.texts[i];
        }
        const auto* focused=ui?binding(ui->focused_control()):nullptr;
        const std::array pinned{inspected,focused && focused->kind==Binding::Kind::structure?focused->object:inspected};
        const auto window=window_tree(tree,inspected,structure_query,
            ui?ui->tree_viewport(control::structure_tree):ui::TreeViewport{0,size.height,12},pinned);
        auto rows=std::make_shared<std::vector<ui_language::Item>>();rows->reserve(window.size());
        for(const auto index:window) {
            const auto& row=tree.rows[index];
            const auto object=tree.objects[row.node];
            const ui_language::TreeRowBinding position{index,row.depth,row.branch,row.expanded,object==inspected,row.lines};
            const auto key=bind(object,{},Binding::Kind::structure);
            if(session.world().contains(object)) {
                const auto state=take(world::inspect_entity(runtime,object));
                rows->push_back({key,{std::wstring(session.name(object)),state.can_edit,inspected==object,double(entity_icon(session,object)),
                    position,std::wstring(session.has_retained(object)?L"entity retained":L"entity"),false,ui::ControlKey{}}});
            } else {
                // A Component row's parent is its Entity.
                const auto state=take(world::inspect_entity(runtime,tree.objects[tree.nodes[row.node].parent]));
                rows->push_back({key,{object_title(object),state.can_edit,inspected==object,double(ui::ControlIcon::component),
                    position,std::wstring(session.has_retained(object)?L"component retained":L"component"),false,ui::ControlKey{}}});
            }
        }
        const std::array<ui_language::InputValue,9> inputs{selected==ObjectId{}?std::wstring(L"Entity Structure"):std::wstring(session.name(selected)),
            ui_language::ItemList{std::move(rows)},static_cast<double>(tree.rows.size()),
            ui_language::TextEditBinding{control::structure_filter,structure_query},selected!=ObjectId{},
            selected!=ObjectId{} && entity_status.can_edit,control::add_component,
            ui_language::ActionBinding{control::remove_selected,command_enabled(Command::erase_selected,session,project)},double(ui::ControlIcon::close)};
        const std::array<ui_language::KeyBinding,2> keys{{{"tree",control::structure_tree},{"add",control::add_component}}};
        static_cast<void>(ui_document_->append("EntityStructure",view.content,keys,inputs,result));
        continue;
    }
    if(inspecting_asset && material_object==ObjectId{}) {
        const auto& asset=assets.selected();auto entries=std::make_shared<std::vector<ui_language::Item>>();
        const auto add=[&](std::size_t index,std::wstring name,std::wstring value) {
            auto& key=resource_properties_[index];if(!key) key=next_binding_++;
            entries->push_back({key,{std::move(name),std::move(value)}});
        };
        std::wstring kind=L"Resource";
        if(asset) {
            kind=asset->kind==AssetKind::mesh?L"Mesh":asset->kind==AssetKind::scene?L"Scene":asset->kind==AssetKind::source?L"Source":asset->kind==AssetKind::folder?L"Folder":asset->kind==AssetKind::material?L"Material":asset->kind==AssetKind::texture?L"Texture":L"File";
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
    const bool show_position=!parameter_by_category_ && material_object==ObjectId{} && transform && (inspected==selected || inspected==*transform);
    auto groups=std::make_shared<std::vector<ui_language::Item>>();
    std::vector<std::pair<std::string,std::shared_ptr<std::vector<ui_language::Item>>>> category_rows;
    const auto category_properties=[&](std::string_view category) {
        auto at=std::ranges::find_if(category_rows,[&](const auto& group){return group.first==category;});
        if(at==category_rows.end()) {
            category_rows.emplace_back(std::string(category),std::make_shared<std::vector<ui_language::Item>>());
            at=std::prev(category_rows.end());
        }
        return at->second;
    };
    const auto append_properties=[&](ObjectId object,ObjectId material_component=ObjectId{},std::string_view slot=std::string_view{}) {
        const bool resource=session.is_material(object);
        const bool instance=material_component!=ObjectId{};
        const auto material_state=resource?std::optional(take(material::read_material(runtime,object))):std::nullopt;
        const auto material_bindings=resource?atelier::Result<std::vector<presentation::MaterialBinding>>{std::vector<presentation::MaterialBinding>{}}:presentation::read_material_bindings(runtime,object);
        const auto mesh=!resource && material_bindings?std::optional(take(presentation::read_static_mesh(runtime,object))):std::nullopt;
        const bool all_slots_bound=mesh && mesh->mesh.resolved && !material_bindings->empty() &&
            std::ranges::all_of(mesh->mesh.resolved->parts(),[&](const auto& part) {
                return std::ranges::find(*material_bindings,presentation::material_slot_name(*mesh->mesh.resolved,part.material),
                    &presentation::MaterialBinding::slot)!=material_bindings->end();
            });
        const auto property_caption=[&](std::string_view name,std::string_view declared) {
            auto label=declared.empty()?parameter_label(name):wide(declared);
            if(!resource && material_bindings && !material_bindings->empty() && !all_slots_bound && name.starts_with("material."))
                label+=L" (Unbound)";
            return label;
        };
        const auto property_key=[&](std::span<const FieldId> path,Binding::Kind kind=Binding::Kind::field) {
            return instance?bind_material(object,path,kind,material_component,slot):bind(object,path,kind);
        };
        const auto type=take(runtime.type_of(object));
        const auto title=instance?L"Material Overrides · "+wide(slot):object_title(object);
        const bool group_matches=lowercase(title).find(folded)!=std::wstring::npos;
        auto component_properties=std::make_shared<std::vector<ui_language::Item>>();
        auto properties=component_properties;
        const auto schema=take(session.properties(object));
        const auto light=take(runtime.is_a(type,session.presentation_types().light_component))?
            std::optional(take(presentation::read_light(runtime,object))):std::nullopt;
        for(std::size_t index=0;index<schema.size();++index) {
            const auto& property=schema[index];
            if(parameter_by_category_ && material_object==ObjectId{}) {
                const std::string_view category=resource?std::string_view{"Material"}:
                    property.category.empty()?std::string_view{"General"}:std::string_view{property.category};
                properties=category_properties(category);
            }
            if(instance && !property.name.starts_with("parameters.")) continue;
            if(all_slots_bound && property.name.starts_with("material.")) continue;
            const auto override_bit=instance?take(material::override_bit(runtime,property.path)):0u;
            const bool overridden=instance && (material_state->overrides & override_bit)!=0;
            const bool override_available=instance && (material_state->parent.registered() || !material_state->source.empty());
            const bool editable=(resource?!session.world().running():entity_status.can_edit) && (!override_available || overridden);
            const auto override_key=instance?bind_material(object,property.path,Binding::Kind::material_override,material_component,slot):ui::ControlKey{};
            if(show_position && object==*transform && detail::transform_path(session.types(),property.path)) continue;
            if(light) {
                const auto& name=property.name;
                if(light->kind!=presentation::LightKind::rectangle && (name=="settings.width" || name=="settings.height" || name=="settings.two_sided")) continue;
                if(light->kind==presentation::LightKind::rectangle && name=="settings.source_radius") continue;
                if(light->kind==presentation::LightKind::directional && name=="settings.range") continue;
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
                const bool base_color=(aggregate_type==session.presentation_types().material || aggregate_type==take(runtime.native_type<material::ParameterValues>())) &&
                    property.path[property.path.size()-2]==take(runtime.find_field(aggregate_type,"base_color"));
                const bool emission=(aggregate_type==session.presentation_types().material || aggregate_type==take(runtime.native_type<material::ParameterValues>())) &&
                    property.path[property.path.size()-2]==take(runtime.find_field(aggregate_type,"emissive"));
                const bool light_color=aggregate_type==session.presentation_types().light_settings &&
                    property.path[property.path.size()-2]==take(runtime.find_field(session.presentation_types().light_settings,"color"));
                const bool color=base_color || emission || light_color;
                const auto euler=spatial_rotation?euler_degrees(take(world::read_transform(runtime,object)).rotation):math::Vector3{};
                std::vector<ui::NumberField> tuple;bool matches=group_matches || lowercase(wide(name+" "+property.label+" "+property.category)).find(folded)!=std::wstring::npos;
                for(std::size_t lane=0;lane<count && index+lane<schema.size();++lane) {
                    const auto& field=schema[index+lane];
                    if(field.path.size()!=property.path.size() || !std::equal(prefix.begin(),prefix.end(),field.path.begin())) break;
                    matches|=lowercase(wide(field.name)).find(folded)!=std::wstring::npos;
                    if(spatial_rotation && lane==3) continue;
                    const auto value=spatial_rotation?double(euler[lane]):std::visit([](const auto& v)->double {
                        if constexpr(std::same_as<std::decay_t<decltype(v)>,EnumValue>) return v.bits;
                        else return double(v);
                    },field.value);
                    const auto [field_minimum,field_maximum]=legal_range(field.value,field.range);
                    const double minimum=spatial_position?session.parameters()[lane].minimum:spatial_scale?-Session::maximum_scale:spatial_rotation?-360000:field_minimum;
                    const double maximum=spatial_position?session.parameters()[lane].maximum:spatial_scale?Session::maximum_scale:spatial_rotation?360000:field_maximum;
                    const auto key=property_key(field.path,spatial_rotation?Binding::Kind::rotation:Binding::Kind::field);
                    tuple.push_back({key,std::wstring(1,(color?L"RGBW":L"XYZW")[lane]),value,spatial_position || spatial_rotation?0:spatial_scale?1:value,
                        minimum,maximum,spatial_rotation?1:.01,spatial_rotation?.1:.001,static_cast<ui::VectorAxis>(lane+1)});
                    tuple.back().single_precision=parent->math.precision==math::Precision::f32;
                }
                index+=count-1;
                if(matches) {
                    const auto field_key=tuple.front().key;
                    // Swatch shows the display colour; HDR emission is clamped for the chip only.
                    const auto channel=[&](std::size_t lane){return tuple.size()>lane?std::clamp(tuple[lane].value,0.0,1.0):0.0;};
                    const double r=channel(0),g=channel(1),b=channel(2);
                    PropertyRow row;
                    row.key=property_key(prefix,Binding::Kind::property_row);
                    row.title=spatial_rotation?std::wstring(L"Rotation (°)"):property_caption(name,property.label);
                    row.edit={spatial_position?bind(object,{},Binding::Kind::position_default):spatial_rotation?bind(object,{},Binding::Kind::rotation_default):spatial_scale?bind(object,{},Binding::Kind::scale_default):ui::ControlKey{},editable,false,std::move(tuple)};
                    row.enabled=editable; row.change={field_key,editable}; row.target=field_key;
                    if(color) {
                        row.swatch=std::array{r,g,b};row.pick={property_key(prefix,Binding::Kind::color_pick),editable};
                        std::ranges::find(bindings_,row.pick.target,&Binding::key)->color_maximum=row.edit.fields.front().maximum;
                    }
                    row.override_available=override_available; row.overridden=overridden;
                    row.inherit={instance?override_key:field_key,override_available && !session.world().running()};
                    row.help=property_help_.get(row.key,row.title,wide(take(runtime.describe(type))->name)+L"."+wide(name)+L" : "+wide(parent->name)+(spatial_rotation?L" · Euler (°)":L""),described(property.description,property.range));
                    properties->push_back(std::move(row).item());
                }
                continue;
            }
            auto caption=property_caption(property.name,property.label);
            if(light && property.name.starts_with("settings.")) {
                const auto field=std::string_view(property.name).substr(9);
                if(field=="kind") caption=L"Light Type";
                else if(field=="intensity") caption=light->kind==presentation::LightKind::point?L"Intensity (cd)":
                    light->kind==presentation::LightKind::rectangle?L"Intensity (cd/m²)":L"Illuminance (lux)";
                else if(field=="source_radius") caption=light->kind==presentation::LightKind::directional?L"Angular Radius (rad)":L"Source Radius (m)";
                else if(field=="range") caption=L"Range (m)";
                else if(field=="width") caption=L"Width (m)";
                else if(field=="height") caption=L"Height (m)";
                else if(field=="two_sided") caption=L"Two Sided";
                else if(field=="casts_shadow") caption=L"Cast Shadows";
            }
            if(!group_matches && lowercase(caption+L" "+wide(property.name+" "+property.category)).find(folded)==std::wstring::npos) continue;
            std::optional<Scalar> default_value;auto owner=type;
            for(const auto id:property.path) {
                const auto info=take(runtime.describe(owner));const auto at=std::ranges::find(info->fields,id,&FieldInfo::id);
                if(at==info->fields.end()) break;
                default_value=at->default_value;owner=at->value.type;
            }
            const bool boolean=std::holds_alternative<bool>(property.value);
            const auto* enumeration=std::get_if<EnumValue>(&property.value);
            const bool integral=std::holds_alternative<std::int32_t>(property.value) || std::holds_alternative<std::uint32_t>(property.value);
            const auto [minimum,maximum]=legal_range(property.value,property.range);
            const auto key=property_key(property.path);const double value=scalar_number(property.value),initial=default_value?scalar_number(*default_value):value;
            ui_language::EditBinding edit{0,editable,value!=initial,{{key,L"",value,initial,minimum,maximum,integral?1:.01,integral?1:.001}}};
            edit.fields.front().single_precision=std::holds_alternative<float>(property.value);
            // Integral fields commit whole numbers only, so they keep the ladder without a slider.
            if(const auto slider=slider_range(property.range);slider && !integral && !boolean && !enumeration)
                edit.fields.front().slider=ui::SliderRange{property_key(property.path,Binding::Kind::slider),slider->first,slider->second};
            std::wstring choice;
            if(enumeration) for(const auto& member:take(runtime.describe(enumeration->type))->enumeration->values)
                if(member.second==enumeration->bits) {choice=parameter_label(member.first);break;}
            PropertyRow row;
            row.key=property_key(property.path,Binding::Kind::property_row);row.title=std::move(caption);row.edit=std::move(edit);
            row.kind=boolean?PropertyRow::Kind::boolean:enumeration?PropertyRow::Kind::enumeration:PropertyRow::Kind::number;
            row.checked=boolean && std::get<bool>(property.value);row.enabled=editable;row.change={key,editable};
            row.choice=std::move(choice);row.target=key;row.override_available=override_available;row.overridden=overridden;
            row.inherit={instance?override_key:key,override_available && !session.world().running()};
            row.help=property_help_.get(row.key,row.title,wide(take(runtime.describe(type))->name)+L"."+wide(property.name)+L" : "+wide(take(runtime.describe(property.type))->name),described(property.description,property.range));
            properties->push_back(std::move(row).item());
        }
        properties=parameter_by_category_ && material_object==ObjectId{} && material_bindings?
            category_properties("Material"):component_properties;
        const auto selector=[&](ui::ControlKey key,std::wstring caption,std::wstring choice,std::string_view description) {
            if(!group_matches && lowercase(caption+L" "+choice).find(folded)==std::wstring::npos)return;
            PropertyRow row;
            row.key=key;row.title=std::move(caption);row.kind=PropertyRow::Kind::enumeration;
            row.enabled=!session.world().running();row.change={key,row.enabled};row.choice=std::move(choice);row.target=key;
            row.help=property_help_.get(key,row.title,L"Resource binding",description);
            properties->push_back(std::move(row).item());
        };
        const auto mark_unavailable=[&](AssetKind kind,std::string_view key,std::wstring& name) {
            if(const auto* item=session.unavailable(kind,key)) name.insert(0,item->kind==world::DiagnosticKind::unreadable_resource?L"(unreadable) ":L"(missing) ");
        };
        if(resource && !instance) {
            const auto asset=take(session.material_asset(object));
            constexpr std::array names{L"Base Color Texture",L"Roughness Texture",L"Metallic Texture",L"Normal Texture",L"Emissive Texture",L"Opacity Texture"};
            for(std::size_t i=0;i<names.size();++i) {
                auto name=asset.textures[i].empty()?std::wstring(L"None"):wide(asset.textures[i]);
                if(project) for(const auto& entry:project->entries()) if(entry.key==asset.textures[i]) {name=entry.path.stem().wstring();break;}
                mark_unavailable(AssetKind::texture,asset.textures[i],name);
                selector(bind_material(object,{},Binding::Kind::material_texture,{}, {},i),names[i],std::move(name),"选择材质输入使用的 Texture 资源。None 清除该输入的贴图；修改参与材质撤销与保存。");
            }
        } else if(!resource && material_bindings) {
            if(!mesh->mesh.resolved && !mesh->mesh.source.empty() &&
                (group_matches || lowercase(L"Mesh "+wide(mesh->mesh.source)).find(folded)!=std::wstring::npos)) {
                PropertyRow row;
                row.key=bind_material(object,{},Binding::Kind::saved_data,{},"mesh");row.title=L"Mesh";row.kind=PropertyRow::Kind::enumeration;
                row.enabled=false;row.change={row.key,false};row.choice=wide(mesh->mesh.source);mark_unavailable(AssetKind::mesh,mesh->mesh.source,row.choice);row.target=row.key;
                row.help=property_help_.get(row.key,row.title,L"Unavailable resource",
                    "网格资源缺失或无法读取：组件不绘制，原引用和材质绑定原样保存。恢复资源后重新打开场景即可使用。");
                properties->push_back(std::move(row).item());
            }
            if(mesh->mesh.resolved) for(std::size_t i=0;i<mesh->mesh.resolved->materials().size();++i) {
                const auto slot_name=presentation::material_slot_name(*mesh->mesh.resolved,i);std::wstring choice=L"Mesh Default";
                const auto at=std::ranges::find(*material_bindings,slot_name,&presentation::MaterialBinding::slot);
                if(at!=material_bindings->end()) {
                    auto state=take(material::read_material(runtime,at->resource.id()));
                    if(state.parent.registered()) state=take(material::read_material(runtime,state.parent.id()));
                    choice=wide(state.source);
                    if(project) for(const auto& entry:project->entries()) if(entry.key==state.source) {choice=entry.path.stem().wstring();break;}
                    mark_unavailable(AssetKind::material,state.source,choice);
                }
                selector(bind_material(object,{},Binding::Kind::material_slot,{},slot_name),L"Material · "+wide(slot_name),std::move(choice),"为这个 Mesh slot 选择 Material 资源。Mesh Default 恢复网格自带材质；修改参与当前 Scene 的撤销与保存。");
            }
        }
        if(!component_properties->empty()) groups->push_back({property_key({},Binding::Kind::property_group),
            {title,double(object==selected?ui::ControlIcon::object:ui::ControlIcon::component),ui_language::ItemList{std::move(component_properties)}}});
        if(instance || !session.has_retained(object)) return;
        const auto saved=session.retained(object);if(!saved || saved->entries.empty()) return;
        const bool categorized=parameter_by_category_ && material_object==ObjectId{};
        auto saved_rows=categorized?category_properties("Saved Data"):std::make_shared<std::vector<ui_language::Item>>();
        for(const auto& entry:saved->entries) {
            std::string path;for(const auto& part:entry.path) path+=(path.empty()?"":".")+part;
            const auto caption=wide(path);
            if(!group_matches && lowercase(caption).find(folded)==std::wstring::npos) continue;
            PropertyRow row;
            row.key=bind_material(object,{},Binding::Kind::saved_data,{},path);row.title=caption;row.kind=PropertyRow::Kind::enumeration;
            row.enabled=false;row.change={row.key,false};row.choice=wide(entry.value);row.target=row.key;
            row.help=property_help_.get(row.key,row.title,L"Saved data · "+wide(world::name(entry.kind)),
                entry.kind==world::DiagnosticKind::missing_resource || entry.kind==world::DiagnosticKind::unreadable_resource?
                "依赖的资源缺失或无法读取，这些数据无法应用，会原样写回文件。恢复资源后重新打开场景即可应用；Edit > Discard Saved Data 可丢弃。":
                "保存的场景数据无法应用到当前类型。它会原样写回文件；编辑对应字段或使用 Edit > Discard Saved Data 可丢弃。");
            saved_rows->push_back(std::move(row).item());
        }
        if(!categorized && !saved_rows->empty()) groups->push_back({bind(object,{},Binding::Kind::saved_data),
            {L"Saved Data · "+title,double(ui::ControlIcon::component),ui_language::ItemList{std::move(saved_rows)}}});
    };
    if(material_object!=ObjectId{}) {
        append_properties(material_object);
        std::wstring name=wide(material_inspection);
        if(project) for(const auto& entry:project->entries()) if(entry.key==material_inspection) {name=entry.path.stem().wstring();break;}
        const std::array<ui_language::InputValue,7> inputs{name,session.material_dirty(material_inspection),ui_language::ItemList{std::move(groups)},
            parameter_search_visible,ui_language::TextEditBinding{control::parameter_search,query},
            ui_language::ActionBinding{control::parameter_search_toggle},double(ui::ControlIcon::search)};
        static_cast<void>(ui_document_->append("MaterialParameters",view.content,{},inputs,result));continue;
    }
    const auto append_object=[&](ObjectId object) {
        append_properties(object);
        const auto bindings=presentation::read_material_bindings(runtime,object);
        if(bindings) for(const auto& binding:*bindings) append_properties(binding.resource.id(),object,binding.slot);
    };
    if(inspected!=ObjectId{}) {
        append_object(inspected);
        if(inspected==selected) for(const auto& component:take(world::components(runtime,selected))) append_object(component.id());
    }
    for(auto& [category,rows]:category_rows) if(!rows->empty()) {
        auto& key=category_keys_[category];if(!key) key=next_binding_++;
        groups->push_back({key,{wide(category),0.,ui_language::ItemList{std::move(rows)}}});
    }
    constexpr std::array aliases{L"transform entity position 变换 位置 x", L"transform entity position 变换 位置 y", L"transform entity position 变换 位置 z"};
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
        rotation_edit={bind(*transform,{},Binding::Kind::rotation_default),entity_status.can_edit,angles!=math::Vector3{}, {}};
        scale_edit={bind(*transform,{},Binding::Kind::scale_default),entity_status.can_edit,trs.scale!=math::Vector3{1,1,1}, {}};
        for(std::size_t axis=0;axis<3;++axis) {
            rotation_edit.fields.push_back({bind(*transform,session.types().rotation_fields[axis],Binding::Kind::rotation),names[axis],angles[axis],0,-360000,360000,1,.1,static_cast<ui::VectorAxis>(axis+1)});
            scale_edit.fields.push_back({bind(*transform,session.types().scale_fields[axis]),names[axis],trs.scale[axis],1,-Session::maximum_scale,Session::maximum_scale,.01,.001,static_cast<ui::VectorAxis>(axis+1)});
            rotation_edit.fields.back().single_precision=scale_edit.fields.back().single_precision=true;
        }
    }
    const bool matches=position_matches || rotation_matches || scale_matches,empty=groups->empty() && !matches;
    ui_language::EditBinding position{show_position?bind(*transform,{},Binding::Kind::position_default):ui::ControlKey{74},
        entity_status.can_edit && show_position,group_modified,std::move(fields)};
    const bool entity_selected=inspected!=ObjectId{} && inspected==selected;
    const auto transform_help=[&](std::span<const FieldId> lane,ui::ControlKey key,std::wstring_view title,bool rotation=false)->ui_language::TooltipBinding {
        if(!show_position) return {};
        const auto type=take(runtime.describe(take(runtime.type_of(*transform))));
        auto owner=type;const FieldInfo* field{};
        for(std::size_t depth=0;depth+1<lane.size();++depth) {
            field=&*std::ranges::find(owner->fields,lane[depth],&FieldInfo::id);
            owner=take(runtime.describe(field->value.type));
        }
        return property_help_.get(key,title,wide(type->name)+L"."+wide(field->name)+L" : "+wide(owner->name)+(rotation?L" · Euler (°)":L""),field->description);
    };
    ui_language::TooltipBinding type_help;
    if(inspected!=ObjectId{}) {
        const auto type=take(runtime.describe(take(runtime.type_of(inspected))));
        type_help=property_help_.get(52,short_type(inspected),wide(type->name),type->description);
    }
    const std::array transform_descriptions{
        transform_help(session.types().position_fields[0],70,L"Position (m)"),
        transform_help(session.types().rotation_fields[0],rotation_edit.group,L"Rotation (°)",true),
        transform_help(session.types().scale_fields[0],scale_edit.group,L"Scale")};
    const std::array<ui_language::InputValue,29> inputs{inspected==ObjectId{}?std::wstring(L"Object"):object_title(inspected),
        ui_language::TextEditBinding{entity_selected?bind(selected,{},Binding::Kind::name):ui::ControlKey{77},
            inspected==ObjectId{}?L"No selection":entity_selected?std::wstring(session.name(selected)):object_title(inspected),entity_selected && entity_status.can_edit},
        ui_language::ActionBinding{control::parameter_back,can_navigate(-1)},ui_language::ActionBinding{control::parameter_forward,can_navigate(1)},
        double(ui::ControlIcon::back),double(ui::ControlIcon::forward),double(ui::ControlIcon::world),
        inspected!=ObjectId{},ui_language::ItemList{std::move(groups)},matches,std::move(position),ui_language::TextEditBinding{control::parameter_search,query},empty,
        double(entity_selected?entity_icon(session,selected):ui::ControlIcon::component),double(ui::ControlIcon::search),
        parameter_search_visible && inspected!=ObjectId{},ui_language::ActionBinding{control::parameter_search_toggle,inspected!=ObjectId{}},
        std::move(rotation_edit),std::move(scale_edit),position_matches,rotation_matches,scale_matches,parameter_by_category_,
        ui_language::ActionBinding{control::group_components,true},ui_language::ActionBinding{control::group_categories,true},
        transform_descriptions[0],transform_descriptions[1],transform_descriptions[2],std::move(type_help)};
    const std::array<ui_language::KeyBinding,14> keys{{{"components",control::group_components},{"categories",control::group_categories},{"path.bar",75},
        {"path.path",control::parameter_path},{"path.back",control::parameter_back},{"path.forward",control::parameter_forward},
        {"header",51},{"type",52},{"search_toggle",control::parameter_search_toggle},
        {"scroll",control::parameter_list},{"transform",85},
        {"position.section",control::position_fold},{"position.contents",58},{"position.position",70,72,73}}};
    static_cast<void>(ui_document_->append("Parameters",view.content,keys,inputs,result));
    }
    const auto preview=placement.preview.value_or(ui::Rect{});
    const std::array<ui_language::InputValue,5> preview_inputs{placement.preview.has_value(),double(preview.x),double(preview.y),double(preview.width),double(preview.height)};
    const std::array<ui_language::KeyBinding,1> preview_keys{{{"preview",placement.preview_key}}};
    static_cast<void>(ui_document_->append("DockPreview",0,preview_keys,preview_inputs,result));
    menu_items.clear(); component_choices_.clear(); property_choices_.clear();material_choices_.clear(); menu_selection_=session.selection();
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
                ui::MenuItem item{key,std::wstring(definition->title),command_enabled(command,session,project),false,{},std::wstring(definition->shortcut),definition->separator};
                const auto radio=[&](Command first,bool selected) {item.behavior=ui::MenuBehavior::radio;item.radio_group=static_cast<ui::ControlKey>(first);item.selected=selected;};
                if(command>=Command::tool_select && command<=Command::tool_scale)
                    radio(Command::tool_select,key-static_cast<ui::ControlKey>(Command::tool_select)==std::size_t(viewport_settings.tool));
                else if(command==Command::pivot_median || command==Command::pivot_cursor)
                    radio(Command::pivot_median,(command==Command::pivot_cursor)==(viewport_settings.pivot==PivotMode::cursor));
                else if(command>=Command::snap_grid_01 && command<=Command::snap_grid_1) {
                    constexpr std::array steps{.1f,.2f,.5f,1.f};radio(Command::snap_grid_01,viewport_settings.grid_spacing==steps[key-static_cast<ui::ControlKey>(Command::snap_grid_01)]);
                } else if(const auto preset=camera_view_command(command))
                    radio(Command::view_perspective,*preset==camera_view);
                else if(command==Command::show_fps) {item.behavior=ui::MenuBehavior::checkbox;item.selected=show_fps;}
                else if(command==Command::orientation_global || command==Command::orientation_local)
                    radio(Command::orientation_global,(command==Command::orientation_local)==(viewport_settings.orientation==Orientation::local));
                else if(command==Command::snap_affect_move || command==Command::snap_affect_rotate || command==Command::snap_affect_scale) {
                    item.behavior=ui::MenuBehavior::checkbox;
                    item.selected=command==Command::snap_affect_move?viewport_settings.snap_move:command==Command::snap_affect_rotate?viewport_settings.snap_rotate:viewport_settings.snap_scale;
                } else if(command==Command::snap_rotation_1 || command==Command::snap_rotation_5 || command==Command::snap_rotation_15 || command==Command::snap_rotation_45)
                    radio(Command::snap_rotation_1,viewport_settings.rotation_step==(command==Command::snap_rotation_1?1.f:command==Command::snap_rotation_5?5.f:command==Command::snap_rotation_15?15.f:45.f));
                else if(command>=Command::select_mode_set && command<=Command::select_mode_intersect)
                    radio(Command::select_mode_set,key-static_cast<ui::ControlKey>(Command::select_mode_set)==std::size_t(viewport_settings.select_mode));
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
                    snap_affect_move,snap_affect_rotate,snap_affect_scale,snap_rotation_1,snap_rotation_5,snap_rotation_15,snap_rotation_45,
                    selection_grid,selection_cursor,cursor_selected,cursor_origin,cursor_grid,cursor_surface});
                case control::viewport_camera: return commands({view_perspective,view_front,view_back,view_left,view_right,view_top,view_bottom,focus,reset_camera});
                case control::viewport_pivot: return commands({pivot_median,pivot_cursor,cursor_selected,cursor_origin,cursor_grid});
                case control::viewport_orientation: {
                    auto items=commands({orientation_global,orientation_local});items.front().separator_before=false;
                    items.front().heading=L"Transform Orientations";return items;
                }
                default: return std::vector<ui::MenuItem>{};
                }
            };
            const auto submenu=[&](ui::ControlKey key,std::wstring_view title,std::vector<ui::MenuItem> children,bool separator=false) {
                ui::MenuItem item{.key=key,.text=std::wstring(title),.edit_command={},.separator_before=separator,.children=std::move(children)};
                item.enabled=std::ranges::any_of(item.children,&ui::MenuItem::enabled);return item;
            };
            const auto header_menu=[&](ui::ControlKey key)->std::vector<ui::MenuItem> {
                using enum Command;
                if(key==control::viewport_view_menu) {
                    auto items=commands({focus,frame_all});
                    items.push_back(submenu(control::view_viewpoint_menu,L"Viewpoint",viewport_commands(control::viewport_camera),true));
                    auto options=commands({viewport_options,show_fps});options.front().separator_before=true;
                    for(auto& item:options) items.push_back(std::move(item));
                    return items;
                }
                if(key==control::viewport_select_menu) {
                    auto items=commands({select_all,select_none,select_invert,select_box});
                    items.front().separator_before=false;
                    auto modes=commands({select_mode_set,select_mode_extend,select_mode_subtract,select_mode_invert,select_mode_intersect});
                    modes.front().separator_before=false;
                    items.push_back(submenu(control::select_mode_menu,L"Selection Mode",std::move(modes),true));
                    return items;
                }
                if(key==control::viewport_header_add) {
                    std::vector<ui::MenuItem> items;
                    for(const auto& definition:command_definitions) if(definition.group==CommandGroup::create) items.push_back(command_item(definition.id));
                    return items;
                }
                if(key==control::viewport_object_menu) {
                    auto items=commands({duplicate_selected,erase_entities});items.front().separator_before=false;
                    auto clear=commands({clear_location,clear_rotation,clear_scale});clear.front().separator_before=false;
                    items.push_back(submenu(control::object_clear_menu,L"Clear",std::move(clear),true));
                    auto snap=commands({selection_grid,selection_cursor,cursor_selected,cursor_grid,cursor_origin});
                    for(auto& item:snap) item.separator_before=false;
                    items.push_back(submenu(control::object_snap_menu,L"Snap",std::move(snap)));
                    auto transform=commands({add_transform,remove_transform});transform.front().separator_before=true;
                    for(auto& item:transform) items.push_back(std::move(item));
                    return items;
                }
                return {};
            };
            std::vector<ui::MenuItem> items=dock.menu_items(source->key,frame);
            if(auto lighting_items=lighting.menu(source->context_target?source->context_target:source->key,this->view);!lighting_items.empty()) {
                items=std::move(lighting_items);
            } else if(auto header_items=header_menu(source->key);!header_items.empty()) {
                items=std::move(header_items);
            } else if(const auto* row=binding(source->key);row && row->kind==Binding::Kind::object && row->path.empty() &&
                      source->kind==ui::ControlKind::tree_item) {
                // Houdini tree / Blender outliner row menu over the shared command model.
                using enum Command;
                items=commands({rename_selected,duplicate_selected,erase_entities,unparent_selected,focus});
                for(auto& item:items) item.separator_before=false;
                items[3].separator_before=items[4].separator_before=true;
                items[0].icon=ui::ControlIcon::rename;items[1].icon=ui::ControlIcon::duplicate;items[2].icon=ui::ControlIcon::trash;
                items[3].icon=ui::ControlIcon::unparent;
            } else if (const auto* group=command_group(source->key)) {
                if(group->id==CommandGroup::view) {
                    items.push_back(command_item(Command::focus));
                    for(const auto& [key,title]:std::array<std::pair<ui::ControlKey,std::wstring_view>,4>{{
                        {control::viewport_tools_menu,L"Interaction Mode"},{control::viewport_pivot,L"Pivot"},
                        {control::viewport_snap,L"Snapping"},{control::viewport_camera,L"Camera View"}}})
                        items.push_back({.key=key,.text=std::wstring(title),.edit_command={},.children=viewport_commands(key)});
                    items.push_back(command_item(Command::viewport_options));
                } else {
                    if(group->id==CommandGroup::project) {
                        ui::MenuItem examples{.key=control::examples_menu,.text=L"Examples",.edit_command={}};
                        for(const auto& example:official_examples) examples.children.push_back(command_item(example.command));
                        items.push_back(std::move(examples));
                    }
                    for (const auto& definition:command_definitions) if (definition.group==group->id && !example_definition(definition.id)) {
                        auto item=command_item(definition.id);
                        if(group->id==CommandGroup::project && items.size()==1) item.separator_before=true;
                        items.push_back(std::move(item));
                    }
                }
            } else if (source->key==control::viewport && menu->group==control::viewport_shading_pie) {
                constexpr std::array titles{L"Wireframe",L"Solid",L"MatCap",L"Scene Lit"};
                const auto selected=this->view.wireframe==renderer::Wireframe::only?0u:
                    this->view.shading==renderer::Shading::matcap?2u:this->view.shading==renderer::Shading::scene_lit?3u:1u;
                for (std::size_t i=0;i<titles.size();++i) {
                    ui::MenuItem item{control::shading_preset_first+i,titles[i],true,i==selected,{}};
                    item.behavior=ui::MenuBehavior::radio;item.radio_group=control::shading_preset_first;
                    items.push_back(std::move(item));
                }
            } else if(source->key==control::viewport && menu->group==control::viewport_delete_menu) {
                auto item=command_item(Command::erase_entities);item.separator_before=false;
                item.text=session.selected_entities().size()==1?L"Delete Entity":L"Delete "+std::to_wstring(session.selected_entities().size())+L" Entities";
                items.push_back(std::move(item));
            } else if(source->key==control::viewport && menu->group==control::viewport_axis_pie) {
                using enum Command;
                items=commands({view_left,view_right,view_bottom,view_top,view_front,view_back,view_perspective,focus});
                items.back().text=L"Focus";
            } else if(source->key==control::viewport && menu->group==control::viewport_add_menu) {
                // Blender Shift+A: everything the Create group can place at the 3D cursor.
                for(const auto& definition:command_definitions) if(definition.group==CommandGroup::create)
                    items.push_back(command_item(definition.id));
            } else if(source->key==control::viewport || source->key==control::viewport_tools_menu) {
                using enum Command;
                items=viewport_commands(control::viewport_tools_menu);
                if(source->key==control::viewport) {
                    items.push_back({.key=control::viewport_snap,.text=L"Snap",.edit_command={},.children=viewport_commands(control::viewport_snap)});
                    for(auto item:commands({focus,create_static_mesh,duplicate_selected,erase_entities})) items.push_back(std::move(item));
                }
            } else if(source->key==control::viewport_snap) {
                // Blender snapping popover: captioned sections over the shared command model.
                items=viewport_commands(source->key);
                for(auto& item:items) {
                    const auto command=static_cast<Command>(item.key);
                    item.separator_before=false;
                    if(command==Command::snap_toggle) item.heading=L"Snapping";
                    else if(command==Command::snap_grid_01) item.heading=L"Increment";
                    else if(command==Command::snap_affect_move) item.heading=L"Affect";
                    else if(command==Command::snap_rotation_1) item.heading=L"Rotation Increment";
                    else if(command==Command::selection_grid) item.heading=L"Snap Selection / Cursor";
                    item.separator_before=!item.heading.empty() && command!=Command::snap_toggle;
                    // The section caption carries the context; rows show just the choice.
                    if(command>=Command::snap_affect_move && command<=Command::snap_affect_scale) item.text=item.text.substr(5);
                    else if(command==Command::snap_rotation_1 || command==Command::snap_rotation_5 || command==Command::snap_rotation_15 ||
                            command==Command::snap_rotation_45) item.text=item.text.substr(9);
                }
            } else if(source->key==control::viewport_camera || source->key==control::viewport_pivot || source->key==control::viewport_orientation) {
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
                        ui::MenuItem item{key,parameter_label(member.first),(session.is_material(target->object)?!session.world().running():entity_status.can_edit),member.second==enumeration->bits,{}};
                        item.behavior=ui::MenuBehavior::radio;item.radio_group=source->key;items.push_back(std::move(item));
                    }
                }
            } else if(const auto* target=binding(source->context_target);source->kind==ControlKind::combo && target &&
                (target->kind==Binding::Kind::material_slot || target->kind==Binding::Kind::material_texture)) {
                items=material_menu(*target,session,project);
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
            } else if(source->key==control::viewport_cavity) {
                const bool available=this->view.shading!=renderer::Shading::scene_lit && this->view.wireframe!=renderer::Wireframe::only;
                ui::MenuItem enabled{control::cavity_enabled,L"Screen Cavity",available,this->view.cavity.enabled,{}};
                enabled.behavior=ui::MenuBehavior::checkbox;items.push_back(std::move(enabled));
                for(std::size_t group=0;group<cavity_options.size();++group) {
                    const auto& option=cavity_options[group];
                    ui::MenuItem parent{control::cavity_group_first+group,
                        std::format(L"{}: {:g}{}",option.title,view_value(this->view,option.option),group==2?L" px":L""),
                        available && this->view.cavity.enabled,false,{},L"",group==0};
                    for(std::size_t index=0;index<cavity_values[group].size();++index) {
                        const auto value=cavity_values[group][index];
                        ui::MenuItem child{control::cavity_value_first+group*5+index,std::format(L"{:g}{}",value,group==2?L" px":L""),
                            true,view_value(this->view,option.option)==value,{}};
                        child.behavior=ui::MenuBehavior::radio;child.radio_group=parent.key;parent.children.push_back(std::move(child));
                    }
                    items.push_back(std::move(parent));
                }
            } else if (source->key==source_control::files) {
                items=source_editor.menu_items();
            } else if(source->key==control::add_component) {
                items.push_back(command_item(take(world::has_transform(runtime,selected))?Command::remove_transform:Command::add_transform));
                component_choices_=take(session.component_types());
                for(std::size_t i=0;i<component_choices_.size();++i)
                    items.push_back({control::component_choice_first+i,wide(component_choices_[i].name),selected!=ObjectId{},false,{}});
            } else if (source->key==control::scene_guides) {
                const bool studio=view.shading!=renderer::Shading::scene_lit;
                items.push_back({control::guide_background,L"Gradient Background",studio,studio && view.background==renderer::Background::gradient,{}});
                items.push_back({control::guide_grid,L"Reference Grid",true,view.reference_grid,{}});
                items.push_back({control::guide_axes,L"Origin Axes",true,view.origin_axes,{}});
                items.push_back({static_cast<ui::ControlKey>(Command::show_fps),L"Show FPS",true,show_fps,{},L"",true});
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
    if (pie_menu() && !menu_items.empty()) {
        const auto dpi=frame?frame->drawing->dpi_scale:1.f;
        auto entries=std::make_shared<std::vector<ui_language::Item>>();
        constexpr std::array<std::pair<double,double>,8> positions{{{60,104},{300,104},{180,181},{180,27},{72,55},{288,55},{72,153},{288,153}}};
        for (std::size_t i=0;i<menu_items.size();++i) {
            const auto& item=menu_items[i];
            entries->push_back({item.key,{item.text,item.enabled,item.selected,positions[i].first,positions[i].second,
                item.radio_group,std::wstring(item.behavior==ui::MenuBehavior::radio?L"radio":L"command")}});
        }
        const std::array<ui_language::InputValue,4> inputs{double(std::clamp(menu->x/dpi-180.f,0.f,std::max(0.f,size.width-360.f))),
            double(std::clamp(menu->y/dpi-104.f,0.f,std::max(0.f,size.height-208.f))),
            std::wstring(menu->group==control::viewport_axis_pie?L"View":L"Shading"),ui_language::ItemList{entries}};
        const std::array<ui_language::KeyBinding,1> keys{{{"menu",control::viewport_pie}}};
        static_cast<void>(ui_document_->append("ViewportPie",0,keys,inputs,result));
    }
    if (row_rename && row_rename->object!=ObjectId{} && !live.contains(row_rename->object)) row_rename.reset();
    if (row_rename && frame) {
        // Drawn over the accepted row geometry. A row not presented yet keeps the rename
        // pending (its host keeps it described); a table cell that left the frame ends it.
        const auto* table=row_rename->cell?frame->find_table(browser_control::results):nullptr;
        const auto cell=table?std::ranges::find(table->cells,row_rename->cell,&ui::TableCellGeometry::cell):std::vector<ui::TableCellGeometry>::const_iterator{};
        const auto row_key=row_rename->cell?ui::ControlKey{}:object_control(row_rename->object);
        const auto* row=row_key?frame->find(row_key):nullptr;
        const auto geometry=row?std::ranges::find(frame->tree_rows,row_key,&ui::TreeRowGeometry::key):frame->tree_rows.end();
        if (row_rename->cell && (!table || cell==table->cells.end())) row_rename.reset();
        else if (row_rename->cell) {
            // Name cell: the icon leads by one row height; the field starts where the name text does.
            const auto dpi=frame->drawing->dpi_scale;const auto& bounds=cell->bounds;
            const auto icon=bounds.height/dpi;
            const std::array<ui_language::InputValue,5> inputs{ui_language::TextEditBinding{control::inline_rename,row_rename->text},
                double(bounds.x/dpi+icon),double(bounds.y/dpi),double(std::max(48.f,bounds.width/dpi-icon-2)),double(bounds.height/dpi)};
            static_cast<void>(ui_document_->append("InlineRename",0,{},inputs,result));
        } else if (row && (row->bounds.height<=0 || geometry==frame->tree_rows.end())) row_rename.reset();
        else if (row) {
            const auto dpi=frame->drawing->dpi_scale;
            // Text origin as Session lays it out: depth indents plus the row icon's leading space.
            const auto* tree=frame->find(row->parent);
            const auto step=tree && tree->style?tree->style->tree_indent:17.f;
            const auto indent=row->style?row->style->padding.left+float(geometry->depth+1)*step+row->style->indicator_size+6-3:24.f;
            const auto padding=float(row->bounds.x)/dpi+indent;
            const std::array<ui_language::InputValue,5> inputs{ui_language::TextEditBinding{control::inline_rename,row_rename->text},
                double(padding),double(row->bounds.y/dpi),double(std::max(48.f,row->bounds.width/dpi-indent-2)),double(row->bounds.height/dpi)};
            static_cast<void>(ui_document_->append("InlineRename",0,{},inputs,result));
        } else if (!dock.visible(control::entities_tab) ||
            std::ranges::none_of(scene_tree_.rows,[&](const auto& shown){return scene_tree_.objects[shown.node]==row_rename->object;}))
            row_rename.reset();
    }
    if (color_edit && !pie_menu()) {
        // One popup per session; the colour editor is projected last, after every owner.
        const auto& edit=*color_edit;
        const auto hsv=rgb_to_hsv(edit.value);
        const auto field=[](ui::ControlKey key,const wchar_t* label,double value,double maximum,double step,ui::VectorAxis axis) {
            ui::NumberField result{key,label,value,value,0,maximum,step,step*.1,axis};
            result.slider.reset();return result;
        };
        ui_language::EditBinding rgb{control::color_group,true,false,{
            field(control::color_rgb,L"R",edit.value[0],edit.maximum,.005,ui::VectorAxis::x),field(control::color_rgb+1,L"G",edit.value[1],edit.maximum,.005,ui::VectorAxis::y),
            field(control::color_rgb+2,L"B",edit.value[2],edit.maximum,.005,ui::VectorAxis::z)}};
        ui_language::EditBinding hsv_edit{control::color_group+1,true,false,{
            field(control::color_hsv,L"H",hsv[0],360,1,ui::VectorAxis::none),field(control::color_hsv+1,L"S",hsv[1],1,.005,ui::VectorAxis::none),
            field(control::color_hsv+2,L"V",hsv[2],edit.maximum,.005,ui::VectorAxis::none)}};
        const auto clamp01=[](double v){return std::clamp(v,0.0,1.0);};
        const std::array<ui_language::InputValue,13> inputs{double(std::clamp(edit.x,0.f,std::max(0.f,size.width-420.f))),
            double(std::clamp(edit.y,0.f,std::max(0.f,size.height-200.f))),
            clamp01(edit.value[0]),clamp01(edit.value[1]),clamp01(edit.value[2]),
            clamp01(edit.original[0]),clamp01(edit.original[1]),clamp01(edit.original[2]),
            std::move(rgb),std::move(hsv_edit),ui_language::TextEditBinding{control::color_hex,edit.hex},
            ui_language::ActionBinding{control::color_revert,true},ui_language::ActionBinding{control::color_done,true}};
        const std::array<ui_language::KeyBinding,1> keys{{{"editor",control::color_editor}}};
        color_edit->composition_failed=!ui_document_->append("ColorEditor",0,keys,inputs,result);
    }
    if (ui_document_->candidate_failed()) {
        reject_ui_controls(std::string(ui_error()));
        return describe(session,size,frame,project,ui);
    }
    property_help_.end();
    return result;
}
std::array<double,3> rgb_to_hsv(std::array<double,3> c) noexcept {
    const auto high=std::max({c[0],c[1],c[2]}),low=std::min({c[0],c[1],c[2]}),span=high-low;
    double h=0;
    if(span>0) {
        if(high==c[0]) h=60*std::fmod((c[1]-c[2])/span,6.0);
        else if(high==c[1]) h=60*((c[2]-c[0])/span+2);
        else h=60*((c[0]-c[1])/span+4);
        if(h<0) h+=360;
    }
    return {h,high>0?span/high:0,high};
}
std::array<double,3> hsv_to_rgb(std::array<double,3> hsv) noexcept {
    const auto h=std::fmod(std::max(hsv[0],0.0),360.0)/60,s=std::clamp(hsv[1],0.0,1.0),v=std::max(hsv[2],0.0);
    const auto chroma=v*s,x=chroma*(1-std::abs(std::fmod(h,2.0)-1)),m=v-chroma;
    const int sector=int(h);
    const std::array<std::array<double,3>,6> table{{{chroma,x,0},{x,chroma,0},{0,chroma,x},{0,x,chroma},{x,0,chroma},{chroma,0,x}}};
    const auto& rgb=table[std::clamp(sector,0,5)];
    return {rgb[0]+m,rgb[1]+m,rgb[2]+m};
}
std::wstring color_hex(std::array<double,3> c) {
    const auto byte=[](double v){return int(std::lround(std::clamp(v,0.0,1.0)*255));};
    return std::format(L"{:02X}{:02X}{:02X}",byte(c[0]),byte(c[1]),byte(c[2]));
}
std::optional<std::array<double,3>> parse_color_hex(std::wstring_view text) {
    if(!text.empty() && text.front()==L'#') text.remove_prefix(1);
    if(text.size()!=6) return {};
    std::array<double,3> result{};
    for(std::size_t i=0;i<3;++i) {
        int value=0;
        for(std::size_t j=0;j<2;++j) {
            const auto c=text[i*2+j];const int digit=c>=L'0' && c<=L'9'?c-L'0':c>=L'a' && c<=L'f'?c-L'a'+10:c>=L'A' && c<=L'F'?c-L'A'+10:-1;
            if(digit<0) return {};value=value*16+digit;
        }
        result[i]=value/255.0;
    }
    return result;
}
}
