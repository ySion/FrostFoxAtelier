#include "editor/graph_format.hpp"
#include "editor/fields.hpp"
#include "editor/properties.hpp"
#include "procedural/graph.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_map>

namespace atelier::editor {
namespace {
struct Failed {
    Error error;
};

template <class T>
T take(Result<T> value) {
    if (!value)
        throw Failed{value.error()};
    return std::move(*value);
}

void take(Result<void> value) {
    if (!value)
        throw Failed{value.error()};
}

[[noreturn]] void malformed(std::string message) {
    throw Failed{{ErrorCode::argument_mismatch, std::move(message)}};
}

bool identifier(std::string_view text) {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    return !text.empty() && letter(text.front()) &&
           std::ranges::all_of(text.substr(1), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

// "node.output", or nothing when malformed.
std::optional<std::pair<std::string_view, std::string_view>> endpoint(json::Value value) {
    if (value.kind() != json::Kind::string)
        return std::nullopt;
    const std::string_view text = *value.string();
    const auto dot = text.find('.');
    if (dot == text.npos || !identifier(text.substr(0, dot)) || !identifier(text.substr(dot + 1)))
        return std::nullopt;
    return std::pair{text.substr(0, dot), text.substr(dot + 1)};
}

std::string dotted(std::span<const std::string> path) {
    std::string text;
    for (const auto& name : path)
        text += (text.empty() ? "" : ".") + name;
    return text;
}

// The parameters of a graph or node type: its public fields. The graph structure (a node's
// connections, a graph's nodes and display) lives in hidden fields, which are not among them.
std::vector<detail::ScalarField> parameters(Runtime& runtime, TypeId type) {
    return take(detail::author_fields(runtime, type));
}
}

Result<GraphDocument> load_graph(Runtime& runtime, std::string_view text) {
    try {
        const auto file = assets::json_format::parse(text);
        if (!file)
            return std::unexpected(Error{ErrorCode::argument_mismatch, file.error().describe()});
        const auto root = file->root();
        if (auto fields = root.allowed_members({"version", "settings", "nodes"}); !fields)
            return std::unexpected(Error{ErrorCode::argument_mismatch, fields.error().describe()});
        const auto version = root.u64({"version"});
        if (!version || *version != 1)
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Unsupported procedural graph version."});
        const json::Document empty;
        const auto settings = root.find("settings")->value_or(empty.root());
        const auto entries = root.find("nodes")->value_or(empty.root());
        // Validate the complete file before creating Runtime graph objects.
        const auto validated = [&]() -> json::Result<void> {
            if (settings.kind() != json::Kind::object)
                return std::unexpected(settings.error(json::Fault::type, "Graph settings must be a record."));
            if (entries.kind() != json::Kind::object)
                return std::unexpected(
                    entries.error(json::Fault::type, "Graph nodes must be a record keyed by node name."));
            if (const auto display = *settings.find("display")) {
                ATELIER_TRY_BIND(name, display->string());
                if (!identifier(*name))
                    return std::unexpected(display->error(json::Fault::range, "The graph display must name one node."));
            }
            const auto members = entries.object_items();
            for (auto [name, entry] : *members) {
                if (!identifier(name))
                    return std::unexpected(entry.error(json::Fault::range, "Invalid node name."));
                ATELIER_TRY(entry.allowed_members({"type", "position", "properties", "inputs"}));
                ATELIER_TRY(entry.string({"type"}));
                if (const auto position = *entry.find("position")) {
                    ATELIER_TRY(assets::json_format::vector<2>(*position));
                }
                if (const auto properties = *entry.find("properties");
                    properties && properties->kind() != json::Kind::object)
                    return std::unexpected(properties->error(json::Fault::type, "Properties must be a record."));
                const auto inputs = *entry.find("inputs");
                if (!inputs)
                    continue;
                ATELIER_TRY_BIND(ports, inputs->object_items());
                for (auto [port, value] : *ports) {
                    bool shaped = identifier(port);
                    if (value.kind() == json::Kind::array) {
                        const auto connections = *value.array();
                        for (auto connection : connections)
                            shaped &= endpoint(connection).has_value();
                    } else
                        shaped &= endpoint(value).has_value();
                    if (!shaped)
                        return std::unexpected(
                            value.error(json::Fault::type, "A connection is \"node.output\", or an array of those."));
                }
            }
            return {};
        }();
        if (!validated)
            return std::unexpected(Error{ErrorCode::argument_mismatch, validated.error().describe()});
        const auto members = entries.object_items();

        const auto graph_type = take(runtime.native_type<procedural::Graph>());
        GraphDocument document;
        json::Builder kept_settings;
        document.graph = take(runtime.create(graph_type));
        const auto graph = document.graph.id();
        const auto report = [&](world::Severity severity, world::DiagnosticKind kind, ObjectId object, std::string type,
                                std::vector<std::string> field, const json::Value* value, const std::string& detail) {
            world::Diagnostic item;
            item.serial = document.issues.size() + 1;
            item.severity = severity;
            item.kind = kind;
            item.phase = world::DiagnosticPhase::load;
            item.object = object;
            item.type = std::move(type);
            if (value)
                item.value = assets::json_format::preview(*value);
            item.message = dotted(field) + ": " + detail;
            item.field = std::move(field);
            document.issues.push_back(std::move(item));
        };
        // Leaves that do not decode are kept at their path under `kept` and reported under `prefix`.
        const auto keeping = [&](json::Builder& kept, json::Value target, ObjectId object, const std::string& type,
                                 std::vector<std::string> prefix) {
            return PropertyReport{
                [&, target, object, type, prefix](std::vector<std::string> at, json::Value item,
                                                  world::DiagnosticKind kind, const std::string& detail) {
                    const auto slot = kept.object_path_validated(target, at);
                    kept.replace(slot, item);
                    auto field = prefix;
                    field.insert(field.end(), at.begin(), at.end());
                    report(world::Severity::warning, kind, object, type, std::move(field), &item, detail);
                },
                [&, object, type, prefix](std::vector<std::string> at, json::Value item) {
                    auto field = prefix;
                    field.insert(field.end(), at.begin(), at.end());
                    report(world::Severity::info, world::DiagnosticKind::converted, object, type, std::move(field),
                           &item, "Integral number converted exactly to the field's integer type.");
                }};
        };
        json::Builder own(settings);
        own.erase(own.root(), "display");
        take(decode_properties(runtime, {graph_type, graph}, parameters(runtime, graph_type), own.root(),
                               keeping(kept_settings, kept_settings.root(), graph, "procedural.Graph", {"settings"})));

        for (const auto& [key, entry] : *members) {
            const std::string name(key);
            const auto position = *entry.find("position");
            GraphNode item{name, {}, position ? *assets::json_format::vector<2>(*position) : std::array<float, 2>{}};
            const std::string type_name(*entry.string({"type"}));
            const auto type = runtime.find_type(type_name);
            auto added =
                type ? procedural::add_node(runtime, graph, *type) : Result<ObjectRoot>{std::unexpected(type.error())};
            // A type that is missing or cannot be a node is the file's problem; any other failure is the Runtime's.
            if (!added && added.error().code != ErrorCode::invalid_type &&
                added.error().code != ErrorCode::invalid_definition)
                throw Failed{added.error()};
            if (!added) {
                item.kept = json::Document(entry);
                report(world::Severity::warning, world::DiagnosticKind::unknown_type, {}, type_name, {"nodes", name},
                       nullptr, added.error().message + " The node is kept.");
            } else {
                item.node = std::move(*added);
                json::Builder kept;
                const auto properties = kept.object_validated(kept.root(), "properties");
                take(decode_properties(
                    runtime, {*type, item.node.id()}, parameters(runtime, *type),
                    entry.find("properties")->value_or(empty.root()),
                    keeping(kept, properties, item.node.id(), type_name, {"nodes", name, "properties"})));
                if (!properties.size())
                    kept.erase(kept.root(), "properties");
                item.kept = kept.finish();
            }
            document.nodes.push_back(std::move(item));
        }

        // Nodes are in name order, as in the file.
        const auto named = [&](std::string_view name) -> GraphNode* {
            const auto at = std::ranges::lower_bound(
                document.nodes, name, {}, [](const GraphNode& node) -> std::string_view { return node.name; });
            return at != document.nodes.end() && at->name == name ? &*at : nullptr;
        };
        for (auto& item : document.nodes) {
            const auto entry = *entries.at(item.name);
            const auto inputs = *entry.find("inputs");
            if (!item.node.registered() || !inputs)
                continue;
            const auto to = item.node.id();
            const auto type_name = take(runtime.describe(take(runtime.type_of(to))))->name;
            const auto ports = take(procedural::ports(runtime, take(runtime.type_of(to))));
            json::Builder kept(item.kept);
            const auto saved_inputs = inputs->object_items();
            for (const auto& [port_name, value] : *saved_inputs) {
                const std::string port(port_name);
                std::string problem;
                const auto at = std::ranges::find(ports.inputs, port);
                const auto input = static_cast<std::uint32_t>(at - ports.inputs.begin());
                if (at == ports.inputs.end())
                    problem = "The node has no input port " + port + ".";
                else if (ports.multiple[input] != (value.kind() == json::Kind::array))
                    problem = ports.multiple[input] ? "The port takes an array of connections."
                                                    : "The port takes one connection.";
                else {
                    const auto connect = [&](json::Value connection) {
                        const auto [from, output] = *endpoint(connection);
                        const auto* source = named(from);
                        std::string failed;
                        if (!source)
                            failed = "No node is named " + std::string{from} + ".";
                        else if (!source->node.registered())
                            failed = "Node " + std::string{from} + " is kept, not loaded.";
                        else {
                            const auto outputs =
                                take(procedural::ports(runtime, take(runtime.type_of(source->node.id())))).outputs;
                            const auto out = std::ranges::find(outputs, output);
                            if (out == outputs.end())
                                failed = "Node " + std::string{from} + " has no output " + std::string{output} + ".";
                            else if (auto made = procedural::connect(runtime, graph, source->node.id(),
                                                                     static_cast<std::uint32_t>(out - outputs.begin()),
                                                                     to, input);
                                     !made) {
                                // connect rejects a connection that would close a cycle; other failures are the Runtime's.
                                if (made.error().code != ErrorCode::argument_mismatch)
                                    throw Failed{made.error()};
                                failed = made.error().message;
                            }
                        }
                        if (problem.empty())
                            problem = std::move(failed);
                    };
                    if (ports.multiple[input]) {
                        const auto connections = *value.array();
                        for (auto connection : connections)
                            connect(connection);
                    } else
                        connect(value);
                }
                if (problem.empty())
                    continue;
                // A multiple port keeps its whole saved list, so its order survives the connections that did resolve.
                const std::array<std::string, 1> path{"inputs"};
                kept.set_validated(kept.object_path_validated(kept.root(), path), port, value);
                report(world::Severity::warning, world::DiagnosticKind::reference_dropped, to, type_name,
                       {"nodes", item.name, "inputs", port}, &value, problem + " The saved connections are kept.");
            }
            item.kept = kept.finish();
        }
        if (const auto at = *settings.find("display")) {
            const auto* shown = named(*at->string());
            if (shown && shown->node.registered())
                take(procedural::set_display(runtime, graph, shown->node.id()));
            else {
                kept_settings.set_validated(kept_settings.root(), "display", *at);
                report(world::Severity::warning, world::DiagnosticKind::reference_dropped, graph, "procedural.Graph",
                       {"settings", "display"}, &*at, "No loaded node has this name. The display is kept.");
            }
        }
        document.kept = kept_settings.finish();
        return document;
    } catch (const Failed& error) {
        return std::unexpected(error.error);
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::argument_mismatch, error.what()});
    }
}

Result<std::string> save_graph(Runtime& runtime, const GraphDocument& document) {
    try {
        const auto graph = document.graph.id();
        const auto contents = take(procedural::contents(runtime, graph));
        std::map<std::string_view, const GraphNode*> by_name;
        std::unordered_map<ObjectId, const GraphNode*> by_node;
        for (const auto& item : document.nodes) {
            if (!identifier(item.name) || !by_name.emplace(item.name, &item).second)
                malformed("Graph node names must be valid and unique: " + item.name);
            if (!std::isfinite(item.position[0]) || !std::isfinite(item.position[1]))
                malformed(item.name + ": node positions must be finite.");
            const auto saved_type = item.kept.root().find("type");
            if (item.node.registered() ? !by_node.emplace(item.node.id(), &item).second : !saved_type || !*saved_type)
                malformed(item.name + ": an entry names a loaded node once, or keeps a saved one.");
        }
        if (by_node.size() != contents.nodes.size() ||
            !std::ranges::all_of(contents.nodes, [&](ObjectId node) { return by_node.contains(node); }))
            malformed("Every graph node needs exactly one document entry.");
        const EncodeReference no_reference = [](ObjectId) -> Result<json::Document> {
            throw Failed{{ErrorCode::unsupported_operation, "Graph files keep no object references."}};
        };
        auto settings =
            take(encode_properties(runtime, graph, parameters(runtime, take(runtime.type_of(graph))), no_reference));
        json::Builder result;
        static_cast<void>(result.set(result.root(), "version", std::uint32_t{1}));
        result.set_validated(result.root(), "settings", settings.root());
        const auto settings_node = *result.root().at("settings");
        if (contents.display != ObjectId{})
            result.set_validated(settings_node, "display", by_node.at(contents.display)->name);
        assets::json_format::overlay(result, settings_node, document.kept.root());
        const auto nodes = result.object_validated(result.root(), "nodes");
        for (const auto& [name, item] : by_name) {
            const auto entry = result.object_validated(nodes, name);
            if (item->node.registered()) {
                const auto node = item->node.id();
                const auto type = take(runtime.type_of(node));
                const auto ports = take(procedural::ports(runtime, type));
                const auto inputs = result.object_validated(entry, "inputs");
                for (const auto& input : take(procedural::inputs(runtime, node))) {
                    const auto from = take(procedural::ports(runtime, take(runtime.type_of(input.from))));
                    auto connection = by_node.at(input.from)->name + "." + from.outputs.at(input.output);
                    const auto& port_name = ports.inputs.at(input.input);
                    if (ports.multiple[input.input]) {
                        const auto found = *inputs.find(port_name);
                        const auto port = found ? *found : *result.array(inputs, port_name);
                        if (auto added = result.append(port, connection); !added)
                            return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
                    } else if (auto added = result.set(inputs, port_name, connection); !added)
                        return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
                }
                const auto properties = take(encode_properties(runtime, node, parameters(runtime, type), no_reference));
                result.set_validated(entry, "type", take(runtime.describe(type))->name);
                result.set_validated(entry, "properties", properties.root());
            }
            assets::json_format::overlay(result, entry, item->kept.root());
            const auto position = *result.array(entry, "position");
            for (auto coordinate : item->position) {
                if (auto added = result.append(position, coordinate); !added)
                    return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
            }
        }
        const auto bytes = json::write(result.root(), {true, true});
        return std::string(bytes.data(), bytes.size());
    } catch (const Failed& error) {
        return std::unexpected(error.error);
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::argument_mismatch, error.what()});
    }
}
}
