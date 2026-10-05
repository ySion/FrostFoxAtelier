#include "editor/workspace.hpp"
#include "editor/assets.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <cwctype>

namespace atelier::editor {
Workspace::Workspace() : dock(1ULL<<48,{
    {control::scene_tab,control::scene_content,L"Scene View",{240,160}},
    {control::parameters_tab,50,L"Parameters",{220,260}},
    {control::workbench,300,L"Controls",{220,280}},
    {control::actors_tab,21,L"Scene Tree",{180,140}},
    {control::assets_tab,control::assets_content,L"Asset Browser",{240,140}}}) { reset_layout(); }
void Workspace::reset_layout() {
    dock.reset();
    dock.move(control::scene_tab,control::parameters_tab,ui::DockSide::left,.62f);
    dock.move(control::actors_tab,control::parameters_tab,ui::DockSide::bottom,.7f);
    dock.move(control::assets_tab,control::scene_tab,ui::DockSide::bottom,.73f);
    dock.select(control::parameters_tab); menu.reset(); menu_bar_collapsed=shelf_collapsed=false;
}
void Workspace::reset_document() {
    ui_document_.reset();
    bindings_.clear(); binding_cursor_=0; tree_expansion_.clear(); tree_frame_=0;
    navigation_={ObjectId{}}; navigation_cursor_=0; query.clear(); tree_query.clear();
    menu.reset(); assets.reset(project_open);
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
        return item.kind==(path.empty()?Binding::Kind::object:Binding::Kind::field) && item.object == object && std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
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
    if (changed) navigation_cursor_=next;
    return changed;
}
Result<void> Workspace::select_tree(Session& session,ui::ControlKey key) {
    if (key==control::tree_world) return session.select({});
    const auto* target=binding(key);
    return target && target->kind==Binding::Kind::object?session.select(target->object):Result<void>{};
}
bool Workspace::command_enabled(Command command,const Session& session) const {
    switch (command) {
    case Command::save_scene: case Command::save_scene_as: case Command::open_scene:
    case Command::import_mesh: case Command::refresh_assets: return project_open;
    case Command::undo: return (sample_history()?workbench.undo_count():session.undo_count())!=0;
    case Command::redo: return (sample_history()?workbench.redo_count():session.redo_count())!=0;
    case Command::create_actor: case Command::create_static_mesh: {
        const auto members=session.world().members(); return members && members->size()<Session::maximum_actors;
    }
    case Command::erase_actor: case Command::focus: {
        if (session.selected()==ObjectId{}) return false;
        const auto state=world::inspect_actor(session.world().runtime(),session.selected());
        if (!state || !state->can_edit) return false;
        if (command==Command::erase_actor) return true;
        const auto root=world::root_component(session.world().runtime(),session.selected());
        return root && root->has_value();
    }
    case Command::previous_tab: case Command::next_tab: case Command::close_tab: case Command::maximize_pane:
        return dock.focused_tab()!=0;
    default: return true;
    }
}
ui::ControlKey Workspace::bind(ObjectId object, std::span<const FieldId> path, Binding::Kind kind) {
    if (!path.empty()) kind=Binding::Kind::field;
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
    if (!ui_document_) ui_document_=std::make_unique<ui_language::LiveDocument>(runtime,ui_source(),next_binding_);
    ui_document_->poll();
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
    const auto add = [&](ui::ControlKey key, ui::ControlKey parent, ControlKind kind,
                         NodeInput layout, std::wstring text = {}) -> ui::Control& {
        ui::Control item;
        item.key = key; item.parent = parent; item.kind = kind; item.layout = std::move(layout); item.text = std::move(text);
        result.push_back(std::move(item));
        return result.back();
    };
    const auto requested_selection=session.selected();
    const auto selected=std::ranges::any_of(members,[&](const auto& member){return member.id()==requested_selection;})?
        requested_selection:ObjectId{};
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
    if (navigation_[navigation_cursor_]!=selected) {
        navigation_.resize(navigation_cursor_+1); navigation_.push_back(selected);
        if (navigation_.size()>64) navigation_.erase(navigation_.begin()+1); // Keep World reachable.
        navigation_cursor_=navigation_.size()-1;
    }
    const auto actor_status = selected == ObjectId{} ? world::ActorStatus{} : take(world::inspect_actor(runtime, selected));
    const auto transform = selected == ObjectId{} ? std::optional<ObjectId>{} : take(world::root_component(runtime, selected));
    auto root = NodeInput::column(); root.width = root.height = Axis::expand();
    add(1, 0, ControlKind::container, root);
    auto menu_body=NodeInput::row(); menu_body.padding={0,1,0,1};
    const auto menu_height=ui::append_collapsible_bar(result,
        {2,1,control::menu_bar_handle,control::menu_bar_body,32,menu_bar_collapsed,menu_body,"menubar"});
    for (const auto& group:command_groups) {
        auto shape=NodeInput::box(0,0); shape.height=Axis::expand(); shape.width=Axis::content(38);
        const auto key=group.key;
        auto& title=add(key,control::menu_bar_body,ControlKind::button,shape,std::wstring(group.title));
        title.classes={"menubar-item"}; title.context_target=key;
        title.selected=menu && menu->source==key;
    }
    auto create_tools=std::make_shared<std::vector<ui_language::Item>>();
    auto view_tools=std::make_shared<std::vector<ui_language::Item>>();
    for (std::size_t index=0;index<command_definitions.size();++index) {
        const auto& definition=command_definitions[index];
        if (definition.group!=CommandGroup::create && definition.group!=CommandGroup::view) continue;
        if (!shelf_items_[index]) shelf_items_[index]=next_binding_++;
        const auto title=definition.shelf_title.empty()?definition.title:definition.shelf_title;
        const auto icon=definition.id==Command::create_static_mesh?ui::ControlIcon::mesh:
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
    static_cast<void>(ui_document_->append("ToolShelf",1,shelf_keys,shelf_inputs,result));
    // DockSpace still consumes an explicit extent. Read the authored root's
    // height; the C++ shell does not maintain a second shelf size formula.
    float shelf_height{};
    if (const auto shelf=std::ranges::find(result,control::shelf,&ui::Control::key);shelf!=result.end()) {
        if (shelf->layout.height.rule!=ui_layout::SizeRule::fixed) {
            if (reject_ui_controls("ToolShelf root requires a fixed height until the shell supplies layout slots."))
                return describe(session,size,frame,project);
        } else shelf_height=shelf->layout.height.value;
    }
    auto body = NodeInput::column(); body.height = Axis::expand();
    add(20, 1, ControlKind::container, body);
    constexpr float footer_height=ui::single_line_height;
    dock.append(result,20,{size.width,std::max(0.f,size.height-menu_height-shelf_height-footer_height)},[&](const ui::DockTab& view,std::vector<ui::Control>&,ui::ControlKey parent) {
    if (view.key==control::scene_tab) {
        auto contents=NodeInput::column(); contents.width=contents.height=Axis::expand();
        add(control::scene_content,parent,ControlKind::container,contents);
        ui::append_path_bar(result,{641,control::scene_content,control::scene_path,control::scene_back,control::scene_forward,L"World",can_navigate(-1),can_navigate(1)});
        const std::array<ui_language::InputValue,4> inputs{static_cast<double>(ui::ControlIcon::mesh),
            static_cast<double>(ui::ControlIcon::axes),menu && menu->source==control::scene_shading,
            menu && menu->source==control::scene_guides};
        const std::array<ui_language::KeyBinding,2> keys{{{"display",control::scene_shading},{"guides",control::scene_guides}}};
        static_cast<void>(ui_document_->append("ViewportTools",641,keys,inputs,result));
        auto viewport=NodeInput::box(0,0); viewport.width=viewport.height=Axis::expand();
        add(control::viewport,control::scene_content,ControlKind::viewport,viewport);
        return;
    }
    auto panel=NodeInput::column(); panel.width=panel.height=Axis::expand();
    auto& content_slot=add(view.content,parent,ControlKind::container,panel);
    if (view.key!=control::parameters_tab) content_slot.role=ui::Role::panel;
    if (view.key==control::workbench) { workbench.append(result,view.content); return; }
    if (view.key==control::assets_tab) {
        assets.append(result,view.content,project,command_enabled(Command::create_static_mesh,session),frame,*ui_document_);
        return;
    }
    if (view.key==control::actors_tab) {
        ui::append_path_bar(result,{24,21,control::actor_path,control::actor_back,control::actor_forward,L"World",can_navigate(-1),can_navigate(1)});
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
                member.id()==selected,static_cast<double>(mesh_actor?ui::ControlIcon::mesh:ui::ControlIcon::object),
                control::tree_world,expansion!=tree_expansion_.end() && expansion->second,std::wstring(mesh_actor?L"mesh-actor":L"actor")}});
        }
        const auto expansion=tree_expansion_.find(control::tree_world);
        const std::array<ui_language::InputValue,5> inputs{ui_language::ItemList{std::move(rows)},
            ui_language::TextEditBinding{control::tree_filter,tree_query},selected==ObjectId{},
            expansion==tree_expansion_.end() || expansion->second,static_cast<double>(ui::ControlIcon::world)};
        const std::array<ui_language::KeyBinding,2> keys{{{"tree",control::actor_tree},{"world",control::tree_world}}};
        static_cast<void>(ui_document_->append("SceneTree",21,keys,inputs,result));
        return;
    }
    const auto type_name=selected==ObjectId{}?L"Actor":([&] { auto name=take(runtime.describe(actor_status.type))->name;
            if (const auto dot=name.find_last_of('.');dot!=name.npos) name=name.substr(dot+1);
            return std::wstring(name.begin(),name.end()); })();
    const auto lowercase=[](std::wstring value) {
        for (auto& c:value) c=static_cast<wchar_t>(std::towlower(c));
        return value;
    };
    const auto folded=lowercase(query);
    auto components=std::make_shared<std::vector<ui_language::Item>>();
    bool has_components{};
    if (selected!=ObjectId{}) for (const auto& component:take(world::components(runtime,selected))) {
        const auto state=take(world::inspect_component(runtime,component.id()));
        auto type=take(runtime.describe(state.type))->name;
        if (const auto dot=type.find_last_of('.');dot!=type.npos) type=type.substr(dot+1);
        has_components=true;
        const auto title=std::wstring(type.begin(),type.end());
        if (lowercase(title+L" components 组件").find(folded)!=std::wstring::npos)
            components->push_back({bind(component.id()),{title,static_cast<double>(ui::ControlIcon::component)}});
    }
    const bool components_empty=components->empty();
    const bool transform_selected=parameter_page==control::parameter_transform && transform.has_value();
    auto pages=std::make_shared<std::vector<ui_language::Item>>();
    const auto page=[&](ui::ControlKey key,std::wstring title,bool active) {
        pages->push_back({key,{std::move(title),active,ui_language::ActionBinding{key},control::parameter_pages,
            std::wstring(pages->empty()?L"parameter-tab":L"parameter-tab joined")}});
    };
    if (selected!=ObjectId{}) {
        if (transform) page(control::parameter_transform,L"Transform",transform_selected);
        page(control::parameter_components,L"Components",!transform_selected);
    }
    constexpr std::array aliases{L"transform scene root position 变换 位置 x", L"transform scene root position 变换 位置 y", L"transform scene root position 变换 位置 z"};
    std::array<bool, 3> matches{};
    for (std::size_t axis = 0; axis < matches.size(); ++axis)
        matches[axis] = std::wstring_view(aliases[axis]).find(folded) != std::wstring_view::npos;
    const bool any = std::any_of(matches.begin(), matches.end(), [](bool match) { return match; });
    constexpr std::array names{L"X", L"Y", L"Z"};
    const auto definitions = session.parameters();
    std::array<ui::NumberField,3> fields;
    std::size_t field_count{};
    bool group_modified{};
    for (std::size_t axis = 0; axis < definitions.size(); ++axis) {
        const auto& definition = definitions[axis];
        const auto field = transform ? bind(*transform, definition.path) : control::empty_field_base + axis;
        const double value=transform ? std::get<float>(take(session.world().read(*transform, definition.path))) : definition.default_value;
        group_modified|=value!=definition.default_value;
        if (!matches[axis]) continue;
        fields[field_count++]={field,names[axis],value,definition.default_value,definition.minimum,definition.maximum,definition.drag_step,.001,static_cast<ui::VectorAxis>(axis+1)};
    }
    ui_language::EditBinding position{transform?bind(*transform,{},Binding::Kind::position_default):ui::ControlKey{74},
        actor_status.can_edit && transform.has_value(),group_modified,{fields.begin(),fields.begin()+static_cast<std::ptrdiff_t>(field_count)}};
    const std::array<ui_language::InputValue,16> inputs{std::wstring(type_name),
        ui_language::TextEditBinding{selected==ObjectId{}?ui::ControlKey{77}:bind(selected,{},Binding::Kind::name),
            selected==ObjectId{}?L"No selection":std::wstring(session.name(selected)),actor_status.can_edit},
        ui_language::ActionBinding{control::parameter_back,can_navigate(-1)},ui_language::ActionBinding{control::parameter_forward,can_navigate(1)},
        static_cast<double>(ui::ControlIcon::back),static_cast<double>(ui::ControlIcon::forward),static_cast<double>(ui::ControlIcon::world),
        selected!=ObjectId{},transform_selected,ui_language::ItemList{std::move(pages)},ui_language::ItemList{std::move(components)},
        any,std::move(position),ui_language::TextEditBinding{control::parameter_search,query},components_empty,
        std::wstring(has_components?L"No matching components":L"No components")};
    const std::array<ui_language::KeyBinding,14> keys{{{"path.bar",75},
        {"path.path",control::parameter_path},{"path.back",control::parameter_back},{"path.forward",control::parameter_forward},
        {"header.row",51},{"header.type",52},{"pages",93},{"tabs",control::parameter_pages},
        {"scroll",control::parameter_list},{"components",56},{"transform",85},
        {"position.section",control::position_fold},{"position.contents",58},{"position.position",70,72,73}}};
    static_cast<void>(ui_document_->append("Parameters",view.content,keys,inputs,result));
    });
    auto footer = NodeInput::row(); footer.height = Axis::fixed(footer_height); footer.padding = {8, 0, 8, 0};
    add(100, 1, ControlKind::container, footer).classes={"status-bar"};
    auto status_box = ui::single_line_layout(); status_box.width = Axis::expand();
    auto status_text=status;
    if (!ui_error().empty()) {
        auto diagnostic=wide(std::string(ui_error()));
        if (diagnostic.size()>512) {diagnostic.resize(512);if (diagnostic.back()>=0xd800 && diagnostic.back()<=0xdbff) diagnostic.pop_back();}
        status_text=L"UI: "+diagnostic;
    }
    add(101, 100, ControlKind::label, status_box, status_text+L"  |  "+std::to_wstring(members.size())+L" Actors").role = ui::Role::caption;
    menu_items.clear();
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
            std::vector<ui::MenuItem> items=dock.menu_items(source->key,frame);
            if (const auto* group=command_group(source->key)) {
                for (const auto& definition:command_definitions) if (definition.group==group->id)
                    items.push_back({static_cast<ui::ControlKey>(definition.id),std::wstring(definition.title),
                        command_enabled(definition.id,session),false,{},std::wstring(definition.shortcut),definition.separator});
            } else if (control::is_path(source->key)) {
                items.push_back({control::world_context,L"World",true,selected==ObjectId{},{}});
                for (const auto& member:members) items.push_back({bind(member.id(),{},Binding::Kind::navigation),
                    std::wstring(session.name(member.id())),true,selected==member.id(),{},L"",items.size()==1});
                for (auto& item:items) {item.behavior=ui::MenuBehavior::radio;item.radio_group=source->key;}
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
                group(control::edges_first,edge_titles,edge_option);
                group(control::topology_first,topology_titles,this->view.triangle_edges?1u:0u);
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
