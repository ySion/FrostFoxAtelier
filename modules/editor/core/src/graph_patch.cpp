#include "editor/session.hpp"
#include "editor/graph_format.hpp"
#include "procedural/graph.hpp"
#include <algorithm>
#include <cmath>

namespace atelier::editor {
namespace {
using namespace atelier::literals;

auto invalid(Utf8Text message) {
    return std::unexpected(Error{ErrorCode::argument_mismatch, std::move(message).into_string()});
}

auto at_path(Utf8Text path_text) {
    return [path = std::move(path_text).into_string()](Error error) {
        error.message = String::concat(path, ": ", error.message);
        return error;
    };
}

String port_choices(Span<const String> names) {
    if (names.empty())
        return " (none available)"_s;
    String list = String::concat(" (available: ", names.front());
    for (const auto& name : names.slice(1)) {
        list.append_literal<", ">();
        list += name;
    }
    list.append_ascii(')');
    return list;
}

struct NodeEdit {
    String name;

    String type;

    Optional<String> asset;

    Optional<std::array<float, 2>> position;

    Optional<json::Value> properties;

    Optional<json::Value> inputs;
};

struct PortEdit {
    String node;

    String port;

    Array<std::pair<StringView, StringView>> endpoints;
};

Result<NodeEdit> read_node(StringView name, json::Value value) {
    const String path = String::concat("nodes.", String::validated(name));
    if (!valid_graph_identifier(name))
        return invalid(String::concat(path, ": invalid node identifier"));
    if (auto fields = value.allowed_members({"type", "asset", "position", "properties", "inputs"}); !fields)
        return invalid(String::concat(path, ": ", fields.error().describe()));
    NodeEdit edit;
    edit.name = String::from_utf8_validated(name);
    if (const auto type = *value.find("type")) {
        if (type->kind() != json::Kind::string || type->string()->empty())
            return invalid(String::concat(path, ".type: expected a nonempty type name"));
        edit.type = String::from_utf8_validated(*type->string());
    }
    if (const auto asset = *value.find("asset")) {
        if (asset->kind() != json::Kind::string)
            return invalid(String::concat(path, ".asset: expected an asset key"));
        edit.asset = String::from_utf8_validated(*asset->string());
    }
    if (edit.type.starts_with("asset:")) {
        const String key = String::from_utf8_validated(edit.type.view().slice(6));
        if (key.empty() || (edit.asset && *edit.asset != key))
            return invalid(String::concat(path, ".asset: expected the asset named by type"));
        edit.type = "procedural.AssetNode"_s;
        edit.asset = key;
    }
    if (const auto position = *value.find("position")) {
        if (position->kind() != json::Kind::array || position->size() != 2)
            return invalid(String::concat(path, ".position: expected two finite coordinates"));
        std::array<float, 2> xy{};
        for (std::size_t i = 0; i < xy.size(); ++i) {
            const auto number = position->at(i)->number();
            if (!number || !std::isfinite(static_cast<float>(*number)))
                return invalid(String::concat(path, ".position: expected two finite coordinates"));
            xy[i] = static_cast<float>(*number);
        }
        edit.position = xy;
    }
    edit.inputs = *value.find("inputs");
    edit.properties = *value.find("properties");
    if (edit.inputs && edit.inputs->kind() != json::Kind::object)
        return invalid(String::concat(path, ".inputs: expected an object"));
    if (edit.properties && edit.properties->kind() != json::Kind::object)
        return invalid(String::concat(path, ".properties: expected an object"));
    return edit;
}

} // namespace

Result<void> Session::apply_graph_patch(StringView key, json::Value patch, Project* project) {
    if (auto fields = patch.allowed_members({"asset", "nodes", "remove", "settings"}); !fields)
        return invalid(fields.error().describe());
    Array<NodeEdit> nodes;
    if (const auto entries = *patch.find("nodes")) {
        if (entries->kind() != json::Kind::object)
            return invalid("nodes: expected an object keyed by node name");
        for (const auto& [name, value] : *entries->object_items()) {
            ATELIER_TRY_BIND(node, read_node(name, value));
            nodes.add(std::move(*node));
        }
    }
    Array<String> removed;
    if (const auto remove = *patch.find("remove")) {
        if (remove->kind() != json::Kind::array)
            return invalid("remove: expected an array of node names");
        for (const auto item : *remove->array()) {
            const auto name = item.string();
            if (!name || !graph_node(key, *name) || std::ranges::find(removed, *name) != removed.end() ||
                std::ranges::find(nodes, *name, &NodeEdit::name) != nodes.end())
                return invalid("remove: expected distinct existing nodes outside nodes");
            removed.add(String::from_utf8_validated(*name));
        }
    }
    Optional<StringView> display;

    json::Document root_properties;
    if (const auto settings = *patch.find("settings")) {
        if (settings->kind() != json::Kind::object)
            return invalid("settings: expected an object");
        json::Builder fields(*settings);
        fields.erase(fields.root(), "display");
        root_properties = fields.finish();
        if (const auto field = *settings->find("display")) {
            if (field->kind() != json::Kind::string)
                return invalid("settings.display: expected a node name or an empty string");
            display = *field->string();
        }
    }
    const auto will_exist = [&](StringView name) {
        return std::ranges::find(removed, name) == removed.end() &&
               (graph_node(key, name) || std::ranges::find(nodes, name, &NodeEdit::name) != nodes.end());
    };
    if (display && !display->empty() && !will_exist(*display))
        return invalid(String::concat("settings.display: unknown output node '", String::validated(*display), "'"));

    if (root_properties.root().size())
        ATELIER_TRY(graph_set_properties(key, {}, root_properties.root()).transform_error(at_path("settings")));

    // All named nodes are available before resolving any input, irrespective of JSON member order.
    Array<std::pair<String, std::array<float, 2>>> moves;
    for (const auto& edit : nodes) {
        const String path = String::concat("nodes.", edit.name);

        const auto* node = graph_node(key, edit.name);
        if (!node) {
            if (edit.type.empty())
                return invalid(String::concat(path, ".type: required for a new node"));
            ATELIER_TRY(
                graph_add_node(key, edit.type, edit.name, edit.position.value_or(std::array<float, 2>{}), project)
                    .transform_error(at_path(String::concat(path, ".type"))));
        } else {
            if (!node->node.registered())
                return invalid(String::concat(path, ": unresolved node"));
            if (!edit.type.empty()) {
                ATELIER_TRY_BIND(type, runtime_.type_of(node->node.id()));
                ATELIER_TRY_BIND(info, runtime_.describe(*type));
                if ((*info)->name != edit.type && (*info)->name != String::concat("procedural.", edit.type))
                    return invalid(String::concat(path, ".type: an existing node cannot change type"));
            }
            if (edit.position && node->position != *edit.position)
                moves.emplace(edit.name, *edit.position);
        }
        if (edit.asset)
            ATELIER_TRY(graph_set_node_asset(key, edit.name, *edit.asset, project)
                            .transform_error(at_path(String::concat(path, ".asset"))));
    }

    ATELIER_TRY_BIND(contents, procedural::contents(runtime_, loaded_graph(key)));
    Array<PortEdit> wires;
    for (const auto& edit : nodes) {
        if (!edit.inputs || !edit.inputs->size())
            continue;
        const auto* target = graph_node(key, edit.name);
        ATELIER_TRY_BIND(ports, procedural::ports(runtime_, target->node.id()));
        ATELIER_TRY_BIND(current, procedural::inputs(runtime_, *contents, target->node.id()));
        for (const auto& [name, value] : *edit.inputs->object_items()) {
            const String path = String::concat("nodes.", edit.name, ".inputs.", String::validated(name));

            const auto it = std::ranges::find(ports->inputs, name);
            if (it == ports->inputs.end())
                return invalid(String::concat(path, ": unknown input port", port_choices(ports->inputs)));
            const auto index = static_cast<std::size_t>(it - ports->inputs.begin());

            PortEdit wire{edit.name, String::from_utf8_validated(name), {}};

            const auto connect = [&](json::Value endpoint) -> Result<void> {
                const auto ep = graph_endpoint(endpoint);
                if (!ep)
                    return invalid(String::concat(path, ": expected node.output"));
                const auto id = graph_node_id(key, ep->first);
                if (id == ObjectId{} || !will_exist(ep->first))
                    return invalid(
                        String::concat(path, ": missing upstream node '", String::validated(ep->first), "'"));
                ATELIER_TRY_BIND(upstream, procedural::ports(runtime_, id));
                if (std::ranges::find(upstream->outputs, ep->second) == upstream->outputs.end())
                    return invalid(String::concat(path, ": unknown output '", String::validated(*endpoint.string()),
                                                  "'", port_choices(upstream->outputs)));
                if (std::ranges::find(wire.endpoints, *ep) != wire.endpoints.end())
                    return invalid(
                        String::concat(path, ": duplicate endpoint '", String::validated(*endpoint.string()), "'"));
                wire.endpoints.add(*ep);
                return {};
            };
            if (value.kind() != json::Kind::null) {
                if (ports->multiple[index]) {
                    if (value.kind() != json::Kind::array)
                        return invalid(String::concat(path, ": expected an ordered endpoint array"));
                    for (const auto ep : *value.array())
                        ATELIER_TRY(connect(ep));
                } else {
                    ATELIER_TRY(connect(value));
                }
            }
            // Reapplying identical text keeps redo history and the node's evaluation cache.
            const auto kept = *target->kept.root().find("inputs");

            bool equal = !kept || kept->kind() != json::Kind::object || !*kept->find(name);

            std::size_t n = 0;
            for (const auto& edge : *current) {
                if (edge.input != index)
                    continue;
                if (!equal || n == wire.endpoints.size() || edge.from != graph_node_id(key, wire.endpoints[n].first)) {
                    equal = false;
                    break;
                }
                ATELIER_TRY_BIND(upstream, procedural::ports(runtime_, edge.from));
                equal = upstream->outputs[edge.output] == wire.endpoints[n++].second;
            }
            if (!equal || n != wire.endpoints.size())
                wires.add(std::move(wire));
        }
    }
    for (const auto& edit : nodes)
        if (edit.properties && edit.properties->size())
            ATELIER_TRY(graph_set_properties(key, edit.name, *edit.properties)
                            .transform_error(at_path(String::concat("nodes.", edit.name, ".properties"))));
    if (!moves.empty())
        ATELIER_TRY(graph_move_nodes(key, moves));
    // Detach all replaced ports before reconnecting. An acyclic final graph can reverse old edges.
    if (!removed.empty())
        ATELIER_TRY(graph_remove_nodes(key, removed));
    for (const auto& wire : wires)
        ATELIER_TRY(graph_disconnect(key, wire.node, wire.port)
                        .transform_error(at_path(String::concat("nodes.", wire.node, ".inputs.", wire.port))));
    for (const auto& wire : wires)
        for (const auto& [from, output] : wire.endpoints)
            ATELIER_TRY(graph_connect(key, from, output, wire.node, wire.port)
                            .transform_error(at_path(String::concat("nodes.", wire.node, ".inputs.", wire.port))));
    if (display)
        ATELIER_TRY(graph_set_display(key, *display).transform_error(at_path("settings.display")));
    return {};
}

} // namespace atelier::editor
