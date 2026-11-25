#include "editor/operators.hpp"
#include "assets/assets.hpp"
#include "assets/builtin_materials.hpp"
#include "assets/file.hpp"
#include "assets/material_format.hpp"
#include "editor/project.hpp"
#include "editor/view_options.hpp"
#include "editor/viewport_tool.hpp"
#include "platform/utf.hpp"
#include <limits>
#include <unordered_set>

namespace atelier::editor {
namespace {
std::unexpected<Error> unavailable(std::string reason) {
    return std::unexpected(Error{ErrorCode::unsupported_operation, std::move(reason)});
}

Result<void> available(const Context&) {
    return {};
}

// Adapts an operator over the reflected parameter record P to the type-erased entry.
template <class P>
auto with_parameters(Result<void> (*execute)(OperatorHost&, const Context&, const P&)) {
    return [execute](OperatorHost& host, const Context& context, const OperatorParameters& parameters,
                     ValueOutput) -> Result<void> {
        const auto input = parameters.value;
        const auto type = context.document.world().runtime().native_type<P>();
        if (!type || *type != input.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Operator parameters have another type."});
        return execute(host, context, *static_cast<const P*>(input.data));
    };
}

template <class P, class R>
auto with_parameters(Result<void> (*execute)(OperatorHost&, const Context&, const P&, R&)) {
    return [execute](OperatorHost& host, const Context& context, const OperatorParameters& parameters,
                     ValueOutput output) -> Result<void> {
        auto& runtime = context.document.world().runtime();
        const auto parameter_type = runtime.native_type<P>();
        const auto result_type = runtime.native_type<R>();
        if (!parameter_type || *parameter_type != parameters.value.type || !result_type || *result_type != output.type)
            return std::unexpected(Error{ErrorCode::type_mismatch, "Operator parameters or result have another type."});
        return execute(host, context, *static_cast<const P*>(parameters.value.data), *static_cast<R*>(output.data));
    };
}

template <class P>
OperatorInfo interactive_when(OperatorInfo info, bool (*condition)(const P&), std::string description) {
    info.requires_interaction = [condition](ValueInput value) { return condition(*static_cast<const P*>(value.data)); };
    info.interaction = std::move(description);
    return info;
}

template <class P>
OperatorInfo path_operator(OperatorInfo info) {
    return interactive_when<P>(
        std::move(info), +[](const P& p) { return p.path.empty(); },
        "An empty path opens a picker; provide path to execute directly.");
}

Result<void> committed(Result<world::Commit> result) {
    if (!result)
        return std::unexpected(result.error());
    if (result->notification_error)
        return std::unexpected(*result->notification_error);
    return {};
}

// Blender View Selected / View All: bounds of the rendered geometry; locators count as points.
Result<void> frame(OperatorHost& host, const Context& context, bool selected_only) {
    if (!context.active_graph.empty()) {
        ATELIER_TRY_BIND(eval, context.document.evaluate_graph(context.active_graph, context.project));
        const auto* shown = eval->preview_output.get();
        const auto summary = summarize_geometry(shown, 0);
        if (!summary.has_bounds)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "The procedural graph has no geometry bounds to frame."});
        auto camera = host.camera();
        camera.frame((summary.bounds_min + summary.bounds_max) * .5f,
                     std::max(math::length(summary.bounds_max - summary.bounds_min) * .5f, .25f));
        return host.set_camera(camera);
    }
    auto& runtime = context.document.world().runtime();
    math::Vector3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    math::Vector3 high = -low;
    bool any{};
    const auto include = [&](math::Vector3 p) {
        for (int i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], p[i]);
            high[i] = std::max(high[i], p[i]);
        }
        any = true;
    };
    const auto& selection = context.selection;
    const auto wanted = [&](ObjectId entity) {
        if (!selected_only)
            return true;
        while (entity != ObjectId{}) {
            if (entity == selection.active || std::ranges::find(selection.entities, entity) != selection.entities.end())
                return true;
            const auto parent = world::parent_entity(runtime, entity);
            if (!parent)
                return false;
            entity = *parent;
        }
        return false;
    };
    ATELIER_TRY(context.document.sync_procedural_generators(context.project));
    ATELIER_TRY_BIND(items, context.document.presentation().render_items());
    std::unordered_set<ObjectId> rendered;
    for (const auto& item : *items) {
        rendered.insert(item.entity);
        if (!wanted(item.entity))
            continue;
        const auto world_b = math::transform_bounds(item.geometry->bounds(), item.world);
        if (!math::is_empty(world_b) && math::is_finite(world_b)) {
            include(world_b.minimum);
            include(world_b.maximum);
        }
    }
    ATELIER_TRY_BIND(members, context.document.world().members());
    for (const auto& member : *members) {
        if (!wanted(member.id()) || rendered.contains(member.id()))
            continue;
        auto spatial = world::has_transform(runtime, member.id());
        if (!spatial || !*spatial)
            continue;
        auto matrix = world::world_matrix(runtime, member.id());
        if (!matrix)
            continue;
        include({(*matrix)[12], (*matrix)[13], (*matrix)[14]});
    }
    if (!any)
        return std::unexpected(Error{ErrorCode::invalid_definition, selected_only
                                                                        ? "Select an Entity with a Transform to frame."
                                                                        : "The scene has nothing to frame."});
    auto camera = host.camera();
    camera.frame((low + high) * .5f, math::length(high - low) * .5f);
    return host.set_camera(camera);
}

// Something in a selected subtree must have a Transform to frame.
Result<void> frame_selected_poll(const Context& context) {
    if (!context.active_graph.empty())
        return context.document.has_graph(context.active_graph) ? Result<void>{}
                                                                : unavailable("Open a procedural graph first.");
    if (!context.scene_available)
        return unavailable("Load a scene before framing the selection.");
    if (context.selection.entities.empty())
        return unavailable("Select an Entity with a Transform to frame.");
    auto& runtime = context.document.world().runtime();
    std::vector<ObjectId> pending = context.selection.entities;
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto object = pending.back();
        pending.pop_back();
        if (!seen.insert(object).second)
            continue;
        const auto state = world::inspect_entity(runtime, object);
        if (!state || !state->can_edit)
            return unavailable("The selection cannot be edited.");
        const auto spatial = world::has_transform(runtime, object);
        if (!spatial)
            return std::unexpected(spatial.error());
        if (*spatial)
            return {};
        const auto children = world::children(runtime, object);
        if (!children)
            return std::unexpected(children.error());
        for (const auto& child : *children)
            pending.push_back(child.id());
    }
    return unavailable("Select an Entity with a Transform to frame.");
}

// The focused region owns the history: Code drafts, an inspected material/graph asset, the Controls samples, else the scene.
OperatorInfo history(bool redo) {
    const auto empty = [redo] { return unavailable(redo ? "Nothing to redo." : "Nothing to undo."); };
    return {redo ? "edit.redo" : "edit.undo",
            redo ? L"Redo" : L"Undo",
            redo ? "Redo one step of the focused region's history" : "Undo one step of the focused region's history",
            OperatorFlag::history,
            [redo, empty](const Context& context) -> Result<void> {
                const auto can = [&](HistoryState history) {
                    return (redo ? history.redo : history.undo) ? Result<void>{} : empty();
                };
                if (context.region == RegionKind::code)
                    return can(context.source_history);
                if (const auto graph_key = context.focused_graph(); !graph_key.empty()) {
                    if (context.document.world().running())
                        return unavailable("Stop the running World before editing procedural graphs.");
                    if (context.interactive && context.is_graph_locked(graph_key))
                        return unavailable("Graph is in Locked Preview; click Locked in the Network toolbar to edit.");
                    return context.document.graph_can_undo(graph_key, redo) ? Result<void>{} : empty();
                }
                if (!context.asset.empty())
                    return context.document.material_can_undo(context.asset, redo) ? Result<void>{} : empty();
                if (context.region == RegionKind::controls)
                    return can(context.workbench_history);
                return (redo ? context.document.redo_count() : context.document.undo_count()) ? Result<void>{}
                                                                                              : empty();
            },
            [redo](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) -> Result<void> {
                if (context.region == RegionKind::code)
                    return host.source_history(redo);
                if (const auto graph_key = context.focused_graph(); !graph_key.empty())
                    return context.document.graph_undo(graph_key, redo);
                if (!context.asset.empty())
                    return context.document.material_undo(context.asset, redo, context.project);
                if (context.region == RegionKind::controls)
                    return host.workbench_history(redo);
                auto moved =
                    committed(redo ? context.document.redo(context.project) : context.document.undo(context.project));
                host.scene_changed();
                return moved;
            }};
}

// Scene edits address the context's active Entity.
Result<void> editable_entity(const Context& context) {
    if (!context.scene_available)
        return unavailable("Load a scene before editing it.");
    if (context.selection.active == ObjectId{})
        return unavailable("Select an Entity.");
    const auto state = world::inspect_entity(context.document.world().runtime(), context.selection.active);
    if (!state || !state->can_edit)
        return unavailable("The selected Entity cannot be edited.");
    return {};
}

OperatorInfo transform_presence(bool add) {
    return {add ? "scene.transform_add" : "scene.transform_remove",
            add ? L"Add Transform" : L"Remove Transform",
            add ? "Give the active Entity an identity Transform" : "Remove the active Entity's Transform",
            OperatorFlag::undo,
            [add](const Context& context) -> Result<void> {
                ATELIER_TRY(editable_entity(context));
                const auto spatial = world::has_transform(context.document.world().runtime(), context.selection.active);
                if (!spatial)
                    return std::unexpected(spatial.error());
                if (*spatial == add)
                    return unavailable(add ? "The Entity already has a Transform." : "The Entity has no Transform.");
                return {};
            },
            [add](OperatorHost&, const Context& context, const OperatorParameters&, ValueOutput) {
                return add ? context.document.set_transform(context.selection.active, {})
                           : context.document.remove_transform(context.selection.active);
            }};
}

Result<void> docked(const Context& context) {
    return context.region == RegionKind::none ? unavailable("Focus a pane first.") : Result<void>{};
}

Result<void> spatial_selection(const Context& context) {
    if (!context.scene_available)
        return unavailable("Load a scene before editing Entities.");
    for (const auto entity : context.selection.entities) {
        const auto spatial = world::has_transform(context.document.world().runtime(), entity);
        if (!spatial)
            return std::unexpected(spatial.error());
        if (*spatial)
            return {};
    }
    return unavailable("Select an Entity with a Transform.");
}

Result<void> select_all(OperatorHost& host, const Context& context, const SceneSelectAll& p) {
    auto result = host.viewport_tool().select_all(context.document, context.selection.entities, p.action);
    host.scene_changed();
    return result;
}

Result<void> clear_transform(OperatorHost& host, const Context& context, const SceneTransformClear& p) {
    return host.viewport_tool().clear(context.document, context.selection.entities, p.channel);
}

Result<void> scene_delete(OperatorHost& host, const Context& context, const SceneDelete& p) {
    const auto& selection = context.selection;
    if (p.target == DeleteTarget::object && selection.object == ObjectId{})
        return {};
    auto result = p.target == DeleteTarget::object && selection.object != selection.active
                      ? context.document.remove_component(selection.object)
                      : context.document.erase_entities(selection.entities);
    host.scene_changed();
    return committed(std::move(result));
}

Result<void> scene_duplicate(OperatorHost& host, const Context& context, const SceneDuplicate& p) {
    if (p.place && context.region != RegionKind::viewport)
        return unavailable("Duplicate placement requires a viewport context.");
    auto result = context.document.duplicate_entities(context.selection.entities);
    host.scene_changed();
    ATELIER_TRY(committed(std::move(result)));
    return p.place ? host.place_duplicates() : Result<void>{};
}

Result<void> scene_add_component(OperatorHost& host, const Context& context, const SceneAddComponent& p) {
    const auto type = context.document.world().runtime().find_type({p.type.c_str(), p.type.size()});
    if (!type)
        return std::unexpected(type.error());
    auto result = context.document.add_component(context.selection.active, *type);
    host.scene_changed();
    return committed(std::move(result));
}

Result<void> scene_transform(OperatorHost& host, const Context& context, const SceneTransform& p) {
    auto result = context.document.set_transform(context.selection.active, {p.position, p.rotation, p.scale});
    host.scene_changed();
    return result;
}

Result<void> scene_rename(OperatorHost& host, const Context& context, const SceneRename& p) {
    if (p.name.empty())
        return host.begin_entity_rename(context.selection.active, context.region);
    auto result = context.document.rename(context.selection.active,
                                          platform::utf8_to_utf16_validated({p.name.c_str(), p.name.size()}));
    host.scene_changed();
    return result;
}

Result<void> scene_set_enabled(OperatorHost& host, const Context& context, const SceneSetEnabled& p) {
    const ObjectId target =
        context.selection.object != ObjectId{} ? context.selection.object : context.selection.active;
    auto result = committed(context.document.set_enabled(target, p.enabled));
    host.scene_changed();
    return result;
}

Result<void> snap_selection(OperatorHost& host, const Context& context, const SceneSnap& p) {
    return host.viewport_tool().snap_selection(context.document, context.selection.entities, context.tools,
                                               p.to == SnapTarget::cursor ? std::optional{context.cursor}
                                                                          : std::nullopt);
}

Result<void> layout_show(OperatorHost& host, const Context&, const LayoutShow& p) {
    if (p.tab == RegionKind::none)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Choose a workspace page to show."});
    host.show_tab(p.tab);
    return {};
}

Result<void> layout_cycle(OperatorHost& host, const Context& context, const LayoutCycle& p) {
    if (p.direction)
        host.cycle_tab(context.region, p.direction);
    return {};
}

Result<void> add_poll(const Context& context) {
    if (!context.scene_available)
        return unavailable("Load a scene before adding to it.");
    const auto members = context.document.world().members();
    if (!members)
        return std::unexpected(members.error());
    if (members->size() >= Session::maximum_entities)
        return unavailable("The scene has reached its Entity limit.");
    return {};
}

Result<void> scene_add(OperatorHost& host, const Context& context, const SceneAdd& p) {
    auto& document = context.document;
    Result<world::Commit> added;
    switch (p.kind) {
    case AddKind::entity:
        added = document.add_entity();
        break;
    case AddKind::mesh: {
        const auto catalog = assets::mesh_assets();
        std::string_view key{p.mesh.c_str(), p.mesh.size()};
        if (key.empty())
            key = catalog.front().key;
        assets::Reader engine;
        auto& reader = context.project ? context.project->assets() : engine;
        auto asset = reader.mesh(key);
        if (!asset)
            return context.project ? std::unexpected(asset.error())
                                   : std::unexpected(Error{ErrorCode::invalid_definition, "Unknown mesh asset."});
        added = document.create_static_mesh(*asset, context.cursor, context.project);
        break;
    }
    case AddKind::procedural_generator: {
        std::string_view key{p.graph.c_str(), p.graph.size()};
        added = document.create_procedural_generator(key, context.cursor, context.project);
        break;
    }
    case AddKind::point:
        added = document.create_light(presentation::LightKind::point, context.cursor);
        break;
    case AddKind::rectangle:
        added = document.create_light(presentation::LightKind::rectangle, context.cursor);
        break;
    case AddKind::directional:
        added = document.create_light(presentation::LightKind::directional, context.cursor);
        break;
    case AddKind::camera:
        added = document.create_camera(context.cursor);
        break;
    }
    auto result = committed(std::move(added));
    host.scene_changed();
    return result;
}

Result<void> scene_parent(OperatorHost& host, const Context& context, const SceneParent& p) {
    auto& document = context.document;
    auto& runtime = document.world().runtime();
    const auto reference = runtime.read_reference({document.types().entity, ValueForm::weak_reference}, p.parent);
    if (!reference)
        return std::unexpected(reference.error());
    ATELIER_TRY_BIND(parent, runtime.try_acquire(*reference));
    if (!reference->is_null() && !*parent)
        return std::unexpected(Error{ErrorCode::invalid_object, "The parent Entity is no longer alive."});
    auto result =
        committed(document.reparent(context.selection.entities, *parent ? (*parent)->id() : ObjectId{},
                                    p.keep_world ? world::ReparentRule::keep_world : world::ReparentRule::keep_local,
                                    p.index < 0 ? world::append : static_cast<std::size_t>(p.index)));
    host.scene_changed();
    return result;
}

Result<void> scene_space_reference(OperatorHost& host, const Context& context, const SceneSpaceReference& p) {
    auto& document = context.document;
    auto& runtime = document.world().runtime();
    const auto reference = runtime.read_reference({document.types().entity, ValueForm::weak_reference}, p.origin);
    if (!reference)
        return std::unexpected(reference.error());
    if (p.world_space && !reference->is_null())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "World space requires a null origin."});
    ATELIER_TRY_BIND(held, runtime.try_acquire(*reference));
    if (!reference->is_null() && !*held)
        return std::unexpected(Error{ErrorCode::invalid_object, "The spatial origin Entity is no longer alive."});
    std::optional<ObjectId> origin;
    if (*held)
        origin = (*held)->id();
    else if (p.world_space)
        origin = ObjectId{};
    auto result = committed(
        document.set_space_reference(context.selection.active, origin,
                                     p.keep_world ? world::ReparentRule::keep_world : world::ReparentRule::keep_local));
    host.scene_changed();
    return result;
}

Result<void> tool_settings(OperatorHost& host, const Context&, const ToolSettings& p) {
    host.set_tool_settings(p);
    return {};
}

constexpr std::array tool_flags{&ToolSettings::grid_snap, &ToolSettings::cursor_surface, &ToolSettings::snap_move,
                                &ToolSettings::snap_rotate, &ToolSettings::snap_scale};

Result<void> tool_toggle(OperatorHost& host, const Context& context, const ToolToggle& p) {
    auto settings = context.tools;
    auto& flag = settings.*tool_flags[static_cast<std::size_t>(p.setting)];
    flag = !flag;
    host.set_tool_settings(settings);
    return {};
}

Result<void> view_axis(OperatorHost& host, const Context&, const ViewAxis& p) {
    auto camera = host.camera();
    camera.set_view(p.view);
    return host.set_camera(camera);
}

Result<void> seed_view(const Context& context, ValueOutput out) {
    auto& runtime = context.document.world().runtime();
    const auto info = runtime.describe(out.type);
    if (!info)
        return std::unexpected(info.error());
    auto current = context.view;
    // Author fields can have changed since the workspace's last projection (for example by Undo).
    const auto environment = context.document.presentation().environment();
    if (!environment)
        return std::unexpected(environment.error());
    static_cast<rendering::SceneLightingSettings&>(current.lit) = *environment;
    const auto options = view_options();
    const std::span storage{static_cast<std::byte*>(out.data), (*info)->size};
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& option = options[i];
        const auto& field = (*info)->fields[i];
        const auto number = view_value(current, option.option);
        Scalar value;
        switch (option.kind) {
        case ViewValueKind::boolean:
            value = number != 0;
            break;
        case ViewValueKind::integer:
            value = static_cast<std::int32_t>(number);
            break;
        case ViewValueKind::number:
            value = number;
            break;
        case ViewValueKind::enumeration:
            value = EnumValue{field.value.type, static_cast<std::uint32_t>(number)};
            break;
        }
        ATELIER_TRY(runtime.write(out.type, storage, std::span{&field.id, 1}, value));
    }
    return {};
}

Result<void> view_settings(OperatorHost& host, const Context& context, const OperatorParameters& parameters,
                           ValueOutput) {
    const auto input = parameters.value;
    auto& runtime = context.document.world().runtime();
    const auto info = runtime.describe(input.type);
    if (!info)
        return std::unexpected(info.error());
    auto candidate = context.view;
    const auto options = view_options();
    const std::span storage{static_cast<const std::byte*>(input.data), (*info)->size};
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& field = (*info)->fields[i];
        const auto value = runtime.read(input.type, storage, std::span{&field.id, 1});
        if (!value)
            return std::unexpected(value.error());
        const auto number = std::visit(
            [](const auto& leaf) -> double {
                if constexpr (std::same_as<std::decay_t<decltype(leaf)>, EnumValue>)
                    return leaf.bits;
                else
                    return static_cast<double>(leaf);
            },
            *value);
        ATELIER_TRY(set_view_option(candidate, options[i].option, number));
    }
    if (candidate.grid_plane != context.view.grid_plane)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Persistent grid plane follows camera view."});
    return host.set_view_settings(candidate);
}

Result<void> project_open(const Context& context) {
    return context.project ? Result<void>{} : unavailable("Open a project folder first.");
}

Result<std::optional<std::filesystem::path>> operation_path(OperatorHost& host, const String& value,
                                                            PathPurpose purpose, const std::filesystem::path& initial,
                                                            std::wstring_view filename = {}) {
    if (!value.empty())
        return std::optional<std::filesystem::path>{platform::utf8_to_utf16_validated({value.c_str(), value.size()})};
    auto chosen = host.choose_path(purpose, initial, filename);
    if (chosen && !*chosen)
        host.cancel_operator();
    return chosen;
}

OperatorInfo open_document(std::string id, std::wstring title, DocumentRequest::Kind kind) {
    return path_operator<OpenPath>(
        {std::move(id), std::move(title), "Replace the document after the current operator's values retire",
         OperatorFlag::replaces_document, kind == DocumentRequest::Kind::open_scene ? project_open : available,
         [kind](OperatorHost& host, const Context& context, const OperatorParameters& parameters,
                ValueOutput) -> Result<void> {
             const auto& p = *static_cast<const OpenPath*>(parameters.value.data);
             const bool scene = kind == DocumentRequest::Kind::open_scene;
             ATELIER_TRY_BIND(
                 path, operation_path(host, p.path, scene ? PathPurpose::scene_open : PathPurpose::folder,
                                      context.project ? context.project->root() / "Content" : std::filesystem::path{}));
             if (!*path)
                 return {};
             host.replace_document(
                 {kind, scene && (**path).is_absolute() ? (**path).lexically_relative(context.project->root()) : **path,
                  context.interactive});
             return {};
         },
         "editor.OpenPath"});
}

Result<void> project_build(OperatorHost& host, const Context& context, const ProjectBuildParameters& parameters) {
    host.replace_document({DocumentRequest::Kind::build, {}, context.interactive, parameters.rebuild});
    return {};
}

OperatorInfo source_open(std::string id, std::wstring title, bool create, bool project_ui = false) {
    return path_operator<OpenPath>(
        {std::move(id), std::move(title), "Open source, or create and open project source", OperatorFlag::undo,
         create ? project_open : available,
         [create, project_ui](OperatorHost& host, const Context& context, const OperatorParameters& parameters,
                              ValueOutput) -> Result<void> {
             const auto& p = *static_cast<const OpenPath*>(parameters.value.data);
             if (!context.project && !context.editor_ui_source)
                 return std::unexpected(context.editor_ui_source.error());
             auto path = operation_path(host, p.path, create ? PathPurpose::source_save : PathPurpose::source,
                                        context.project ? context.project->root() / "Source"
                                                        : context.editor_ui_source->parent_path(),
                                        project_ui ? L"Main.ui.source" : L"Move.source");
             if (!path)
                 return std::unexpected(path.error());
             if (!*path)
                 return {};
             if (!create)
                 return host.open_source(**path);
             auto relative = (**path).is_absolute() ? (**path).lexically_relative(context.project->root()) : **path;
             if (project_ui && !relative.filename().wstring().ends_with(L".ui.source"))
                 relative.replace_extension(L".ui.source");
             return host.create_source(relative, project_ui);
         },
         "editor.OpenPath"});
}

Result<void> save_scene(OperatorHost& host, const Context& context, const SceneSave& p) {
    if (p.path.empty() && !p.as_new)
        return host.save_scene({});
    ATELIER_TRY_BIND(path, operation_path(host, p.path, PathPurpose::scene_save, context.project->root() / "Content"));
    if (!*path)
        return {};
    return host.save_scene((**path).is_absolute() ? (**path).lexically_relative(context.project->root()) : **path);
}

Result<void> import_asset(OperatorHost& host, const Context& context, const AssetImport& p, AssetResult& result) {
    ATELIER_TRY_BIND(path,
                     operation_path(host, p.path, p.kind == ImportKind::mesh ? PathPurpose::mesh : PathPurpose::texture,
                                    context.project->root() / "Content"));
    if (!*path)
        return {};
    ATELIER_TRY_BIND(imported, host.import_asset(p.kind, **path));
    if (!result.asset.assign(StringView{*imported}))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Imported asset key requires valid UTF-8."});
    return {};
}

Result<void> reimport_asset(OperatorHost& host, const Context& context, const OpenPath& p) {
    const auto& entry = *context.project->entry(context.asset);
    auto path =
        operation_path(host, p.path, entry.kind == assets::AssetKind::mesh ? PathPurpose::mesh : PathPurpose::texture,
                       context.project->root() / entry.path.parent_path());
    if (!path)
        return std::unexpected(path.error());
    return *path ? host.reimport_asset(context.asset, **path) : Result<void>{};
}

template <class P>
Result<void> new_material(OperatorHost& host, const Context& context, const P& p, AssetResult& result) {
    constexpr bool source = std::same_as<P, NewMaterialSource>;
    auto& project = *context.project;
    assets::MaterialAsset author;
    if constexpr (!source) {
        const auto parsed =
            json::Document::parse({p.entries.json.c_str(), p.entries.json.size()},
                                  {std::numeric_limits<std::size_t>::max(), json::DepthPolicy::containers, true});
        if (!parsed)
            return std::unexpected(Error{ErrorCode::argument_mismatch, parsed.error().describe()});
        auto entries = assets::material_format::decode_entries(parsed->root());
        if (!entries)
            return std::unexpected(Error{ErrorCode::argument_mismatch, entries.error().describe()});
        std::get<assets::MaterialInstanceAsset>(author.author).entries = std::move(*entries);
    }
    ATELIER_TRY_BIND(chosen, operation_path(host, p.path,
                                            source ? PathPurpose::material_source_save : PathPurpose::material_save,
                                            project.root() / "Content"));
    if (!*chosen)
        return {};
    const auto& path = **chosen;
    const auto relative = path.is_absolute() ? path.lexically_relative(project.root()) : path;
    const auto* parent = project.entry(context.asset);
    if ((parent && parent->kind == assets::AssetKind::material) ||
        std::ranges::find(assets::material_assets(), context.asset, &assets::MaterialAsset::key) !=
            assets::material_assets().end())
        std::get<assets::MaterialInstanceAsset>(author.author).parent_key = context.asset;
    auto created = [&] {
        if constexpr (source)
            return project.create_material_source(relative, {p.source.c_str(), p.source.size()});
        else
            return project.create_material(relative, author);
    }();
    if (!created)
        return std::unexpected(created.error());
    if (!result.asset.assign(StringView{*created}))
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Created material key requires valid UTF-8."});
    if (context.interactive) {
        ATELIER_TRY_BIND(loaded, context.document.material_resource(project.assets(), *created));
        host.asset_created(*created, true);
    }
    return {};
}

Result<void> new_procedural_graph(OperatorHost& host, const Context& context, const NewProceduralGraph& p,
                                  AssetResult& result) {
    auto& project = *context.project;
    ATELIER_TRY_BIND(chosen, operation_path(host, p.path, PathPurpose::procedural_graph_save,
                                            project.root() / "Content", L"Terrain.procgraph"));
    if (!*chosen)
        return {};
    const auto& path = **chosen;
    auto relative = path.is_absolute() ? path.lexically_relative(project.root()) : path;
    if (!assets::is_procedural_graph_path(relative))
        relative.replace_extension(L".procgraph");
    ATELIER_TRY_BIND(created, project.create_procedural_graph(relative, {p.source.c_str(), p.source.size()}));
    if (!result.asset.assign(StringView{*created}))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Created procedural graph key requires valid UTF-8."});
    ATELIER_TRY(context.document.open_graph(project, *created));
    if (context.interactive) {
        host.asset_created(*created, false);
        ATELIER_TRY(host.open_graph_context(*created));
    }
    return {};
}

Result<std::string> target_graph_key(const Context& context, const String& explicit_asset) {
    if (!explicit_asset.empty())
        return std::string(explicit_asset.c_str(), explicit_asset.size());
    if (!context.asset.empty() && context.document.has_graph(context.asset))
        return context.asset;
    if (!context.active_graph.empty())
        return context.active_graph;
    if (!context.network_graph.empty())
        return context.network_graph;
    return unavailable("Open or specify a procedural graph first.");
}

Result<std::string> editable_target_graph_key(const Context& context, const String& explicit_asset) {
    ATELIER_TRY_BIND(key, target_graph_key(context, explicit_asset));
    if (context.document.world().running())
        return unavailable("Stop the running World before editing procedural graphs.");
    if (context.interactive && context.is_graph_locked(*key))
        return unavailable("Graph is in Locked Preview; click Locked in the Network toolbar to edit.");
    return key;
}

Result<std::string> resolve_graph_node_name(const Context& context, std::string_view key, const String& explicit_node,
                                            std::string_view action_verb) {
    const bool is_viewed = (key == context.active_graph || key == context.network_graph);
    std::string node_name = !explicit_node.empty() ? std::string(explicit_node.c_str(), explicit_node.size())
                                                   : (is_viewed ? context.selected_graph_node : std::string{});
    if (node_name.empty())
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Select or specify a node to " + std::string(action_verb) + "."});
    return node_name;
}

Result<std::vector<std::string>> resolve_graph_node_targets(const Context& context, std::string_view key,
                                                            const String& explicit_nodes, bool split_commas,
                                                            std::string_view action_verb) {
    const bool is_viewed = (key == context.active_graph || key == context.network_graph);
    std::vector<std::string> targets;
    if (!explicit_nodes.empty()) {
        if (split_commas) {
            const std::string_view raw{explicit_nodes.c_str(), explicit_nodes.size()};
            std::size_t pos = 0;
            while (pos < raw.size()) {
                const std::size_t comma = raw.find(',', pos);
                std::string_view tok = comma == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, comma - pos);
                while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                    tok.remove_prefix(1);
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                    tok.remove_suffix(1);
                if (!tok.empty())
                    targets.emplace_back(tok);
                if (comma == std::string_view::npos)
                    break;
                pos = comma + 1;
            }
        } else {
            targets.emplace_back(explicit_nodes.c_str(), explicit_nodes.size());
        }
    } else if (is_viewed) {
        if (!context.selected_graph_nodes.empty())
            targets = context.selected_graph_nodes;
        else if (!context.selected_graph_node.empty())
            targets.push_back(context.selected_graph_node);
    }
    if (targets.empty())
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Select or specify a node to " + std::string(action_verb) + "."});
    return targets;
}
}

Result<OperatorRegistry> OperatorRegistry::builtins() {
    OperatorRegistry registry;
    const auto add = [&](OperatorInfo info) { registry.operators_.push_back(std::move(info)); };
    add({"ui.menu",
         L"Viewport Menu",
         "Open a menu at the viewport pointer",
         OperatorFlag::none,
         [](const Context& c) {
             return c.region == RegionKind::viewport ? Result<void>{} : unavailable("Menus require a viewport.");
         },
         with_parameters(+[](OperatorHost& host, const Context&, const MenuParameters& p) -> Result<void> {
             host.show_menu(p.menu);
             return {};
         }),
         "editor.Menu",
         {},
         {},
         {},
         OperatorKind::interactive});
    add({"view.frame_all", L"Frame All", "Frame every rendered Entity and spatial locator", OperatorFlag::none,
         available, [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             return frame(host, context, false);
         }});
    add({"view.frame_selected", L"Focus Selection", "Frame the selected Entities and their subtrees",
         OperatorFlag::none, frame_selected_poll,
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             return frame(host, context, true);
         }});
    add({"view.reset", L"Reset Camera", "Return the viewport camera to its default view", OperatorFlag::none, available,
         [](OperatorHost& host, const Context&, const OperatorParameters&, ValueOutput) {
             auto camera = host.camera();
             camera.reset();
             return host.set_camera(camera);
         }});
    add({"view.from_camera", L"View from Camera",
         "Copy the selected scene camera's pose and projection into the editor viewport once", OperatorFlag::none,
         [](const Context& context) -> Result<void> {
             ATELIER_TRY(editable_entity(context));
             const auto camera = context.document.presentation().camera(context.selection.active);
             return camera ? Result<void>{} : std::unexpected(camera.error());
         },
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) -> Result<void> {
             ATELIER_TRY_BIND(snapshot, context.document.presentation().camera(context.selection.active));
             auto camera = host.camera();
             if (!camera.view_from(*snapshot))
                 return unavailable("The camera view cannot be represented in the editor viewport.");
             return host.set_camera(camera);
         }});
    add({"scene.main_camera_set", L"Set as Main Camera",
         "Select this Entity's camera for Play; disabled cameras may be selected", OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             ATELIER_TRY(editable_entity(context));
             if (context.document.world().running())
                 return unavailable("Stop the document before changing its main camera.");
             ATELIER_TRY_BIND(cameras, context.document.world().members(context.document.presentation().cameras()));
             if (std::ranges::count(*cameras, context.selection.active, &world::CapabilityMember::entity) != 1)
                 return unavailable("Select an Entity with exactly one CameraComponent.");
             return {};
         },
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             auto result = context.document.set_main_camera(context.selection.active);
             host.scene_changed();
             return result;
         }});
    add({"scene.main_camera_clear", L"Clear Main Camera", "Clear the scene's explicit Play camera selection",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (!context.scene_available || context.document.world().running())
                 return unavailable("Open a stopped document first.");
             const auto environment = context.document.environment();
             ATELIER_TRY_BIND(selected, context.document.main_camera());
             if (*selected == ObjectId{} && (environment == ObjectId{} || !context.document.has_retained(environment)))
                 return unavailable("The scene has no main camera selection.");
             return {};
         },
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             auto result = context.document.set_main_camera({});
             host.scene_changed();
             return result;
         }});
    add({"layout.reset", L"Reset Layout", "Restore the default pane layout", OperatorFlag::none, available,
         [](OperatorHost& host, const Context&, const OperatorParameters&, ValueOutput) {
             return host.reset_layout();
         }});
    add({"layout.show", L"Show Pane", "Show and focus a workspace page", OperatorFlag::none, available,
         with_parameters(layout_show), "editor.LayoutShow"});
    add({"layout.cycle_tab", L"Cycle Pane Tab", "Select the adjacent tab in the context region's pane",
         OperatorFlag::none, docked, with_parameters(layout_cycle), "editor.LayoutCycle"});
    add({"layout.close_tab", L"Close Pane Tab",
         "Close the focused region's pane tab; on the Code page, close its active document first", OperatorFlag::none,
         docked, [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             return host.close_tab(context.region, context.interactive);
         }});
    add({"layout.maximize", L"Maximize / Restore Pane",
         "Maximize the focused region's pane, or restore the layout when it is maximized", OperatorFlag::none, docked,
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             host.maximize_tab(context.region);
             return Result<void>{};
         }});
    add(history(false));
    add(history(true));
    add(transform_presence(true));
    add(transform_presence(false));
    add({"scene.discard_saved_data", L"Discard Saved Data",
         "Drop the saved data kept for the inspected object's missing type, fields or resources", OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (!context.scene_available)
                 return unavailable("Load a scene before editing it.");
             const auto object = context.selection.object;
             if (object == ObjectId{} || !context.document.has_retained(object))
                 return unavailable("The inspected object has no saved data.");
             return {};
         },
         [](OperatorHost&, const Context& context, const OperatorParameters&, ValueOutput) {
             return context.document.discard_retained(context.selection.object);
         }});
    add({"scene.select", L"Select",
         "Make the context selection the document selection; a component can be inspected only with one Entity selected",
         OperatorFlag::none, available,
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) -> Result<void> {
             const auto& selection = context.selection;
             if (selection.object != selection.active && selection.entities.size() != 1)
                 return std::unexpected(
                     Error{ErrorCode::unsupported_operation, "Inspect a component with only its Entity selected."});
             ATELIER_TRY_BIND(selected, selection.object != selection.active
                                            ? context.document.select(selection.object)
                                            : context.document.select_entities(selection.entities, selection.active));
             host.scene_changed();
             return {};
         }});
    add({"scene.add", L"Add", "Add an Entity, a static mesh, a light or a camera at the 3D cursor and select it",
         OperatorFlag::undo, add_poll, with_parameters(scene_add), "editor.SceneAdd"});
    add({"scene.parent_set", L"Set Parent",
         "Reparent the explicit selection, recording its committed prefix as one history entry", OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (!context.scene_available)
                 return unavailable("Load a scene before reparenting Entities.");
             if (context.selection.entities.empty())
                 return unavailable("Select Entities to reparent.");
             return {};
         },
         with_parameters(scene_parent), "editor.SceneParent"});
    add({"scene.space_reference_set", L"Set Space Reference",
         "Choose inherited, explicit World or an Entity's space without changing composition", OperatorFlag::undo,
         editable_entity, with_parameters(scene_space_reference), "editor.SceneSpaceReference"});
    add({"tool.settings",
         L"Tool Settings",
         "Change the viewport tool, selection mode and snapping; unnamed fields keep their values",
         OperatorFlag::none,
         available,
         with_parameters(tool_settings),
         "editor.ToolSettings",
         [](const Context& context, ValueOutput out) -> Result<void> {
             const auto type = context.document.world().runtime().native_type<ToolSettings>();
             if (!type || *type != out.type)
                 return std::unexpected(Error{ErrorCode::type_mismatch, "Operator parameters have another type."});
             *static_cast<ToolSettings*>(out.data) = context.tools;
             return {};
         },
         {},
         [](const Context& c, ValueInput p) { return c.tools == *static_cast<const ToolSettings*>(p.data); }});
    add({"tool.toggle",
         L"Toggle Tool Setting",
         "Invert one tool setting (non-idempotent); automation should set absolute values with tool.settings",
         OperatorFlag::none,
         available,
         with_parameters(tool_toggle),
         "editor.ToolToggle",
         {},
         {},
         [](const Context& c, ValueInput p) {
             return c.tools.*tool_flags[static_cast<std::size_t>(static_cast<const ToolToggle*>(p.data)->setting)];
         }});
    add({"view.axis",
         L"View",
         "Switch the viewport camera to perspective or an axis view around the same target",
         OperatorFlag::none,
         available,
         with_parameters(view_axis),
         "editor.ViewAxis",
         {},
         {},
         [](const Context& c, ValueInput p) { return c.camera_view == static_cast<const ViewAxis*>(p.data)->view; }});
    add({"view.camera_set", L"Set Camera", "Restore a complete viewport camera state", OperatorFlag::none, available,
         with_parameters(+[](OperatorHost& host, const Context&, const ViewCameraSet& p) -> Result<void> {
             auto camera = host.camera();
             if (!camera.set_state(p.state))
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid camera state."});
             return host.set_camera(camera);
         }),
         "editor.ViewCameraSet"});
    add({"view.look_at", L"Look At", "Aim the viewport camera at a world-space target", OperatorFlag::none, available,
         with_parameters(+[](OperatorHost& host, const Context&, const ViewLookAt& p) -> Result<void> {
             auto camera = host.camera();
             if (!camera.look_at(p.eye, p.target))
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid camera eye/target."});
             return host.set_camera(camera);
         }),
         "editor.ViewLookAt"});
    add({"view.set", L"View Settings",
         "Patch viewport display and World author settings; unnamed fields keep their values", OperatorFlag::undo,
         available, view_settings, "editor.ViewSettings", seed_view});
    add({"scene.select_all", L"Select All", "Select, deselect or invert the context selection", OperatorFlag::none,
         [](const Context& context) -> Result<void> {
             return context.scene_available ? Result<void>{} : unavailable("Load a scene before selecting Entities.");
         },
         with_parameters(select_all), "editor.SceneSelectAll"});
    add({"scene.transform_clear", L"Clear Transform", "Clear one local transform channel of the selected roots",
         OperatorFlag::undo, spatial_selection, with_parameters(clear_transform), "editor.SceneTransformClear"});
    add({"scene.delete", L"Delete", "Delete context objects while preserving unrelated document selection",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             return context.scene_available && !context.selection.entities.empty()
                        ? Result<void>{}
                        : unavailable("Select Entities to delete.");
         },
         with_parameters(scene_delete), "editor.SceneDelete"});
    add(interactive_when<SceneDuplicate>(
        {"scene.duplicate", L"Duplicate Entities", "Copy context subtrees with fresh identities and shared resources",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (!context.scene_available || context.selection.entities.empty())
                 return unavailable("Select Entities to duplicate.");
             if (context.interactive && context.region == RegionKind::scene_tree &&
                 context.selection.object != ObjectId{} && context.selection.object != context.selection.active)
                 return unavailable("Select an Entity row to duplicate.");
             const auto members = context.document.world().members();
             if (!members)
                 return std::unexpected(members.error());
             return members->size() + context.selection.entities.size() <= Session::maximum_entities
                        ? Result<void>{}
                        : unavailable("Editor Entity limit reached.");
         },
         with_parameters(scene_duplicate), "editor.SceneDuplicate"},
        +[](const SceneDuplicate& p) { return p.place; },
        "place=true starts a human placement gesture; place=false (default) executes directly."));
    add({"scene.add_component", L"Add Component", "Add a registered component to the context's active Entity",
         OperatorFlag::undo, editable_entity, with_parameters(scene_add_component), "editor.SceneAddComponent"});
    add({"scene.transform_set", L"Set Transform", "Set the context Entity's full local transform in one history entry",
         OperatorFlag::undo, editable_entity, with_parameters(scene_transform), "editor.SceneTransform"});
    add(interactive_when<SceneRename>(
        {"scene.rename", L"Rename", "Rename the context Entity; interactive calls can start inline editing",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (context.interactive && context.region == RegionKind::scene_tree &&
                 context.selection.object != ObjectId{} && context.selection.object != context.selection.active)
                 return unavailable("Select an Entity row to rename.");
             return editable_entity(context);
         },
         with_parameters(scene_rename), "editor.SceneRename"},
        +[](const SceneRename& p) { return p.name.empty(); },
        "An empty name starts inline editing; provide name to execute directly."));
    add({"scene.set_enabled", L"Set Enabled", "Enable or disable the context Entity or inspected Component",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             if (!context.scene_available || context.document.world().running())
                 return unavailable("Stop the document before changing enabled state.");
             const ObjectId target =
                 context.selection.object != ObjectId{} ? context.selection.object : context.selection.active;
             return target != ObjectId{} ? Result<void>{} : unavailable("Select an Entity or Component.");
         },
         with_parameters(scene_set_enabled), "editor.SceneSetEnabled"});
    add({"scene.set_properties", L"Set Properties", "Apply a scalar patch to the inspected object's actual type",
         OperatorFlag::undo,
         [](const Context& context) -> Result<void> {
             const auto object = context.selection.object;
             if (const auto graph_key = context.document.graph_of_object(object)) {
                 if (context.document.world().running())
                     return unavailable("Stop the document first.");
                 if (context.interactive && context.is_graph_locked(*graph_key))
                     return unavailable("Graph is in Locked Preview; click Locked in the Network toolbar to edit.");
                 return {};
             }
             ATELIER_TRY(editable_entity(context));
             if (object == context.selection.active)
                 return {};
             const auto state = world::inspect_component(context.document.world().runtime(), object);
             if (!state || !state->valid || state->owner != context.selection.active)
                 return unavailable("Select an editable object of the active Entity.");
             return {};
         },
         [](OperatorHost& host, const Context& context, const OperatorParameters& parameters, ValueOutput) {
             const bool graph_obj = context.document.is_graph_object(context.selection.object);
             auto written = context.document.set_properties(context.selection.object, parameters.patch);
             if (written && !graph_obj)
                 host.scene_changed();
             return written;
         },
         "@context.object"});
    add({"scene.snap_selection", L"Snap Selection", "Snap selected spatial roots to the grid or the context cursor",
         OperatorFlag::undo, spatial_selection, with_parameters(snap_selection), "editor.SceneSnap"});
    add({"cursor.to_selection", L"Cursor to Selection", "Place the cursor at the center of the selected spatial roots",
         OperatorFlag::none, spatial_selection,
         [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             return host.viewport_tool().cursor_to_selection(context.document, context.selection.entities);
         }});
    add({"cursor.to_grid", L"Cursor to Grid", "Snap the context cursor to the current grid", OperatorFlag::none,
         available, [](OperatorHost& host, const Context& context, const OperatorParameters&, ValueOutput) {
             auto& tool = host.viewport_tool();
             tool.cursor = context.cursor;
             tool.cursor_to_grid(context.tools);
             return Result<void>{};
         }});
    add({"cursor.reset", L"Cursor to Origin", "Reset the 3D cursor's position and orientation", OperatorFlag::none,
         available, [](OperatorHost& host, const Context&, const OperatorParameters&, ValueOutput) {
             auto& tool = host.viewport_tool();
             tool.cursor = {};
             tool.cursor_rotation = {};
             return Result<void>{};
         }});
    add(open_document("project.new", L"New Project", DocumentRequest::Kind::new_project));
    add(open_document("project.open", L"Open Project", DocumentRequest::Kind::open_project));
    add(open_document("scene.open", L"Open Scene", DocumentRequest::Kind::open_scene));
    add({"project.build", L"Build Project", "Build compatible code or explicitly rebuild the document's types",
         OperatorFlag::replaces_document, project_open, with_parameters(project_build), "editor.ProjectBuild"});
    const auto scene = [](const Context& c) -> Result<void> {
        return c.scene_available ? Result<void>{} : unavailable("Load a scene first.");
    };
    const auto saveable = [scene](const Context& c) -> Result<void> { return c.project ? scene(c) : project_open(c); };
    const auto source = [](const Context& c) -> Result<void> {
        return !c.source.empty() ? Result<void>{} : unavailable("Open a source document first.");
    };
    const auto ui_source = [](const Context& c) -> Result<void> {
        return c.project && !c.project->ui_source().empty() ? Result<void>{}
                                                            : unavailable("Create a project UI first.");
    };
    const auto interactive_action = [&](std::string id, std::wstring title, auto poll, void (OperatorHost::*action)()) {
        add({std::move(id),
             std::move(title),
             "Workspace action",
             OperatorFlag::none,
             poll,
             [action](OperatorHost& h, const Context&, const OperatorParameters&, ValueOutput) {
                 (h.*action)();
                 return Result<void>{};
             },
             {},
             {},
             {},
             {},
             OperatorKind::interactive});
    };
    interactive_action("app.quit", L"Exit", available, &OperatorHost::quit);
    interactive_action("app.about", L"About", available, &OperatorHost::about);
    interactive_action("view.select_box", L"Box Select", scene, &OperatorHost::select_box);
    add({"view.toggle",
         L"Toggle View Display",
         "Toggle a viewport display preference",
         OperatorFlag::none,
         available,
         with_parameters(+[](OperatorHost& h, const Context&, const ViewToggle& p) -> Result<void> {
             h.toggle_view(p.option);
             return {};
         }),
         "editor.ViewToggle",
         {},
         {},
         [](const Context& c, ValueInput p) {
             return static_cast<const ViewToggle*>(p.data)->option == ViewToggleOption::fps ? c.show_fps
                                                                                            : c.viewport_options;
         }});
    add({"view.lighting_fit", L"Fit Lighting", "Fit the scene probe volume", OperatorFlag::undo, available,
         with_parameters(+[](OperatorHost& h, const Context&, const LightingFit& p) {
             return h.fit_lighting(p.mode == LightingFitMode::scene_focus);
         }),
         "editor.LightingFit"});
    add(interactive_when<SceneRename>(
        {"asset.rename", L"Rename Asset", "Rename the context's project asset", OperatorFlag::none,
         [](const Context& c) -> Result<void> {
             const auto* entry = c.project ? c.project->entry(c.asset) : nullptr;
             if (!entry || std::distance(entry->path.begin(), entry->path.end()) < 2)
                 return unavailable("Select a project file or folder below a top-level root.");
             if (entry->path == c.project->startup_scene() ||
                 (!c.project->ui_source().empty() && entry->path == c.project->ui_source()))
                 return unavailable("The configured startup scene and UI source cannot be renamed.");
             return {};
         },
         with_parameters(+[](OperatorHost& h, const Context& c, const SceneRename& p) {
             return h.rename_asset(c.asset, {p.name.c_str(), p.name.size()});
         }),
         "editor.SceneRename"},
        +[](const SceneRename& p) { return p.name.empty(); },
        "An empty name starts inline editing; provide name to execute directly."));
    add(interactive_when<SceneSave>(
        {"scene.save", L"Save Scene", "Save the scene after committing its active field", OperatorFlag::undo, saveable,
         with_parameters(save_scene), "editor.SceneSave"},
        +[](const SceneSave& p) { return p.as_new && p.path.empty(); },
        "as_new=true with an empty path opens a picker; provide path or save in place to execute directly."));
    add({"file.save", L"Save", "Save the active document", OperatorFlag::undo,
         [source, saveable](const Context& c) -> Result<void> {
             if (c.region == RegionKind::code)
                 return source(c);
             if (const auto graph = c.focused_graph(); !graph.empty())
                 return c.project && c.project->entry(graph)
                            ? Result<void>{}
                            : unavailable("Create a project .procgraph asset before saving this procedural graph.");
             if (!c.asset.empty())
                 return c.project && c.document.loaded_material(c.asset) != ObjectId{}
                            ? Result<void>{}
                            : unavailable("Open a project material before saving it.");
             return saveable(c);
         },
         [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.save_documents(c, false);
         }});
    add({"file.save_all", L"Save All", "Save dirty materials, source and the current scene", OperatorFlag::undo,
         available, [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.save_documents(c, true);
         }});
    add(source_open("source.open", L"Open Source", false));
    add(source_open("source.new_component", L"New Component Source", true));
    add(source_open("source.new_project_ui", L"New Project UI", true, true));
    add({"source.open_editor_ui", L"Edit Editor UI", "Open the editor UI source", OperatorFlag::undo, available,
         [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) -> Result<void> {
             ATELIER_TRY(c.editor_ui_source);
             return h.open_source(*c.editor_ui_source);
         }});
    add({"source.open_project_ui", L"Edit Project UI", "Open the configured project UI source", OperatorFlag::undo,
         ui_source, [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) -> Result<void> {
             auto path = c.project->resolve(c.project->ui_source());
             return path ? h.open_source(*path) : Result<void>{std::unexpected(path.error())};
         }});
    add({"source.close", L"Close Source", "Close the active source after its save decision", OperatorFlag::undo, source,
         [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.close_source(c.interactive);
         }});
    add({"source.reload", L"Reload Source", "Reload the active source, confirming dirty interactive drafts",
         OperatorFlag::undo, source, [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.reload_source(c.interactive);
         }});
    add({"play.start", L"Run Project", "Save project sources and start Play", OperatorFlag::undo,
         [](const Context& c) -> Result<void> {
             return c.project && c.scene_available && !c.project_running && c.build_ready
                        ? Result<void>{}
                        : unavailable("Open a ready project with a scene and no active run.");
         },
         [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.start_play(false, c.interactive);
         }});
    add({"play.preview_ui", L"Preview Project UI", "Save sources and start the project's UI preview",
         OperatorFlag::undo,
         [ui_source](const Context& c) {
             return c.project_running ? unavailable("Stop the current run first.") : ui_source(c);
         },
         [](OperatorHost& h, const Context& c, const OperatorParameters&, ValueOutput) {
             return h.start_play(true, c.interactive);
         }});
    for (const bool restart : {false, true})
        add({restart ? "play.restart" : "play.stop", restart ? L"Restart Play" : L"Stop Play", "Stop the active run",
             OperatorFlag::none,
             [restart](const Context& c) -> Result<void> {
                 return c.project_running && !c.project_stopping && (!restart || c.play_linked)
                            ? Result<void>{}
                            : unavailable("No stoppable run with the required change link.");
             },
             [restart](OperatorHost& h, const Context&, const OperatorParameters&, ValueOutput) {
                 h.stop_play(restart);
                 return Result<void>{};
             }});
    add(path_operator<AssetImport>({"asset.import",
                                    L"Import Asset",
                                    "Import a mesh or texture and return its asset key",
                                    OperatorFlag::none,
                                    project_open,
                                    with_parameters(import_asset),
                                    "editor.AssetImport",
                                    {},
                                    "editor.AssetResult"}));
    add(path_operator<OpenPath>({"asset.reimport", L"Reimport Asset",
                                 "Replace the context resource while retaining its identity", OperatorFlag::undo,
                                 [](const Context& c) -> Result<void> {
                                     const auto* entry = c.project ? c.project->entry(c.asset) : nullptr;
                                     return c.scene_available && entry &&
                                                    (entry->kind == assets::AssetKind::mesh ||
                                                     entry->kind == assets::AssetKind::texture)
                                                ? Result<void>{}
                                                : unavailable("Select a project mesh or texture in a loaded scene.");
                                 },
                                 with_parameters(reimport_asset), "editor.OpenPath"}));
    add({"asset.refresh", L"Refresh Assets", "Refresh the project catalog", OperatorFlag::none, project_open,
         [](OperatorHost&, const Context& c, const OperatorParameters&, ValueOutput) { return c.project->refresh(); }});
    add(path_operator<NewMaterial>({"asset.new_material",
                                    L"New Material Instance",
                                    "Create an MI, inheriting from the active material",
                                    OperatorFlag::none,
                                    project_open,
                                    with_parameters(new_material<NewMaterial>),
                                    "editor.NewMaterial",
                                    {},
                                    "editor.AssetResult"}));
    add(path_operator<NewMaterialSource>({"asset.new_material_source",
                                          L"New Material Source",
                                          "Create an MS and its source",
                                          OperatorFlag::none,
                                          project_open,
                                          with_parameters(new_material<NewMaterialSource>),
                                          "editor.NewMaterialSource",
                                          {},
                                          "editor.AssetResult"}));
    add(path_operator<NewProceduralGraph>({"asset.new_procedural_graph",
                                           L"New Procedural Graph",
                                           "Create a .procgraph procedural geometry asset",
                                           OperatorFlag::none,
                                           project_open,
                                           with_parameters(new_procedural_graph),
                                           "editor.NewProceduralGraph",
                                           {},
                                           "editor.AssetResult"}));
    const auto world_stopped = [](const Context& c) -> Result<void> {
        if (c.document.world().running())
            return unavailable("Stop the running World before editing procedural graphs.");
        return {};
    };
    const auto graph_editable = [](const Context& c) -> Result<void> {
        if (c.document.world().running())
            return unavailable("Stop the running World before editing procedural graphs.");
        if (c.interactive) {
            const std::string_view key = !c.asset.empty() && c.document.has_graph(c.asset) ? std::string_view{c.asset}
                                         : !c.active_graph.empty() ? std::string_view{c.active_graph}
                                                                   : std::string_view{c.network_graph};
            if (!key.empty() && c.is_graph_locked(key))
                return unavailable("Graph is in Locked Preview; click Locked in the Network toolbar to edit.");
        }
        return {};
    };
    add({"graph.open", L"Open Procedural Graph", "Open a .procgraph or scratch graph into the SOP viewport context",
         OperatorFlag::none, world_stopped,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphOpen& p) -> Result<void> {
             const std::string key = !p.asset.empty() ? std::string(p.asset.c_str(), p.asset.size()) : c.asset;
             if (key.empty())
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Specify a procedural graph asset key."});
             return host.open_graph_context(key);
         }),
         "editor.GraphOpen"});
    add({"graph.exit", L"Exit Procedural Graph", "Return the Scene View from SOP graph context to World",
         OperatorFlag::none,
         [](const Context& c) -> Result<void> {
             return !c.active_graph.empty() ? Result<void>{} : unavailable("No procedural graph context is active.");
         },
         [](OperatorHost& host, const Context&, const OperatorParameters&, ValueOutput) -> Result<void> {
             host.exit_graph_context();
             return {};
         }});
    add({"graph.dive_in", L"Dive Into Digital Asset",
         "Dive into a procedural.AssetNode's referenced .procgraph Digital Asset", OperatorFlag::none, world_stopped,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphDiveIn& p) -> Result<void> {
             ATELIER_TRY_BIND(key, target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "dive into"));
             return host.dive_into_asset_node(*key, *node_name);
         }),
         "editor.GraphDiveIn"});
    add({"graph.save", L"Save Procedural Graph", "Save an open .procgraph asset to disk", OperatorFlag::none,
         project_open, with_parameters(+[](OperatorHost&, const Context& c, const GraphSave& p) -> Result<void> {
             ATELIER_TRY_BIND(key, target_graph_key(c, p.asset));
             ATELIER_TRY(c.document.save_graph(*c.project, *key));
             return c.project->refresh();
         }),
         "editor.GraphSave"});
    add({"graph.add_node",
         L"Add Graph Node",
         "Add a procedural node to a graph and select it",
         OperatorFlag::none,
         graph_editable,
         with_parameters(
             +[](OperatorHost& host, const Context& c, const GraphAddNode& p, GraphNodeResult& out) -> Result<void> {
                 ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
                 if (!c.document.has_graph(*key)) {
                     if (!c.project)
                         return unavailable("Open a project or scratch graph first.");
                     ATELIER_TRY(c.document.open_graph(*c.project, *key));
                 }
                 ATELIER_TRY_BIND(created_name, c.document.graph_add_node(*key, {p.type.c_str(), p.type.size()},
                                                                          {p.name.c_str(), p.name.size()},
                                                                          {p.position.x, p.position.y}, c.project));
                 if (!out.name.assign(StringView{*created_name}))
                     return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid UTF-8 node name."});
                 if (*key == c.active_graph || *key == c.network_graph)
                     host.select_graph_node(*key, *created_name);
                 return {};
             }),
         "editor.GraphAddNode",
         {},
         "editor.GraphNodeResult"});
    add({"graph.delete_node", L"Delete Graph Node", "Remove selected or named nodes from a procedural graph",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphDeleteNode& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(targets, resolve_graph_node_targets(c, *key, p.name, false, "delete"));
             const bool is_viewed = (*key == c.active_graph || *key == c.network_graph);
             ATELIER_TRY(c.document.graph_remove_nodes(*key, *targets));
             if (is_viewed && std::ranges::find(*targets, c.selected_graph_node) != targets->end())
                 host.select_graph_node(*key, {});
             return {};
         }),
         "editor.GraphDeleteNode"});
    add({"graph.duplicate_nodes",
         L"Duplicate Graph Nodes",
         "Duplicate selected or named nodes in a procedural graph while preserving properties and connections",
         OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphDuplicateNodes& p,
                             GraphNodeResult& out) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(targets, resolve_graph_node_targets(c, *key, p.node, false, "duplicate"));
             const bool is_viewed = (*key == c.active_graph || *key == c.network_graph);
             ATELIER_TRY_BIND(created, c.document.graph_duplicate_nodes(*key, *targets, {p.offset.x, p.offset.y}));
             if (!created->empty() && !out.name.assign(StringView{created->back()}))
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid UTF-8 node name."});
             if (is_viewed && !created->empty())
                 host.select_graph_nodes(*key, *created);
             return {};
         }),
         "editor.GraphDuplicateNodes",
         {},
         "editor.GraphNodeResult"});
    add({"graph.rename_node", L"Rename Graph Node", "Rename a node in a procedural graph", OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphRenameNode& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(from, resolve_graph_node_name(c, *key, p.node, "rename"));
             const bool is_viewed = (*key == c.active_graph || *key == c.network_graph);
             const std::string to(p.name.c_str(), p.name.size());
             ATELIER_TRY(c.document.graph_rename_node(*key, *from, to));
             if (is_viewed && c.selected_graph_node == *from)
                 host.select_graph_node(*key, to);
             return {};
         }),
         "editor.GraphRenameNode"});
    add({"graph.move_node", L"Move Graph Node", "Move a node on the procedural graph canvas", OperatorFlag::none,
         graph_editable, with_parameters(+[](OperatorHost&, const Context& c, const GraphMoveNode& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             const std::array moves{
                 std::pair{std::string(p.node.c_str(), p.node.size()), std::array{p.position.x, p.position.y}}};
             return c.document.graph_move_nodes(*key, moves);
         }),
         "editor.GraphMoveNode"});
    add({"graph.connect", L"Connect Graph Nodes", "Connect an upstream output port to a downstream input port",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphConnect& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_connect(
                 *key, {p.from_node.c_str(), p.from_node.size()}, {p.from_port.c_str(), p.from_port.size()},
                 {p.to_node.c_str(), p.to_node.size()}, {p.to_port.c_str(), p.to_port.size()});
         }),
         "editor.GraphConnect"});
    add({"graph.disconnect", L"Disconnect Graph Ports", "Disconnect input connections on a procedural graph node",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphDisconnect& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_disconnect(
                 *key, {p.to_node.c_str(), p.to_node.size()}, {p.to_port.c_str(), p.to_port.size()},
                 {p.from_node.c_str(), p.from_node.size()}, {p.from_port.c_str(), p.from_port.size()});
         }),
         "editor.GraphDisconnect"});
    add({"graph.set_display", L"Set Graph Display Node", "Set or clear the blue Display flag on a procedural graph",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetDisplay& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_set_display(*key, {p.node.c_str(), p.node.size()});
         }),
         "editor.GraphSetDisplay"});
    add({"graph.set_template", L"Set Graph Template Node",
         "Set or clear the magenta Template reference wireframe flag on a procedural graph", OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetTemplate& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_set_template(*key, {p.node.c_str(), p.node.size()});
         }),
         "editor.GraphSetTemplate"});
    add({"graph.set_bypass", L"Set Graph Node Bypass",
         "Enable or disable Bypass pass-through on a procedural graph node", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetBypass& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "bypass"));
             return c.document.graph_set_bypass(*key, *node_name, p.bypass);
         }),
         "editor.GraphSetBypass"});
    add({"graph.set_frozen", L"Set Graph Node Frozen",
         "Freeze or unfreeze a procedural graph node's cooked Geometry output", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetFrozen& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "freeze"));
             return c.document.graph_set_frozen(*key, *node_name, p.frozen, c.project);
         }),
         "editor.GraphSetFrozen"});
    add({"graph.set_appearance", L"Set Graph Node Appearance",
         "Change the shape preset, color swatch, or comment of procedural graph nodes", OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetAppearance& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(targets, resolve_graph_node_targets(c, *key, p.node, false, "style"));
             std::optional<GraphNodeShape> shape;
             if (!p.shape.empty()) {
                 const auto parsed = parse_graph_node_shape({p.shape.c_str(), p.shape.size()});
                 if (!parsed)
                     return std::unexpected(Error{ErrorCode::argument_mismatch, "Unknown node shape preset."});
                 shape = *parsed;
             }
             std::optional<std::optional<std::uint32_t>> color;
             if (!p.color.empty()) {
                 const std::string_view col_sv{p.color.c_str(), p.color.size()};
                 if (col_sv == "default" || col_sv == "auto") {
                     color = std::optional<std::uint32_t>{};
                 } else {
                     const auto parsed = parse_graph_hex_color(col_sv);
                     if (!parsed)
                         return std::unexpected(Error{ErrorCode::argument_mismatch, "Node color must be #RRGGBB."});
                     color = *parsed;
                 }
             }
             std::optional<std::string> comment;
             if (!p.comment.empty()) {
                 const std::string_view com_sv{p.comment.c_str(), p.comment.size()};
                 comment = com_sv == "-" ? std::string{} : std::string(com_sv);
             }
             return c.document.graph_set_node_appearance(*key, *targets, shape, color, comment);
         }),
         "editor.GraphSetAppearance"});
    add({"graph.select", L"Select Graph Node", "Inspect a procedural graph node without changing the Display flag",
         OperatorFlag::none, world_stopped,
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphSelect& p) -> Result<void> {
             ATELIER_TRY_BIND(key, target_graph_key(c, p.asset));
             const std::string node_name(p.node.c_str(), p.node.size());
             if (!node_name.empty() && !c.document.graph_node(*key, node_name))
                 return std::unexpected(
                     Error{ErrorCode::argument_mismatch, "No loaded node named '" + node_name + "' in this graph."});
             host.select_graph_node(*key, node_name);
             return {};
         }),
         "editor.GraphSelect"});
    add({"graph.layout", L"Auto Layout Graph", "Arrange procedural graph nodes into top-to-bottom topological ranks",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphLayout& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_auto_layout(*key);
         }),
         "editor.GraphLayout"});
    add({"graph.expose_parameter",
         L"Expose as Graph Parameter",
         "Promote a procedural node field or vector lane to a top-level Digital Asset parameter",
         OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphExposeParameter& p,
                             GraphNodeResult& out) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "expose a parameter from"));
             ATELIER_TRY_BIND(exposed,
                              c.document.graph_expose_parameter(
                                  *key, *node_name, {p.path.c_str(), p.path.size()}, {p.name.c_str(), p.name.size()},
                                  {p.label.c_str(), p.label.size()}, {p.category.c_str(), p.category.size()},
                                  {p.description.c_str(), p.description.size()}));
             if (!out.name.assign(StringView{*exposed}))
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid UTF-8 parameter name."});
             return {};
         }),
         "editor.GraphExposeParameter",
         {},
         "editor.GraphNodeResult"});
    add({"graph.bind_parameter", L"Bind to Graph Parameter",
         "Bind a procedural node field or vector lane as an additional target of an existing Graph Parameter",
         OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphBindParameter& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "bind"));
             return c.document.graph_bind_parameter_target(*key, {p.parameter.c_str(), p.parameter.size()}, *node_name,
                                                           {p.path.c_str(), p.path.size()});
         }),
         "editor.GraphBindParameter"});
    add({"graph.unbind_parameter", L"Unbind from Graph Parameter",
         "Unbind a procedural node field or vector lane from its controlling Graph Parameter", OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphUnbindParameter& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "unbind"));
             return c.document.graph_unbind_parameter_target(*key, *node_name, {p.path.c_str(), p.path.size()});
         }),
         "editor.GraphUnbindParameter"});
    add({"graph.update_parameter", L"Update Graph Parameter",
         "Rename, update metadata, or reorder a top-level Graph Parameter", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphUpdateParameter& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             const std::string param_name(p.parameter.c_str(), p.parameter.size());
             std::optional<std::string> new_name;
             if (!p.name.empty())
                 new_name.emplace(p.name.c_str(), p.name.size());
             const auto opt_field = [](const String& s) -> std::optional<std::string> {
                 if (s.empty())
                     return std::nullopt;
                 const std::string_view sv{s.c_str(), s.size()};
                 return sv == "-" ? std::string{} : std::string(sv);
             };
             std::optional<std::size_t> new_idx;
             if (p.move_delta != 0) {
                 const auto* doc = c.document.graph_document(*key);
                 if (!doc)
                     return std::unexpected(Error{ErrorCode::argument_mismatch, "Graph is not open."});
                 const auto it = std::ranges::find(doc->parameters, param_name, &GraphPromotedParameter::name);
                 if (it == doc->parameters.end())
                     return std::unexpected(Error{ErrorCode::argument_mismatch, "Unknown graph parameter."});
                 const auto cur = static_cast<std::ptrdiff_t>(it - doc->parameters.begin());
                 const auto last = static_cast<std::ptrdiff_t>(doc->parameters.size()) - 1;
                 new_idx = static_cast<std::size_t>(
                     std::clamp<std::ptrdiff_t>(cur + p.move_delta, 0, std::max<std::ptrdiff_t>(0, last)));
             }
             return c.document.graph_update_parameter(*key, param_name, std::move(new_name), opt_field(p.label),
                                                      opt_field(p.category), opt_field(p.description), new_idx);
         }),
         "editor.GraphUpdateParameter"});
    add({"graph.remove_parameter", L"Remove Graph Parameter",
         "Remove a top-level Graph Parameter and release all bound node targets", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphRemoveParameter& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             return c.document.graph_remove_parameter(*key, {p.parameter.c_str(), p.parameter.size()});
         }),
         "editor.GraphRemoveParameter"});
    add({"graph.set_parameter", L"Set Graph Parameter",
         "Set the value of a top-level Graph Parameter across all bound targets", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetParameter& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             const auto parsed = json::Document::parse({p.value_json.c_str(), p.value_json.size()}, {8});
             if (!parsed)
                 return std::unexpected(Error{ErrorCode::argument_mismatch, parsed.error().describe()});
             return c.document.graph_set_parameter_json(*key, {p.parameter.c_str(), p.parameter.size()},
                                                        parsed->root());
         }),
         "editor.GraphSetParameter"});
    add({"graph.set_override", L"Set AssetNode Override",
         "Set or clear a parameter override on a procedural.AssetNode instance", OperatorFlag::none, graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetOverride& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "override"));
             if (p.clear)
                 return c.document.graph_set_asset_override_json(
                     *key, *node_name, {p.parameter.c_str(), p.parameter.size()}, std::nullopt, c.project);
             const auto parsed = json::Document::parse({p.value_json.c_str(), p.value_json.size()}, {8});
             if (!parsed)
                 return std::unexpected(Error{ErrorCode::argument_mismatch, parsed.error().describe()});
             return c.document.graph_set_asset_override_json(
                 *key, *node_name, {p.parameter.c_str(), p.parameter.size()}, parsed->root(), c.project);
         }),
         "editor.GraphSetOverride"});
    add({"graph.set_node_asset", L"Assign Node Digital Asset",
         "Bind or switch the .procgraph Digital Asset referenced by a procedural.AssetNode", OperatorFlag::none,
         graph_editable,
         with_parameters(+[](OperatorHost&, const Context& c, const GraphSetNodeAsset& p) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(node_name, resolve_graph_node_name(c, *key, p.node, "assign a Digital Asset to"));
             return c.document.graph_set_node_asset(*key, *node_name, {p.target_asset.c_str(), p.target_asset.size()},
                                                    c.project);
         }),
         "editor.GraphSetNodeAsset"});
    add(path_operator<GraphExtractAsset>(
        {"graph.extract_asset",
         L"Extract Digital Asset…",
         "Extract selected graph nodes into a reusable .procgraph Digital Asset and replace them with a wired AssetNode",
         OperatorFlag::none,
         [graph_editable](const Context& c) -> Result<void> {
             ATELIER_TRY(project_open(c));
             return graph_editable(c);
         },
         with_parameters(+[](OperatorHost& host, const Context& c, const GraphExtractAsset& p,
                             GraphExtractAssetResult& out) -> Result<void> {
             ATELIER_TRY_BIND(key, editable_target_graph_key(c, p.asset));
             ATELIER_TRY_BIND(targets, resolve_graph_node_targets(c, *key, p.nodes, true, "extract"));
             const bool is_viewed = (*key == c.active_graph || *key == c.network_graph);
             ATELIER_TRY_BIND(chosen, operation_path(host, p.path, PathPurpose::procedural_graph_save,
                                                     c.project->root() / "Content", L"ExtractedAsset.procgraph"));
             if (!*chosen)
                 return {};
             auto relative = (**chosen).is_absolute() ? (**chosen).lexically_relative(c.project->root()) : **chosen;
             if (!assets::is_procedural_graph_path(relative))
                 relative.replace_extension(L".procgraph");
             ATELIER_TRY_BIND(extracted, c.document.graph_extract_asset(*c.project, *key, *targets, relative,
                                                                        {p.name.c_str(), p.name.size()}));
             if (!out.asset.assign(StringView{extracted->first}) || !out.node.assign(StringView{extracted->second}))
                 return std::unexpected(Error{ErrorCode::argument_mismatch, "Extracted identifiers require UTF-8."});
             if (c.interactive)
                 host.asset_created(extracted->first, false);
             if (is_viewed)
                 host.select_graph_node(*key, extracted->second);
             return {};
         }),
         "editor.GraphExtractAsset",
         {},
         "editor.GraphExtractAssetResult"}));
    const auto stopped_scene = [scene](const Context& c) -> Result<void> {
        ATELIER_TRY(scene(c));
        if (c.document.world().running())
            return unavailable("Stop the running World first.");
        return {};
    };
    static constexpr auto resolve_generator =
        +[](const Context& c, const ReferenceSlot& comp_slot, const ReferenceSlot* ent_slot) -> Result<ObjectId> {
        auto& doc = c.document;
        auto& runtime = doc.world().runtime();
        ATELIER_TRY_BIND(gen_type, runtime.native_type<presentation::ProceduralGeneratorComponent>());
        ATELIER_TRY_BIND(comp_ref, runtime.read_reference({*gen_type, ValueForm::weak_reference}, comp_slot));
        ATELIER_TRY_BIND(comp_held, runtime.try_acquire(*comp_ref));
        if (!comp_ref->is_null() && !*comp_held)
            return std::unexpected(
                Error{ErrorCode::invalid_object, "The target ProceduralGeneratorComponent is no longer alive."});
        ObjectId candidate = *comp_held ? (*comp_held)->id() : ObjectId{};
        if (candidate == ObjectId{} && ent_slot) {
            ATELIER_TRY_BIND(ent_ref,
                             runtime.read_reference({doc.types().entity, ValueForm::weak_reference}, *ent_slot));
            ATELIER_TRY_BIND(ent_held, runtime.try_acquire(*ent_ref));
            if (!ent_ref->is_null() && !*ent_held)
                return std::unexpected(Error{ErrorCode::invalid_object, "The target Entity is no longer alive."});
            if (*ent_held)
                candidate = (*ent_held)->id();
        }
        if (candidate == ObjectId{})
            candidate = c.selection.object != ObjectId{} ? c.selection.object : c.selection.active;
        return doc.resolve_procedural_generator(candidate);
    };
    add({"procedural.assign_graph", L"Assign Procedural Graph",
         "Bind or clear the .procgraph asset on a ProceduralGeneratorComponent", OperatorFlag::undo, stopped_scene,
         with_parameters(+[](OperatorHost& host, const Context& c, const AssignProceduralGraph& p) -> Result<void> {
             ATELIER_TRY_BIND(target, resolve_generator(c, p.component, nullptr));
             auto result = c.document.set_procedural_graph(c.project, *target, {p.asset.c_str(), p.asset.size()});
             host.scene_changed();
             return result;
         }),
         "editor.AssignProceduralGraph"});
    add({"procedural.set_override", L"Set Generator Parameter Override",
         "Set or clear a promoted parameter override on a Scene ProceduralGeneratorComponent", OperatorFlag::undo,
         stopped_scene,
         with_parameters(+[](OperatorHost& host, const Context& c, const ProceduralSetOverride& p) -> Result<void> {
             ATELIER_TRY_BIND(target, resolve_generator(c, p.component, &p.entity));
             Result<void> result;
             if (p.clear) {
                 result = c.document.set_generator_override_json(
                     c.project, *target, {p.parameter.c_str(), p.parameter.size()}, std::nullopt);
             } else {
                 const auto parsed = json::Document::parse({p.value_json.c_str(), p.value_json.size()}, {8});
                 if (!parsed)
                     return std::unexpected(Error{ErrorCode::argument_mismatch, parsed.error().describe()});
                 result = c.document.set_generator_override_json(
                     c.project, *target, {p.parameter.c_str(), p.parameter.size()}, parsed->root());
             }
             host.scene_changed();
             return result;
         }),
         "editor.ProceduralSetOverride"});
    add({"procedural.bake", L"Bake Procedural Generator",
         "Convert published CopyToPoints mesh instances of a Procedural Generator into StaticMeshEntity objects",
         OperatorFlag::undo, stopped_scene,
         with_parameters(+[](OperatorHost& host, const Context& c, const ProceduralBake& p) -> Result<void> {
             ATELIER_TRY_BIND(target, resolve_generator(c, p.component, &p.entity));
             auto result = committed(c.document.bake_procedural_generator(c.project, *target));
             host.scene_changed();
             return result;
         }),
         "editor.ProceduralBake"});
    ATELIER_TRY(registry.validate());
    return registry;
}
}
