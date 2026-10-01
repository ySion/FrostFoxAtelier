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
    dock.select(control::parameters_tab); menu.reset();
}
const Workspace::Binding* Workspace::binding(ui::ControlKey key) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [key](const auto& item) { return item.key == key; });
    return found == bindings_.end() ? nullptr : &*found;
}
ui::ControlKey Workspace::object_control(ObjectId object) const noexcept { return field_control(object, {}); }
ui::ControlKey Workspace::field_control(ObjectId object, std::span<const FieldId> path) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
        return !item.reset_position && item.object == object && std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
    });
    return found == bindings_.end() ? 0 : found->key;
}
ui::ControlKey Workspace::reset_control(ObjectId object) const noexcept {
    const auto found=std::find_if(bindings_.begin(),bindings_.end(),[&](const auto& item) { return item.reset_position && item.object==object; });
    return found==bindings_.end()?0:found->key;
}
ui::ControlKey Workspace::bind(ObjectId object, std::span<const FieldId> path, bool reset_position) {
    const auto first=bindings_.begin()+static_cast<std::ptrdiff_t>(binding_cursor_);
    const auto found=std::find_if(first,bindings_.end(),[&](const auto& item) {
        return item.reset_position==reset_position && item.object==object && std::equal(item.path.begin(),item.path.end(),path.begin(),path.end());
    });
    if (found!=bindings_.end()) { std::iter_swap(first,found); return bindings_[binding_cursor_++].key; }
    if (next_binding_==std::numeric_limits<ui::ControlKey>::max()) throw std::runtime_error("Editor control identities exhausted.");
    bindings_.push_back({next_binding_++,object,{path.begin(),path.end()},reset_position});
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
    // Closing/hiding a view is not an object lifetime change. Removed components
    // still retire their bindings, including when undo later restores their identity.
    std::erase_if(bindings_,[&](const auto& item){return !live.contains(item.object);});
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
    auto toolbar = NodeInput::row(6); toolbar.height = Axis::fixed(36); toolbar.padding = {8, 5, 8, 5};
    auto& toolbar_panel=add(2,1,ControlKind::container,toolbar); toolbar_panel.role=ui::Role::panel; toolbar_panel.classes={"toolbar"};
    add(3, 2, ControlKind::label, NodeInput::box(150, 26), L"FrostFox Atelier").role = ui::Role::title;
    const auto command = [&](ui::ControlKey key, float minimum_width, const wchar_t* text) -> ui::Control& {
        auto layout = NodeInput::box(0, 26); layout.width = Axis::content(minimum_width);
        auto& button=add(key, 2, ControlKind::button, layout, text);
        if (key==control::remove_actor) button.classes.push_back("danger");
        return button;
    };
    command(control::add_actor, 76, L"+ Add Actor").enabled = members.size() < Session::maximum_actors;
    command(control::remove_actor, 58, L"Delete").enabled = actor_status.can_edit;
    command(control::undo, 48, L"Undo").enabled = (sample_history() ? workbench.undo_count() : session.undo_count()) != 0;
    command(control::redo, 48, L"Redo").enabled = (sample_history() ? workbench.redo_count() : session.redo_count()) != 0;
    command(control::focus, 48, L"Focus").enabled = transform.has_value() &&
        (actor_status.state == world::ActorState::attached_inactive || actor_status.state == world::ActorState::active);
    command(control::reset, 92, L"Reset camera");
    command(control::reset_layout, 94, L"Reset layout");
    auto body = NodeInput::column(); body.height = Axis::expand();
    add(20, 1, ControlKind::container, body);
    dock.append(result,20,{size.width,std::max(0.f,size.height-62)},[&](const ui::DockTab& view,std::vector<ui::Control>&,ui::ControlKey parent) {
    auto heading=NodeInput::box(0,26); heading.width=Axis::expand();
    if (view.key==control::scene_tab) {
        auto contents=NodeInput::column(); contents.width=contents.height=Axis::expand();
        add(control::scene_content,parent,ControlKind::container,contents);
        auto navigation=NodeInput::row(); navigation.height=Axis::fixed(30); navigation.padding={10,3,10,3};
        auto& bar=add(641,control::scene_content,ControlKind::container,navigation); bar.classes={"pane-path"};
        auto path=NodeInput::box(0,24); path.width=Axis::expand();
        add(642,641,ControlKind::label,path,L"/ World");
        auto viewport=NodeInput::box(0,0); viewport.width=viewport.height=Axis::expand();
        add(control::viewport,control::scene_content,ControlKind::viewport,viewport);
        return;
    }
    auto panel=NodeInput::column(7); panel.width=panel.height=Axis::expand(); panel.padding={12,8,12,8};
    auto& pane_content=add(view.content,parent,view.key==control::parameters_tab?ControlKind::list:ControlKind::container,panel);
    pane_content.role=ui::Role::panel;
    if (view.key==control::parameters_tab) pane_content.classes={"parameter-pane"};
    if (view.key==control::workbench) { workbench.append(result,view.content); return; }
    if (view.key==control::actors_tab) {
        auto path=heading; add(22,21,ControlKind::label,path,L"/ World").classes={"pane-path"};
        auto list=NodeInput::column(1); list.height=Axis::expand();
        add(23,21,ControlKind::list,list);
        std::size_t ordinal{};
        for (const auto& member:members) {
            const auto state=take(world::inspect_actor(runtime,member.id()));
            auto row=NodeInput::box(0,28); row.width=Axis::expand();
            auto& item=add(bind(member.id()),23,ControlKind::item,row,L"Actor "+std::to_wstring(++ordinal));
            item.enabled=state.state==world::ActorState::attached_inactive || state.state==world::ActorState::active;
            item.selected=member.id()==selected;
        }
        return;
    }
    std::wstring selected_name = L"No selection";
    for (std::size_t i=0;i<members.size();++i) if (members[i].id()==selected) selected_name=L"Actor "+std::to_wstring(i+1);
    add(52, 50, ControlKind::label, heading, std::move(selected_name)).role = ui::Role::title;
    auto component_row = NodeInput::row(6); component_row.height = Axis::fixed(26);
    add(56, 50, ControlKind::container, component_row);
    auto component_button = NodeInput::box(0, 26); component_button.width = Axis::content();
    add(control::add_transform, 56, ControlKind::button, component_button, L"+ Transform").enabled = actor_status.can_edit && !transform;
    component_button.width = Axis::expand();
    if (cube != ObjectId{}) add(bind(cube), 56, ControlKind::button, component_button, L"− Cube").enabled = actor_status.can_edit;
    else add(control::add_cube, 56, ControlKind::button, component_button, L"+ Cube").enabled = actor_status.can_edit && transform.has_value();
    auto search_label = NodeInput::box(0, 18); search_label.width = Axis::expand();
    add(55, 50, ControlKind::label, search_label, L"Search parameters").role = ui::Role::caption;
    auto search_layout = NodeInput::box(0, 26); search_layout.width = Axis::expand();
    add(control::parameter_search, 50, ControlKind::text_edit, search_layout, query);
    auto folded = query;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    });
    constexpr std::array aliases{L"position 位置 x", L"position 位置 y", L"position 位置 z"};
    std::array<bool, 3> matches{};
    for (std::size_t axis = 0; axis < matches.size(); ++axis)
        matches[axis] = std::wstring_view(aliases[axis]).find(folded) != std::wstring_view::npos;
    const bool any = std::any_of(matches.begin(), matches.end(), [](bool match) { return match; });
    if (!any || !transform) add(53, 50, ControlKind::label, heading, any ? L"No Transform" : L"No matching parameters");
    auto section=NodeInput::box(0,26); section.width=Axis::expand();
    auto& fold=add(control::position_fold,50,ControlKind::foldout,section,L"Transform");
    fold.selected=position_expanded; fold.classes={"section"}; fold.layout.collapsed=!any;
    auto contents=NodeInput::column(7); contents.collapsed=!position_expanded || !any;
    add(58,50,ControlKind::container,contents);
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
        fields[field_count++]={field,names[axis],value,definition.default_value,definition.minimum,definition.maximum,definition.drag_step};
    }
    if (field_count) ui::append_number_row(result,{70,72,73,transform?bind(*transform,{},true):ui::ControlKey{74},58,
        L"Position (m)",actor_status.can_edit && transform.has_value(),group_modified},std::span(fields).first(field_count));
    auto hint=NodeInput::box(0,0); hint.width=Axis::expand(); hint.height=Axis::content();
    add(90, 50, ControlKind::label, hint, L"Middle drag: value ladder · Shift: fine steps").role = ui::Role::caption;
    add(91, 50, ControlKind::label, hint, L"Right click: parameter menu · Ctrl + middle: defaults").role = ui::Role::caption;
    add(92, 50, ControlKind::label, hint, L"Right drag: orbit · Wheel: zoom").role = ui::Role::caption;
    });
    auto footer = NodeInput::row(); footer.height = Axis::fixed(26); footer.padding = {8, 2, 8, 0};
    add(100, 1, ControlKind::container, footer).role = ui::Role::panel;
    auto status_box = NodeInput::box(0, 26); status_box.width = Axis::expand();
    add(101, 100, ControlKind::label, status_box, status + L"  |  " + std::to_wstring(members.size()) + L" Actors").role = ui::Role::caption;
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
            if (!items.empty()) {}
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
            else ui::append_menu(result,control::parameter_menu,source->key,menu->x,menu->y,216,items);
        }
    }
    return result;
}
}
