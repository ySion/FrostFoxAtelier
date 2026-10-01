#include "editor/workspace.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace atelier::editor {
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
    // Stable description order takes the first comparison; membership changes
    // search the remaining bindings. Retired keys are never assigned again.
    const auto first = bindings_.begin() + static_cast<std::ptrdiff_t>(binding_count_);
    auto found = std::find_if(first, bindings_.end(), [&](const auto& item) {
        return item.reset_position==reset_position && item.object == object && std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
    });
    if (found == bindings_.end()) {
        if (next_binding_ == std::numeric_limits<ui::ControlKey>::max())
            throw std::runtime_error("Editor control binding identities exhausted.");
        bindings_.push_back({next_binding_++, object, {path.begin(), path.end()}, reset_position});
        found = bindings_.end() - 1;
    }
    std::iter_swap(bindings_.begin() + static_cast<std::ptrdiff_t>(binding_count_), found);
    return bindings_[binding_count_++].key;
}
std::vector<ui::Control> Workspace::describe(const Session& session) {
    const auto take = []<class T>(Result<T> value) -> T {
        if (!value) throw std::runtime_error(value.error().message);
        return std::move(*value);
    };
    using namespace ui_layout;
    using ui::ControlKind;
    auto& runtime = session.world().runtime();
    const auto members = take(session.world().members());
    binding_count_ = 0;
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
    auto toolbar = NodeInput::row(6); toolbar.height = Axis::fixed(48); toolbar.padding = {12, 8, 12, 8};
    add(2, 1, ControlKind::container, toolbar).role = ui::Role::panel;
    add(3, 2, ControlKind::label, NodeInput::box(168, 32), L"FrostFox Atelier").role = ui::Role::title;
    const auto command = [&](ui::ControlKey key, float minimum_width, const wchar_t* text) -> ui::Control& {
        auto layout = NodeInput::box(0, 32); layout.width = Axis::content(minimum_width);
        auto& button=add(key, 2, ControlKind::button, layout, text);
        button.classes={"compact"};
        if (key==control::remove_actor) button.classes.push_back("danger");
        return button;
    };
    command(control::add_actor, 88, L"+ Add Actor").enabled = members.size() < Session::maximum_actors;
    command(control::remove_actor, 72, L"Delete").enabled = actor_status.can_edit;
    command(control::undo, 64, L"Undo").enabled = (show_workbench ? workbench.undo_count() : session.undo_count()) != 0;
    command(control::redo, 64, L"Redo").enabled = (show_workbench ? workbench.redo_count() : session.redo_count()) != 0;
    command(control::focus, 64, L"Focus").enabled = transform.has_value() &&
        (actor_status.state == world::ActorState::attached_inactive || actor_status.state == world::ActorState::active);
    command(control::reset, 108, L"Reset camera");
    command(control::workbench, 86, L"Controls").selected=show_workbench;
    auto body = NodeInput::row(); body.height = Axis::expand();
    add(20, 1, ControlKind::container, body);
    auto left = NodeInput::column(8); left.width = Axis::fixed(192); left.height = Axis::expand(); left.padding = {10, 12, 10, 12};
    add(21, 20, ControlKind::container, left).role = ui::Role::panel;
    auto heading = NodeInput::box(0, 28); heading.width = Axis::expand();
    add(22, 21, ControlKind::label, heading, L"ACTORS").role = ui::Role::caption;
    auto list = NodeInput::column(3); list.height = Axis::expand();
    add(23, 21, ControlKind::list, list);
    std::wstring selected_name = L"No selection";
    std::size_t ordinal{};
    for (const auto& member : members) {
        const auto state = take(world::inspect_actor(runtime, member.id()));
        auto name = L"Actor " + std::to_wstring(++ordinal);
        auto row = NodeInput::box(0, 32); row.width = Axis::expand();
        auto& item = add(bind(member.id()), 23, ControlKind::item, row, name);
        item.enabled = state.state == world::ActorState::attached_inactive || state.state == world::ActorState::active;
        item.selected = member.id() == selected;
        if (item.selected) selected_name = std::move(name);
    }
    auto viewport = NodeInput::box(0, 0); viewport.width = viewport.height = Axis::expand();
    add(control::viewport, 20, ControlKind::viewport, viewport);
    auto splitter = NodeInput::box(6, 0); splitter.height = Axis::expand();
    auto& divider = add(control::splitter, 20, ControlKind::splitter, splitter);
    divider.value = -parameter_width; divider.minimum = -560; divider.maximum = -220;
    divider.drag_step = divider.fine_step = 1;
    auto parameters = NodeInput::column(show_workbench?4.f:8.f); parameters.width = Axis::fixed(parameter_width); parameters.height = Axis::expand();
    parameters.padding = {16, 12, 16, 12};
    add(50, 20, ControlKind::container, parameters).role = ui::Role::panel;
    if (show_workbench) {
        workbench.append(result,50);
    } else {
    add(51, 50, ControlKind::label, heading, L"PARAMETERS").role = ui::Role::caption;
    add(52, 50, ControlKind::label, heading, std::move(selected_name)).role = ui::Role::title;
    auto component_row = NodeInput::row(6); component_row.height = Axis::fixed(30);
    add(56, 50, ControlKind::container, component_row);
    auto component_button = NodeInput::box(0, 30); component_button.width = Axis::content();
    add(control::add_transform, 56, ControlKind::button, component_button, L"+ Transform").enabled = actor_status.can_edit && !transform;
    component_button.width = Axis::expand();
    if (cube != ObjectId{}) add(bind(cube), 56, ControlKind::button, component_button, L"− Cube").enabled = actor_status.can_edit;
    else add(control::add_cube, 56, ControlKind::button, component_button, L"+ Cube").enabled = actor_status.can_edit && transform.has_value();
    auto search_label = NodeInput::box(0, 18); search_label.width = Axis::expand();
    add(55, 50, ControlKind::label, search_label, L"Search parameters").role = ui::Role::caption;
    auto search_layout = NodeInput::box(0, 34); search_layout.width = Axis::expand();
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
    auto section=NodeInput::box(0,28); section.width=Axis::expand();
    auto& fold=add(control::position_fold,50,ControlKind::foldout,section,L"Transform");
    fold.selected=position_expanded; fold.classes={"section"}; fold.layout.collapsed=!any;
    auto contents=NodeInput::column(8); contents.collapsed=!position_expanded || !any;
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
        fields[field_count++]={field,80+axis,names[axis],value,definition.default_value,definition.minimum,definition.maximum,definition.drag_step};
    }
    if (field_count) ui::append_number_row(result,{70,71,72,73,transform?bind(*transform,{},true):ui::ControlKey{74},58,
        L"Position (m)",actor_status.can_edit && transform.has_value(),group_modified},std::span(fields).first(field_count));
    auto hint=NodeInput::box(0,0); hint.width=Axis::expand(); hint.height=Axis::content();
    add(90, 50, ControlKind::label, hint, L"Drag to adjust · Shift for fine steps").role = ui::Role::caption;
    add(91, 50, ControlKind::label, hint, L"Click to type · Enter commits · Esc cancels").role = ui::Role::caption;
    add(92, 50, ControlKind::label, hint, L"Right drag: orbit · Wheel: zoom").role = ui::Role::caption;
    }
    auto footer = NodeInput::row(); footer.height = Axis::fixed(28); footer.padding = {12, 0, 12, 0};
    add(100, 1, ControlKind::container, footer).role = ui::Role::panel;
    auto status_box = NodeInput::box(0, 28); status_box.width = Axis::expand();
    add(101, 100, ControlKind::label, status_box, status + L"  |  " + std::to_wstring(members.size()) + L" Actors").role = ui::Role::caption;
    bindings_.erase(bindings_.begin() + static_cast<std::ptrdiff_t>(binding_count_), bindings_.end());
    return result;
}
}
