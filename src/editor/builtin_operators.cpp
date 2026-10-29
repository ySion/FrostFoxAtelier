#include "editor/operators.hpp"
#include "assets/assets.hpp"
#include "editor/project.hpp"
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace atelier::editor {
namespace {
std::unexpected<Error> unavailable(std::string reason) { return std::unexpected(Error{ErrorCode::unsupported_operation, std::move(reason)}); }
Result<void> available(const Context&) { return {}; }
// Adapts an operator over the reflected parameter record P to the type-erased entry.
template<class P> auto with_parameters(Result<void> (*execute)(OperatorHost&, const Context&, const P&)) {
    return [execute](OperatorHost& host, const Context& context, ValueInput input) -> Result<void> {
        const auto type = context.document.world().runtime().native_type<P>();
        if (!type || *type != input.type) return std::unexpected(Error{ErrorCode::type_mismatch, "Operator parameters have another type."});
        return execute(host, context, *static_cast<const P*>(input.data));
    };
}
Result<void> committed(Result<world::Commit> result) {
    if (!result) return std::unexpected(result.error());
    if (result->notification_error) return std::unexpected(*result->notification_error);
    return {};
}
// Blender View Selected / View All: bounds of the rendered geometry; locators count as points.
Result<void> frame(OperatorHost& host, const Context& context, bool selected_only) {
    auto& runtime = context.document.world().runtime();
    math::Vector3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}, high = -low;
    bool any{};
    const auto include = [&](math::Vector3 p) { for (int i = 0; i < 3; ++i) { low[i] = std::min(low[i], p[i]); high[i] = std::max(high[i], p[i]); } any = true; };
    const auto& selection = context.selection;
    const auto wanted = [&](ObjectId entity) {
        if (!selected_only) return true;
        while (entity != ObjectId{}) {
            if (entity == selection.active || std::ranges::find(selection.entities, entity) != selection.entities.end()) return true;
            const auto parent = world::parent_entity(runtime, entity); if (!parent) return false; entity = *parent;
        }
        return false;
    };
    auto items = context.document.presentation().render_items(); if (!items) return std::unexpected(items.error());
    std::vector<ObjectId> rendered;
    for (const auto& item : *items) {
        rendered.push_back(item.entity); if (!wanted(item.entity)) continue;
        for (const auto& vertex : item.geometry->vertices())
            include(math::transform_point(math::Vector3{vertex.position[0], vertex.position[1], vertex.position[2]}, item.world));
    }
    auto members = context.document.world().members(); if (!members) return std::unexpected(members.error());
    for (const auto& member : *members) {
        if (!wanted(member.id()) || std::ranges::find(rendered, member.id()) != rendered.end()) continue;
        auto spatial = world::has_transform(runtime, member.id()); if (!spatial || !*spatial) continue;
        auto matrix = world::world_matrix(runtime, member.id()); if (!matrix) continue;
        include({(*matrix)[12], (*matrix)[13], (*matrix)[14]});
    }
    if (!any) return std::unexpected(Error{ErrorCode::invalid_definition, selected_only ? "Select an Entity with a Transform to frame." : "The scene has nothing to frame."});
    auto camera = host.camera(); camera.frame((low + high) * .5f, math::length(high - low) * .5f);
    return host.set_camera(camera);
}
// Something in a selected subtree must have a Transform to frame.
Result<void> frame_selected_poll(const Context& context) {
    if (!context.scene_available) return unavailable("Load a scene before framing the selection.");
    if (context.selection.entities.empty()) return unavailable("Select an Entity with a Transform to frame.");
    auto& runtime = context.document.world().runtime();
    std::vector<ObjectId> pending = context.selection.entities;
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto object = pending.back(); pending.pop_back();
        if (!seen.insert(object).second) continue;
        const auto state = world::inspect_entity(runtime, object); if (!state || !state->can_edit) return unavailable("The selection cannot be edited.");
        const auto spatial = world::has_transform(runtime, object); if (!spatial) return std::unexpected(spatial.error());
        if (*spatial) return {};
        const auto children = world::children(runtime, object); if (!children) return std::unexpected(children.error());
        for (const auto& child : *children) pending.push_back(child.id());
    }
    return unavailable("Select an Entity with a Transform to frame.");
}
// The focused region owns the history: Code drafts, an inspected material asset, the Controls samples, else the scene.
OperatorInfo history(bool redo) {
    const auto empty = [redo] { return unavailable(redo ? "Nothing to redo." : "Nothing to undo."); };
    return {redo ? "edit.redo" : "edit.undo", redo ? L"Redo" : L"Undo",
        redo ? "Redo one step of the focused region's history" : "Undo one step of the focused region's history", OperatorFlag::history,
        [redo, empty](const Context& context) -> Result<void> {
            const auto can = [&](HistoryState history) { return (redo ? history.redo : history.undo) ? Result<void>{} : empty(); };
            if (context.region == RegionKind::code) return can(context.source_history);
            if (!context.asset.empty()) return context.document.material_can_undo(context.asset, redo) ? Result<void>{} : empty();
            if (context.region == RegionKind::controls) return can(context.workbench_history);
            return (redo ? context.document.redo_count() : context.document.undo_count()) ? Result<void>{} : empty();
        },
        [redo](OperatorHost& host, const Context& context, ValueInput) -> Result<void> {
            if (context.region == RegionKind::code) return host.source_history(redo);
            if (!context.asset.empty()) return context.document.material_undo(context.asset, redo, context.project);
            if (context.region == RegionKind::controls) return host.workbench_history(redo);
            auto moved = committed(redo ? context.document.redo(context.project) : context.document.undo(context.project));
            host.scene_changed();
            return moved;
        }};
}
// Scene edits address the context's active Entity.
Result<void> editable_entity(const Context& context) {
    if (!context.scene_available) return unavailable("Load a scene before editing it.");
    if (context.selection.active == ObjectId{}) return unavailable("Select an Entity.");
    const auto state = world::inspect_entity(context.document.world().runtime(), context.selection.active);
    if (!state || !state->can_edit) return unavailable("The selected Entity cannot be edited.");
    return {};
}
OperatorInfo transform_presence(bool add) {
    return {add ? "scene.transform_add" : "scene.transform_remove", add ? L"Add Transform" : L"Remove Transform",
        add ? "Give the active Entity an identity Transform" : "Remove the active Entity's Transform", OperatorFlag::undo,
        [add](const Context& context) -> Result<void> {
            if (auto editable = editable_entity(context); !editable) return editable;
            const auto spatial = world::has_transform(context.document.world().runtime(), context.selection.active);
            if (!spatial) return std::unexpected(spatial.error());
            if (*spatial == add) return unavailable(add ? "The Entity already has a Transform." : "The Entity has no Transform.");
            return {};
        },
        [add](OperatorHost&, const Context& context, ValueInput) {
            return add ? context.document.set_transform(context.selection.active, {}) : context.document.remove_transform(context.selection.active);
        }};
}
Result<void> docked(const Context& context) {
    return context.region == RegionKind::none ? unavailable("Focus a pane first.") : Result<void>{};
}
Result<void> add_poll(const Context& context) {
    if (!context.scene_available) return unavailable("Load a scene before adding to it.");
    const auto members = context.document.world().members();
    if (!members) return std::unexpected(members.error());
    if (members->size() >= Session::maximum_entities) return unavailable("The scene has reached its Entity limit.");
    return {};
}
Result<void> scene_add(OperatorHost& host, const Context& context, const SceneAdd& p) {
    auto& document = context.document;
    Result<world::Commit> added;
    switch (p.kind) {
    case AddKind::entity: added = document.add_entity(); break;
    case AddKind::mesh: {
        const auto catalog = assets::mesh_assets();
        std::string_view key{p.mesh.c_str(), p.mesh.size()};
        if (key.empty()) key = catalog.front().key;
        assets::Reader engine;
        auto& reader=context.project?context.project->assets():engine;
        auto asset=reader.mesh(key);
        if(!asset) return context.project?std::unexpected(asset.error()):
            std::unexpected(Error{ErrorCode::invalid_definition,"Unknown mesh asset."});
        added=document.create_static_mesh(*asset,context.cursor,context.project);
        break;
    }
    case AddKind::point: added = document.create_light(presentation::LightKind::point, context.cursor); break;
    case AddKind::rectangle: added = document.create_light(presentation::LightKind::rectangle, context.cursor); break;
    case AddKind::directional: added = document.create_light(presentation::LightKind::directional, context.cursor); break;
    case AddKind::camera: added = document.create_camera(context.cursor); break;
    }
    auto result = committed(std::move(added));
    host.scene_changed();
    return result;
}
Result<void> tool_settings(OperatorHost& host, const Context&, const ToolSettings& p) { host.set_tool_settings(p); return {}; }
Result<void> view_axis(OperatorHost& host, const Context&, const ViewAxis& p) {
    auto camera = host.camera();
    camera.set_view(p.view);
    return host.set_camera(camera);
}
}
void register_builtin_operators(OperatorRegistry& registry) {
    const auto add = [&](OperatorInfo info) { if (auto added = registry.add(std::move(info)); !added) throw std::logic_error(added.error().message); };
    add({"view.frame_all", L"Frame All", "Frame every rendered Entity and spatial locator", OperatorFlag::none, available,
        [](OperatorHost& host, const Context& context, ValueInput) { return frame(host, context, false); }});
    add({"view.frame_selected", L"Focus Selection", "Frame the selected Entities and their subtrees", OperatorFlag::none, frame_selected_poll,
        [](OperatorHost& host, const Context& context, ValueInput) { return frame(host, context, true); }});
    add({"view.reset", L"Reset Camera", "Return the viewport camera to its default view", OperatorFlag::none, available,
        [](OperatorHost& host, const Context&, ValueInput) { auto camera = host.camera(); camera.reset(); return host.set_camera(camera); }});
    add({"view.from_camera",L"View from Camera","Copy the selected scene camera's pose and projection into the editor viewport once",OperatorFlag::none,
        [](const Context& context)->Result<void> {
            if(auto editable=editable_entity(context);!editable) return editable;
            const auto camera=context.document.presentation().camera(context.selection.active);
            return camera?Result<void>{}:std::unexpected(camera.error());
        },
        [](OperatorHost& host,const Context& context,ValueInput)->Result<void> {
            auto snapshot=context.document.presentation().camera(context.selection.active);if(!snapshot) return std::unexpected(snapshot.error());
            auto camera=host.camera();
            if(!camera.view_from(*snapshot)) return unavailable("The camera view cannot be represented in the editor viewport.");
            return host.set_camera(camera);
        }});
    add({"scene.main_camera_set",L"Set as Main Camera","Select this Entity's camera for Play; disabled cameras may be selected",OperatorFlag::undo,
        [](const Context& context)->Result<void> {
            if(auto editable=editable_entity(context);!editable) return editable;
            if(context.document.world().running()) return unavailable("Stop the document before changing its main camera.");
            auto cameras=context.document.world().members(context.document.presentation().cameras());
            if(!cameras) return std::unexpected(cameras.error());
            if(std::ranges::count(*cameras,context.selection.active,&world::CapabilityMember::entity)!=1)
                return unavailable("Select an Entity with exactly one CameraComponent.");
            return {};
        },
        [](OperatorHost& host,const Context& context,ValueInput) {
            auto result=context.document.set_main_camera(context.selection.active);host.scene_changed();return result;
        }});
    add({"scene.main_camera_clear",L"Clear Main Camera","Clear the scene's explicit Play camera selection",OperatorFlag::undo,
        [](const Context& context)->Result<void> {
            if(!context.scene_available || context.document.world().running()) return unavailable("Open a stopped document first.");
            const auto environment=context.document.environment();
            auto selected=context.document.main_camera();if(!selected) return std::unexpected(selected.error());
            if(*selected==ObjectId{} && (environment==ObjectId{} || !context.document.has_retained(environment)))
                return unavailable("The scene has no main camera selection.");
            return {};
        },
        [](OperatorHost& host,const Context& context,ValueInput) {
            auto result=context.document.set_main_camera({});host.scene_changed();return result;
        }});
    add({"layout.reset", L"Reset Layout", "Restore the default pane layout", OperatorFlag::none, available,
        [](OperatorHost& host, const Context&, ValueInput) { host.reset_layout(); return Result<void>{}; }});
    add({"layout.close_tab", L"Close Pane Tab", "Close the focused region's pane tab; on the Code page, close its active document first",
        OperatorFlag::none, docked, [](OperatorHost& host, const Context& context, ValueInput) { return host.close_tab(context.region, context.interactive); }});
    add({"layout.maximize", L"Maximize / Restore Pane", "Maximize the focused region's pane, or restore the layout when it is maximized",
        OperatorFlag::none, docked, [](OperatorHost& host, const Context& context, ValueInput) { host.maximize_tab(context.region); return Result<void>{}; }});
    add(history(false));
    add(history(true));
    add(transform_presence(true));
    add(transform_presence(false));
    add({"scene.discard_saved_data", L"Discard Saved Data", "Drop the saved data kept for the inspected object's missing type, fields or resources",
        OperatorFlag::undo,
        [](const Context& context) -> Result<void> {
            if (!context.scene_available) return unavailable("Load a scene before editing it.");
            const auto object = context.selection.object;
            if (object == ObjectId{} || !context.document.has_retained(object)) return unavailable("The inspected object has no saved data.");
            return {};
        },
        [](OperatorHost&, const Context& context, ValueInput) { return context.document.discard_retained(context.selection.object); }});
    add({"scene.select", L"Select", "Make the context selection the document selection; a component can be inspected only with one Entity selected",
        OperatorFlag::none, available,
        [](OperatorHost& host, const Context& context, ValueInput) -> Result<void> {
            const auto& selection = context.selection;
            if (selection.object != selection.active && selection.entities.size() != 1)
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Inspect a component with only its Entity selected."});
            if (auto selected = context.document.select_entities(selection.entities, selection.active); !selected) return selected;
            if (selection.object != selection.active) if (auto inspected = context.document.select(selection.object); !inspected) return inspected;
            host.scene_changed();
            return {};
        }});
    add({"scene.add", L"Add", "Add an Entity, a static mesh, a light or a camera at the 3D cursor and select it", OperatorFlag::undo, add_poll,
        with_parameters(scene_add), "editor.SceneAdd"});
    add({"tool.settings", L"Tool Settings", "Change the viewport tool, selection mode and snapping; unnamed fields keep their values",
        OperatorFlag::none, available, with_parameters(tool_settings), "editor.ToolSettings",
        [](const Context& context, ValueOutput out) -> Result<void> {
            const auto type = context.document.world().runtime().native_type<ToolSettings>();
            if (!type || *type != out.type) return std::unexpected(Error{ErrorCode::type_mismatch, "Operator parameters have another type."});
            *static_cast<ToolSettings*>(out.data) = context.tools;
            return {};
        }});
    add({"view.axis", L"View", "Switch the viewport camera to perspective or an axis view around the same target", OperatorFlag::none,
        available, with_parameters(view_axis), "editor.ViewAxis"});
}
}
