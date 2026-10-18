#include "editor/operators.hpp"
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace atelier::editor {
namespace {
std::unexpected<Error> unavailable(std::string reason) { return std::unexpected(Error{ErrorCode::unsupported_operation, std::move(reason)}); }
Result<void> available(const OperatorHost&, const Context&) { return {}; }
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
    auto items = context.document.world().render_items(); if (!items) return std::unexpected(items.error());
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
Result<void> frame_selected_poll(const OperatorHost&, const Context& context) {
    if (!context.scene_available || context.playing) return unavailable("Stop the project and load a scene before framing the selection.");
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
        [redo, empty](const OperatorHost& host, const Context& context) -> Result<void> {
            if (context.region == RegionKind::code) return host.source_history_available(redo) ? Result<void>{} : empty();
            if (!context.asset.empty()) return context.document.material_can_undo(context.asset, redo) ? Result<void>{} : empty();
            if (context.region == RegionKind::controls) return host.workbench_history_available(redo) ? Result<void>{} : empty();
            return (redo ? context.document.redo_count() : context.document.undo_count()) ? Result<void>{} : empty();
        },
        [redo](OperatorHost& host, const Context& context) -> Result<void> {
            if (context.region == RegionKind::code) return host.source_history(redo);
            if (!context.asset.empty()) return context.document.material_undo(context.asset, redo);
            if (context.region == RegionKind::controls) return host.workbench_history(redo);
            auto moved = committed(redo ? context.document.redo() : context.document.undo());
            host.scene_changed();
            return moved;
        }};
}
}
void register_builtin_operators(OperatorRegistry& registry) {
    const auto add = [&](OperatorInfo info) { if (auto added = registry.add(std::move(info)); !added) throw std::logic_error(added.error().message); };
    add({"view.frame_all", L"Frame All", "Frame every rendered Entity and spatial locator", OperatorFlag::none, available,
        [](OperatorHost& host, const Context& context) { return frame(host, context, false); }});
    add({"view.frame_selected", L"Focus Selection", "Frame the selected Entities and their subtrees", OperatorFlag::none, frame_selected_poll,
        [](OperatorHost& host, const Context& context) { return frame(host, context, true); }});
    add({"view.reset", L"Reset Camera", "Return the viewport camera to its default view", OperatorFlag::none, available,
        [](OperatorHost& host, const Context&) { auto camera = host.camera(); camera.reset(); return host.set_camera(camera); }});
    add({"layout.reset", L"Reset Layout", "Restore the default pane layout", OperatorFlag::none, available,
        [](OperatorHost& host, const Context&) { host.reset_layout(); return Result<void>{}; }});
    add(history(false));
    add(history(true));
    add({"scene.select", L"Select", "Make the context selection the document selection; a component can be inspected only with one Entity selected",
        OperatorFlag::none, available,
        [](OperatorHost& host, const Context& context) -> Result<void> {
            const auto& selection = context.selection;
            if (selection.object != selection.active && selection.entities.size() != 1)
                return std::unexpected(Error{ErrorCode::unsupported_operation, "Inspect a component with only its Entity selected."});
            if (auto selected = context.document.select_entities(selection.entities, selection.active); !selected) return selected;
            if (selection.object != selection.active) if (auto inspected = context.document.select(selection.object); !inspected) return inspected;
            host.scene_changed();
            return {};
        }});
}
}
