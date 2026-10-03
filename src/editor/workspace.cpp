#include "editor/workspace.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace atelier::editor {
Workspace::Workspace() : dock(1ULL<<48,{
    {control::scene_tab,control::scene_content,L"Scene View",{240,160}},
    {control::parameters_tab,50,L"Parameters",{220,260}},
    {control::workbench,300,L"Controls",{220,280}},
    {control::actors_tab,21,L"Actors",{180,140}}}) { reset_layout(); }
void Workspace::reset_layout() {
    dock.reset();
    dock.move(control::scene_tab,control::parameters_tab,ui::DockSide::left,.62f);
    dock.move(control::actors_tab,control::parameters_tab,ui::DockSide::bottom,.7f);
    dock.select(control::parameters_tab); menu.reset(); menu_bar_collapsed=false;
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
    if (key==control::tree_world) {
        auto result=session.select({});
        if (result) { tree_component_={}; tree_actor_={}; }
        return result;
    }
    const auto* target=binding(key);
    if (!target) return {};
    auto actor=target->object;
    const bool component=target->kind==Binding::Kind::tree_component;
    if (component) {
        auto state=world::inspect_component(session.world().runtime(),target->object);
        if (!state) return std::unexpected(state.error());
        actor=state->owner;
    }
    auto result=session.select(actor);
    if (result) {
        tree_actor_=actor; tree_component_=component?target->object:ObjectId{};
        if (component) parameter_page=control::parameter_components;
    }
    return result;
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
std::vector<ui::Control> Workspace::describe(const Session& session, ui_layout::Size size, const ui::UiFrame* frame) {
    const auto take = []<class T>(Result<T> value) -> T {
        if (!value) throw std::runtime_error(value.error().message);
        return std::move(*value);
    };
    using namespace ui_layout;
    using ui::ControlKind;
    auto& runtime = session.world().runtime();
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
    const auto selected = session.selected();
    if (selected!=tree_actor_ || !live.contains(tree_component_)) tree_component_={};
    tree_actor_=selected;
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
    const auto transform = selected == ObjectId{} ? std::optional<ObjectId>{} : take(world::root_transform(runtime, selected));
    ObjectId cube;
    if (selected != ObjectId{}) {
        const auto composition = take(world::components(runtime, selected));
        for (const auto& component : composition) {
            const auto state = take(world::inspect_component(runtime, component.id()));
            if (state.valid && take(runtime.is_a(state.type, session.types().cube))) { cube = component.id(); break; }
        }
    }
    auto root = NodeInput::column(); root.width = root.height = Axis::expand();
    add(1, 0, ControlKind::container, root);
    const float menu_height=menu_bar_collapsed?8.f:32.f;
    auto bar=NodeInput::row(); bar.height=Axis::fixed(menu_height);
    add(2,1,ControlKind::container,bar).classes={"menubar"};
    auto handle=NodeInput::box(12,menu_height); handle.padding.bottom=menu_bar_collapsed?0.f:18.f;
    auto& stow=add(control::menu_bar_handle,2,ControlKind::button,handle);
    stow.icon=menu_bar_collapsed?ui::ControlIcon::down:ui::ControlIcon::up;
    stow.classes={"bar-stow"};
    auto menu_body=NodeInput::row(); menu_body.height=Axis::expand(); menu_body.width=Axis::expand();
    menu_body.padding={0,1,0,1}; menu_body.collapsed=menu_bar_collapsed;
    add(control::menu_bar_body,2,ControlKind::container,menu_body);
    constexpr std::array titles{L"File",L"Edit",L"Create",L"View",L"Window"};
    for (std::size_t index=0;index<titles.size();++index) {
        auto shape=NodeInput::box(0,0); shape.height=Axis::expand(); shape.width=Axis::content(38);
        const auto key=control::file_menu+index;
        auto& title=add(key,control::menu_bar_body,ControlKind::button,shape,titles[index]);
        title.classes={"menubar-item"}; title.context_target=key;
        title.selected=menu && menu->source==key;
    }
    auto body = NodeInput::column(); body.height = Axis::expand();
    add(20, 1, ControlKind::container, body);
    dock.append(result,20,{size.width,std::max(0.f,size.height-menu_height-26)},[&](const ui::DockTab& view,std::vector<ui::Control>&,ui::ControlKey parent) {
    auto heading=NodeInput::box(0,26); heading.width=Axis::expand();
    if (view.key==control::scene_tab) {
        auto contents=NodeInput::column(); contents.width=contents.height=Axis::expand();
        add(control::scene_content,parent,ControlKind::container,contents);
        ui::append_path_bar(result,{641,control::scene_content,control::scene_path,control::scene_back,control::scene_forward,L"World",can_navigate(-1),can_navigate(1)});
        auto viewport=NodeInput::box(0,0); viewport.width=viewport.height=Axis::expand();
        add(control::viewport,control::scene_content,ControlKind::viewport,viewport);
        return;
    }
    auto panel=NodeInput::column(); panel.width=panel.height=Axis::expand();
    auto& pane_content=add(view.content,parent,ControlKind::container,panel);
    pane_content.role=ui::Role::panel;
    if (view.key==control::parameters_tab) pane_content.classes={"parameter-pane"};
    if (view.key==control::workbench) { workbench.append(result,view.content); return; }
    if (view.key==control::actors_tab) {
        ui::append_path_bar(result,{24,21,control::actor_path,control::actor_back,control::actor_forward,L"World",can_navigate(-1),can_navigate(1)});
        auto list=NodeInput::column(); list.height=Axis::expand();
        add(control::actor_tree,21,ControlKind::tree,list).tree_filter=tree_query;
        auto row=NodeInput::box(0,20); row.width=Axis::expand();
        auto& world_row=add(control::tree_world,control::actor_tree,ControlKind::tree_item,row,L"World");
        world_row.icon=ui::ControlIcon::world; world_row.selected=selected==ObjectId{};
        if (const auto state=tree_expansion_.find(control::tree_world);state!=tree_expansion_.end()) world_row.tree_initially_expanded=state->second;
        for (const auto& member:members) {
            const auto state=take(world::inspect_actor(runtime,member.id()));
            const auto actor_key=bind(member.id());
            auto& item=add(actor_key,control::actor_tree,ControlKind::tree_item,row,std::wstring(session.name(member.id())));
            item.tree_parent=control::tree_world; item.tree_initially_expanded=false;
            if (const auto expansion=tree_expansion_.find(actor_key);expansion!=tree_expansion_.end()) item.tree_initially_expanded=expansion->second;
            item.icon=ui::ControlIcon::object; item.classes={"actor"};
            item.enabled=state.state==world::ActorState::attached_inactive || state.state==world::ActorState::active;
            item.selected=member.id()==selected && tree_component_==ObjectId{};
            for (const auto& component:take(world::components(runtime,member.id()))) {
                const auto info=take(world::inspect_component(runtime,component.id()));
                const auto type=take(runtime.describe(info.type));
                auto name=type->name;
                if (const auto dot=name.find_last_of('.');dot!=name.npos) name=name.substr(dot+1);
                auto& child=add(bind(component.id(),{},Binding::Kind::tree_component),control::actor_tree,ControlKind::tree_item,row,std::wstring(name.begin(),name.end()));
                child.tree_parent=actor_key; child.selected=component.id()==tree_component_;
                const bool is_transform=take(runtime.is_a(info.type,session.types().transform));
                child.icon=is_transform?ui::ControlIcon::axes:ui::ControlIcon::component;
                child.classes={is_transform?"transform":"component"};
            }
        }
        auto filter=NodeInput::row(6); filter.height=Axis::content(ui::single_line_height); filter.padding={4,2,3,2};
        add(29,21,ControlKind::container,filter);
        auto caption=ui::single_line_layout(36);
        add(30,29,ControlKind::label,caption,L"Filter").classes={"field-label"};
        auto field=ui::single_line_layout(); field.width=Axis::expand();
        add(control::tree_filter,29,ControlKind::text_edit,field,tree_query);
        return;
    }
    ui::append_path_bar(result,{75,50,control::parameter_path,control::parameter_back,control::parameter_forward,L"World",can_navigate(-1),can_navigate(1)});
    ui::append_object_header(result,51,50,52,selected==ObjectId{}?ui::ControlKey{77}:bind(selected,{},Binding::Kind::name),
        L"Actor",selected==ObjectId{}?L"No selection":std::wstring(session.name(selected)),actor_status.can_edit);
    const std::array tabs{ui::ParameterTab{control::parameter_transform,L"Transform"},ui::ParameterTab{control::parameter_components,L"Components"}};
    auto page_stack=NodeInput::column(); page_stack.height=Axis::expand();
    add(93,50,ControlKind::container,page_stack).classes={"parameter-page"};
    ui::append_parameter_tabs(result,control::parameter_pages,93,parameter_page,tabs);
    auto scroll=NodeInput::column(7); scroll.height=Axis::expand(); scroll.padding={0,7.333f,0,8};
    add(control::parameter_list,93,ControlKind::list,scroll).classes={"parameter-list"};
    auto component_row = NodeInput::row(6); component_row.height = Axis::fixed(26);
    component_row.collapsed=parameter_page!=control::parameter_components;
    add(56, control::parameter_list, ControlKind::container, component_row).classes={"parameter-inset"};
    auto component_button = NodeInput::box(0, 26); component_button.width = Axis::content();
    add(control::add_transform, 56, ControlKind::button, component_button, L"+ Transform").enabled = actor_status.can_edit && !transform;
    component_button.width = Axis::expand();
    if (cube != ObjectId{}) add(bind(cube), 56, ControlKind::button, component_button, L"− Cube").enabled = actor_status.can_edit;
    else add(control::add_cube, 56, ControlKind::button, component_button, L"+ Cube").enabled = actor_status.can_edit && transform.has_value();
    auto page=NodeInput::column(7); page.collapsed=parameter_page!=control::parameter_transform;
    add(85,control::parameter_list,ControlKind::container,page);
    auto search_row=NodeInput::row(8); search_row.flow=FlexSpec{}; search_row.flow->align_items=Alignment::fit(.5f);
    add(86,85,ControlKind::container,search_row).classes={"parameter-inset"};
    auto search_label = ui::single_line_layout(48);
    auto& filter_caption=add(55,86,ControlKind::label,search_label,L"Filter");
    filter_caption.role=ui::Role::caption; filter_caption.classes={"field-label"};
    auto search_layout = ui::single_line_layout(); search_layout.width = Axis::expand();
    add(control::parameter_search,86,ControlKind::text_edit,search_layout,query);
    auto folded = query;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    });
    constexpr std::array aliases{L"position 位置 x", L"position 位置 y", L"position 位置 z"};
    std::array<bool, 3> matches{};
    for (std::size_t axis = 0; axis < matches.size(); ++axis)
        matches[axis] = std::wstring_view(aliases[axis]).find(folded) != std::wstring_view::npos;
    const bool any = std::any_of(matches.begin(), matches.end(), [](bool match) { return match; });
    if (!any || !transform) add(53,85,ControlKind::label,heading,any ? L"No Transform" : L"No matching parameters");
    auto section=NodeInput::box(0,28); section.width=Axis::expand();
    auto& fold=add(control::position_fold,85,ControlKind::foldout,section,L"Transform");
    fold.selected=position_expanded; fold.classes={"section"}; fold.layout.collapsed=!any;
    auto contents=NodeInput::column(7); contents.collapsed=!position_expanded || !any;
    add(58,85,ControlKind::container,contents).classes={"parameter-inset"};
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
    if (field_count) ui::append_number_row(result,{70,72,73,transform?bind(*transform,{},Binding::Kind::position_default):ui::ControlKey{74},58,
        L"Position (m)",actor_status.can_edit && transform.has_value(),group_modified},std::span(fields).first(field_count));
    auto hint=NodeInput::box(0,0); hint.width=Axis::expand(); hint.height=Axis::content();
    hint.padding.left=8;
    add(90,85,ControlKind::label,hint,L"Middle drag: value ladder · Shift: fine steps").role = ui::Role::caption;
    add(91,85,ControlKind::label,hint,L"Right click: parameter menu · Ctrl + middle: defaults").role = ui::Role::caption;
    auto info=hint; info.collapsed=parameter_page!=control::parameter_components;
    add(92,control::parameter_list,ControlKind::label,info,L"Components belong to this Actor. Adding or removing one participates in scene history.").role=ui::Role::caption;
    });
    auto footer = NodeInput::row(); footer.height = Axis::fixed(26); footer.padding = {8, 2, 8, 0};
    add(100, 1, ControlKind::container, footer).role = ui::Role::panel;
    auto status_box = NodeInput::box(0, 26); status_box.width = Axis::expand();
    add(101, 100, ControlKind::label, status_box, status + L"  |  " + std::to_wstring(members.size()) + L" Actors").role = ui::Role::caption;
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
            if (control::is_menu(source->key)) {
                const auto command=[&](ui::ControlKey key,const wchar_t* text,bool enabled=true,const wchar_t* shortcut=L"",bool separator=false) {
                    items.push_back({key,text,enabled,false,{},shortcut,separator});
                };
                switch (source->key) {
                case control::file_menu: command(control::exit,L"Exit",true,L"Alt+F4"); break;
                case control::edit_menu:
                    command(control::undo,L"Undo",(sample_history()?workbench.undo_count():session.undo_count())!=0,L"Ctrl+Z");
                    command(control::redo,L"Redo",(sample_history()?workbench.redo_count():session.redo_count())!=0,L"Ctrl+Shift+Z");
                    command(control::remove_actor,L"Delete Actor",actor_status.can_edit,L"Delete",true); break;
                case control::create_menu:
                    command(control::add_actor,L"Add Actor",members.size()<Session::maximum_actors);
                    command(control::add_transform,L"Add Transform",actor_status.can_edit && !transform,L"",true);
                    command(control::add_cube,L"Add Cube",actor_status.can_edit && transform.has_value() && cube==ObjectId{}); break;
                case control::view_menu:
                    command(control::focus,L"Focus Selection",transform.has_value() &&
                        (actor_status.state==world::ActorState::attached_inactive || actor_status.state==world::ActorState::active),L"F");
                    command(control::reset,L"Reset Camera"); break;
                case control::window_menu:
                    command(control::show_scene,L"Scene View"); command(control::show_parameters,L"Parameters");
                    command(control::show_controls,L"Controls"); command(control::show_actors,L"Actors");
                    command(control::maximize_pane,L"Maximize / Restore Pane",true,L"Ctrl+B",true);
                    command(control::reset_layout,L"Reset Layout"); break;
                }
            } else if (control::is_path(source->key)) {
                items.push_back({control::world_context,L"World",true,selected==ObjectId{},{}});
                for (const auto& member:members) items.push_back({bind(member.id(),{},Binding::Kind::navigation),
                    std::wstring(session.name(member.id())),true,selected==member.id(),{},L"",items.size()==1});
            } else if (!items.empty()) {}
            else if (source->kind==ControlKind::combo && source->key==workbench_control::mode) {
                for (std::size_t i=0;i<ControlWorkbench::mode_names.size();++i)
                    items.push_back({workbench_control::mode_first+i,ControlWorkbench::mode_names[i],true,workbench.values().mode==i,{}});
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
    return result;
}
}
