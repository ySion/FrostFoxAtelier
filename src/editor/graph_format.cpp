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
using assets::json_format::Json;
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
std::optional<std::pair<std::string_view, std::string_view>> endpoint(const Json& value) {
    if (!value.is_string())
        return std::nullopt;
    const std::string_view text = value.get_ref<const std::string&>();
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
        assets::json_format::fields(file, {"version", "settings", "nodes"});
        if (!file.contains("version") || assets::json_format::identity(file.at("version")) != 1)
            malformed("Unsupported procedural graph version.");
        const auto settings = file.value("settings", Json::object());
        const auto entries = file.value("nodes", Json::object());
        if (!settings.is_object())
            malformed("Graph settings must be a record.");
        if (!entries.is_object())
            malformed("Graph nodes must be a record keyed by node name.");
        if (const auto at = settings.find("display");
            at != settings.end() && !(at->is_string() && identifier(at->get_ref<const std::string&>())))
            malformed("The graph display must name one node.");
        for (const auto& [name, entry] : entries.items()) {
            if (!identifier(name))
                malformed("Invalid node name: " + name);
            assets::json_format::fields(entry, {"type", "position", "properties", "inputs"});
            if (!entry.contains("type") || !entry.at("type").is_string())
                malformed(name + ": a node needs a type name.");
            if (entry.contains("position"))
                static_cast<void>(assets::json_format::vector<2>(entry.at("position")));
            if (entry.contains("properties") && !entry.at("properties").is_object())
                malformed(name + ": properties must be a record.");
            const auto inputs = entry.find("inputs");
            if (inputs == entry.end())
                continue;
            if (!inputs->is_object())
                malformed(name + ": inputs must be a record keyed by port name.");
            for (const auto& [port, value] : inputs->items()) {
                const bool shaped =
                    value.is_array()
                        ? std::ranges::all_of(value, [](const Json& item) { return endpoint(item).has_value(); })
                        : endpoint(value).has_value();
                if (!identifier(port) || !shaped)
                    malformed(name + ".inputs." + port + ": a connection is \"node.output\", or an array of those.");
            }
        }

        const auto graph_type = take(runtime.native_type<procedural::Graph>());
        GraphDocument document;
        document.graph = take(runtime.create(graph_type));
        const auto graph = document.graph.id();
        const auto report = [&](world::Severity severity, world::DiagnosticKind kind, ObjectId object, std::string type,
                                std::vector<std::string> field, const Json* value, const std::string& detail) {
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
        const auto keeping = [&](Json& kept, ObjectId object, const std::string& type,
                                 std::vector<std::string> prefix) {
            return PropertyReport{
                [&, object, type, prefix](std::vector<std::string> at, const Json& item, world::DiagnosticKind kind,
                                          const std::string& detail) {
                    Json* slot = &kept;
                    for (const auto& name : at)
                        slot = &(*slot)[name];
                    *slot = item;
                    auto field = prefix;
                    field.insert(field.end(), at.begin(), at.end());
                    report(world::Severity::warning, kind, object, type, std::move(field), &item, detail);
                },
                [&, object, type, prefix](std::vector<std::string> at, const Json& item) {
                    auto field = prefix;
                    field.insert(field.end(), at.begin(), at.end());
                    report(world::Severity::info, world::DiagnosticKind::converted, object, type, std::move(field),
                           &item, "Integral number converted exactly to the field's integer type.");
                }};
        };
        auto own = settings;
        own.erase("display");
        take(decode_properties(runtime, {graph_type, graph}, parameters(runtime, graph_type), own,
                               keeping(document.kept, graph, "procedural.Graph", {"settings"})));

        for (const auto& [name, entry] : entries.items()) {
            GraphNode item{name,
                           {},
                           entry.contains("position") ? assets::json_format::vector<2>(entry.at("position"))
                                                      : std::array<float, 2>{}};
            const auto& type_name = entry.at("type").get_ref<const std::string&>();
            const auto type = runtime.find_type(type_name);
            auto added =
                type ? procedural::add_node(runtime, graph, *type) : Result<ObjectRoot>{std::unexpected(type.error())};
            // A type that is missing or cannot be a node is the file's problem; any other failure is the Runtime's.
            if (!added && added.error().code != ErrorCode::invalid_type &&
                added.error().code != ErrorCode::invalid_definition)
                throw Failed{added.error()};
            if (!added) {
                item.kept = entry;
                report(world::Severity::warning, world::DiagnosticKind::unknown_type, {}, type_name, {"nodes", name},
                       nullptr, added.error().message + " The node is kept.");
            } else {
                item.node = std::move(*added);
                take(decode_properties(
                    runtime, {*type, item.node.id()}, parameters(runtime, *type),
                    entry.value("properties", Json::object()),
                    keeping(item.kept["properties"], item.node.id(), type_name, {"nodes", name, "properties"})));
                if (item.kept["properties"].empty())
                    item.kept.erase("properties");
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
            const auto& entry = entries.at(item.name);
            const auto inputs = entry.find("inputs");
            if (!item.node.registered() || inputs == entry.end())
                continue;
            const auto to = item.node.id();
            const auto type_name = take(runtime.describe(take(runtime.type_of(to))))->name;
            const auto ports = take(procedural::ports(runtime, take(runtime.type_of(to))));
            for (const auto& [port, value] : inputs->items()) {
                std::string problem;
                const auto at = std::ranges::find(ports.inputs, port);
                const auto input = static_cast<std::uint32_t>(at - ports.inputs.begin());
                if (at == ports.inputs.end())
                    problem = "The node has no input port " + port + ".";
                else if (ports.multiple[input] != value.is_array())
                    problem = ports.multiple[input] ? "The port takes an array of connections."
                                                    : "The port takes one connection.";
                else
                    for (const auto& connection : ports.multiple[input] ? value : Json::array({value})) {
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
                    }
                if (problem.empty())
                    continue;
                // A multiple port keeps its whole saved list, so its order survives the connections that did resolve.
                item.kept["inputs"][port] = value;
                report(world::Severity::warning, world::DiagnosticKind::reference_dropped, to, type_name,
                       {"nodes", item.name, "inputs", port}, &value, problem + " The saved connections are kept.");
            }
        }
        if (const auto at = settings.find("display"); at != settings.end()) {
            const auto* shown = named(at->get_ref<const std::string&>());
            if (shown && shown->node.registered())
                take(procedural::set_display(runtime, graph, shown->node.id()));
            else {
                document.kept["display"] = *at;
                report(world::Severity::warning, world::DiagnosticKind::reference_dropped, graph, "procedural.Graph",
                       {"settings", "display"}, &*at, "No loaded node has this name. The display is kept.");
            }
        }
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
            if (item.node.registered() ? !by_node.emplace(item.node.id(), &item).second : !item.kept.contains("type"))
                malformed(item.name + ": an entry names a loaded node once, or keeps a saved one.");
        }
        if (by_node.size() != contents.nodes.size() ||
            !std::ranges::all_of(contents.nodes, [&](ObjectId node) { return by_node.contains(node); }))
            malformed("Every graph node needs exactly one document entry.");
        const EncodeReference no_reference = [](ObjectId) -> Json {
            throw Failed{{ErrorCode::unsupported_operation, "Graph files keep no object references."}};
        };
        auto settings =
            take(encode_properties(runtime, graph, parameters(runtime, take(runtime.type_of(graph))), no_reference));
        if (contents.display != ObjectId{})
            settings["display"] = by_node.at(contents.display)->name;
        assets::json_format::overlay(settings, document.kept);
        Json nodes = Json::object();
        for (const auto& [name, item] : by_name) {
            Json entry = item->kept;
            if (item->node.registered()) {
                const auto node = item->node.id();
                const auto type = take(runtime.type_of(node));
                const auto ports = take(procedural::ports(runtime, type));
                Json inputs = Json::object();
                for (const auto& input : take(procedural::inputs(runtime, node))) {
                    const auto from = take(procedural::ports(runtime, take(runtime.type_of(input.from))));
                    auto connection = by_node.at(input.from)->name + "." + from.outputs.at(input.output);
                    auto& port = inputs[ports.inputs.at(input.input)];
                    if (ports.multiple[input.input])
                        port.push_back(std::move(connection));
                    else
                        port = std::move(connection);
                }
                entry = {
                    {"type", take(runtime.describe(type))->name},
                    {"properties", take(encode_properties(runtime, node, parameters(runtime, type), no_reference))},
                    {"inputs", std::move(inputs)}};
                assets::json_format::overlay(entry, item->kept);
            }
            entry["position"] = {item->position[0], item->position[1]};
            nodes[std::string{name}] = std::move(entry);
        }
        return Json{{"version", 1}, {"settings", std::move(settings)}, {"nodes", std::move(nodes)}}.dump(2) + "\n";
    } catch (const Failed& error) {
        return std::unexpected(error.error);
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::argument_mismatch, error.what()});
    }
}
}
