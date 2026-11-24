#include "editor/graph_format.hpp"
#include "editor/properties.hpp"
#include "foundation/map.hpp"
#include "procedural/nodes.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>

namespace atelier::editor {
// "node.output", or nothing when malformed.
std::optional<std::pair<std::string_view, std::string_view>> graph_endpoint(json::Value value) {
    if (value.kind() != json::Kind::string)
        return std::nullopt;
    const std::string_view text = *value.string();
    const auto dot = text.find('.');
    if (dot == text.npos || !valid_graph_identifier(text.substr(0, dot)) ||
        !valid_graph_identifier(text.substr(dot + 1)))
        return std::nullopt;
    return std::pair{text.substr(0, dot), text.substr(dot + 1)};
}

namespace {
constexpr std::uint32_t graph_format_version = 4;

Result<void> append_vec2(json::Builder& builder, json::Value parent, std::string_view key,
                         const std::array<float, 2>& v) {
    const auto arr = *builder.array(parent, key);
    for (const float coordinate : v) {
        if (auto added = builder.append(arr, coordinate); !added)
            return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
    }
    return {};
}

Scalar scalar_max(const Scalar& a, const Scalar& b) noexcept {
    return detail::scalar_less(a, b) ? b : a;
}

Scalar scalar_min(const Scalar& a, const Scalar& b) noexcept {
    return detail::scalar_less(b, a) ? b : a;
}

bool is_scratch_asset_key(std::string_view key) noexcept {
    return key == "scratch" || key.starts_with("scratch/");
}

} // namespace

bool valid_graph_identifier(std::string_view text) noexcept {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    return !text.empty() && text.size() <= 128 && letter(text.front()) &&
           std::ranges::all_of(text.substr(1), [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

std::string make_unique_graph_identifier(std::string_view preferred, std::string_view fallback,
                                         const std::function<bool(std::string_view)>& is_taken) {
    std::string base;
    base.reserve(preferred.size());
    for (const char c : preferred) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            base.push_back(c);
        else
            base.push_back('_');
    }
    const auto head = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    std::string safe_fb(fallback);
    if (safe_fb.empty() || !head(safe_fb.front()))
        safe_fb = "item";
    if (base.empty())
        base = safe_fb;
    else if (!head(base.front()))
        base = safe_fb + "_" + base;
    if (base.size() > 128)
        base.resize(128);
    if (!is_taken || !is_taken(base))
        return base;
    for (std::uint32_t idx = 2;; ++idx) {
        const std::string suffix = "_" + std::to_string(idx);
        const std::size_t max_base = suffix.size() < 128 ? 128 - suffix.size() : 1;
        std::string cand = base.substr(0, std::min(base.size(), max_base)) + suffix;
        if (!is_taken(cand))
            return cand;
    }
}

bool valid_graph_target_path(std::string_view text) noexcept {
    if (text.empty())
        return false;
    std::size_t segments = 0;
    std::string_view rem = text;
    while (true) {
        const auto dot = rem.find('.');
        const auto part = rem.substr(0, dot);
        if (!valid_graph_identifier(part))
            return false;
        ++segments;
        if (dot == std::string_view::npos)
            break;
        rem.remove_prefix(dot + 1);
        if (segments > 17)
            return false;
    }
    return segments >= 2 && segments <= 17;
}

bool graph_target_controls_path(std::string_view promoted_target, std::string_view candidate) noexcept {
    const auto dot_a = promoted_target.find('.');
    const auto dot_b = candidate.find('.');
    if (dot_a == std::string_view::npos || dot_b == std::string_view::npos)
        return promoted_target == candidate;
    if (promoted_target.substr(0, dot_a) != candidate.substr(0, dot_b))
        return false;
    if (promoted_target == candidate)
        return true;
    return promoted_target.size() < candidate.size() && candidate.starts_with(promoted_target) &&
           candidate[promoted_target.size()] == '.';
}

bool graph_targets_overlap(std::string_view a, std::string_view b) noexcept {
    return graph_target_controls_path(a, b) || graph_target_controls_path(b, a);
}

std::string_view graph_node_shape_name(GraphNodeShape shape) noexcept {
    switch (shape) {
    case GraphNodeShape::auto_role:
        return {};
    case GraphNodeShape::pill:
        return "pill";
    case GraphNodeShape::rect:
        return "rect";
    case GraphNodeShape::domed:
        return "domed";
    case GraphNodeShape::trapezoid:
        return "trapezoid";
    case GraphNodeShape::hexagon:
        return "hexagon";
    case GraphNodeShape::ribbon:
        return "ribbon";
    case GraphNodeShape::diamond:
        return "diamond";
    case GraphNodeShape::circle:
        return "circle";
    }
    return {};
}

std::optional<GraphNodeShape> parse_graph_node_shape(std::string_view text) noexcept {
    if (text.empty() || text == "auto")
        return GraphNodeShape::auto_role;
    if (text == "pill")
        return GraphNodeShape::pill;
    if (text == "rect")
        return GraphNodeShape::rect;
    if (text == "domed")
        return GraphNodeShape::domed;
    if (text == "trapezoid")
        return GraphNodeShape::trapezoid;
    if (text == "hexagon")
        return GraphNodeShape::hexagon;
    if (text == "ribbon")
        return GraphNodeShape::ribbon;
    if (text == "diamond")
        return GraphNodeShape::diamond;
    if (text == "circle")
        return GraphNodeShape::circle;
    return std::nullopt;
}

std::string format_graph_hex_color(std::uint32_t rgb) {
    return std::format("#{:06x}", rgb & 0xffffffu);
}

std::optional<std::uint32_t> parse_graph_hex_color(std::string_view text) noexcept {
    if (text.size() != 7 || text.front() != '#')
        return std::nullopt;
    text.remove_prefix(1);
    std::uint32_t value = 0;
    for (const char c : text) {
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return std::nullopt;
        value = (value << 4u) | digit;
    }
    return value;
}

Result<FieldRange> intersect_field_ranges(const FieldRange& a, const FieldRange& b) {
    FieldRange out;
    if (a.hard_min && b.hard_min) {
        if (a.hard_min->index() != b.hard_min->index())
            return std::unexpected(Error{ErrorCode::type_mismatch, "Mismatched FieldRange scalar types."});
        out.hard_min = scalar_max(*a.hard_min, *b.hard_min);
    } else {
        out.hard_min = a.hard_min ? a.hard_min : b.hard_min;
    }

    if (a.hard_max && b.hard_max) {
        if (a.hard_max->index() != b.hard_max->index())
            return std::unexpected(Error{ErrorCode::type_mismatch, "Mismatched FieldRange scalar types."});
        out.hard_max = scalar_min(*a.hard_max, *b.hard_max);
    } else {
        out.hard_max = a.hard_max ? a.hard_max : b.hard_max;
    }

    if (out.hard_min && out.hard_max) {
        if (out.hard_min->index() != out.hard_max->index())
            return std::unexpected(Error{ErrorCode::type_mismatch, "Mismatched FieldRange scalar types."});
        if (detail::scalar_less(*out.hard_max, *out.hard_min))
            return std::unexpected(Error{ErrorCode::argument_mismatch,
                                         "Promoted parameter target hard bounds have an empty intersection."});
    }

    if (a.ui_min && b.ui_min && a.ui_min->index() == b.ui_min->index())
        out.ui_min = scalar_max(*a.ui_min, *b.ui_min);
    else
        out.ui_min = a.ui_min ? a.ui_min : b.ui_min;

    if (a.ui_max && b.ui_max && a.ui_max->index() == b.ui_max->index())
        out.ui_max = scalar_min(*a.ui_max, *b.ui_max);
    else
        out.ui_max = a.ui_max ? a.ui_max : b.ui_max;

    if (out.ui_min)
        out.ui_min = detail::clamp_scalar(*out.ui_min, out).first;
    if (out.ui_max)
        out.ui_max = detail::clamp_scalar(*out.ui_max, out).first;
    if (out.ui_min && out.ui_max && detail::scalar_less(*out.ui_max, *out.ui_min))
        out.ui_max = out.ui_min;
    return out;
}

Result<ResolvedGraphTarget> resolve_graph_parameter_target(Runtime& runtime, const GraphDocument& document,
                                                           std::string_view target_spec) {
    if (!valid_graph_target_path(target_spec))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Invalid parameter target path '" + std::string(target_spec) + "'."});
    const auto dot = target_spec.find('.');
    const std::string node_name(target_spec.substr(0, dot));
    const std::string field_path(target_spec.substr(dot + 1));
    const auto node_it = std::ranges::find(document.nodes, node_name, &GraphNode::name);
    if (node_it == document.nodes.end())
        return std::unexpected(Error{ErrorCode::argument_mismatch, "No node named '" + node_name + "'."});
    if (!node_it->node.registered())
        return std::unexpected(Error{ErrorCode::invalid_type, "Node '" + node_name + "' is unresolved."});

    ATELIER_TRY_BIND(type, runtime.type_of(node_it->node.id()));
    ATELIER_TRY_BIND(fields, detail::author_fields(runtime, *type, false));
    for (const auto& leaf : *fields) {
        if (detail::dotted(leaf.names) != field_path)
            continue;
        if (leaf.read_only || leaf.form != ValueForm::inline_value || leaf.kind == TypeKind::array ||
            leaf.material_entries_json) {
            return std::unexpected(Error{ErrorCode::argument_mismatch, "Field '" + std::string(target_spec) +
                                                                           "' cannot be promoted as a parameter."});
        }
        return ResolvedGraphTarget{
            .node_name = node_name,
            .field_path = field_path,
            .node = node_it->node.id(),
            .value_type = leaf.type,
            .is_vector = false,
            .label = leaf.label,
            .category = leaf.category,
            .description = leaf.description,
            .leaves = {leaf},
        };
    }

    // Check math vector aggregate path (e.g. "grid1.size").
    TypeId cur_type = *type;
    const FieldInfo* matched_field = nullptr;
    std::vector<std::string> segments;
    std::string_view rem = field_path;
    while (true) {
        const auto next_dot = rem.find('.');
        const auto part = rem.substr(0, next_dot);
        segments.emplace_back(part);
        ATELIER_TRY_BIND(info, runtime.describe(cur_type));
        const auto f_it = std::ranges::find((*info)->fields, part, &FieldInfo::name);
        if (f_it == (*info)->fields.end() || f_it->transient || f_it->read_only ||
            f_it->value.form != ValueForm::inline_value) {
            matched_field = nullptr;
            break;
        }
        matched_field = &*f_it;
        cur_type = f_it->value.type;
        if (next_dot == std::string_view::npos)
            break;
        rem.remove_prefix(next_dot + 1);
    }

    if (matched_field) {
        ATELIER_TRY_BIND(vec_info, runtime.describe(cur_type));
        if ((*vec_info)->math.vector()) {
            std::vector<detail::ScalarField> lanes;
            for (const auto& leaf : *fields) {
                if (leaf.names.size() == segments.size() + 1 &&
                    std::equal(segments.begin(), segments.end(), leaf.names.begin())) {
                    if (leaf.read_only || leaf.form != ValueForm::inline_value)
                        return std::unexpected(Error{ErrorCode::argument_mismatch,
                                                     "Vector field '" + std::string(target_spec) + "' is read-only."});
                    lanes.push_back(leaf);
                }
            }
            if (lanes.size() == (*vec_info)->math.components() && !lanes.empty()) {
                return ResolvedGraphTarget{
                    .node_name = node_name,
                    .field_path = field_path,
                    .node = node_it->node.id(),
                    .value_type = cur_type,
                    .is_vector = true,
                    .label = matched_field->label,
                    .category = lanes.front().category,
                    .description = matched_field->description,
                    .leaves = std::move(lanes),
                };
            }
        }
    }

    return std::unexpected(
        Error{ErrorCode::argument_mismatch, "Node '" + node_name + "' has no promotable field '" + field_path + "'."});
}

Result<procedural::Ports> extract_graph_ports(Runtime& runtime, const GraphDocument& document,
                                              std::vector<ObjectId>* input_nodes, std::vector<ObjectId>* output_nodes) {
    if (input_nodes)
        input_nodes->clear();
    if (output_nodes)
        output_nodes->clear();
    ATELIER_TRY_BIND(in_type, runtime.native_type<procedural::GraphInput>());
    ATELIER_TRY_BIND(out_type, runtime.native_type<procedural::GraphOutput>());

    struct OrderedBoundary {
        std::int32_t order{};

        std::string node_name;

        std::string port_name;

        ObjectId id{};
    };

    std::vector<OrderedBoundary> inputs_list;
    std::vector<OrderedBoundary> outputs_list;
    for (const auto& item : document.nodes) {
        if (!item.node.registered())
            continue;
        ATELIER_TRY_BIND(type, runtime.type_of(item.node.id()));
        if (*type == *in_type) {
            std::string port_name;
            std::int32_t order = 0;
            ATELIER_TRY(runtime.with_native<const procedural::GraphInput>(
                item.node.id(), [&](const procedural::GraphInput& value) -> Result<void> {
                    port_name = std::string(value.port_name.view().std_view());
                    order = value.order;
                    return {};
                }));
            if (!valid_graph_identifier(port_name))
                return std::unexpected(
                    Error{ErrorCode::invalid_definition,
                          "GraphInput node '" + item.name + "' has invalid port_name '" + port_name + "'."});
            inputs_list.push_back({order, item.name, std::move(port_name), item.node.id()});
        } else if (*type == *out_type) {
            std::string port_name;
            std::int32_t order = 0;
            ATELIER_TRY(runtime.with_native<const procedural::GraphOutput>(
                item.node.id(), [&](const procedural::GraphOutput& value) -> Result<void> {
                    port_name = std::string(value.port_name.view().std_view());
                    order = value.order;
                    return {};
                }));
            if (!valid_graph_identifier(port_name))
                return std::unexpected(
                    Error{ErrorCode::invalid_definition,
                          "GraphOutput node '" + item.name + "' has invalid port_name '" + port_name + "'."});
            outputs_list.push_back({order, item.name, std::move(port_name), item.node.id()});
        }
    }

    const auto by_order_then_name = [](const OrderedBoundary& a, const OrderedBoundary& b) {
        return a.order != b.order ? a.order < b.order : a.node_name < b.node_name;
    };
    std::ranges::sort(inputs_list, by_order_then_name);
    std::ranges::sort(outputs_list, by_order_then_name);

    procedural::Ports ports;
    std::set<std::string> seen_in;
    for (const auto& entry : inputs_list) {
        if (!seen_in.insert(entry.port_name).second)
            return std::unexpected(
                Error{ErrorCode::invalid_definition, "Duplicate GraphInput port_name '" + entry.port_name + "'."});
        ports.inputs.push_back(entry.port_name);
        ports.multiple.push_back(false);
        if (input_nodes)
            input_nodes->push_back(entry.id);
    }

    if (!outputs_list.empty()) {
        std::set<std::string> seen_out;
        for (const auto& entry : outputs_list) {
            if (!seen_out.insert(entry.port_name).second)
                return std::unexpected(
                    Error{ErrorCode::invalid_definition, "Duplicate GraphOutput port_name '" + entry.port_name + "'."});
            ports.outputs.push_back(entry.port_name);
            if (output_nodes)
                output_nodes->push_back(entry.id);
        }
    } else if (document.graph.registered()) {
        ATELIER_TRY_BIND(listed, procedural::contents(runtime, document.graph.id()));
        if (listed->display() != ObjectId{}) {
            ports.outputs.push_back("output");
            if (output_nodes)
                output_nodes->push_back(listed->display());
        }
    }
    return ports;
}

Result<ResolvedPromotedSchema> resolve_promoted_parameter_schema(Runtime& runtime, const GraphDocument& document,
                                                                 const GraphPromotedParameter& param) {
    if (param.targets.empty())
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Promoted parameter '" + param.name + "' has no targets."});
    ATELIER_TRY_BIND(primary, resolve_graph_parameter_target(runtime, document, param.targets.front()));
    ResolvedPromotedSchema schema;
    schema.primary = *primary;
    schema.all_targets.push_back(*primary);
    for (const auto& leaf : primary->leaves)
        schema.ranges.push_back(leaf.range);

    for (std::size_t i = 1; i < param.targets.size(); ++i) {
        ATELIER_TRY_BIND(next, resolve_graph_parameter_target(runtime, document, param.targets[i]));
        if (next->value_type != schema.primary.value_type || next->leaves.size() != schema.primary.leaves.size())
            return std::unexpected(Error{ErrorCode::argument_mismatch,
                                         "Target '" + param.targets[i] + "' does not match primary target TypeId."});
        for (std::size_t li = 0; li < schema.ranges.size(); ++li) {
            ATELIER_TRY_BIND(merged, intersect_field_ranges(schema.ranges[li], next->leaves[li].range));
            schema.ranges[li] = *merged;
        }
        schema.all_targets.push_back(std::move(*next));
    }
    return schema;
}

void destroy_private_graph_document(Runtime& runtime, GraphDocument& document) noexcept {
    std::vector<ObjectId> node_ids;
    node_ids.reserve(document.nodes.size());
    for (auto& item : document.nodes) {
        if (item.node.registered()) {
            node_ids.push_back(item.node.id());
            item.node = {};
        }
    }
    const ObjectId graph_id = document.graph.registered() ? document.graph.id() : ObjectId{};
    document.graph = {};
    document.nodes.clear();
    document.parameters.clear();
    document.notes.clear();
    document.boxes.clear();
    document.kept = {};
    document.issues.clear();
    document.template_node = {};
    if (graph_id != ObjectId{} && runtime.type_of(graph_id))
        (void)runtime.destroy(graph_id);
    for (const ObjectId nid : node_ids) {
        if (nid != ObjectId{} && runtime.type_of(nid))
            (void)runtime.destroy(nid);
    }
}

namespace {

Result<void> copy_object_author_fields(Runtime& runtime, ObjectId src, ObjectId dst, TypeId type) {
    ATELIER_TRY_BIND(fields, detail::author_fields(runtime, type, false));
    for (const auto& leaf : *fields) {
        if (leaf.form != ValueForm::inline_value)
            continue;
        if (leaf.kind == TypeKind::string) {
            String s;
            ATELIER_TRY(runtime.read(src, leaf.path, ValueOutput{leaf.type, &s}));
            ATELIER_TRY(runtime.write(dst, leaf.path, ValueInput{leaf.type, &s}));
        } else if (leaf.kind == TypeKind::array) {
            ATELIER_TRY_BIND(arr, ArrayLeaf::create(runtime, leaf.type));
            ATELIER_TRY(runtime.read(src, leaf.path, arr->output()));
            ATELIER_TRY(runtime.write(dst, leaf.path, arr->input()));
        } else {
            ATELIER_TRY_BIND(s, runtime.read(src, leaf.path));
            ATELIER_TRY(runtime.write(dst, leaf.path, *s));
        }
    }
    return {};
}

} // namespace

Result<GraphDocument> clone_graph_document(Runtime& runtime, const GraphDocument& source) {
    if (!source.graph.registered())
        return std::unexpected(Error{ErrorCode::invalid_object, "Source GraphDocument has no live graph root."});
    ATELIER_TRY_BIND(graph_type, runtime.native_type<procedural::Graph>());
    ATELIER_TRY_BIND(asset_node_type, runtime.native_type<procedural::AssetNode>());
    GraphDocument cloned;
    struct Guard {
        Runtime& rt;

        GraphDocument& doc;

        bool ok{};

        ~Guard() {
            if (!ok)
                destroy_private_graph_document(rt, doc);
        }
    } guard{runtime, cloned, false};

    ATELIER_TRY_BIND(created_graph, runtime.create(*graph_type));
    cloned.graph = std::move(*created_graph);
    const ObjectId src_graph = source.graph.id();
    const ObjectId dst_graph = cloned.graph.id();
    ATELIER_TRY(copy_object_author_fields(runtime, src_graph, dst_graph, *graph_type));

    ATELIER_TRY_BIND(src_contents, procedural::contents(runtime, src_graph));
    Map<ObjectId, ObjectId> id_map;
    id_map.reserve(src_contents->nodes().size());
    cloned.nodes.reserve(source.nodes.size());
    for (const auto& src_item : source.nodes) {
        GraphNode dst_item;
        dst_item.name = src_item.name;
        dst_item.asset_key = src_item.asset_key;
        dst_item.overrides = src_item.overrides;
        dst_item.position = src_item.position;
        dst_item.shape = src_item.shape;
        dst_item.color = src_item.color;
        dst_item.comment = src_item.comment;
        dst_item.kept = src_item.kept;
        if (src_item.node.registered()) {
            const ObjectId sid = src_item.node.id();
            ATELIER_TRY_BIND(ntype, runtime.type_of(sid));
            ATELIER_TRY_BIND(created_node, procedural::add_node(runtime, dst_graph, *ntype));
            const ObjectId did = created_node->id();
            ATELIER_TRY(copy_object_author_fields(runtime, sid, did, *ntype));
            if (*ntype == *asset_node_type) {
                ATELIER_TRY_BIND(sub, procedural::asset_subgraph(runtime, sid));
                procedural::AssetSubgraph stub;
                stub.asset_key = sub->asset_key;
                stub.ports = sub->ports;
                ATELIER_TRY(procedural::set_asset_subgraph(runtime, did, std::move(stub)));
            }
            ATELIER_TRY_BIND(byp, procedural::bypassed(runtime, sid));
            if (*byp)
                ATELIER_TRY(procedural::set_bypassed(runtime, did, true));
            ATELIER_TRY_BIND(frz, procedural::frozen(runtime, sid));
            if (*frz) {
                ATELIER_TRY_BIND(snap, procedural::frozen_output(runtime, sid));
                ATELIER_TRY(procedural::set_frozen(runtime, did, true, *snap));
            }
            id_map.try_emplace(sid, did);
            dst_item.node = std::move(*created_node);
        }
        cloned.nodes.push_back(std::move(dst_item));
    }
    std::ranges::sort(cloned.nodes, {}, &GraphNode::name);

    for (const auto& src_item : source.nodes) {
        if (!src_item.node.registered())
            continue;
        const ObjectId sid = src_item.node.id();
        const ObjectId did = *id_map.find(sid);
        ATELIER_TRY_BIND(conns, procedural::inputs(runtime, *src_contents, sid));
        for (const auto& in : *conns) {
            if (const auto* from = id_map.find(in.from))
                ATELIER_TRY(procedural::connect(runtime, dst_graph, *from, in.output, did, in.input));
        }
    }

    if (src_contents->display() != ObjectId{}) {
        if (const auto* display = id_map.find(src_contents->display()))
            ATELIER_TRY(procedural::set_display(runtime, dst_graph, *display));
    }
    if (source.template_node != ObjectId{}) {
        if (const auto* target = id_map.find(source.template_node))
            cloned.template_node = *target;
    }
    cloned.parameters = source.parameters;
    cloned.notes = source.notes;
    cloned.boxes = source.boxes;
    cloned.kept = source.kept;
    guard.ok = true;
    return cloned;
}

Result<void> apply_saved_graph_node_inputs(Runtime& runtime, GraphDocument& document, GraphNode& target,
                                           const SavedGraphPortMap& saved, std::vector<world::Diagnostic>* issues) {
    if (!target.node.registered())
        return {};
    const ObjectId graph = document.graph.id();
    const ObjectId to = target.node.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime, graph));
    ATELIER_TRY_BIND(existing, procedural::inputs(runtime, *listed, to));
    for (auto it = existing->rbegin(); it != existing->rend(); ++it)
        ATELIER_TRY(procedural::disconnect(runtime, it->from, it->output, to, it->input));

    ATELIER_TRY_BIND(ports, procedural::ports(runtime, to));
    ATELIER_TRY_BIND(type, runtime.type_of(to));
    ATELIER_TRY_BIND(info, runtime.describe(*type));

    const auto named = [&](std::string_view name) -> const GraphNode* {
        const auto at = std::ranges::lower_bound(document.nodes, name, {}, &GraphNode::name);
        return at != document.nodes.end() && at->name == name ? &*at : nullptr;
    };

    json::Builder next_kept(target.kept);
    next_kept.erase(next_kept.root(), "inputs");

    for (const auto& [pname, entry] : saved) {
        const auto& [was_array, eps] = entry;
        std::string problem;
        const auto at = std::ranges::find(ports->inputs, pname);
        const auto in_idx = static_cast<std::uint32_t>(at - ports->inputs.begin());
        if (at == ports->inputs.end()) {
            problem = "The node has no input port " + pname + ".";
        } else if (ports->multiple[in_idx] != was_array) {
            problem =
                ports->multiple[in_idx] ? "The port takes an array of connections." : "The port takes one connection.";
        } else {
            for (const auto& ep : eps) {
                const auto dot = ep.find('.');
                const auto from_name = std::string_view(ep).substr(0, dot);
                const auto out_name = std::string_view(ep).substr(dot + 1);
                const auto* src = named(from_name);
                std::string failed;
                if (!src) {
                    failed = "No node is named " + std::string(from_name) + ".";
                } else if (!src->node.registered()) {
                    failed = "Node " + std::string(from_name) + " is kept, not loaded.";
                } else {
                    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime, src->node.id()));
                    const auto out_it = std::ranges::find(src_ports->outputs, out_name);
                    if (out_it == src_ports->outputs.end()) {
                        failed = "Node " + std::string(from_name) + " has no output " + std::string(out_name) + ".";
                    } else if (auto made = procedural::connect(
                                   runtime, graph, src->node.id(),
                                   static_cast<std::uint32_t>(out_it - src_ports->outputs.begin()), to, in_idx);
                               !made) {
                        if (made.error().code != ErrorCode::argument_mismatch)
                            return std::unexpected(made.error());
                        failed = made.error().message;
                    }
                }
                if (problem.empty() && !failed.empty())
                    problem = std::move(failed);
            }
        }
        if (problem.empty())
            continue;
        const std::array<std::string, 1> path{"inputs"};
        const auto inputs_obj = next_kept.object_path_validated(next_kept.root(), path);
        if (was_array) {
            const auto arr = *next_kept.array(inputs_obj, pname);
            for (const auto& ep : eps)
                static_cast<void>(next_kept.append(arr, ep));
        } else if (!eps.empty()) {
            next_kept.set_validated(inputs_obj, pname, eps.front());
        }
        if (issues) {
            world::Diagnostic diag;
            diag.serial = issues->size() + 1;
            diag.severity = world::Severity::warning;
            diag.kind = world::DiagnosticKind::reference_dropped;
            diag.phase = world::DiagnosticPhase::load;
            diag.object = to;
            diag.type = std::string((*info)->name);
            diag.field = {"nodes", target.name, "inputs", pname};
            if (const auto kept_val = *inputs_obj.find(pname))
                diag.value = assets::json_format::preview(*kept_val);
            diag.message = detail::dotted(diag.field) + ": " + problem + " The saved connections are kept.";
            issues->push_back(std::move(diag));
        }
    }
    target.kept = next_kept.finish();
    return {};
}

procedural::AssetSubgraph resolve_asset_binding(const AssetPortResolver& resolver, std::string_view asset_key) {
    procedural::AssetSubgraph binding;
    binding.asset_key = asset_key;
    auto ports = resolver(asset_key);
    if (ports)
        binding.ports = std::move(*ports);
    else
        binding.bind_error = std::move(ports.error());
    return binding;
}

Result<GraphDocument> load_graph(Runtime& runtime, std::string_view text, const AssetPortResolver& asset_ports) {
    const auto file = assets::json_format::parse(text);
    if (!file)
        return std::unexpected(Error{ErrorCode::argument_mismatch, file.error().describe()});
    const auto root = file->root();
    if (auto fields = root.allowed_members({"version", "settings", "parameters", "nodes", "notes", "boxes"}); !fields)
        return std::unexpected(Error{ErrorCode::argument_mismatch, fields.error().describe()});
    const auto version = root.u64({"version"});
    if (!version || *version != graph_format_version)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Unsupported procedural graph version."});
    const json::Document empty;
    const auto settings = root.find("settings")->value_or(empty.root());
    const auto parameters_opt = *root.find("parameters");
    const auto entries = root.find("nodes")->value_or(empty.root());
    const auto notes_json = root.find("notes")->value_or(empty.root());
    const auto boxes_json = root.find("boxes")->value_or(empty.root());

    // Validate the complete file before creating Runtime graph objects.
    const auto validated = [&]() -> json::Result<void> {
        if (settings.kind() != json::Kind::object)
            return std::unexpected(settings.error(json::Fault::type, "Graph settings must be a record."));
        if (entries.kind() != json::Kind::object)
            return std::unexpected(
                entries.error(json::Fault::type, "Graph nodes must be a record keyed by node name."));
        if (notes_json.kind() != json::Kind::object)
            return std::unexpected(
                notes_json.error(json::Fault::type, "Graph notes must be a record keyed by note name."));
        if (boxes_json.kind() != json::Kind::object)
            return std::unexpected(
                boxes_json.error(json::Fault::type, "Graph boxes must be a record keyed by box name."));
        if (parameters_opt) {
            ATELIER_TRY_BIND(param_items, parameters_opt->array());
            std::set<std::string_view> seen_param_names;
            std::vector<std::string_view> all_targets;
            for (const auto param : *param_items) {
                ATELIER_TRY(param.allowed_members({"name", "targets", "label", "category", "description"}));
                ATELIER_TRY_BIND(pname, param.string({"name"}));
                if (!valid_graph_identifier(*pname))
                    return std::unexpected(param.error(json::Fault::range, "Invalid promoted parameter name."));
                if (!seen_param_names.insert(*pname).second)
                    return std::unexpected(
                        param.error(json::Fault::duplicate_key, "Duplicate promoted parameter name."));
                const auto targets_val = *param.find("targets");
                if (!targets_val)
                    return std::unexpected(param.error(json::Fault::missing, "Promoted parameter requires 'targets'."));
                ATELIER_TRY_BIND(target_arr, targets_val->array());
                if (target_arr->size() == 0)
                    return std::unexpected(
                        targets_val->error(json::Fault::range, "Promoted parameter 'targets' must not be empty."));
                for (const auto target_item : *target_arr) {
                    ATELIER_TRY_BIND(tspec, target_item.string());
                    if (!valid_graph_target_path(*tspec))
                        return std::unexpected(
                            target_item.error(json::Fault::range, "Invalid promoted parameter target path."));
                    for (const auto prev : all_targets) {
                        if (graph_targets_overlap(prev, *tspec))
                            return std::unexpected(target_item.error(
                                json::Fault::range, "Promoted parameter targets cannot overlap on the same field."));
                    }
                    all_targets.push_back(*tspec);
                }
                if (const auto lbl = *param.find("label"))
                    ATELIER_TRY(lbl->string());
                if (const auto cat = *param.find("category"))
                    ATELIER_TRY(cat->string());
                if (const auto dsc = *param.find("description"))
                    ATELIER_TRY(dsc->string());
            }
        }
        if (const auto display = *settings.find("display")) {
            ATELIER_TRY_BIND(name, display->string());
            if (!valid_graph_identifier(*name))
                return std::unexpected(display->error(json::Fault::range, "The graph display must name one node."));
        }
        if (const auto tmpl = *settings.find("template")) {
            ATELIER_TRY_BIND(name, tmpl->string());
            if (!valid_graph_identifier(*name))
                return std::unexpected(tmpl->error(json::Fault::range, "The graph template must name one node."));
        }
        const auto members = entries.object_items();
        for (auto [name, entry] : *members) {
            if (!valid_graph_identifier(name))
                return std::unexpected(entry.error(json::Fault::range, "Invalid node name."));
            ATELIER_TRY(entry.allowed_members({"type", "asset", "overrides", "position", "properties", "inputs",
                                               "bypass", "shape", "color", "comment"}));
            ATELIER_TRY_BIND(node_type_sv, entry.string({"type"}));
            const bool is_asset_node = *node_type_sv == "procedural.AssetNode";
            const auto asset_field = *entry.find("asset");
            const auto overrides_field = *entry.find("overrides");
            if (!is_asset_node && (asset_field || overrides_field)) {
                return std::unexpected(entry.error(
                    json::Fault::members, "Only procedural.AssetNode entries may specify 'asset' or 'overrides'."));
            }
            if (asset_field) {
                ATELIER_TRY_BIND(asset_sv, asset_field->string());
                if (is_scratch_asset_key(*asset_sv))
                    return std::unexpected(
                        asset_field->error(json::Fault::range, "AssetNode cannot reference a scratch graph."));
            }
            if (overrides_field) {
                ATELIER_TRY_BIND(ov_items, overrides_field->object_items());
                for (auto [ov_key, _] : *ov_items) {
                    if (!valid_graph_identifier(ov_key))
                        return std::unexpected(
                            overrides_field->error(json::Fault::range, "Invalid override parameter identifier."));
                }
            }
            if (const auto position = *entry.find("position")) {
                ATELIER_TRY(assets::json_format::vector<2>(*position));
            }
            if (const auto bypass = *entry.find("bypass")) {
                ATELIER_TRY(bypass->boolean());
            }
            if (const auto shape = *entry.find("shape")) {
                ATELIER_TRY_BIND(shape_text, shape->string());
                if (!parse_graph_node_shape(*shape_text))
                    return std::unexpected(shape->error(json::Fault::range, "Unknown procedural node shape."));
            }
            if (const auto color = *entry.find("color")) {
                ATELIER_TRY_BIND(color_text, color->string());
                if (!parse_graph_hex_color(*color_text))
                    return std::unexpected(color->error(json::Fault::range, "Node color must be #RRGGBB."));
            }
            if (const auto comment = *entry.find("comment")) {
                ATELIER_TRY(comment->string());
            }
            if (const auto properties = *entry.find("properties");
                properties && properties->kind() != json::Kind::object)
                return std::unexpected(properties->error(json::Fault::type, "Properties must be a record."));
            const auto inputs = *entry.find("inputs");
            if (!inputs)
                continue;
            ATELIER_TRY_BIND(ports, inputs->object_items());
            for (auto [port, value] : *ports) {
                bool shaped = valid_graph_identifier(port);
                if (value.kind() == json::Kind::array) {
                    const auto connections = *value.array();
                    for (auto connection : connections)
                        shaped &= graph_endpoint(connection).has_value();
                } else
                    shaped &= graph_endpoint(value).has_value();
                if (!shaped)
                    return std::unexpected(
                        value.error(json::Fault::type, "A connection is \"node.output\", or an array of those."));
            }
        }
        const auto validate_canvas_box = [](json::Value item, std::string_view kind_label,
                                            std::initializer_list<std::string_view> fields) -> json::Result<void> {
            ATELIER_TRY(item.allowed_members(fields));
            if (const auto pos = *item.find("position"))
                ATELIER_TRY(assets::json_format::vector<2>(*pos));
            if (const auto sz = *item.find("size")) {
                ATELIER_TRY_BIND(v, assets::json_format::vector<2>(*sz));
                if ((*v)[0] <= 0.f || (*v)[1] <= 0.f)
                    return std::unexpected(
                        sz->error(json::Fault::range, std::string(kind_label) + " size must be positive."));
            }
            if (const auto title = *item.find("title"))
                ATELIER_TRY(title->string());
            if (const auto body = *item.find("text"))
                ATELIER_TRY(body->string());
            if (const auto color = *item.find("color")) {
                ATELIER_TRY_BIND(color_text, color->string());
                if (!parse_graph_hex_color(*color_text))
                    return std::unexpected(
                        color->error(json::Fault::range, std::string(kind_label) + " color must be #RRGGBB."));
            }
            return {};
        };
        for (auto [name, note] : *notes_json.object_items()) {
            if (!valid_graph_identifier(name))
                return std::unexpected(note.error(json::Fault::range, "Invalid sticky note name."));
            ATELIER_TRY(validate_canvas_box(note, "Sticky note", {"position", "size", "title", "text", "color"}));
        }
        for (auto [name, box] : *boxes_json.object_items()) {
            if (!valid_graph_identifier(name))
                return std::unexpected(box.error(json::Fault::range, "Invalid network box name."));
            ATELIER_TRY(validate_canvas_box(box, "Network box", {"position", "size", "title", "color"}));
        }
        return {};
    }();
    if (!validated)
        return std::unexpected(Error{ErrorCode::argument_mismatch, validated.error().describe()});
    const auto members = entries.object_items();

    ATELIER_TRY_BIND(graph_type, runtime.native_type<procedural::Graph>());
    GraphDocument document;
    json::Builder kept_settings;
    ATELIER_TRY_BIND(created, runtime.create(*graph_type));
    document.graph = std::move(*created);
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
        item.message = detail::dotted(field) + ": " + detail;
        item.field = std::move(field);
        document.issues.push_back(std::move(item));
    };
    // Leaves that do not decode are kept at their path under `kept` and reported under `prefix`.
    const auto keeping = [&](json::Builder& kept, json::Value target, ObjectId object, const std::string& type,
                             std::vector<std::string> prefix) {
        return PropertyReport{
            [&, target, object, type, prefix](std::vector<std::string> at, json::Value item, world::DiagnosticKind kind,
                                              const std::string& detail) -> Result<void> {
                const auto slot = kept.object_path_validated(target, at);
                kept.replace(slot, item);
                auto field = prefix;
                field.insert(field.end(), at.begin(), at.end());
                report(world::Severity::warning, kind, object, type, std::move(field), &item, detail);
                return {};
            },
            [&, object, type, prefix](std::vector<std::string> at, json::Value item) -> Result<void> {
                auto field = prefix;
                field.insert(field.end(), at.begin(), at.end());
                report(world::Severity::info, world::DiagnosticKind::converted, object, type, std::move(field), &item,
                       "Integral number converted exactly to the field's integer type.");
                return {};
            }};
    };
    json::Builder own(settings);
    own.erase(own.root(), "display");
    own.erase(own.root(), "template");
    // Parameters are public fields; graph membership, connections, display and template are hidden.
    ATELIER_TRY(
        decode_properties(runtime, {*graph_type, graph}, own.root(),
                          keeping(kept_settings, kept_settings.root(), graph, "procedural.Graph", {"settings"})));

    for (const auto& [key, entry] : *members) {
        const std::string name(key);
        const auto position = *entry.find("position");
        GraphNode item;
        item.name = name;
        item.position = position ? *assets::json_format::vector<2>(*position) : std::array<float, 2>{};
        if (const auto shape = *entry.find("shape"))
            item.shape = *parse_graph_node_shape(*shape->string());
        if (const auto color = *entry.find("color"))
            item.color = *parse_graph_hex_color(*color->string());
        if (const auto comment = *entry.find("comment"))
            item.comment = std::string(*comment->string());
        if (const auto asset_field = *entry.find("asset"))
            item.asset_key = std::string(*asset_field->string());
        if (const auto overrides_field = *entry.find("overrides"))
            item.overrides = json::Document(*overrides_field);

        const std::string type_name(*entry.string({"type"}));
        const auto type = runtime.find_type(type_name);
        auto added =
            type ? procedural::add_node(runtime, graph, *type) : Result<ObjectRoot>{std::unexpected(type.error())};
        // A type that is missing or cannot be a node is the file's problem; any other failure is the Runtime's.
        if (!added && added.error().code != ErrorCode::invalid_type &&
            added.error().code != ErrorCode::invalid_definition)
            return std::unexpected(added.error());
        if (!added) {
            item.kept = json::Document(entry);
            report(world::Severity::warning, world::DiagnosticKind::unknown_type, {}, type_name, {"nodes", name},
                   nullptr, added.error().message + " The node is kept.");
        } else {
            item.node = std::move(*added);
            if (const auto bypass = *entry.find("bypass")) {
                ATELIER_TRY(procedural::set_bypassed(runtime, item.node.id(), *bypass->boolean()));
            }
            if (type_name == "procedural.AssetNode" && asset_ports && !item.asset_key.empty()) {
                ATELIER_TRY(procedural::set_asset_subgraph(runtime, item.node.id(),
                                                           resolve_asset_binding(asset_ports, item.asset_key)));
            }
            json::Builder kept;
            const auto properties = kept.object_validated(kept.root(), "properties");
            ATELIER_TRY(
                decode_properties(runtime, {*type, item.node.id()}, entry.find("properties")->value_or(empty.root()),
                                  keeping(kept, properties, item.node.id(), type_name, {"nodes", name, "properties"})));
            if (!properties.size())
                kept.erase(kept.root(), "properties");
            item.kept = kept.finish();
        }
        document.nodes.push_back(std::move(item));
    }
    std::ranges::sort(document.nodes, {}, &GraphNode::name);

    for (const auto& [key, note] : *notes_json.object_items()) {
        GraphStickyNote item;
        item.name = std::string(key);
        if (const auto pos = *note.find("position"))
            item.position = *assets::json_format::vector<2>(*pos);
        if (const auto sz = *note.find("size"))
            item.size = *assets::json_format::vector<2>(*sz);
        if (const auto title = *note.find("title"))
            item.title = std::string(*title->string());
        if (const auto body = *note.find("text"))
            item.text = std::string(*body->string());
        if (const auto color = *note.find("color"))
            item.color = *parse_graph_hex_color(*color->string());
        document.notes.push_back(std::move(item));
    }
    std::ranges::sort(document.notes, {}, &GraphStickyNote::name);

    for (const auto& [key, box] : *boxes_json.object_items()) {
        GraphNetworkBox item;
        item.name = std::string(key);
        if (const auto pos = *box.find("position"))
            item.position = *assets::json_format::vector<2>(*pos);
        if (const auto sz = *box.find("size"))
            item.size = *assets::json_format::vector<2>(*sz);
        if (const auto title = *box.find("title"))
            item.title = std::string(*title->string());
        if (const auto color = *box.find("color"))
            item.color = *parse_graph_hex_color(*color->string());
        document.boxes.push_back(std::move(item));
    }
    std::ranges::sort(document.boxes, {}, &GraphNetworkBox::name);

    // Nodes are in sorted name order.
    const auto named = [&](std::string_view name) -> GraphNode* {
        const auto at = std::ranges::lower_bound(document.nodes, name, {},
                                                 [](const GraphNode& node) -> std::string_view { return node.name; });
        return at != document.nodes.end() && at->name == name ? &*at : nullptr;
    };
    for (auto& item : document.nodes) {
        const auto entry = *entries.at(item.name);
        const auto inputs = *entry.find("inputs");
        if (!item.node.registered() || !inputs)
            continue;
        SavedGraphPortMap saved;
        for (const auto& [port_name, value] : *inputs->object_items()) {
            std::vector<std::string> eps;
            const bool is_arr = value.kind() == json::Kind::array;
            if (is_arr) {
                for (const auto connection : *value.array())
                    eps.emplace_back(*connection.string());
            } else {
                eps.emplace_back(*value.string());
            }
            saved.emplace(std::string(port_name), std::pair{is_arr, std::move(eps)});
        }
        ATELIER_TRY(apply_saved_graph_node_inputs(runtime, document, item, saved, &document.issues));
    }

    if (parameters_opt) {
        for (const auto param : *parameters_opt->array()) {
            GraphPromotedParameter item;
            item.name = std::string(*param.string({"name"}));
            for (const auto t : *(*param.find("targets"))->array())
                item.targets.emplace_back(*t.string());
            if (const auto lbl = *param.find("label"))
                item.label = std::string(*lbl->string());
            if (const auto cat = *param.find("category"))
                item.category = std::string(*cat->string());
            if (const auto dsc = *param.find("description"))
                item.description = std::string(*dsc->string());

            std::optional<ResolvedGraphTarget> primary;
            std::vector<FieldRange> ranges;
            for (std::size_t ti = 0; ti < item.targets.size(); ++ti) {
                const auto& tspec = item.targets[ti];
                auto resolved = resolve_graph_parameter_target(runtime, document, tspec);
                if (!resolved) {
                    report(world::Severity::warning, world::DiagnosticKind::reference_dropped, graph,
                           "procedural.Graph", {"parameters", item.name, "targets"}, nullptr,
                           resolved.error().message + " The parameter binding is kept.");
                    continue;
                }
                if (!primary) {
                    primary = std::move(*resolved);
                    for (const auto& leaf : primary->leaves)
                        ranges.push_back(leaf.range);
                    continue;
                }
                if (resolved->value_type != primary->value_type || resolved->leaves.size() != primary->leaves.size()) {
                    report(world::Severity::warning, world::DiagnosticKind::mismatch, graph, "procedural.Graph",
                           {"parameters", item.name, "targets"}, nullptr,
                           "Target '" + tspec + "' does not match primary target '" + primary->node_name + "." +
                               primary->field_path + "' TypeId.");
                    continue;
                }
                bool range_ok = true;
                std::vector<FieldRange> next_ranges = ranges;
                for (std::size_t li = 0; li < ranges.size(); ++li) {
                    auto merged = intersect_field_ranges(next_ranges[li], resolved->leaves[li].range);
                    if (!merged) {
                        report(world::Severity::warning, world::DiagnosticKind::mismatch, graph, "procedural.Graph",
                               {"parameters", item.name, "targets"}, nullptr,
                               "Target '" + tspec + "': " + merged.error().message);
                        range_ok = false;
                        break;
                    }
                    next_ranges[li] = *merged;
                }
                if (!range_ok)
                    continue;
                ranges = std::move(next_ranges);

                // Synchronize secondary target leaf values to the primary target's value.
                for (std::size_t li = 0; li < primary->leaves.size(); ++li) {
                    const auto& src_leaf = primary->leaves[li];
                    const auto& dst_leaf = resolved->leaves[li];
                    if (src_leaf.kind == TypeKind::string) {
                        String str_val;
                        ATELIER_TRY(runtime.read(primary->node, src_leaf.path, ValueOutput{src_leaf.type, &str_val}));
                        ATELIER_TRY(runtime.write(resolved->node, dst_leaf.path, ValueInput{dst_leaf.type, &str_val}));
                    } else {
                        ATELIER_TRY_BIND(val, runtime.read(primary->node, src_leaf.path));
                        const auto [synced, clamped] = detail::clamp_scalar(*val, ranges[li]);
                        if (clamped) {
                            if (named(primary->node_name)) {
                                ATELIER_TRY_BIND(ptype, runtime.type_of(primary->node));
                                ATELIER_TRY_BIND(pinfo, runtime.describe(*ptype));
                                std::vector<std::string> fpath = {"nodes", primary->node_name, "properties"};
                                fpath.insert(fpath.end(), src_leaf.names.begin(), src_leaf.names.end());
                                report(world::Severity::warning, world::DiagnosticKind::invalid_value, primary->node,
                                       (*pinfo)->name, std::move(fpath), nullptr,
                                       "Primary target value clamped to promoted parameter '" + item.name +
                                           "' intersected hard bounds.");
                            }
                            ATELIER_TRY(runtime.write(primary->node, src_leaf.path, synced));
                        }
                        ATELIER_TRY(runtime.write(resolved->node, dst_leaf.path, synced));
                    }
                }
            }
            document.parameters.push_back(std::move(item));
        }
    }

    if (const auto at = *settings.find("display")) {
        const auto* shown = named(*at->string());
        if (shown && shown->node.registered()) {
            ATELIER_TRY(procedural::set_display(runtime, graph, shown->node.id()));
        } else {
            kept_settings.set_validated(kept_settings.root(), "display", *at);
            report(world::Severity::warning, world::DiagnosticKind::reference_dropped, graph, "procedural.Graph",
                   {"settings", "display"}, &*at, "No loaded node has this name. The display is kept.");
        }
    }
    if (const auto at = *settings.find("template")) {
        const auto* shown = named(*at->string());
        if (shown && shown->node.registered()) {
            document.template_node = shown->node.id();
        } else {
            kept_settings.set_validated(kept_settings.root(), "template", *at);
            report(world::Severity::warning, world::DiagnosticKind::reference_dropped, graph, "procedural.Graph",
                   {"settings", "template"}, &*at, "No loaded node has this name. The template is kept.");
        }
    }
    document.kept = kept_settings.finish();
    return document;
}

Result<std::string> save_graph(Runtime& runtime, const GraphDocument& document) {
    const auto graph = document.graph.id();
    ATELIER_TRY_BIND(content_result, procedural::contents(runtime, graph));
    const auto& contents = *content_result;
    std::map<std::string_view, const GraphNode*> by_name;
    Map<ObjectId, const GraphNode*> by_node;
    by_node.reserve(contents.nodes().size());
    for (const auto& item : document.nodes) {
        if (!valid_graph_identifier(item.name) || !by_name.emplace(item.name, &item).second)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Graph node names must be valid and unique: " + item.name});
        if (!std::isfinite(item.position[0]) || !std::isfinite(item.position[1]))
            return std::unexpected(Error{ErrorCode::argument_mismatch, item.name + ": node positions must be finite."});
        if (static_cast<std::uint8_t>(item.shape) > static_cast<std::uint8_t>(GraphNodeShape::circle))
            return std::unexpected(Error{ErrorCode::argument_mismatch, item.name + ": invalid node shape preset."});
        if (!StringView(item.comment).is_valid_utf8() || !StringView(item.asset_key).is_valid_utf8())
            return std::unexpected(Error{ErrorCode::argument_mismatch, item.name + ": node strings must be UTF-8."});
        if (is_scratch_asset_key(item.asset_key))
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, item.name + ": AssetNode cannot save a scratch graph asset key."});
        const auto saved_type = item.kept.root().find("type");
        if (item.node.registered() ? !by_node.try_emplace(item.node.id(), &item).inserted : !saved_type || !*saved_type)
            return std::unexpected(Error{ErrorCode::argument_mismatch,
                                         item.name + ": an entry names a loaded node once, or keeps a saved one."});
        if (!item.overrides.root().is_null() && item.overrides.root().kind() != json::Kind::object)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, item.name + ": node overrides must be a JSON object."});
        if (item.overrides.root().kind() == json::Kind::object) {
            for (const auto& [ov_key, _] : *item.overrides.root().object_items()) {
                if (!valid_graph_identifier(ov_key))
                    return std::unexpected(
                        Error{ErrorCode::argument_mismatch, item.name + ": invalid override parameter identifier."});
            }
        }
    }
    if (by_node.size() != contents.nodes().size() ||
        !std::ranges::all_of(contents.nodes(), [&](ObjectId node) { return by_node.contains(node); }))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Every graph node needs exactly one document entry."});
    if (document.template_node != ObjectId{} && !by_node.contains(document.template_node))
        return std::unexpected(
            Error{ErrorCode::argument_mismatch, "Graph template node must belong to the saved graph."});

    std::set<std::string_view> seen_param_names;
    std::vector<std::string_view> seen_targets;
    for (const auto& param : document.parameters) {
        if (!valid_graph_identifier(param.name) || !seen_param_names.insert(param.name).second)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Invalid or duplicate promoted parameter name: " + param.name});
        if (param.targets.empty())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Promoted parameter '" + param.name + "' has no targets."});
        if (!StringView(param.label).is_valid_utf8() || !StringView(param.category).is_valid_utf8() ||
            !StringView(param.description).is_valid_utf8())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Promoted parameter metadata must be valid UTF-8: " + param.name});
        for (const auto& target : param.targets) {
            if (!valid_graph_target_path(target))
                return std::unexpected(
                    Error{ErrorCode::argument_mismatch, "Invalid promoted parameter target path: " + target});
            for (const auto prev : seen_targets) {
                if (graph_targets_overlap(prev, target))
                    return std::unexpected(
                        Error{ErrorCode::argument_mismatch, "Overlapping promoted parameter target path: " + target});
            }
            seen_targets.push_back(target);
        }
    }

    const auto valid_box_rect = [](const std::array<float, 2>& pos, const std::array<float, 2>& sz) noexcept {
        return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(sz[0]) && std::isfinite(sz[1]) &&
               sz[0] > 0.f && sz[1] > 0.f;
    };
    std::map<std::string_view, const GraphStickyNote*> notes_by_name;
    for (const auto& note : document.notes) {
        if (!valid_graph_identifier(note.name) || !notes_by_name.emplace(note.name, &note).second)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Sticky note names must be valid and unique: " + note.name});
        if (!valid_box_rect(note.position, note.size))
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, note.name + ": sticky note position and size must be valid."});
        if (!StringView(note.title).is_valid_utf8() || !StringView(note.text).is_valid_utf8())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, note.name + ": sticky note text must be UTF-8."});
    }

    std::map<std::string_view, const GraphNetworkBox*> boxes_by_name;
    for (const auto& box : document.boxes) {
        if (!valid_graph_identifier(box.name) || !boxes_by_name.emplace(box.name, &box).second)
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, "Network box names must be valid and unique: " + box.name});
        if (!valid_box_rect(box.position, box.size))
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, box.name + ": network box position and size must be valid."});
        if (!StringView(box.title).is_valid_utf8())
            return std::unexpected(
                Error{ErrorCode::argument_mismatch, box.name + ": network box title must be UTF-8."});
    }

    // contents/inputs guarantee membership; the coverage above guarantees every by_node lookup below.
    const EncodeReference no_reference = [](ObjectId) -> Result<json::Document> {
        return std::unexpected(Error{ErrorCode::unsupported_operation, "Graph files keep no object references."});
    };
    ATELIER_TRY_BIND(graph_type, runtime.type_of(graph));
    ATELIER_TRY_BIND(graph_fields, detail::author_fields(runtime, *graph_type));
    ATELIER_TRY_BIND(settings, encode_properties(runtime, graph, *graph_fields, no_reference));
    json::Builder result;
    static_cast<void>(result.set(result.root(), "version", graph_format_version));
    result.set_validated(result.root(), "settings", settings->root());
    const auto settings_node = *result.root().at("settings");
    if (contents.display() != ObjectId{})
        result.set_validated(settings_node, "display", (*by_node.find(contents.display()))->name);
    if (document.template_node != ObjectId{})
        result.set_validated(settings_node, "template", (*by_node.find(document.template_node))->name);
    assets::json_format::overlay(result, settings_node, document.kept.root());

    if (!document.parameters.empty()) {
        const auto params_arr = *result.array(result.root(), "parameters");
        for (const auto& param : document.parameters) {
            const auto entry = result.append_object(params_arr);
            result.set_validated(entry, "name", param.name);
            const auto targets_arr = *result.array(entry, "targets");
            for (const auto& target : param.targets)
                static_cast<void>(result.append(targets_arr, target));
            if (!param.label.empty())
                result.set_validated(entry, "label", param.label);
            if (!param.category.empty())
                result.set_validated(entry, "category", param.category);
            if (!param.description.empty())
                result.set_validated(entry, "description", param.description);
        }
    }

    if (!boxes_by_name.empty()) {
        const auto boxes = result.object_validated(result.root(), "boxes");
        for (const auto& [name, box] : boxes_by_name) {
            const auto entry = result.object_validated(boxes, name);
            result.set_validated(entry, "color", format_graph_hex_color(box->color));
            ATELIER_TRY(append_vec2(result, entry, "position", box->position));
            ATELIER_TRY(append_vec2(result, entry, "size", box->size));
            if (auto added = result.set(entry, "title", box->title); !added)
                return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
        }
    }

    const auto nodes = result.object_validated(result.root(), "nodes");
    for (const auto& [name, item] : by_name) {
        const auto entry = result.object_validated(nodes, name);
        if (item->node.registered()) {
            const auto node = item->node.id();
            ATELIER_TRY_BIND(type, runtime.type_of(node));
            ATELIER_TRY_BIND(port_result, procedural::ports(runtime, node));
            const auto& ports = *port_result;
            const auto inputs = result.object_validated(entry, "inputs");
            ATELIER_TRY_BIND(connections, procedural::inputs(runtime, contents, node));
            for (const auto& input : *connections) {
                ATELIER_TRY_BIND(from, procedural::ports(runtime, input.from));
                if (input.output >= from->outputs.size())
                    return std::unexpected(
                        Error{ErrorCode::argument_mismatch, "Graph input names a missing output port."});
                auto connection = (*by_node.find(input.from))->name + "." + from->outputs[input.output];
                const auto& port_name = ports.inputs.at(input.input);
                if (ports.multiple[input.input]) {
                    const auto found = *inputs.find(port_name);
                    const auto port = found ? *found : *result.array(inputs, port_name);
                    if (auto added = result.append(port, connection); !added)
                        return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
                } else if (auto added = result.set(inputs, port_name, connection); !added)
                    return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
            }
            ATELIER_TRY_BIND(fields, detail::author_fields(runtime, *type));
            ATELIER_TRY_BIND(properties, encode_properties(runtime, node, *fields, no_reference));
            ATELIER_TRY_BIND(info, runtime.describe(*type));
            result.set_validated(entry, "type", (*info)->name);
            result.set_validated(entry, "properties", properties->root());
            if ((*info)->name == "procedural.AssetNode") {
                if (!item->asset_key.empty())
                    result.set_validated(entry, "asset", item->asset_key);
                if (item->overrides.root().kind() == json::Kind::object && item->overrides.root().size() > 0)
                    result.set_validated(entry, "overrides", item->overrides.root());
            } else if (!item->asset_key.empty() ||
                       (item->overrides.root().kind() == json::Kind::object && item->overrides.root().size() > 0)) {
                return std::unexpected(Error{ErrorCode::argument_mismatch,
                                             item->name + ": only procedural.AssetNode may save 'asset'/'overrides'."});
            }
            ATELIER_TRY_BIND(is_bypassed, procedural::bypassed(runtime, node));
            if (*is_bypassed) {
                if (auto added = result.set(entry, "bypass", true); !added)
                    return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
            }
        }
        assets::json_format::overlay(result, entry, item->kept.root());
        result.erase(entry, "position");
        result.erase(entry, "shape");
        result.erase(entry, "color");
        result.erase(entry, "comment");
        ATELIER_TRY(append_vec2(result, entry, "position", item->position));
        if (const auto shape_name = graph_node_shape_name(item->shape); !shape_name.empty())
            result.set_validated(entry, "shape", shape_name);
        if (item->color)
            result.set_validated(entry, "color", format_graph_hex_color(*item->color));
        if (!item->comment.empty()) {
            if (auto added = result.set(entry, "comment", item->comment); !added)
                return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
        }
    }

    if (!notes_by_name.empty()) {
        const auto notes = result.object_validated(result.root(), "notes");
        for (const auto& [name, note] : notes_by_name) {
            const auto entry = result.object_validated(notes, name);
            result.set_validated(entry, "color", format_graph_hex_color(note->color));
            ATELIER_TRY(append_vec2(result, entry, "position", note->position));
            ATELIER_TRY(append_vec2(result, entry, "size", note->size));
            if (auto added = result.set(entry, "text", note->text); !added)
                return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
            if (auto added = result.set(entry, "title", note->title); !added)
                return std::unexpected(Error{ErrorCode::argument_mismatch, added.error().describe()});
        }
    }

    const auto bytes = json::write(result.root(), {true, true});
    return std::string(bytes.data(), bytes.size());
}

} // namespace atelier::editor
