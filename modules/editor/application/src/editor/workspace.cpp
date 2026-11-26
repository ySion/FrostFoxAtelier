#include "editor/workspace.hpp"
#include "editor/assets.hpp"
#include "application/diagnostics.hpp"
#include "application/procedural_preview.hpp"
#include "editor/fields.hpp"
#include "editor/view_options.hpp"
#include "procedural/graph.hpp"
#include "platform/utf.hpp"
#include <algorithm>
#include <limits>
#include <cwctype>
#include <format>

namespace atelier::editor {
namespace {
Error tree_error(const ui::Error& error) {
    return {error.code == ui::ErrorCode::capacity_exceeded ? ErrorCode::capacity_exceeded
                                                           : ErrorCode::invalid_definition,
            std::string(error.message)};
}

Result<const FieldInfo*> property_field(const TypeInfo& owner, FieldId id) {
    const auto field = std::ranges::find(owner.fields, id, &FieldInfo::id);
    if (field == owner.fields.end())
        return std::unexpected(
            Error{ErrorCode::invalid_member, "Property path contains an unknown field on " + owner.name});
    return &*field;
}

struct CavityOption {
    std::wstring_view title;

    ViewOption option;
};

struct ViewToggleBinding {
    ui::ControlKey key;

    ViewOption option;

    double on{1};

    double off{};
};

constexpr ViewToggleBinding view_toggles[]{{control::display_grid, ViewOption::reference_grid},
                                           {control::guide_grid, ViewOption::reference_grid},
                                           {control::display_axes, ViewOption::origin_axes},
                                           {control::guide_axes, ViewOption::origin_axes},
                                           {control::display_wire, ViewOption::wireframe,
                                            double(renderer::Wireframe::overlay), double(renderer::Wireframe::off)},
                                           {control::display_wire_only, ViewOption::wireframe,
                                            double(renderer::Wireframe::only), double(renderer::Wireframe::off)},
                                           {control::display_triangles, ViewOption::triangle_edges},
                                           {control::display_backfaces, ViewOption::backface_tint},
                                           {control::display_cavity, ViewOption::cavity},
                                           {control::cavity_enabled, ViewOption::cavity},
                                           {control::display_lit, ViewOption::shading,
                                            double(renderer::Shading::scene_lit), double(renderer::Shading::smooth)},
                                           {control::guide_background, ViewOption::background,
                                            double(renderer::Background::gradient),
                                            double(renderer::Background::none)}};

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
            {control::entities_tab, 21, L"Scene Outliner", {200, 140}},
            {control::assets_tab, control::assets_content, L"Asset Browser", {240, 140}},
            {source_control::tab, source_control::content, L"Code", {300, 180}},
            {control::output_tab, control::output_content, L"Project Output", {240, 120}},
            {world_control::tab, world_control::content, L"World", {280, 240}},
            {network_control::tab, network_control::content, L"Network", {220, 140}},
            {spreadsheet_control::tab, spreadsheet_control::content, L"Geometry Spreadsheet", {260, 140}}}) {}

Result<void> Workspace::initialize() {
    ATELIER_TRY(ui_language::register_types(ui_runtime_));
    ATELIER_TRY_BIND(builtins, OperatorRegistry::builtins());
    const auto require_operator = [&](const OperatorCall& call) -> atelier::Result<void> {
        if (!builtins->find(call.id))
            return std::unexpected(Error{ErrorCode::invalid_definition, "Unknown built-in operator: " + call.id});
        return {};
    };
    for (const auto& item : menu_bindings)
        ATELIER_TRY(require_operator(item.call));
    for (const auto& entry : keymap())
        ATELIER_TRY(require_operator(entry.call));
    ATELIER_TRY(reset_layout());
    operators = std::move(*builtins);
    return {};
}

namespace {
constexpr std::array<std::pair<RegionKind, ui::ControlKey>, 10> region_tabs{
    {{RegionKind::viewport, control::scene_tab},
     {RegionKind::scene_tree, control::entities_tab},
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

std::string Workspace::default_selected_graph_node(const Session& session, std::string_view graph_key) const {
    const auto* doc = session.graph_document(graph_key);
    if (!doc)
        return {};
    if (doc->graph.registered())
        if (const auto listed = procedural::contents(session.world().runtime(), doc->graph.id()))
            if (listed->display() != ObjectId{})
                if (const auto disp_name = session.graph_node_name(listed->display()))
                    return *disp_name;
    return doc->nodes.empty() ? std::string{} : doc->nodes.back().name;
}

bool Workspace::is_network_graph_locked(const Session& session, std::string_view net_graph) const {
    if (net_graph.empty() || session.is_scratch_graph(net_graph))
        return false;
    if (const auto it = network_lock_overrides.find(std::string(net_graph)); it != network_lock_overrides.end())
        return it->second;
    if (!graph_breadcrumbs.empty())
        return true;
    return active_graph.empty();
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
    const std::string world_root =
        (network_pinned && !pinned_graph.empty()) ? pinned_graph : selected_generator_graph(session);
    if (!graph_breadcrumbs.empty() && !dived_network_graph.empty() &&
        graph_breadcrumbs.front().graph_key == world_root && session.has_graph(dived_network_graph))
        return dived_network_graph;
    if (!world_root.empty() && session.has_graph(world_root))
        return world_root;
    return {};
}

std::string Workspace::effective_network_graph(Session& session, Project* project) {
    if (!active_graph.empty() && session.has_graph(active_graph))
        return active_graph;
    const std::string world_root =
        (network_pinned && !pinned_graph.empty()) ? pinned_graph : selected_generator_graph(session);
    if (!graph_breadcrumbs.empty()) {
        if (graph_breadcrumbs.front().graph_key != world_root || dived_network_graph.empty()) {
            graph_breadcrumbs.clear();
            dived_network_graph.clear();
        } else {
            if (!session.has_graph(dived_network_graph) && project && project->entry(dived_network_graph))
                static_cast<void>(session.open_graph(*project, dived_network_graph));
            if (session.has_graph(dived_network_graph))
                return dived_network_graph;
            graph_breadcrumbs.clear();
            dived_network_graph.clear();
        }
    }
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
    const auto graph_stem = [&](std::string_view key) {
        if (const auto* entry = project ? project->entry(key) : nullptr)
            return entry->path.stem().wstring();
        return platform::utf8_to_utf16_validated(key);
    };
    std::wstring stem = graph_stem(net_graph);
    std::wstring rel_path = L"/" + stem;
    if (const auto* entry = project ? project->entry(net_graph) : nullptr)
        rel_path = L"/" + entry->path.generic_wstring();
    const std::wstring dirty_mark = session.graph_dirty(net_graph) ? L" *" : L"";
    if (!graph_breadcrumbs.empty()) {
        std::wstring prefix = !active_graph.empty() ? L"SOP" : (network_pinned ? L"Pinned" : L"World");
        if (active_graph.empty() && !network_pinned) {
            const auto sel = session.selection();
            if (sel.entity != ObjectId{} && session.world().contains(sel.entity))
                prefix += L"  ▸  " + std::wstring(session.name(sel.entity));
        }
        for (std::size_t i = 0; i < graph_breadcrumbs.size(); ++i) {
            const auto& crumb = graph_breadcrumbs[i];
            if (i == 0)
                prefix += L"  ▸  " + graph_stem(crumb.graph_key);
            prefix += L"  ▸  " + platform::utf8_to_utf16_validated(crumb.asset_node_name);
        }
        return prefix + L" (" + stem + dirty_mark + L")";
    }
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
    std::vector<std::string> locked;
    for (const auto& gkey : session.open_graphs())
        if (is_network_graph_locked(session, gkey))
            locked.push_back(gkey);
    return {session,
            project,
            std::move(selection_context),
            region,
            std::move(asset),
            active_graph,
            net_graph,
            selected_graph_node,
            selected_graph_nodes,
            std::move(locked),
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

Result<void> Workspace::reset_layout() {
    using enum ui::DockSide;
    static constexpr ui::DockPlacement placements[]{{world_control::tab, control::parameters_tab, tabs},
                                                    {control::scene_tab, control::parameters_tab, left, .62f},
                                                    {control::entities_tab, control::parameters_tab, bottom, .7f},
                                                    {network_control::tab, control::entities_tab, tabs},
                                                    {control::assets_tab, control::scene_tab, bottom, .73f},
                                                    {source_control::tab, control::scene_tab, tabs},
                                                    {control::output_tab, control::assets_tab, tabs},
                                                    {spreadsheet_control::tab, control::assets_tab, tabs}};
    if (const auto reset = dock.reset(placements); !reset)
        return std::unexpected(Error{ErrorCode::capacity_exceeded, std::string(reset.error().message)});
    lighting.cancel();
    dock.select(control::entities_tab);
    dock.select(control::assets_tab);
    dock.select(control::scene_tab);
    dock.select(control::parameters_tab);
    menu.reset();
    menu_bar_collapsed = shelf_collapsed = false;
    return {};
}

void Workspace::reset_document(bool preserve_view) {
    sop_group = {};
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
    pending_live_.reset();
    scene_tree_ = {};
    navigation_ = {ObjectId{}};
    navigation_cursor_ = 0;
    if (!preserve_view) {
        query.clear();
        tree_query.clear();
        outliner_show_components = false;
        parameter_search_visible = false;
        inspecting_asset = false;
        inspecting_network = false;
        material_inspection.clear();
        active_graph.clear();
        network_pinned = false;
        pinned_graph.clear();
        graph_breadcrumbs.clear();
        dived_network_graph.clear();
        network_lock_overrides.clear();
        selected_graph_node.clear();
        selected_graph_nodes.clear();
        last_network_graph_.clear();
    }
    network_context_node.clear();
    failed_auto_open_graph_.clear();
    spreadsheet.reset(preserve_view);
    network.reset(preserve_view);
    menu.reset();
    if (!preserve_view)
        assets.reset(project_open);
    component_choices_.clear();
    material_choices_.clear();
    network_path_choices_.clear();
    menu_selection_ = {};
}

void Workspace::reconcile_rebuild(Session& document, Project* project) {
    if (inspecting_asset) {
        if (material_inspection.empty() || !project || !document.open_material(*project, material_inspection)) {
            inspecting_asset = false;
            material_inspection.clear();
        }
    }
    if (network_pinned) {
        if (pinned_graph.empty() || (!document.has_graph(pinned_graph) && !(project && project->entry(pinned_graph)))) {
            network_pinned = false;
            pinned_graph.clear();
        }
    }
    if (!active_graph.empty() && !document.has_graph(active_graph))
        active_graph.clear();

    if (!graph_breadcrumbs.empty()) {
        std::size_t valid_depth = 0;
        for (; valid_depth < graph_breadcrumbs.size(); ++valid_depth) {
            const auto& crumb = graph_breadcrumbs[valid_depth];
            if (!document.has_graph(crumb.graph_key) && project && project->entry(crumb.graph_key))
                static_cast<void>(document.open_graph(*project, crumb.graph_key));
            if (!document.has_graph(crumb.graph_key) || !document.graph_node(crumb.graph_key, crumb.asset_node_name))
                break;
        }
        if (valid_depth < graph_breadcrumbs.size()) {
            const std::string fallback_graph =
                valid_depth > 0 ? graph_breadcrumbs[valid_depth - 1].graph_key : graph_breadcrumbs.front().graph_key;
            graph_breadcrumbs.resize(valid_depth);
            if (!active_graph.empty()) {
                if (document.has_graph(fallback_graph))
                    active_graph = fallback_graph;
                else
                    active_graph.clear();
            } else {
                dived_network_graph =
                    (valid_depth > 0 && document.has_graph(fallback_graph)) ? fallback_graph : std::string{};
            }
        }
    }

    const std::string target_graph = !active_graph.empty() ? active_graph : current_network_graph(document);
    if (!target_graph.empty() && !document.has_graph(target_graph) && project && project->entry(target_graph))
        static_cast<void>(document.open_graph(*project, target_graph));

    if (!target_graph.empty() && document.has_graph(target_graph)) {
        std::erase_if(selected_graph_nodes,
                      [&](const std::string& name) { return document.graph_node(target_graph, name) == nullptr; });
        if (!selected_graph_node.empty() && document.graph_node(target_graph, selected_graph_node) == nullptr)
            selected_graph_node = selected_graph_nodes.empty() ? std::string{} : selected_graph_nodes.back();
    } else {
        selected_graph_nodes.clear();
        selected_graph_node.clear();
    }

    if (!active_graph.empty())
        inspecting_network = false;
    else if (selected_graph_nodes.empty() && selected_graph_node.empty())
        inspecting_network = false;
    bump_view_revision();
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

Result<void> Workspace::commit_ui_controls() {
    if (!ui_document_)
        return {};
    if (ui_runtime_.collection_phase() == CollectionPhase::idle)
        ATELIER_TRY(ui_runtime_.begin_collection());
    ATELIER_TRY(ui_runtime_.collect_step({32}));
    const auto revision = ui_document_->revision();
    ui_document_->commit_controls();
    if (pending_live_) {
        scene_tree_.commit(*pending_live_);
        // Retire absent objects only among unused bindings. This frame can introduce
        // material keys before the next membership scan needs to enumerate material slots.
        const auto unused = bindings_.begin() + static_cast<std::ptrdiff_t>(binding_cursor_);
        bindings_.erase(std::remove_if(unused, bindings_.end(),
                                       [&](const auto& binding) {
                                           return !(binding.kind == Binding::Kind::operator_call &&
                                                    binding.object == ObjectId{}) &&
                                                  !pending_live_->contains(binding.object);
                                       }),
                        bindings_.end());
        pending_live_.reset();
    }
    if (ui_document_->revision() != revision && revision)
        status = L"UI updated: revision " + std::to_wstring(ui_document_->revision());
    return {};
}

bool Workspace::reject_ui_controls(std::string message) {
    pending_live_.reset();
    return ui_document_ && ui_document_->reject_controls(std::move(message));
}

Result<void> Workspace::set_ui_source(std::filesystem::path path) {
    if (ui_document_)
        return std::unexpected(
            Error{ErrorCode::unsupported_operation, "Choose the authoring file before opening the UI document."});
    ui_source_ = std::move(path);
    return {};
}

Result<std::filesystem::path> Workspace::ui_source() const {
    if (!ui_source_.empty())
        return ui_source_;
    const std::filesystem::path authoring{ATELIER_UI_AUTHORING_PATH};
    std::error_code error;
    if (std::filesystem::is_regular_file(authoring, error))
        return authoring;
    ATELIER_TRY_BIND(directory, asset_directory());
    return *directory / "editor.ui.source";
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
    if (target && target->kind == Binding::Kind::object && session.world().contains(target->object) &&
        (modifiers.shift || modifiers.control)) {
        std::vector<ObjectId> selected;
        if (modifiers.control)
            selected.assign(session.selected_entities().begin(), session.selected_entities().end());
        if (modifiers.shift) {
            // The range runs over the Entity rows the Scene Outliner shows, described or scrolled away.
            const auto row_of = [&](ObjectId entity) {
                return std::ranges::find_if(scene_tree_.rows,
                                            [&](const auto& row) { return scene_tree_.objects[row.node] == entity; });
            };
            const auto first = row_of(session.selection().entity);
            const auto last = row_of(target->object);
            if (first != scene_tree_.rows.end() && last != scene_tree_.rows.end()) {
                for (auto at = std::min(first, last); at <= std::max(first, last); ++at)
                    if (const auto entity = scene_tree_.objects[at->node];
                        entity != ObjectId{} && session.world().contains(entity) &&
                        std::ranges::find(selected, entity) == selected.end())
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
    return target && target->kind == Binding::Kind::object ? select_object(session, host, target->object)
                                                           : Result<void>{};
}

Result<void> Workspace::set_tree_expanded(const Session& session, std::span<const ObjectId> objects, bool expanded) {
    for (const auto object : objects)
        if (object != ObjectId{} && !session.world().contains(object))
            return std::unexpected(Error{ErrorCode::invalid_member, "The identity is not a row of this tree."});
    for (const auto object : objects)
        scene_tree_.requests.insert_or_assign(object, expanded);
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
    ATELIER_TRY(set_tree_expanded(session, ancestors, true));
    return tree_query.empty() || std::ranges::any_of(scene_tree_.rows, [&](const auto& row) {
               return scene_tree_.objects[row.node] == entity;
           });
}

void Workspace::EntityTreeView::commit(const Set<ObjectId>& live) {
    std::erase_if(expansion,
                  [&](const auto& entry) { return entry.first != ObjectId{} && !live.contains(entry.first); });
    if (!pending)
        return;
    objects.swap(candidate.objects);
    rows.swap(candidate.rows);
    primary = candidate.primary;
    for (const auto& [object, expanded] : candidate.edits)
        if (object == ObjectId{} || live.contains(object))
            expansion[object] = expanded;
    pending = false;
}

Result<std::vector<std::uint32_t>> Workspace::EntityTreeView::window(ObjectId selected, std::wstring_view filter,
                                                                     ui::TreeViewport viewport,
                                                                     std::span<const ObjectId> pinned) {
    auto& tree = candidate;
    tree.primary.reset();
    // Expand ancestors of a new primary (or parent) in the candidate only.
    if (const auto node = tree.node(selected); node != ui::no_tree_node) {
        const auto parent = tree.nodes[node].parent;
        tree.primary.emplace(selected, parent == ui::no_tree_node ? ObjectId{} : tree.objects[parent]);
        if (tree.primary != primary)
            for (auto at = parent; at != ui::no_tree_node; at = tree.nodes[at].parent) {
                tree.edits.insert_or_assign(tree.objects[at], true);
                tree.nodes[at].expanded = true;
            }
    }
    ATELIER_TRY_BIND(flattened, ui::flatten_tree(tree.nodes, filter, std::move(tree.rows)).transform_error(tree_error));
    tree.rows = std::move(*flattened);
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

Result<ui::ControlKey> Workspace::allocate_binding_key() {
    return ui_language::allocate_control_key(next_binding_, "Editor control identities exhausted.");
}

Result<ui::ControlKey> Workspace::bind(ObjectId object, std::span<const FieldId> path, Binding::Kind kind) {
    if (!path.empty() && kind == Binding::Kind::object)
        kind = Binding::Kind::field;
    return bind_row(
        binding_cursor_,
        [&](const Binding& item) {
            return item.kind == kind && item.object == object && item.component == ObjectId{} && item.slot.empty() &&
                   std::equal(item.path.begin(), item.path.end(), path.begin(), path.end());
        },
        [&](ui::ControlKey key) { return Binding{key, object, {path.begin(), path.end()}, kind}; });
}

Result<ui::ControlKey> Workspace::bind_operator(OperatorCall call) {
    return bind_row(
        binding_cursor_,
        [&](const Binding& item) { return item.kind == Binding::Kind::operator_call && item.call->request == call; },
        [&](ui::ControlKey key) {
            Binding row{key,
                        call.context.selection ? call.context.selection->object : ObjectId{},
                        {},
                        Binding::Kind::operator_call};
            row.call = std::make_unique<OperatorBinding>(std::move(call));
            return row;
        });
}

Result<ui::ControlKey> Workspace::bind_graph_param(std::string_view graph_key, std::string_view node_name,
                                                   std::string_view param_path, Binding::Kind kind, std::size_t lane,
                                                   ObjectId object, ObjectId component, std::span<const FieldId> path) {
    return bind_row(
        0,
        [&](const Binding& item) {
            return item.kind == kind && item.object == object && item.component == component &&
                   item.slot == graph_key && item.node_name == node_name && item.material_path == param_path &&
                   item.index == lane && std::ranges::equal(item.path, path);
        },
        [&](ui::ControlKey key) {
            Binding item{key, object, {path.begin(), path.end()}, kind};
            item.component = component;
            item.slot = graph_key;
            item.index = lane;
            item.material_path = param_path;
            item.node_name = node_name;
            return item;
        });
}

bool Workspace::effective_show_points(const procedural::Geometry* geometry) const noexcept {
    if (sop_show_points.has_value())
        return *sop_show_points;
    return geometry != nullptr && application::is_pure_point_cloud(*geometry);
}

Result<ObjectId> Workspace::active_procedural_generator(const Session& session) const {
    const auto sel = session.selection();
    const auto target = sel.object != ObjectId{} ? sel.object : sel.entity;
    if (!active_graph.empty() || target == ObjectId{})
        return ObjectId{};
    ATELIER_TRY_BIND(resolved, session.resolve_procedural_generator(target, /*required=*/false));
    if (*resolved == ObjectId{})
        return ObjectId{};
    auto& runtime = session.world().runtime();
    ATELIER_TRY_BIND(state, world::inspect_component(runtime, *resolved));
    if (!state->valid || !state->enabled || !session.world().contains(state->owner))
        return ObjectId{};
    ATELIER_TRY_BIND(enabled, world::effective_enabled(runtime, state->owner));
    return *enabled ? *resolved : ObjectId{};
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
    const auto publish = [&]() -> atelier::Result<bool> {
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
        ATELIER_TRY(call(operators, host, operator_context(session, nullptr, true), {"view.set", patch.finish()}));
        return true;
    };
    ATELIER_TRY_BIND(edited, lighting.apply(action, candidate));
    if (*edited)
        return publish();
    if (action.kind != ui::ActionKind::activate)
        return false;
    const auto key = action.key;
    if (const auto* target = binding(key); target && target->kind == Binding::Kind::group_guide) {
        sop_group.select(static_cast<procedural::Layer>(target->index), target->slot);
        ++view_revision_;
        return true;
    }
    if (key >= control::shading_preset_first && key < control::shading_preset_first + 4) {
        const auto preset = key - control::shading_preset_first;
        ATELIER_TRY(
            set_view_option(candidate, ViewOption::wireframe,
                            preset == 0 ? double(renderer::Wireframe::only) : double(renderer::Wireframe::off)));
        if (preset)
            ATELIER_TRY(set_view_option(candidate, ViewOption::shading,
                                        preset == 1   ? double(renderer::Shading::smooth)
                                        : preset == 2 ? double(renderer::Shading::matcap)
                                                      : double(renderer::Shading::scene_lit)));
    } else if (const auto toggle = std::ranges::find(view_toggles, key, &ViewToggleBinding::key);
               toggle != std::end(view_toggles)) {
        const auto* checked = std::get_if<bool>(&action.value);
        if (!checked || (key == control::guide_background && view.shading == renderer::Shading::scene_lit))
            return false;
        ATELIER_TRY(set_view_option(candidate, toggle->option, *checked ? toggle->on : toggle->off));
    } else if (key >= control::cavity_value_first && key < control::cavity_value_first + 15) {
        const auto index = key - control::cavity_value_first;
        ATELIER_TRY(set_view_option(candidate, cavity_options[index / 5].option, cavity_values[index / 5][index % 5]));
    } else if (key >= control::shading_first && key < control::shading_first + 3)
        ATELIER_TRY(set_view_option(candidate, ViewOption::shading, double(key - control::shading_first)));
    else if (key == control::shading_scene_lit)
        ATELIER_TRY(set_view_option(candidate, ViewOption::shading, double(renderer::Shading::scene_lit)));
    else if (key >= control::edges_first && key < control::edges_first + 4) {
        const auto option = key - control::edges_first;
        ATELIER_TRY(set_view_option(candidate, ViewOption::wireframe, double(option < 2 ? option : 2)));
        ATELIER_TRY(set_view_option(candidate, ViewOption::hidden_lines, option != 3 ? 1 : 0));
    } else if (key >= control::topology_first && key < control::topology_first + 2)
        ATELIER_TRY(set_view_option(candidate, ViewOption::triangle_edges, key != control::topology_first ? 1 : 0));
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
    } else
        return false;
    ATELIER_TRY(publish());
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
        auto& runtime = session.world().runtime();
        ObjectId target_entity = target->object;
        ui::ControlKey drop_key = box.key;
        if (!session.world().contains(target_entity)) {
            const auto comp = world::inspect_component(runtime, target_entity);
            if (!comp || !comp->valid)
                return {};
            target_entity = comp->owner;
            if (const auto ent_key = object_control(target_entity))
                drop_key = ent_key;
        }
        // An Entity cannot become a child of itself or of its own descendant.
        for (auto ancestor = target_entity; ancestor != ObjectId{};) {
            if (ancestor == dragged)
                return {};
            const auto parent = world::parent_entity(runtime, ancestor);
            if (!parent)
                return {};
            ancestor = *parent;
        }
        return TreeDrop{target_entity, drop_key};
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
    apply_view_preferences(candidate);
    return {};
}

void Workspace::apply_view_preferences(const renderer::ViewSettings& candidate) {
    if (view != candidate) {
        lighting.cancel();
        view = candidate;
        ++view_revision_;
    }
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

std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

std::string property_condition_token(const Runtime& runtime, const Property& prop) {
    if (prop.is_string)
        return prop.string_value;
    return std::visit(
        [&](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::same_as<T, EnumValue>) {
                if (const auto info = runtime.describe(v.type); info && (*info)->enumeration) {
                    for (const auto& [name, bits] : (*info)->enumeration->values)
                        if (bits == v.bits)
                            return name;
                }
                return std::to_string(v.bits);
            } else {
                char buffer[32];
                const auto written = std::to_chars(buffer, buffer + sizeof(buffer), v);
                return written.ec == std::errc{} ? std::string(buffer, written.ptr) : std::string{};
            }
        },
        prop.value);
}

bool property_visible_when(const Runtime& runtime, std::span<const Property> schema, const Property& current) {
    const auto meta = std::ranges::find(current.metadata, std::string_view{"ui.visible_when"},
                                        &std::pair<std::string, std::string>::first);
    if (meta == current.metadata.end() || meta->second.empty())
        return true;
    std::string_view remaining = meta->second;
    while (!remaining.empty()) {
        const auto semi = remaining.find(';');
        const std::string_view clause = trim_ascii(remaining.substr(0, semi));
        remaining = semi == std::string_view::npos ? std::string_view{} : remaining.substr(semi + 1);
        if (clause.empty())
            continue;
        bool negate = false;
        auto op = clause.find("!=");
        std::size_t op_len = 2;
        if (op == std::string_view::npos) {
            op = clause.find("==");
            if (op == std::string_view::npos) {
                op = clause.find('=');
                op_len = 1;
            }
        } else {
            negate = true;
        }
        if (op == std::string_view::npos)
            continue;
        const std::string_view key = trim_ascii(clause.substr(0, op));
        std::string_view rhs = trim_ascii(clause.substr(op + op_len));
        if (key.empty())
            continue;
        const Property* target = nullptr;
        if (const auto direct = std::ranges::find(schema, key, &Property::name); direct != schema.end()) {
            target = &*direct;
        } else {
            std::string_view prefix = current.name;
            for (auto dot = prefix.rfind('.'); dot != std::string_view::npos; dot = prefix.rfind('.')) {
                prefix = prefix.substr(0, dot);
                const std::string qualified = std::string(prefix) + "." + std::string(key);
                if (const auto nested = std::ranges::find(schema, qualified, &Property::name); nested != schema.end()) {
                    target = &*nested;
                    break;
                }
            }
        }
        if (!target)
            continue;
        const std::string actual = property_condition_token(runtime, *target);
        bool matched = false;
        while (true) {
            const auto pipe = rhs.find('|');
            if (actual == trim_ascii(rhs.substr(0, pipe))) {
                matched = true;
                break;
            }
            if (pipe == std::string_view::npos)
                break;
            rhs.remove_prefix(pipe + 1);
        }
        if (negate ? matched : !matched)
            return false;
    }
    return true;
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
struct EntityRowAppearance {
    ui::ControlIcon icon;

    bool mesh;

    bool retained;

    std::array<ui::ControlIcon, 4> badges{};
};

Result<ui::ControlIcon> component_row_icon(const Session& session, ObjectId part_id) {
    auto& runtime = session.world().runtime();
    if (session.has_retained(part_id)) {
        ATELIER_TRY_BIND(saved, session.retained(part_id));
        if (!saved->type.empty())
            return ui::ControlIcon::component;
    }
    ATELIER_TRY_BIND(part_type, runtime.type_of(part_id));
    const auto& types = session.presentation_types();
    ATELIER_TRY_BIND(is_mesh, runtime.is_a(*part_type, types.static_mesh_component));
    if (*is_mesh)
        return ui::ControlIcon::mesh;
    ATELIER_TRY_BIND(is_gen, runtime.is_a(*part_type, types.procedural_generator_component));
    if (*is_gen)
        return ui::ControlIcon::procgraph;
    ATELIER_TRY_BIND(is_light, runtime.is_a(*part_type, types.light_component));
    if (*is_light) {
        ATELIER_TRY_BIND(light, presentation::read_light(runtime, part_id));
        return light_icon(light->kind);
    }
    ATELIER_TRY_BIND(is_cam, runtime.is_a(*part_type, types.camera_component));
    if (*is_cam)
        return ui::ControlIcon::camera;
    ATELIER_TRY_BIND(is_env, runtime.is_a(*part_type, types.environment_component));
    if (*is_env)
        return ui::ControlIcon::world;
    const auto& phys = session.physics_types();
    for (const TypeId pt : {phys.character_component, phys.ragdoll_component, phys.ragdoll_part_component,
                            phys.distance_constraint_component, phys.force_component, phys.kinematic_motion_component,
                            phys.scene_query_component}) {
        if (pt == TypeId{})
            continue;
        ATELIER_TRY_BIND(is_phys, runtime.is_a(*part_type, pt));
        if (*is_phys)
            return ui::ControlIcon::physics;
    }
    return ui::ControlIcon::component;
}

Result<EntityRowAppearance> entity_row_appearance(const Session& session, ObjectId entity, TypeId type) {
    auto& runtime = session.world().runtime();
    const auto& types = session.presentation_types();
    ATELIER_TRY_BIND(mesh, runtime.is_a(type, types.static_mesh_entity));
    ATELIER_TRY_BIND(generator_ent, runtime.is_a(type, types.procedural_generator_entity));
    ATELIER_TRY_BIND(parts, world::components(runtime, entity));
    const bool retained = session.has_retained(entity) || std::ranges::any_of(*parts, [&](const auto& part) {
                              return session.has_retained(part.id());
                          });
    EntityRowAppearance row{ui::ControlIcon::object, *mesh, retained, {}};
    std::size_t badge_count{};
    const auto push_badge = [&](ui::ControlIcon badge) {
        if (badge == ui::ControlIcon::none || badge == ui::ControlIcon::component || badge_count >= row.badges.size())
            return;
        for (std::size_t i = 0; i < badge_count; ++i)
            if (row.badges[i] == badge)
                return;
        row.badges[badge_count++] = badge;
    };
    if (*mesh)
        row.icon = ui::ControlIcon::mesh;
    else if (*generator_ent)
        row.icon = ui::ControlIcon::procgraph;
    for (const auto& part : *parts) {
        ATELIER_TRY_BIND(part_icon, component_row_icon(session, part.id()));
        if (row.icon == ui::ControlIcon::object && part_icon != ui::ControlIcon::component)
            row.icon = *part_icon;
        push_badge(*part_icon);
        if (const auto body = physics::read_settings(runtime, part.id()); body && body->enabled)
            push_badge(ui::ControlIcon::physics);
    }
    // Keep trailing badges focused on secondary/additional capabilities when there is only a single primary capability.
    if (badge_count == 1 && row.badges[0] == row.icon) {
        row.badges[0] = ui::ControlIcon::none;
    }
    return row;
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
    } else if (call.id == "graph.dive_in") {
        title = L"Dive Into Digital Asset";
        desc = "进入当前 AssetNode 实例引用的 .procgraph 子图查看与编辑（默认开启 Locked Preview 防误改保护）。";
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
    std::optional<Error> first_error;
    for (;;) {
        bool retry = false;
        property_help_.begin();
        auto built = build_controls(result, session, size, frame, project, ui, command_focus, retry);
        property_help_.end();
        if (built)
            return {};
        const bool rejected = reject_ui_controls(built.error().message);
        if (first_error)
            return std::unexpected(std::move(*first_error));
        if (!retry || !rejected)
            return built;
        first_error = std::move(built).error();
    }
}

Result<void> Workspace::build_controls(ui::ControlList& result, Session& session, ui_layout::Size size,
                                       const ui::UiFrame* frame, Project* project, const ui::Session* ui,
                                       const ui::ControlBox* command_focus, bool& retry) {
    pending_live_.reset();
    scene_tree_.begin(dock.visible(control::entities_tab));
    sync_world(session);
    if (!command_focus && frame && ui)
        command_focus = frame->find(ui->focused_control());
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
        if (!recovery) {
            auto bundled = asset_directory().and_then(
                [](const auto& directory) { return assets::read_file(directory / "editor.ui.source", 1024 * 1024); });
            if (bundled)
                recovery = std::move(*bundled);
        }
        ATELIER_TRY_BIND(source_path, ui_source());
        ui_source_ = std::move(*source_path);
        ui_document_ =
            std::make_unique<ui_language::LiveDocument>(ui_runtime_, ui_source_, next_binding_, std::move(recovery));
    }
    source_editor.poll(frame && frame->input_anchor && frame->input_anchor->token.key == source_control::buffer &&
                       frame->input_anchor->composition_id);
    ui_document_->poll();
    project_ui_configured = project && !project->ui_source().empty();
    ATELIER_TRY_BIND(world_members, session.world().members());
    const auto members = *world_members;
    // The World already indexes all attached components, including disabled
    // ones. UI liveness only needs their IDs, not a new Root for each member.
    ATELIER_TRY_BIND(component_ids, session.world().query(session.types().component));
    const auto& components = *component_ids;
    const auto graph_objects = session.graph_objects();
    if (!active_graph.empty() && !session.has_graph(active_graph)) {
        active_graph.clear();
        set_single_graph_selection({});
    } else if (!active_graph.empty() && !selected_graph_node.empty() &&
               !session.graph_node(active_graph, selected_graph_node)) {
        set_single_graph_selection(default_selected_graph_node(session, active_graph));
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
    world::EntityStatus entity_status{};
    std::optional<ObjectId> transform;
    if (selected != ObjectId{}) {
        ATELIER_TRY_BIND(state, world::inspect_entity(runtime, selected));
        entity_status = *state;
        ATELIER_TRY_BIND(spatial, world::has_transform(runtime, selected));
        if (*spatial)
            transform = selected;
    }
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
    const auto action = [&](OperatorCall call) -> atelier::Result<ui_language::ActionBinding> {
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
        ATELIER_TRY_BIND(bound_key, bind_operator(call));
        auto help = describe_operator_tooltip(property_help_, *bound_key, call, info, target, polled);
        return ui_language::ActionBinding{*bound_key, polled.has_value(), std::move(help)};
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
    ATELIER_TRY_BIND(restart_play, action({"play.restart"}));
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
                                                               std::move(*restart_play)};
    const std::array<ui_language::KeyBinding, 8> shell_keys{{{"root", 1},
                                                             {"menubar", 2},
                                                             {"stow", control::menu_bar_handle},
                                                             {"menu_body", control::menu_bar_body},
                                                             {"shelf_slot", 3},
                                                             {"workspace", 20},
                                                             {"footer", 100},
                                                             {"status", 101}}};
    const auto composition_failed = [&](atelier::Error error) -> atelier::Result<void> {
        retry = ui_document_->candidate_failed();
        return std::unexpected(std::move(error));
    };
    if (auto staged = ui_document_->append("EditorShell", 0, shell_keys, shell_inputs, result); !staged)
        return composition_failed(std::move(staged).error());
    auto create_tools = std::make_shared<std::vector<ui_language::Item>>();
    auto view_tools = std::make_shared<std::vector<ui_language::Item>>();
    for (std::size_t index = 0; index < std::size(menu_bindings); ++index) {
        const auto& definition = menu_bindings[index];
        if (definition.group != MenuGroup::create && definition.group != MenuGroup::view)
            continue;
        if (definition.group == MenuGroup::view && definition.shelf_title.empty())
            continue;
        if (!shelf_items_[index])
            ATELIER_TRY_ASSIGN(shelf_items_[index], allocate_binding_key());
        const auto title = definition.shelf_title.empty() ? definition.title : definition.shelf_title;
        const auto kind_field = *definition.call.args.root().find("kind");
        const auto kind = kind_field ? *kind_field->string() : std::string_view{};
        const auto icon = kind == "point"                  ? ui::ControlIcon::point_light
                          : kind == "rectangle"            ? ui::ControlIcon::area_light
                          : kind == "directional"          ? ui::ControlIcon::sun_light
                          : kind == "mesh"                 ? ui::ControlIcon::mesh
                          : kind == "procedural_generator" ? ui::ControlIcon::procgraph
                          : kind == "entity"               ? ui::ControlIcon::object
                                                           : ui::ControlIcon::axes;
        auto& items = definition.group == MenuGroup::create ? create_tools : view_tools;
        ATELIER_TRY_BIND(activate, action(definition.call));
        items->push_back({shelf_items_[index], {std::wstring(title), static_cast<double>(icon), std::move(*activate)}});
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
    if (auto staged = ui_document_->append("ToolShelf", 3, shelf_keys, shelf_inputs, result); !staged)
        return composition_failed(std::move(staged).error());
    // The three authored fixed-height bands define the native dock host extent.
    float chrome_height{};
    for (const auto key : {ui::ControlKey{2}, control::shelf, ui::ControlKey{100}}) {
        const auto chrome = result.controls();
        const auto at = std::ranges::find(chrome, key, &ui::Control::key);
        if (at == chrome.end() || at->layout.height.rule != ui_layout::SizeRule::fixed) {
            constexpr auto message = "Editor chrome bands require fixed heights for the dock host.";
            retry = true;
            return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_definition, message});
        }
        chrome_height += at->layout.height.value;
    }
    const auto placement = dock.presentation(20, {size.width, std::max(0.f, size.height - chrome_height)});
    const auto focused_tab = dock.focused_tab();
    auto panes = std::make_shared<std::vector<ui_language::Item>>();
    for (const auto& pane : placement.panes) {
        const bool pane_focused = pane.active && pane.active->key == focused_tab;
        auto tabs = std::make_shared<std::vector<ui_language::Item>>();
        for (const auto& tab : pane.tabs) {
            std::wstring style = L"dock-tab";
            for (const auto& name : tab.view->classes)
                style += L" " + std::wstring(name.begin(), name.end());
            std::wstring group_style = tab.view->key == focused_tab ? L"dock-tab-group focused-tab" : L"dock-tab-group";
            tabs->push_back(
                {tab.wrapper,
                 {tab.view->title, tab.view == pane.active, std::move(style), std::move(group_style), tab.view->key},
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
                           std::wstring(pane_focused ? L"dock-tabs focused-pane" : L"dock-tabs"),
                           double(pane.maximized ? ui::ControlIcon::restore : ui::ControlIcon::maximize),
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
    if (auto staged = ui_document_->append("DockWorkspace", 20, {}, dock_inputs, result); !staged)
        return composition_failed(std::move(staged).error());
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
                ATELIER_TRY_BIND(binding, action({"tool.settings", args.finish()}));
                modes->push_back(
                    {binding->target,
                     {*binding, double(icons[i]), std::size_t(viewport_settings.tool) == i, control::viewport_modes,
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
                ATELIER_TRY_BIND(activate, action({"tool.settings", args.finish()}));
                select_modes->push_back(
                    {990 + i,
                     {std::move(*activate), double(select_icons[i]), std::size_t(viewport_settings.select_mode) == i,
                      control::viewport_select_modes,
                      std::wstring(i == 0                         ? L"viewport-segment-item first"
                                   : i + 1 == select_icons.size() ? L"viewport-segment-item last"
                                                                  : L"viewport-segment-item")}});
            }
            // Resolve active procedural geometry for both SOP context and World ProceduralGenerator inspection.
            const bool in_sop = !active_graph.empty() && session.has_graph(active_graph);
            const ObjectId gen_comp = in_sop ? ObjectId{} : active_procedural_generator(session).value_or(ObjectId{});
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
                ui_language::ActionBinding binding;
                if (item.key == control::display_fps)
                    ATELIER_TRY_ASSIGN(
                        binding,
                        action({"view.toggle", json::Document{json::Document::literal<R"({"option":"fps"})">()}}));
                else
                    binding = {item.key, true,
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
            ATELIER_TRY_BIND(
                grid_snap,
                action({"tool.toggle", json::Document{json::Document::literal<R"({"setting":"grid_snap"})">()}}));
            ATELIER_TRY_BIND(
                cursor_surface,
                action({"tool.toggle", json::Document{json::Document::literal<R"({"setting":"cursor_surface"})">()}}));
            ATELIER_TRY_BIND(
                view_options_action,
                action({"view.toggle", json::Document{json::Document::literal<R"({"option":"options"})">()}}));
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
                std::move(*grid_snap),
                double(ui::ControlIcon::magnet),
                viewport_settings.grid_snap,
                step_name,
                menu && menu->source == control::viewport_snap,
                viewport_settings.cursor_surface,
                std::move(*cursor_surface),
                std::wstring(camera_view_name(camera_view)),
                menu && menu->source == control::viewport_camera,
                std::move(*view_options_action),
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
            if (auto staged = ui_document_->append("SceneViewport", control::scene_content, keys, inputs, result);
                !staged)
                return composition_failed(std::move(staged).error());
            if (sop_show_info && shown_geom && (in_sop || gen_comp != ObjectId{})) {
                const bool pts_auto = !sop_show_points.has_value() && application::is_pure_point_cloud(*shown_geom);
                ui::Control hud;
                hud.key = control::viewport_geometry_hud;
                hud.parent = control::viewport_scene_overlay;
                hud.kind = ui::ControlKind::label;
                hud.enabled = false;
                hud.text =
                    application::format_geometry_info_hud(hud_title, *shown_geom, pts_auto, hud_cook_ms, hud_chunks);
                if (sop_group.resolve(*shown_geom)) {
                    const auto layer = sop_group.layer();
                    hud.text += std::format(L"\n{}  {} / {} {}", platform::utf8_to_utf16_validated(sop_group.name()),
                                            sop_group.count(), shown_geom->count(layer),
                                            layer == procedural::Layer::point ? L"points" : L"primitives");
                    if (sop_group.samples().size() < sop_group.count())
                        hud.text += std::format(L" ({} shown)", sop_group.samples().size());
                }
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
            if (auto staged =
                    source_editor.append(result, view.content, *ui_document_, action, project, project_build.get(),
                                         project_running, &session.world().runtime(), frame);
                !staged)
                return composition_failed(std::move(staged).error());
            continue;
        }
        if (view.key == control::output_tab) {
            if (auto appended = output.append(result, view.content, *ui_document_, pane.bounds.height - 22); !appended)
                return composition_failed(std::move(appended).error());
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
            ATELIER_TRY_BIND(clear_camera, action({"scene.main_camera_clear"}));
            lighting.append(result, view.content, *ui_document_, this->view, !session.world().running(), label,
                            std::move(*clear_camera));
            continue;
        }
        if (view.key == control::assets_tab) {
            std::optional<ui_language::TextEditBinding> asset_rename;
            if (row_rename && row_rename->cell) {
                const auto& sel_asset = assets.selected();
                if (!sel_asset || sel_asset->path != row_rename->asset)
                    row_rename.reset();
                else
                    asset_rename.emplace(control::inline_rename, row_rename->text, true, row_rename->invalid, true,
                                         browser_control::results, row_rename->cell);
            }
            if (auto appended =
                    assets.append(result, view.content, project, operators.find("scene.add")->poll(context).has_value(),
                                  frame, *ui_document_, asset_rename ? &*asset_rename : nullptr);
                !appended)
                return composition_failed(std::move(appended).error());
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
                        error_text = application::diagnostic_text(eval->inspected_error->message +
                                                                  format_error_coordinates(*eval->inspected_error));
                } else {
                    target_label = eval->display_name.empty()
                                       ? L"(no display)"
                                       : platform::utf8_to_utf16_validated(eval->display_name) + L" [D]";
                    geom = eval->preview_output.get();
                    generation = eval->generation;
                    if (eval->display_error)
                        error_text = application::diagnostic_text(eval->display_error->message +
                                                                  format_error_coordinates(*eval->display_error));
                }
            } else if (const ObjectId gen_comp = active_procedural_generator(session).value_or(ObjectId{});
                       gen_comp != ObjectId{}) {
                const auto comp_st = world::inspect_component(runtime, gen_comp);
                const auto ent_st = comp_st ? world::inspect_entity(runtime, comp_st->owner)
                                            : atelier::Result<world::EntityStatus>{std::unexpected(comp_st.error())};
                const bool gen_active = comp_st && ent_st && comp_st->enabled && ent_st->effective_enabled;
                if (!gen_active) {
                    target_label = std::wstring(session.name(selected)) + L" [Generator disabled]";
                } else {
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
                if (auto cat = session.procedural_node_types(project, net_graph))
                    catalog = std::move(*cat);

                if (network.tab_search().open) {
                    const auto matches = network.filter_catalog(catalog);
                    tab_items->reserve(matches.size());
                    for (std::size_t i = 0; i < matches.size(); ++i) {
                        const bool is_asset = !matches[i].asset_key.empty();
                        std::string short_name =
                            is_asset && !matches[i].display_label.empty() ? matches[i].display_label : matches[i].name;
                        if (!is_asset)
                            if (const auto dot = short_name.find_last_of('.'); dot != short_name.npos)
                                short_name = short_name.substr(dot + 1);
                        const auto short_w = platform::utf8_to_utf16_validated(short_name);
                        const auto pick_key = network_control::tab_pick_first + i;
                        const std::wstring identity =
                            is_asset ? (L"Digital Asset · " + platform::utf8_to_utf16_validated(matches[i].asset_key))
                                     : (platform::utf8_to_utf16_validated(matches[i].name) +
                                        (matches[i].native ? L" · C++ SOP" : L" · Source SOP"));
                        auto pick_help =
                            property_help_.get(pick_key, short_w, identity,
                                               matches[i].description.empty() ? "在画布光标位置创建此程序化几何节点。"
                                                                              : matches[i].description);
                        const bool active_row = i == std::min(network.tab_search().highlight, matches.size() - 1);
                        tab_items->push_back(
                            {network_control::tab_item_first + i,
                             {short_w,
                              std::wstring(is_asset            ? L"Asset"
                                           : matches[i].native ? L"C++"
                                                               : L"Source"),
                              active_row,
                              ui_language::ActionBinding{
                                  pick_key, !session.world().running() && !is_network_graph_locked(session, net_graph),
                                  std::move(pick_help)},
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
                    net_status =
                        L"Cook error: " + application::diagnostic_text(eval->display_error->message +
                                                                       format_error_coordinates(*eval->display_error));
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
            const bool net_locked = is_network_graph_locked(session, net_graph);
            const bool can_edit_net = has_graph && !session.world().running() && !net_locked;
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
            ATELIER_TRY_BIND(layout_binding, net_action({"graph.layout"}));
            layout_binding->enabled = layout_binding->enabled && can_edit_net;
            ATELIER_TRY_BIND(del_binding, net_action({"graph.delete_node"}));
            del_binding->enabled = del_binding->enabled && can_edit_net && has_selected_node;

            const std::wstring graph_title = network_path_title(session, project, net_graph);
            const std::wstring zoom_text = std::format(L"{}%", static_cast<int>(std::lround(network.zoom() * 100.f)));
            const bool is_scratch = has_graph && session.is_scratch_graph(net_graph);
            const double lock_icon = double(net_locked ? ui::ControlIcon::lock : ui::ControlIcon::unlock);
            const auto& ts = network.tab_search();
            ATELIER_TRY_BIND(select_action, net_action({"graph.select", seed_args.finish()}));
            const std::array<ui_language::InputValue, 56> inputs{
                graph_title,
                double(path_icon_val),
                ui_language::ActionBinding{network_control::nav_back, !graph_breadcrumbs.empty() || can_navigate(-1)},
                ui_language::ActionBinding{network_control::nav_forward, can_navigate(1)},
                double(ui::ControlIcon::back),
                double(ui::ControlIcon::forward),
                net_locked,
                lock_icon,
                ui_language::ActionBinding{
                    network_control::lock_toggle, has_graph && !is_scratch && !session.world().running(),
                    property_help_.get(
                        network_control::lock_toggle, L"Lock / Unlock Shared Digital Asset",
                        net_locked ? L"Currently: Locked Preview (Read-Only)" : L"Currently: Unlocked (Live Authoring)",
                        "场景预览与子图下潜默认处于 Locked Preview 防误改模式（可平移缩放、点选查看 Spreadsheet、双击下潜）；点击解锁后所做修改将实时同步到引用该 .procgraph 的所有实例与生成器。")},
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
                std::move(*layout_binding),
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
                std::move(*select_action),
                double(ui::ControlIcon::sliders),
                std::move(*del_binding),
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
            ui_language::ControlRegion panel{&result};
            static_cast<void>(ui_document_->append("NetworkPanel", view.content, keys, inputs, panel));
            const auto tab_it = std::ranges::find(panel.controls, network_control::tab_search_box, &ui::Control::key);
            const auto head = std::span{panel.controls.begin(), tab_it};
            const auto tail = std::span{tab_it, panel.controls.end()};
            result.append(head, panel.revision);
            const bool has_canvas_host =
                std::ranges::find(head, network_control::canvas, &ui::Control::key) != head.end();
            if (has_graph && inspected && has_canvas_host) {
                ui_layout::Size canvas_dip{std::max(160.f, size.width * .36f), std::max(120.f, size.height * .26f)};
                if (frame) {
                    if (const auto* box = frame->find(network_control::canvas);
                        box && box->bounds.width > 0.f && box->bounds.height > 0.f) {
                        canvas_dip = {box->bounds.width / dscale, box->bounds.height / dscale};
                    }
                }
                network.append_canvas(result, network_control::canvas, canvas_dip, inspected, catalog,
                                      selected_graph_node, selected_graph_nodes, !can_edit_net);
            } else {
                result.append({}, empty_network_canvas_revision_);
            }
            if (has_graph && inspected && network.tab_search().open && !tail.empty()) {
                std::vector<ui::Control> tail_controls(tail.begin(), tail.end());
                network.append_tab_categories(tail_controls, network_control::tab_search_box);
                result.append(std::move(tail_controls));
            } else {
                result.append(tail, panel.revision);
            }
            continue;
        }
        const auto short_type = [](StringView name) {
            if (const auto dot = name.rfind('.'); dot != StringView::npos)
                name = name.slice(dot + 1);
            return platform::utf8_to_utf16_validated(name.std_view());
        };
        // Placeholders show the saved type they stand in for.
        const auto object_title = [&](ObjectId object) -> atelier::Result<std::wstring> {
            if (session.has_retained(object)) {
                ATELIER_TRY_BIND(saved, session.retained(object));
                if (!saved->type.empty())
                    return short_type(StringView{saved->type}) + L" (missing type)";
            }
            ATELIER_TRY_BIND(type, runtime.type_of(object));
            ATELIER_TRY_BIND(info, runtime.describe(*type));
            return short_type(StringView{(*info)->name});
        };
        if (view.key == control::entities_tab) {
            // Every member is flattened each frame; only rows near the viewport are described.
            auto& tree = scene_tree_.candidate;
            tree.nodes.clear();
            tree.objects.clear();
            tree.nodes.push_back({ui::no_tree_node, scene_tree_.expanded(ObjectId{}, true), L"World"});
            tree.objects.push_back({});
            Array<std::uint32_t> path{0u}; // Members are in composition preorder.
            Array<std::wstring> filter_titles;
            Array<std::pair<std::uint32_t, std::uint32_t>> deferred_filter_nodes;
            if (!tree_query.empty() && outliner_show_components) {
                filter_titles.reserve(members.size() * 2 + 1);
                deferred_filter_nodes.reserve(members.size() * 2 + 1);
            }
            for (const auto& member : members) {
                ATELIER_TRY_BIND(parent, world::parent_entity(runtime, member.id()));
                while (path.size() > 1 && tree.objects[path.back()] != *parent)
                    path.pop();
                const auto entity_node = static_cast<std::uint32_t>(tree.nodes.size());
                path.add(entity_node);
                tree.nodes.push_back({path[path.size() - 2], scene_tree_.expanded(member.id(), false),
                                      tree_query.empty() ? std::wstring_view{} : session.name(member.id())});
                tree.objects.push_back(member.id());
                if (outliner_show_components) {
                    ATELIER_TRY_BIND(parts, world::components(runtime, member.id()));
                    for (const auto& part : *parts) {
                        const auto comp_node = static_cast<std::uint32_t>(tree.nodes.size());
                        if (!tree_query.empty()) {
                            ATELIER_TRY_BIND(title, object_title(part.id()));
                            const auto title_idx = static_cast<std::uint32_t>(filter_titles.size());
                            filter_titles.emplace(std::move(*title));
                            deferred_filter_nodes.emplace(comp_node, title_idx);
                        }
                        tree.nodes.push_back({entity_node, false, {}});
                        tree.objects.push_back(part.id());
                    }
                }
            }
            for (const auto& [node_idx, title_idx] : deferred_filter_nodes)
                tree.nodes[node_idx].text = filter_titles[title_idx];
            const auto* focused = ui ? binding(ui->focused_control()) : nullptr;
            const ObjectId primary_target = outliner_show_components && inspected != ObjectId{} ? inspected : selected;
            const std::array pinned{primary_target, selected,
                                    focused && focused->kind == Binding::Kind::object ? focused->object
                                                                                      : primary_target,
                                    row_rename && row_rename->object != ObjectId{} ? row_rename->object : selected};
            ATELIER_TRY_BIND(window, scene_tree_.window(primary_target, tree_query,
                                                        ui ? ui->tree_viewport(control::entity_tree)
                                                           : ui::TreeViewport{0, size.height, 12},
                                                        pinned));
            // World stays when the filter matches nothing.
            if (tree.rows.empty()) {
                tree.rows.push_back({0, ui::no_tree_node, 0, false, false, {}});
                *window = {0};
            }
            auto rows = std::make_shared<std::vector<ui_language::Item>>();
            rows->reserve(window->size());
            Set<ObjectId> selected_entities;
            selected_entities.reserve(session.selected_entities().size());
            for (const auto entity : session.selected_entities())
                selected_entities.add(entity);
            ui_language::TreeRowBinding world_row;
            ui::ControlKey rename_anchor_key{};
            for (const auto index : *window) {
                const auto& row = tree.rows[index];
                const auto object = tree.objects[row.node];
                ui_language::TreeRowBinding position{
                    index, row.depth, row.branch, row.expanded, object == primary_target, row.lines};
                if (!row.node) {
                    world_row = position;
                    continue;
                }
                if (session.world().contains(object)) {
                    ATELIER_TRY_BIND(state, world::inspect_entity(runtime, object));
                    ATELIER_TRY_BIND(bound_entity, bind(object));
                    const auto entity_key = *bound_entity;
                    if (row_rename && row_rename->object == object)
                        rename_anchor_key = entity_key;
                    ATELIER_TRY_BIND(appearance, entity_row_appearance(session, object, state->type));
                    position.badges = appearance->badges;
                    position.trailing_icon = state->effective_enabled ? ui::ControlIcon::eye : ui::ControlIcon::eye_off;
                    position.trailing_dimmed = !state->effective_enabled;
                    const bool row_selected = outliner_show_components && inspected != selected
                                                  ? inspected == object
                                                  : selected_entities.contains(object);
                    rows->push_back({entity_key,
                                     {std::wstring(session.name(object)),
                                      state->state == world::EntityState::attached_inactive ||
                                          state->state == world::EntityState::active,
                                      row_selected, static_cast<double>(appearance->icon), position,
                                      std::wstring(appearance->mesh ? L"mesh-entity" : L"entity") +
                                          (!state->effective_enabled ? L" inactive" : L"") +
                                          (appearance->retained ? L" retained" : L"") +
                                          (tree_drop_row == entity_key ? L" drop-target" : L""),
                                      state->can_edit, entity_key}});
                } else {
                    const ObjectId owner_ent = tree.objects[tree.nodes[row.node].parent];
                    ATELIER_TRY_BIND(ent_state, world::inspect_entity(runtime, owner_ent));
                    ATELIER_TRY_BIND(comp_state, world::inspect_component(runtime, object));
                    ATELIER_TRY_BIND(bound_comp, bind(object));
                    ATELIER_TRY_BIND(title, object_title(object));
                    ATELIER_TRY_BIND(comp_icon, component_row_icon(session, object));
                    const bool comp_enabled = comp_state->enabled && ent_state->effective_enabled;
                    position.trailing_icon = comp_enabled ? ui::ControlIcon::eye : ui::ControlIcon::eye_off;
                    position.trailing_dimmed = !comp_enabled;
                    rows->push_back({*bound_comp,
                                     {std::move(*title), ent_state->can_edit, inspected == object,
                                      static_cast<double>(*comp_icon), position,
                                      std::wstring(L"component") + (!comp_enabled ? L" inactive" : L"") +
                                          (session.has_retained(object) ? L" retained" : L""),
                                      false, *bound_comp}});
                }
            }
            if (row_rename && row_rename->object != ObjectId{} &&
                std::ranges::none_of(tree.rows,
                                     [&](const auto& shown) { return tree.objects[shown.node] == row_rename->object; }))
                row_rename.reset();
            ATELIER_TRY_BIND(
                remove_object,
                action({"scene.delete", json::Document{json::Document::literal<R"({"target":"object"})">()}}));
            const bool entity_renaming = row_rename && row_rename->object != ObjectId{} && rename_anchor_key != 0;
            const std::array<ui_language::InputValue, 17> inputs{
                ui_language::ItemList{std::move(rows)},
                static_cast<double>(tree.rows.size()),
                ui_language::TextEditBinding{control::tree_filter, tree_query},
                selected == ObjectId{},
                world_row,
                ui_language::ActionBinding{control::entity_back, can_navigate(-1)},
                ui_language::ActionBinding{control::entity_forward, can_navigate(1)},
                std::wstring(tree_drop_row == control::tree_world ? L"drop-target" : L""),
                outliner_show_components,
                ui_language::ActionBinding{control::outliner_components_toggle, true},
                selected != ObjectId{} && entity_status.can_edit && !session.world().running(),
                control::add_component,
                std::move(*remove_object),
                entity_renaming,
                ui_language::TextEditBinding{control::inline_rename,
                                             entity_renaming ? row_rename->text : std::wstring{}, true,
                                             entity_renaming && row_rename->invalid, true, rename_anchor_key, 0},
                tree_query.empty(),
                ui_language::ActionBinding{control::tree_filter_clear, !tree_query.empty()}};
            const std::array<ui_language::KeyBinding, 7> keys{{{"tree", control::entity_tree},
                                                               {"world", control::tree_world},
                                                               {"header", 24},
                                                               {"path", control::entity_path},
                                                               {"back", control::entity_back},
                                                               {"forward", control::entity_forward},
                                                               {"add", control::add_component}}};
            auto staged = ui_document_->append("SceneOutliner", 21, keys, inputs, result);
            if (!staged)
                return composition_failed(std::move(staged).error());
            scene_tree_.pending = *staged;
            continue;
        }
        if (inspecting_asset && material_object == ObjectId{}) {
            const auto& asset = assets.selected();
            auto entries = std::make_shared<std::vector<ui_language::Item>>();
            const auto add = [&](std::size_t index, std::wstring name, std::wstring value) -> atelier::Result<void> {
                auto& key = resource_properties_[index];
                if (!key)
                    ATELIER_TRY_ASSIGN(key, allocate_binding_key());
                auto& val_key = resource_property_values_[index];
                if (!val_key)
                    ATELIER_TRY_ASSIGN(val_key, allocate_binding_key());
                ui_language::TextEditBinding val_edit{val_key, single_line_field_text(value)};
                val_edit.read_only = true;
                entries->push_back({key, {std::move(name), std::move(val_edit)}});
                return {};
            };
            std::wstring kind = L"Resource";
            auto res_icon = ui::ControlIcon::folder;
            if (asset) {
                kind = asset->kind == assets::AssetKind::mesh               ? L"Mesh"
                       : asset->kind == assets::AssetKind::scene            ? L"Scene"
                       : asset->kind == assets::AssetKind::source           ? L"Source"
                       : asset->kind == assets::AssetKind::folder           ? L"Folder"
                       : asset->kind == assets::AssetKind::material         ? L"Material"
                       : asset->kind == assets::AssetKind::texture          ? L"Texture"
                       : asset->kind == assets::AssetKind::sky              ? L"Sky"
                       : asset->kind == assets::AssetKind::procedural_graph ? L"Procedural Graph"
                                                                            : L"File";
                res_icon = asset->kind == assets::AssetKind::mesh               ? ui::ControlIcon::mesh
                           : asset->kind == assets::AssetKind::scene            ? ui::ControlIcon::scene
                           : asset->kind == assets::AssetKind::source           ? ui::ControlIcon::source_file
                           : asset->kind == assets::AssetKind::material         ? ui::ControlIcon::material
                           : asset->kind == assets::AssetKind::texture          ? ui::ControlIcon::texture
                           : asset->kind == assets::AssetKind::sky              ? ui::ControlIcon::sky_light
                           : asset->kind == assets::AssetKind::procedural_graph ? ui::ControlIcon::procgraph
                           : asset->kind == assets::AssetKind::folder           ? ui::ControlIcon::folder
                                                                                : ui::ControlIcon::object;
                ATELIER_TRY(add(0, L"Type", kind));
                ATELIER_TRY(add(1, L"Resource", platform::utf8_to_utf16_validated(asset->key)));
                ATELIER_TRY(add(2, L"Location", asset->path.empty() ? L"Engine" : asset->path.generic_wstring()));
                if (!asset->path.empty() && asset->kind != assets::AssetKind::folder)
                    ATELIER_TRY(add(3, L"Size", std::to_wstring(asset->bytes) + L" bytes"));
                if (auto mesh = atelier::assets::Reader{}.mesh(asset->key)) {
                    ATELIER_TRY(add(4, L"Triangles", std::to_wstring(mesh->mesh->vertices().size() / 3)));
                }
            }
            ui_language::TextEditBinding res_name{
                control::resource_header_name,
                asset ? single_line_field_text(asset->name) : std::wstring(L"No resource selected"), asset.has_value()};
            res_name.read_only = true;
            const std::array<ui_language::InputValue, 4> inputs{
                kind, std::move(res_name), static_cast<double>(res_icon), ui_language::ItemList{std::move(entries)}};
            if (auto staged = ui_document_->append("ResourceParameters", view.content, {}, inputs, result); !staged)
                return composition_failed(std::move(staged).error());
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
        using CategoryBuckets = std::vector<std::pair<std::string, std::shared_ptr<std::vector<ui_language::Item>>>>;
        const auto bucket_rows = [](CategoryBuckets& buckets, std::string_view category) {
            auto at = std::ranges::find_if(buckets, [&](const auto& group) { return group.first == category; });
            if (at == buckets.end()) {
                buckets.emplace_back(std::string(category), std::make_shared<std::vector<ui_language::Item>>());
                at = std::prev(buckets.end());
            }
            return at->second;
        };
        auto groups = std::make_shared<std::vector<ui_language::Item>>();
        CategoryBuckets category_rows;
        const auto category_properties = [&](std::string_view category) {
            return bucket_rows(category_rows, category);
        };
        const bool param_graph_locked = param_is_graph && is_network_graph_locked(session, net_graph);
        const auto format_enum_choice = [&](const EnumValue& enumeration,
                                            std::string& description) -> atelier::Result<std::wstring> {
            std::wstring choice;
            ATELIER_TRY_BIND(enum_type, runtime.describe(enumeration.type));
            const auto& enum_info = *(*enum_type)->enumeration;
            const auto enum_label = [&](std::size_t index) {
                return index < enum_info.metadata.size() && !enum_info.metadata[index].label.empty()
                           ? platform::utf8_to_utf16_validated(enum_info.metadata[index].label)
                           : parameter_label(enum_info.values[index].first);
            };
            for (std::size_t i = 0; i < enum_info.values.size(); ++i)
                if (enum_info.values[i].second == enumeration.bits) {
                    choice = enum_label(i);
                    if (i < enum_info.metadata.size() && !enum_info.metadata[i].description.empty()) {
                        if (!description.empty())
                            description += "\n";
                        description += enum_info.metadata[i].description;
                    }
                    break;
                }
            if (choice.empty() && enum_info.flags) {
                if (enumeration.bits == 0) {
                    choice = L"None";
                } else {
                    std::uint32_t covered = 0;
                    const auto append_matching = [&](bool single_bit_only) {
                        for (std::size_t i = 0; i < enum_info.values.size(); ++i) {
                            const auto member_bits = enum_info.values[i].second;
                            if (member_bits == 0 || (enumeration.bits & member_bits) != member_bits)
                                continue;
                            if (single_bit_only ? !std::has_single_bit(member_bits) : (member_bits & ~covered) == 0)
                                continue;
                            covered |= member_bits;
                            if (!choice.empty())
                                choice += L" | ";
                            choice += enum_label(i);
                        }
                    };
                    append_matching(true);
                    if (covered != enumeration.bits)
                        append_matching(false);
                }
            }
            return choice;
        };
        const auto append_promoted_parameter_rows =
            [&](ObjectId owner_object, std::string_view graph_key, std::string_view node_name,
                ObjectId generator_component, std::span<const GraphPromotedParameterView> params, bool as_overrides,
                bool editable, std::string_view default_group_title) -> atelier::Result<void> {
            CategoryBuckets local_groups;
            const auto target_rows = [&](std::string_view cat) {
                const std::string_view resolved_cat = cat.empty() ? default_group_title : cat;
                return (parameter_by_category_ && material_object == ObjectId{})
                           ? category_properties(resolved_cat)
                           : bucket_rows(local_groups, resolved_cat);
            };
            for (const auto& pv : params) {
                const std::wstring caption =
                    pv.label.empty() ? parameter_label(pv.name) : platform::utf8_to_utf16_validated(pv.label);
                if (lowercase(caption + L" " + platform::utf8_to_utf16_validated(pv.name + " " + pv.category))
                        .find(folded) == std::wstring::npos)
                    continue;
                auto rows = target_rows(pv.category);
                ui::ControlKey row_key{};
                ATELIER_TRY_ASSIGN(row_key, bind_graph_param(graph_key, node_name, pv.name, Binding::Kind::property_row,
                                                             0, owner_object, generator_component));
                ui::ControlKey ctx_key{};
                ATELIER_TRY_ASSIGN(ctx_key,
                                   bind_graph_param(graph_key, node_name, pv.name, Binding::Kind::graph_param_context,
                                                    0, owner_object, generator_component));
                if (pv.undeclared) {
                    PropertyRow row;
                    row.key = row_key;
                    row.title = caption + L" (Undeclared)";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.choice = L"Retained override";
                    row.target = row_key;
                    row.enabled = false;
                    row.row_enabled = editable;
                    row.change = {row_key, false};
                    row.context_menu = ctx_key;
                    if (as_overrides) {
                        ui::ControlKey ov_key{};
                        ATELIER_TRY_ASSIGN(ov_key, bind_graph_param(graph_key, node_name, pv.name,
                                                                    Binding::Kind::graph_param_override_toggle, 0,
                                                                    owner_object, generator_component));
                        row.override_available = true;
                        row.overridden = true;
                        row.inherit = {ov_key, editable};
                    }
                    row.help =
                        property_help_.get(row_key, row.title, L"Undeclared Digital Asset Override", pv.invalid_reason);
                    rows->push_back(std::move(row).item());
                    continue;
                }
                if (!pv.valid || pv.properties.empty()) {
                    PropertyRow row;
                    row.key = row_key;
                    row.title = caption + L" (Invalid)";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.choice = application::diagnostic_text(pv.invalid_reason);
                    row.target = row_key;
                    row.enabled = false;
                    row.row_enabled = editable;
                    row.change = {row_key, false};
                    row.context_menu = ctx_key;
                    row.help = property_help_.get(row_key, row.title, L"Invalid Graph Parameter", pv.invalid_reason);
                    rows->push_back(std::move(row).item());
                    continue;
                }
                PropertyRow row;
                row.key = row_key;
                row.title = caption;
                row.context_menu = ctx_key;
                row.row_enabled = editable;
                if (as_overrides) {
                    ui::ControlKey ov_key{};
                    ATELIER_TRY_ASSIGN(ov_key, bind_graph_param(graph_key, node_name, pv.name,
                                                                Binding::Kind::graph_param_override_toggle, 0,
                                                                owner_object, generator_component));
                    row.override_available = true;
                    row.overridden = pv.overridden;
                    row.inherit = {ov_key, editable};
                }
                ATELIER_TRY_BIND(value_type, runtime.describe(pv.value_type));
                std::string desc = pv.description;
                if (!as_overrides && !pv.targets.empty()) {
                    if (!desc.empty())
                        desc += "\n";
                    desc += "Targets: ";
                    for (std::size_t t = 0; t < pv.targets.size(); ++t) {
                        if (t > 0)
                            desc += ", ";
                        desc += pv.targets[t];
                    }
                }
                if (pv.is_vector) {
                    std::vector<ui::NumberField> tuple;
                    for (std::size_t lane = 0; lane < pv.properties.size(); ++lane) {
                        const auto& prop = pv.properties[lane];
                        const double val = scalar_number(prop.value);
                        const double def_val =
                            lane < pv.default_scalars.size() ? scalar_number(pv.default_scalars[lane]) : val;
                        const auto [fmin, fmax] = legal_range(prop.value, prop.range);
                        const bool lane_int = std::holds_alternative<std::int32_t>(prop.value) ||
                                              std::holds_alternative<std::uint32_t>(prop.value);
                        ui::ControlKey fkey{};
                        ATELIER_TRY_ASSIGN(fkey, bind_graph_param(graph_key, node_name, pv.name,
                                                                  Binding::Kind::graph_param_field, lane, owner_object,
                                                                  generator_component));
                        tuple.push_back({fkey, std::wstring(1, L"XYZW"[lane]), val, def_val, fmin, fmax,
                                         lane_int ? 1. : .01, lane_int ? 1. : .001,
                                         static_cast<ui::VectorAxis>(lane + 1)});
                        tuple.back().single_precision = std::holds_alternative<float>(prop.value);
                        tuple.back().integral = lane_int;
                    }
                    const bool modified = as_overrides ? pv.overridden : std::ranges::any_of(tuple, [](const auto& f) {
                        return f.value != f.default_value;
                    });
                    const auto first_key = tuple.front().key;
                    row.edit = {ctx_key, editable, modified, std::move(tuple)};
                    row.enabled = editable;
                    row.change = {first_key, editable};
                    row.target = first_key;
                    row.help =
                        property_help_.get(row_key, row.title,
                                           L"Digital Asset Parameter · " + platform::utf8_to_utf16_validated(pv.name) +
                                               L" : " + platform::utf8_to_utf16_validated((*value_type)->name),
                                           described(desc, pv.properties.front().range));
                } else if (pv.properties.front().is_string) {
                    ui::ControlKey tkey{};
                    ATELIER_TRY_ASSIGN(tkey,
                                       bind_graph_param(graph_key, node_name, pv.name, Binding::Kind::graph_param_text,
                                                        0, owner_object, generator_component));
                    row.kind = PropertyRow::Kind::text;
                    row.enabled = editable;
                    row.change = {tkey, false};
                    row.target = tkey;
                    bool sanitized_str = false;
                    auto str_val = single_line_field_text(
                        platform::utf8_to_utf16_validated(pv.properties.front().string_value), &sanitized_str);
                    row.text_edit = {tkey, std::move(str_val), editable};
                    row.text_edit.read_only = sanitized_str;
                    row.help = property_help_.get(row_key, row.title,
                                                  L"Digital Asset Parameter · " +
                                                      platform::utf8_to_utf16_validated(pv.name) + L" : String",
                                                  desc);
                } else {
                    const auto& prop = pv.properties.front();
                    const bool boolean = std::holds_alternative<bool>(prop.value);
                    const auto* enumeration = std::get_if<EnumValue>(&prop.value);
                    const bool integral = std::holds_alternative<std::int32_t>(prop.value) ||
                                          std::holds_alternative<std::uint32_t>(prop.value);
                    const auto [minimum, maximum] = legal_range(prop.value, prop.range);
                    ui::ControlKey fkey{};
                    ATELIER_TRY_ASSIGN(fkey,
                                       bind_graph_param(graph_key, node_name, pv.name, Binding::Kind::graph_param_field,
                                                        0, owner_object, generator_component));
                    const double value = scalar_number(prop.value);
                    const double initial =
                        !pv.default_scalars.empty() ? scalar_number(pv.default_scalars.front()) : value;
                    const bool modified = as_overrides ? pv.overridden : (value != initial);
                    ui_language::EditBinding edit{
                        ctx_key,
                        editable,
                        modified,
                        {{fkey, L"", value, initial, minimum, maximum, integral ? 1. : .01, integral ? 1. : .001}}};
                    edit.fields.front().single_precision = std::holds_alternative<float>(prop.value);
                    edit.fields.front().integral = integral;
                    if (const auto slider = slider_range(prop.range);
                        slider && !boolean && !enumeration && slider->first >= minimum && slider->second <= maximum) {
                        ATELIER_TRY_BIND(slider_key, bind_graph_param(graph_key, node_name, pv.name,
                                                                      Binding::Kind::graph_param_slider, 0,
                                                                      owner_object, generator_component));
                        edit.fields.front().slider = ui::SliderRange{*slider_key, slider->first, slider->second};
                    }
                    std::wstring choice;
                    if (enumeration)
                        ATELIER_TRY_ASSIGN(choice, format_enum_choice(*enumeration, desc));
                    row.kind = boolean       ? PropertyRow::Kind::boolean
                               : enumeration ? PropertyRow::Kind::enumeration
                                             : PropertyRow::Kind::number;
                    row.edit = std::move(edit);
                    row.checked = boolean && std::get<bool>(prop.value);
                    row.enabled = editable;
                    row.change = {fkey, editable};
                    row.choice = std::move(choice);
                    row.target = fkey;
                    row.help =
                        property_help_.get(row_key, row.title,
                                           L"Digital Asset Parameter · " + platform::utf8_to_utf16_validated(pv.name) +
                                               L" : " + platform::utf8_to_utf16_validated((*value_type)->name),
                                           described(desc, prop.range));
                }
                rows->push_back(std::move(row).item());
            }
            for (auto& [cat, rows] : local_groups)
                if (!rows->empty()) {
                    ui::ControlKey key{};
                    ATELIER_TRY_ASSIGN(key, bind_graph_param(graph_key, node_name, cat, Binding::Kind::property_group,
                                                             0, owner_object, generator_component));
                    groups->push_back({key,
                                       {platform::utf8_to_utf16_validated(cat), double(ui::ControlIcon::sliders),
                                        ui_language::ItemList{std::move(rows)}}});
                }
            return {};
        };
        struct ParameterMaterials {
            std::vector<presentation::MaterialBinding> bindings;

            std::optional<presentation::StaticMeshState> mesh;

            std::optional<presentation::ProceduralSource> generator;
        };
        const auto append_properties = [&](ObjectId object, TypeId type,
                                           const ParameterMaterials& materials) -> atelier::Result<void> {
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
            ATELIER_TRY_BIND(info, runtime.describe(type));
            const auto type_name = platform::utf8_to_utf16_validated((*info)->name);
            ATELIER_TRY_BIND(title, object_title(object));
            const bool group_matches = lowercase(*title).find(folded) != std::wstring::npos;
            auto component_properties = std::make_shared<std::vector<ui_language::Item>>();
            auto properties = component_properties;
            ATELIER_TRY_BIND(schema, session.properties(object));
            const bool node_categorized =
                param_is_graph && param_node_name.has_value() && !parameter_by_category_ &&
                material_object == ObjectId{} && std::ranges::any_of(*schema, [&](const Property& prop) {
                    return !prop.category.empty() && property_visible_when(runtime, *schema, prop);
                });
            CategoryBuckets node_category_rows;
            const auto node_category_properties = [&](std::string_view category) {
                return bucket_rows(node_category_rows, category);
            };
            ATELIER_TRY_BIND(is_light, runtime.is_a(type, session.presentation_types().light_component));
            std::optional<presentation::LightSettings> light;
            if (*is_light)
                ATELIER_TRY_ASSIGN(light, presentation::read_light(runtime, object));
            const auto* type_desc = *info;
            const bool is_sop_node = param_is_graph && param_node_name.has_value() && object == param_object;
            const bool is_asset_node = is_sop_node && type_desc->name == "procedural.AssetNode";
            if (is_asset_node) {
                if (const auto* gnode = session.graph_node(net_graph, *param_node_name)) {
                    ui::ControlKey slot_key{};
                    ATELIER_TRY_ASSIGN(slot_key, bind_graph_param(net_graph, *param_node_name, "asset",
                                                                  Binding::Kind::graph_node_asset_slot, 0, object));
                    std::wstring asset_choice = gnode->asset_key.empty() ? L"None" : asset_name(gnode->asset_key);
                    if (!gnode->asset_key.empty() && project && !project->entry(gnode->asset_key) &&
                        !session.has_graph(gnode->asset_key))
                        asset_choice = L"(missing) " + asset_choice;
                    if (group_matches ||
                        lowercase(L"Digital Asset " + asset_choice).find(folded) != std::wstring::npos) {
                        PropertyRow row;
                        row.key = slot_key;
                        row.title = L"Digital Asset";
                        row.kind = PropertyRow::Kind::enumeration;
                        row.enabled = !session.world().running() && !param_graph_locked;
                        row.change = {slot_key, row.enabled};
                        row.choice = std::move(asset_choice);
                        row.target = slot_key;
                        row.help = property_help_.get(
                            slot_key, row.title, L"Digital Asset (.procgraph)",
                            "选择此 AssetNode 实例引用的 .procgraph 数字资产；下拉菜单中也可一键下潜进入子图查看或编辑。");
                        component_properties->push_back(std::move(row).item());
                    }
                }
            }
            for (std::size_t index = 0; index < schema->size(); ++index) {
                const auto& property = (*schema)[index];
                auto parent = *info;
                bool color{};
                for (std::size_t depth = 0; depth + 1 < property.path.size(); ++depth) {
                    ATELIER_TRY_BIND(field, property_field(*parent, property.path[depth]));
                    color = parent->id == session.presentation_types().light_settings && (*field)->name == "color";
                    ATELIER_TRY_ASSIGN(parent, runtime.describe((*field)->value.type));
                }
                const bool is_vec_prop = property.path.size() > 1 &&
                                         (parent->math.vector() || parent->math.shape == math::Shape::quaternion);
                std::optional<std::string> controlling_param;
                bool any_lane_controlled = false;
                if (is_sop_node) {
                    if (is_vec_prop) {
                        const auto vec_name = property.name.substr(0, property.name.rfind('.'));
                        controlling_param = session.graph_controlling_parameter(net_graph, *param_node_name, vec_name);
                        any_lane_controlled = controlling_param.has_value();
                        const auto count = parent->math.components();
                        const auto prefix = std::span(property.path).first(property.path.size() - 1);
                        for (std::size_t lane = 0;
                             !any_lane_controlled && lane < count && index + lane < schema->size(); ++lane) {
                            const auto& lf = (*schema)[index + lane];
                            if (lf.path.size() != property.path.size() ||
                                !std::equal(prefix.begin(), prefix.end(), lf.path.begin()))
                                break;
                            if (session.graph_controlling_parameter(net_graph, *param_node_name, lf.name))
                                any_lane_controlled = true;
                        }
                    } else {
                        controlling_param =
                            session.graph_controlling_parameter(net_graph, *param_node_name, property.name);
                        any_lane_controlled = controlling_param.has_value();
                    }
                }
                if (!any_lane_controlled && !property_visible_when(runtime, *schema, property)) {
                    if (is_vec_prop) {
                        const auto count = parent->math.components();
                        const auto prefix = std::span(property.path).first(property.path.size() - 1);
                        std::size_t skip = 0;
                        while (skip + 1 < count && index + skip + 1 < schema->size() &&
                               (*schema)[index + skip + 1].path.size() == property.path.size() &&
                               std::equal(prefix.begin(), prefix.end(), (*schema)[index + skip + 1].path.begin())) {
                            ++skip;
                        }
                        index += skip;
                    }
                    continue;
                }
                if (parameter_by_category_ && material_object == ObjectId{}) {
                    const std::string_view category =
                        property.category.empty() ? std::string_view{"General"} : std::string_view{property.category};
                    properties = category_properties(category);
                } else if (node_categorized) {
                    const std::string_view category =
                        property.category.empty() ? std::string_view{"General"} : std::string_view{property.category};
                    properties = node_category_properties(category);
                }
                const bool can_edit_object = session.is_graph_object(object)
                                                 ? (!session.world().running() && !param_graph_locked)
                                                 : entity_status.can_edit;
                const bool editable = can_edit_object && !property.read_only && !controlling_param.has_value();
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
                if (is_vec_prop) {
                    const auto count = parent->math.components();
                    const auto name = property.name.substr(0, property.name.rfind('.'));
                    const auto prefix = std::span(property.path).first(property.path.size() - 1);
                    const bool spatial_position = std::ranges::equal(property.path, session.types().position_fields[0]);
                    const bool spatial_rotation = std::ranges::equal(property.path, session.types().rotation_fields[0]);
                    const bool spatial_scale = std::ranges::equal(property.path, session.types().scale_fields[0]);
                    math::Vector3 euler{};
                    if (spatial_rotation) {
                        ATELIER_TRY_BIND(trs, world::read_transform(runtime, object));
                        euler = euler_degrees(trs->rotation);
                    }
                    std::vector<ui::NumberField> tuple;
                    std::string lane_binding_note;
                    bool matches =
                        group_matches || lowercase(platform::utf8_to_utf16_validated(name + " " + property.label + " " +
                                                                                     property.category))
                                                 .find(folded) != std::wstring::npos;
                    for (std::size_t lane = 0; lane < count && index + lane < schema->size(); ++lane) {
                        const auto& field = (*schema)[index + lane];
                        if (field.path.size() != property.path.size() ||
                            !std::equal(prefix.begin(), prefix.end(), field.path.begin()))
                            break;
                        matches |=
                            lowercase(platform::utf8_to_utf16_validated(field.name)).find(folded) != std::wstring::npos;
                        if (spatial_rotation && lane == 3)
                            continue;
                        const auto lane_ctrl =
                            (is_sop_node && !controlling_param)
                                ? session.graph_controlling_parameter(net_graph, *param_node_name, field.name)
                                : std::nullopt;
                        if (lane_ctrl) {
                            if (!lane_binding_note.empty())
                                lane_binding_note += ", ";
                            lane_binding_note += std::string(1, "xyzw"[lane]) + "↗" + *lane_ctrl;
                        }
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
                        const bool lane_integral = std::holds_alternative<std::int32_t>(field.value) ||
                                                   std::holds_alternative<std::uint32_t>(field.value);
                        ui::ControlKey key{};
                        ATELIER_TRY_ASSIGN(key, property_key(field.path, spatial_rotation ? Binding::Kind::rotation
                                                                                          : Binding::Kind::field));
                        tuple.push_back({key, std::wstring(1, (color ? L"RGBW" : L"XYZW")[lane]), value,
                                         spatial_position || spatial_rotation ? 0
                                         : spatial_scale                      ? 1
                                                                              : value,
                                         minimum, maximum, spatial_rotation || lane_integral ? 1 : .01,
                                         lane_integral ? 1 : (spatial_rotation ? .1 : .001),
                                         static_cast<ui::VectorAxis>(lane + 1)});
                        tuple.back().single_precision = parent->math.precision == math::Precision::f32;
                        tuple.back().integral = lane_integral;
                        tuple.back().enabled = !lane_ctrl.has_value();
                    }
                    index += count - 1;
                    if (matches) {
                        const auto field_key = tuple.front().key;
                        ui::ControlKey sop_ctx_key{};
                        if (is_sop_node)
                            ATELIER_TRY_ASSIGN(sop_ctx_key, bind_graph_param(net_graph, *param_node_name, name,
                                                                             Binding::Kind::graph_param_context, 0,
                                                                             object, {}, prefix));
                        // Swatch shows the display colour; HDR emission is clamped for the chip only.
                        const auto channel = [&](std::size_t lane) {
                            return tuple.size() > lane ? std::clamp(tuple[lane].value, 0.0, 1.0) : 0.0;
                        };
                        const double r = channel(0);
                        const double g = channel(1);
                        const double b = channel(2);
                        PropertyRow row;
                        ATELIER_TRY_ASSIGN(row.key, property_key(prefix, Binding::Kind::property_row));
                        row.title =
                            spatial_rotation ? std::wstring(L"Rotation (°)") : property_caption(name, property.label);
                        if (controlling_param)
                            row.title += L"  [↗ " + platform::utf8_to_utf16_validated(*controlling_param) + L"]";
                        else if (!lane_binding_note.empty())
                            row.title += L"  [" + platform::utf8_to_utf16_validated(lane_binding_note) + L"]";
                        row.edit = {sop_ctx_key, editable, false, std::move(tuple)};
                        if (spatial_position || spatial_rotation || spatial_scale) {
                            const auto kind = spatial_position   ? Binding::Kind::position_default
                                              : spatial_rotation ? Binding::Kind::rotation_default
                                                                 : Binding::Kind::scale_default;
                            ATELIER_TRY_ASSIGN(row.edit.group, bind(object, {}, kind));
                        }
                        row.enabled = editable;
                        if (is_sop_node) {
                            row.context_menu = sop_ctx_key;
                            row.row_enabled = can_edit_object && !property.read_only;
                        }
                        row.change = {field_key, editable};
                        row.target = field_key;
                        if (color) {
                            row.swatch = std::array{r, g, b};
                            ATELIER_TRY_ASSIGN(row.pick.target, property_key(prefix, Binding::Kind::color_pick));
                            row.pick.enabled = editable;
                            std::ranges::find(bindings_, row.pick.target, &Binding::key)->color_maximum =
                                row.edit.fields.front().maximum;
                        }
                        row.help = property_help_.get(row.key, row.title,
                                                      type_name + L"." + platform::utf8_to_utf16_validated(name) +
                                                          L" : " + platform::utf8_to_utf16_validated(parent->name) +
                                                          (spatial_rotation ? L" · Euler (°)" : L""),
                                                      described(property.description, property.range));
                        properties->push_back(std::move(row).item());
                    }
                    continue;
                }
                auto caption = property_caption(property.name, property.label);
                if (controlling_param)
                    caption += L"  [↗ " + platform::utf8_to_utf16_validated(*controlling_param) + L"]";
                ui::ControlKey sop_ctx_key{};
                if (is_sop_node)
                    ATELIER_TRY_ASSIGN(sop_ctx_key, bind_graph_param(net_graph, *param_node_name, property.name,
                                                                     Binding::Kind::graph_param_context, 0, object, {},
                                                                     property.path));
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
                    ATELIER_TRY_BIND(text_key, property_key(property.path, Binding::Kind::text_field));
                    PropertyRow row;
                    ATELIER_TRY_ASSIGN(row.key, property_key(property.path, Binding::Kind::property_row));
                    row.title = std::move(caption);
                    row.kind = PropertyRow::Kind::text;
                    row.enabled = editable;
                    if (is_sop_node) {
                        row.context_menu = sop_ctx_key;
                        row.row_enabled = can_edit_object && !property.read_only;
                    }
                    row.change = {*text_key, false};
                    row.target = *text_key;
                    bool sanitized_prop = false;
                    auto prop_val = single_line_field_text(platform::utf8_to_utf16_validated(property.string_value),
                                                           &sanitized_prop);
                    row.text_edit = {*text_key, std::move(prop_val), editable};
                    row.text_edit.read_only = sanitized_prop;
                    row.help = property_help_.get(row.key, row.title,
                                                  platform::utf8_to_utf16_validated(type_desc->name) + L"." +
                                                      platform::utf8_to_utf16_validated(property.name) + L" : String",
                                                  property.description);
                    properties->push_back(std::move(row).item());
                    continue;
                }
                ATELIER_TRY_BIND(leaf, property_field(*parent, property.path.back()));
                const auto& defaults = (*leaf)->default_leaves;
                const bool boolean = std::holds_alternative<bool>(property.value);
                const auto* enumeration = std::get_if<EnumValue>(&property.value);
                const bool integral = std::holds_alternative<std::int32_t>(property.value) ||
                                      std::holds_alternative<std::uint32_t>(property.value);
                const auto [minimum, maximum] = legal_range(property.value, property.range);
                ATELIER_TRY_BIND(key, property_key(property.path));
                const double value = scalar_number(property.value);
                const double initial = defaults.size() == 1 ? scalar_number(defaults.front()) : value;
                ui_language::EditBinding edit{
                    sop_ctx_key,
                    editable,
                    value != initial,
                    {{*key, L"", value, initial, minimum, maximum, integral ? 1 : .01, integral ? 1 : .001}}};
                edit.fields.front().single_precision = std::holds_alternative<float>(property.value);
                edit.fields.front().integral = integral;
                if (const auto slider = slider_range(property.range);
                    slider && !boolean && !enumeration && slider->first >= minimum && slider->second <= maximum) {
                    ATELIER_TRY_BIND(slider_key, property_key(property.path, Binding::Kind::slider));
                    edit.fields.front().slider = ui::SliderRange{*slider_key, slider->first, slider->second};
                }
                std::string description = property.description;
                std::wstring choice;
                if (enumeration)
                    ATELIER_TRY_ASSIGN(choice, format_enum_choice(*enumeration, description));
                PropertyRow row;
                ATELIER_TRY_ASSIGN(row.key, property_key(property.path, Binding::Kind::property_row));
                row.title = std::move(caption);
                row.edit = std::move(edit);
                row.kind = boolean       ? PropertyRow::Kind::boolean
                           : enumeration ? PropertyRow::Kind::enumeration
                                         : PropertyRow::Kind::number;
                row.checked = boolean && std::get<bool>(property.value);
                row.enabled = editable;
                if (is_sop_node) {
                    row.context_menu = sop_ctx_key;
                    row.row_enabled = can_edit_object && !property.read_only;
                }
                row.change = {*key, editable};
                row.choice = std::move(choice);
                row.target = *key;
                ATELIER_TRY_BIND(value_type, runtime.describe(property.type));
                row.help = property_help_.get(row.key, row.title,
                                              platform::utf8_to_utf16_validated(type_desc->name) + L"." +
                                                  platform::utf8_to_utf16_validated(property.name) + L" : " +
                                                  platform::utf8_to_utf16_validated((*value_type)->name),
                                              described(description, property.range));
                properties->push_back(std::move(row).item());
            }
            properties =
                parameter_by_category_ && material_object == ObjectId{} && (materials.mesh || materials.generator)
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
            if (materials.generator) {
                properties = parameter_by_category_ && material_object == ObjectId{} ? category_properties("Procedural")
                                                                                     : component_properties;
                std::wstring graph_choice =
                    materials.generator->source.empty() ? L"None" : asset_name(materials.generator->source);
                mark_unavailable(assets::AssetKind::procedural_graph, materials.generator->source, graph_choice);
                ATELIER_TRY_BIND(graph_key,
                                 bind_material(object, {}, Binding::Kind::procedural_graph_slot, {}, "graph"));
                selector(
                    *graph_key, L"Procedural Graph", std::move(graph_choice),
                    "选择此生成器求值的 .procgraph 资产；菜单内也可一键进入二维 Network 画布编辑图节点，或将实例烘焙为普通静态网格实体。");
                properties =
                    parameter_by_category_ && material_object == ObjectId{} && (materials.mesh || materials.generator)
                        ? category_properties("Material")
                        : component_properties;
            }
            const auto material_choice_label = [&](ObjectId resource) -> atelier::Result<std::wstring> {
                ATELIER_TRY_BIND(material, session.inspect_material(resource));
                auto label = material->scene ? std::wstring(L"Scene Instance") : asset_name(material->asset_key);
                mark_unavailable(assets::AssetKind::material, material->asset_key, label);
                return label;
            };
            if (materials.mesh) {
                const auto& mesh = materials.mesh->mesh;
                if (!mesh.resolved && !mesh.source.empty() &&
                    (group_matches ||
                     lowercase(L"Mesh " + platform::utf8_to_utf16_validated(mesh.source)).find(folded) !=
                         std::wstring::npos)) {
                    PropertyRow row;
                    ATELIER_TRY_ASSIGN(row.key, bind_material(object, {}, Binding::Kind::saved_data, {}, "mesh"));
                    row.title = L"Mesh";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.enabled = false;
                    row.change = {row.key, false};
                    row.choice = platform::utf8_to_utf16_validated(mesh.source);
                    mark_unavailable(assets::AssetKind::mesh, mesh.source, row.choice);
                    row.target = row.key;
                    row.help = property_help_.get(
                        row.key, row.title, L"Unavailable resource",
                        "网格资源缺失或无法读取：组件不绘制，原引用和材质绑定原样保存。恢复资源后重新打开场景即可使用。");
                    properties->push_back(std::move(row).item());
                }
                if (mesh.resolved)
                    for (std::size_t i = 0; i < mesh.resolved->materials().size(); ++i) {
                        const auto slot_name = presentation::material_slot_name(*mesh.resolved, i);
                        std::wstring choice = L"Mesh Default";
                        const auto at =
                            std::ranges::find(materials.bindings, slot_name, &presentation::MaterialBinding::slot);
                        if (at != materials.bindings.end()) {
                            ATELIER_TRY_ASSIGN(choice, material_choice_label(at->resource.id()));
                        } else if (const auto fallback = std::ranges::find(mesh.defaults, slot_name,
                                                                           &presentation::MaterialBinding::slot);
                                   fallback != mesh.defaults.end()) {
                            ATELIER_TRY_BIND(source, material::material_key(runtime, fallback->resource.id()));
                            auto name = asset_name(*source);
                            mark_unavailable(assets::AssetKind::material, *source, name);
                            choice += L" · " + name;
                        }
                        ATELIER_TRY_BIND(slot_key,
                                         bind_material(object, {}, Binding::Kind::material_slot, {}, slot_name));
                        selector(
                            *slot_key, L"Material · " + platform::utf8_to_utf16_validated(slot_name), std::move(choice),
                            "为这个 Mesh slot 选择 Material 资源。Mesh Default 恢复网格自带材质；修改参与当前 Scene 的撤销与保存。");
                    }
            } else if (materials.generator) {
                std::wstring choice = L"Default Grid";
                if (const auto at = std::ranges::find(materials.bindings, std::string_view{"default"},
                                                      &presentation::MaterialBinding::slot);
                    at != materials.bindings.end()) {
                    ATELIER_TRY_ASSIGN(choice, material_choice_label(at->resource.id()));
                }
                ATELIER_TRY_BIND(slot_key, bind_material(object, {}, Binding::Kind::material_slot, {}, "default"));
                selector(*slot_key, L"Material · default", std::move(choice),
                         "为程序化生成的表面多边形选择 Material 资源；CopyToPoints 实例各自保留其网格资产材质。");
            }
            for (auto& [category, rows] : node_category_rows)
                if (!rows->empty()) {
                    ui::ControlKey key{};
                    ATELIER_TRY_ASSIGN(key, bind_graph_param(net_graph, *param_node_name, category,
                                                             Binding::Kind::property_group, 0, object));
                    groups->push_back({key,
                                       {platform::utf8_to_utf16_validated(category), double(ui::ControlIcon::sliders),
                                        ui_language::ItemList{std::move(rows)}}});
                }
            if (!component_properties->empty()) {
                ATELIER_TRY_BIND(group_key, property_key({}, Binding::Kind::property_group));
                auto grp_icon = ui::ControlIcon::component;
                if (is_sop_node)
                    grp_icon = ui::ControlIcon::procgraph;
                else if (object == selected) {
                    if (const auto app = entity_row_appearance(session, object, type))
                        grp_icon = app->icon;
                    else
                        grp_icon = ui::ControlIcon::object;
                } else if (const auto ci = component_row_icon(session, object))
                    grp_icon = *ci;
                groups->push_back(
                    {*group_key, {*title, double(grp_icon), ui_language::ItemList{std::move(component_properties)}}});
            }
            const auto append_promoted_diagnostic = [&](std::string_view graph_key, std::string_view node_name,
                                                        ObjectId generator_component, std::string_view group_title,
                                                        std::string_view message) -> atelier::Result<void> {
                auto diag_rows = std::make_shared<std::vector<ui_language::Item>>();
                ui::ControlKey rkey{};
                ATELIER_TRY_ASSIGN(rkey, bind_graph_param(graph_key, node_name, "$diagnostic",
                                                          Binding::Kind::saved_data, 0, object, generator_component));
                PropertyRow row;
                row.key = rkey;
                row.title = L"Digital Asset Error";
                row.kind = PropertyRow::Kind::enumeration;
                row.choice = application::diagnostic_text(message);
                row.target = rkey;
                row.enabled = false;
                row.change = {rkey, false};
                row.help = property_help_.get(rkey, row.title, L"Digital Asset Diagnostic", message);
                diag_rows->push_back(std::move(row).item());
                ui::ControlKey gkey{};
                ATELIER_TRY_ASSIGN(gkey,
                                   bind_graph_param(graph_key, node_name, group_title, Binding::Kind::property_group, 0,
                                                    object, generator_component));
                groups->push_back({gkey,
                                   {platform::utf8_to_utf16_validated(group_title), double(ui::ControlIcon::sliders),
                                    ui_language::ItemList{std::move(diag_rows)}}});
                return {};
            };
            if (is_asset_node) {
                const auto asset_params = session.graph_asset_node_parameters(net_graph, *param_node_name, project);
                if (!asset_params) {
                    ATELIER_TRY(append_promoted_diagnostic(net_graph, *param_node_name, ObjectId{}, "Asset Parameters",
                                                           asset_params.error().message));
                } else if (!asset_params->empty()) {
                    ATELIER_TRY(append_promoted_parameter_rows(
                        object, net_graph, *param_node_name, ObjectId{}, *asset_params, true,
                        !session.world().running() && !param_graph_locked, "Asset Parameters"));
                }
            } else if (param_is_graph && !param_node_name.has_value() && object == param_object) {
                const auto root_params = session.graph_promoted_parameters(net_graph);
                if (!root_params) {
                    ATELIER_TRY(append_promoted_diagnostic(net_graph, {}, ObjectId{}, "Promoted Parameters",
                                                           root_params.error().message));
                } else if (!root_params->empty()) {
                    ATELIER_TRY(append_promoted_parameter_rows(object, net_graph, {}, ObjectId{}, *root_params, false,
                                                               !session.world().running() && !param_graph_locked,
                                                               "Promoted Parameters"));
                }
            } else if (materials.generator && !materials.generator->source.empty()) {
                const auto gen_params = session.generator_promoted_parameters(object, project);
                if (!gen_params) {
                    ATELIER_TRY(append_promoted_diagnostic(materials.generator->source, {}, object,
                                                           "Generator Parameters", gen_params.error().message));
                } else if (!gen_params->empty()) {
                    const auto comp_state = world::inspect_component(runtime, object);
                    const bool comp_editable =
                        comp_state && comp_state->valid && entity_status.can_edit && !session.world().running();
                    ATELIER_TRY(append_promoted_parameter_rows(object, materials.generator->source, {}, object,
                                                               *gen_params, true, comp_editable,
                                                               "Generator Parameters"));
                }
            }
            if (!session.has_retained(object))
                return {};
            ATELIER_TRY_BIND(saved, session.retained(object));
            if (saved->entries.empty())
                return {};
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
                ATELIER_TRY_ASSIGN(row.key, bind_material(object, {}, Binding::Kind::saved_data, {}, path));
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
            if (!categorized && !saved_rows->empty()) {
                ATELIER_TRY_BIND(saved_key, bind(object, {}, Binding::Kind::saved_data));
                groups->push_back({*saved_key,
                                   {L"Saved Data · " + *title, double(ui::ControlIcon::component),
                                    ui_language::ItemList{std::move(saved_rows)}}});
            }
            return {};
        };
        const auto append_material = [&](ObjectId object, ObjectId component = ObjectId{},
                                         std::string_view slot = std::string_view{}) -> atelier::Result<void> {
            ATELIER_TRY_BIND(material, session.inspect_material(object));
            const auto title = (material->source  ? L"Material Source"
                                : material->scene ? L"Scene Material Instance"
                                                  : L"Material Instance") +
                               (slot.empty() ? std::wstring{} : L" · " + platform::utf8_to_utf16_validated(slot));
            const bool editable = !material->source && !session.world().running();
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
            for (const auto& field : material->declarations) {
                if (lowercase(title + L" " + platform::utf8_to_utf16_validated(field.path)).find(folded) ==
                    std::wstring::npos)
                    continue;
                const auto local = std::ranges::find(material->entries, field.path, &material::Override::path);
                const bool overridden = local != material->entries.end() && local->enabled;
                // Disabled/mismatched local entries remain in the owning view; numbers show effective values.
                PropertyRow row;
                ATELIER_TRY_ASSIGN(row.key, key(field.path, Binding::Kind::property_row));
                row.title = platform::utf8_to_utf16_validated(field.path);
                row.enabled = editable && overridden;
                row.override_available = !material->source;
                row.overridden = overridden;
                ATELIER_TRY_ASSIGN(row.inherit.target, key(field.path, Binding::Kind::material_override));
                row.inherit.enabled = editable;
                if (!field.components) {
                    row.kind = PropertyRow::Kind::enumeration;
                    ATELIER_TRY_ASSIGN(row.target, key(field.path, Binding::Kind::material_texture));
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
                        ATELIER_TRY_BIND(lane_key, key(field.path, Binding::Kind::material_field, lane));
                        row.edit.fields.push_back(
                            {*lane_key, field.components == 1 ? L"" : std::wstring(1, L"XYZW"[lane]), value, initial,
                             -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), .01, .001,
                             static_cast<ui::VectorAxis>(lane + 1)});
                        row.edit.fields.back().single_precision = true;
                    }
                    row.target = row.edit.fields.front().key;
                    row.change = {row.target, row.enabled};
                    if (field.components >= 3) {
                        row.swatch = std::array{std::clamp(lane_value(field.value, 0), 0., 1.),
                                                std::clamp(lane_value(field.value, 1), 0., 1.),
                                                std::clamp(lane_value(field.value, 2), 0., 1.)};
                        ATELIER_TRY_ASSIGN(row.pick.target, key(field.path, Binding::Kind::color_pick));
                        row.pick.enabled = row.enabled;
                        std::ranges::find(bindings_, row.pick.target, &Binding::key)->color_maximum =
                            std::numeric_limits<float>::max();
                    }
                }
                row.help = property_help_.get(
                    row.key, row.title, L"Material declaration · " + platform::utf8_to_utf16_validated(field.path),
                    "数值显示当前有效值。关闭覆盖会保留本地值；右键 Discard Local Entry 删除本地条目并恢复继承。");
                rows->push_back(std::move(row).item());
            }
            for (const auto& entry : material->entries)
                if (std::ranges::find(material->declarations, entry.path, &MaterialParameterView::path) ==
                    material->declarations.end()) {
                    PropertyRow row;
                    ATELIER_TRY_ASSIGN(row.key, key(entry.path, Binding::Kind::property_row));
                    row.title = platform::utf8_to_utf16_validated(entry.path) + L" (Undeclared)";
                    row.kind = PropertyRow::Kind::enumeration;
                    row.choice = L"Retained local entry";
                    row.target = row.key;
                    row.enabled = false;
                    row.change = {row.key, false};
                    row.override_available = true;
                    row.overridden = entry.enabled;
                    ATELIER_TRY_ASSIGN(row.inherit.target, key(entry.path, Binding::Kind::material_override));
                    row.inherit.enabled = editable;
                    rows->push_back(std::move(row).item());
                }
            for (std::size_t i = 0; i < material->diagnostics.size(); ++i) {
                const auto& diagnostic = material->diagnostics[i];
                PropertyRow row;
                ATELIER_TRY_ASSIGN(row.key, key(diagnostic.path, Binding::Kind::saved_data, i));
                row.title = platform::utf8_to_utf16_validated(diagnostic.path);
                row.kind = PropertyRow::Kind::enumeration;
                row.choice = application::diagnostic_text(diagnostic.message);
                row.target = row.key;
                row.enabled = false;
                row.change = {row.key, false};
                rows->push_back(std::move(row).item());
            }
            ATELIER_TRY_BIND(group_key, key({}, Binding::Kind::property_group));
            groups->push_back(
                {*group_key, {title, double(ui::ControlIcon::material), ui_language::ItemList{std::move(rows)}}});
            return {};
        };
        if (material_object != ObjectId{}) {
            ATELIER_TRY(append_material(material_object));
            std::wstring name = platform::utf8_to_utf16_validated(material_inspection);
            if (const auto* entry = project ? project->entry(material_inspection) : nullptr)
                name = entry->path.stem().wstring();
            ui_language::TextEditBinding mat_name{control::material_header_name, single_line_field_text(name)};
            mat_name.read_only = true;
            const std::array<ui_language::InputValue, 8> inputs{
                std::move(mat_name),
                session.material_dirty(material_inspection),
                ui_language::ItemList{std::move(groups)},
                parameter_search_visible,
                ui_language::TextEditBinding{control::parameter_search, query},
                query.empty(),
                ui_language::ActionBinding{control::parameter_search_clear, !query.empty()},
                ui_language::ActionBinding{control::parameter_search_toggle}};
            if (auto staged = ui_document_->append("MaterialParameters", view.content, {}, inputs, result); !staged)
                return composition_failed(std::move(staged).error());
            continue;
        }
        const auto append_object = [&](ObjectId object) -> atelier::Result<void> {
            ATELIER_TRY_BIND(type, runtime.type_of(object));
            const auto& types = session.presentation_types();
            ParameterMaterials materials;
            ATELIER_TRY_BIND(is_mesh, runtime.is_a(*type, types.static_mesh_component));
            if (*is_mesh) {
                ATELIER_TRY_ASSIGN(materials.mesh, presentation::read_static_mesh(runtime, object));
            } else {
                ATELIER_TRY_BIND(generator, runtime.is_a(*type, types.procedural_generator_component));
                if (*generator)
                    ATELIER_TRY_ASSIGN(materials.generator, presentation::read_procedural_source(runtime, object));
            }
            if (materials.mesh || materials.generator)
                ATELIER_TRY_ASSIGN(materials.bindings, presentation::read_material_bindings(runtime, object));
            ATELIER_TRY(append_properties(object, *type, materials));
            // Keep the snapshot owning both explicit and default bindings alive through group construction.
            std::vector<const presentation::MaterialBinding*> shown;
            if (materials.mesh) {
                const auto& mesh = materials.mesh->mesh;
                if (mesh.resolved)
                    for (std::size_t i = 0; i < mesh.resolved->materials().size(); ++i) {
                        const auto name = presentation::material_slot_name(*mesh.resolved, i);
                        if (const auto at =
                                std::ranges::find(materials.bindings, name, &presentation::MaterialBinding::slot);
                            at != materials.bindings.end())
                            shown.push_back(&*at);
                        else if (const auto fallback =
                                     std::ranges::find(mesh.defaults, name, &presentation::MaterialBinding::slot);
                                 fallback != mesh.defaults.end())
                            shown.push_back(&*fallback);
                    }
            }
            for (const auto& binding : materials.bindings)
                if (std::ranges::find(shown, &binding) == shown.end())
                    shown.push_back(&binding);
            for (const auto* binding : shown)
                ATELIER_TRY(append_material(binding->resource.id(), object, binding->slot));
            return {};
        };
        if (param_object != ObjectId{}) {
            ATELIER_TRY(append_object(param_object));
            if (!param_is_graph && param_object == selected) {
                ATELIER_TRY_BIND(parts, world::components(runtime, selected));
                for (const auto& component : *parts)
                    ATELIER_TRY(append_object(component.id()));
            }
        }
        for (auto& [category, rows] : category_rows)
            if (!rows->empty()) {
                auto& key = category_keys_[category];
                if (!key)
                    ATELIER_TRY_ASSIGN(key, allocate_binding_key());
                groups->push_back({key,
                                   {platform::utf8_to_utf16_validated(category), double(ui::ControlIcon::sliders),
                                    ui_language::ItemList{std::move(rows)}}});
            }
        constexpr std::array aliases{L"transform entity position 变换 位置 x", L"transform entity position 变换 位置 y",
                                     L"transform entity position 变换 位置 z"};
        constexpr std::array names{L"X", L"Y", L"Z"};
        world::Trs trs{};
        if (show_position)
            ATELIER_TRY_ASSIGN(trs, world::read_transform(runtime, *transform));
        const auto definitions = session.parameters();
        std::vector<ui::NumberField> fields;
        bool group_modified{};
        for (std::size_t axis = 0; axis < definitions.size(); ++axis) {
            const auto& definition = definitions[axis];
            const double value = show_position ? trs.position[axis] : definition.default_value;
            group_modified |= value != definition.default_value;
            if (!show_position || std::wstring_view(aliases[axis]).find(folded) == std::wstring_view::npos)
                continue;
            ATELIER_TRY_BIND(position_key, bind(*transform, definition.path));
            fields.push_back({*position_key, names[axis], value, definition.default_value, definition.minimum,
                              definition.maximum, definition.drag_step, .001, static_cast<ui::VectorAxis>(axis + 1)});
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
            const auto angles = euler_degrees(trs.rotation);
            ATELIER_TRY_BIND(rotation_group, bind(*transform, {}, Binding::Kind::rotation_default));
            rotation_edit = {*rotation_group, entity_status.can_edit, angles != math::Vector3{}, {}};
            ATELIER_TRY_BIND(scale_group, bind(*transform, {}, Binding::Kind::scale_default));
            scale_edit = {*scale_group, entity_status.can_edit, trs.scale != math::Vector3{1, 1, 1}, {}};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                ATELIER_TRY_BIND(rotation_key,
                                 bind(*transform, session.types().rotation_fields[axis], Binding::Kind::rotation));
                rotation_edit.fields.push_back({*rotation_key, names[axis], angles[axis], 0, -360000, 360000, 1, .1,
                                                static_cast<ui::VectorAxis>(axis + 1)});
                ATELIER_TRY_BIND(scale_key, bind(*transform, session.types().scale_fields[axis]));
                scale_edit.fields.push_back({*scale_key, names[axis], trs.scale[axis], 1, -Session::maximum_scale,
                                             Session::maximum_scale, .01, .001, static_cast<ui::VectorAxis>(axis + 1)});
                rotation_edit.fields.back().single_precision = scale_edit.fields.back().single_precision = true;
            }
        }
        const bool matches = position_matches || rotation_matches || scale_matches;
        const bool empty = groups->empty() && !matches;
        ui_language::EditBinding position{74, entity_status.can_edit && show_position, group_modified,
                                          std::move(fields)};
        if (show_position)
            ATELIER_TRY_ASSIGN(position.group, bind(*transform, {}, Binding::Kind::position_default));
        const bool entity_selected = !param_is_graph && param_object != ObjectId{} && param_object == selected;
        std::array<ui_language::TooltipBinding, 3> transform_descriptions{};
        if (show_position) {
            ATELIER_TRY_BIND(type, runtime.describe(entity_status.type));
            const auto help = [&](std::span<const FieldId> lane, ui::ControlKey key, std::wstring_view title,
                                  bool rotation = false) -> atelier::Result<ui_language::TooltipBinding> {
                auto owner = *type;
                const FieldInfo* field{};
                for (const auto id : lane.first(lane.size() - 1)) {
                    ATELIER_TRY_ASSIGN(field, property_field(*owner, id));
                    ATELIER_TRY_ASSIGN(owner, runtime.describe(field->value.type));
                }
                return property_help_.get(key, title,
                                          platform::utf8_to_utf16_validated((*type)->name) + L"." +
                                              platform::utf8_to_utf16_validated(field->name) + L" : " +
                                              platform::utf8_to_utf16_validated(owner->name) +
                                              (rotation ? L" · Euler (°)" : L""),
                                          field->description);
            };
            ATELIER_TRY_ASSIGN(transform_descriptions[0],
                               help(session.types().position_fields[0], 70, L"Position (m)"));
            ATELIER_TRY_ASSIGN(transform_descriptions[1],
                               help(session.types().rotation_fields[0], rotation_edit.group, L"Rotation (°)", true));
            ATELIER_TRY_ASSIGN(transform_descriptions[2],
                               help(session.types().scale_fields[0], scale_edit.group, L"Scale"));
        }
        ui_language::TooltipBinding type_help;
        if (param_object != ObjectId{}) {
            ATELIER_TRY_BIND(id, runtime.type_of(param_object));
            ATELIER_TRY_BIND(type, runtime.describe(*id));
            type_help = property_help_.get(52, short_type(StringView{(*type)->name}),
                                           platform::utf8_to_utf16_validated((*type)->name), (*type)->description);
        }
        const std::wstring param_graph_title =
            in_world_net ? network_path_title(session, project, net_graph) : graph_path_title(project);
        std::wstring param_title = L"Object";
        if (param_object != ObjectId{}) {
            ATELIER_TRY_BIND(title, object_title(param_object));
            param_title = std::move(*title);
        }
        auto header_icon = ui::ControlIcon::component;
        if (entity_selected) {
            ATELIER_TRY_BIND(appearance, entity_row_appearance(session, selected, entity_status.type));
            header_icon = appearance->icon;
        }
        const std::wstring header_name = param_object == ObjectId{} ? L"No selection"
                                         : param_node_name ? platform::utf8_to_utf16_validated(*param_node_name)
                                         : param_is_graph  ? param_graph_title
                                         : entity_selected ? std::wstring(session.name(selected))
                                                           : param_title;
        const bool can_rename_header =
            param_node_name ? !session.world().running() : (entity_selected && entity_status.can_edit);
        ui::ControlKey name_key{77};
        if (entity_selected || param_node_name)
            ATELIER_TRY_ASSIGN(name_key, bind(param_object, {}, Binding::Kind::name));
        bool sanitized_header = false;
        auto safe_header_name = single_line_field_text(header_name, &sanitized_header);
        ui_language::TextEditBinding header_name_edit{name_key, std::move(safe_header_name),
                                                      param_object != ObjectId{}};
        header_name_edit.read_only = (param_object != ObjectId{} && !can_rename_header) || sanitized_header;
        const std::array<ui_language::InputValue, 30> inputs{
            std::move(param_title),
            std::move(header_name_edit),
            ui_language::ActionBinding{control::parameter_back, can_navigate(-1)},
            ui_language::ActionBinding{control::parameter_forward, can_navigate(1)},
            double(ui::ControlIcon::world),
            param_object != ObjectId{},
            ui_language::ItemList{std::move(groups)},
            matches,
            std::move(position),
            ui_language::TextEditBinding{control::parameter_search, query},
            empty,
            double(header_icon),
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
            param_graph_title,
            std::wstring(can_rename_header ? L"object-name" : L"object-name readonly-inline"),
            query.empty(),
            ui_language::ActionBinding{control::parameter_search_clear, !query.empty()}};
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
        if (auto staged = ui_document_->append("Parameters", view.content, keys, inputs, result); !staged)
            return composition_failed(std::move(staged).error());
    }
    const auto preview = placement.preview.value_or(ui::Rect{});
    const std::array<ui_language::InputValue, 5> preview_inputs{placement.preview.has_value(), double(preview.x),
                                                                double(preview.y), double(preview.width),
                                                                double(preview.height)};
    const std::array<ui_language::KeyBinding, 1> preview_keys{{{"preview", placement.preview_key}}};
    if (auto staged = ui_document_->append("DockPreview", 0, preview_keys, preview_inputs, result); !staged)
        return composition_failed(std::move(staged).error());
    std::vector<ui::MenuItem> next_menu_items;
    std::vector<ComponentType> component_choices;
    std::vector<MaterialChoice> material_choices;
    std::vector<NetworkPathChoice> network_path_choices;
    bool dismiss_menu = false;
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
            dismiss_menu = true;
        else {
            const auto operator_item = [&](OperatorCall call,
                                           const MenuBinding* definition = nullptr) -> atelier::Result<ui::MenuItem> {
                if (!definition)
                    definition = std::ranges::find_if(menu_bindings, [&](const auto& row) {
                        return row.call.id == call.id && row.call.args == call.args;
                    });
                if (definition == std::end(menu_bindings))
                    return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_definition,
                                                          "Missing operator menu presentation: " + call.id});
                if (definition->asset_kind &&
                    (!assets.selected() || assets.selected()->kind != *definition->asset_kind))
                    call.context.asset = std::string{};
                if (call.id == "scene.rename" && context.region == RegionKind::assets)
                    call.id = "asset.rename";
                const auto key_text = shortcut(call, context.region);
                ATELIER_TRY_BIND(binding, action(std::move(call)));
                ATELIER_TRY_BIND(selected, bindings_[binding_cursor_ - 1].call->state(operators, context));
                ui::MenuItem item{binding->target,
                                  std::wstring(definition->title),
                                  binding->enabled,
                                  selected->value_or(false),
                                  {},
                                  std::wstring(key_text),
                                  definition->separator};
                item.radio_group = definition->radio_group;
                item.behavior = definition->radio_group ? ui::MenuBehavior::radio
                                : *selected             ? ui::MenuBehavior::checkbox
                                                        : ui::MenuBehavior::command;
                return item;
            };
            const auto append_operator = [&](std::vector<ui::MenuItem>& items, OperatorCall call,
                                             const MenuBinding* definition = nullptr) -> atelier::Result<void> {
                ATELIER_TRY_BIND(item, operator_item(std::move(call), definition));
                items.push_back(std::move(*item));
                return {};
            };
            const auto calls =
                [&](std::initializer_list<OperatorCall> calls) -> atelier::Result<std::vector<ui::MenuItem>> {
                std::vector<ui::MenuItem> result;
                for (const auto& call : calls)
                    ATELIER_TRY(append_operator(result, call));
                return result;
            };
            const auto viewport_calls = [&](ui::ControlKey key) -> atelier::Result<std::vector<ui::MenuItem>> {
                // These exact literals are initialized by menu_bindings at startup.
                switch (key) {
                case control::viewport_tools_menu:
                    return calls({{"tool.settings", json::Document::literal<R"({"tool":"select"})">()},
                                  {"tool.settings", json::Document::literal<R"({"tool":"transform"})">()},
                                  {"tool.settings", json::Document::literal<R"({"tool":"move"})">()},
                                  {"tool.settings", json::Document::literal<R"({"tool":"rotate"})">()},
                                  {"tool.settings", json::Document::literal<R"({"tool":"scale"})">()}});
                case control::viewport_snap:
                    return calls({{"tool.toggle", json::Document::literal<R"({"setting":"grid_snap"})">()},
                                  {"tool.settings", json::Document::literal<R"({"grid_spacing":0.1})">()},
                                  {"tool.settings", json::Document::literal<R"({"grid_spacing":0.2})">()},
                                  {"tool.settings", json::Document::literal<R"({"grid_spacing":0.5})">()},
                                  {"tool.settings", json::Document::literal<R"({"grid_spacing":1.0})">()},
                                  {"tool.toggle", json::Document::literal<R"({"setting":"snap_move"})">()},
                                  {"tool.toggle", json::Document::literal<R"({"setting":"snap_rotate"})">()},
                                  {"tool.toggle", json::Document::literal<R"({"setting":"snap_scale"})">()},
                                  {"tool.settings", json::Document::literal<R"({"rotation_step":1.0})">()},
                                  {"tool.settings", json::Document::literal<R"({"rotation_step":5.0})">()},
                                  {"tool.settings", json::Document::literal<R"({"rotation_step":15.0})">()},
                                  {"tool.settings", json::Document::literal<R"({"rotation_step":45.0})">()},
                                  {"scene.snap_selection", json::Document::literal<R"({"to":"grid"})">()},
                                  {"scene.snap_selection", json::Document::literal<R"({"to":"cursor"})">()},
                                  {"cursor.to_selection"},
                                  {"cursor.reset"},
                                  {"cursor.to_grid"},
                                  {"tool.toggle", json::Document::literal<R"({"setting":"cursor_surface"})">()}});
                case control::viewport_camera:
                    return calls({{"view.axis", json::Document::literal<R"({"view":"perspective"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"front"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"back"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"left"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"right"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"top"})">()},
                                  {"view.axis", json::Document::literal<R"({"view":"bottom"})">()},
                                  {"view.frame_selected"},
                                  {"view.from_camera"},
                                  {"view.reset"}});
                case control::viewport_pivot:
                    return calls({{"tool.settings", json::Document::literal<R"({"pivot":"median"})">()},
                                  {"tool.settings", json::Document::literal<R"({"pivot":"cursor"})">()},
                                  {"cursor.to_selection"},
                                  {"cursor.reset"},
                                  {"cursor.to_grid"}});
                case control::viewport_orientation: {
                    std::vector<ui::MenuItem> items;
                    const std::initializer_list<OperatorCall> items_calls{
                        {"tool.settings", json::Document::literal<R"({"orientation":"global"})">()},
                        {"tool.settings", json::Document::literal<R"({"orientation":"local"})">()}};
                    ATELIER_TRY_ASSIGN(items, calls(items_calls));
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
            const auto header_menu = [&](ui::ControlKey key) -> atelier::Result<std::vector<ui::MenuItem>> {
                if (key == control::viewport_view_menu) {
                    std::vector<ui::MenuItem> items;
                    ATELIER_TRY_ASSIGN(items, calls({{"view.frame_selected"}, {"view.frame_all"}}));
                    ATELIER_TRY_BIND(viewpoints, viewport_calls(control::viewport_camera));
                    items.push_back(submenu(control::view_viewpoint_menu, L"Viewpoint", std::move(*viewpoints), true));
                    std::vector<ui::MenuItem> options;
                    const std::initializer_list<OperatorCall> options_calls{
                        {"view.toggle", json::Document::literal<R"({"option":"options"})">()},
                        {"view.toggle", json::Document::literal<R"({"option":"fps"})">()}};
                    ATELIER_TRY_ASSIGN(options, calls(options_calls));
                    options.front().separator_before = true;
                    for (auto& item : options)
                        items.push_back(std::move(item));
                    return items;
                }
                if (key == control::viewport_select_menu) {
                    std::vector<ui::MenuItem> items;
                    const std::initializer_list<OperatorCall> items_calls{
                        {"scene.select_all", json::Document::literal<R"({"action":"select"})">()},
                        {"scene.select_all", json::Document::literal<R"({"action":"deselect"})">()},
                        {"scene.select_all", json::Document::literal<R"({"action":"invert"})">()},
                        {"view.select_box"}};
                    ATELIER_TRY_ASSIGN(items, calls(items_calls));
                    items.front().separator_before = false;
                    std::vector<ui::MenuItem> modes;
                    const std::initializer_list<OperatorCall> modes_calls{
                        {"tool.settings", json::Document::literal<R"({"select_mode":"set"})">()},
                        {"tool.settings", json::Document::literal<R"({"select_mode":"extend"})">()},
                        {"tool.settings", json::Document::literal<R"({"select_mode":"subtract"})">()},
                        {"tool.settings", json::Document::literal<R"({"select_mode":"invert"})">()},
                        {"tool.settings", json::Document::literal<R"({"select_mode":"intersect"})">()}};
                    ATELIER_TRY_ASSIGN(modes, calls(modes_calls));
                    modes.front().separator_before = false;
                    items.push_back(submenu(control::select_mode_menu, L"Selection Mode", std::move(modes), true));
                    return items;
                }
                if (key == control::viewport_header_add) {
                    std::vector<ui::MenuItem> items;
                    for (const auto& definition : menu_bindings)
                        if (definition.group == MenuGroup::create)
                            ATELIER_TRY(append_operator(items, definition.call));
                    return items;
                }
                if (key == control::viewport_object_menu) {
                    std::vector<ui::MenuItem> items;
                    const std::initializer_list<OperatorCall> items_calls{
                        {"scene.duplicate"}, {"scene.delete", json::Document::literal<R"({"target":"entities"})">()}};
                    ATELIER_TRY_ASSIGN(items, calls(items_calls));
                    items.front().separator_before = false;
                    std::vector<ui::MenuItem> clear;
                    const std::initializer_list<OperatorCall> clear_calls{
                        {"scene.transform_clear", json::Document::literal<R"({"channel":"location"})">()},
                        {"scene.transform_clear", json::Document::literal<R"({"channel":"rotation"})">()},
                        {"scene.transform_clear", json::Document::literal<R"({"channel":"scale"})">()}};
                    ATELIER_TRY_ASSIGN(clear, calls(clear_calls));
                    clear.front().separator_before = false;
                    items.push_back(submenu(control::object_clear_menu, L"Clear", std::move(clear), true));
                    std::vector<ui::MenuItem> snap;
                    const std::initializer_list<OperatorCall> snap_calls{
                        {"scene.snap_selection", json::Document::literal<R"({"to":"grid"})">()},
                        {"scene.snap_selection", json::Document::literal<R"({"to":"cursor"})">()},
                        {"cursor.to_selection"},
                        {"cursor.to_grid"},
                        {"cursor.reset"}};
                    ATELIER_TRY_ASSIGN(snap, calls(snap_calls));
                    for (auto& item : snap)
                        item.separator_before = false;
                    items.push_back(submenu(control::object_snap_menu, L"Snap", std::move(snap)));
                    std::vector<ui::MenuItem> transform;
                    ATELIER_TRY_ASSIGN(transform, calls({{"scene.transform_add"}, {"scene.transform_remove"}}));
                    transform.front().separator_before = true;
                    for (auto& item : transform)
                        items.push_back(std::move(item));
                    return items;
                }
                return {};
            };
            const auto net_op = [&](std::string id, json::Document args = {}) {
                return bind_operator(
                    {std::move(id), std::move(args), {.selection = {}, .region = RegionKind::network}});
            };
            std::vector<ui::MenuItem> items = dock.menu_items(source->key, frame);
            if ((source->context_target ? source->context_target : source->key) == world_control::setup) {
                const std::initializer_list<OperatorCall> items_calls{
                    {"view.lighting_fit", json::Document::literal<R"({"mode":"scene"})">()},
                    {"view.lighting_fit", json::Document::literal<R"({"mode":"scene_focus"})">()}};
                ATELIER_TRY_ASSIGN(items, calls(items_calls));
                items[0].text = L"Fit Scene";
                items[1].text = L"Scene + Follow Focus";
            } else if (auto lighting_items =
                           lighting.menu(source->context_target ? source->context_target : source->key, this->view);
                       !lighting_items.empty()) {
                items = std::move(lighting_items);
            } else if (auto header_items = header_menu(source->key); !header_items) {
                return std::unexpected(std::move(header_items).error());
            } else if (!header_items->empty()) {
                items = std::move(*header_items);
            } else if (const auto* row = binding(source->key); row && row->kind == Binding::Kind::object &&
                                                               row->path.empty() &&
                                                               source->kind == ui::ControlKind::tree_item) {
                const auto append_proc_actions = [&](ObjectId owner_entity, ObjectId comp_id) -> atelier::Result<bool> {
                    const auto proc = presentation::read_procedural_source(runtime, comp_id);
                    if (!proc)
                        return false;
                    const bool can_edit = !session.world().running();
                    if (!proc->source.empty()) {
                        json::Builder open_args;
                        static_cast<void>(open_args.set(open_args.root(), "asset", proc->source));
                        ATELIER_TRY_BIND(open_key, bind_operator({"graph.open", open_args.finish()}));
                        items.push_back({*open_key, L"Edit Procedural Graph", can_edit, false, {}, L"", true});
                    }
                    bool has_instances = false;
                    if (proc->output)
                        for (const auto& col : proc->output->publication.collections)
                            if (col.transforms && !col.transforms->empty())
                                has_instances = true;
                    const ContextOverride target{.selection = SelectionContext{owner_entity, comp_id, {owner_entity}},
                                                 .region = RegionKind::scene_tree};
                    ATELIER_TRY_BIND(bake_key, bind_operator({"procedural.bake", {}, target}));
                    items.push_back({*bake_key,
                                     L"Bake to Entities",
                                     can_edit && has_instances,
                                     false,
                                     {},
                                     L"",
                                     proc->source.empty()});
                    return true;
                };
                if (session.world().contains(row->object)) {
                    const auto owner_entity = row->object;
                    const std::initializer_list<OperatorCall> items_calls{
                        {"scene.rename"},
                        {"scene.duplicate"},
                        {"scene.delete", json::Document::literal<R"({"target":"entities"})">()},
                        {"scene.parent_set", json::Document::literal<R"({"parent":null})">()},
                        {"view.frame_selected"}};
                    ATELIER_TRY_ASSIGN(items, calls(items_calls));
                    for (auto& item : items)
                        item.separator_before = false;
                    items[3].separator_before = items[4].separator_before = true;
                    items[0].icon = ui::ControlIcon::rename;
                    items[1].icon = ui::ControlIcon::duplicate;
                    items[2].icon = ui::ControlIcon::trash;
                    items[3].icon = ui::ControlIcon::unparent;
                    ATELIER_TRY_BIND(parts, world::components(runtime, owner_entity));
                    for (const auto& part : *parts) {
                        ATELIER_TRY_BIND(added_proc, append_proc_actions(owner_entity, part.id()));
                        if (*added_proc)
                            break;
                    }
                } else {
                    ATELIER_TRY_BIND(comp_state, world::inspect_component(runtime, row->object));
                    const std::initializer_list<OperatorCall> comp_calls{
                        {"scene.delete", json::Document::literal<R"({"target":"object"})">()}};
                    ATELIER_TRY_ASSIGN(items, calls(comp_calls));
                    items[0].text = L"Remove Component";
                    items[0].icon = ui::ControlIcon::trash;
                    items[0].separator_before = false;
                    if (session.has_retained(row->object)) {
                        ATELIER_TRY(append_operator(items, {"scene.discard_saved_data"}));
                        items.back().separator_before = true;
                    }
                    ATELIER_TRY(append_proc_actions(comp_state->owner, row->object));
                }
            } else if (const auto* group = menu_group(source->key)) {
                if (group->id == MenuGroup::view) {
                    ATELIER_TRY(append_operator(items, {"view.frame_selected"}));
                    for (const auto& [key, title] : std::array<std::pair<ui::ControlKey, std::wstring_view>, 4>{
                             {{control::viewport_tools_menu, L"Interaction Mode"},
                              {control::viewport_pivot, L"Pivot"},
                              {control::viewport_snap, L"Snapping"},
                              {control::viewport_camera, L"Camera View"}}}) {
                        ATELIER_TRY_BIND(children, viewport_calls(key));
                        items.push_back({.key = key,
                                         .text = std::wstring(title),
                                         .edit_command = {},
                                         .children = std::move(*children)});
                    }
                    ATELIER_TRY(
                        append_operator(items, {"view.toggle", json::Document::literal<R"({"option":"options"})">()}));
                } else {
                    for (const auto& definition : menu_bindings)
                        if (definition.group == group->id) {
                            ATELIER_TRY(append_operator(items, definition.call, &definition));
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
                ATELIER_TRY_BIND(
                    item, operator_item({"scene.delete", json::Document::literal<R"({"target":"entities"})">()}));
                item->separator_before = false;
                item->text = session.selected_entities().size() == 1
                                 ? L"Delete Entity"
                                 : L"Delete " + std::to_wstring(session.selected_entities().size()) + L" Entities";
                items.push_back(std::move(*item));
            } else if (source->key == control::viewport && menu->group == control::viewport_axis_pie) {
                const std::initializer_list<OperatorCall> items_calls{
                    {"view.axis", json::Document::literal<R"({"view":"left"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"right"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"bottom"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"top"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"front"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"back"})">()},
                    {"view.axis", json::Document::literal<R"({"view":"perspective"})">()},
                    {"view.frame_selected"}};
                ATELIER_TRY_ASSIGN(items, calls(items_calls));
                items.back().text = L"Focus";
            } else if (source->key == control::viewport && menu->group == control::viewport_add_menu) {
                // Blender Shift+A: everything the Create group can place at the 3D cursor.
                for (const auto& definition : menu_bindings)
                    if (definition.group == MenuGroup::create)
                        ATELIER_TRY(append_operator(items, definition.call));
            } else if (source->key == control::viewport || source->key == control::viewport_tools_menu) {
                ATELIER_TRY_ASSIGN(items, viewport_calls(control::viewport_tools_menu));
                if (source->key == control::viewport) {
                    ATELIER_TRY_BIND(children, viewport_calls(control::viewport_snap));
                    items.push_back({.key = control::viewport_snap,
                                     .text = L"Snap",
                                     .edit_command = {},
                                     .children = std::move(*children)});
                    const std::initializer_list<OperatorCall> commands_calls{
                        {"view.frame_selected"},
                        {"scene.add", json::Document::literal<R"({"kind":"mesh"})">()},
                        {"scene.duplicate"},
                        {"scene.delete", json::Document::literal<R"({"target":"entities"})">()}};
                    ATELIER_TRY_BIND(commands, calls(commands_calls));
                    for (auto& item : *commands)
                        items.push_back(std::move(item));
                }
            } else if (source->key == control::viewport_snap) {
                ATELIER_TRY_ASSIGN(items, viewport_calls(source->key));
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
                ATELIER_TRY_ASSIGN(items, viewport_calls(source->key));
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
                    ATELIER_TRY_BIND(open_key, bind_operator({"graph.open", args.finish()}));
                    items.push_back({*open_key, std::move(label), true, active_graph == gkey, {}, L"", g == 0});
                }
                if (active_graph.empty())
                    for (std::size_t m = 0; m < members.size(); ++m) {
                        ATELIER_TRY_BIND(navigation_key, bind(members[m].id(), {}, Binding::Kind::navigation));
                        items.push_back({*navigation_key,
                                         std::wstring(session.name(members[m].id())),
                                         true,
                                         selected == members[m].id(),
                                         {},
                                         L"",
                                         m == 0});
                    }
                for (auto& item : items) {
                    item.behavior = ui::MenuBehavior::radio;
                    item.radio_group = source->key;
                }
            } else if (source->key == network_control::path_combo) {
                const auto push_choice = [&](NetworkPathChoice choice, std::wstring title, bool checked,
                                             bool separator = false, std::wstring_view heading = {}) {
                    const ui::ControlKey ckey = network_control::path_choice_first + network_path_choices.size();
                    network_path_choices.push_back(std::move(choice));
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
                if (!active_graph.empty()) {
                    ATELIER_TRY_BIND(exit_key, bind_operator({"graph.exit"}));
                    items.push_back({*exit_key, L"Return 3D Viewport to World", true, false, {}});
                }
            } else if (network_control::is_header_menu(source->key)) {
                const bool has_net = !net_graph.empty() && session.has_graph(net_graph);
                const bool net_locked = is_network_graph_locked(session, net_graph);
                const bool can_edit = has_net && !session.world().running() && !net_locked;
                const bool has_sel = has_net && (!selected_graph_node.empty() || !selected_graph_nodes.empty());
                const auto* graph_doc = has_net ? session.graph_document(net_graph) : nullptr;
                const bool has_nodes = graph_doc && !graph_doc->nodes.empty();
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
                    ATELIER_TRY_BIND(undo_key, net_op("edit.undo"));
                    items.push_back({*undo_key,
                                     L"Undo",
                                     can_edit && session.graph_can_undo(net_graph, false),
                                     false,
                                     {},
                                     L"Ctrl+Z"});
                    ATELIER_TRY_BIND(redo_key, net_op("edit.redo"));
                    items.push_back({*redo_key,
                                     L"Redo",
                                     can_edit && session.graph_can_undo(net_graph, true),
                                     false,
                                     {},
                                     L"Ctrl+Y"});
                    ATELIER_TRY_BIND(duplicate_nodes_key, net_op("graph.duplicate_nodes"));
                    items.push_back(
                        {*duplicate_nodes_key, L"Duplicate", can_edit && has_sel, false, {}, L"Shift+D", true});
                    ATELIER_TRY_BIND(extract_asset_key, net_op("graph.extract_asset"));
                    items.push_back(
                        {*extract_asset_key, L"Extract Digital Asset…", can_edit && has_sel && project, false, {}});
                    ATELIER_TRY_BIND(delete_node_key, net_op("graph.delete_node"));
                    items.push_back({*delete_node_key, L"Delete", can_edit && has_sel, false, {}, L"Del / X"});
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
                        ATELIER_TRY_BIND(set_frozen_key, net_op("graph.set_frozen", frz_args.finish()));
                        items.push_back({*set_frozen_key,
                                         is_frz ? L"Unfreeze Node" : L"Freeze Node",
                                         can_edit && sel_id != ObjectId{},
                                         is_frz,
                                         {},
                                         L"",
                                         true});
                        json::Builder disc_args;
                        static_cast<void>(disc_args.set(disc_args.root(), "to_node", selected_graph_node));
                        ATELIER_TRY_BIND(disconnect_key, net_op("graph.disconnect", disc_args.finish()));
                        items.push_back({*disconnect_key,
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
                    ATELIER_TRY_BIND(select_key, net_op("graph.select", seed_args.finish()));
                    items.push_back({*select_key, L"Inspect Graph Seed", has_net, false, {}});
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
                    ATELIER_TRY_BIND(layout_key, net_op("graph.layout"));
                    items.push_back({*layout_key, L"Auto Arrange Nodes", can_edit, false, {}, L"L"});
                    items.push_back(
                        {network_control::open_shape_picker, L"Shape Palette…", can_edit, false, {}, L"Z", true});
                    items.push_back({network_control::open_color_picker, L"Color Palette…", can_edit, false, {}, L"C"});
                }
            } else if (source->key == network_control::canvas) {
                const bool has_net = !net_graph.empty() && session.has_graph(net_graph);
                const bool net_locked = is_network_graph_locked(session, net_graph);
                const bool can_edit = has_net && !session.world().running() && !net_locked;
                if (menu->group == network_control::context_node_menu && !network_context_node.empty()) {
                    const auto* graph_doc = has_net ? session.graph_document(net_graph) : nullptr;
                    const auto* ctx_gnode = has_net ? session.graph_node(net_graph, network_context_node) : nullptr;
                    bool ctx_is_asset_node{};
                    if (ctx_gnode && ctx_gnode->node.registered()) {
                        ATELIER_TRY_BIND(type, runtime.type_of(ctx_gnode->node.id()));
                        ATELIER_TRY_BIND(info, runtime.describe(*type));
                        ctx_is_asset_node = (*info)->name == "procedural.AssetNode";
                    }
                    if (ctx_is_asset_node) {
                        json::Builder dive_args;
                        static_cast<void>(dive_args.set(dive_args.root(), "asset", net_graph));
                        static_cast<void>(dive_args.set(dive_args.root(), "node", network_context_node));
                        ATELIER_TRY_BIND(dive_key, net_op("graph.dive_in", dive_args.finish()));
                        items.push_back({*dive_key,
                                         L"Dive Into Digital Asset",
                                         !session.world().running() && !ctx_gnode->asset_key.empty(),
                                         false,
                                         {},
                                         L"Enter"});
                    }
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
                        ATELIER_TRY_BIND(set_display_key, net_op("graph.set_display", args.finish()));
                        items.push_back({*set_display_key,
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
                        ATELIER_TRY_BIND(set_template_key, net_op("graph.set_template", args.finish()));
                        items.push_back({*set_template_key,
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
                        ATELIER_TRY_BIND(set_bypass_key, net_op("graph.set_bypass", args.finish()));
                        items.push_back({*set_bypass_key,
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
                        ATELIER_TRY_BIND(set_frozen_key, net_op("graph.set_frozen", args.finish()));
                        items.push_back({*set_frozen_key,
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
                        ATELIER_TRY_BIND(duplicate_nodes_key, net_op("graph.duplicate_nodes", dup_args.finish()));
                        items.push_back({*duplicate_nodes_key, L"Duplicate", can_edit, false, {}, L"Shift+D", true});
                    }
                    {
                        json::Builder ext_args;
                        if (std::ranges::find(selected_graph_nodes, network_context_node) == selected_graph_nodes.end())
                            static_cast<void>(ext_args.set(ext_args.root(), "nodes", network_context_node));
                        ATELIER_TRY_BIND(extract_key, net_op("graph.extract_asset", ext_args.finish()));
                        items.push_back(
                            {*extract_key, L"Extract Digital Asset…", can_edit && project != nullptr, false, {}, L""});
                    }
                    if (can_edit && target_id != ObjectId{}) {
                        json::Builder disc_args;
                        static_cast<void>(disc_args.set(disc_args.root(), "to_node", network_context_node));
                        ATELIER_TRY_BIND(disconnect_key, net_op("graph.disconnect", disc_args.finish()));
                        items.push_back({*disconnect_key, L"Disconnect All Inputs", true, false, {}, L"", true});
                    }
                    {
                        json::Builder del_args;
                        static_cast<void>(del_args.set(del_args.root(), "name", network_context_node));
                        ATELIER_TRY_BIND(delete_node_key, net_op("graph.delete_node", del_args.finish()));
                        items.push_back({*delete_node_key, L"Delete Node", can_edit, false, {}, L"Del", true});
                    }
                } else {
                    items.push_back({network_control::add_node, L"Add Node…", can_edit, false, {}, L"Tab"});
                    items.push_back({network_control::add_sticky_note, L"Add Sticky Note", can_edit, false, {}, L"N"});
                    items.push_back({network_control::add_network_box, L"Add Network Box", can_edit, false, {}, L"O"});
                    items.push_back({network_control::frame_all, L"Frame All", has_net, false, {}, L"H", true});
                    ATELIER_TRY_BIND(layout_key, net_op("graph.layout"));
                    items.push_back({*layout_key, L"Auto Layout", can_edit, false, {}, L"L"});
                    json::Builder seed_args;
                    seed_args.set_validated(seed_args.root(), "node", "");
                    ATELIER_TRY_BIND(select_key, net_op("graph.select", seed_args.finish()));
                    items.push_back({*select_key, L"Inspect Graph Seed", has_net, false, {}, L"", true});
                    items.push_back(
                        {network_control::isolate_toggle,
                         active_graph.empty() ? L"Isolate SOP in 3D Viewport" : L"Return 3D Viewport to World",
                         has_net || !active_graph.empty(),
                         !active_graph.empty(),
                         {}});
                }
            } else if (const auto* target_ptr = binding(source->context_target);
                       source->kind == ControlKind::combo && target_ptr &&
                       (target_ptr->kind == Binding::Kind::field ||
                        target_ptr->kind == Binding::Kind::graph_param_field)) {
                const auto append_enum_choices = [&](const EnumValue& enumeration, bool available,
                                                     auto&& build_call) -> atelier::Result<void> {
                    ATELIER_TRY_BIND(enum_type, runtime.describe(enumeration.type));
                    const auto& enum_info = *(*enum_type)->enumeration;
                    for (std::size_t i = 0; i < enum_info.values.size(); ++i) {
                        const auto& member = enum_info.values[i];
                        const std::wstring title = i < enum_info.metadata.size() && !enum_info.metadata[i].label.empty()
                                                       ? platform::utf8_to_utf16_validated(enum_info.metadata[i].label)
                                                       : parameter_label(member.first);
                        bool checked = false;
                        std::optional<std::string_view> exact_name;
                        std::uint32_t next_bits = member.second;
                        if (enum_info.flags) {
                            checked = member.second == 0 ? enumeration.bits == 0
                                                         : (enumeration.bits & member.second) == member.second;
                            next_bits = member.second == 0 ? 0u
                                        : checked          ? (enumeration.bits & ~member.second)
                                                           : (enumeration.bits | member.second);
                            const auto exact = std::ranges::find(enum_info.values, next_bits,
                                                                 &std::pair<std::string, std::uint32_t>::second);
                            if (exact != enum_info.values.end())
                                exact_name = exact->first;
                        } else {
                            checked = member.second == enumeration.bits;
                            exact_name = member.first;
                        }
                        ATELIER_TRY_BIND(choice_key, bind_operator(build_call(exact_name, next_bits)));
                        ui::MenuItem item{*choice_key, title, available, checked, {}};
                        item.behavior = enum_info.flags ? ui::MenuBehavior::checkbox : ui::MenuBehavior::radio;
                        item.radio_group = enum_info.flags ? ui::ControlKey{} : source->key;
                        items.push_back(std::move(item));
                    }
                    return {};
                };
                if (target_ptr->kind == Binding::Kind::field) {
                    const ObjectId target_object = target_ptr->object;
                    const std::vector<FieldId> target_path = target_ptr->path;
                    auto value = session.read_property(target_object, target_path);
                    if (value)
                        if (const auto* enumeration = std::get_if<EnumValue>(&*value)) {
                            const SelectionContext enum_selection =
                                session.is_graph_object(target_object)
                                    ? SelectionContext{ObjectId{}, target_object, {}}
                                    : SelectionContext{selected, target_object, {selected}};
                            OperatorCall base_call{"scene.set_properties",
                                                   json::Document{},
                                                   {.selection = enum_selection, .region = RegionKind::parameters}};
                            ATELIER_TRY_BIND(type, runtime.type_of(target_object));
                            json::Builder args;
                            auto argument = args.root();
                            std::string_view name;
                            for (std::size_t i = 0; i < target_path.size(); ++i) {
                                ATELIER_TRY_BIND(info, runtime.describe(*type));
                                ATELIER_TRY_BIND(field, property_field(**info, target_path[i]));
                                const auto at = *field;
                                if (i + 1 < target_path.size())
                                    argument = args.object_validated(argument, at->name);
                                else
                                    name = at->name;
                                *type = at->value.type;
                            }
                            auto ctx = operator_context(session, project, true, command_focus);
                            ctx.selection = *base_call.context.selection;
                            ctx.region = *base_call.context.region;
                            const bool available = operators.find(base_call.id)->poll(ctx).has_value();
                            ATELIER_TRY(append_enum_choices(
                                *enumeration, available,
                                [&](std::optional<std::string_view> exact_name, std::uint32_t next_bits) {
                                    if (exact_name)
                                        args.set_validated(argument, name, *exact_name);
                                    else if (auto added =
                                                 args.set(argument, name, static_cast<std::uint64_t>(next_bits));
                                             !added)
                                        fatal(added.error().describe());
                                    auto call = base_call;
                                    call.args = args.finish();
                                    return call;
                                }));
                        }
                } else {
                    const ObjectId target_comp = target_ptr->component;
                    const std::string target_slot = target_ptr->slot;
                    const std::string target_node = target_ptr->node_name;
                    const std::string target_param = target_ptr->material_path;
                    const bool is_gen_comp = target_comp != ObjectId{};
                    const auto params = is_gen_comp ? session.generator_promoted_parameters(target_comp, project)
                                        : target_node.empty()
                                            ? session.graph_promoted_parameters(target_slot)
                                            : session.graph_asset_node_parameters(target_slot, target_node, project);
                    if (params) {
                        const auto pit = std::ranges::find(*params, target_param, &GraphPromotedParameterView::name);
                        if (pit != params->end() && pit->valid && !pit->properties.empty()) {
                            if (const auto* enumeration = std::get_if<EnumValue>(&pit->properties.front().value)) {
                                bool available = !session.world().running();
                                SelectionContext gen_sel{};
                                if (is_gen_comp) {
                                    const auto comp_st = world::inspect_component(runtime, target_comp);
                                    const ObjectId owner_ent = comp_st ? comp_st->owner : ObjectId{};
                                    const auto ent_st =
                                        owner_ent != ObjectId{}
                                            ? world::inspect_entity(runtime, owner_ent)
                                            : atelier::Result<world::EntityStatus>{world::EntityStatus{}};
                                    available = available && comp_st && comp_st->valid && owner_ent != ObjectId{} &&
                                                ent_st && ent_st->can_edit;
                                    gen_sel = {owner_ent, target_comp,
                                               owner_ent == ObjectId{} ? std::vector<ObjectId>{}
                                                                       : std::vector<ObjectId>{owner_ent}};
                                } else {
                                    available = available && !is_network_graph_locked(session, target_slot);
                                }
                                ATELIER_TRY(append_enum_choices(
                                    *enumeration, available,
                                    [&](std::optional<std::string_view> exact_name, std::uint32_t next_bits) {
                                        std::string val_json;
                                        if (exact_name) {
                                            json::Builder jb;
                                            jb.set_validated(jb.root(), *exact_name);
                                            const auto doc_val = jb.finish();
                                            const auto bytes = json::write(doc_val.root());
                                            val_json.assign(bytes.data(), bytes.size());
                                        } else {
                                            val_json = std::to_string(next_bits);
                                        }
                                        json::Builder args;
                                        static_cast<void>(args.set(args.root(), "parameter", target_param));
                                        static_cast<void>(args.set(args.root(), "value_json", val_json));
                                        if (is_gen_comp) {
                                            static_cast<void>(args.set(args.root(), "clear", false));
                                            return OperatorCall{
                                                "procedural.set_override",
                                                args.finish(),
                                                {.selection = gen_sel, .region = RegionKind::parameters}};
                                        }
                                        if (!target_node.empty()) {
                                            static_cast<void>(args.set(args.root(), "asset", target_slot));
                                            static_cast<void>(args.set(args.root(), "node", target_node));
                                            static_cast<void>(args.set(args.root(), "clear", false));
                                            return OperatorCall{"graph.set_override", args.finish()};
                                        }
                                        static_cast<void>(args.set(args.root(), "asset", target_slot));
                                        return OperatorCall{"graph.set_parameter", args.finish()};
                                    }));
                            }
                        }
                    }
                }
            } else if (const auto* target_ptr = binding(source->context_target);
                       target_ptr && (target_ptr->kind == Binding::Kind::procedural_graph_slot ||
                                      (source->kind == ControlKind::combo &&
                                       target_ptr->kind == Binding::Kind::graph_node_asset_slot))) {
                const bool is_node_slot = target_ptr->kind == Binding::Kind::graph_node_asset_slot;
                const ObjectId target_object = target_ptr->object;
                const std::string target_slot = target_ptr->slot;
                const std::string target_node = target_ptr->node_name;
                const auto collect_procgraph_choices = [&](std::string_view exclude_key) {
                    std::vector<std::pair<std::string, std::wstring>> choices;
                    if (project) {
                        for (const auto& entry : project->entries())
                            if (entry.kind == assets::AssetKind::procedural_graph && entry.key != exclude_key)
                                choices.emplace_back(entry.key, entry.path.stem().wstring());
                    }
                    for (const auto& open_key : session.open_graphs()) {
                        if (open_key == exclude_key || session.is_scratch_graph(open_key))
                            continue;
                        if (std::ranges::find(choices, open_key, &std::pair<std::string, std::wstring>::first) ==
                            choices.end())
                            choices.emplace_back(open_key, platform::utf8_to_utf16_validated(open_key));
                    }
                    return choices;
                };
                if (!is_node_slot) {
                    if (const auto proc = presentation::read_procedural_source(runtime, target_object)) {
                        const bool can_edit = !session.world().running();
                        const auto comp_status = world::inspect_component(runtime, target_object);
                        const ObjectId owner_entity = comp_status ? comp_status->owner : selected;
                        const SelectionContext gen_sel{
                            owner_entity, target_object,
                            owner_entity == ObjectId{} ? std::vector<ObjectId>{} : std::vector<ObjectId>{owner_entity}};
                        {
                            json::Builder clear_args;
                            clear_args.set_validated(clear_args.root(), "asset", "");
                            ATELIER_TRY_BIND(clear_key,
                                             bind_operator({"procedural.assign_graph",
                                                            clear_args.finish(),
                                                            {.selection = gen_sel, .region = RegionKind::parameters}}));
                            ui::MenuItem none_item{
                                *clear_key, L"None (Clear Graph)", can_edit, proc->source.empty(), {}};
                            none_item.behavior = ui::MenuBehavior::radio;
                            none_item.radio_group = source->key;
                            items.push_back(std::move(none_item));
                        }
                        if (!proc->source.empty()) {
                            json::Builder open_args;
                            static_cast<void>(open_args.set(open_args.root(), "asset", proc->source));
                            ATELIER_TRY_BIND(open_key, bind_operator({"graph.open", open_args.finish()}));
                            items.push_back({*open_key, L"Edit Graph in Network", can_edit, false, {}});
                        }
                        bool has_instances = false;
                        if (proc->output)
                            for (const auto& col : proc->output->publication.collections)
                                if (col.transforms && !col.transforms->empty())
                                    has_instances = true;
                        ATELIER_TRY_BIND(bake_key,
                                         bind_operator({"procedural.bake",
                                                        json::Document{},
                                                        {.selection = gen_sel, .region = RegionKind::parameters}}));
                        items.push_back(
                            {*bake_key, L"Bake Instances to Entities", can_edit && has_instances, false, {}});
                        const auto choices = collect_procgraph_choices({});
                        for (std::size_t i = 0; i < choices.size(); ++i) {
                            json::Builder assign_args;
                            static_cast<void>(assign_args.set(assign_args.root(), "asset", choices[i].first));
                            ATELIER_TRY_BIND(assign_key,
                                             bind_operator({"procedural.assign_graph",
                                                            assign_args.finish(),
                                                            {.selection = gen_sel, .region = RegionKind::parameters}}));
                            ui::MenuItem item{
                                *assign_key, choices[i].second, can_edit, proc->source == choices[i].first, {}, L"",
                                i == 0};
                            item.behavior = ui::MenuBehavior::radio;
                            item.radio_group = source->key;
                            items.push_back(std::move(item));
                        }
                    }
                } else {
                    const bool can_edit = !session.world().running() && !is_network_graph_locked(session, target_slot);
                    const auto* gnode = session.graph_node(target_slot, target_node);
                    const std::string current_asset = gnode ? gnode->asset_key : std::string{};
                    {
                        json::Builder clear_args;
                        static_cast<void>(clear_args.set(clear_args.root(), "asset", target_slot));
                        static_cast<void>(clear_args.set(clear_args.root(), "node", target_node));
                        static_cast<void>(clear_args.set(clear_args.root(), "target_asset", ""));
                        ATELIER_TRY_BIND(clear_key, bind_operator({"graph.set_node_asset", clear_args.finish()}));
                        ui::MenuItem none_item{
                            *clear_key, L"None (Clear Digital Asset)", can_edit, current_asset.empty(), {}};
                        none_item.behavior = ui::MenuBehavior::radio;
                        none_item.radio_group = source->key;
                        items.push_back(std::move(none_item));
                    }
                    if (!current_asset.empty()) {
                        json::Builder dive_args;
                        static_cast<void>(dive_args.set(dive_args.root(), "asset", target_slot));
                        static_cast<void>(dive_args.set(dive_args.root(), "node", target_node));
                        ATELIER_TRY_BIND(dive_key, bind_operator({"graph.dive_in", dive_args.finish()}));
                        items.push_back(
                            {*dive_key, L"Dive Into Digital Asset", !session.world().running(), false, {}, L"Enter"});
                    }
                    const auto choices = collect_procgraph_choices(target_slot);
                    for (std::size_t i = 0; i < choices.size(); ++i) {
                        json::Builder assign_args;
                        static_cast<void>(assign_args.set(assign_args.root(), "asset", target_slot));
                        static_cast<void>(assign_args.set(assign_args.root(), "node", target_node));
                        static_cast<void>(assign_args.set(assign_args.root(), "target_asset", choices[i].first));
                        ATELIER_TRY_BIND(assign_key, bind_operator({"graph.set_node_asset", assign_args.finish()}));
                        ui::MenuItem item{
                            *assign_key, choices[i].second, can_edit, current_asset == choices[i].first, {}, L"",
                            i == 0};
                        item.behavior = ui::MenuBehavior::radio;
                        item.radio_group = source->key;
                        items.push_back(std::move(item));
                    }
                }
            } else if (const auto* target = binding(source->context_target);
                       target && target->kind == Binding::Kind::procedural_graph_slot) {
                ATELIER_TRY_BIND(proc, presentation::read_procedural_source(runtime, target->object));
                const bool can_edit = !session.world().running();
                ATELIER_TRY_BIND(comp_status, world::inspect_component(runtime, target->object));
                const ObjectId owner_entity = comp_status->owner;
                const SelectionContext gen_sel{owner_entity, target->object,
                                               owner_entity == ObjectId{} ? std::vector<ObjectId>{}
                                                                          : std::vector<ObjectId>{owner_entity}};
                const auto assign_graph = [&](std::string_view asset) {
                    json::Builder args;
                    args.set_validated(args.root(), "asset", asset);
                    return bind_operator({"procedural.assign_graph",
                                          args.finish(),
                                          {.selection = gen_sel, .region = RegionKind::parameters}});
                };
                {
                    ATELIER_TRY_BIND(assign_graph_key, assign_graph(""));
                    ui::MenuItem none_item{
                        *assign_graph_key, L"None (Clear Graph)", can_edit, proc->source.empty(), {}};
                    none_item.behavior = ui::MenuBehavior::radio;
                    none_item.radio_group = source->key;
                    items.push_back(std::move(none_item));
                }
                if (!proc->source.empty()) {
                    json::Builder open_args;
                    static_cast<void>(open_args.set(open_args.root(), "asset", proc->source));
                    ATELIER_TRY_BIND(open_key, bind_operator({"graph.open", open_args.finish()}));
                    items.push_back({*open_key, L"Edit Graph in Network", can_edit, false, {}});
                }
                bool has_instances = false;
                if (proc->output)
                    for (const auto& col : proc->output->publication.collections)
                        if (col.transforms && !col.transforms->empty())
                            has_instances = true;
                ATELIER_TRY_BIND(bake_key, bind_operator({"procedural.bake",
                                                          json::Document{},
                                                          {.selection = gen_sel, .region = RegionKind::parameters}}));
                items.push_back({*bake_key, L"Bake Instances to Entities", can_edit && has_instances, false, {}});
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
                    ATELIER_TRY_BIND(assign_graph_key, assign_graph(choices[i].first));
                    ui::MenuItem item{*assign_graph_key,
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
            } else if (const auto* target = binding(source->context_target);
                       target && (target->kind == Binding::Kind::material_slot ||
                                  target->kind == Binding::Kind::material_texture ||
                                  target->kind == Binding::Kind::material_override ||
                                  target->kind == Binding::Kind::material_field ||
                                  (target->kind == Binding::Kind::property_row && target->slot.empty() &&
                                   target->node_name.empty() && !target->material_path.empty()))) {
                ATELIER_TRY_ASSIGN(items, material_menu(*target, session, project, material_choices));
            } else if (auto asset_items = assets.menu_items(source->key); !asset_items.empty()) {
                items = std::move(asset_items);
                if (source->key == browser_control::actions || source->key == browser_control::results)
                    for (const auto& definition : menu_bindings)
                        if (definition.call.id.starts_with("asset."))
                            ATELIER_TRY(append_operator(items, definition.call, &definition));
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
            } else if (source->key == source_control::outline) {
                items = source_editor.outline_menu_items();
            } else if (source->key == control::add_component) {
                ATELIER_TRY(append_operator(items, {transform ? "scene.transform_remove" : "scene.transform_add"}));
                ATELIER_TRY_ASSIGN(component_choices, session.component_types());
                for (std::size_t i = 0; i < component_choices.size(); ++i)
                    items.push_back({control::component_choice_first + i,
                                     platform::utf8_to_utf16_validated(component_choices[i].name),
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
                ATELIER_TRY(append_operator(items, {"view.toggle", json::Document::literal<R"({"option":"fps"})">()}));
                const bool in_sop = !active_graph.empty() && session.has_graph(active_graph);
                const ObjectId gen_comp =
                    in_sop ? ObjectId{} : active_procedural_generator(session).value_or(ObjectId{});
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
                    if (menu_geom) {
                        std::vector<ui::MenuItem> groups;
                        const auto add_group = [&](procedural::Layer layer, std::string_view name,
                                                   std::wstring title) -> atelier::Result<void> {
                            ATELIER_TRY_BIND(key, bind_row(
                                                      binding_cursor_,
                                                      [&](const Binding& row) {
                                                          return row.kind == Binding::Kind::group_guide &&
                                                                 row.slot == name &&
                                                                 row.index == static_cast<std::size_t>(layer);
                                                      },
                                                      [&](ui::ControlKey id) {
                                                          Binding row{id, {}, {}, Binding::Kind::group_guide};
                                                          row.slot = name;
                                                          row.index = static_cast<std::size_t>(layer);
                                                          return row;
                                                      }));
                            const bool selected = name.empty() ? !sop_group.resolve(*menu_geom)
                                                               : name == sop_group.name() && layer == sop_group.layer();
                            ui::MenuItem item{*key, std::move(title), true, selected, {}};
                            item.behavior = ui::MenuBehavior::radio;
                            item.radio_group = control::display_sop_group;
                            groups.push_back(std::move(item));
                            return {};
                        };
                        ATELIER_TRY(add_group(procedural::Layer::primitive, {}, L"None"));
                        for (const auto& attr : menu_geom->attributes()) {
                            if ((attr.layer == procedural::Layer::point ||
                                 attr.layer == procedural::Layer::primitive) &&
                                std::holds_alternative<SharedArray<bool>>(attr.data))
                                ATELIER_TRY(add_group(
                                    attr.layer, attr.name.view().std_view(),
                                    platform::utf8_to_utf16_validated(attr.name.view().std_view()) +
                                        (attr.layer == procedural::Layer::point ? L"  (points)" : L"  (primitives)")));
                        }
                        if (groups.size() > 1 || !sop_group.name().empty())
                            items.push_back(submenu(control::display_sop_group, L"Highlight Group", std::move(groups)));
                    }
                }
                for (auto& item : items)
                    if (item.children.empty())
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
                    if (!source->read_only)
                        items.push_back({control::menu_paste, L"Paste Value", true, false,
                                         ui::EditCommand{ui::EditCommandKind::paste, source->key}});
                }
                const auto* ctx_ptr = source->context_group ? binding(source->context_group) : nullptr;
                if (!ctx_ptr || ctx_ptr->kind != Binding::Kind::graph_param_context) {
                    if (const auto* direct = binding(source->context_target)) {
                        if (direct->kind == Binding::Kind::graph_param_context) {
                            ctx_ptr = direct;
                        } else if (direct->kind == Binding::Kind::text_field &&
                                   session.is_graph_object(direct->object)) {
                            for (const auto& b : bindings_)
                                if (b.kind == Binding::Kind::graph_param_context && b.object == direct->object &&
                                    std::ranges::equal(b.path, direct->path)) {
                                    ctx_ptr = &b;
                                    break;
                                }
                        } else if (direct->kind == Binding::Kind::graph_param_text) {
                            for (const auto& b : bindings_)
                                if (b.kind == Binding::Kind::graph_param_context && b.object == direct->object &&
                                    b.component == direct->component && b.slot == direct->slot &&
                                    b.node_name == direct->node_name && b.material_path == direct->material_path) {
                                    ctx_ptr = &b;
                                    break;
                                }
                        }
                    }
                }
                if (ctx_ptr && ctx_ptr->kind == Binding::Kind::graph_param_context && !ctx_ptr->slot.empty()) {
                    const ObjectId ctx_object = ctx_ptr->object;
                    const ObjectId ctx_comp = ctx_ptr->component;
                    const std::string ctx_slot = ctx_ptr->slot;
                    const std::string ctx_node = ctx_ptr->node_name;
                    const std::string ctx_param = ctx_ptr->material_path;
                    const std::vector<FieldId> ctx_path = ctx_ptr->path;
                    const bool can_edit_param =
                        !session.world().running() && !is_network_graph_locked(session, ctx_slot);
                    bool need_sep = !items.empty();
                    if (ctx_comp == ObjectId{} && !ctx_node.empty() && !ctx_path.empty()) {
                        // Single-node SOP field promotion / multi-target binding / unbind:
                        std::vector<std::string> candidate_paths;
                        if (const auto* num_b = binding(source->context_target);
                            source->kind == ControlKind::number && num_b && num_b->kind == Binding::Kind::field &&
                            num_b->object == ctx_object && num_b->path.size() > ctx_path.size()) {
                            ATELIER_TRY_BIND(cur_t, runtime.type_of(num_b->object));
                            std::string full_p;
                            for (const auto fid : num_b->path) {
                                ATELIER_TRY_BIND(inf, runtime.describe(*cur_t));
                                const auto fit = std::ranges::find((*inf)->fields, fid, &FieldInfo::id);
                                if (fit == (*inf)->fields.end())
                                    break;
                                if (!full_p.empty())
                                    full_p.push_back('.');
                                full_p.append(fit->name);
                                *cur_t = fit->value.type;
                            }
                            if (!full_p.empty() && full_p != ctx_param)
                                candidate_paths.push_back(std::move(full_p));
                        } else if (!session.graph_controlling_parameter(ctx_slot, ctx_node, ctx_param) &&
                                   session.graph_overlapping_parameter(ctx_slot, ctx_node, ctx_param) &&
                                   ctx_object != ObjectId{}) {
                            ATELIER_TRY_BIND(cur_t, runtime.type_of(ctx_object));
                            bool valid_prefix = true;
                            for (const auto fid : ctx_path) {
                                ATELIER_TRY_BIND(inf, runtime.describe(*cur_t));
                                const auto fit = std::ranges::find((*inf)->fields, fid, &FieldInfo::id);
                                if (fit == (*inf)->fields.end()) {
                                    valid_prefix = false;
                                    break;
                                }
                                *cur_t = fit->value.type;
                            }
                            if (valid_prefix) {
                                ATELIER_TRY_BIND(vec_desc, runtime.describe(*cur_t));
                                if ((*vec_desc)->math.vector() || (*vec_desc)->math.shape == math::Shape::quaternion) {
                                    for (const auto& lf : (*vec_desc)->fields)
                                        candidate_paths.push_back(ctx_param + "." + lf.name);
                                }
                            }
                        }
                        candidate_paths.push_back(ctx_param);

                        const auto root_params = session.graph_promoted_parameters(ctx_slot);
                        const auto* gdoc = session.graph_document(ctx_slot);
                        const auto* gnode = session.graph_node(ctx_slot, ctx_node);
                        bool added_inspect_root = false;
                        for (const auto& fpath : candidate_paths) {
                            const auto wide_fp = platform::utf8_to_utf16_validated(fpath);
                            if (const auto ctrl = session.graph_controlling_parameter(ctx_slot, ctx_node, fpath)) {
                                json::Builder uargs;
                                static_cast<void>(uargs.set(uargs.root(), "asset", ctx_slot));
                                static_cast<void>(uargs.set(uargs.root(), "node", ctx_node));
                                static_cast<void>(uargs.set(uargs.root(), "path", fpath));
                                ATELIER_TRY_BIND(unbind_key, bind_operator({"graph.unbind_parameter", uargs.finish()}));
                                items.push_back({*unbind_key,
                                                 L"Unbind '" + wide_fp + L"' from '" +
                                                     platform::utf8_to_utf16_validated(*ctrl) + L"'",
                                                 can_edit_param,
                                                 false,
                                                 {},
                                                 L"",
                                                 need_sep});
                                need_sep = false;
                                if (!added_inspect_root) {
                                    added_inspect_root = true;
                                    json::Builder sargs;
                                    static_cast<void>(sargs.set(sargs.root(), "asset", ctx_slot));
                                    static_cast<void>(sargs.set(sargs.root(), "node", ""));
                                    ATELIER_TRY_BIND(inspect_key, bind_operator({"graph.select", sargs.finish()}));
                                    items.push_back({*inspect_key, L"Inspect Graph Root Parameters", true, false, {}});
                                }
                            } else if (gnode && gdoc &&
                                       !session.graph_overlapping_parameter(ctx_slot, ctx_node, fpath)) {
                                const std::string target_full = ctx_node + "." + fpath;
                                const auto resolved = resolve_graph_parameter_target(runtime, *gdoc, target_full);
                                if (!resolved)
                                    continue;
                                json::Builder eargs;
                                static_cast<void>(eargs.set(eargs.root(), "asset", ctx_slot));
                                static_cast<void>(eargs.set(eargs.root(), "node", ctx_node));
                                static_cast<void>(eargs.set(eargs.root(), "path", fpath));
                                ATELIER_TRY_BIND(expose_key, bind_operator({"graph.expose_parameter", eargs.finish()}));
                                items.push_back({*expose_key,
                                                 candidate_paths.size() > 1
                                                     ? (L"Expose '" + wide_fp + L"' as Graph Parameter")
                                                     : L"Expose as Graph Parameter",
                                                 can_edit_param,
                                                 false,
                                                 {},
                                                 L"",
                                                 need_sep});
                                need_sep = false;
                                if (root_params && !root_params->empty()) {
                                    std::vector<ui::MenuItem> bind_children;
                                    for (const auto& rp : *root_params) {
                                        if (!rp.valid || rp.value_type != resolved->value_type)
                                            continue;
                                        json::Builder bargs;
                                        static_cast<void>(bargs.set(bargs.root(), "asset", ctx_slot));
                                        static_cast<void>(bargs.set(bargs.root(), "parameter", rp.name));
                                        static_cast<void>(bargs.set(bargs.root(), "node", ctx_node));
                                        static_cast<void>(bargs.set(bargs.root(), "path", fpath));
                                        ATELIER_TRY_BIND(bind_key,
                                                         bind_operator({"graph.bind_parameter", bargs.finish()}));
                                        bind_children.push_back({*bind_key,
                                                                 platform::utf8_to_utf16_validated(rp.name),
                                                                 can_edit_param,
                                                                 false,
                                                                 {}});
                                    }
                                    if (!bind_children.empty()) {
                                        ui::ControlKey sub_key{};
                                        ATELIER_TRY_ASSIGN(sub_key, bind_graph_param(ctx_slot, ctx_node, fpath,
                                                                                     Binding::Kind::graph_param_submenu,
                                                                                     0, ctx_object));
                                        items.push_back(submenu(sub_key,
                                                                candidate_paths.size() > 1
                                                                    ? (L"Bind '" + wide_fp + L"' to Parameter")
                                                                    : L"Bind to Graph Parameter",
                                                                std::move(bind_children), false));
                                    }
                                }
                            }
                        }
                    } else if (ctx_comp == ObjectId{} && ctx_node.empty()) {
                        // Graph Root promoted parameter management:
                        const auto* gdoc = session.graph_document(ctx_slot);
                        std::size_t idx = 0;
                        std::size_t total = 0;
                        if (gdoc) {
                            total = gdoc->parameters.size();
                            const auto it =
                                std::ranges::find(gdoc->parameters, ctx_param, &GraphPromotedParameter::name);
                            if (it != gdoc->parameters.end())
                                idx = static_cast<std::size_t>(it - gdoc->parameters.begin());
                        }
                        {
                            json::Builder up_args;
                            static_cast<void>(up_args.set(up_args.root(), "asset", ctx_slot));
                            static_cast<void>(up_args.set(up_args.root(), "parameter", ctx_param));
                            static_cast<void>(up_args.set(up_args.root(), "move_delta", std::int64_t{-1}));
                            ATELIER_TRY_BIND(move_up_key, bind_operator({"graph.update_parameter", up_args.finish()}));
                            items.push_back({*move_up_key,
                                             L"Move Parameter Up",
                                             can_edit_param && idx > 0,
                                             false,
                                             {},
                                             L"",
                                             need_sep});
                            need_sep = false;
                        }
                        {
                            json::Builder dn_args;
                            static_cast<void>(dn_args.set(dn_args.root(), "asset", ctx_slot));
                            static_cast<void>(dn_args.set(dn_args.root(), "parameter", ctx_param));
                            static_cast<void>(dn_args.set(dn_args.root(), "move_delta", std::int64_t{1}));
                            ATELIER_TRY_BIND(move_down_key,
                                             bind_operator({"graph.update_parameter", dn_args.finish()}));
                            items.push_back(
                                {*move_down_key, L"Move Parameter Down", can_edit_param && idx + 1 < total, false, {}});
                        }
                        {
                            json::Builder rm_args;
                            static_cast<void>(rm_args.set(rm_args.root(), "asset", ctx_slot));
                            static_cast<void>(rm_args.set(rm_args.root(), "parameter", ctx_param));
                            ATELIER_TRY_BIND(remove_key, bind_operator({"graph.remove_parameter", rm_args.finish()}));
                            items.push_back(
                                {*remove_key, L"Remove Promoted Parameter", can_edit_param, false, {}, L"", true});
                        }
                    } else if (ctx_comp == ObjectId{} && !ctx_node.empty() && ctx_path.empty()) {
                        // AssetNode override row:
                        bool is_overridden = false;
                        if (const auto params = session.graph_asset_node_parameters(ctx_slot, ctx_node, project)) {
                            const auto it = std::ranges::find(*params, ctx_param, &GraphPromotedParameterView::name);
                            if (it != params->end())
                                is_overridden = it->overridden;
                        }
                        json::Builder clr_args;
                        static_cast<void>(clr_args.set(clr_args.root(), "asset", ctx_slot));
                        static_cast<void>(clr_args.set(clr_args.root(), "node", ctx_node));
                        static_cast<void>(clr_args.set(clr_args.root(), "parameter", ctx_param));
                        static_cast<void>(clr_args.set(clr_args.root(), "clear", true));
                        ATELIER_TRY_BIND(clear_key, bind_operator({"graph.set_override", clr_args.finish()}));
                        items.push_back({*clear_key,
                                         L"Revert Override to Asset Default",
                                         can_edit_param && is_overridden,
                                         false,
                                         {},
                                         L"",
                                         need_sep});
                    } else if (ctx_comp != ObjectId{}) {
                        // Scene ProceduralGeneratorComponent override row:
                        bool is_overridden = false;
                        if (const auto params = session.generator_promoted_parameters(ctx_comp, project)) {
                            const auto it = std::ranges::find(*params, ctx_param, &GraphPromotedParameterView::name);
                            if (it != params->end())
                                is_overridden = it->overridden;
                        }
                        const auto comp_st = world::inspect_component(runtime, ctx_comp);
                        const ObjectId owner_ent = comp_st ? comp_st->owner : ObjectId{};
                        const auto ent_st = owner_ent != ObjectId{}
                                                ? world::inspect_entity(runtime, owner_ent)
                                                : atelier::Result<world::EntityStatus>{world::EntityStatus{}};
                        const bool can_edit_gen = !session.world().running() && comp_st && comp_st->valid &&
                                                  owner_ent != ObjectId{} && ent_st && ent_st->can_edit;
                        const SelectionContext gen_sel{owner_ent, ctx_comp,
                                                       owner_ent == ObjectId{} ? std::vector<ObjectId>{}
                                                                               : std::vector<ObjectId>{owner_ent}};
                        json::Builder clr_args;
                        static_cast<void>(clr_args.set(clr_args.root(), "parameter", ctx_param));
                        static_cast<void>(clr_args.set(clr_args.root(), "clear", true));
                        ATELIER_TRY_BIND(clear_key,
                                         bind_operator({"procedural.set_override",
                                                        clr_args.finish(),
                                                        {.selection = gen_sel, .region = RegionKind::parameters}}));
                        items.push_back({*clear_key,
                                         L"Revert Override to Asset Default",
                                         can_edit_gen && is_overridden,
                                         false,
                                         {},
                                         L"",
                                         need_sep});
                    }
                }
            }
            if (items.empty())
                dismiss_menu = true;
            else
                next_menu_items = std::move(items);
        }
    }
    if (pie_menu() && !next_menu_items.empty()) {
        const auto dpi = frame ? frame->drawing->dpi_scale : 1.f;
        auto entries = std::make_shared<std::vector<ui_language::Item>>();
        constexpr std::array<std::pair<double, double>, 8> positions{
            {{60, 104}, {300, 104}, {180, 181}, {180, 27}, {72, 55}, {288, 55}, {72, 153}, {288, 153}}};
        for (std::size_t i = 0; i < next_menu_items.size(); ++i) {
            const auto& item = next_menu_items[i];
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
    if (color_edit && !pie_menu()) {
        // One popup per session; the colour editor is projected last, after every owner.
        const auto& edit = *color_edit;
        const auto hsv = rgb_to_hsv(edit.value);
        const double hue = std::clamp(edit.hue, 0.0, 360.0);
        const double sat = std::clamp(edit.saturation, 0.0, 1.0);
        const double val = std::clamp(hsv[2], 0.0, edit.maximum);
        const double hdr_ceiling = std::max(1.0, edit.hdr_scale);
        const double val_norm = std::clamp(val / hdr_ceiling, 0.0, 1.0);
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
            {field(control::color_hsv, L"H", hue, 360, 1, ui::VectorAxis::none),
             field(control::color_hsv + 1, L"S", sat, 1, .005, ui::VectorAxis::none),
             field(control::color_hsv + 2, L"V", val, edit.maximum, .005, ui::VectorAxis::none)}};
        const double slider_max = std::min(edit.maximum, std::max(1.0, hdr_ceiling * (edit.maximum > 1.0 ? 2.0 : 1.0)));
        ui::NumberField intensity_field{control::color_intensity, L"", val, val, 0.0, edit.maximum, .01, .001};
        if (slider_max > 0.0)
            intensity_field.slider = ui::SliderRange{control::color_intensity_slider, 0.0, slider_max};
        ui_language::EditBinding intensity_edit{control::color_intensity_group, true, false, {intensity_field}};
        const auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
        const std::array<ui_language::InputValue, 17> inputs{
            double(std::clamp(edit.x, 0.f, std::max(0.f, size.width - 416.f))),
            double(std::clamp(edit.y, 0.f, std::max(0.f, size.height - 320.f))),
            clamp01(edit.value[0]),
            clamp01(edit.value[1]),
            clamp01(edit.value[2]),
            clamp01(edit.original[0]),
            clamp01(edit.original[1]),
            clamp01(edit.original[2]),
            hue,
            std::clamp(hsv[1], 0.0, 1.0),
            val_norm,
            std::move(rgb),
            std::move(hsv_edit),
            std::move(intensity_edit),
            ui_language::TextEditBinding{control::color_hex, edit.hex},
            ui_language::ActionBinding{control::color_revert, true},
            ui_language::ActionBinding{control::color_done, true}};
        const std::array<ui_language::KeyBinding, 3> keys{{{"editor", control::color_editor},
                                                           {"sv_plane", control::color_sv_plane},
                                                           {"hue_bar", control::color_hue_bar}}};
        color_edit->composition_failed = !ui_document_->append("ColorEditor", 0, keys, inputs, result);
    }
    if (ui_document_->candidate_failed()) {
        retry = true;
        return std::unexpected(atelier::Error{atelier::ErrorCode::invalid_definition, std::string(ui_error())});
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
            set_help(ctrl, L"Add Component", L"Scene Outliner", "向当前选中的实体挂载空间变换或业务功能组件。");
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
            set_help(
                ctrl, L"Pin to Display Node",
                spreadsheet.follow_selected() ? L"Currently: Follow Selection" : L"Currently: Pinned to Display Node",
                "锁定电子表格目标：默认未锁定时实时跟随画布中选中的节点；点击钉住后始终锁定检视 Display Flag 节点输出。");
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
    if (dismiss_menu)
        menu.reset();
    menu_items = std::move(next_menu_items);
    component_choices_ = std::move(component_choices);
    material_choices_ = std::move(material_choices);
    network_path_choices_ = std::move(network_path_choices);
    menu_selection_ = session.selection();
    pending_live_.emplace(std::move(live));
    return {};
}

std::wstring Workspace::single_line_field_text(std::wstring_view raw, bool* sanitized_out) {
    std::wstring out;
    out.reserve(std::min<std::size_t>(raw.size(), 4096));
    bool changed = false;
    for (std::size_t i = 0; i < raw.size() && out.size() < 4096; ++i) {
        const wchar_t c = raw[i];
        if (c == L'\r' && i + 1 < raw.size() && raw[i + 1] == L'\n') {
            out.push_back(L' ');
            changed = true;
            ++i;
        } else if (c < 32 || c == 127 || c == 0x2028 || c == 0x2029) {
            out.push_back(L' ');
            changed = true;
        } else if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 < raw.size() && raw[i + 1] >= 0xdc00 && raw[i + 1] <= 0xdfff && out.size() + 2 <= 4096) {
                out.push_back(c);
                out.push_back(raw[++i]);
            } else {
                changed = true;
            }
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            changed = true;
        } else {
            out.push_back(c);
        }
    }
    if (raw.size() > 4096)
        changed = true;
    if (sanitized_out)
        *sanitized_out = changed;
    return out;
}

std::array<double, 3> rgb_to_hsv(std::array<double, 3> c) noexcept {
    c[0] = std::max(0.0, c[0]);
    c[1] = std::max(0.0, c[1]);
    c[2] = std::max(0.0, c[2]);
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
