#include "procedural/graph.hpp"
#include "procedural/nodes.hpp"
#include "runtime/foundation.hpp"

#include <algorithm>
#include <unordered_set>

namespace atelier::procedural {
namespace detail {
struct Access {
    template <class N>
    static auto& inputs(N& node) {
        return node.inputs_;
    }

    template <class N>
    static auto& connections(N& node) {
        return node.connections_;
    }

    template <class G>
    static auto& nodes(G& graph) {
        return graph.nodes_;
    }

    template <class G>
    static auto& display(G& graph) {
        return graph.display_;
    }
};
}

namespace {
using detail::Access;

std::unexpected<Error> error(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

Result<bool> member(Runtime& runtime, ObjectId graph, ObjectId node) {
    auto listed = contents(runtime, graph);
    if (!listed)
        return std::unexpected(listed.error());
    return std::ranges::find(listed->nodes, node) != listed->nodes.end();
}

// True when `target` is `from` or feeds it through any path of connections.
Result<bool> upstream(Runtime& runtime, ObjectId from, ObjectId target) {
    std::vector<ObjectId> pending{from};
    std::unordered_set<ObjectId> seen;
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        if (node == target)
            return true;
        if (!seen.insert(node).second)
            continue;
        auto into = inputs(runtime, node);
        if (!into)
            return std::unexpected(into.error());
        for (const auto& input : *into)
            pending.push_back(input.from);
    }
    return false;
}

}

Result<Reference> Context::mesh(StringView key) const {
    if (!resources_)
        return error(ErrorCode::unsupported_operation, "This evaluation provides no resources.");
    auto mesh = resources_->mesh(key);
    if (!mesh)
        return std::unexpected(mesh.error());
    auto name = String::from_utf8(key);
    if (!name)
        return failure("Mesh resource key requires valid UTF-8.");
    Reference reference{std::move(*name), std::move(*mesh)};
    if (std::ranges::find(*dependencies_, reference.key, &Reference::key) == dependencies_->end())
        dependencies_->push_back(reference);
    return reference;
}

Result<void> register_types(Runtime& runtime) {
    if (runtime.native_type<Graph>())
        return {};
    std::vector<TypeDefinition> definitions;
    if (!runtime.native_type<String>())
        definitions.push_back(string_definition(runtime));
    if (!runtime.native_type<Array<String>>())
        definitions.push_back(array_definition<String>(runtime, "procedural.Strings"));
    // Every point column type, so Source nodes can borrow columns.
    const auto column = [&]<class T>(std::type_identity<T>, const char* name) {
        if (!runtime.native_type<Array<T>>())
            definitions.push_back(array_definition<T>(runtime, name));
    };
    column(std::type_identity<bool>{}, "procedural.Bools");
    column(std::type_identity<std::int32_t>{}, "procedural.Ints");
    column(std::type_identity<std::uint32_t>{}, "procedural.Uints");
    column(std::type_identity<float>{}, "procedural.Floats");
    column(std::type_identity<math::Vector2>{}, "procedural.Vector2s");
    column(std::type_identity<math::Vector3>{}, "procedural.Vector3s");
    column(std::type_identity<math::Vector4>{}, "procedural.Vector4s");
    column(std::type_identity<math::Quaternion>{}, "procedural.Quaternions");
    column(std::type_identity<math::Matrix4>{}, "procedural.Matrix4s");
    definitions.push_back(reflection::Definition<Layer>::build(runtime));
    definitions.push_back(reflection::Definition<Reduce>::build(runtime));
    definitions.push_back(reflection::Definition<Geometry>::build(runtime));
    definitions.push_back(array_definition<Geometry>(runtime, "procedural.Geometries"));
    definitions.push_back(reflection::Definition<Context>::build(runtime));
    definitions.push_back(reflection::Definition<Node>::build(runtime));
    definitions.push_back(reflection::Definition<PointNode>::build(runtime));
    definitions.push_back(reflection::Definition<Graph>::build(runtime));
    definitions.push_back(reflection::Definition<MeshImport>::build(runtime));
    definitions.push_back(reflection::Definition<Grid>::build(runtime));
    definitions.push_back(reflection::Definition<Line>::build(runtime));
    definitions.push_back(reflection::Definition<Box>::build(runtime));
    definitions.push_back(reflection::Definition<Sphere>::build(runtime));
    definitions.push_back(reflection::Definition<Tube>::build(runtime));
    definitions.push_back(reflection::Definition<Polyline>::build(runtime));
    definitions.push_back(reflection::Definition<GroupBox>::build(runtime));
    definitions.push_back(reflection::Definition<Scatter>::build(runtime));
    definitions.push_back(reflection::Definition<VolumeScatter>::build(runtime));
    definitions.push_back(reflection::Definition<Transform>::build(runtime));
    definitions.push_back(reflection::Definition<Promote>::build(runtime));
    definitions.push_back(reflection::Definition<Remove>::build(runtime));
    definitions.push_back(reflection::Definition<Merge>::build(runtime));
    definitions.push_back(reflection::Definition<RayMode>::build(runtime));
    definitions.push_back(reflection::Definition<Ray>::build(runtime));
    definitions.push_back(reflection::Definition<CopyToPoints>::build(runtime));
    definitions.push_back(reflection::Definition<Realize>::build(runtime));
    auto published = runtime.register_types(std::move(definitions));
    if (!published)
        return std::unexpected(published.error());
    return {};
}

Result<Ports> ports(Runtime& runtime, TypeId type) {
    auto node = runtime.native_type<Node>();
    if (!node)
        return std::unexpected(node.error());
    auto derived = runtime.is_a(type, *node);
    if (!derived)
        return std::unexpected(derived.error());
    auto info = runtime.describe(type);
    if (!info)
        return std::unexpected(info.error());
    const auto& name = (*info)->name;
    if (!*derived)
        return error(ErrorCode::invalid_type, name + " is not a procedural.Node.");
    auto point = runtime.native_type<PointNode>();
    if (!point)
        return std::unexpected(point.error());
    auto attribute = runtime.is_a(type, *point);
    if (!attribute)
        return std::unexpected(attribute.error());
    if (*attribute) {
        if (type == *point)
            return error(ErrorCode::invalid_type, "procedural.PointNode is the base of Source nodes, not a node.");
        return Ports{{}, {}, {"input"}, {false}, {"output"}};
    }
    auto method = runtime.find_method(type, "evaluate");
    if (!method)
        return error(ErrorCode::invalid_definition, "Node type " + name + " has no evaluate method.");
    const auto found = std::ranges::find((*info)->methods, *method, &MethodInfo::id);
    const auto& signature = found->signature;
    if (!signature.receiver || !signature.receiver->read_only || signature.result ||
        signature.execution != FunctionExecution::synchronous)
        return error(ErrorCode::invalid_definition,
                     name + ".evaluate must be a const synchronous method without a result.");
    auto context = runtime.native_type<Context>();
    auto geometry = runtime.native_type<Geometry>();
    auto geometries = runtime.native_type<Geometries>();
    if (!context || !geometry || !geometries)
        return error(ErrorCode::invalid_type, "Register the procedural types first.");
    Ports result;
    result.evaluate = *method;
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        const auto& parameter = signature.parameters[i];
        const bool inline_value = parameter.form == ValueForm::inline_value;
        const bool read = inline_value && parameter.passing == ParameterPassing::read_only;
        const bool context_seen = std::ranges::find(result.parameters, Slot::context) != result.parameters.end();
        std::optional<Slot> slot;
        if (parameter.type == *context && read && !context_seen)
            slot = Slot::context;
        else if (parameter.type == *geometry && read)
            slot = Slot::input;
        else if (parameter.type == *geometries && read)
            slot = Slot::inputs;
        else if (parameter.type == *geometry && inline_value && parameter.passing == ParameterPassing::reference)
            slot = Slot::output;
        if (!slot)
            return error(ErrorCode::invalid_definition, name + ".evaluate parameter " + std::to_string(i) +
                                                            " is not a context, geometry input or geometry output.");
        result.parameters.push_back(*slot);
        if (*slot == Slot::context)
            continue;
        const auto& port = found->parameter_names[i];
        if (port.empty())
            return error(ErrorCode::invalid_definition,
                         name + ".evaluate parameter " + std::to_string(i) + " needs a name to be a port.");
        if (*slot == Slot::output) {
            result.outputs.push_back(port);
            continue;
        }
        result.inputs.push_back(port);
        result.multiple.push_back(*slot == Slot::inputs);
    }
    if (result.outputs.empty())
        return error(ErrorCode::invalid_definition, name + ".evaluate has no output.");
    return result;
}

Result<std::vector<Input>> inputs(Runtime& runtime, ObjectId node) {
    return runtime.with_native<const Node>(node, [&](const Node& value) -> Result<std::vector<Input>> {
        auto field = runtime.reference_field(node, Access::inputs(value));
        if (!field)
            return std::unexpected(field.error());
        const auto& connections = Access::connections(value);
        std::vector<Input> result;
        result.reserve(connections.size());
        for (std::size_t i = 0; i < connections.size(); ++i) {
            auto from = runtime.sequence_read(*field, i);
            if (!from)
                return std::unexpected(from.error());
            result.push_back({from->id(), connections[i].output, connections[i].input});
        }
        return result;
    });
}

Result<Contents> contents(Runtime& runtime, ObjectId graph) {
    return runtime.with_native<const Graph>(graph, [&](const Graph& value) -> Result<Contents> {
        auto field = runtime.reference_field(graph, Access::nodes(value));
        if (!field)
            return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count)
            return std::unexpected(count.error());
        Contents result;
        result.nodes.reserve(*count);
        for (std::size_t i = 0; i < *count; ++i) {
            auto node = runtime.sequence_read(*field, i);
            if (!node)
                return std::unexpected(node.error());
            result.nodes.push_back(node->id());
        }
        auto slot = runtime.reference_field(graph, Access::display(value));
        if (!slot)
            return std::unexpected(slot.error());
        auto display = runtime.read_reference(*slot);
        if (!display)
            return std::unexpected(display.error());
        result.display = display->id();
        return result;
    });
}

Result<ObjectRoot> add_node(Runtime& runtime, ObjectId graph, TypeId type) {
    if (auto valid = ports(runtime, type); !valid)
        return std::unexpected(valid.error());
    auto node = runtime.create(type);
    if (!node)
        return std::unexpected(node.error());
    auto added = runtime.with_native<Graph>(graph, [&](Graph& value) -> Result<void> {
        auto field = runtime.reference_field(graph, Access::nodes(value));
        if (!field)
            return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count)
            return std::unexpected(count.error());
        const auto reference = node->reference();
        const ReferenceChange change = SequenceSplice{*field, *count, 0, {&reference, 1}};
        return runtime.apply_reference_changes({&change, 1});
    });
    if (!added)
        return std::unexpected(added.error());
    return std::move(*node);
}

Result<void> remove_node(Runtime& runtime, ObjectId graph, ObjectId node) {
    auto listed = contents(runtime, graph);
    if (!listed)
        return std::unexpected(listed.error());
    const auto position = std::ranges::find(listed->nodes, node);
    if (position == listed->nodes.end())
        return error(ErrorCode::invalid_member, "The node is not in this graph.");
    for (const auto other : listed->nodes) {
        auto into = inputs(runtime, other);
        if (!into)
            return std::unexpected(into.error());
        if (std::ranges::none_of(*into, [&](const Input& input) { return input.from == node; }))
            continue;
        auto cleared = runtime.with_native<Node>(other, [&](Node& value) -> Result<void> {
            auto field = runtime.reference_field(other, Access::inputs(value));
            if (!field)
                return std::unexpected(field.error());
            auto& connections = Access::connections(value);
            std::vector<ObjectReference> kept;
            std::vector<Connection> remaining;
            for (std::size_t i = 0; i < connections.size(); ++i) {
                auto from = runtime.sequence_read(*field, i);
                if (!from)
                    return std::unexpected(from.error());
                if (from->id() == node)
                    continue;
                kept.push_back(*from);
                remaining.push_back(connections[i]);
            }
            const ReferenceChange change = SequenceSplice{*field, 0, connections.size(), kept};
            if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
                return applied;
            connections.swap(remaining);
            return {};
        });
        if (!cleared)
            return cleared;
    }
    auto node_type = runtime.native_type<Node>();
    if (!node_type)
        return std::unexpected(node_type.error());
    auto none = runtime.null_reference(*node_type);
    if (!none)
        return std::unexpected(none.error());
    return runtime.with_native<Graph>(graph, [&](Graph& value) -> Result<void> {
        auto field = runtime.reference_field(graph, Access::nodes(value));
        if (!field)
            return std::unexpected(field.error());
        auto slot = runtime.reference_field(graph, Access::display(value));
        if (!slot)
            return std::unexpected(slot.error());
        std::vector<ReferenceChange> changes{
            SequenceSplice{*field, std::size_t(position - listed->nodes.begin()), 1, {}}};
        if (listed->display == node)
            changes.push_back(ReferenceWrite{*slot, *none});
        return runtime.apply_reference_changes(changes);
    });
}

Result<void> connect(Runtime& runtime, ObjectId graph, ObjectId from, std::uint32_t output, ObjectId to,
                     std::uint32_t input) {
    auto listed = contents(runtime, graph);
    if (!listed)
        return std::unexpected(listed.error());
    for (const auto node : {from, to})
        if (std::ranges::find(listed->nodes, node) == listed->nodes.end())
            return error(ErrorCode::invalid_member, "Connected nodes must be in the graph.");
    auto from_type = runtime.type_of(from);
    auto to_type = runtime.type_of(to);
    if (!from_type || !to_type)
        return error(ErrorCode::invalid_object, "A connected node is not alive.");
    auto source = ports(runtime, *from_type);
    if (!source)
        return std::unexpected(source.error());
    auto target = ports(runtime, *to_type);
    if (!target)
        return std::unexpected(target.error());
    if (output >= source->outputs.size())
        return error(ErrorCode::argument_mismatch, "The upstream node has no output " + std::to_string(output) + ".");
    if (input >= target->inputs.size())
        return error(ErrorCode::argument_mismatch, "The downstream node has no input " + std::to_string(input) + ".");
    auto cycle = upstream(runtime, from, to);
    if (!cycle)
        return std::unexpected(cycle.error());
    if (*cycle)
        return error(ErrorCode::argument_mismatch, "The connection would form a cycle.");
    auto held = runtime.hold(from);
    if (!held)
        return std::unexpected(held.error());
    const bool multiple = target->multiple[input];
    return runtime.with_native<Node>(to, [&](Node& value) -> Result<void> {
        auto field = runtime.reference_field(to, Access::inputs(value));
        if (!field)
            return std::unexpected(field.error());
        auto& connections = Access::connections(value);
        const auto reference = held->reference();
        const Connection connection{input, output};
        const auto existing = std::ranges::find(connections, input, &Connection::input);
        if (!multiple && existing != connections.end()) {
            const ReferenceChange change =
                SequenceSplice{*field, std::size_t(existing - connections.begin()), 1, {&reference, 1}};
            if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
                return applied;
            *existing = connection;
            return {};
        }
        const auto position =
            std::size_t(std::ranges::upper_bound(connections, input, {}, &Connection::input) - connections.begin());
        connections.reserve(connections.size() + 1);
        const ReferenceChange change = SequenceSplice{*field, position, 0, {&reference, 1}};
        if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
            return applied;
        connections.insert(connections.begin() + std::ptrdiff_t(position), connection);
        return {};
    });
}

Result<void> disconnect(Runtime& runtime, ObjectId from, std::uint32_t output, ObjectId to, std::uint32_t input) {
    auto into = inputs(runtime, to);
    if (!into)
        return std::unexpected(into.error());
    const auto found = std::ranges::find_if(*into, [&](const Input& value) {
        return value.from == from && value.output == output && value.input == input;
    });
    if (found == into->end())
        return error(ErrorCode::invalid_member, "The nodes are not connected this way.");
    const auto position = std::size_t(found - into->begin());
    return runtime.with_native<Node>(to, [&](Node& value) -> Result<void> {
        auto field = runtime.reference_field(to, Access::inputs(value));
        if (!field)
            return std::unexpected(field.error());
        const ReferenceChange change = SequenceSplice{*field, position, 1, {}};
        if (auto applied = runtime.apply_reference_changes({&change, 1}); !applied)
            return applied;
        auto& connections = Access::connections(value);
        connections.erase(connections.begin() + std::ptrdiff_t(position));
        return {};
    });
}

Result<void> set_display(Runtime& runtime, ObjectId graph, ObjectId node) {
    ObjectReference reference;
    if (node == ObjectId{}) {
        auto type = runtime.native_type<Node>();
        if (!type)
            return std::unexpected(type.error());
        auto none = runtime.null_reference(*type);
        if (!none)
            return std::unexpected(none.error());
        reference = *none;
    } else {
        auto listed = member(runtime, graph, node);
        if (!listed)
            return std::unexpected(listed.error());
        if (!*listed)
            return error(ErrorCode::invalid_member, "The display node must be in the graph.");
        auto root = runtime.hold(node);
        if (!root)
            return std::unexpected(root.error());
        reference = root->reference();
    }
    return runtime.with_native<Graph>(graph, [&](Graph& value) -> Result<void> {
        auto slot = runtime.reference_field(graph, Access::display(value));
        if (!slot)
            return std::unexpected(slot.error());
        const ReferenceChange change = ReferenceWrite{*slot, reference};
        return runtime.apply_reference_changes({&change, 1});
    });
}

}
