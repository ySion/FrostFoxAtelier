#include "procedural/graph.hpp"
#include "detail/node_plan.hpp"
#include "foundation/result.hpp"
#include "procedural/nodes.hpp"
#include "runtime/foundation.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace atelier::procedural {
namespace detail {
struct Access {
    static Result<Contents> contents(Runtime&, ObjectId);

    static Result<std::vector<Input>> inputs(Runtime&, ObjectId);

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

template <std::size_t I>
using ColumnArray = Array<typename std::variant_alternative_t<I, ColumnData>::Value>;

std::unexpected<Error> error(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

Result<bool> member(Runtime& runtime, ObjectId graph, ObjectId node) {
    auto listed = Access::contents(runtime, graph);
    if (!listed)
        return std::unexpected(listed.error());
    return listed->contains(node);
}

// True when `target` is `from` or feeds it through any path of connections.
Result<bool> upstream(Runtime& runtime, const Contents& graph, ObjectId from, ObjectId target) {
    std::vector<ObjectId> pending{from};
    Map<ObjectId, std::uint8_t> seen;
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        if (node == target)
            return true;
        if (!seen.try_emplace(node, std::uint8_t{1}).inserted)
            continue;
        auto into = inputs(runtime, graph, node);
        if (!into)
            return std::unexpected(into.error());
        for (const auto& input : *into)
            pending.push_back(input.from);
    }
    return false;
}

std::optional<Layer> parse_layer_name(std::string_view text) noexcept {
    constexpr std::array<std::string_view, 5> names{"point", "vertex", "primitive", "detail", "instance"};
    const auto found = std::ranges::find(names, text);
    if (found == names.end())
        return std::nullopt;
    return static_cast<Layer>(found - names.begin());
}

}

Result<Reference> Context::mesh(StringView key) const noexcept {
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
    // Every attribute column type, so Source and native nodes can borrow columns.
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
    definitions.push_back(reflection::Definition<RayMode>::build(runtime));
    definitions.push_back(reflection::Definition<Geometry>::build(runtime));
    definitions.push_back(array_definition<Geometry>(runtime, "procedural.Geometries"));
    definitions.push_back(reflection::Definition<Context>::build(runtime));
    definitions.push_back(reflection::Definition<Node>::build(runtime));
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
    definitions.push_back(reflection::Definition<Ray>::build(runtime));
    definitions.push_back(reflection::Definition<CopyToPoints>::build(runtime));
    definitions.push_back(reflection::Definition<Realize>::build(runtime));
    ATELIER_TRY(runtime.register_types(std::move(definitions)));
    return {};
}

Result<detail::NodePlan> detail::inspect_node(Runtime& runtime, TypeId type) {
    ATELIER_TRY_BIND(node_base, runtime.native_type<Node>());
    ATELIER_TRY_BIND(derived, runtime.is_a(type, *node_base));
    ATELIER_TRY_BIND(info, runtime.describe(type));
    const auto& name = (*info)->name;
    if (!*derived)
        return error(ErrorCode::invalid_type, name + " is not a procedural.Node.");
    if (type == *node_base)
        return error(ErrorCode::invalid_type, "procedural.Node is the node base, not a concrete node.");

    auto context_type = runtime.native_type<Context>();
    auto geometry_type = runtime.native_type<Geometry>();
    auto geometries_type = runtime.native_type<Geometries>();
    auto layer_type = runtime.native_type<Layer>();
    auto string_type = runtime.native_type<String>();
    if (!context_type || !geometry_type || !geometries_type || !layer_type || !string_type)
        return error(ErrorCode::invalid_type, "Register the procedural types first.");

    std::array<TypeId, column_type_count> column_types{};
    bool columns_ready = true;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((columns_ready &=
          [&] {
              auto found = runtime.native_type<ColumnArray<I>>();
              if (!found)
                  return false;
              column_types[I] = *found;
              return true;
          }()),
         ...);
    }(std::make_index_sequence<column_type_count>{});
    if (!columns_ready)
        return error(ErrorCode::invalid_type, "Register the procedural column types first.");

    NodePlan plan;
    const FunctionSignature* signature{};
    std::span<const std::string> parameter_names;
    std::span<const MetadataList> parameter_metadata;

    if ((*info)->native_storage) {
        const auto found = std::ranges::find_if((*info)->methods, [&](const MethodInfo& m) {
            return m.name == "process" && m.signature.receiver && m.signature.receiver->type == type;
        });
        if (found == (*info)->methods.end())
            return error(ErrorCode::invalid_definition, "Node type " + name + " has no process method.");
        if (!found->signature.receiver || !found->signature.receiver->read_only || found->signature.result ||
            found->signature.execution != FunctionExecution::synchronous)
            return error(ErrorCode::invalid_definition,
                         name + ".process must be a const synchronous method without a result.");
        plan.native = true;
        plan.method = found->id;
        signature = &found->signature;
        parameter_names = found->parameter_names;
        parameter_metadata = found->parameter_metadata;
    } else {
        const auto found = std::ranges::find((*info)->described_methods, "process", &DescribedMethodInfo::name);
        if (found == (*info)->described_methods.end())
            return error(ErrorCode::invalid_definition, "Node type " + name + " has no process method.");
        if (found->signature.result || found->signature.execution != FunctionExecution::synchronous)
            return error(ErrorCode::invalid_definition,
                         name + ".process must be a synchronous method without a result.");
        plan.native = false;
        signature = &found->signature;
        parameter_names = found->parameter_names;
        parameter_metadata = found->parameter_metadata;
    }

    for (const auto& field : (*info)->fields) {
        for (const auto& [key, value] : field.metadata) {
            if (!key.starts_with("procedural."))
                continue;
            if (key != "procedural.role" || value != "run_over")
                return error(ErrorCode::invalid_definition,
                             name + "." + field.name + ": unknown procedural field metadata " + key + ".");
            if (plan.run_over_field != FieldId{})
                return error(ErrorCode::invalid_definition, name + ": at most one RunOver field is allowed.");
            if (field.value.form != ValueForm::inline_value || field.value.type != *layer_type)
                return error(ErrorCode::invalid_definition,
                             name + "." + field.name + ": RunOver requires a procedural.Layer field.");
            plan.run_over_field = field.id;
        }
    }

    struct ParsedMeta {
        std::string_view role;

        std::string_view explicit_name;

        bool has_name{};

        std::string_view from;

        std::string_view layer;

        bool optional{};
    };
    std::vector<ParsedMeta> parsed_meta(signature->parameters.size());
    for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
        auto& meta = parsed_meta[i];
        if (i >= parameter_metadata.size())
            continue;
        for (const auto& [key, value] : parameter_metadata[i]) {
            if (!key.starts_with("procedural."))
                continue;
            if (key == "procedural.role")
                meta.role = value;
            else if (key == "procedural.name") {
                meta.explicit_name = value;
                meta.has_name = true;
            } else if (key == "procedural.from")
                meta.from = value;
            else if (key == "procedural.layer")
                meta.layer = value;
            else if (key == "procedural.optional" && value == "true")
                meta.optional = true;
            else
                return error(ErrorCode::invalid_definition,
                             name + ".process parameter " + std::to_string(i) + ": invalid procedural metadata.");
        }
        if (meta.role == "in" || meta.role == "out" || meta.role == "inout" || meta.role == "group")
            plan.uses_primary_stream = true;
    }

    if (plan.uses_primary_stream) {
        plan.ports.inputs.push_back("input");
        plan.ports.multiple.push_back(false);
        plan.ports.outputs.push_back("output");
    }

    bool uses_dynamic_layer = false;
    plan.arguments.reserve(signature->parameters.size());

    for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
        const auto& param = signature->parameters[i];
        const std::string& param_name = parameter_names[i];
        const auto& meta = parsed_meta[i];
        const bool inline_value = param.form == ValueForm::inline_value;
        const bool read = inline_value && param.passing == ParameterPassing::read_only;
        const bool writable = inline_value && param.passing == ParameterPassing::reference;

        if (meta.role.empty() && param.type == *context_type && read) {
            if (meta.has_name || !meta.from.empty() || !meta.layer.empty() || meta.optional)
                return error(ErrorCode::invalid_definition,
                             name + ".process Context parameter accepts no column options.");
            if (plan.uses_context)
                return error(ErrorCode::invalid_definition, name + ".process accepts at most one Context parameter.");
            plan.uses_context = true;
            plan.arguments.push_back({.kind = ArgKind::context, .type = param.type});
            continue;
        }

        if (meta.role.empty() || meta.role == "port") {
            if (meta.has_name || !meta.from.empty() || !meta.layer.empty() || meta.optional)
                return error(ErrorCode::invalid_definition,
                             name + ".process port parameter " + std::to_string(i) + " accepts no column options.");
            std::optional<ArgKind> port_kind;
            if (param.type == *geometry_type && read)
                port_kind = ArgKind::port_input;
            else if (param.type == *geometries_type && read)
                port_kind = ArgKind::port_inputs;
            else if (param.type == *geometry_type && writable)
                port_kind = ArgKind::port_output;
            if (!port_kind)
                return error(ErrorCode::invalid_definition,
                             name + ".process parameter " + std::to_string(i) +
                                 " is not a Context, Geometry port, or annotated column stream.");
            if (param_name.empty() || !Geometry::admit_name(StringView{param_name}))
                return error(ErrorCode::invalid_definition, name + ".process port parameter " + std::to_string(i) +
                                                                " requires a valid identifier name.");
            if (*port_kind == ArgKind::port_output) {
                if (std::ranges::find(plan.ports.outputs, param_name) != plan.ports.outputs.end())
                    return error(ErrorCode::invalid_definition,
                                 name + ".process has duplicate output port '" + param_name + "'.");
                const auto index = static_cast<std::uint32_t>(plan.ports.outputs.size());
                plan.ports.outputs.push_back(param_name);
                plan.arguments.push_back({.kind = *port_kind, .type = param.type, .port_index = index});
            } else {
                if (std::ranges::find(plan.ports.inputs, param_name) != plan.ports.inputs.end())
                    return error(ErrorCode::invalid_definition,
                                 name + ".process has duplicate input port '" + param_name + "'.");
                const auto index = static_cast<std::uint32_t>(plan.ports.inputs.size());
                plan.ports.inputs.push_back(param_name);
                plan.ports.multiple.push_back(*port_kind == ArgKind::port_inputs);
                plan.arguments.push_back({.kind = *port_kind, .type = param.type, .port_index = index});
            }
            continue;
        }

        ArgKind kind{};
        if (meta.role == "in")
            kind = ArgKind::column_in;
        else if (meta.role == "inout")
            kind = ArgKind::column_inout;
        else if (meta.role == "out")
            kind = ArgKind::column_out;
        else if (meta.role == "group")
            kind = ArgKind::column_group;
        else
            return error(ErrorCode::invalid_definition, name + ".process parameter " + std::to_string(i) +
                                                            " has unknown role '" + std::string(meta.role) + "'.");

        const bool expects_read = kind == ArgKind::column_in || kind == ArgKind::column_group;
        if (expects_read ? !read : !writable)
            return error(ErrorCode::invalid_definition,
                         name + ".process parameter '" + param_name +
                             (expects_read ? "' requires const Array<T>&." : "' requires mutable Array<T>&."));
        if (meta.optional && kind != ArgKind::column_in)
            return error(ErrorCode::invalid_definition,
                         name + ".process parameter '" + param_name + "': Optional applies only to In parameters.");
        const auto column_it = std::ranges::find(column_types, param.type);
        if (column_it == column_types.end())
            return error(ErrorCode::invalid_definition,
                         name + ".process parameter '" + param_name + "' is not a supported Geometry column Array.");
        const auto variant = static_cast<std::size_t>(column_it - column_types.begin());
        if (kind == ArgKind::column_group && variant != 0)
            return error(ErrorCode::invalid_definition,
                         name + ".process parameter '" + param_name + "': Group requires const Array<bool>&.");
        if (meta.has_name && !meta.from.empty())
            return error(ErrorCode::invalid_definition,
                         name + ".process parameter '" + param_name + "' specifies both a literal name and From.");

        ArgPlan arg{.kind = kind, .type = param.type, .column_variant = variant, .optional = meta.optional};
        if (!meta.from.empty()) {
            const auto field = std::ranges::find((*info)->fields, meta.from, &FieldInfo::name);
            if (field == (*info)->fields.end() || field->value.form != ValueForm::inline_value ||
                field->value.type != *string_type)
                return error(ErrorCode::invalid_definition, name + ".process parameter '" + param_name +
                                                                "': From must name a String field on " + name + ".");
            arg.name_selector = field->id;
        } else {
            const std::string_view target = meta.has_name ? meta.explicit_name : std::string_view{param_name};
            if (kind != ArgKind::column_group || !target.empty()) {
                if (auto valid = Geometry::admit_name(StringView{target}); !valid)
                    return error(ErrorCode::invalid_definition,
                                 name + ".process parameter '" + param_name + "': " + valid.error().message);
            }
            auto owned = String::from_utf8(StringView{target});
            if (!owned)
                return error(ErrorCode::invalid_definition,
                             name + ".process parameter '" + param_name + "' requires valid UTF-8.");
            arg.static_name = std::move(*owned);
        }

        if (!meta.layer.empty()) {
            const auto parsed_layer = parse_layer_name(meta.layer);
            if (!parsed_layer)
                return error(ErrorCode::invalid_definition, name + ".process parameter '" + param_name +
                                                                "': invalid Layer '" + std::string(meta.layer) + "'.");
            arg.fixed_layer = *parsed_layer;
        } else if (plan.run_over_field == FieldId{}) {
            arg.fixed_layer = Layer::point;
        } else {
            uses_dynamic_layer = true;
        }

        if ((kind == ArgKind::column_inout || kind == ArgKind::column_out) && arg.name_selector == FieldId{}) {
            for (const auto& prior : plan.arguments)
                if ((prior.kind == ArgKind::column_inout || prior.kind == ArgKind::column_out) &&
                    prior.name_selector == FieldId{} && prior.static_name == arg.static_name)
                    return error(ErrorCode::invalid_definition, name + ".process writes attribute '" +
                                                                    std::string{arg.static_name.view().std_view()} +
                                                                    "' more than once.");
        }

        plan.arguments.push_back(std::move(arg));
    }

    if (plan.ports.outputs.empty())
        return error(ErrorCode::invalid_definition, name + ".process has no output.");
    if (plan.run_over_field != FieldId{} && !uses_dynamic_layer)
        return error(ErrorCode::invalid_definition,
                     name + ": RunOver requires at least one column or group parameter without a fixed Layer.");
    return plan;
}

Result<Ports> ports(Runtime& runtime, TypeId type) {
    ATELIER_TRY_BIND(plan, detail::inspect_node(runtime, type));
    return std::move(plan->ports);
}

Result<std::vector<Input>> detail::Access::inputs(Runtime& runtime, ObjectId node) {
    return runtime.with_native<const Node>(node, [&](const Node& value) -> Result<std::vector<Input>> {
        auto field = runtime.reference_field(node, Access::inputs(value));
        if (!field)
            return std::unexpected(field.error());
        const auto& connections = Access::connections(value);
        ATELIER_TRY_BIND(count, runtime.sequence_size(*field));
        if (*count != connections.size())
            return error(ErrorCode::argument_mismatch, "Graph input references do not match its connections.");
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

Result<Contents> detail::Access::contents(Runtime& runtime, ObjectId graph) {
    return runtime.with_native<const Graph>(graph, [&](const Graph& value) -> Result<Contents> {
        auto field = runtime.reference_field(graph, Access::nodes(value));
        if (!field)
            return std::unexpected(field.error());
        auto count = runtime.sequence_size(*field);
        if (!count)
            return std::unexpected(count.error());
        Contents result;
        result.nodes_.reserve(*count);
        result.members_.reserve(*count);
        for (std::size_t i = 0; i < *count; ++i) {
            auto node = runtime.sequence_read(*field, i);
            if (!node)
                return std::unexpected(node.error());
            result.nodes_.push_back(node->id());
            result.members_.insert_or_assign(node->id(), std::uint8_t{1});
        }
        auto slot = runtime.reference_field(graph, Access::display(value));
        if (!slot)
            return std::unexpected(slot.error());
        auto display = runtime.read_reference(*slot);
        if (!display)
            return std::unexpected(display.error());
        result.display_ = display->id();
        return result;
    });
}

Result<void> Contents::check(std::span<const Input> connections) const {
    for (const auto& input : connections)
        if (!contains(input.from))
            return error(ErrorCode::argument_mismatch, "Graph input connects a node outside this graph.");
    return {};
}

Result<Contents> contents(Runtime& runtime, ObjectId graph) {
    ATELIER_TRY_BIND(result, Access::contents(runtime, graph));
    if (result->display() != ObjectId{} && !result->contains(result->display()))
        return error(ErrorCode::argument_mismatch, "Graph display names a node outside this graph.");
    return std::move(*result);
}

Result<std::vector<Input>> inputs(Runtime& runtime, const Contents& graph, ObjectId node) {
    if (!graph.contains(node))
        return error(ErrorCode::argument_mismatch, "The node is not in this graph.");
    ATELIER_TRY_BIND(result, Access::inputs(runtime, node));
    ATELIER_TRY(graph.check(*result));
    return std::move(*result);
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
    auto listed = Access::contents(runtime, graph);
    if (!listed)
        return std::unexpected(listed.error());
    const auto position = std::ranges::find(listed->nodes(), node);
    if (position == listed->nodes().end())
        return error(ErrorCode::invalid_member, "The node is not in this graph.");
    for (const auto other : listed->nodes()) {
        auto into = Access::inputs(runtime, other);
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
            SequenceSplice{*field, std::size_t(position - listed->nodes().begin()), 1, {}}};
        if (listed->display() == node)
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
        if (!listed->contains(node))
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
    auto cycle = upstream(runtime, *listed, from, to);
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
    auto into = Access::inputs(runtime, to);
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
