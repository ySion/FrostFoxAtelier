#include "editor/workspace.hpp"
#include "editor/assets.hpp"
#include "application/diagnostics.hpp"
#include "application/procedural_preview.hpp"
#include "editor/fields.hpp"
#include "editor/view_options.hpp"
#include "procedural/graph.hpp"
#include "foundation/set.hpp"
#include "platform/utf.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <cwctype>
#include <format>

namespace atelier::editor {
namespace {
struct CavityOption {
    std::wstring_view title;

    ViewOption option;
};

constexpr std::array cavity_options{CavityOption{L"Ridge", ViewOption::cavity_ridge},
                                    CavityOption{L"Valley", ViewOption::cavity_valley},
                                    CavityOption{L"Radius", ViewOption::cavity_radius}};

constexpr std::array<std::array<double, 5>, 3> cavity_values{
    {{0, .5, 1, 1.5, 2}, {0, .5, 1, 1.5, 2}, {.5, 1, 2, 3, 4}}};
}

Workspace::Workspace()
    : dock(1ULL << 48,
           {{control::scene_tab, control::scene_content, L"Scene View", {240, 160}},
            {control::parameters_tab, 50, L"Parameters", {220, 260}},
            {control::workbench, 300, L"Controls", {220, 280}},
            {control::entities_tab, 21, L"Scene Tree", {180, 140}},
            {control::structure_tab, control::structure_content, L"Entity Structure", {200, 140}},
            {control::assets_tab, control::assets_content, L"Asset Browser", {240, 140}},
            {source_control::tab, source_control::content, L"Code", {300, 180}},
            {control::output_tab, control::output_content, L"Project Output", {240, 120}},
            {world_control::tab, world_control::content, L"World", {280, 240}},
            {network_control::tab, network_control::content, L"Network", {220, 140}},
            {spreadsheet_control::tab, spreadsheet_control::content, L"Geometry Spreadsheet", {260, 140}}}) {
    const auto registered = ui_language::register_types(ui_runtime_);
    if (!registered)
        throw std::runtime_error(registered.error().message);
    register_builtin_operators(operators);
    const auto require_operator = [&](const OperatorCall& call) {
        if (!operators.find(call.id))
            throw std::logic_error("Unknown built-in operator: " + call.id);
    };
    for (const auto& item : menu_bindings)
        require_operator(item.call);
    for (const auto& entry : keymap())
        require_operator(entry.call);
    reset_layout();
}

namespace {
constexpr std::array<std::pair<RegionKind, ui::ControlKey>, 11> region_tabs{
    {{RegionKind::viewport, control::scene_tab},
     {RegionKind::scene_tree, control::entities_tab},
     {RegionKind::structure, control::structure_tab},
     {RegionKind::parameters, control::parameters_tab},
     {RegionKind::assets, control::assets_tab},
     {RegionKind::code, source_control::tab},
     {RegionKind::output, control::output_tab},
     {RegionKind::world, world_control::tab},
     {RegionKind::controls, control::workbench},
     {RegionKind::network, network_control::tab},
     {RegionKind::spreadsheet, spreadsheet_control::tab}}};
}

RegionKind region_of(ui::ControlKey tab) noexcept {
    const auto at = std::ranges::find(region_tabs, tab, &std::pair<RegionKind, ui::ControlKey>::second);
    return at == region_tabs.end() ? RegionKind::none : at->first;
}

ui::ControlKey region_tab(RegionKind region) noexcept {
    const auto at = std::ranges::find(region_tabs, region, &std::pair<RegionKind, ui::ControlKey>::first);
    return at == region_tabs.end() ? ui::ControlKey{} : at->second;
}

ObjectId Workspace::inspected_graph_object(const Session& session, std::string_view graph_key) const noexcept {
    const std::string resolved = graph_key.empty() ? current_network_graph(session) : std::string(graph_key);
    if (resolved.empty() || !session.has_graph(resolved))
        return {};
    if (!selected_graph_node.empty())
        if (const auto id = session.graph_node_id(resolved, selected_graph_node); id != ObjectId{})
            return id;
    return session.loaded_graph(resolved);
}

std::string Workspace::selected_generator_graph(const Session& session) const {
    const auto sel = session.selection();
    const ObjectId candidate = sel.object != ObjectId{} ? sel.object : sel.entity;
    if (candidate == ObjectId{})
        return {};
    const auto resolved = session.resolve_procedural_generator(candidate);
    if (!resolved)
        return {};
    const auto src = presentation::read_procedural_source(session.world().runtime(), *resolved);
    if (!src || src->source.empty() || session.is_scratch_graph(src->source))
        return {};
    return src->source;
}

std::string Workspace::current_network_graph(const Session& session) const {
    if (!active_graph.empty() && session.has_graph(active_graph))
        return active_graph;
    if (network_pinned && !pinned_graph.empty() && session.has_graph(pinned_graph))
        return pinned_graph;
    const auto key = selected_generator_graph(session);
    if (!key.empty() && session.has_graph(key))
        return key;
    return {};
}

std::string Workspace::effective_network_graph(Session& session, Project* project) {
    if (!active_graph.empty() && session.has_graph(active_graph))
        return active_graph;
    if (network_pinned && !pinned_graph.empty()) {
        if (!session.has_graph(pinned_graph) && project && project->entry(pinned_graph) &&
            failed_auto_open_graph_ != pinned_graph) {
            if (!session.open_graph(*project, pinned_graph))
                failed_auto_open_graph_ = pinned_graph;
            else
                failed_auto_open_graph_.clear();
        }
        if (session.has_graph(pinned_graph))
            return pinned_graph;
        network_pinned = false;
        pinned_graph.clear();
    }
    const auto key = selected_generator_graph(session);
    if (key.empty()) {
        failed_auto_open_graph_.clear();
        return {};
    }
    if (!session.has_graph(key) && project && project->entry(key) && failed_auto_open_graph_ != key) {
        if (!session.open_graph(*project, key))
            failed_auto_open_graph_ = key;
        else
            failed_auto_open_graph_.clear();
    }
    return session.has_graph(key) ? key : std::string{};
}

std::wstring Workspace::graph_path_title(const Project* project) const {
    if (active_graph.empty())
        return L"World";
    if (const auto* entry = project ? project->entry(active_graph) : nullptr)
        return L"/" + entry->path.generic_wstring();
    return L"/" + platform::utf8_to_utf16_validated(active_graph);
}

std::wstring Workspace::network_path_title(const Session& session, const Project* project,
                                           std::string_view net_graph) const {
    if (net_graph.empty())
        return L"World  ▸  (Select Generator Entity or Open .procgraph)";
    std::wstring stem = platform::utf8_to_utf16_validated(net_graph);
    std::wstring rel_path = L"/" + stem;
    if (const auto* entry = project ? project->entry(net_graph) : nullptr) {
        stem = entry->path.stem().wstring();
        rel_path = L"/" + entry->path.generic_wstring();
    }
    const std::wstring dirty_mark = session.graph_dirty(net_graph) ? L" *" : L"";
    if (!active_graph.empty() && active_graph == net_graph)
        return L"SOP  ▸  " + rel_path + dirty_mark;
    if (network_pinned && pinned_graph == net_graph)
        return L"Pinned  ▸  " + stem + dirty_mark;
    const auto sel = session.selection();
    if (sel.entity != ObjectId{} && session.world().contains(sel.entity))
        return L"World  ▸  " + std::wstring(session.name(sel.entity)) + L"  ▸  " + stem + dirty_mark;
    return L"World  ▸  " + stem + dirty_mark;
}

Context Workspace::operator_context(Session& session, Project* project, bool interactive,
                                    const ui::ControlBox* focused) const {
    const auto* source = source_editor.active();
    const auto source_history =
        source ? HistoryState{source->document->edit().can_undo(), source->document->edit().can_redo()}
               : HistoryState{};
    const auto region = region_of(dock.focused_tab());
    const auto selection = session.selection();
    SelectionContext selection_context{
        selection.entity, selection.object, {session.selected_entities().begin(), session.selected_entities().end()}};
    const std::string net_graph = current_network_graph(session);
    const bool graph_focus =
        (!active_graph.empty() && !inspecting_asset &&
         (region == RegionKind::parameters || region == RegionKind::network)) ||
        (active_graph.empty() && !net_graph.empty() && !inspecting_asset &&
         (region == RegionKind::network || (region == RegionKind::parameters && inspecting_network)));
    if (graph_focus) {
        if (const auto graph_object = inspected_graph_object(session, net_graph); graph_object != ObjectId{})
            selection_context = {ObjectId{}, graph_object, {}};
    }
    std::string asset;
    if (inspecting_asset && (region == RegionKind::parameters || region == RegionKind::assets))
        asset = material_inspection;
    else if (region == RegionKind::parameters && !graph_focus) {
        ObjectId object{};
        if (color_edit && color_edit->material_entry && session.owns_edit(control::color_group))
            object = color_edit->object;
        else if (focused && selection.entity != ObjectId{}) {
            const auto* target = binding(focused->action_target ? focused->action_target : focused->key);
            if (target && target->component != ObjectId{}) {
                // A selection can change before the next frame retires its old field.
                const auto component = world::inspect_component(session.world().runtime(), target->component);
                if (component && component->owner == selection.entity &&
                    (selection.object == selection.entity || selection.object == target->component))
                    object = target->object;
            }
        }
        if (object != ObjectId{})
            if (auto key = material::material_key(session.world().runtime(), object))
                asset = std::move(*key);
    }
    return {session,
            project,
            std::move(selection_context),
            region,
            std::move(asset),
            active_graph,
            net_graph,
            selected_graph_node,
            selected_graph_nodes,
            source_history,
            {workbench.undo_count() != 0, workbench.redo_count() != 0},
            scene_available,
            interactive,
            viewport_settings,
            {},
            view,
            source ? source->path : std::filesystem::path{},
            ui_source(),
            project_running,
            project_stopping,
            play_pending.has_value(),
            project_build && project_build->state == BuildState::ready,
            show_fps,
            !viewport_options_collapsed,
            camera_view};
}

bool Workspace::poll_output() {
    return output.poll();
}

Result<void> Workspace::locate_output() {
    const auto location = output.selected_source();
    if (!location)
        return std::unexpected(Error{ErrorCode::invalid_storage, "The selected record has no source location."});
    ATELIER_TRY(source_editor.locate(*location));
    dock.select(source_control::tab);
    return {};
}

void Workspace::reset_layout() {
    lighting.cancel();
    dock.reset();
    dock.move(world_control::tab, control::parameters_tab, ui::DockSide::tabs);
    dock.move(control::scene_tab, control::parameters_tab, ui::DockSide::left, .62f);
    dock.move(control::entities_tab, control::parameters_tab, ui::DockSide::bottom, .7f);
    dock.move(control::structure_tab, control::entities_tab, ui::DockSide::tabs);
    dock.move(network_control::tab, control::entities_tab, ui::DockSide::tabs);
    dock.select(control::entities_tab);
    dock.move(control::assets_tab, control::scene_tab, ui::DockSide::bottom, .73f);
    dock.move(source_control::tab, control::scene_tab, ui::DockSide::tabs);
    dock.move(control::output_tab, control::assets_tab, ui::DockSide::tabs);
    dock.move(spreadsheet_control::tab, control::assets_tab, ui::DockSide::tabs);
    dock.select(control::assets_tab);
    dock.select(control::scene_tab);
    dock.select(control::parameters_tab);
    menu.reset();
    menu_bar_collapsed = shelf_collapsed = false;
}

void Workspace::reset_document() {
    camera_flying = false;
    color_edit.reset();
    category_keys_.clear();
    property_help_.clear();
    lighting.cancel();
    lighting.observe(renderer::FrameStats{});
    lighting.performance().reset();
    if (ui_document_ && ui_document_->committed_source())
        ui_checkpoint_ = ui_document_->committed_source();
    ui_document_.reset();
    bindings_.clear();
    binding_cursor_ = 0;
    scene_tree_ = {};
    structure_tree_ = {};
    navigation_ = {ObjectId{}};
    navigation_cursor_ = 0;
    query.clear();
    tree_query.clear();
    structure_query.clear();
    parameter_search_visible = false;
    inspecting_asset = false;
    inspecting_network = false;
    material_inspection.clear();
    active_graph.clear();
    network_pinned = false;
    pinned_graph.clear();
    selected_graph_node.clear();
    selected_graph_nodes.clear();
    network_context_node.clear();
    last_network_graph_.clear();
    failed_auto_open_graph_.clear();
    spreadsheet.reset();
    network.reset();
    menu.reset();
    assets.reset(project_open);
    component_choices_.clear();
    material_choices_.clear();
    network_path_choices_.clear();
    menu_selection_ = {};
}

std::optional<Workspace::NetworkPathChoice> Workspace::network_path_choice(ui::ControlKey key) const {
    if (key < network_control::path_choice_first ||
        key - network_control::path_choice_first >= network_path_choices_.size())
        return {};
    return network_path_choices_[static_cast<std::size_t>(key - network_control::path_choice_first)];
}

std::optional<TypeId> Workspace::component_choice(ui::ControlKey key) const noexcept {
    if (key < control::component_choice_first || key - control::component_choice_first >= component_choices_.size())
        return {};
    return component_choices_[static_cast<std::size_t>(key - control::component_choice_first)].type;
}

Result<bool> Workspace::apply_ui(const ui::Action& action) {
    if (output.apply(action))
        return true;
    if (spreadsheet.apply(action))
        return true;
    return ui_document_ ? ui_document_->event(action) : Result<bool>{false};
}

void Workspace::commit_ui_controls() {
    if (!ui_document_)
        return;
    const auto revision = ui_document_->revision();
    ui_document_->commit_controls();
    if (ui_document_->revision() != revision && revision)
        status = L"UI updated: revision " + std::to_wstring(ui_document_->revision());
    if (ui_runtime_.collection_phase() == CollectionPhase::idle) {
        const auto started = ui_runtime_.begin_collection();
        if (!started)
            throw std::runtime_error(started.error().message);
    }
    const auto collected = ui_runtime_.collect_step({32});
    if (!collected)
        throw std::runtime_error(collected.error().message);
}

bool Workspace::reject_ui_controls(std::string message) {
    return ui_document_ && ui_document_->reject_controls(std::move(message));
}

void Workspace::set_ui_source(std::filesystem::path path) {
    if (ui_document_)
        throw std::logic_error("Choose the authoring file before opening the UI document.");
    ui_source_ = std::move(path);
}

std::filesystem::path Workspace::ui_source() const {
    if (!ui_source_.empty())
        return ui_source_;
    const std::filesystem::path authoring{ATELIER_UI_AUTHORING_PATH};
    std::error_code error;
    return std::filesystem::is_regular_file(authoring, error) ? authoring : asset_directory() / "editor.ui.source";
}

const Workspace::Binding* Workspace::binding(ui::ControlKey key) const noexcept {
    const auto found =
        std::find_if(bindings_.begin(), bindings_.end(), [key](const auto& item) { return item.key == key; });
    return found == bindings_.end() ? nullptr : &*found;
}

ui::ControlKey Workspace::object_control(ObjectId object) const noexcept {
    return field_control(object, {});
}

ui::ControlKey Workspace::field_control(ObjectId object, std::span<const FieldId> path) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
        return (item.kind == (path.empty() ? Binding::Kind::object : Binding::Kind::field) ||
                (!path.empty() && item.kind == Binding::Kind::rotation)) &&
               item.object == object && std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
    });
    return found == bindings_.end() ? 0 : found->key;
}

ui::ControlKey Workspace::reset_control(ObjectId object) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
        return item.kind == Binding::Kind::position_default && item.object == object;
    });
    return found == bindings_.end() ? 0 : found->key;
}

ui::ControlKey Workspace::name_control(ObjectId object) const noexcept {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& item) {
        return item.kind == Binding::Kind::name && item.object == object;
    });
    return found == bindings_.end() ? 0 : found->key;
}

bool Workspace::can_navigate(int direction) const noexcept {
    return direction < 0 ? navigation_cursor_ > 0 : navigation_cursor_ + 1 < navigation_.size();
}

Result<void> Workspace::navigate(Session& session, OperatorHost& host, int direction) {
    if (!can_navigate(direction))
        return {};
    const auto next = direction < 0 ? navigation_cursor_ - 1 : navigation_cursor_ + 1;
    auto changed = select_object(session, host, navigation_[next]);
    if (changed) {
        navigation_cursor_ = next;
        inspecting_asset = false;
        inspecting_network = false;
    }
    return changed;
}

Result<void> Workspace::select_object(Session& session, OperatorHost& host, ObjectId object) {
    SelectionContext selection;
    if (object != ObjectId{}) {
        auto entity = object;
        if (!session.world().contains(object)) {
            const auto component = world::inspect_component(session.world().runtime(), object);
            if (!component)
                return std::unexpected(component.error());
            entity = component->owner;
        }
        selection = {entity, object, {entity}};
    }
    auto result = call(operators, host, operator_context(session, nullptr, true),
                       {"scene.select", json::Document{}, {.selection = std::move(selection), .region = {}}});
    if (!result)
        return std::unexpected(result.error());
    inspecting_asset = false;
    inspecting_network = false;
    return {};
}

Result<void> Workspace::select_tree(Session& session, OperatorHost& host, ui::ControlKey key,
                                    ui::SelectionModifiers modifiers) {
    inspecting_asset = false;
    inspecting_network = false;
    if (key == control::tree_world)
        return select_object(session, host, {});
    const auto* target = binding(key);
    if (target && target->kind == Binding::Kind::object && (modifiers.shift || modifiers.control)) {
        std::vector<ObjectId> selected;
        if (modifiers.control)
            selected.assign(session.selected_entities().begin(), session.selected_entities().end());
        if (modifiers.shift) {
            // The range runs over the rows the Scene Tree shows, described or scrolled away.
            const auto row_of = [&](ObjectId entity) {
                return std::ranges::find_if(scene_tree_.rows,
                                            [&](const auto& row) { return scene_tree_.objects[row.node] == entity; });
            };
            const auto first = row_of(session.selection().entity);
            const auto last = row_of(target->object);
            if (first != scene_tree_.rows.end() && last != scene_tree_.rows.end()) {
                for (auto at = std::min(first, last); at <= std::max(first, last); ++at)
                    if (const auto entity = scene_tree_.objects[at->node];
                        entity != ObjectId{} && std::ranges::find(selected, entity) == selected.end())
                        selected.push_back(entity);
            } else
                selected.push_back(target->object);
        } else if (std::ranges::find(selected, target->object) != selected.end())
            std::erase(selected, target->object);
        else
            selected.push_back(target->object);
        const auto active = std::ranges::find(selected, target->object) != selected.end() ? target->object
                            : selected.empty()                                            ? ObjectId{}
                                                                                          : selected.back();
        auto result = call(operators, host, operator_context(session, nullptr, true),
                           {"scene.select",
                            json::Document{},
                            {.selection = SelectionContext{active, active, std::move(selected)}, .region = {}}});
        return result ? Result<void>{} : std::unexpected(result.error());
    }
    return target && (target->kind == Binding::Kind::object || target->kind == Binding::Kind::structure)
               ? select_object(session, host, target->object)
               : Result<void>{};
}

void Workspace::build_structure_tree(const Session& session, ObjectId entity) {
    // The Entity, then each Entity's Components before its children, in preorder.
    auto& tree = structure_tree_;
    tree.nodes.clear();
    tree.objects.clear();
    if (entity == ObjectId{})
        return;
    auto& runtime = session.world().runtime();
    struct Pending {
        ObjectId entity;

        std::uint32_t parent;
    };
    std::vector<Pending> pending{{entity, ui::no_tree_node}};
    while (!pending.empty()) {
        const auto entry = pending.back();
        pending.pop_back();
        const auto node = static_cast<std::uint32_t>(tree.nodes.size());
        tree.nodes.push_back({entry.parent, tree.expanded(entry.entity, true), {}});
        tree.objects.push_back(entry.entity);
        auto components = world::components(runtime, entry.entity);
        if (!components)
            throw std::runtime_error(components.error().message);
        for (const auto& component : *components) {
            tree.nodes.push_back({node, false, {}});
            tree.objects.push_back(component.id());
        }
        auto children = world::children(runtime, entry.entity);
        if (!children)
            throw std::runtime_error(children.error().message);
        for (auto child = children->rbegin(); child != children->rend(); ++child)
            pending.push_back({child->id(), node});
    }
}

Result<void> Workspace::set_tree_expanded(const Session& session, EntityTree tree, std::span<const ObjectId> objects,
                                          bool expanded) {
    auto& view = tree == EntityTree::scene_tree ? scene_tree_ : structure_tree_;
    if (tree == EntityTree::structure) {
        const auto entity = session.selection().entity;
        build_structure_tree(session, entity != ObjectId{} && session.world().contains(entity) ? entity : ObjectId{});
    }
    for (const auto object : objects)
        if (tree == EntityTree::scene_tree ? object != ObjectId{} && !session.world().contains(object)
                                           : view.node(object) == ui::no_tree_node)
            return std::unexpected(Error{ErrorCode::invalid_member, "The identity is not a row of this tree."});
    for (const auto object : objects)
        view.expansion[object] = expanded;
    return {};
}

Result<bool> Workspace::reveal_in_tree(const Session& session, ObjectId entity) {
    if (!session.world().contains(entity))
        return std::unexpected(Error{ErrorCode::invalid_member, "The Entity is not a member of this World."});
    std::vector<ObjectId> ancestors{ObjectId{}};
    for (auto parent = world::parent_entity(session.world().runtime(), entity);;
         parent = world::parent_entity(session.world().runtime(), *parent)) {
        if (!parent)
            return std::unexpected(parent.error());
        if (*parent == ObjectId{})
            break;
        ancestors.push_back(*parent);
    }
    ATELIER_TRY(set_tree_expanded(session, EntityTree::scene_tree, ancestors, true));
    return tree_query.empty() || std::ranges::any_of(scene_tree_.rows, [&](const auto& row) {
               return scene_tree_.objects[row.node] == entity;
           });
}

std::vector<std::uint32_t> Workspace::window_tree(EntityTreeView& tree, ObjectId primary, std::wstring_view filter,
                                                  ui::TreeViewport viewport, std::span<const ObjectId> pinned) {
    // A new primary row, or the primary row under a new parent, expands its ancestors once.
    if (const auto node = tree.node(primary); node != ui::no_tree_node) {
        const auto parent = tree.nodes[node].parent;
        const std::pair current{primary, parent == ui::no_tree_node ? ObjectId{} : tree.objects[parent]};
        if (tree.primary != current) {
            for (auto at = parent; at != ui::no_tree_node; at = tree.nodes[at].parent) {
                tree.expansion[tree.objects[at]] = true;
                tree.nodes[at].expanded = true;
            }
            tree.primary = current;
        }
    } else
        tree.primary.reset();
    ui::flatten_tree(tree.nodes, filter, tree.rows);
    std::vector<std::uint32_t> rows;
    for (const auto object : pinned)
        if (const auto node = tree.node(object); node != ui::no_tree_node)
            if (const auto row = std::ranges::find(tree.rows, node, &ui::TreeRow::node); row != tree.rows.end())
                rows.push_back(static_cast<std::uint32_t>(row - tree.rows.begin()));
    return ui::tree_window(tree.rows, viewport, rows);
}

ui::ControlKey Workspace::operator_control(const OperatorCall& call) const {
    for (const auto& binding : bindings_)
        if (binding.call && binding.call->request.id == call.id && binding.call->request.args == call.args)
            return binding.key;
    return 0;
}

ui::ControlKey Workspace::bind(ObjectId object, std::span<const FieldId> path, Binding::Kind kind) {
    if (!path.empty() && kind == Binding::Kind::object)
        kind = Binding::Kind::field;
    const auto first = bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_);
    const auto found = std::find_if(first, bindings_.end(), [&](const auto& item) {
        return item.kind == kind && item.object == object && item.component == ObjectId{} && item.slot.empty() &&
               std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
    });
    if (found != bindings_.end()) {
        std::iter_swap(first, found);
        return bindings_[binding_cursor_++].key;
    }
    if (next_binding_ == std::numeric_limits<ui::ControlKey>::max())
        throw std::runtime_error("Editor control identities exhausted.");
    bindings_.push_back({next_binding_++, object, {path.begin(), path.end()}, kind});
    std::iter_swap(bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_), bindings_.end() - 1);
    return bindings_[binding_cursor_++].key;
}

ui::ControlKey Workspace::bind_operator(OperatorCall call) {
    const auto first = bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_);
    const auto found = std::find_if(first, bindings_.end(), [&](const auto& item) {
        return item.kind == Binding::Kind::operator_call && item.call->request == call;
    });
    if (found != bindings_.end()) {
        std::iter_swap(first, found);
        return bindings_[binding_cursor_++].key;
    }
    if (next_binding_ == std::numeric_limits<ui::ControlKey>::max())
        throw std::runtime_error("Editor control identities exhausted.");
    Binding binding{next_binding_++,
                    call.context.selection ? call.context.selection->object : ObjectId{},
                    {},
                    Binding::Kind::operator_call};
    binding.call = std::make_unique<OperatorBinding>(std::move(call));
    bindings_.push_back(std::move(binding));
    std::iter_swap(bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_), bindings_.end() - 1);
    return bindings_[binding_cursor_++].key;
}

bool Workspace::effective_show_points(const procedural::Geometry* geometry) const noexcept {
    if (sop_show_points.has_value())
        return *sop_show_points;
    return geometry != nullptr && application::is_pure_point_cloud(*geometry);
}

ObjectId Workspace::active_procedural_generator(const Session& session) const noexcept {
    if (!active_graph.empty())
        return {};
    const auto sel = session.selection();
    const ObjectId candidate = sel.object != ObjectId{} ? sel.object : sel.entity;
    const auto resolved = session.resolve_procedural_generator(candidate);
    if (!resolved)
        return {};
    auto& runtime = session.world().runtime();
    const auto comp_state = world::inspect_component(runtime, *resolved);
    if (!comp_state || !comp_state->valid || comp_state->owner == ObjectId{} ||
        !session.world().contains(comp_state->owner))
        return {};
    const auto owner_enabled = world::effective_enabled(runtime, comp_state->owner);
    if (!comp_state->enabled || !owner_enabled || !*owner_enabled)
        return {};
    return *resolved;
}

Result<bool> Workspace::apply_view(Session& session, OperatorHost& host, const ui::Action& action) {
    if (action.kind == ui::ActionKind::activate &&
        (action.key == control::group_components || action.key == control::group_categories)) {
        ATELIER_TRY(session.cancel_edit());
        parameter_by_category_ = action.key == control::group_categories;
        return true;
    }
    sync_world(session);
    auto candidate = view;
    const auto publish = [&]() -> Result<bool> {
        if (candidate == view)
            return true;
        json::Builder patch;
        for (const auto& option : view_options()) {
            const auto value = view_value(candidate, option.option);
            if (value == view_value(view, option.option))
                continue;
            json::Result<void> added;
            if (option.kind == ViewValueKind::enumeration)
                patch.set_validated(patch.root(), option.name, option.choices[static_cast<std::size_t>(value)]);
            else if (option.kind == ViewValueKind::boolean)
                added = patch.set(patch.root(), option.name, value != 0);
            else
                added = patch.set(patch.root(), option.name, value);
            if (!added)
                return std::unexpected(Error{ErrorCode::invalid_definition, added.error().describe()});
        }
        auto result = call(operators, host, operator_context(session, nullptr, true), {"view.set", patch.finish()});
        return result ? Result<bool>{true} : std::unexpected(result.error());
    };
    const auto edited = lighting.apply(action, candidate);
    if (!edited)
        return std::unexpected(edited.error());
    if (*edited)
        return publish();
    if (action.kind != ui::ActionKind::activate)
        return false;
    const auto key = action.key;
    const auto set = [&](ViewOption option, double value) {
        auto changed = set_view_option(candidate, option, value);
        if (!changed)
            throw std::runtime_error(changed.error().message);
    };
    if (key >= control::shading_preset_first && key < control::shading_preset_first + 4) {
        const auto preset = key - control::shading_preset_first;
        set(ViewOption::wireframe, preset == 0 ? double(renderer::Wireframe::only) : double(renderer::Wireframe::off));
        if (preset)
            set(ViewOption::shading, preset == 1   ? double(renderer::Shading::smooth)
                                     : preset == 2 ? double(renderer::Shading::matcap)
                                                   : double(renderer::Shading::scene_lit));
    } else if (((key >= control::display_grid && key <= control::display_lit) || key == control::display_backfaces) &&
               key != control::display_fps) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked)
            return false;
        if (key == control::display_grid)
            set(ViewOption::reference_grid, *checked ? 1 : 0);
        else if (key == control::display_axes)
            set(ViewOption::origin_axes, *checked ? 1 : 0);
        else if (key == control::display_wire)
            set(ViewOption::wireframe, double(*checked ? renderer::Wireframe::overlay : renderer::Wireframe::off));
        else if (key == control::display_wire_only)
            set(ViewOption::wireframe, double(*checked ? renderer::Wireframe::only : renderer::Wireframe::off));
        else if (key == control::display_triangles)
            set(ViewOption::triangle_edges, *checked ? 1 : 0);
        else if (key == control::display_backfaces)
            set(ViewOption::backface_tint, *checked ? 1 : 0);
        else if (key == control::display_cavity)
            set(ViewOption::cavity, *checked ? 1 : 0);
        else
            set(ViewOption::shading, double(*checked ? renderer::Shading::scene_lit : renderer::Shading::smooth));
    } else if (key == control::cavity_enabled) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked)
            return false;
        set(ViewOption::cavity, *checked ? 1 : 0);
    } else if (key >= control::cavity_value_first && key < control::cavity_value_first + 15) {
        const auto index = key - control::cavity_value_first;
        set(cavity_options[index / 5].option, cavity_values[index / 5][index % 5]);
    } else if (key >= control::shading_first && key < control::shading_first + 3)
        set(ViewOption::shading, double(key - control::shading_first));
    else if (key == control::shading_scene_lit)
        set(ViewOption::shading, double(renderer::Shading::scene_lit));
    else if (key >= control::edges_first && key < control::edges_first + 4) {
        const auto option = key - control::edges_first;
        set(ViewOption::wireframe, double(option < 2 ? option : 2));
        set(ViewOption::hidden_lines, option != 3 ? 1 : 0);
    } else if (key >= control::topology_first && key < control::topology_first + 2)
        set(ViewOption::triangle_edges, key != control::topology_first ? 1 : 0);
    else if (key == control::display_sop_points || key == control::display_sop_normals ||
             key == control::display_sop_face_normals || key == control::display_sop_orient ||
             key == control::display_sop_bounds || key == control::display_sop_info) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked)
            return false;
        if (key == control::display_sop_points)
            sop_show_points = *checked;
        else if (key == control::display_sop_normals)
            sop_show_normals = *checked;
        else if (key == control::display_sop_face_normals)
            sop_show_face_normals = *checked;
        else if (key == control::display_sop_orient)
            sop_show_orient = *checked;
        else if (key == control::display_sop_bounds)
            sop_show_bounds = *checked;
        else
            sop_show_info = *checked;
        ++view_revision_;
        status = L"Procedural guide display updated";
        return true;
    } else if (key == control::guide_background || key == control::guide_grid || key == control::guide_axes) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked)
            return false;
        if (key == control::guide_background) {
            if (view.shading == renderer::Shading::scene_lit)
                return false;
            set(ViewOption::background, *checked ? 1 : 0);
        } else if (key == control::guide_grid)
            set(ViewOption::reference_grid, *checked ? 1 : 0);
        else
            set(ViewOption::origin_axes, *checked ? 1 : 0);
    } else
        return false;
    const auto updated = publish();
    if (!updated)
        return updated;
    status = L"Viewport display updated";
    return true;
}

std::optional<Workspace::TreeDrop> Workspace::tree_drop(const Session& session, const ui::UiFrame& frame,
                                                        ObjectId dragged, ui::DragPoint at) const {
    const auto* tree = frame.find(control::entity_tree);
    if (!tree || at.x < tree->bounds.x || at.x >= tree->bounds.x + tree->bounds.width || at.y < tree->bounds.y ||
        at.y >= tree->bounds.y + tree->bounds.height)
        return {};
    for (const auto& box : frame.controls) {
        // Rows scrolled out of the tree are outside its bounds, so the tree test above already excludes them.
        if (box.kind != ui::ControlKind::tree_item || box.parent != control::entity_tree || at.y < box.bounds.y ||
            at.y >= box.bounds.y + box.bounds.height)
            continue;
        if (box.key == control::tree_world)
            return TreeDrop{{}, box.key};
        const auto* target = binding(box.key);
        if (!target || target->kind != Binding::Kind::object || !target->path.empty())
            return {};
        // An Entity cannot become a child of itself or of its own descendant.
        auto& runtime = session.world().runtime();
        for (auto ancestor = target->object; ancestor != ObjectId{};) {
            if (ancestor == dragged)
                return {};
            const auto parent = world::parent_entity(runtime, ancestor);
            if (!parent)
                return {};
            ancestor = *parent;
        }
        return TreeDrop{target->object, box.key};
    }
    // Empty space below the rows is World, as in Houdini's tree view.
    return TreeDrop{{}, control::tree_world};
}

bool Workspace::sync_world(const Session& session) {
    auto& author = static_cast<rendering::SceneLightingSettings&>(view.lit);
    const auto current = session.presentation().environment();
    if (!current || author == *current)
        return false;
    lighting.cancel();
    author = *current;
    ++view_revision_;
    return true;
}

Result<void> Workspace::set_view_settings(Session& session, const renderer::ViewSettings& candidate,
                                          math::Vector3 focus) {
    ATELIER_TRY(validate_view_options(candidate, focus));
    ATELIER_TRY(session.set_environment(candidate.lit));
    if (view != candidate) {
        lighting.cancel();
        view = candidate;
        ++view_revision_;
    }
    return {};
}

namespace {
double scalar_number(const Scalar& value) {
    return std::visit(
        [](const auto& scalar) -> double {
            if constexpr (std::same_as<std::decay_t<decltype(scalar)>, EnumValue>)
                return scalar.bits;
            else
                return static_cast<double>(scalar);
        },
        value);
}

// Typed and dragged values stay within the hard bounds, as Runtime writes clamp to them.
// A stored value outside them (a trusted native write) widens the range so it still displays.
std::pair<double, double> legal_range(const Scalar& value, const FieldRange& range) {
    auto limits = std::visit(
        [](const auto& scalar) -> std::pair<double, double> {
            using T = std::decay_t<decltype(scalar)>;
            if constexpr (std::same_as<T, EnumValue> || std::same_as<T, bool>)
                return {-std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
            else
                return {double(std::numeric_limits<T>::lowest()), double(std::numeric_limits<T>::max())};
        },
        value);
    if (range.hard_min)
        limits.first = scalar_number(*range.hard_min);
    if (range.hard_max)
        limits.second = scalar_number(*range.hard_max);
    const auto current = scalar_number(value);
    return {std::min(limits.first, current), std::max(limits.second, current)};
}

// Slider endpoints are the UI range; a side with only a hard bound uses it.
std::optional<std::pair<double, double>> slider_range(const FieldRange& range) {
    const auto& low = range.ui_min ? range.ui_min : range.hard_min;
    const auto& high = range.ui_max ? range.ui_max : range.hard_max;
    if (!low || !high || !(scalar_number(*low) < scalar_number(*high)))
        return std::nullopt;
    return std::pair{scalar_number(*low), scalar_number(*high)};
}

std::string range_note(const FieldRange& range) {
    const auto text = [](const Scalar& value) {
        char buffer[32];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), scalar_number(value));
        return std::string(buffer, written.ptr);
    };
    std::string note;
    const auto add = [&](const std::string& part) { note += (note.empty() ? "" : "，") + part; };
    if (range.ui_min || range.ui_max)
        add("范围 " + (range.ui_min ? text(*range.ui_min) : "-∞") + " – " + (range.ui_max ? text(*range.ui_max) : "∞"));
    const bool lock_min = range.hard_min && range.hard_min == range.ui_min;
    const bool lock_max = range.hard_max && range.hard_max == range.ui_max;
    if (lock_min && lock_max)
        add("上下限锁定");
    else if (lock_min)
        add("下限锁定");
    else if (lock_max)
        add("上限锁定");
    if (range.hard_min && !lock_min)
        add("硬下限 " + text(*range.hard_min));
    if (range.hard_max && !lock_max)
        add("硬上限 " + text(*range.hard_max));
    return note;
}

std::string described(std::string description, const FieldRange& range) {
    if (auto note = range_note(range); !note.empty())
        description += (description.empty() ? "" : "\n") + note;
    return description;
}

// Houdini-style parameter label from a reflected field path: "material.base_color"
// becomes "Base Color". The path still identifies the field; only the caption changes.
ui::ControlIcon light_icon(presentation::LightKind kind) {
    switch (kind) {
    case presentation::LightKind::point:
        return ui::ControlIcon::point_light;
    case presentation::LightKind::rectangle:
        return ui::ControlIcon::area_light;
    case presentation::LightKind::directional:
        return ui::ControlIcon::sun_light;
    }
    return ui::ControlIcon::point_light;
}

// One glyph per Entity, as Houdini's network/tree icons: the type is carried by
// the icon so rows can show only the name.
ui::ControlIcon entity_icon(const Session& session, ObjectId entity) {
    auto& runtime = session.world().runtime();
    const auto state = world::inspect_entity(runtime, entity);
    if (state && (runtime.is_a(state->type, session.presentation_types().static_mesh_entity).value_or(false) ||
                  runtime.is_a(state->type, session.presentation_types().procedural_generator_entity).value_or(false)))
        return ui::ControlIcon::mesh;
    if (const auto parts = world::components(runtime, entity))
        for (const auto& part : *parts) {
            const auto type = runtime.type_of(part.id());
            if (type && runtime.is_a(*type, session.presentation_types().light_component).value_or(false))
                if (const auto light = presentation::read_light(runtime, part.id()))
                    return light_icon(light->kind);
            if (type &&
                runtime.is_a(*type, session.presentation_types().procedural_generator_component).value_or(false))
                return ui::ControlIcon::mesh;
        }
    return ui::ControlIcon::object;
}

std::wstring parameter_label(std::string_view path) {
    if (const auto dot = path.rfind('.'); dot != path.npos)
        path = path.substr(dot + 1);
    std::wstring label;
    bool start = true;
    for (const char c : path) {
        if (c == '_') {
            label += L' ';
            start = true;
            continue;
        }
        label += static_cast<wchar_t>(start && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        start = false;
    }
    return label;
}

std::string_view call_string_arg(const OperatorCall& call, std::string_view key) noexcept {
    if (const auto field = call.args.root().find(key); field && *field)
        if (const auto text = (*field)->string())
            return *text;
    return {};
}

std::wstring sanitize_single_line(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text)
        out.push_back(c < 32 ? L' ' : c);
    return out;
}

ui_language::TooltipBinding describe_operator_tooltip(PropertyHelpCache& cache, ui::ControlKey key,
                                                      const OperatorCall& call, const OperatorInfo* info,
                                                      const Context& target, const Result<void>& polled) {
    std::wstring title = info ? info->title : platform::utf8_to_utf16_validated(call.id);
    std::wstring identity = L"Operator · " + platform::utf8_to_utf16_validated(call.id);
    std::string desc = info ? info->description : std::string{};

    if (call.id == "tool.settings") {
        if (const auto tool = call_string_arg(call, "tool"); !tool.empty()) {
            if (tool == "select") {
                title = L"Select Tool";
                desc = "左键放置 3D 游标，右键点选或拖拽框选实体；中键旋转视图，Shift+中键平移。";
            } else if (tool == "transform") {
                title = L"Transform Tool";
                desc = "综合变换手柄：可直接拖拽移动、旋转或缩放，也可按 G / R / S 进入模态变换。";
            } else if (tool == "move") {
                title = L"Move Tool";
                desc = "拖拽轴向箭头或平面移动选中实体；按 G 后可按 X / Y / Z 锁定单轴并直接输入数值。";
            } else if (tool == "rotate") {
                title = L"Rotate Tool";
                desc = "拖拽旋转环旋转选中实体；按 R 后可按 X / Y / Z 锁定旋转轴并输入角度。";
            } else if (tool == "scale") {
                title = L"Scale Tool";
                desc = "拖拽轴向方块缩放单轴，拖拽中心均匀缩放；按 S 后可按 X / Y / Z 锁定缩放轴。";
            }
        } else if (const auto mode = call_string_arg(call, "select_mode"); !mode.empty()) {
            if (mode == "set") {
                title = L"Selection Mode: New";
                desc = "点选或框选时以新结果替换当前选择集。";
            } else if (mode == "extend") {
                title = L"Selection Mode: Extend";
                desc = "将命中的实体追加并入当前选择集。";
            } else if (mode == "subtract") {
                title = L"Selection Mode: Subtract";
                desc = "从当前选择集中移除本次命中的实体。";
            } else if (mode == "invert") {
                title = L"Selection Mode: Invert";
                desc = "翻转本次命中实体的选中状态。";
            } else if (mode == "intersect") {
                title = L"Selection Mode: Intersect";
                desc = "仅保留原选择集与本次框选区域的交集。";
            }
        }
    } else if (call.id == "tool.toggle") {
        const auto setting = call_string_arg(call, "setting");
        if (setting == "grid_snap") {
            title = L"Grid Snapping";
            identity = target.tools.grid_snap ? L"Currently: On" : L"Currently: Off";
            desc = "启用后移动操作按步长吸附到参考网格，旋转与缩放按步进吸附。";
        } else if (setting == "cursor_surface") {
            title = L"Surface Cursor";
            identity = target.tools.cursor_surface ? L"Currently: On" : L"Currently: Off";
            desc = "启用后左键放置 3D 游标时会射线投射吸附到场景几何体表面，否则落在焦平面上。";
        }
    } else if (call.id == "view.toggle") {
        const auto opt = call_string_arg(call, "option");
        if (opt == "fps") {
            title = L"Frame Rate Overlay";
            identity = target.show_fps ? L"Currently: On" : L"Currently: Off";
            desc = "在视口左上角显示平滑后的实时呈现帧率（FPS）读数。";
        } else if (opt == "options") {
            title = L"Tool Options Bar";
            identity = target.viewport_options ? L"Currently: Expanded" : L"Currently: Collapsed";
            desc = "展开或收起视口顶部的工具选项栏（选择模式、变换坐标系与表面游标）。";
        }
    } else if (call.id == "scene.add") {
        const auto kind = call_string_arg(call, "kind");
        if (kind == "entity") {
            title = L"Create Empty Entity";
            desc = "在当前 3D 游标位置创建一个空白实体，可挂载任意组件或作为层级父节点。";
        } else if (kind == "mesh") {
            title = L"Create Static Mesh";
            desc = "在当前 3D 游标位置创建一个静态网格实体（StaticMeshEntity）。";
        } else if (kind == "procedural_generator") {
            title = L"Create Procedural Generator";
            desc = "在当前 3D 游标位置创建程序化几何生成器实体，绑定 .procgraph 图实时生成网格与实例。";
        } else if (kind == "camera") {
            title = L"Create Camera";
            desc = "在当前 3D 游标位置创建一个场景相机实体。";
        } else if (kind == "point") {
            title = L"Create Point Light";
            desc = "在当前 3D 游标位置创建全向点光源实体。";
        } else if (kind == "rectangle") {
            title = L"Create Area Light";
            desc = "在当前 3D 游标位置创建矩形面光源实体。";
        } else if (kind == "directional") {
            title = L"Create Sun Light";
            desc = "创建平行方向光源实体，用于驱动主方向照明与阴影。";
        }
    } else if (call.id == "view.frame_selected") {
        title = L"Frame Selected";
        desc = "将视口相机对准并完整框显当前选中的实体或 SOP 程序化几何体边界。";
    } else if (call.id == "view.reset") {
        title = L"Reset Camera View";
        desc = "将视口相机恢复为默认透视观察角度与距离。";
    } else if (call.id == "graph.layout") {
        title = L"Auto Layout Network";
        desc = "按拓扑数据流层级自动排布当前程序化图中的所有节点。";
    } else if (call.id == "graph.select" && call_string_arg(call, "node").empty()) {
        title = L"Graph Settings";
        desc = "取消节点单选，在属性面板中查看和编辑整张 .procgraph 图的根种子与属性。";
    } else if (call.id == "graph.delete_node") {
        title = L"Delete Selected Nodes";
        desc = "删除画布中选中的程序化节点，并在单输入链上自动保留直通连线。";
    } else if (call.id == "graph.exit") {
        title = L"Exit SOP to World";
        desc = "退出当前程序化图（SOP）编辑上下文，返回场景世界（World）视图。";
    } else if (call.id == "play.start") {
        title = L"Run Project";
        desc = "编译并启动当前工程的实时运行会话。";
    } else if (call.id == "play.stop") {
        title = L"Stop Project";
        desc = "停止当前正在运行的工程会话并恢复编辑态。";
    } else if (call.id == "play.restart") {
        title = L"Restart Project";
        desc = "重启正在运行的工程会话，使最新场景或代码改动重新生效。";
    } else if (call.id == "project.build") {
        title = L"Build Project";
        desc = "编译当前工程中的自有静态语言组件、材质与程序化节点源码。";
    } else if (call.id == "source.open") {
        title = L"Open Source File...";
        desc = "选择并打开源码文件到代码编辑器中。";
    } else if (call.id == "file.save") {
        title = L"Save Document";
        desc = "保存当前面板关联的活动文档（源码、材质、程序化图或场景）。";
    } else if (call.id == "source.close") {
        title = L"Close Source Tab";
        desc = "关闭当前活动源码文档标签页。";
    } else if (call.id == "source.reload") {
        title = L"Reload from Disk";
        desc = "放弃内存未保存改动，从磁盘重新读取最新文件内容。";
    } else if (call.id == "source.new_component") {
        title = L"New Source File...";
        desc = "在工程源码目录下创建新的自有静态语言源码文件。";
    } else if (call.id == "scene.main_camera_clear") {
        title = L"Clear Main Camera";
        desc = "解除当前场景绑定的主相机引用。";
    } else if (call.id == "scene.delete") {
        title = L"Remove Selected Object";
        desc = "移除当前选中的组件或场景实体（记录一步撤销历史）。";
    }

    std::wstring_view key_hint = shortcut(call, target.region);
    if (key_hint.empty())
        key_hint = shortcut(call, RegionKind::viewport);
    if (key_hint.empty())
        key_hint = shortcut(call, RegionKind::network);
    if (!key_hint.empty() && title.find(L'[') == std::wstring::npos)
        title += L" [" + std::wstring(key_hint) + L"]";

    if (!polled && !polled.error().message.empty()) {
        if (!desc.empty())
            desc += "\n";
        desc += "当前不可用：" + polled.error().message;
    }
    return cache.get(key, sanitize_single_line(title), sanitize_single_line(identity), desc);
}
}

Result<std::vector<ui::Control>> Workspace::describe(Session& session, ui_layout::Size size, const ui::UiFrame* frame,
                                                     Project* project, const ui::Session* ui,
                                                     const ui::ControlBox* command_focus) {
    ui::ControlList list;
    ATELIER_TRY(describe(list, session, size, frame, project, ui, command_focus));
    return std::move(list).take();
}

Result<void> Workspace::describe(ui::ControlList& result, Session& session, ui_layout::Size size,
                                 const ui::UiFrame* frame, Project* project, const ui::Session* ui,
                                 const ui::ControlBox* command_focus) {
    property_help_.begin();
    sync_world(session);
    if (!command_focus && frame && ui)
        command_focus = frame->find(ui->focused_control());
    const auto take = []<class T>(Result<T> value) -> T {
        if (!value)
            throw std::runtime_error(value.error().message);
        return std::move(*value);
    };
    using namespace ui_layout;
    using ui::ControlKind;
    constexpr std::array shading_titles{L"Smooth Shaded", L"Flat Shaded", L"MatCap Shaded"};
    constexpr std::array edge_titles{L"Shading Only", L"Shading + Wireframe", L"Wireframe · Visible Edges",
                                     L"Wireframe · All Edges"};
    constexpr std::array topology_titles{L"Polygon Edges", L"Triangle Edges"};
    const auto edge_option = view.wireframe == renderer::Wireframe::only ? (view.hidden_lines ? 2u : 3u)
                                                                         : static_cast<unsigned>(view.wireframe);
    auto& runtime = session.world().runtime();
    if (!ui_document_) {
        auto recovery = ui_checkpoint_;
        if (!recovery)
            if (auto bundled = assets::read_file(asset_directory() / "editor.ui.source", 1024 * 1024); bundled)
                recovery = std::move(*bundled);
        ui_document_ =
            std::make_unique<ui_language::LiveDocument>(ui_runtime_, ui_source(), next_binding_, std::move(recovery));
    }
    source_editor.poll(frame && frame->input_anchor && frame->input_anchor->token.key == source_control::buffer &&
                       frame->input_anchor->composition_id);
    ui_document_->poll();
    project_ui_configured = project && !project->ui_source().empty();
    const auto members = take(session.world().members());
    // The World already indexes all attached components, including disabled
    // ones. UI liveness only needs their IDs, not a new Root for each member.
    const auto components = take(session.world().query(session.types().component));
    const auto graph_objects = session.graph_objects();
    if (!active_graph.empty() && !session.has_graph(active_graph)) {
        active_graph.clear();
        selected_graph_node.clear();
    } else if (!active_graph.empty() && !selected_graph_node.empty() &&
               !session.graph_node(active_graph, selected_graph_node)) {
        if (const auto* graph_doc = session.graph_document(active_graph)) {
            std::optional<std::string> disp_name;
            if (graph_doc->graph.registered())
                if (const auto listed = procedural::contents(runtime, graph_doc->graph.id()))
                    if (listed->display() != ObjectId{})
                        disp_name = session.graph_node_name(listed->display());
            if (disp_name)
                selected_graph_node = std::move(*disp_name);
            else if (!graph_doc->nodes.empty())
                selected_graph_node = graph_doc->nodes.back().name;
            else
                selected_graph_node.clear();
        } else
            selected_graph_node.clear();
    }
    Set<ObjectId> live;
    live.reserve(members.size() + components.size() + graph_objects.size() + 1);
    for (const auto& member : members)
        live.add(member.id());
    for (const auto component : components)
        live.add(component);
    for (const auto graph_object : graph_objects)
        live.add(graph_object);
    const auto material_object = inspecting_asset ? session.loaded_material(material_inspection) : ObjectId{};
    if (material_object != ObjectId{})
        live.add(material_object);
    // Only material objects can extend the membership directory. Most frames
    // reference Entity/Component bindings exclusively, so avoid reading every
    // mesh's material slots unless an existing UI reference needs that lookup.
    const auto unresolved = [&](ObjectId object) { return object != ObjectId{} && !live.contains(object); };
    const bool needs_materials =
        unresolved(session.selection().object) || std::ranges::any_of(navigation_, unresolved) ||
        std::ranges::any_of(bindings_, [&](const auto& binding) { return unresolved(binding.object); });
    if (needs_materials)
        for (const auto component : components) {
            const auto bindings = presentation::read_material_bindings(runtime, component);
            if (bindings)
                for (const auto& binding : *bindings)
                    live.add(binding.resource.id());
            if (const auto mesh = presentation::read_static_mesh(runtime, component))
                for (const auto& binding : mesh->mesh.defaults)
                    live.add(binding.resource.id());
        }
    // Closing/hiding a view is not an object lifetime change. Retire bindings and
    // tree view state together; undo later restores identity with fresh keys.
    std::erase_if(bindings_, [&](const auto& item) {
        return !(item.kind == Binding::Kind::operator_call && item.object == ObjectId{}) && !live.contains(item.object);
    });
    for (auto* tree : {&scene_tree_, &structure_tree_})
        std::erase_if(tree->expansion,
                      [&](const auto& entry) { return entry.first != ObjectId{} && !live.contains(entry.first); });
    binding_cursor_ = 0;
    // The final host-authored tooltips also depend on these inputs outside their DSL region.
    const unsigned help_state = unsigned(menu_bar_collapsed) | (unsigned(shelf_collapsed) << 1) |
                                (unsigned(parameter_search_visible) << 2) |
                                (unsigned(spreadsheet.follow_selected()) << 3);
    result.begin(help_state);
    const auto requested_selection = session.selection().entity;
    const auto selected =
        std::ranges::any_of(members, [&](const auto& member) { return member.id() == requested_selection; })
            ? requested_selection
            : ObjectId{};
    const auto inspected =
        selected != ObjectId{} && live.contains(session.selection().object) ? session.selection().object : ObjectId{};
    const auto old_cursor = navigation_cursor_;
    std::size_t destination{};
    for (std::size_t i = 0; i < navigation_.size(); ++i) {
        const auto id = navigation_[i];
        if (id != ObjectId{} && !live.contains(id))
            continue;
        navigation_[destination] = id;
        if (i <= old_cursor)
            navigation_cursor_ = destination;
        ++destination;
    }
    navigation_.resize(destination); // The World entry always survives.
    if (navigation_[navigation_cursor_] != inspected) {
        navigation_.resize(navigation_cursor_ + 1);
        navigation_.push_back(inspected);
        if (navigation_.size() > 64)
            navigation_.erase(navigation_.begin() + 1); // Keep World reachable.
        navigation_cursor_ = navigation_.size() - 1;
    }
    const auto entity_status =
        selected == ObjectId{} ? world::EntityStatus{} : take(world::inspect_entity(runtime, selected));
    const auto transform = selected != ObjectId{} && take(world::has_transform(runtime, selected))
                               ? std::optional(selected)
                               : std::nullopt;
    auto status_text = status;
    if (!render_notice.empty())
        status_text += L"  |  " + render_notice;
    if (!ui_error().empty()) {
        auto diagnostic = application::diagnostic_text(ui_error());
        if (diagnostic.size() > 512) {
            diagnostic.resize(512);
            if (diagnostic.back() >= 0xd800 && diagnostic.back() <= 0xdbff)
                diagnostic.pop_back();
        }
        status_text = L"UI: " + diagnostic;
    }
    const std::string net_graph =
        dock.visible(network_control::tab) ? effective_network_graph(session, project) : current_network_graph(session);
    if (net_graph != last_network_graph_) {
        last_network_graph_ = net_graph;
        if (active_graph.empty()) {
            set_single_graph_selection({});
            inspecting_network = false;
        }
    }
    const auto context = operator_context(session, project, true, command_focus);
    const auto action = [&](OperatorCall call) {
        auto target = context;
        if (call.context.region)
            target.region = *call.context.region;
        if (call.id.starts_with("asset.")) {
            const auto& selected = assets.selected();
            call.context.asset = call.context.asset.value_or(selected ? selected->key : std::string{});
            target.asset = *call.context.asset;
        }
        const auto* info = operators.find(call.id);
        const atelier::Result<void> polled =
            info ? info->poll(target)
                 : atelier::Result<void>{std::unexpected(atelier::Error{atelier::ErrorCode::invalid_definition, {}})};
        const auto bound_key = bind_operator(call);
        auto help = describe_operator_tooltip(property_help_, bound_key, call, info, target, polled);
        return ui_language::ActionBinding{bound_key, polled.has_value(), std::move(help)};
    };
    auto menus = std::make_shared<std::vector<ui_language::Item>>();
    for (const auto& group : menu_groups)
        menus->push_back({group.key, {std::wstring(group.title), menu && menu->source == group.key, group.key}});
    // Houdini status line: the message, then muted scene facts separated by space, not a rule.
    auto facts = std::to_wstring(members.size()) + (members.size() == 1 ? L" Entity" : L" Entities");
    if (play_pending)
        facts += play_diverged   ? L"   Play diverged"
                 : *play_pending ? L"   Play: " + std::to_wstring(*play_pending) + L" pending"
                                 : std::wstring(L"   Play in sync");
    const std::array<ui_language::InputValue, 12> shell_inputs{menu_bar_collapsed,
                                                               ui_language::ItemList{std::move(menus)},
                                                               status_text,
                                                               ui_language::ActionBinding{control::menu_bar_handle},
                                                               double(ui::stow_rail_width),
                                                               double(ui::stow_collapsed_height),
                                                               double(ui::stow_arrow_region_height),
                                                               double(ui::ControlIcon::up),
                                                               double(ui::ControlIcon::down),
                                                               std::move(facts),
                                                               play_diverged,
                                                               action({"play.restart"})};
    const std::array<ui_language::KeyBinding, 8> shell_keys{{{"root", 1},
                                                             {"menubar", 2},
                                                             {"stow", control::menu_bar_handle},
                                                             {"menu_body", control::menu_bar_body},
                                                             {"shelf_slot", 3},
                                                             {"workspace", 20},
                                                             {"footer", 100},
                                                             {"status", 101}}};
    static_cast<void>(ui_document_->append("EditorShell", 0, shell_keys, shell_inputs, result));
    auto create_tools = std::make_shared<std::vector<ui_language::Item>>();
    auto view_tools = std::make_shared<std::vector<ui_language::Item>>();
    for (std::size_t index = 0; index < std::size(menu_bindings); ++index) {
        const auto& definition = menu_bindings[index];
        if (definition.group != MenuGroup::create && definition.group != MenuGroup::view)
            continue;
        if (definition.group == MenuGroup::view && definition.shelf_title.empty())
            continue;
        if (!shelf_items_[index])
            shelf_items_[index] = next_binding_++;
        const auto title = definition.shelf_title.empty() ? definition.title : definition.shelf_title;
        const auto kind_field = *definition.call.args.root().find("kind");
        const auto kind = kind_field ? *kind_field->string() : std::string_view{};
        const auto icon = kind == "point"                  ? ui::ControlIcon::point_light
                          : kind == "rectangle"            ? ui::ControlIcon::area_light
                          : kind == "directional"          ? ui::ControlIcon::sun_light
                          : kind == "mesh"                 ? ui::ControlIcon::mesh
                          : kind == "procedural_generator" ? ui::ControlIcon::mesh
                          : kind == "entity"               ? ui::ControlIcon::object
                                                           : ui::ControlIcon::axes;
        auto& items = definition.group == MenuGroup::create ? create_tools : view_tools;
        items->push_back(
            {shelf_items_[index], {std::wstring(title), static_cast<double>(icon), action(definition.call)}});
    }
    const std::array<ui_language::InputValue, 12> shelf_inputs{shelf_collapsed,
                                                               shelf_page == control::shelf_create,
                                                               ui_language::ItemList{std::move(create_tools)},
                                                               ui_language::ItemList{std::move(view_tools)},
                                                               ui_language::ActionBinding{control::shelf_handle},
                                                               ui_language::ActionBinding{control::shelf_create},
                                                               ui_language::ActionBinding{control::shelf_view},
                                                               static_cast<double>(ui::stow_rail_width),
                                                               static_cast<double>(ui::stow_collapsed_height),
                                                               static_cast<double>(ui::stow_arrow_region_height),
                                                               static_cast<double>(ui::ControlIcon::up),
                                                               static_cast<double>(ui::ControlIcon::down)};
    const std::array<ui_language::KeyBinding, 7> shelf_keys{{{"shelf", control::shelf},
                                                             {"stow", control::shelf_handle},
                                                             {"contents", control::shelf_contents},
                                                             {"tabs", control::shelf_tabs},
                                                             {"create", control::shelf_create},
                                                             {"view", control::shelf_view},
                                                             {"body", control::shelf_body}}};
    static_cast<void>(ui_document_->append("ToolShelf", 3, shelf_keys, shelf_inputs, result));
    // The three authored fixed-height bands define the native dock host extent.
    float chrome_height{};
    for (const auto key : {ui::ControlKey{2}, control::shelf, ui::ControlKey{100}}) {
        const auto chrome = result.controls();
        const auto at = std::ranges::find(chrome, key, &ui::Control::key);
        if (at == chrome.end() || at->layout.height.rule != ui_layout::SizeRule::fixed) {
            if (reject_ui_controls("Editor chrome bands require fixed heights for the dock host."))
                return describe(result, session, size, frame, project, ui, command_focus);
            throw std::runtime_error(std::string(ui_error()));
        }
        chrome_height += at->layout.height.value;
    }
    const auto placement = dock.presentation(20, {size.width, std::max(0.f, size.height - chrome_height)});
    auto panes = std::make_shared<std::vector<ui_language::Item>>();
    for (const auto& pane : placement.panes) {
        auto tabs = std::make_shared<std::vector<ui_language::Item>>();
        for (const auto& tab : pane.tabs) {
            std::wstring style = L"dock-tab";
            for (const auto& name : tab.view->classes)
                style += L" " + std::wstring(name.begin(), name.end());
            tabs->push_back({tab.wrapper,
                             {tab.view->title, tab.view == pane.active, std::move(style), tab.view->key,
                              double(ui::ControlIcon::close)},
                             {{"title", tab.view->key}, {"close", tab.close}}});
        }
        auto slots = std::make_shared<std::vector<ui_language::Item>>();
        if (pane.active)
            slots->push_back({pane.active->content,
                              {pane.active->key != control::scene_tab && pane.active->key != control::parameters_tab &&
                               pane.active->key != network_control::tab}});
        const bool h = pane.horizontal;
        const bool first = pane.collapsed == 1;
        const bool second = pane.collapsed == 2;
        panes->push_back({pane.key,
                          {double(pane.bounds.width),
                           double(pane.bounds.height),
                           double(pane.bounds.x),
                           double(pane.bounds.y),
                           pane.split,
                           ui_language::ItemList{std::move(tabs)},
                           ui_language::ItemList{std::move(slots)},
                           pane.active != nullptr,
                           double(pane.maximized ? ui::ControlIcon::restore : ui::ControlIcon::maximize),
                           double(ui::ControlIcon::plus),
                           double(ui::ControlIcon::down),
                           double(pane.divider.x),
                           double(pane.divider.y),
                           double(pane.divider.width),
                           double(pane.divider.height),
                           !h,
                           pane.value,
                           pane.minimum,
                           pane.maximum,
                           pane.step,
                           double(h ? (first ? ui::ControlIcon::right : ui::ControlIcon::left)
                                    : (first ? ui::ControlIcon::down : ui::ControlIcon::up)),
                           double(h ? (second ? ui::ControlIcon::left : ui::ControlIcon::right)
                                    : (second ? ui::ControlIcon::up : ui::ControlIcon::down)),
                           double(h ? ui::ControlIcon::grip_vertical : ui::ControlIcon::grip_horizontal),
                           first,
                           second,
                           std::wstring(h ? L"dock-divider vertical" : L"dock-divider horizontal"),
                           std::wstring(h ? L"dock-grip vertical" : L"dock-grip horizontal")},
                          {{"tabs.strip", pane.key + 2},
                           {"tabs.tabs", pane.key + 9},
                           {"tabs.add", pane.key + 11},
                           {"tabs.gap", pane.key + 15},
                           {"tabs.maximize", pane.key + 3},
                           {"tabs.menu", pane.key + 4},
                           {"tabs.empty", pane.key + 10},
                           {"divider.rail", pane.key + 15},
                           {"divider.track", pane.key + 1},
                           {"divider.first", pane.key + 12},
                           {"divider.second", pane.key + 13},
                           {"divider.grip", pane.key + 14}},
                          pane.parent});
    }
    const std::array<ui_language::InputValue, 1> dock_inputs{ui_language::ItemList{std::move(panes)}};
    static_cast<void>(ui_document_->append("DockWorkspace", 20, {}, dock_inputs, result));
    for (const auto& pane : placement.panes) {
        if (!pane.active)
            continue;
        const auto& view = *pane.active;
        if (view.key == control::scene_tab) {
            auto modes = std::make_shared<std::vector<ui_language::Item>>();
            constexpr std::array icons{ui::ControlIcon::pointer, ui::ControlIcon::axes, ui::ControlIcon::translate,
                                       ui::ControlIcon::rotate, ui::ControlIcon::scale};
            constexpr std::array tools{"select", "transform", "move", "rotate", "scale"};
            for (std::size_t i = 0; i < icons.size(); ++i) {
                json::Builder args;
                args.set_validated(args.root(), "tool", tools[i]);
                const auto binding = action({"tool.settings", args.finish()});
                modes->push_back(
                    {binding.target,
                     {binding, double(icons[i]), std::size_t(viewport_settings.tool) == i, control::viewport_modes,
                      std::wstring(i == 1 ? L"viewport-mode grouped" : L"viewport-mode")}});
            }
            constexpr std::array mode_names{L"Select", L"Transform", L"Move", L"Rotate", L"Scale"};
            // Bindings as actually routed by Application::viewport_input and ViewportTool.
            constexpr std::array tool_hints{
                L"Left click places the 3D cursor. Right click selects, drag box-selects. Middle tumbles, Shift+Middle pans.",
                L"Drag a handle to move, rotate or scale. G / R / S for modal transforms. Middle tumbles, Shift+Middle pans.",
                L"Drag an arrow or plane to move. G then X/Y/Z constrains, type a value, Enter confirms.",
                L"Drag a ring to rotate. R then X/Y/Z constrains, type degrees, Enter confirms.",
                L"Drag a box to scale an axis, the centre for uniform scale. S then X/Y/Z constrains."};
            const auto open = [&](ui::ControlKey key) { return menu && menu->source == key; };
            // Blender's shading buttons: Wireframe / Solid / MatCap / Scene Lit, routed as the Z pie presets.
            auto shading_modes = std::make_shared<std::vector<ui_language::Item>>();
            const auto shading_selected = this->view.wireframe == renderer::Wireframe::only    ? 0u
                                          : this->view.shading == renderer::Shading::matcap    ? 2u
                                          : this->view.shading == renderer::Shading::scene_lit ? 3u
                                                                                               : 1u;
            constexpr std::array shading_icons{ui::ControlIcon::shade_wire, ui::ControlIcon::shade_solid,
                                               ui::ControlIcon::shade_matcap, ui::ControlIcon::shade_lit};
            constexpr std::array shading_preset_titles{L"Wireframe [Z]", L"Studio Solid [Z]", L"MatCap Shading [Z]",
                                                       L"Scene Lit [Z]"};
            constexpr std::array shading_preset_descs{
                "纯线框模式：隐藏面片着色，仅绘制多边形拓扑轮廓。",
                "工作室平滑着色：使用相机跟随光照明，适合检查模型体积与曲面形态。",
                "MatCap 材质球着色：通过高对比反射球检验法线连续性与细节起伏。",
                "真实场景光照：启用场景直接光、天光与 DDGI 实时全局光照预览。"};
            for (std::size_t i = 0; i < shading_icons.size(); ++i) {
                const auto key = control::shading_preset_first + i;
                auto help = property_help_.get(930 + i, shading_preset_titles[i],
                                               shading_selected == i ? L"Currently: Active" : L"Shading Preset",
                                               shading_preset_descs[i]);
                shading_modes->push_back(
                    {930 + i,
                     {ui_language::ActionBinding{key, true, std::move(help)}, double(shading_icons[i]),
                      shading_selected == i, control::viewport_shading_modes,
                      std::wstring(i == 0                          ? L"viewport-segment-item first"
                                   : i + 1 == shading_icons.size() ? L"viewport-segment-item last"
                                                                   : L"viewport-segment-item")}});
            }
            // Blender's select-mode segment: Set / Extend / Subtract / Invert / Intersect.
            auto select_modes = std::make_shared<std::vector<ui_language::Item>>();
            constexpr std::array select_icons{ui::ControlIcon::select_new, ui::ControlIcon::select_extend,
                                              ui::ControlIcon::select_subtract, ui::ControlIcon::select_invert,
                                              ui::ControlIcon::select_intersect};
            constexpr std::array selections{"set", "extend", "subtract", "invert", "intersect"};
            for (std::size_t i = 0; i < select_icons.size(); ++i) {
                json::Builder args;
                args.set_validated(args.root(), "select_mode", selections[i]);
                select_modes->push_back(
                    {990 + i,
                     {action({"tool.settings", args.finish()}), double(select_icons[i]),
                      std::size_t(viewport_settings.select_mode) == i, control::viewport_select_modes,
                      std::wstring(i == 0                         ? L"viewport-segment-item first"
                                   : i + 1 == select_icons.size() ? L"viewport-segment-item last"
                                                                  : L"viewport-segment-item")}});
            }
            // Resolve active procedural geometry for both SOP context and World ProceduralGenerator inspection.
            const bool in_sop = !active_graph.empty() && session.has_graph(active_graph);
            const ObjectId gen_comp = in_sop ? ObjectId{} : active_procedural_generator(session);
            if (gen_comp != ObjectId{} && !session.world().running())
                static_cast<void>(session.sync_procedural_generators(project));
            const procedural::Geometry* shown_geom{};
            std::wstring hud_title;
            std::optional<double> hud_cook_ms;
            std::size_t hud_chunks{};
            if (in_sop) {
                if (const auto eval = session.evaluate_graph(active_graph, project)) {
                    shown_geom = eval->preview_output.get();
                    hud_title = eval->display_name.empty()
                                    ? graph_path_title(project)
                                    : platform::utf8_to_utf16_validated(eval->display_name) + L" [Display]";
                    if (const auto insp = session.graph_topology(active_graph); insp && !eval->display_name.empty())
                        if (const auto* dn = (*insp)->find_node(eval->display_name);
                            dn && dn->status && dn->status->time)
                            hud_cook_ms = static_cast<double>(dn->status->time->count()) * 1e-6;
                }
            } else if (gen_comp != ObjectId{}) {
                if (const auto src = presentation::read_procedural_source(runtime, gen_comp);
                    src && src->output && src->output->geometry) {
                    shown_geom = src->output->geometry.get();
                    hud_chunks = src->output->publication.collections.size();
                    hud_title = std::wstring(session.name(selected)) + L" [Generator]";
                }
            }
            // Houdini display bar: each toggle mirrors the one ViewSettings / procedural guide value it edits.
            auto display_toggles = std::make_shared<std::vector<ui_language::Item>>();
            const auto lit = this->view.shading == renderer::Shading::scene_lit;
            struct DisplayToggleDef {
                ui::ControlKey key;

                ui::ControlIcon icon;

                bool selected;

                bool grouped;

                const wchar_t* title;

                const char* desc;
            };
            const std::array<DisplayToggleDef, 9> display{
                {{control::display_grid, ui::ControlIcon::grid, this->view.reference_grid, false, L"Reference Grid",
                  "显示或隐藏水平地面参考网格与公制刻度线。"},
                 {control::display_axes, ui::ControlIcon::axes_origin, this->view.origin_axes, false, L"Origin Axes",
                  "显示或隐藏世界原点处的红（X）、绿（Y）、蓝（Z）基准坐标轴。"},
                 {control::display_wire, ui::ControlIcon::wire_overlay,
                  this->view.wireframe == renderer::Wireframe::overlay, true, L"Wireframe Overlay",
                  "在实体着色表面之上叠加绘制多边形线框（Shaded Wireframe）。"},
                 {control::display_wire_only, ui::ControlIcon::shade_wire,
                  this->view.wireframe == renderer::Wireframe::only, false, L"Wireframe Only",
                  "切换为纯线框显示模式，透视查看内部网格拓扑结构。"},
                 {control::display_triangles, ui::ControlIcon::mesh, this->view.triangle_edges, false,
                  L"Triangle Edges", "在线框中额外显示多边形三角化后的内部对角线，便于检查三角剖分质量。"},
                 {control::display_backfaces, ui::ControlIcon::backface_tint, this->view.backface_tint && !lit, false,
                  L"Backface Winding Tint",
                  "在工作室着色下以高对比青蓝色高亮背面三角形（Winding Order），用于快速排查法线翻转或绕序错误。"},
                 {control::display_cavity, ui::ControlIcon::shade_matcap, this->view.cavity.enabled && !lit, true,
                  L"Screen-Space Cavity", "在工作室着色下增强棱角高光与凹缝暗部，提升几何体轮廓体积感。"},
                 {control::display_lit, ui::ControlIcon::shade_lit, lit, false, L"Scene Lighting",
                  "在工作室默认照明与真实场景光照（直接光 + 天光 + DDGI）之间一键切换。"},
                 {control::display_fps, ui::ControlIcon::fps, show_fps, true, L"Frame Rate (FPS)",
                  "在视口左上角显示平滑后的实时帧率与帧耗时读数。"}}};
            for (std::size_t index = 0; const auto& item : display) {
                const auto button_key = control::display_toggle_first + index++;
                ui_language::ActionBinding binding =
                    item.key == control::display_fps
                        ? action({"view.toggle", json::Document{json::Document::literal<R"({"option":"fps"})">()}})
                        : ui_language::ActionBinding{
                              item.key, true,
                              property_help_.get(button_key, item.title,
                                                 item.selected ? L"Currently: On" : L"Currently: Off", item.desc)};
                display_toggles->push_back(
                    {button_key,
                     {std::move(binding), double(item.icon), item.selected,
                      std::wstring(item.grouped ? L"display-toggle grouped" : L"display-toggle")}});
            }
            if (in_sop || gen_comp != ObjectId{}) {
                const bool pts_effective = effective_show_points(shown_geom);
                const bool pts_auto =
                    !sop_show_points.has_value() && shown_geom && application::is_pure_point_cloud(*shown_geom);
                const std::array<DisplayToggleDef, 6> sop_display{
                    {{control::display_sop_points, ui::ControlIcon::points, pts_effective, true, L"Display Points",
                      "在视口中绘制程序化点云标记（支持 Cd 点颜色与半球深度消隐）。纯点云节点默认自动开启，点击后切换为手动开关。"},
                     {control::display_sop_normals, ui::ControlIcon::point_normals, sop_show_normals, false,
                      L"Display Point Normals (N)",
                      "在每个采样点处绘制法线向量（N）青色线段，用于检查散点朝向与曲面插值法线。"},
                     {control::display_sop_face_normals, ui::ControlIcon::face_normals, sop_show_face_normals, false,
                      L"Display Primitive Normals",
                      "在多边形图元重心处绘制琥珀色面法线向量，直观验证多边形绕序与挤出朝向。"},
                     {control::display_sop_orient, ui::ControlIcon::orient_scale, sop_show_orient, false,
                      L"Display Orient & pscale",
                      "绘制点与实例的局部三轴旋转架（orient）及缩放半径圆环（pscale），用于调试 CopyToPoints 散布姿态。"},
                     {control::display_sop_bounds, ui::ControlIcon::bounds_cage, sop_show_bounds, false,
                      L"Display Bounds & Chunks",
                      "绘制程序化几何体总包围盒（AABB）以及生成器在局部空间划分的实例分块（Spatial Chunks）网格。"},
                     {control::display_sop_info, ui::ControlIcon::info, sop_show_info, true, L"Geometry Info HUD",
                      "在视口右上角显示当前程序化几何体的元素计数、尺寸范围、属性列表与求值耗时卡片。"}}};
                std::size_t sop_index = 0;
                for (const auto& item : sop_display) {
                    const auto button_key = control::display_sop_toggle_first + sop_index++;
                    const std::wstring state_label = item.key == control::display_sop_points && pts_auto
                                                         ? L"Currently: Auto-On (Point Cloud)"
                                                         : (item.selected ? L"Currently: On" : L"Currently: Off");
                    auto help = property_help_.get(button_key, item.title, state_label, item.desc);
                    display_toggles->push_back(
                        {button_key,
                         {ui_language::ActionBinding{item.key, true, std::move(help)}, double(item.icon), item.selected,
                          std::wstring(item.grouped ? L"display-toggle grouped" : L"display-toggle")}});
                }
            }
            const auto step = viewport_settings.grid_spacing;
            const std::wstring step_name = step == .1f   ? L"0.1 m"
                                           : step == .2f ? L"0.2 m"
                                           : step == .5f ? L"0.5 m"
                                                         : L"1 m";
            const std::array<ui_language::InputValue, 50> inputs{
                ui_language::ActionBinding{control::scene_back, can_navigate(-1)},
                ui_language::ActionBinding{control::scene_forward, can_navigate(1)},
                double(ui::ControlIcon::back),
                double(ui::ControlIcon::forward),
                double(ui::ControlIcon::world),
                double(ui::ControlIcon::down),
                double(ui::ControlIcon::overlays),
                menu && menu->source == control::scene_shading,
                menu && menu->source == control::scene_guides,
                ui_language::ItemList{std::move(modes)},
                double(ui::ControlIcon::cursor3d),
                menu && menu->source == control::viewport_pivot,
                std::wstring(camera_flying ? L"Fly" : mode_names[std::size_t(viewport_mode)]),
                double(camera_flying ? ui::ControlIcon::fly : icons[std::size_t(viewport_mode)]),
                std::wstring(viewport_mode == TransformTool::scale ||
                                     viewport_settings.orientation == Orientation::local
                                 ? L"Local"
                                 : L"Global"),
                std::wstring(viewport_settings.pivot == PivotMode::median ? L"Selection Center" : L"3D Cursor"),
                action({"tool.toggle", json::Document{json::Document::literal<R"({"setting":"grid_snap"})">()}}),
                double(ui::ControlIcon::magnet),
                viewport_settings.grid_snap,
                step_name,
                menu && menu->source == control::viewport_snap,
                viewport_settings.cursor_surface,
                action({"tool.toggle", json::Document{json::Document::literal<R"({"setting":"cursor_surface"})">()}}),
                std::wstring(camera_view_name(camera_view)),
                menu && menu->source == control::viewport_camera,
                action({"view.toggle", json::Document{json::Document::literal<R"({"option":"options"})">()}}),
                viewport_options_collapsed,
                double(ui::ControlIcon::up),
                double(ui::ControlIcon::down),
                viewport_readout,
                !viewport_readout.empty(),
                ui_language::ActionBinding{
                    control::viewport_rail_snap, true,
                    property_help_.get(control::viewport_rail_snap, L"Grid Snapping",
                                       viewport_settings.grid_snap ? L"Currently: On" : L"Currently: Off",
                                       "左键切换网格步进吸附；右键打开吸附步长与游标对齐菜单。")},
                std::wstring(
                    camera_flying
                        ? L"WASD move · Q/E down/up · Wheel speed · Shift fast · Enter/LMB confirm · Esc/RMB cancel"
                    : !active_graph.empty()
                        ? L"SOP Context · Middle tumbles · Shift+Middle pans · Wheel zooms · F/Home frames SOP · World exits"
                        : tool_hints[std::size_t(viewport_mode)]),
                std::wstring(this->view.shading == renderer::Shading::scene_lit ||
                                     this->view.background == renderer::Background::none
                                 ? L"viewport-tool-hint dark-background"
                                 : L"viewport-tool-hint"),
                menu && menu->source == control::viewport_cavity,
                show_fps,
                viewport_fps.empty() ? std::wstring(L"FPS —") : viewport_fps,
                camera_flying,
                ui_language::ItemList{std::move(shading_modes)},
                ui_language::ItemList{std::move(select_modes)},
                open(control::viewport_view_menu),
                open(control::viewport_select_menu),
                open(control::viewport_header_add),
                open(control::viewport_object_menu),
                open(control::viewport_tools_menu),
                pane.bounds.width < 660,
                ui_language::ItemList{std::move(display_toggles)},
                double(ui::ControlIcon::axes_origin),
                open(control::viewport_orientation),
                graph_path_title(project)};
            const std::array<ui_language::KeyBinding, 28> keys{{{"navigation", 641},
                                                                {"path", control::scene_path},
                                                                {"back", control::scene_back},
                                                                {"readout", control::viewport_readout},
                                                                {"fps", control::viewport_fps},
                                                                {"fly_reticle", control::viewport_fly_reticle},
                                                                {"tool_hint", control::viewport_hint},
                                                                {"cavity", control::viewport_cavity},
                                                                {"forward", control::scene_forward},
                                                                {"display", control::scene_shading},
                                                                {"guides", control::scene_guides},
                                                                {"viewport", control::viewport},
                                                                {"scene", control::viewport_scene_overlay},
                                                                {"pivot", control::viewport_pivot},
                                                                {"edit_tools", control::viewport_modes},
                                                                {"actions", control::viewport_tools_menu},
                                                                {"step", control::viewport_snap},
                                                                {"camera", control::viewport_camera},
                                                                {"mode", control::viewport_hud},
                                                                {"view_menu", control::viewport_view_menu},
                                                                {"select_menu", control::viewport_select_menu},
                                                                {"add_menu", control::viewport_header_add},
                                                                {"object_menu", control::viewport_object_menu},
                                                                {"shading_modes", control::viewport_shading_modes},
                                                                {"selection_modes", control::viewport_select_modes},
                                                                {"snapping", control::viewport_snapping},
                                                                {"display_bar", control::display_bar},
                                                                {"basis", control::viewport_orientation}}};
            static_cast<void>(ui_document_->append("SceneViewport", control::scene_content, keys, inputs, result));
            if (sop_show_info && shown_geom && (in_sop || gen_comp != ObjectId{})) {
                const bool pts_auto = !sop_show_points.has_value() && application::is_pure_point_cloud(*shown_geom);
                ui::Control hud;
                hud.key = control::viewport_geometry_hud;
                hud.parent = control::viewport_scene_overlay;
                hud.kind = ui::ControlKind::label;
                hud.enabled = false;
                hud.text =
                    application::format_geometry_info_hud(hud_title, *shown_geom, pts_auto, hud_cook_ms, hud_chunks);
                hud.layout.out_of_flow = true;
                hud.layout.anchor_x = 1.f;
                hud.layout.pivot_x = 1.f;
                hud.layout.offset_x = -10.f;
                hud.layout.offset_y = 40.f;
                hud.layout.width = ui_layout::Axis::fixed(264.f);
                hud.layout.height = ui_layout::Axis::content(0.f);
                hud.layout.padding = {9.f, 7.f, 9.f, 7.f};
                hud.style.background = ui::Color{.018f, .022f, .028f, .86f};
                hud.style.border = ui::Color{.22f, .28f, .34f, .85f};
                hud.style.border_width = 1.f;
                hud.style.radius = {5.f, 5.f, 5.f, 5.f};
                hud.style.font_size = 11.5f;
                hud.style.foreground = ui::Color{.88f, .91f, .94f, 1.f};
                result.append(std::span{&hud, 1}, 0);
            }
            continue;
        }
        if (view.key == source_control::tab) {
            source_editor.append(result, view.content, *ui_document_, action, project, project_build.get(),
                                 project_running);
            continue;
        }
        if (view.key == control::output_tab) {
            auto appended = output.append(result, view.content, *ui_document_, pane.bounds.height - 22);
            if (!appended) {
                auto error = application::ui_error(appended.error());
                reject_ui_controls(error.message);
                property_help_.end();
                return std::unexpected(std::move(error));
            }
            continue;
        }
        if (view.key == control::workbench) {
            workbench.append(result, view.content, *ui_document_);
            continue;
        }
        if (view.key == world_control::tab) {
            const auto selected = session.main_camera();
            const auto name = selected && *selected != ObjectId{} ? session.name(*selected) : std::wstring_view{};
            const auto label = !selected                 ? L"Unavailable"
                               : *selected == ObjectId{} ? L"None"
                               : name.empty()            ? L"Missing Entity"
                                                         : std::wstring(name);
            lighting.append(result, view.content, *ui_document_, this->view, !session.world().running(), label,
                            action({"scene.main_camera_clear"}));
            continue;
        }
        if (view.key == control::assets_tab) {
            assets.append(result, view.content, project, operators.find("scene.add")->poll(context).has_value(), frame,
                          *ui_document_);
            continue;
        }
        if (view.key == spreadsheet_control::tab) {
            const procedural::Geometry* geom{};
            std::uint64_t generation{};
            std::wstring target_label = L"No active graph";
            std::wstring error_text;
            const bool in_sop = !active_graph.empty() && session.has_graph(active_graph);
            const bool follow_world_node = !in_sop && !net_graph.empty() && session.has_graph(net_graph) &&
                                           spreadsheet.follow_selected() && inspecting_network &&
                                           !selected_graph_node.empty();
            if (in_sop || follow_world_node) {
                const std::string& sheet_graph = in_sop ? active_graph : net_graph;
                const ObjectId target_node = spreadsheet.follow_selected() && !selected_graph_node.empty()
                                                 ? session.graph_node_id(sheet_graph, selected_graph_node)
                                                 : ObjectId{};
                const auto eval = session.evaluate_graph(sheet_graph, project, target_node);
                if (!eval)
                    error_text = application::diagnostic_text(eval.error().message);
                else if (target_node != ObjectId{}) {
                    target_label = platform::utf8_to_utf16_validated(selected_graph_node);
                    geom = eval->inspected_output;
                    generation = eval->generation;
                    if (eval->inspected_error)
                        error_text = application::diagnostic_text(eval->inspected_error->message);
                } else {
                    target_label = eval->display_name.empty()
                                       ? L"(no display)"
                                       : platform::utf8_to_utf16_validated(eval->display_name) + L" [D]";
                    geom = eval->preview_output.get();
                    generation = eval->generation;
                    if (eval->display_error)
                        error_text = application::diagnostic_text(eval->display_error->message);
                }
            } else if (const ObjectId gen_comp = active_procedural_generator(session); gen_comp != ObjectId{}) {
                if (!session.world().running())
                    static_cast<void>(session.sync_procedural_generators(project));
                if (const auto src = presentation::read_procedural_source(runtime, gen_comp);
                    src && src->output && src->output->geometry) {
                    geom = src->output->geometry.get();
                    generation = src->output->generation;
                    target_label = std::wstring(session.name(selected)) + L" [Generator]";
                } else if (src && !src->source.empty()) {
                    target_label = platform::utf8_to_utf16_validated(src->source) + L" (no output)";
                }
            }
            spreadsheet.append(result, view.content, *ui_document_, geom, generation, target_label, error_text);
            continue;
        }
        if (view.key == network_control::tab) {
            const bool has_graph = !net_graph.empty() && session.has_graph(net_graph);
            const auto* graph_doc = has_graph ? session.graph_document(net_graph) : nullptr;
            auto tab_items = std::make_shared<std::vector<ui_language::Item>>();
            std::wstring cook_summary;
            std::wstring cook_summary_style = L"network-cook-summary";
            std::wstring net_status;
            std::shared_ptr<const GraphInspectView> inspected;
            std::vector<GraphNodeCatalogEntry> catalog;
            if (graph_doc) {
                // Prune any node names removed via Undo or external Control calls.
                std::erase_if(selected_graph_nodes,
                              [&](const std::string& n) { return !session.graph_node(net_graph, n); });
                if (!selected_graph_node.empty() && !session.graph_node(net_graph, selected_graph_node))
                    selected_graph_node = selected_graph_nodes.empty() ? std::string{} : selected_graph_nodes.back();

                const ObjectId sel_node =
                    !selected_graph_node.empty() ? session.graph_node_id(net_graph, selected_graph_node) : ObjectId{};
                const auto eval = session.evaluate_graph(net_graph, project, sel_node);
                if (auto insp = session.graph_topology(net_graph))
                    inspected = std::move(*insp);
                if (auto cat = session.procedural_node_types())
                    catalog = std::move(*cat);

                if (network.tab_search().open) {
                    const auto matches = network.filter_catalog(catalog);
                    tab_items->reserve(matches.size());
                    for (std::size_t i = 0; i < matches.size(); ++i) {
                        std::string short_name = matches[i].name;
                        if (const auto dot = short_name.find_last_of('.'); dot != short_name.npos)
                            short_name = short_name.substr(dot + 1);
                        const auto short_w = platform::utf8_to_utf16_validated(short_name);
                        const auto pick_key = network_control::tab_pick_first + i;
                        auto pick_help =
                            property_help_.get(pick_key, short_w,
                                               platform::utf8_to_utf16_validated(matches[i].name) +
                                                   (matches[i].native ? L" · C++ SOP" : L" · Source SOP"),
                                               matches[i].description.empty() ? "在画布光标位置创建此程序化几何节点。"
                                                                              : matches[i].description);
                        const bool active_row = i == std::min(network.tab_search().highlight, matches.size() - 1);
                        tab_items->push_back(
                            {network_control::tab_item_first + i,
                             {short_w, std::wstring(matches[i].native ? L"C++" : L"Source"), active_row,
                              ui_language::ActionBinding{pick_key, !session.world().running(), std::move(pick_help)},
                              std::wstring(active_row ? L"network-tab-row-active" : L"")}});
                    }
                }

                const std::size_t wire_count = inspected ? inspected->connections.size() : 0u;
                if (!eval) {
                    cook_summary = L"Cook Error";
                    cook_summary_style = L"network-cook-error";
                    net_status = L"Cook error: " + application::diagnostic_text(eval.error().message);
                } else if (eval->display_error) {
                    cook_summary =
                        std::format(L"Cook Error · '{}'", platform::utf8_to_utf16_validated(eval->display_name));
                    cook_summary_style = L"network-cook-error";
                    net_status = L"Cook error: " + application::diagnostic_text(eval->display_error->message);
                } else if (const auto* shown = eval->preview_output.get()) {
                    const std::size_t pts = shown->count(procedural::Layer::point);
                    const std::size_t prims = shown->count(procedural::Layer::primitive);
                    const std::size_t insts = shown->count(procedural::Layer::instance);
                    cook_summary =
                        insts > 0
                            ? std::format(L"{} nodes · {} wires · {} ({} inst)", graph_doc->nodes.size(), wire_count,
                                          platform::utf8_to_utf16_validated(eval->display_name), insts)
                            : std::format(L"{} nodes · {} wires · {} ({} pts, {} prims)", graph_doc->nodes.size(),
                                          wire_count, platform::utf8_to_utf16_validated(eval->display_name), pts,
                                          prims);
                    net_status = std::format(
                        L"Display '{}': {} pts · {} prims · {} inst   (Tab add · D/T/B flags · C/Z style · N note · O box · Y+drag cut)",
                        platform::utf8_to_utf16_validated(eval->display_name), pts, prims, insts);
                } else {
                    cook_summary =
                        std::format(L"{} nodes · {} wires · No Display", graph_doc->nodes.size(), wire_count);
                    net_status = std::format(
                        L"{} nodes · No Display Flag set   (Press D or hover Air-Ring to display · Tab add · C/Z style · H frame)",
                        graph_doc->nodes.size());
                }
            }
            const bool can_edit_net = has_graph && !session.world().running();
            const bool has_selected_node = has_graph && (!selected_graph_node.empty() || !selected_graph_nodes.empty());
            const bool is_isolated = !active_graph.empty() && active_graph == net_graph;
            const auto popup_kind = network.popup().kind;
            const ui::ControlIcon path_icon_val = is_isolated      ? ui::ControlIcon::isolate
                                                  : network_pinned ? ui::ControlIcon::pin
                                                                   : ui::ControlIcon::world;
            const float dscale = frame && frame->drawing ? frame->drawing->dpi_scale : 1.f;
            const float pane_dip_w = pane.bounds.width > 0.f ? (pane.bounds.width / dscale) : size.width * .38f;
            const bool net_compact = pane_dip_w < 380.f;

            const auto net_action = [&](OperatorCall call) {
                call.context.region = RegionKind::network;
                return action(std::move(call));
            };
            json::Builder seed_args;
            seed_args.set_validated(seed_args.root(), "node", "");
            auto del_binding = net_action({"graph.delete_node"});
            del_binding.enabled = del_binding.enabled && has_selected_node;

            const std::wstring graph_title = network_path_title(session, project, net_graph);
            const std::wstring zoom_text = std::format(L"{}%", static_cast<int>(std::lround(network.zoom() * 100.f)));
            const auto& ts = network.tab_search();
            const std::array<ui_language::InputValue, 53> inputs{
                graph_title,
                double(path_icon_val),
                ui_language::ActionBinding{network_control::nav_back, can_navigate(-1)},
                ui_language::ActionBinding{network_control::nav_forward, can_navigate(1)},
                double(ui::ControlIcon::back),
                double(ui::ControlIcon::forward),
                network_pinned,
                double(ui::ControlIcon::pin),
                ui_language::ActionBinding{
                    network_control::pin_toggle, has_graph || network_pinned,
                    property_help_.get(
                        network_control::pin_toggle, L"Pin Network Tab",
                        network_pinned ? L"Currently: Pinned" : L"Currently: Follow Selection",
                        "锁定当前节点网络面板：开启后切换场景实体时保持当前程序化图不变；关闭时点击任意程序化生成器实体即自动显示其节点网络。")},
                is_isolated,
                double(ui::ControlIcon::isolate),
                ui_language::ActionBinding{
                    network_control::isolate_toggle, has_graph || !active_graph.empty(),
                    property_help_.get(
                        network_control::isolate_toggle, L"Isolate SOP in 3D Viewport",
                        is_isolated ? L"Currently: Isolated SOP View" : L"Currently: In-World Scene View",
                        "在 3D 视口独显当前 SOP 图（局部原点检查模式）与完整 World 场景光照实时预览之间一键切换。")},
                has_graph,
                net_compact,
                menu && menu->source == network_control::menu_add,
                menu && menu->source == network_control::menu_edit,
                menu && menu->source == network_control::menu_view,
                menu && menu->source == network_control::menu_layout,
                std::move(cook_summary),
                std::move(cook_summary_style),
                ui_language::ActionBinding{
                    network_control::new_scratch, !session.world().running(),
                    property_help_.get(network_control::new_scratch, L"New Scratch Graph", L"Procedural Network",
                                       "在内存中创建一张新的临时程序化图（Scratch Graph）并进入独显 SOP 编辑模式。")},
                ui_language::ActionBinding{
                    network_control::add_node, can_edit_net,
                    property_help_.get(
                        network_control::add_node, L"Add SOP Node [Tab / Shift+A]", L"Network Create",
                        "打开支持分类筛选与首字母缩写的节点检索面板（如输入 ctp 快速创建 CopyToPoints）。")},
                double(ui::ControlIcon::plus),
                ui_language::ActionBinding{
                    network_control::add_sticky_note, can_edit_net,
                    property_help_.get(
                        network_control::add_sticky_note, L"Add Sticky Note [N]", L"Network Annotation",
                        "在画布中插入黄色便签注释块（支持拖动、右下角缩放，双击标题栏可编辑标题与多行正文）。")},
                double(ui::ControlIcon::sticky_note),
                ui_language::ActionBinding{
                    network_control::add_network_box, can_edit_net,
                    property_help_.get(
                        network_control::add_network_box, L"Add Network Box [O]", L"Network Annotation",
                        "创建分组背景框（若已选中节点会自动包裹它们）；拖动分组框标题栏可将内部节点与便签整组同步移动。")},
                double(ui::ControlIcon::network_box),
                popup_kind == NetworkCanvas::PopupKind::color_picker,
                ui_language::ActionBinding{
                    network_control::open_color_picker, can_edit_net,
                    property_help_.get(
                        network_control::open_color_picker, L"Color Palette [C]", L"Node Appearance",
                        "打开 24 色调色板自定义选中节点或注释的主题颜色（支持批量多选着色与一键恢复角色默认色）。")},
                double(ui::ControlIcon::palette),
                popup_kind == NetworkCanvas::PopupKind::shape_picker,
                ui_language::ActionBinding{
                    network_control::open_shape_picker, can_edit_net,
                    property_help_.get(
                        network_control::open_shape_picker, L"Shape Palette [Z]", L"Node Appearance",
                        "打开 8 种几何轮廓形状预设面板（Pill / Rect / Domed / Trap / Hexagon / Ribbon / Diamond / Circle）。")},
                double(ui::ControlIcon::node_shapes),
                popup_kind == NetworkCanvas::PopupKind::node_info,
                ui_language::ActionBinding{
                    network_control::open_node_info, has_selected_node,
                    property_help_.get(
                        network_control::open_node_info, L"Node Info & Comment [I]", L"Node Diagnostics",
                        "查看当前选中节点的类型、端口数、求值耗时、缓存命中、布尔统计，并编辑节点备注标签。")},
                double(ui::ControlIcon::info),
                net_action({"graph.layout"}),
                double(ui::ControlIcon::network_layout),
                ui_language::ActionBinding{
                    network_control::frame_all, has_graph,
                    property_help_.get(network_control::frame_all, L"Frame Network [H / Home]", L"Network View",
                                       "平移并缩放画布视口，使整张程序化图中的所有节点与注释完整居中显示。")},
                double(ui::ControlIcon::bounds_cage),
                zoom_text,
                ui_language::ActionBinding{network_control::zoom_reset, has_graph,
                                           property_help_.get(network_control::zoom_reset, L"Reset Canvas Zoom (100%)",
                                                              L"Network View",
                                                              "将二维节点画布的缩放倍率重置为 100% 标准大小。")},
                has_graph && selected_graph_node.empty() && (is_isolated || inspecting_network),
                net_action({"graph.select", seed_args.finish()}),
                double(ui::ControlIcon::sliders),
                std::move(del_binding),
                double(ui::ControlIcon::trash),
                has_graph && ts.open,
                static_cast<double>(ts.dip_x),
                static_cast<double>(ts.dip_y),
                ui_language::TextEditBinding{network_control::tab_search_input, ts.query, has_graph && ts.open},
                ui_language::ItemList{std::move(tab_items)},
                std::move(net_status)};
            const std::array<ui_language::KeyBinding, 17> keys{{{"toolbar", network_control::toolbar_row},
                                                                {"back", network_control::nav_back},
                                                                {"forward", network_control::nav_forward},
                                                                {"path", network_control::path_combo},
                                                                {"pin_btn", network_control::pin_toggle},
                                                                {"isolate_btn", network_control::isolate_toggle},
                                                                {"action_bar", network_control::action_bar},
                                                                {"add_menu", network_control::menu_add},
                                                                {"edit_menu", network_control::menu_edit},
                                                                {"view_menu", network_control::menu_view},
                                                                {"layout_menu", network_control::menu_layout},
                                                                {"add_node", network_control::add_node},
                                                                {"note_btn", network_control::add_sticky_note},
                                                                {"box_btn", network_control::add_network_box},
                                                                {"frame_btn", network_control::frame_all},
                                                                {"canvas", network_control::canvas},
                                                                {"tab_box", network_control::tab_search_box}}};
            std::vector<ui::Control> network_controls;
            static_cast<void>(ui_document_->append("NetworkPanel", view.content, keys, inputs, network_controls));
            if (has_graph && inspected) {
                ui_layout::Size canvas_dip{std::max(160.f, size.width * .36f), std::max(120.f, size.height * .26f)};
                if (frame) {
                    if (const auto* box = frame->find(network_control::canvas);
                        box && box->bounds.width > 0.f && box->bounds.height > 0.f) {
                        canvas_dip = {box->bounds.width / dscale, box->bounds.height / dscale};
                    }
                }
                std::vector<ui::Control> canvas_controls;
                network.append_canvas(canvas_controls, network_control::canvas, canvas_dip, inspected, catalog,
                                      selected_graph_node, selected_graph_nodes);
                const auto tab_it =
                    std::ranges::find(network_controls, network_control::tab_search_box, &ui::Control::key);
                network_controls.insert(tab_it, std::make_move_iterator(canvas_controls.begin()),
                                        std::make_move_iterator(canvas_controls.end()));
                network.append_tab_categories(network_controls, network_control::tab_search_box);
            }
            result.append(std::move(network_controls));
            continue;
        }
        if (view.key == control::entities_tab) {
            // Every member is flattened each frame; only rows near the viewport are described.
            auto& tree = scene_tree_;
            tree.nodes.clear();
            tree.objects.clear();
            tree.nodes.push_back({ui::no_tree_node, tree.expanded(ObjectId{}, true), L"World"});
            tree.objects.push_back({});
            std::vector<std::uint32_t> path{0}; // Members are in composition preorder.
            for (const auto& member : members) {
                const auto parent = take(world::parent_entity(runtime, member.id()));
                while (path.size() > 1 && tree.objects[path.back()] != parent)
                    path.pop_back();
                path.push_back(static_cast<std::uint32_t>(tree.nodes.size()));
                tree.nodes.push_back({path[path.size() - 2], tree.expanded(member.id(), false),
                                      tree_query.empty() ? std::wstring_view{} : session.name(member.id())});
                tree.objects.push_back(member.id());
            }
            const auto* focused = ui ? binding(ui->focused_control()) : nullptr;
            const std::array pinned{selected,
                                    focused && focused->kind == Binding::Kind::object ? focused->object : selected,
                                    row_rename && row_rename->object != ObjectId{} ? row_rename->object : selected};
            auto window = window_tree(
                tree, selected, tree_query,
                ui ? ui->tree_viewport(control::entity_tree) : ui::TreeViewport{0, size.height, 12}, pinned);
            // World stays when the filter matches nothing.
            if (tree.rows.empty()) {
                tree.rows.push_back({0, ui::no_tree_node, 0, false, false, {}});
                window = {0};
            }
            auto rows = std::make_shared<std::vector<ui_language::Item>>();
            rows->reserve(window.size());
            Set<ObjectId> selected_entities;
            selected_entities.reserve(session.selected_entities().size());
            for (const auto entity : session.selected_entities())
                selected_entities.add(entity);
            ui_language::TreeRowBinding world_row;
            for (const auto index : window) {
                const auto& row = tree.rows[index];
                const auto entity = tree.objects[row.node];
                const ui_language::TreeRowBinding position{index,        row.depth,          row.branch,
                                                           row.expanded, entity == selected, row.lines};
                if (!row.node) {
                    world_row = position;
                    continue;
                }
                const auto state = take(world::inspect_entity(runtime, entity));
                const auto entity_key = bind(entity);
                const bool mesh_entity =
                    take(runtime.is_a(state.type, session.presentation_types().static_mesh_entity));
                const bool retained =
                    session.has_retained(entity) ||
                    std::ranges::any_of(take(world::components(runtime, entity)),
                                        [&](const auto& component) { return session.has_retained(component.id()); });
                rows->push_back(
                    {entity_key,
                     {std::wstring(session.name(entity)),
                      state.state == world::EntityState::attached_inactive || state.state == world::EntityState::active,
                      selected_entities.contains(entity), static_cast<double>(entity_icon(session, entity)), position,
                      std::wstring(mesh_entity ? L"mesh-entity" : L"entity") + (retained ? L" retained" : L"") +
                          (tree_drop_row == entity_key ? L" drop-target" : L""),
                      state.can_edit, entity_key}});
            }
            const std::array<ui_language::InputValue, 12> inputs{
                ui_language::ItemList{std::move(rows)},
                static_cast<double>(tree.rows.size()),
                ui_language::TextEditBinding{control::tree_filter, tree_query},
                selected == ObjectId{},
                world_row,
                static_cast<double>(ui::ControlIcon::world),
                ui_language::ActionBinding{control::entity_back, can_navigate(-1)},
                ui_language::ActionBinding{control::entity_forward, can_navigate(1)},
                double(ui::ControlIcon::back),
                double(ui::ControlIcon::forward),
                double(ui::ControlIcon::world),
                std::wstring(tree_drop_row == control::tree_world ? L"drop-target" : L"")};
            const std::array<ui_language::KeyBinding, 6> keys{{{"tree", control::entity_tree},
                                                               {"world", control::tree_world},
                                                               {"path.bar", 24},
                                                               {"path.path", control::entity_path},
                                                               {"path.back", control::entity_back},
                                                               {"path.forward", control::entity_forward}}};
            static_cast<void>(ui_document_->append("SceneTree", 21, keys, inputs, result));
            continue;
        }
        const auto short_type = [&](ObjectId object) {
            auto name = take(runtime.describe(take(runtime.type_of(object))))->name;
            if (const auto dot = name.find_last_of('.'); dot != name.npos)
                name = name.substr(dot + 1);
            return platform::utf8_to_utf16_validated(name);
        };
        // Placeholders show the saved type they stand in for.
        const auto object_title = [&](ObjectId object) {
            if (session.has_retained(object))
                if (const auto saved = session.retained(object); saved && !saved->type.empty()) {
                    auto name = saved->type;
                    if (const auto dot = name.find_last_of('.'); dot != name.npos)
                        name = name.substr(dot + 1);
                    return platform::utf8_to_utf16_validated(name) + L" (missing type)";
                }
            return short_type(object);
        };
        if (view.key == control::structure_tab) {
            auto& tree = structure_tree_;
            build_structure_tree(session, selected);
            tree.texts.clear();
            if (!structure_query.empty()) {
                tree.texts.reserve(tree.objects.size());
                for (const auto object : tree.objects)
                    tree.texts.push_back(session.world().contains(object) ? std::wstring(session.name(object))
                                                                          : object_title(object));
                for (std::size_t i = 0; i < tree.nodes.size(); ++i)
                    tree.nodes[i].text = tree.texts[i];
            }
            const auto* focused = ui ? binding(ui->focused_control()) : nullptr;
            const std::array pinned{inspected,
                                    focused && focused->kind == Binding::Kind::structure ? focused->object : inspected};
            const auto window = window_tree(
                tree, inspected, structure_query,
                ui ? ui->tree_viewport(control::structure_tree) : ui::TreeViewport{0, size.height, 12}, pinned);
            auto rows = std::make_shared<std::vector<ui_language::Item>>();
            rows->reserve(window.size());
            for (const auto index : window) {
                const auto& row = tree.rows[index];
                const auto object = tree.objects[row.node];
                const ui_language::TreeRowBinding position{index,        row.depth,           row.branch,
                                                           row.expanded, object == inspected, row.lines};
                const auto key = bind(object, {}, Binding::Kind::structure);
                if (session.world().contains(object)) {
                    const auto state = take(world::inspect_entity(runtime, object));
                    rows->push_back({key,
                                     {std::wstring(session.name(object)), state.can_edit, inspected == object,
                                      double(entity_icon(session, object)), position,
                                      std::wstring(session.has_retained(object) ? L"entity retained" : L"entity"),
                                      false, ui::ControlKey{}}});
                } else {
                    // A Component row's parent is its Entity.
                    const auto state = take(world::inspect_entity(runtime, tree.objects[tree.nodes[row.node].parent]));
                    rows->push_back(
                        {key,
                         {object_title(object), state.can_edit, inspected == object, double(ui::ControlIcon::component),
                          position, std::wstring(session.has_retained(object) ? L"component retained" : L"component"),
                          false, ui::ControlKey{}}});
                }
            }
            const std::array<ui_language::InputValue, 9> inputs{
                selected == ObjectId{} ? std::wstring(L"Entity Structure") : std::wstring(session.name(selected)),
                ui_language::ItemList{std::move(rows)},
                static_cast<double>(tree.rows.size()),
                ui_language::TextEditBinding{control::structure_filter, structure_query},
                selected != ObjectId{},
                selected != ObjectId{} && entity_status.can_edit,
                control::add_component,
                action({"scene.delete", json::Document{json::Document::literal<R"({"target":"object"})">()}}),
                double(ui::ControlIcon::close)};
            const std::array<ui_language::KeyBinding, 2> keys{
                {{"tree", control::structure_tree}, {"add", control::add_component}}};
            static_cast<void>(ui_document_->append("EntityStructure", view.content, keys, inputs, result));
            continue;
        }
        if (inspecting_asset && material_object == ObjectId{}) {
            const auto& asset = assets.selected();
            auto entries = std::make_shared<std::vector<ui_language::Item>>();
            const auto add = [&](std::size_t index, std::wstring name, std::wstring value) {
                auto& key = resource_properties_[index];
                if (!key)
                    key = next_binding_++;
                entries->push_back({key, {std::move(name), std::move(value)}});
            };
            std::wstring kind = L"Resource";
            if (asset) {
                kind = asset->kind == assets::AssetKind::mesh               ? L"Mesh"
                       : asset->kind == assets::AssetKind::scene            ? L"Scene"
                       : asset->kind == assets::AssetKind::source           ? L"Source"
                       : asset->kind == assets::AssetKind::folder           ? L"Folder"
                       : asset->kind == assets::AssetKind::material         ? L"Material"
                       : asset->kind == assets::AssetKind::texture          ? L"Texture"
                       : asset->kind == assets::AssetKind::procedural_graph ? L"Procedural Graph"
                                                                            : L"File";
                add(0, L"Type", kind);
                add(1, L"Resource", platform::utf8_to_utf16_validated(asset->key));
                add(2, L"Location", asset->path.empty() ? L"Engine" : asset->path.generic_wstring());
                if (!asset->path.empty() && asset->kind != assets::AssetKind::folder)
                    add(3, L"Size", std::to_wstring(asset->bytes) + L" bytes");
                if (auto mesh = atelier::assets::Reader{}.mesh(asset->key)) {
                    add(4, L"Triangles", std::to_wstring(mesh->mesh->vertices().size() / 3));
                }
            }
            const std::array<ui_language::InputValue, 4> inputs{
                kind, asset ? asset->name : std::wstring(L"No resource selected"),
                double(asset && asset->kind == assets::AssetKind::mesh ? ui::ControlIcon::mesh
                                                                       : ui::ControlIcon::folder),
                ui_language::ItemList{std::move(entries)}};
            static_cast<void>(ui_document_->append("ResourceParameters", view.content, {}, inputs, result));
            continue;
        }
        const bool in_graph = !active_graph.empty() && session.has_graph(active_graph);
        const bool in_world_net = !in_graph && inspecting_network && !net_graph.empty() && session.has_graph(net_graph);
        const ObjectId param_object =
            (in_graph || in_world_net) ? inspected_graph_object(session, net_graph) : inspected;
        const bool param_is_graph = (in_graph || in_world_net) && param_object != ObjectId{};
        const auto param_node_name = param_is_graph ? session.graph_node_name(param_object) : std::nullopt;
        const auto lowercase = [](std::wstring value) {
            for (auto& c : value)
                c = static_cast<wchar_t>(std::towlower(c));
            return value;
        };
        const auto folded = parameter_search_visible ? lowercase(query) : std::wstring{};
        const bool show_position = !param_is_graph && !parameter_by_category_ && material_object == ObjectId{} &&
                                   transform && (inspected == selected || inspected == *transform);
        auto groups = std::make_shared<std::vector<ui_language::Item>>();
        std::vector<std::pair<std::string, std::shared_ptr<std::vector<ui_language::Item>>>> category_rows;
        const auto category_properties = [&](std::string_view category) {
            auto at = std::ranges::find_if(category_rows, [&](const auto& group) { return group.first == category; });
            if (at == category_rows.end()) {
                category_rows.emplace_back(std::string(category), std::make_shared<std::vector<ui_language::Item>>());
                at = std::prev(category_rows.end());
            }
            return at->second;
        };
        const auto append_properties = [&](ObjectId object) {
            const auto material_bindings = presentation::read_material_bindings(runtime, object);
            const auto mesh =
                material_bindings
                    ? presentation::read_static_mesh(runtime, object)
                    : atelier::Result<presentation::StaticMeshState>{std::unexpected(material_bindings.error())};
            const auto proc_binding =
                material_bindings
                    ? presentation::read_procedural_source(runtime, object)
                    : atelier::Result<presentation::ProceduralSource>{std::unexpected(material_bindings.error())};
            const auto asset_name = [&](const std::string& key) {
                if (const auto* entry = project ? project->entry(key) : nullptr)
                    return entry->path.stem().wstring();
                return platform::utf8_to_utf16_validated(key);
            };
            const auto property_caption = [&](std::string_view name, std::string_view declared) {
                return declared.empty() ? parameter_label(name) : platform::utf8_to_utf16_validated(declared);
            };
            const auto property_key = [&](std::span<const FieldId> path, Binding::Kind kind = Binding::Kind::field) {
                return bind(object, path, kind);
            };
            const auto type = take(runtime.type_of(object));
            const auto title = object_title(object);
            const bool group_matches = lowercase(title).find(folded) != std::wstring::npos;
            auto component_properties = std::make_shared<std::vector<ui_language::Item>>();
            auto properties = component_properties;
            const auto schema = take(session.properties(object));
            const auto light = take(runtime.is_a(type, session.presentation_types().light_component))
                                   ? std::optional(take(presentation::read_light(runtime, object)))
                                   : std::nullopt;
            for (std::size_t index = 0; index < schema.size(); ++index) {
                const auto& property = schema[index];
                if (parameter_by_category_ && material_object == ObjectId{}) {
                    const std::string_view category =
                        property.category.empty() ? std::string_view{"General"} : std::string_view{property.category};
                    properties = category_properties(category);
                }
                const bool can_edit_object =
                    session.is_graph_object(object) ? !session.world().running() : entity_status.can_edit;
                const bool editable = can_edit_object && !property.read_only;
                if (show_position && object == *transform && detail::transform_path(session.types(), property.path))
                    continue;
                if (light) {
                    const auto& name = property.name;
                    if (light->kind != presentation::LightKind::rectangle &&
                        (name == "settings.width" || name == "settings.height" || name == "settings.two_sided"))
                        continue;
                    if (light->kind == presentation::LightKind::rectangle && name == "settings.source_radius")
                        continue;
                    if (light->kind == presentation::LightKind::directional && name == "settings.range")
                        continue;
                }
                auto parent_type = type;
                TypeId aggregate_type{};
                for (std::size_t depth = 0; depth + 1 < property.path.size(); ++depth) {
                    const auto parent = take(runtime.describe(parent_type));
                    aggregate_type = parent_type;
                    parent_type = std::ranges::find(parent->fields, property.path[depth], &FieldInfo::id)->value.type;
                }
                const auto parent = take(runtime.describe(parent_type));
                if (property.path.size() > 1 &&
                    (parent->math.vector() || parent->math.shape == math::Shape::quaternion)) {
                    const auto count = parent->math.components();
                    const auto name = property.name.substr(0, property.name.rfind('.'));
                    const auto prefix = std::span(property.path).first(property.path.size() - 1);
                    const bool spatial_position = std::ranges::equal(property.path, session.types().position_fields[0]);
                    const bool spatial_rotation = std::ranges::equal(property.path, session.types().rotation_fields[0]);
                    const bool spatial_scale = std::ranges::equal(property.path, session.types().scale_fields[0]);
                    const bool light_color =
                        aggregate_type == session.presentation_types().light_settings &&
                        property.path[property.path.size() - 2] ==
                            take(runtime.find_field(session.presentation_types().light_settings, "color"));
                    const bool color = light_color;
                    const auto euler = spatial_rotation
                                           ? euler_degrees(take(world::read_transform(runtime, object)).rotation)
                                           : math::Vector3{};
                    std::vector<ui::NumberField> tuple;
                    bool matches =
                        group_matches || lowercase(platform::utf8_to_utf16_validated(name + " " + property.label + " " +
                                                                                     property.category))
                                                 .find(folded) != std::wstring::npos;
                    for (std::size_t lane = 0; lane < count && index + lane < schema.size(); ++lane) {
                        const auto& field = schema[index + lane];
                        if (field.path.size() != property.path.size() ||
                            !std::equal(prefix.begin(), prefix.end(), field.path.begin()))
                            break;
                        matches |=
                            lowercase(platform::utf8_to_utf16_validated(field.name)).find(folded) != std::wstring::npos;
                        if (spatial_rotation && lane == 3)
                            continue;
                        const auto value =
                            spatial_rotation ? double(euler[lane])
                                             : std::visit(
                                                   [](const auto& v) -> double {
                                                       if constexpr (std::same_as<std::decay_t<decltype(v)>, EnumValue>)
                                                           return v.bits;
                                                       else
                                                           return double(v);
                                                   },
                                                   field.value);
                        const auto [field_minimum, field_maximum] = legal_range(field.value, field.range);
                        const double minimum = spatial_position   ? session.parameters()[lane].minimum
                                               : spatial_scale    ? -Session::maximum_scale
                                               : spatial_rotation ? -360000
                                                                  : field_minimum;
                        const double maximum = spatial_position   ? session.parameters()[lane].maximum
                                               : spatial_scale    ? Session::maximum_scale
                                               : spatial_rotation ? 360000
                                                                  : field_maximum;
                        const auto key =
                            property_key(field.path, spatial_rotation ? Binding::Kind::rotation : Binding::Kind::field);
                        tuple.push_back({key, std::wstring(1, (color ? L"RGBW" : L"XYZW")[lane]), value,
                                         spatial_position || spatial_rotation ? 0
                                         : spatial_scale                      ? 1
                                                                              : value,
                                         minimum, maximum, spatial_rotation ? 1 : .01, spatial_rotation ? .1 : .001,
                                         static_cast<ui::VectorAxis>(lane + 1)});
                        tuple.back().single_precision = parent->math.precision == math::Precision::f32;
                    }
                    index += count - 1;
                    if (matches) {
                        const auto field_key = tuple.front().key;
                        // Swatch shows the display colour; HDR emission is clamped for the chip only.
                        const auto channel = [&](std::size_t lane) {
                            return tuple.size() > lane ? std::clamp(tuple[lane].value, 0.0, 1.0) : 0.0;
                        };
                        const double r = channel(0);
                        const double g = channel(1);
                        const double b = channel(2);
                        PropertyRow row;
                        row.key = property_key(prefix, Binding::Kind::property_row);
                        row.title =
                            spatial_rotation ? std::wstring(L"Rotation (°)") : property_caption(name, property.label);
                        row.edit = {spatial_position   ? bind(object, {}, Binding::Kind::position_default)
                                    : spatial_rotation ? bind(object, {}, Binding::Kind::rotation_default)
                                    : spatial_scale    ? bind(object, {}, Binding::Kind::scale_default)
                                                       : ui::ControlKey{},
                                    editable, false, std::move(tuple)};
                        row.enabled = editable;
                        row.change = {field_key, editable};
                        row.target = field_key;
                        if (color) {
                            row.swatch = std::array{r, g, b};
                            row.pick = {property_key(prefix, Binding::Kind::color_pick), editable};
                            std::ranges::find(bindings_, row.pick.target, &Binding::key)->color_maximum =
                                row.edit.fields.front().maximum;
                        }
                        row.help =
                            property_help_.get(row.key, row.title,
                                               platform::utf8_to_utf16_validated(take(runtime.describe(type))->name) +
                                                   L"." + platform::utf8_to_utf16_validated(name) + L" : " +
                                                   platform::utf8_to_utf16_validated(parent->name) +
                                                   (spatial_rotation ? L" · Euler (°)" : L""),
                                               described(property.description, property.range));
                        properties->push_back(std::move(row).item());
                    }
                    continue;
                }
                auto caption = property_caption(property.name, property.label);
                if (light && property.name.starts_with("settings.")) {
                    const auto field = std::string_view(property.name).substr(9);
                    if (field == "kind")
                        caption = L"Light Type";
                    else if (field == "intensity")
                        caption = light->kind == presentation::LightKind::point       ? L"Intensity (cd)"
                                  : light->kind == presentation::LightKind::rectangle ? L"Intensity (cd/m²)"
                                                                                      : L"Illuminance (lux)";
                    else if (field == "source_radius")
                        caption = light->kind == presentation::LightKind::directional ? L"Angular Radius (rad)"
                                                                                      : L"Source Radius (m)";
                    else if (field == "range")
                        caption = L"Range (m)";
                    else if (field == "width")
                        caption = L"Width (m)";
                    else if (field == "height")
                        caption = L"Height (m)";
                    else if (field == "two_sided")
                        caption = L"Two Sided";
                    else if (field == "casts_shadow")
                        caption = L"Cast Shadows";
                }
                if (!group_matches &&
                    lowercase(caption + L" " +
                              platform::utf8_to_utf16_validated(property.name + " " + property.category))
                            .find(folded) == std::wstring::npos)
                    continue;
                if (property.is_string) {
                    const auto text_key = property_key(property.path, Binding::Kind::text_field);
                    PropertyRow row;
                    row.key = property_key(property.path, Binding::Kind::property_row);
                    row.title = std::move(caption);
                    row.kind = PropertyRow::Kind::text;
                    row.enabled = editable;
                    row.change = {text_key, false};
                    row.target = text_key;
                    row.text_edit = {text_key, platform::utf8_to_utf16_validated(property.string_value), editable};
                    row.help =
                        property_help_.get(row.key, row.title,
                                           platform::utf8_to_utf16_validated(take(runtime.describe(type))->name) +
                                               L"." + platform::utf8_to_utf16_validated(property.name) + L" : String",
                                           property.description);
                    properties->push_back(std::move(row).item());
                    continue;
                }
                std::optional<Scalar> default_value;
                auto owner = type;
                for (const auto id : property.path) {
                    const auto info = take(runtime.describe(owner));
                    const auto at = std::ranges::find(info->fields, id, &FieldInfo::id);
                    if (at == info->fields.end())
                        break;
                    default_value = at->default_leaves.size() == 1 ? std::optional<Scalar>{at->default_leaves.front()}
                                                                   : std::nullopt;
                    owner = at->value.type;
                }
                const bool boolean = std::holds_alternative<bool>(property.value);
                const auto* enumeration = std::get_if<EnumValue>(&property.value);
                const bool integral = std::holds_alternative<std::int32_t>(property.value) ||
                                      std::holds_alternative<std::uint32_t>(property.value);
                const auto [minimum, maximum] = legal_range(property.value, property.range);
                const auto key = property_key(property.path);
                const double value = scalar_number(property.value);
                const double initial = default_value ? scalar_number(*default_value) : value;
                ui_language::EditBinding edit{
                    0,
                    editable,
                    value != initial,
                    {{key, L"", value, initial, minimum, maximum, integral ? 1 : .01, integral ? 1 : .001}}};
                edit.fields.front().single_precision = std::holds_alternative<float>(property.value);
                // Integral fields commit whole numbers only, so they keep the ladder without a slider.
                if (const auto slider = slider_range(property.range); slider && !integral && !boolean && !enumeration)
                    edit.fields.front().slider = ui::SliderRange{property_key(property.path, Binding::Kind::slider),
                                                                 slider->first, slider->second};
                std::wstring choice;
                std::string description = property.description;
                if (enumeration) {
                    const auto& enum_info = *take(runtime.describe(enumeration->type))->enumeration;
                    const auto enum_label = [&](std::size_t index) {
                        return index < enum_info.metadata.size() && !enum_info.metadata[index].label.empty()
                                   ? platform::utf8_to_utf16_validated(enum_info.metadata[index].label)
                                   : parameter_label(enum_info.values[index].first);
                    };
                    for (std::size_t i = 0; i < enum_info.values.size(); ++i)
                        if (enum_info.values[i].second == enumeration->bits) {
                            choice = enum_label(i);
                            if (i < enum_info.metadata.size() && !enum_info.metadata[i].description.empty()) {
                                if (!description.empty())
                                    description += "\n";
                                description += enum_info.metadata[i].description;
                            }
                            break;
                        }
                    if (choice.empty() && enum_info.flags) {
                        if (enumeration->bits == 0) {
                            choice = L"None";
                        } else {
                            std::uint32_t covered = 0;
                            const auto append_matching = [&](bool single_bit_only) {
                                for (std::size_t i = 0; i < enum_info.values.size(); ++i) {
                                    const auto member_bits = enum_info.values[i].second;
                                    if (member_bits == 0 || (enumeration->bits & member_bits) != member_bits)
                                        continue;
                                    if (single_bit_only ? !std::has_single_bit(member_bits)
                                                        : (member_bits & ~covered) == 0)
                                        continue;
                                    covered |= member_bits;
                                    if (!choice.empty())
                                        choice += L" | ";
                                    choice += enum_label(i);
                                }
                            };
                            append_matching(true);
                            if (covered != enumeration->bits)
                                append_matching(false);
                        }
                    }
                }
                PropertyRow row;
                row.key = property_key(property.path, Binding::Kind::property_row);
                row.title = std::move(caption);
                row.edit = std::move(edit);
                row.kind = boolean       ? PropertyRow::Kind::boolean
                           : enumeration ? PropertyRow::Kind::enumeration
                                         : PropertyRow::Kind::number;
                row.checked = boolean && std::get<bool>(property.value);
                row.enabled = editable;
                row.change = {key, editable};
                row.choice = std::move(choice);
                row.target = key;
                row.help = property_help_.get(
                    row.key, row.title,
                    platform::utf8_to_utf16_validated(take(runtime.describe(type))->name) + L"." +
                        platform::utf8_to_utf16_validated(property.name) + L" : " +
                        platform::utf8_to_utf16_validated(take(runtime.describe(property.type))->name),
                    described(description, property.range));
                properties->push_back(std::move(row).item());
            }
            properties = parameter_by_category_ && material_object == ObjectId{} && material_bindings
                             ? category_properties("Material")
                             : component_properties;
            const auto selector = [&](ui::ControlKey key, std::wstring caption, std::wstring choice,
                                      std::string_view description) {
                if (!group_matches && lowercase(caption + L" " + choice).find(folded) == std::wstring::npos)
                    return;
                PropertyRow row;
                row.key = key;
                row.title = std::move(caption);
                row.kind = PropertyRow::Kind::enumeration;
                row.enabled = !session.world().running();
                row.change = {key, row.enabled};
                row.choice = std::move(choice);
                row.target = key;
                row.help = property_help_.get(key, row.title, L"Resource binding", description);
                properties->push_back(std::move(row).item());
            };
            const auto mark_unavailable = [&](assets::AssetKind kind, std::string_view key, std::wstring& name) {
                if (const auto* item = session.unavailable(kind, key))
                    name.insert(0, item->kind == world::DiagnosticKind::unreadable_resource ? L"(unreadable) "
                                                                                            : L"(missing) ");
            };
            if (proc_binding) {
                properties = parameter_by_category_ && material_object == ObjectId{} ? category_properties("Procedural")
                                                                                     : component_properties;
                std::wstring graph_choice = proc_binding->source.empty() ? L"None" : asset_name(proc_binding->source);
                mark_unavailable(assets::AssetKind::procedural_graph, proc_binding->source, graph_choice);
                selector(
                    bind_material(object, {}, Binding::Kind::procedural_graph_slot, {}, "graph"), L"Procedural Graph",
                    std::move(graph_choice),
                    "选择此生成器求值的 .procgraph 资产；菜单内也可一键进入二维 Network 画布编辑图节点，或将实例烘焙为普通静态网格实体。");
                properties = parameter_by_category_ && material_object == ObjectId{} && material_bindings
                                 ? category_properties("Material")
                                 : component_properties;
            }
            if (material_bindings && mesh) {
                if (!mesh->mesh.resolved && !mesh->mesh.source.empty() &&
                    (group_matches ||
                     lowercase(L"Mesh " + platform::utf8_to_utf16_validated(mesh->mesh.source)).find(folded) !=
                         std::wstring::npos)) {
                    PropertyRow row;
                    row.key = bind_material(object, {}, Binding::Kind::saved_data, {}, "mesh");
                    row.title = L"Mesh";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.enabled = false;
                    row.change = {row.key, false};
                    row.choice = platform::utf8_to_utf16_validated(mesh->mesh.source);
                    mark_unavailable(assets::AssetKind::mesh, mesh->mesh.source, row.choice);
                    row.target = row.key;
                    row.help = property_help_.get(
                        row.key, row.title, L"Unavailable resource",
                        "网格资源缺失或无法读取：组件不绘制，原引用和材质绑定原样保存。恢复资源后重新打开场景即可使用。");
                    properties->push_back(std::move(row).item());
                }
                if (mesh->mesh.resolved)
                    for (std::size_t i = 0; i < mesh->mesh.resolved->materials().size(); ++i) {
                        const auto slot_name = presentation::material_slot_name(*mesh->mesh.resolved, i);
                        std::wstring choice = L"Mesh Default";
                        const auto at =
                            std::ranges::find(*material_bindings, slot_name, &presentation::MaterialBinding::slot);
                        if (at != material_bindings->end()) {
                            const auto info = take(session.inspect_material(at->resource.id()));
                            choice = info.scene ? L"Scene Instance" : asset_name(info.asset_key);
                            mark_unavailable(assets::AssetKind::material, info.asset_key, choice);
                        } else if (const auto fallback = std::ranges::find(mesh->mesh.defaults, slot_name,
                                                                           &presentation::MaterialBinding::slot);
                                   fallback != mesh->mesh.defaults.end()) {
                            const auto source = take(material::material_key(runtime, fallback->resource.id()));
                            auto name = asset_name(source);
                            mark_unavailable(assets::AssetKind::material, source, name);
                            choice += L" · " + name;
                        }
                        selector(
                            bind_material(object, {}, Binding::Kind::material_slot, {}, slot_name),
                            L"Material · " + platform::utf8_to_utf16_validated(slot_name), std::move(choice),
                            "为这个 Mesh slot 选择 Material 资源。Mesh Default 恢复网格自带材质；修改参与当前 Scene 的撤销与保存。");
                    }
            } else if (material_bindings && proc_binding) {
                std::wstring choice = L"Default Grid";
                if (const auto at = std::ranges::find(*material_bindings, std::string_view{"default"},
                                                      &presentation::MaterialBinding::slot);
                    at != material_bindings->end()) {
                    const auto info = take(session.inspect_material(at->resource.id()));
                    choice = info.scene ? L"Scene Instance" : asset_name(info.asset_key);
                    mark_unavailable(assets::AssetKind::material, info.asset_key, choice);
                }
                selector(bind_material(object, {}, Binding::Kind::material_slot, {}, "default"), L"Material · default",
                         std::move(choice),
                         "为程序化生成的表面多边形选择 Material 资源；CopyToPoints 实例各自保留其网格资产材质。");
            }
            if (!component_properties->empty())
                groups->push_back(
                    {property_key({}, Binding::Kind::property_group),
                     {title, double(object == selected ? ui::ControlIcon::object : ui::ControlIcon::component),
                      ui_language::ItemList{std::move(component_properties)}}});
            if (!session.has_retained(object))
                return;
            const auto saved = session.retained(object);
            if (!saved || saved->entries.empty())
                return;
            const bool categorized = parameter_by_category_ && material_object == ObjectId{};
            auto saved_rows =
                categorized ? category_properties("Saved Data") : std::make_shared<std::vector<ui_language::Item>>();
            for (const auto& entry : saved->entries) {
                std::string path;
                for (const auto& part : entry.path)
                    path += (path.empty() ? "" : ".") + part;
                const auto caption = platform::utf8_to_utf16_validated(path);
                if (!group_matches && lowercase(caption).find(folded) == std::wstring::npos)
                    continue;
                PropertyRow row;
                row.key = bind_material(object, {}, Binding::Kind::saved_data, {}, path);
                row.title = caption;
                row.kind = PropertyRow::Kind::enumeration;
                row.enabled = false;
                row.change = {row.key, false};
                row.choice = platform::utf8_to_utf16_validated(entry.value);
                row.target = row.key;
                row.help = property_help_.get(
                    row.key, row.title, L"Saved data · " + platform::utf8_to_utf16_validated(world::name(entry.kind)),
                    entry.kind == world::DiagnosticKind::missing_resource ||
                            entry.kind == world::DiagnosticKind::unreadable_resource
                        ? "依赖的资源缺失或无法读取，这些数据无法应用，会原样写回文件。恢复资源后重新打开场景即可应用；Edit > Discard Saved Data 可丢弃。"
                        : "保存的场景数据无法应用到当前类型。它会原样写回文件；编辑对应字段或使用 Edit > Discard Saved Data 可丢弃。");
                saved_rows->push_back(std::move(row).item());
            }
            if (!categorized && !saved_rows->empty())
                groups->push_back({bind(object, {}, Binding::Kind::saved_data),
                                   {L"Saved Data · " + title, double(ui::ControlIcon::component),
                                    ui_language::ItemList{std::move(saved_rows)}}});
        };
        const auto append_material = [&](ObjectId object, ObjectId component = ObjectId{},
                                         std::string_view slot = std::string_view{}) {
            const auto material = take(session.inspect_material(object));
            const auto title = (material.source  ? L"Material Source"
                                : material.scene ? L"Scene Material Instance"
                                                 : L"Material Instance") +
                               (slot.empty() ? std::wstring{} : L" · " + platform::utf8_to_utf16_validated(slot));
            const bool editable = !material.source && !session.world().running();
            auto rows = std::make_shared<std::vector<ui_language::Item>>();
            const auto key = [&](std::string_view path, Binding::Kind kind, std::size_t lane = 0) {
                return bind_material(object, path, kind, component, slot, lane);
            };
            const auto lane_value = [](const material::LocalValue& value, std::size_t lane) {
                return std::visit(
                    [&](const auto& v) -> double {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::same_as<T, float>)
                            return v;
                        else if constexpr (std::same_as<T, material::TextureBinding>)
                            return 0;
                        else
                            return v[lane];
                    },
                    value);
            };
            for (const auto& field : material.declarations) {
                if (lowercase(title + L" " + platform::utf8_to_utf16_validated(field.path)).find(folded) ==
                    std::wstring::npos)
                    continue;
                const auto local = std::ranges::find(material.entries, field.path, &material::Override::path);
                const bool overridden = local != material.entries.end() && local->enabled;
                // Disabled/mismatched local entries remain in the owning view; numbers show effective values.
                PropertyRow row;
                row.key = key(field.path, Binding::Kind::property_row);
                row.title = platform::utf8_to_utf16_validated(field.path);
                row.enabled = editable && overridden;
                row.override_available = !material.source;
                row.overridden = overridden;
                row.inherit = {key(field.path, Binding::Kind::material_override), editable};
                if (!field.components) {
                    row.kind = PropertyRow::Kind::enumeration;
                    row.target = key(field.path, Binding::Kind::material_texture);
                    row.change = {row.target, row.enabled};
                    const auto& binding = std::get<material::TextureBinding>(field.value);
                    const auto* entry = project ? project->entry(binding.asset_key) : nullptr;
                    row.choice = binding.asset_key.empty() ? L"None"
                                 : entry                   ? entry->path.stem().wstring()
                                                           : platform::utf8_to_utf16_validated(binding.asset_key);
                    if (session.unavailable(assets::AssetKind::texture, binding.asset_key))
                        row.choice = L"(missing) " + row.choice;
                } else {
                    row.edit.enabled = row.enabled;
                    for (std::size_t lane = 0; lane < field.components; ++lane) {
                        const auto value = lane_value(field.value, lane);
                        const auto initial = lane_value(field.initial, lane);
                        row.edit.fields.push_back({key(field.path, Binding::Kind::material_field, lane),
                                                   field.components == 1 ? L"" : std::wstring(1, L"XYZW"[lane]), value,
                                                   initial, -std::numeric_limits<float>::max(),
                                                   std::numeric_limits<float>::max(), .01, .001,
                                                   static_cast<ui::VectorAxis>(lane + 1)});
                        row.edit.fields.back().single_precision = true;
                    }
                    row.target = row.edit.fields.front().key;
                    row.change = {row.target, row.enabled};
                    if (field.components >= 3) {
                        row.swatch = std::array{std::clamp(lane_value(field.value, 0), 0., 1.),
                                                std::clamp(lane_value(field.value, 1), 0., 1.),
                                                std::clamp(lane_value(field.value, 2), 0., 1.)};
                        row.pick = {key(field.path, Binding::Kind::color_pick), row.enabled};
                        std::ranges::find(bindings_, row.pick.target, &Binding::key)->color_maximum =
                            std::numeric_limits<float>::max();
                    }
                }
                row.help = property_help_.get(
                    row.key, row.title, L"Material declaration · " + platform::utf8_to_utf16_validated(field.path),
                    "数值显示当前有效值。关闭覆盖会保留本地值；右键 Discard Local Entry 删除本地条目并恢复继承。");
                rows->push_back(std::move(row).item());
            }
            for (const auto& entry : material.entries)
                if (std::ranges::find(material.declarations, entry.path, &MaterialParameterView::path) ==
                    material.declarations.end()) {
                    PropertyRow row;
                    row.key = key(entry.path, Binding::Kind::property_row);
                    row.title = platform::utf8_to_utf16_validated(entry.path) + L" (Undeclared)";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.choice = L"Retained local entry";
                    row.target = row.key;
                    row.enabled = false;
                    row.change = {row.key, false};
                    row.override_available = true;
                    row.overridden = entry.enabled;
                    row.inherit = {key(entry.path, Binding::Kind::material_override), editable};
                    rows->push_back(std::move(row).item());
                }
            for (std::size_t i = 0; i < material.diagnostics.size(); ++i) {
                const auto& diagnostic = material.diagnostics[i];
                PropertyRow row;
                row.key = key(diagnostic.path, Binding::Kind::saved_data, i);
                row.title = platform::utf8_to_utf16_validated(diagnostic.path);
                row.kind = PropertyRow::Kind::enumeration;
                row.choice = application::diagnostic_text(diagnostic.message);
                row.target = row.key;
                row.enabled = false;
                row.change = {row.key, false};
                rows->push_back(std::move(row).item());
            }
            groups->push_back({key({}, Binding::Kind::property_group),
                               {title, double(ui::ControlIcon::component), ui_language::ItemList{std::move(rows)}}});
        };
        if (material_object != ObjectId{}) {
            append_material(material_object);
            std::wstring name = platform::utf8_to_utf16_validated(material_inspection);
            if (const auto* entry = project ? project->entry(material_inspection) : nullptr)
                name = entry->path.stem().wstring();
            const std::array<ui_language::InputValue, 7> inputs{
                name,
                session.material_dirty(material_inspection),
                ui_language::ItemList{std::move(groups)},
                parameter_search_visible,
                ui_language::TextEditBinding{control::parameter_search, query},
                ui_language::ActionBinding{control::parameter_search_toggle},
                double(ui::ControlIcon::search)};
            static_cast<void>(ui_document_->append("MaterialParameters", view.content, {}, inputs, result));
            continue;
        }
        const auto append_object = [&](ObjectId object) {
            append_properties(object);
            const auto bindings = presentation::read_material_bindings(runtime, object);
            if (!bindings)
                return;
            // Groups edit the actual shared document identity. Creating a scene MI is explicit in the slot menu.
            std::vector<const presentation::MaterialBinding*> shown;
            if (const auto static_mesh = presentation::read_static_mesh(runtime, object)) {
                const auto& mesh = static_mesh->mesh;
                if (mesh.resolved)
                    for (std::size_t i = 0; i < mesh.resolved->materials().size(); ++i) {
                        const auto name = presentation::material_slot_name(*mesh.resolved, i);
                        if (const auto at = std::ranges::find(*bindings, name, &presentation::MaterialBinding::slot);
                            at != bindings->end())
                            shown.push_back(&*at);
                        else if (const auto fallback =
                                     std::ranges::find(mesh.defaults, name, &presentation::MaterialBinding::slot);
                                 fallback != mesh.defaults.end())
                            shown.push_back(&*fallback);
                    }
            }
            for (const auto& binding : *bindings)
                if (std::ranges::find(shown, &binding) == shown.end())
                    shown.push_back(&binding);
            for (const auto* binding : shown)
                append_material(binding->resource.id(), object, binding->slot);
        };
        if (param_object != ObjectId{}) {
            append_object(param_object);
            if (!param_is_graph && param_object == selected)
                for (const auto& component : take(world::components(runtime, selected)))
                    append_object(component.id());
        }
        for (auto& [category, rows] : category_rows)
            if (!rows->empty()) {
                auto& key = category_keys_[category];
                if (!key)
                    key = next_binding_++;
                groups->push_back(
                    {key, {platform::utf8_to_utf16_validated(category), 0., ui_language::ItemList{std::move(rows)}}});
            }
        constexpr std::array aliases{L"transform entity position 变换 位置 x", L"transform entity position 变换 位置 y",
                                     L"transform entity position 变换 位置 z"};
        constexpr std::array names{L"X", L"Y", L"Z"};
        const auto definitions = session.parameters();
        std::vector<ui::NumberField> fields;
        bool group_modified{};
        for (std::size_t axis = 0; axis < definitions.size(); ++axis) {
            const auto& definition = definitions[axis];
            const double value = show_position
                                     ? std::get<float>(take(session.world().read(*transform, definition.path)))
                                     : definition.default_value;
            group_modified |= value != definition.default_value;
            if (!show_position || std::wstring_view(aliases[axis]).find(folded) == std::wstring_view::npos)
                continue;
            fields.push_back({bind(*transform, definition.path), names[axis], value, definition.default_value,
                              definition.minimum, definition.maximum, definition.drag_step, .001,
                              static_cast<ui::VectorAxis>(axis + 1)});
            fields.back().single_precision = true;
        }
        const bool position_matches = !fields.empty();
        const bool rotation_matches =
            show_position && std::wstring_view(L"rotation transform angle euler x y z 旋转 角度").find(folded) !=
                                 std::wstring_view::npos;
        const bool scale_matches =
            show_position && std::wstring_view(L"scale transform x y z 缩放").find(folded) != std::wstring_view::npos;
        ui_language::EditBinding rotation_edit;
        ui_language::EditBinding scale_edit;
        if (show_position) {
            const auto trs = take(world::read_transform(runtime, *transform));
            const auto angles = euler_degrees(trs.rotation);
            rotation_edit = {bind(*transform, {}, Binding::Kind::rotation_default),
                             entity_status.can_edit,
                             angles != math::Vector3{},
                             {}};
            scale_edit = {bind(*transform, {}, Binding::Kind::scale_default),
                          entity_status.can_edit,
                          trs.scale != math::Vector3{1, 1, 1},
                          {}};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                rotation_edit.fields.push_back(
                    {bind(*transform, session.types().rotation_fields[axis], Binding::Kind::rotation), names[axis],
                     angles[axis], 0, -360000, 360000, 1, .1, static_cast<ui::VectorAxis>(axis + 1)});
                scale_edit.fields.push_back({bind(*transform, session.types().scale_fields[axis]), names[axis],
                                             trs.scale[axis], 1, -Session::maximum_scale, Session::maximum_scale, .01,
                                             .001, static_cast<ui::VectorAxis>(axis + 1)});
                rotation_edit.fields.back().single_precision = scale_edit.fields.back().single_precision = true;
            }
        }
        const bool matches = position_matches || rotation_matches || scale_matches;
        const bool empty = groups->empty() && !matches;
        ui_language::EditBinding position{show_position ? bind(*transform, {}, Binding::Kind::position_default)
                                                        : ui::ControlKey{74},
                                          entity_status.can_edit && show_position, group_modified, std::move(fields)};
        const bool entity_selected = !param_is_graph && param_object != ObjectId{} && param_object == selected;
        const auto transform_help = [&](std::span<const FieldId> lane, ui::ControlKey key, std::wstring_view title,
                                        bool rotation = false) -> ui_language::TooltipBinding {
            if (!show_position)
                return {};
            const auto type = take(runtime.describe(take(runtime.type_of(*transform))));
            auto owner = type;
            const FieldInfo* field{};
            for (std::size_t depth = 0; depth + 1 < lane.size(); ++depth) {
                field = &*std::ranges::find(owner->fields, lane[depth], &FieldInfo::id);
                owner = take(runtime.describe(field->value.type));
            }
            return property_help_.get(
                key, title,
                platform::utf8_to_utf16_validated(type->name) + L"." + platform::utf8_to_utf16_validated(field->name) +
                    L" : " + platform::utf8_to_utf16_validated(owner->name) + (rotation ? L" · Euler (°)" : L""),
                field->description);
        };
        ui_language::TooltipBinding type_help;
        if (param_object != ObjectId{}) {
            const auto type = take(runtime.describe(take(runtime.type_of(param_object))));
            type_help = property_help_.get(52, short_type(param_object), platform::utf8_to_utf16_validated(type->name),
                                           type->description);
        }
        const std::array transform_descriptions{
            transform_help(session.types().position_fields[0], 70, L"Position (m)"),
            transform_help(session.types().rotation_fields[0], rotation_edit.group, L"Rotation (°)", true),
            transform_help(session.types().scale_fields[0], scale_edit.group, L"Scale")};
        const std::wstring param_graph_title =
            in_world_net ? network_path_title(session, project, net_graph) : graph_path_title(project);
        const std::wstring header_name = param_object == ObjectId{} ? L"No selection"
                                         : param_node_name ? platform::utf8_to_utf16_validated(*param_node_name)
                                         : param_is_graph  ? param_graph_title
                                         : entity_selected ? std::wstring(session.name(selected))
                                                           : object_title(param_object);
        const bool can_rename_header =
            param_node_name ? !session.world().running() : (entity_selected && entity_status.can_edit);
        const std::array<ui_language::InputValue, 30> inputs{
            param_object == ObjectId{} ? std::wstring(L"Object") : object_title(param_object),
            ui_language::TextEditBinding{
                (entity_selected || param_node_name) ? bind(param_object, {}, Binding::Kind::name) : ui::ControlKey{77},
                header_name, can_rename_header},
            ui_language::ActionBinding{control::parameter_back, can_navigate(-1)},
            ui_language::ActionBinding{control::parameter_forward, can_navigate(1)},
            double(ui::ControlIcon::back),
            double(ui::ControlIcon::forward),
            double(ui::ControlIcon::world),
            param_object != ObjectId{},
            ui_language::ItemList{std::move(groups)},
            matches,
            std::move(position),
            ui_language::TextEditBinding{control::parameter_search, query},
            empty,
            double(entity_selected ? entity_icon(session, selected) : ui::ControlIcon::component),
            double(ui::ControlIcon::search),
            parameter_search_visible && param_object != ObjectId{},
            ui_language::ActionBinding{control::parameter_search_toggle, param_object != ObjectId{}},
            std::move(rotation_edit),
            std::move(scale_edit),
            position_matches,
            rotation_matches,
            scale_matches,
            parameter_by_category_,
            ui_language::ActionBinding{control::group_components, true},
            ui_language::ActionBinding{control::group_categories, true},
            transform_descriptions[0],
            transform_descriptions[1],
            transform_descriptions[2],
            std::move(type_help),
            param_graph_title};
        const std::array<ui_language::KeyBinding, 14> keys{{{"components", control::group_components},
                                                            {"categories", control::group_categories},
                                                            {"path.bar", 75},
                                                            {"path.path", control::parameter_path},
                                                            {"path.back", control::parameter_back},
                                                            {"path.forward", control::parameter_forward},
                                                            {"header", 51},
                                                            {"type", 52},
                                                            {"search_toggle", control::parameter_search_toggle},
                                                            {"scroll", control::parameter_list},
                                                            {"transform", 85},
                                                            {"position.section", control::position_fold},
                                                            {"position.contents", 58},
                                                            {"position.position", 70, 72, 73}}};
        static_cast<void>(ui_document_->append("Parameters", view.content, keys, inputs, result));
    }
    const auto preview = placement.preview.value_or(ui::Rect{});
    const std::array<ui_language::InputValue, 5> preview_inputs{placement.preview.has_value(), double(preview.x),
                                                                double(preview.y), double(preview.width),
                                                                double(preview.height)};
    const std::array<ui_language::KeyBinding, 1> preview_keys{{{"preview", placement.preview_key}}};
    static_cast<void>(ui_document_->append("DockPreview", 0, preview_keys, preview_inputs, result));
    menu_items.clear();
    component_choices_.clear();
    material_choices_.clear();
    network_path_choices_.clear();
    menu_selection_ = session.selection();
    if (menu) {
        const auto find = [&](ui::ControlKey key) -> const ui::Control* {
            const auto controls = result.controls();
            const auto at = std::ranges::find(controls, key, &ui::Control::key);
            return at == controls.end() ? nullptr : &*at;
        };
        const auto* source = find(menu->source);
        bool available = source != nullptr;
        for (auto* item = source; item; item = find(item->parent))
            available &= item->enabled && !item->layout.collapsed;
        if (!available)
            menu.reset();
        else {
            const auto operator_item = [&](OperatorCall call, const MenuBinding* definition = nullptr) {
                if (!definition)
                    definition = std::ranges::find_if(menu_bindings, [&](const auto& row) {
                        return row.call.id == call.id && row.call.args == call.args;
                    });
                if (definition == std::end(menu_bindings))
                    throw std::logic_error("Missing operator menu presentation: " + call.id);
                if (definition->asset_kind &&
                    (!assets.selected() || assets.selected()->kind != *definition->asset_kind))
                    call.context.asset = std::string{};
                if (call.id == "scene.rename" && context.region == RegionKind::assets)
                    call.id = "asset.rename";
                const auto key_text = shortcut(call, context.region);
                const auto binding = action(std::move(call));
                const auto selected = take(bindings_[binding_cursor_ - 1].call->state(operators, context));
                ui::MenuItem item{binding.target,
                                  std::wstring(definition->title),
                                  binding.enabled,
                                  selected.value_or(false),
                                  {},
                                  std::wstring(key_text),
                                  definition->separator};
                item.radio_group = definition->radio_group;
                item.behavior = definition->radio_group ? ui::MenuBehavior::radio
                                : selected              ? ui::MenuBehavior::checkbox
                                                        : ui::MenuBehavior::command;
                return item;
            };
            const auto calls = [&](std::initializer_list<OperatorCall> calls) {
                std::vector<ui::MenuItem> result;
                for (const auto& call : calls)
                    result.push_back(operator_item(call));
                return result;
            };
            const auto viewport_calls = [&](ui::ControlKey key) {
                // These exact literals are initialized by menu_bindings at startup.
                switch (key) {
                case control::viewport_tools_menu:
                    return calls(
                        {{"tool.settings", json::Document{json::Document::literal<R"({"tool":"select"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"tool":"transform"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"tool":"move"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"tool":"rotate"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"tool":"scale"})">()}}});
                case control::viewport_snap:
                    return calls(
                        {{"tool.toggle", json::Document{json::Document::literal<R"({"setting":"grid_snap"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"grid_spacing":0.1})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"grid_spacing":0.2})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"grid_spacing":0.5})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"grid_spacing":1.0})">()}},
                         {"tool.toggle", json::Document{json::Document::literal<R"({"setting":"snap_move"})">()}},
                         {"tool.toggle", json::Document{json::Document::literal<R"({"setting":"snap_rotate"})">()}},
                         {"tool.toggle", json::Document{json::Document::literal<R"({"setting":"snap_scale"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"rotation_step":1.0})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"rotation_step":5.0})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"rotation_step":15.0})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"rotation_step":45.0})">()}},
                         {"scene.snap_selection", json::Document{json::Document::literal<R"({"to":"grid"})">()}},
                         {"scene.snap_selection", json::Document{json::Document::literal<R"({"to":"cursor"})">()}},
                         {"cursor.to_selection"},
                         {"cursor.reset"},
                         {"cursor.to_grid"},
                         {"tool.toggle",
                          json::Document{json::Document::literal<R"({"setting":"cursor_surface"})">()}}});
                case control::viewport_camera:
                    return calls({{"view.axis", json::Document{json::Document::literal<R"({"view":"perspective"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"front"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"back"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"left"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"right"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"top"})">()}},
                                  {"view.axis", json::Document{json::Document::literal<R"({"view":"bottom"})">()}},
                                  {"view.frame_selected"},
                                  {"view.from_camera"},
                                  {"view.reset"}});
                case control::viewport_pivot:
                    return calls({{"tool.settings", json::Document{json::Document::literal<R"({"pivot":"median"})">()}},
                                  {"tool.settings", json::Document{json::Document::literal<R"({"pivot":"cursor"})">()}},
                                  {"cursor.to_selection"},
                                  {"cursor.reset"},
                                  {"cursor.to_grid"}});
                case control::viewport_orientation: {
                    auto items = calls(
                        {{"tool.settings", json::Document{json::Document::literal<R"({"orientation":"global"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"orientation":"local"})">()}}});
                    items.front().separator_before = false;
                    items.front().heading = L"Transform Orientations";
                    return items;
                }
                default:
                    return std::vector<ui::MenuItem>{};
                }
            };
            const auto submenu = [&](ui::ControlKey key, std::wstring_view title, std::vector<ui::MenuItem> children,
                                     bool separator = false) {
                ui::MenuItem item{.key = key,
                                  .text = std::wstring(title),
                                  .edit_command = {},
                                  .separator_before = separator,
                                  .children = std::move(children)};
                item.enabled = std::ranges::any_of(item.children, &ui::MenuItem::enabled);
                return item;
            };
            const auto header_menu = [&](ui::ControlKey key) -> std::vector<ui::MenuItem> {
                if (key == control::viewport_view_menu) {
                    auto items = calls({{"view.frame_selected"}, {"view.frame_all"}});
                    items.push_back(submenu(control::view_viewpoint_menu, L"Viewpoint",
                                            viewport_calls(control::viewport_camera), true));
                    auto options =
                        calls({{"view.toggle", json::Document{json::Document::literal<R"({"option":"options"})">()}},
                               {"view.toggle", json::Document{json::Document::literal<R"({"option":"fps"})">()}}});
                    options.front().separator_before = true;
                    for (auto& item : options)
                        items.push_back(std::move(item));
                    return items;
                }
                if (key == control::viewport_select_menu) {
                    auto items = calls(
                        {{"scene.select_all", json::Document{json::Document::literal<R"({"action":"select"})">()}},
                         {"scene.select_all", json::Document{json::Document::literal<R"({"action":"deselect"})">()}},
                         {"scene.select_all", json::Document{json::Document::literal<R"({"action":"invert"})">()}},
                         {"view.select_box"}});
                    items.front().separator_before = false;
                    auto modes = calls(
                        {{"tool.settings", json::Document{json::Document::literal<R"({"select_mode":"set"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"select_mode":"extend"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"select_mode":"subtract"})">()}},
                         {"tool.settings", json::Document{json::Document::literal<R"({"select_mode":"invert"})">()}},
                         {"tool.settings",
                          json::Document{json::Document::literal<R"({"select_mode":"intersect"})">()}}});
                    modes.front().separator_before = false;
                    items.push_back(submenu(control::select_mode_menu, L"Selection Mode", std::move(modes), true));
                    return items;
                }
                if (key == control::viewport_header_add) {
                    std::vector<ui::MenuItem> items;
                    for (const auto& definition : menu_bindings)
                        if (definition.group == MenuGroup::create)
                            items.push_back(operator_item(definition.call));
                    return items;
                }
                if (key == control::viewport_object_menu) {
                    auto items = calls(
                        {{"scene.duplicate"},
                         {"scene.delete", json::Document{json::Document::literal<R"({"target":"entities"})">()}}});
                    items.front().separator_before = false;
                    auto clear = calls({{"scene.transform_clear",
                                         json::Document{json::Document::literal<R"({"channel":"location"})">()}},
                                        {"scene.transform_clear",
                                         json::Document{json::Document::literal<R"({"channel":"rotation"})">()}},
                                        {"scene.transform_clear",
                                         json::Document{json::Document::literal<R"({"channel":"scale"})">()}}});
                    clear.front().separator_before = false;
                    items.push_back(submenu(control::object_clear_menu, L"Clear", std::move(clear), true));
                    auto snap = calls(
                        {{"scene.snap_selection", json::Document{json::Document::literal<R"({"to":"grid"})">()}},
                         {"scene.snap_selection", json::Document{json::Document::literal<R"({"to":"cursor"})">()}},
                         {"cursor.to_selection"},
                         {"cursor.to_grid"},
                         {"cursor.reset"}});
                    for (auto& item : snap)
                        item.separator_before = false;
                    items.push_back(submenu(control::object_snap_menu, L"Snap", std::move(snap)));
                    auto transform = calls({{"scene.transform_add"}, {"scene.transform_remove"}});
                    transform.front().separator_before = true;
                    for (auto& item : transform)
                        items.push_back(std::move(item));
                    return items;
                }
                return {};
            };
            std::vector<ui::MenuItem> items = dock.menu_items(source->key, frame);
            if ((source->context_target ? source->context_target : source->key) == world_control::setup) {
                items = calls(
                    {{"view.lighting_fit", json::Document{json::Document::literal<R"({"mode":"scene"})">()}},
                     {"view.lighting_fit", json::Document{json::Document::literal<R"({"mode":"scene_focus"})">()}}});
                items[0].text = L"Fit Scene";
                items[1].text = L"Scene + Follow Focus";
            } else if (auto lighting_items =
                           lighting.menu(source->context_target ? source->context_target : source->key, this->view);
                       !lighting_items.empty()) {
                items = std::move(lighting_items);
            } else if (auto header_items = header_menu(source->key); !header_items.empty()) {
                items = std::move(header_items);
            } else if (const auto* row = binding(source->key); row && row->kind == Binding::Kind::object &&
                                                               row->path.empty() &&
                                                               source->kind == ui::ControlKind::tree_item) {
                items = calls({{"scene.rename"},
                               {"scene.duplicate"},
                               {"scene.delete", json::Document{json::Document::literal<R"({"target":"entities"})">()}},
                               {"scene.parent_set", json::Document{json::Document::literal<R"({"parent":null})">()}},
                               {"view.frame_selected"}});
                for (auto& item : items)
                    item.separator_before = false;
                items[3].separator_before = items[4].separator_before = true;
                items[0].icon = ui::ControlIcon::rename;
                items[1].icon = ui::ControlIcon::duplicate;
                items[2].icon = ui::ControlIcon::trash;
                items[3].icon = ui::ControlIcon::unparent;
                if (const auto parts = world::components(runtime, row->object)) {
                    for (const auto& part : *parts) {
                        if (const auto proc = presentation::read_procedural_source(runtime, part.id())) {
                            const bool can_edit = !session.world().running();
                            if (!proc->source.empty()) {
                                json::Builder open_args;
                                static_cast<void>(open_args.set(open_args.root(), "asset", proc->source));
                                items.push_back({bind_operator({"graph.open", open_args.finish()}),
                                                 L"Edit Procedural Graph",
                                                 can_edit,
                                                 false,
                                                 {},
                                                 L"",
                                                 true});
                            }
                            bool has_instances = false;
                            if (proc->output)
                                for (const auto& col : proc->output->publication.collections)
                                    if (col.transforms && !col.transforms->empty())
                                        has_instances = true;
                            items.push_back(
                                {bind_operator({"procedural.bake",
                                                json::Document{},
                                                {.selection = SelectionContext{row->object, part.id(), {row->object}},
                                                 .region = RegionKind::scene_tree}}),
                                 L"Bake to Entities",
                                 can_edit && has_instances,
                                 false,
                                 {},
                                 L"",
                                 proc->source.empty()});
                            break;
                        }
                    }
                }
            } else if (const auto* group = menu_group(source->key)) {
                if (group->id == MenuGroup::view) {
                    items.push_back(operator_item({"view.frame_selected"}));
                    for (const auto& [key, title] : std::array<std::pair<ui::ControlKey, std::wstring_view>, 4>{
                             {{control::viewport_tools_menu, L"Interaction Mode"},
                              {control::viewport_pivot, L"Pivot"},
                              {control::viewport_snap, L"Snapping"},
                              {control::viewport_camera, L"Camera View"}}})
                        items.push_back({.key = key,
                                         .text = std::wstring(title),
                                         .edit_command = {},
                                         .children = viewport_calls(key)});
                    items.push_back(operator_item(
                        {"view.toggle", json::Document{json::Document::literal<R"({"option":"options"})">()}}));
                } else {
                    for (const auto& definition : menu_bindings)
                        if (definition.group == group->id) {
                            auto item = operator_item(definition.call, &definition);
                            items.push_back(std::move(item));
                        }
                }
            } else if (source->key == control::viewport && menu->group == control::viewport_shading_pie) {
                constexpr std::array titles{L"Wireframe", L"Solid", L"MatCap", L"Scene Lit"};
                const auto selected = this->view.wireframe == renderer::Wireframe::only    ? 0u
                                      : this->view.shading == renderer::Shading::matcap    ? 2u
                                      : this->view.shading == renderer::Shading::scene_lit ? 3u
                                                                                           : 1u;
                for (std::size_t i = 0; i < titles.size(); ++i) {
                    ui::MenuItem item{control::shading_preset_first + i, titles[i], true, i == selected, {}};
                    item.behavior = ui::MenuBehavior::radio;
                    item.radio_group = control::shading_preset_first;
                    items.push_back(std::move(item));
                }
            } else if (source->key == control::viewport && menu->group == control::viewport_delete_menu) {
                auto item = operator_item(
                    {"scene.delete", json::Document{json::Document::literal<R"({"target":"entities"})">()}});
                item.separator_before = false;
                item.text = session.selected_entities().size() == 1
                                ? L"Delete Entity"
                                : L"Delete " + std::to_wstring(session.selected_entities().size()) + L" Entities";
                items.push_back(std::move(item));
            } else if (source->key == control::viewport && menu->group == control::viewport_axis_pie) {
                items = calls({{"view.axis", json::Document{json::Document::literal<R"({"view":"left"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"right"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"bottom"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"top"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"front"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"back"})">()}},
                               {"view.axis", json::Document{json::Document::literal<R"({"view":"perspective"})">()}},
                               {"view.frame_selected"}});
                items.back().text = L"Focus";
            } else if (source->key == control::viewport && menu->group == control::viewport_add_menu) {
                // Blender Shift+A: everything the Create group can place at the 3D cursor.
                for (const auto& definition : menu_bindings)
                    if (definition.group == MenuGroup::create)
                        items.push_back(operator_item(definition.call));
            } else if (source->key == control::viewport || source->key == control::viewport_tools_menu) {
                items = viewport_calls(control::viewport_tools_menu);
                if (source->key == control::viewport) {
                    items.push_back({.key = control::viewport_snap,
                                     .text = L"Snap",
                                     .edit_command = {},
                                     .children = viewport_calls(control::viewport_snap)});
                    for (auto item : calls(
                             {{"view.frame_selected"},
                              {"scene.add", json::Document{json::Document::literal<R"({"kind":"mesh"})">()}},
                              {"scene.duplicate"},
                              {"scene.delete", json::Document{json::Document::literal<R"({"target":"entities"})">()}}}))
                        items.push_back(std::move(item));
                }
            } else if (source->key == control::viewport_snap) {
                items = viewport_calls(source->key);
                for (auto& item : items) {
                    const auto& call = binding(item.key)->call->request;
                    const auto args = call.args.root();
                    const auto setting_field = *args.find("setting");
                    const auto setting = setting_field ? *setting_field->string() : std::string_view{};
                    const auto grid = *args.find("grid_spacing");
                    const auto rotation = *args.find("rotation_step");
                    const auto destination = *args.find("to");
                    item.separator_before = false;
                    if (setting == "grid_snap")
                        item.heading = L"Snapping";
                    else if (grid && *grid->number() == .1)
                        item.heading = L"Increment";
                    else if (setting == "snap_move")
                        item.heading = L"Affect";
                    else if (rotation && *rotation->number() == 1.)
                        item.heading = L"Rotation Increment";
                    else if (call.id == "scene.snap_selection" && destination && *destination->string() == "grid")
                        item.heading = L"Snap Selection / Cursor";
                    item.separator_before = !item.heading.empty() && setting != "grid_snap";
                    // The section caption carries the context; rows show just the choice.
                    if (setting.starts_with("snap_"))
                        item.text = item.text.substr(5);
                    else if (rotation)
                        item.text = item.text.substr(9);
                }
            } else if (source->key == control::viewport_camera || source->key == control::viewport_pivot ||
                       source->key == control::viewport_orientation) {
                items = viewport_calls(source->key);
            } else if (control::is_path(source->key)) {
                items.push_back(
                    {control::world_context, L"World", true, active_graph.empty() && selected == ObjectId{}, {}});
                std::vector<std::string> graphs = session.open_graphs();
                if (project)
                    for (const auto& entry : project->entries())
                        if (entry.kind == assets::AssetKind::procedural_graph &&
                            std::ranges::find(graphs, entry.key) == graphs.end())
                            graphs.push_back(entry.key);
                for (std::size_t g = 0; g < graphs.size(); ++g) {
                    const auto& gkey = graphs[g];
                    std::wstring label = L"/" + platform::utf8_to_utf16_validated(gkey);
                    if (const auto* entry = project ? project->entry(gkey) : nullptr)
                        label = L"/" + entry->path.generic_wstring();
                    if (session.graph_dirty(gkey))
                        label += L" *";
                    json::Builder args;
                    static_cast<void>(args.set(args.root(), "asset", gkey));
                    items.push_back({bind_operator({"graph.open", args.finish()}),
                                     std::move(label),
                                     true,
                                     active_graph == gkey,
                                     {},
                                     L"",
                                     g == 0});
                }
                if (active_graph.empty())
                    for (std::size_t m = 0; m < members.size(); ++m)
                        items.push_back({bind(members[m].id(), {}, Binding::Kind::navigation),
                                         std::wstring(session.name(members[m].id())),
                                         true,
                                         selected == members[m].id(),
                                         {},
                                         L"",
                                         m == 0});
                for (auto& item : items) {
                    item.behavior = ui::MenuBehavior::radio;
                    item.radio_group = source->key;
                }
            } else if (source->key == network_control::path_combo) {
                const auto push_choice = [&](NetworkPathChoice choice, std::wstring title, bool checked,
                                             bool separator = false, std::wstring_view heading = {}) {
                    const ui::ControlKey ckey = network_control::path_choice_first + network_path_choices_.size();
                    network_path_choices_.push_back(std::move(choice));
                    ui::MenuItem item{ckey, std::move(title), true, checked, {}, L"", separator};
                    item.heading = std::wstring(heading);
                    item.behavior = ui::MenuBehavior::radio;
                    item.radio_group = source->key;
                    items.push_back(std::move(item));
                };
                push_choice({NetworkPathChoice::Kind::unpin_world, {}, {}}, L"World (Follow Selected Generator)",
                            active_graph.empty() && !network_pinned);

                bool first_gen = true;
                for (const auto& member : members) {
                    const auto resolved = session.resolve_procedural_generator(member.id());
                    if (!resolved)
                        continue;
                    const auto src = presentation::read_procedural_source(runtime, *resolved);
                    if (!src || src->source.empty())
                        continue;
                    std::wstring gname = platform::utf8_to_utf16_validated(src->source);
                    if (const auto* entry = project ? project->entry(src->source) : nullptr)
                        gname = entry->path.stem().wstring();
                    push_choice({NetworkPathChoice::Kind::follow_entity, member.id(), src->source},
                                std::wstring(session.name(member.id())) + L"  ▸  " + gname,
                                active_graph.empty() && !network_pinned && selected == member.id(), first_gen,
                                first_gen ? L"Scene Generators" : L"");
                    first_gen = false;
                }

                std::vector<std::string> graphs = session.open_graphs();
                if (project)
                    for (const auto& entry : project->entries())
                        if (entry.kind == assets::AssetKind::procedural_graph &&
                            std::ranges::find(graphs, entry.key) == graphs.end())
                            graphs.push_back(entry.key);
                for (std::size_t g = 0; g < graphs.size(); ++g) {
                    const auto& gkey = graphs[g];
                    std::wstring label = L"/" + platform::utf8_to_utf16_validated(gkey);
                    if (const auto* entry = project ? project->entry(gkey) : nullptr)
                        label = L"/" + entry->path.generic_wstring();
                    if (session.graph_dirty(gkey))
                        label += L" *";
                    push_choice({NetworkPathChoice::Kind::select_graph, {}, gkey}, std::move(label),
                                (!active_graph.empty() && active_graph == gkey) ||
                                    (active_graph.empty() && network_pinned && pinned_graph == gkey),
                                g == 0, g == 0 ? L"Procedural Graphs" : L"");
                }
                items.push_back({network_control::new_scratch,
                                 L"+ New Scratch Graph",
                                 !session.world().running(),
                                 false,
                                 {},
                                 L"",
                                 true});
                if (!active_graph.empty())
                    items.push_back({bind_operator({"graph.exit"}), L"Return 3D Viewport to World", true, false, {}});
            } else if (network_control::is_header_menu(source->key)) {
                const bool has_net = !net_graph.empty() && session.has_graph(net_graph);
                const bool can_edit = has_net && !session.world().running();
                const bool has_sel = has_net && (!selected_graph_node.empty() || !selected_graph_nodes.empty());
                const auto* graph_doc = has_net ? session.graph_document(net_graph) : nullptr;
                const bool has_nodes = graph_doc && !graph_doc->nodes.empty();
                const auto net_op = [&](std::string id, json::Document args = {}) {
                    return bind_operator(
                        {std::move(id), std::move(args), {.selection = {}, .region = RegionKind::network}});
                };
                if (source->key == network_control::menu_add) {
                    items.push_back({network_control::add_node, L"Add SOP Node…", can_edit, false, {}, L"Tab"});
                    items.push_back({network_control::add_sticky_note, L"Add Sticky Note", can_edit, false, {}, L"N"});
                    items.push_back({network_control::add_network_box, L"Add Network Box", can_edit, false, {}, L"O"});
                    items.push_back({network_control::new_scratch,
                                     L"+ New Scratch Graph",
                                     !session.world().running(),
                                     false,
                                     {},
                                     L"",
                                     true});
                } else if (source->key == network_control::menu_edit) {
                    items.push_back({net_op("edit.undo"),
                                     L"Undo",
                                     has_net && session.graph_can_undo(net_graph, false),
                                     false,
                                     {},
                                     L"Ctrl+Z"});
                    items.push_back({net_op("edit.redo"),
                                     L"Redo",
                                     has_net && session.graph_can_undo(net_graph, true),
                                     false,
                                     {},
                                     L"Ctrl+Y"});
                    items.push_back({net_op("graph.duplicate_nodes"),
                                     L"Duplicate",
                                     can_edit && has_sel,
                                     false,
                                     {},
                                     L"Shift+D",
                                     true});
                    items.push_back(
                        {net_op("graph.delete_node"), L"Delete", can_edit && has_sel, false, {}, L"Del / X"});
                    items.push_back(
                        {network_control::select_all_nodes, L"Select All", has_nodes, false, {}, L"A", true});
                    items.push_back(
                        {network_control::deselect_all_nodes, L"Deselect All", has_sel, false, {}, L"Alt+A"});
                    items.push_back(
                        {network_control::invert_select_nodes, L"Invert Selection", has_nodes, false, {}, L"Ctrl+I"});
                    if (!selected_graph_node.empty()) {
                        const ObjectId sel_id = session.graph_node_id(net_graph, selected_graph_node);
                        bool is_frz = false;
                        if (sel_id != ObjectId{}) {
                            if (const auto frz = procedural::frozen(runtime, sel_id))
                                is_frz = *frz;
                        }
                        json::Builder frz_args;
                        static_cast<void>(frz_args.set(frz_args.root(), "node", selected_graph_node));
                        static_cast<void>(frz_args.set(frz_args.root(), "frozen", !is_frz));
                        items.push_back({net_op("graph.set_frozen", frz_args.finish()),
                                         is_frz ? L"Unfreeze Node" : L"Freeze Node",
                                         can_edit && sel_id != ObjectId{},
                                         is_frz,
                                         {},
                                         L"",
                                         true});
                        json::Builder disc_args;
                        static_cast<void>(disc_args.set(disc_args.root(), "to_node", selected_graph_node));
                        items.push_back({net_op("graph.disconnect", disc_args.finish()),
                                         L"Disconnect All Inputs",
                                         can_edit && sel_id != ObjectId{},
                                         false,
                                         {},
                                         L""});
                    }
                } else if (source->key == network_control::menu_view) {
                    items.push_back({network_control::frame_selected, L"Frame Selection", has_net, false, {}, L"F"});
                    items.push_back({network_control::frame_all, L"Frame All", has_net, false, {}, L"H"});
                    items.push_back({network_control::zoom_reset, L"Reset Zoom (100%)", has_net, false, {}});
                    items.push_back(
                        {network_control::open_node_info, L"Node Info & Comment…", has_sel, false, {}, L"I", true});
                    json::Builder seed_args;
                    seed_args.set_validated(seed_args.root(), "node", "");
                    items.push_back(
                        {net_op("graph.select", seed_args.finish()), L"Inspect Graph Seed", has_net, false, {}});
                    ui::MenuItem iso{network_control::isolate_toggle,
                                     L"Isolate SOP in 3D Viewport",
                                     has_net || !active_graph.empty(),
                                     !active_graph.empty(),
                                     {},
                                     L"",
                                     true};
                    iso.behavior = ui::MenuBehavior::checkbox;
                    items.push_back(std::move(iso));
                } else if (source->key == network_control::menu_layout) {
                    items.push_back({net_op("graph.layout"), L"Auto Arrange Nodes", can_edit, false, {}, L"L"});
                    items.push_back(
                        {network_control::open_shape_picker, L"Shape Palette…", can_edit, false, {}, L"Z", true});
                    items.push_back({network_control::open_color_picker, L"Color Palette…", can_edit, false, {}, L"C"});
                }
            } else if (source->key == network_control::canvas) {
                const bool has_net = !net_graph.empty() && session.has_graph(net_graph);
                const bool can_edit = has_net && !session.world().running();
                const auto net_op = [&](std::string id, json::Document args = {}) {
                    return bind_operator(
                        {std::move(id), std::move(args), {.selection = {}, .region = RegionKind::network}});
                };
                if (menu->group == network_control::context_node_menu && !network_context_node.empty()) {
                    const auto* graph_doc = has_net ? session.graph_document(net_graph) : nullptr;
                    const ObjectId target_id =
                        graph_doc ? session.graph_node_id(net_graph, network_context_node) : ObjectId{};
                    bool is_disp = false;
                    bool is_tmpl = false;
                    bool is_byp = false;
                    bool is_frz = false;
                    if (graph_doc && graph_doc->graph.registered()) {
                        if (const auto listed = procedural::contents(runtime, graph_doc->graph.id()))
                            is_disp = target_id != ObjectId{} && listed->display() == target_id;
                        is_tmpl = target_id != ObjectId{} && graph_doc->template_node == target_id;
                        if (target_id != ObjectId{}) {
                            if (const auto byp = procedural::bypassed(runtime, target_id))
                                is_byp = *byp;
                            if (const auto frz = procedural::frozen(runtime, target_id))
                                is_frz = *frz;
                        }
                    }
                    {
                        json::Builder args;
                        static_cast<void>(
                            args.set(args.root(), "node",
                                     is_disp ? std::string_view{} : std::string_view{network_context_node}));
                        items.push_back({net_op("graph.set_display", args.finish()),
                                         is_disp ? L"Clear Display Flag" : L"Set Display Flag",
                                         can_edit && target_id != ObjectId{},
                                         is_disp,
                                         {},
                                         L"D"});
                    }
                    {
                        json::Builder args;
                        static_cast<void>(
                            args.set(args.root(), "node",
                                     is_tmpl ? std::string_view{} : std::string_view{network_context_node}));
                        items.push_back({net_op("graph.set_template", args.finish()),
                                         is_tmpl ? L"Clear Template Flag" : L"Set Template Flag",
                                         can_edit && target_id != ObjectId{},
                                         is_tmpl,
                                         {},
                                         L"T"});
                    }
                    {
                        json::Builder args;
                        static_cast<void>(args.set(args.root(), "node", network_context_node));
                        static_cast<void>(args.set(args.root(), "bypass", !is_byp));
                        items.push_back({net_op("graph.set_bypass", args.finish()),
                                         is_byp ? L"Disable Bypass" : L"Bypass Node",
                                         can_edit && target_id != ObjectId{},
                                         is_byp,
                                         {},
                                         L"B"});
                    }
                    {
                        json::Builder args;
                        static_cast<void>(args.set(args.root(), "node", network_context_node));
                        static_cast<void>(args.set(args.root(), "frozen", !is_frz));
                        items.push_back({net_op("graph.set_frozen", args.finish()),
                                         is_frz ? L"Unfreeze Node" : L"Freeze Node",
                                         can_edit && target_id != ObjectId{},
                                         is_frz,
                                         {},
                                         L""});
                    }
                    items.push_back(
                        {network_control::open_node_info, L"Node Info & Comment…", true, false, {}, L"I", true});
                    items.push_back({network_control::open_shape_picker, L"Change Shape…", can_edit, false, {}, L"Z"});
                    items.push_back({network_control::open_color_picker, L"Change Color…", can_edit, false, {}, L"C"});
                    {
                        json::Builder dup_args;
                        if (std::ranges::find(selected_graph_nodes, network_context_node) == selected_graph_nodes.end())
                            static_cast<void>(dup_args.set(dup_args.root(), "node", network_context_node));
                        items.push_back({net_op("graph.duplicate_nodes", dup_args.finish()),
                                         L"Duplicate",
                                         can_edit,
                                         false,
                                         {},
                                         L"Shift+D",
                                         true});
                    }
                    if (can_edit && target_id != ObjectId{}) {
                        json::Builder disc_args;
                        static_cast<void>(disc_args.set(disc_args.root(), "to_node", network_context_node));
                        items.push_back({net_op("graph.disconnect", disc_args.finish()),
                                         L"Disconnect All Inputs",
                                         true,
                                         false,
                                         {},
                                         L"",
                                         true});
                    }
                    {
                        json::Builder del_args;
                        static_cast<void>(del_args.set(del_args.root(), "name", network_context_node));
                        items.push_back({net_op("graph.delete_node", del_args.finish()),
                                         L"Delete Node",
                                         can_edit,
                                         false,
                                         {},
                                         L"Del",
                                         true});
                    }
                } else {
                    items.push_back({network_control::add_node, L"Add Node…", can_edit, false, {}, L"Tab"});
                    items.push_back({network_control::add_sticky_note, L"Add Sticky Note", can_edit, false, {}, L"N"});
                    items.push_back({network_control::add_network_box, L"Add Network Box", can_edit, false, {}, L"O"});
                    items.push_back({network_control::frame_all, L"Frame All", has_net, false, {}, L"H", true});
                    items.push_back({net_op("graph.layout"), L"Auto Layout", can_edit, false, {}, L"L"});
                    json::Builder seed_args;
                    seed_args.set_validated(seed_args.root(), "node", "");
                    items.push_back({net_op("graph.select", seed_args.finish()),
                                     L"Inspect Graph Seed",
                                     has_net,
                                     false,
                                     {},
                                     L"",
                                     true});
                    items.push_back(
                        {network_control::isolate_toggle,
                         active_graph.empty() ? L"Isolate SOP in 3D Viewport" : L"Return 3D Viewport to World",
                         has_net || !active_graph.empty(),
                         !active_graph.empty(),
                         {}});
                }
            } else if (const auto* target = binding(source->context_target);
                       source->kind == ControlKind::combo && target && target->kind == Binding::Kind::field) {
                auto value = session.read_property(target->object, target->path);
                if (value)
                    if (const auto* enumeration = std::get_if<EnumValue>(&*value)) {
                        const SelectionContext enum_selection =
                            session.is_graph_object(target->object)
                                ? SelectionContext{ObjectId{}, target->object, {}}
                                : SelectionContext{selected, target->object, {selected}};
                        OperatorCall call{"scene.set_properties",
                                          json::Document{},
                                          {.selection = enum_selection, .region = RegionKind::parameters}};
                        auto type = take(runtime.type_of(target->object));
                        json::Builder args;
                        auto argument = args.root();
                        std::string_view name;
                        for (std::size_t i = 0; i < target->path.size(); ++i) {
                            const auto info = take(runtime.describe(type));
                            const auto at = std::ranges::find(info->fields, target->path[i], &FieldInfo::id);
                            if (i + 1 < target->path.size())
                                argument = args.object_validated(argument, at->name);
                            else
                                name = at->name;
                            type = at->value.type;
                        }
                        auto context = operator_context(session, project, true, command_focus);
                        context.selection = *call.context.selection;
                        context.region = *call.context.region;
                        const bool available = operators.find(call.id)->poll(context).has_value();
                        const auto& enum_info = *take(runtime.describe(enumeration->type))->enumeration;
                        for (std::size_t i = 0; i < enum_info.values.size(); ++i) {
                            const auto& member = enum_info.values[i];
                            const std::wstring title =
                                i < enum_info.metadata.size() && !enum_info.metadata[i].label.empty()
                                    ? platform::utf8_to_utf16_validated(enum_info.metadata[i].label)
                                    : parameter_label(member.first);
                            bool checked = false;
                            if (enum_info.flags) {
                                checked = member.second == 0 ? enumeration->bits == 0
                                                             : (enumeration->bits & member.second) == member.second;
                                const std::uint32_t next_bits = member.second == 0 ? 0u
                                                                : checked ? (enumeration->bits & ~member.second)
                                                                          : (enumeration->bits | member.second);
                                const auto exact = std::ranges::find(enum_info.values, next_bits,
                                                                     &std::pair<std::string, std::uint32_t>::second);
                                if (exact != enum_info.values.end())
                                    args.set_validated(argument, name, exact->first);
                                else if (auto added = args.set(argument, name, static_cast<std::uint64_t>(next_bits));
                                         !added)
                                    fatal(added.error().describe());
                            } else {
                                checked = member.second == enumeration->bits;
                                args.set_validated(argument, name, member.first);
                            }
                            call.args = args.finish();
                            ui::MenuItem item{bind_operator(call), title, available, checked, {}};
                            item.behavior = enum_info.flags ? ui::MenuBehavior::checkbox : ui::MenuBehavior::radio;
                            item.radio_group = enum_info.flags ? ui::ControlKey{} : source->key;
                            items.push_back(std::move(item));
                        }
                    }
            } else if (const auto* target = binding(source->context_target);
                       target && target->kind == Binding::Kind::procedural_graph_slot) {
                if (const auto proc = presentation::read_procedural_source(runtime, target->object)) {
                    const bool can_edit = !session.world().running();
                    const auto comp_status = world::inspect_component(runtime, target->object);
                    const ObjectId owner_entity = comp_status ? comp_status->owner : selected;
                    const SelectionContext gen_sel{owner_entity, target->object,
                                                   owner_entity == ObjectId{} ? std::vector<ObjectId>{}
                                                                              : std::vector<ObjectId>{owner_entity}};
                    {
                        json::Builder clear_args;
                        clear_args.set_validated(clear_args.root(), "asset", "");
                        ui::MenuItem none_item{
                            bind_operator({"procedural.assign_graph",
                                           clear_args.finish(),
                                           {.selection = gen_sel, .region = RegionKind::parameters}}),
                            L"None (Clear Graph)",
                            can_edit,
                            proc->source.empty(),
                            {}};
                        none_item.behavior = ui::MenuBehavior::radio;
                        none_item.radio_group = source->key;
                        items.push_back(std::move(none_item));
                    }
                    if (!proc->source.empty()) {
                        json::Builder open_args;
                        static_cast<void>(open_args.set(open_args.root(), "asset", proc->source));
                        items.push_back({bind_operator({"graph.open", open_args.finish()}),
                                         L"Edit Graph in Network",
                                         can_edit,
                                         false,
                                         {}});
                    }
                    bool has_instances = false;
                    if (proc->output)
                        for (const auto& col : proc->output->publication.collections)
                            if (col.transforms && !col.transforms->empty())
                                has_instances = true;
                    items.push_back({bind_operator({"procedural.bake",
                                                    json::Document{},
                                                    {.selection = gen_sel, .region = RegionKind::parameters}}),
                                     L"Bake Instances to Entities",
                                     can_edit && has_instances,
                                     false,
                                     {}});
                    std::vector<std::pair<std::string, std::wstring>> choices;
                    if (project) {
                        for (const auto& entry : project->entries())
                            if (entry.kind == assets::AssetKind::procedural_graph)
                                choices.emplace_back(entry.key, entry.path.stem().wstring());
                    }
                    for (const auto& open_key : session.open_graphs()) {
                        if (session.is_scratch_graph(open_key))
                            continue;
                        if (std::ranges::find(choices, open_key, &std::pair<std::string, std::wstring>::first) ==
                            choices.end())
                            choices.emplace_back(open_key, platform::utf8_to_utf16_validated(open_key));
                    }
                    for (std::size_t i = 0; i < choices.size(); ++i) {
                        json::Builder assign_args;
                        static_cast<void>(assign_args.set(assign_args.root(), "asset", choices[i].first));
                        ui::MenuItem item{bind_operator({"procedural.assign_graph",
                                                         assign_args.finish(),
                                                         {.selection = gen_sel, .region = RegionKind::parameters}}),
                                          choices[i].second,
                                          can_edit,
                                          proc->source == choices[i].first,
                                          {},
                                          L"",
                                          i == 0};
                        item.behavior = ui::MenuBehavior::radio;
                        item.radio_group = source->key;
                        items.push_back(std::move(item));
                    }
                }
            } else if (const auto* target = binding(source->context_target);
                       target && (target->kind == Binding::Kind::material_slot ||
                                  target->kind == Binding::Kind::material_texture || !target->material_path.empty())) {
                items = material_menu(*target, session, project);
            } else if (auto asset_items = assets.menu_items(source->key); !asset_items.empty()) {
                items = std::move(asset_items);
                if (source->key == browser_control::actions || source->key == browser_control::results)
                    for (const auto& definition : menu_bindings)
                        if (definition.call.id.starts_with("asset."))
                            items.push_back(operator_item(definition.call, &definition));
            } else if (source->key == control::scene_shading) {
                const auto group = [&](ui::ControlKey first, const auto& titles, std::size_t selected) {
                    for (std::size_t i = 0; i < titles.size(); ++i) {
                        ui::MenuItem item{first + i, titles[i], true, selected == i, {}};
                        item.behavior = ui::MenuBehavior::radio;
                        item.radio_group = first;
                        item.separator_before = i == 0 && !items.empty();
                        items.push_back(std::move(item));
                    }
                };
                group(control::shading_first, shading_titles, static_cast<unsigned>(this->view.shading));
                ui::MenuItem lit{control::shading_scene_lit,
                                 L"Scene Lit",
                                 true,
                                 this->view.shading == renderer::Shading::scene_lit,
                                 {}};
                lit.behavior = ui::MenuBehavior::radio;
                lit.radio_group = control::shading_first;
                items.push_back(std::move(lit));
                group(control::edges_first, edge_titles, edge_option);
                group(control::topology_first, topology_titles, this->view.triangle_edges ? 1u : 0u);
            } else if (source->key == control::viewport_cavity) {
                const bool available = this->view.shading != renderer::Shading::scene_lit &&
                                       this->view.wireframe != renderer::Wireframe::only;
                ui::MenuItem enabled{
                    control::cavity_enabled, L"Screen Cavity", available, this->view.cavity.enabled, {}};
                enabled.behavior = ui::MenuBehavior::checkbox;
                items.push_back(std::move(enabled));
                for (std::size_t group = 0; group < cavity_options.size(); ++group) {
                    const auto& option = cavity_options[group];
                    ui::MenuItem parent{control::cavity_group_first + group,
                                        std::format(L"{}: {:g}{}", option.title, view_value(this->view, option.option),
                                                    group == 2 ? L" px" : L""),
                                        available && this->view.cavity.enabled,
                                        false,
                                        {},
                                        L"",
                                        group == 0};
                    for (std::size_t index = 0; index < cavity_values[group].size(); ++index) {
                        const auto value = cavity_values[group][index];
                        ui::MenuItem child{control::cavity_value_first + group * 5 + index,
                                           std::format(L"{:g}{}", value, group == 2 ? L" px" : L""),
                                           true,
                                           view_value(this->view, option.option) == value,
                                           {}};
                        child.behavior = ui::MenuBehavior::radio;
                        child.radio_group = parent.key;
                        parent.children.push_back(std::move(child));
                    }
                    items.push_back(std::move(parent));
                }
            } else if (source->key == source_control::files) {
                items = source_editor.menu_items();
            } else if (source->key == control::add_component) {
                items.push_back(operator_item({take(world::has_transform(runtime, selected)) ? "scene.transform_remove"
                                                                                             : "scene.transform_add"}));
                component_choices_ = take(session.component_types());
                for (std::size_t i = 0; i < component_choices_.size(); ++i)
                    items.push_back({control::component_choice_first + i,
                                     platform::utf8_to_utf16_validated(component_choices_[i].name),
                                     selected != ObjectId{},
                                     false,
                                     {}});
            } else if (source->key == control::scene_guides) {
                const bool studio = view.shading != renderer::Shading::scene_lit;
                items.push_back({control::guide_background,
                                 L"Gradient Background",
                                 studio,
                                 studio && view.background == renderer::Background::gradient,
                                 {}});
                items.push_back({control::guide_grid, L"Reference Grid", true, view.reference_grid, {}});
                items.push_back({control::guide_axes, L"Origin Axes", true, view.origin_axes, {}});
                items.push_back(
                    {control::display_backfaces, L"Backface Winding Tint", studio, studio && view.backface_tint, {}});
                items.push_back(
                    operator_item({"view.toggle", json::Document{json::Document::literal<R"({"option":"fps"})">()}}));
                const bool in_sop = !active_graph.empty() && session.has_graph(active_graph);
                const ObjectId gen_comp = in_sop ? ObjectId{} : active_procedural_generator(session);
                if (in_sop || gen_comp != ObjectId{}) {
                    const procedural::Geometry* menu_geom{};
                    if (in_sop) {
                        if (const auto eval = session.evaluate_graph(active_graph, project))
                            menu_geom = eval->preview_output.get();
                    } else if (const auto src = presentation::read_procedural_source(runtime, gen_comp);
                               src && src->output) {
                        menu_geom = src->output->geometry.get();
                    }
                    items.push_back({control::display_sop_points,
                                     L"Procedural Points",
                                     true,
                                     effective_show_points(menu_geom),
                                     {},
                                     L"",
                                     true});
                    items.push_back({control::display_sop_normals, L"Point Normals (N)", true, sop_show_normals, {}});
                    items.push_back(
                        {control::display_sop_face_normals, L"Primitive Normals", true, sop_show_face_normals, {}});
                    items.push_back({control::display_sop_orient, L"Orient & pscale Axes", true, sop_show_orient, {}});
                    items.push_back(
                        {control::display_sop_bounds, L"Bounds & Spatial Chunks", true, sop_show_bounds, {}});
                    items.push_back({control::display_sop_info, L"Geometry Info HUD", true, sop_show_info, {}});
                }
                for (auto& item : items)
                    item.behavior = ui::MenuBehavior::checkbox;
            } else if (!items.empty()) {
            } else if (source->kind == ControlKind::combo && source->key == workbench_control::mode) {
                for (std::size_t i = 0; i < ControlWorkbench::mode_names.size(); ++i)
                    items.push_back({workbench_control::mode_first + i,
                                     ControlWorkbench::mode_names[i],
                                     true,
                                     workbench.values().mode == i,
                                     {}});
                for (auto& item : items) {
                    item.behavior = ui::MenuBehavior::radio;
                    item.radio_group = source->key;
                }
            } else {
                if (source->kind == ControlKind::number)
                    items.push_back({control::menu_reset, L"Revert to Default", source->modified, false,
                                     ui::EditCommand{ui::EditCommandKind::reset, source->key}});
                const auto* group =
                    menu->group ? find(menu->group) : (source->kind == ControlKind::label ? source : nullptr);
                if (group)
                    items.push_back(
                        {control::menu_group_reset, L"Revert Tuple to Defaults", group->modified, false, {}});
                if (source->kind == ControlKind::number || source->kind == ControlKind::text_edit) {
                    items.push_back({control::menu_copy, L"Copy Value", true, false,
                                     ui::EditCommand{ui::EditCommandKind::copy, source->key}});
                    items.push_back({control::menu_paste, L"Paste Value", true, false,
                                     ui::EditCommand{ui::EditCommandKind::paste, source->key}});
                }
            }
            if (items.empty())
                menu.reset();
            else
                menu_items = std::move(items);
        }
    }
    if (pie_menu() && !menu_items.empty()) {
        const auto dpi = frame ? frame->drawing->dpi_scale : 1.f;
        auto entries = std::make_shared<std::vector<ui_language::Item>>();
        constexpr std::array<std::pair<double, double>, 8> positions{
            {{60, 104}, {300, 104}, {180, 181}, {180, 27}, {72, 55}, {288, 55}, {72, 153}, {288, 153}}};
        for (std::size_t i = 0; i < menu_items.size(); ++i) {
            const auto& item = menu_items[i];
            entries->push_back(
                {item.key,
                 {item.text, item.enabled, item.selected, positions[i].first, positions[i].second, item.radio_group,
                  std::wstring(item.behavior == ui::MenuBehavior::radio ? L"radio" : L"command")}});
        }
        const std::array<ui_language::InputValue, 4> inputs{
            double(std::clamp(menu->x / dpi - 180.f, 0.f, std::max(0.f, size.width - 360.f))),
            double(std::clamp(menu->y / dpi - 104.f, 0.f, std::max(0.f, size.height - 208.f))),
            std::wstring(menu->group == control::viewport_axis_pie ? L"View" : L"Shading"),
            ui_language::ItemList{entries}};
        const std::array<ui_language::KeyBinding, 1> keys{{{"menu", control::viewport_pie}}};
        static_cast<void>(ui_document_->append("ViewportPie", 0, keys, inputs, result));
    }
    if (row_rename && row_rename->object != ObjectId{} && !live.contains(row_rename->object))
        row_rename.reset();
    if (row_rename && frame) {
        // Drawn over the accepted row geometry. A row not presented yet keeps the rename
        // pending (its host keeps it described); a table cell that left the frame ends it.
        const auto* table = row_rename->cell ? frame->find_table(browser_control::results) : nullptr;
        const auto cell = table ? std::ranges::find(table->cells, row_rename->cell, &ui::TableCellGeometry::cell)
                                : std::vector<ui::TableCellGeometry>::const_iterator{};
        const auto row_key = row_rename->cell ? ui::ControlKey{} : object_control(row_rename->object);
        const auto* row = row_key ? frame->find(row_key) : nullptr;
        const auto geometry =
            row ? std::ranges::find(frame->tree_rows, row_key, &ui::TreeRowGeometry::key) : frame->tree_rows.end();
        if (row_rename->cell && (!table || cell == table->cells.end()))
            row_rename.reset();
        else if (row_rename->cell) {
            // Name cell: the icon leads by one row height; the field starts where the name text does.
            const auto dpi = frame->drawing->dpi_scale;
            const auto& bounds = cell->bounds;
            const auto icon = bounds.height / dpi;
            const std::array<ui_language::InputValue, 5> inputs{
                ui_language::TextEditBinding{control::inline_rename, row_rename->text}, double(bounds.x / dpi + icon),
                double(bounds.y / dpi), double(std::max(48.f, bounds.width / dpi - icon - 2)),
                double(bounds.height / dpi)};
            static_cast<void>(ui_document_->append("InlineRename", 0, {}, inputs, result));
        } else if (row && (row->bounds.height <= 0 || geometry == frame->tree_rows.end()))
            row_rename.reset();
        else if (row) {
            const auto dpi = frame->drawing->dpi_scale;
            // Text origin as Session lays it out: depth indents plus the row icon's leading space.
            const auto* tree = frame->find(row->parent);
            const auto step = tree && tree->style ? tree->style->tree_indent : 17.f;
            const auto indent = row->style ? row->style->padding.left + float(geometry->depth + 1) * step +
                                                 row->style->indicator_size + 6 - 3
                                           : 24.f;
            const auto padding = float(row->bounds.x) / dpi + indent;
            const std::array<ui_language::InputValue, 5> inputs{
                ui_language::TextEditBinding{control::inline_rename, row_rename->text}, double(padding),
                double(row->bounds.y / dpi), double(std::max(48.f, row->bounds.width / dpi - indent - 2)),
                double(row->bounds.height / dpi)};
            static_cast<void>(ui_document_->append("InlineRename", 0, {}, inputs, result));
        } else if (!dock.visible(control::entities_tab) ||
                   std::ranges::none_of(scene_tree_.rows, [&](const auto& shown) {
                       return scene_tree_.objects[shown.node] == row_rename->object;
                   }))
            row_rename.reset();
    }
    if (color_edit && !pie_menu()) {
        // One popup per session; the colour editor is projected last, after every owner.
        const auto& edit = *color_edit;
        const auto hsv = rgb_to_hsv(edit.value);
        const auto field = [](ui::ControlKey key, const wchar_t* label, double value, double maximum, double step,
                              ui::VectorAxis axis) {
            ui::NumberField result{key, label, value, value, 0, maximum, step, step * .1, axis};
            result.slider.reset();
            return result;
        };
        ui_language::EditBinding rgb{
            control::color_group,
            true,
            false,
            {field(control::color_rgb, L"R", edit.value[0], edit.maximum, .005, ui::VectorAxis::x),
             field(control::color_rgb + 1, L"G", edit.value[1], edit.maximum, .005, ui::VectorAxis::y),
             field(control::color_rgb + 2, L"B", edit.value[2], edit.maximum, .005, ui::VectorAxis::z)}};
        if (edit.material_entry)
            for (auto& channel : rgb.fields) {
                channel.minimum = -edit.maximum;
                channel.single_precision = true;
            }
        ui_language::EditBinding hsv_edit{
            control::color_group + 1,
            true,
            false,
            {field(control::color_hsv, L"H", hsv[0], 360, 1, ui::VectorAxis::none),
             field(control::color_hsv + 1, L"S", hsv[1], 1, .005, ui::VectorAxis::none),
             field(control::color_hsv + 2, L"V", hsv[2], edit.maximum, .005, ui::VectorAxis::none)}};
        const auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
        const std::array<ui_language::InputValue, 13> inputs{
            double(std::clamp(edit.x, 0.f, std::max(0.f, size.width - 420.f))),
            double(std::clamp(edit.y, 0.f, std::max(0.f, size.height - 200.f))),
            clamp01(edit.value[0]),
            clamp01(edit.value[1]),
            clamp01(edit.value[2]),
            clamp01(edit.original[0]),
            clamp01(edit.original[1]),
            clamp01(edit.original[2]),
            std::move(rgb),
            std::move(hsv_edit),
            ui_language::TextEditBinding{control::color_hex, edit.hex},
            ui_language::ActionBinding{control::color_revert, true},
            ui_language::ActionBinding{control::color_done, true}};
        const std::array<ui_language::KeyBinding, 1> keys{{{"editor", control::color_editor}}};
        color_edit->composition_failed = !ui_document_->append("ColorEditor", 0, keys, inputs, result);
    }
    if (ui_document_->candidate_failed()) {
        reject_ui_controls(std::string(ui_error()));
        return describe(result, session, size, frame, project, ui, command_focus);
    }
    const auto set_help = [&](ui::Control& ctrl, std::wstring_view title, std::wstring_view identity,
                              std::string_view desc) {
        if (!ctrl.tooltip)
            ctrl.tooltip = property_help_.get(ctrl.key, title, identity, desc);
    };
    result.finish([&](ui::Control& ctrl) {
        if (ctrl.tooltip)
            return;
        switch (ctrl.key) {
        case control::menu_bar_handle:
            set_help(ctrl, L"Toggle Menu Bar", menu_bar_collapsed ? L"Currently: Stowed" : L"Currently: Expanded",
                     "收起或展开窗口顶部的主菜单栏，腾出更多工作区纵向空间。");
            break;
        case control::shelf_handle:
            set_help(ctrl, L"Toggle Tool Shelf", shelf_collapsed ? L"Currently: Stowed" : L"Currently: Expanded",
                     "收起或展开顶部工具架（Create / View 快捷图标栏）。");
            break;
        case control::shelf_create:
            set_help(ctrl, L"Create Shelf", L"Shelf Tab",
                     "切换至创建工具架，快速在 3D 游标位置添加实体、生成器与光源。");
            break;
        case control::shelf_view:
            set_help(ctrl, L"View Shelf", L"Shelf Tab", "切换至视图工具架，快速框显目标或重置相机观察视角。");
            break;
        case control::scene_back:
        case control::entity_back:
        case control::parameter_back:
        case network_control::nav_back:
            set_help(ctrl, L"Navigate Back", L"Selection History", "返回上一个检视的场景对象或上下文位置。");
            break;
        case control::scene_forward:
        case control::entity_forward:
        case control::parameter_forward:
        case network_control::nav_forward:
            set_help(ctrl, L"Navigate Forward", L"Selection History", "前进到下一个检视的场景对象或上下文位置。");
            break;
        case control::scene_path:
        case control::entity_path:
        case control::parameter_path:
            set_help(ctrl, L"Context Path", L"Navigation Breadcrumb",
                     "显示当前所处的场景世界（World）或程序化图（SOP）路径；点击可快速切换层级。");
            break;
        case network_control::path_combo:
            set_help(
                ctrl, L"Network Context Path", L"Network Breadcrumb",
                "显示当前网络面板关联的世界生成器实体（World ▸ Entity ▸ Graph）或固定/独显图路径；点击可快速切换场景生成器或项目 .procgraph。");
            break;
        case control::viewport_tools_menu:
            set_help(ctrl, L"Active Tool Menu [W]", L"Viewport Header", "打开当前交互工具与快捷操作菜单。");
            break;
        case control::viewport_view_menu:
            set_help(ctrl, L"View Menu", L"Viewport Header", "打开相机框显、轴向视图、光照覆盖范围与视口显示菜单。");
            break;
        case control::viewport_select_menu:
            set_help(ctrl, L"Select Menu", L"Viewport Header", "打开全选、反选、框选及选择布尔模式菜单。");
            break;
        case control::viewport_header_add:
            set_help(ctrl, L"Add Entity Menu [Shift+A]", L"Viewport Header",
                     "在当前 3D 游标位置创建网格实体、程序化生成器、相机或光源。");
            break;
        case control::viewport_object_menu:
            set_help(ctrl, L"Object Menu", L"Viewport Header",
                     "打开选中实体的复制、重命名、父子层级、变换清零与程序化烘焙菜单。");
            break;
        case control::viewport_pivot:
            set_help(ctrl, L"Transform Pivot", L"Viewport Header",
                     "切换旋转与缩放的枢轴中心（选中集合中心 Selection Center 或 3D Cursor）。");
            break;
        case control::viewport_snap:
            set_help(ctrl, L"Snap Settings", L"Viewport Header",
                     "设置参考网格吸附步长（0.1m – 1m）、旋转步进角度以及游标/选中项对齐操作。");
            break;
        case control::scene_guides:
            set_help(ctrl, L"Viewport Guides & Overlays", L"Viewport Header",
                     "统一管理背景渐变、参考网格、原点坐标轴、背面绕序高亮以及程序化几何诊断叠加层。");
            break;
        case control::viewport_cavity:
            set_help(ctrl, L"Cavity Settings", L"Studio Shading",
                     "调节屏幕空间棱角高光（Ridge）、凹缝暗部（Valley）强度与采样半径。");
            break;
        case control::scene_shading:
            set_help(ctrl, L"Shading & Wireframe Options", L"Viewport Header",
                     "精细选择着色模式（Smooth / Flat / MatCap / Scene Lit）及线框拓扑绘制方式。");
            break;
        case control::viewport_orientation:
            set_help(ctrl, L"Transform Orientation", L"Tool Options",
                     "切换变换手柄的参考坐标系：世界坐标（Global）或实体自身局部坐标（Local）。");
            break;
        case control::viewport_camera:
            set_help(ctrl, L"Camera Viewpoint [`]", L"Viewport Overlay",
                     "在透视视图（Perspective）、正交六视图（Top/Front/Right 等）及主场景相机之间切换。");
            break;
        case control::add_component:
            set_help(ctrl, L"Add Component", L"Entity Structure", "向当前选中的实体挂载空间变换或业务功能组件。");
            break;
        case control::parameter_search_toggle:
            set_help(ctrl, L"Filter Parameters", parameter_search_visible ? L"Currently: Open" : L"Currently: Closed",
                     "展开或收起属性名称搜索栏，按关键字快速过滤当前对象的参数行。");
            break;
        case control::group_components:
            set_help(ctrl, L"Group by Component", L"Parameters Layout", "按实体挂载的组件归属分组展示属性。");
            break;
        case control::group_categories:
            set_help(ctrl, L"Group by Category", L"Parameters Layout", "按功能语义分类合并展示所有组件属性。");
            break;
        case browser_control::back:
            set_help(ctrl, L"Back", L"Asset Browser", "返回上一个浏览的资源目录。");
            break;
        case browser_control::up:
            set_help(ctrl, L"Up One Folder", L"Asset Browser", "返回当前项目文件夹的上一级父目录。");
            break;
        case browser_control::path:
            set_help(ctrl, L"Folder Path", L"Asset Browser", "显示当前浏览目录路径，点击可在常用根目录间跳转。");
            break;
        case browser_control::clear:
            set_help(ctrl, L"Clear Search & Filter", L"Asset Browser", "清空当前搜索关键字并重置资源类型过滤条件。");
            break;
        case browser_control::filter:
            set_help(ctrl, L"Filter by Asset Type", L"Asset Browser",
                     "按网格、材质、贴图、程序化图（.procgraph）、场景或源码类型筛选资源。");
            break;
        case browser_control::view_menu:
            set_help(ctrl, L"Browser View Mode", L"Asset Browser",
                     "在列表（List）、详情表格（Details）与平铺卡片（Tiles）视图之间切换。");
            break;
        case browser_control::actions:
            set_help(ctrl, L"Asset Actions", L"Asset Browser",
                     "新建材质、新建程序化图（.procgraph）、导入外部网格/贴图或刷新资产目录。");
            break;
        case output_control::level:
            set_help(ctrl, L"Minimum Log Severity", L"Project Output",
                     "循环切换日志最低显示级别（All / Info+ / Warning+ / Error+）。");
            break;
        case output_control::clear_filters:
            set_help(ctrl, L"Reset Log Filters", L"Project Output",
                     "重置级别、类别与全文搜索过滤条件，显示全部日志记录。");
            break;
        case output_control::locate:
            set_help(ctrl, L"Jump to Source Location", L"Project Output",
                     "在代码编辑器中打开并跳转到当前选中诊断记录对应的源码行与列。");
            break;
        case output_control::open_directory:
            set_help(ctrl, L"Open Log Directory", L"Project Output",
                     "在系统文件资源管理器中打开当前运行会话的日志文件目录。");
            break;
        case spreadsheet_control::layer_point:
            set_help(ctrl, L"Points Layer", L"Geometry Spreadsheet",
                     "查看点层级（Point）的所有属性列（如 P、N、Cd、uv、orient、pscale）。");
            break;
        case spreadsheet_control::layer_vertex:
            set_help(ctrl, L"Vertices Layer", L"Geometry Spreadsheet",
                     "查看顶点层级（Vertex）拓扑映射及逐角点属性（如面角 UV）。");
            break;
        case spreadsheet_control::layer_primitive:
            set_help(ctrl, L"Primitives Layer", L"Geometry Spreadsheet",
                     "查看图元层级（Primitive）的多边形/折线类型、顶点数与图元属性。");
            break;
        case spreadsheet_control::layer_detail:
            set_help(ctrl, L"Detail Layer", L"Geometry Spreadsheet",
                     "查看整份几何体共享的单行全局属性（Detail Attributes）。");
            break;
        case spreadsheet_control::layer_instance:
            set_help(ctrl, L"Instances Layer", L"Geometry Spreadsheet",
                     "查看 CopyToPoints 生成的网格实例资产引用与局部变换平移分量。");
            break;
        case spreadsheet_control::follow_toggle:
            set_help(ctrl, L"Follow Selected Node",
                     spreadsheet.follow_selected() ? L"Currently: Follow Selection" : L"Currently: Pin Display Node",
                     "开启后电子表格实时检视画布中当前选中的节点输出；关闭后始终锁定检视 Display Flag 节点输出。");
            break;
        case spreadsheet_control::page_prev:
            set_help(ctrl, L"Previous 512 Rows", L"Geometry Spreadsheet", "翻到电子表格的上一页（每页 512 行）。");
            break;
        case spreadsheet_control::page_next:
            set_help(ctrl, L"Next 512 Rows", L"Geometry Spreadsheet", "翻到电子表格的下一页（每页 512 行）。");
            break;
        case control::color_revert:
            set_help(ctrl, L"Revert Color", L"Color Picker", "恢复打开拾色器之前的原始颜色值。");
            break;
        case control::color_done:
            set_help(ctrl, L"Accept Color", L"Color Picker", "关闭拾色器并确认当前颜色修改。");
            break;
        default: {
            const auto has_class = [&](std::string_view cls) {
                return std::ranges::find(ctrl.classes, cls) != ctrl.classes.end();
            };
            if (has_class("dock-add"))
                set_help(ctrl, L"Add Pane Tab", L"Workspace Dock", "在此停靠窗格中打开新的编辑器视图标签页。");
            else if (has_class("dock-tool"))
                set_help(ctrl, L"Maximize / Restore Pane", L"Workspace Dock",
                         "最大化当前停靠窗格，或恢复多窗格分屏布局。");
            else if (has_class("dock-menu"))
                set_help(ctrl, L"Pane Layout Menu", L"Workspace Dock", "拆分、关闭或调整当前停靠窗格布局。");
            else if (has_class("dock-close"))
                set_help(ctrl, L"Close Tab", L"Workspace Dock",
                         "关闭此视图标签页（可通过各窗格 + 按钮或 Window 菜单重新打开）。");
            else if (has_class("dock-collapse"))
                set_help(ctrl, L"Collapse / Expand Split", L"Workspace Dock", "一键折叠或恢复相邻分屏窗格。");
            else if (has_class("dock-grip"))
                set_help(ctrl, L"Resize Split [Alt+Click Reset]", L"Workspace Dock",
                         "拖拽调整两侧窗格尺寸比例；按住 Alt 点击恢复默认分割位置。");
            break;
        }
        }
    });
    // A closed menu or retargeted control must not keep an executable old call.
    bindings_.erase(std::remove_if(bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_), bindings_.end(),
                                   [](const auto& binding) { return binding.kind == Binding::Kind::operator_call; }),
                    bindings_.end());
    property_help_.end();
    return {};
}

std::array<double, 3> rgb_to_hsv(std::array<double, 3> c) noexcept {
    const auto high = std::max(c[0], std::max(c[1], c[2]));
    const auto low = std::min(c[0], std::min(c[1], c[2]));
    const auto span = high - low;
    double h = 0;
    if (span > 0) {
        if (high == c[0])
            h = 60 * std::fmod((c[1] - c[2]) / span, 6.0);
        else if (high == c[1])
            h = 60 * ((c[2] - c[0]) / span + 2);
        else
            h = 60 * ((c[0] - c[1]) / span + 4);
        if (h < 0)
            h += 360;
    }
    return {h, high > 0 ? span / high : 0, high};
}

std::array<double, 3> hsv_to_rgb(std::array<double, 3> hsv) noexcept {
    const auto h = std::fmod(std::max(hsv[0], 0.0), 360.0) / 60;
    const auto s = std::clamp(hsv[1], 0.0, 1.0);
    const auto v = std::max(hsv[2], 0.0);
    const auto chroma = v * s;
    const auto x = chroma * (1 - std::abs(std::fmod(h, 2.0) - 1));
    const auto m = v - chroma;
    const int sector = int(h);
    const std::array<std::array<double, 3>, 6> table{
        {{chroma, x, 0}, {x, chroma, 0}, {0, chroma, x}, {0, x, chroma}, {x, 0, chroma}, {chroma, 0, x}}};
    const auto& rgb = table[std::clamp(sector, 0, 5)];
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

std::wstring color_hex(std::array<double, 3> c) {
    const auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return std::format(L"{:02X}{:02X}{:02X}", byte(c[0]), byte(c[1]), byte(c[2]));
}

std::optional<std::array<double, 3>> parse_color_hex(std::wstring_view text) {
    if (!text.empty() && text.front() == L'#')
        text.remove_prefix(1);
    if (text.size() != 6)
        return {};
    std::array<double, 3> result{};
    for (std::size_t i = 0; i < 3; ++i) {
        int value = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const auto c = text[i * 2 + j];
            const int digit = c >= L'0' && c <= L'9'   ? c - L'0'
                              : c >= L'a' && c <= L'f' ? c - L'a' + 10
                              : c >= L'A' && c <= L'F' ? c - L'A' + 10
                                                       : -1;
            if (digit < 0)
                return {};
            value = value * 16 + digit;
        }
        result[i] = value / 255.0;
    }
    return result;
}
}
