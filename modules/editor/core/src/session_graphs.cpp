#include "editor/session.hpp"
#include "editor/fields.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "editor/project_code.hpp"
#include "editor/properties.hpp"
#include "foundation/map.hpp"
#include "foundation/scope_exit.hpp"
#include "foundation/set.hpp"
#include "platform/utf.hpp"
#include "procedural/bvh.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/graph.hpp"
#include "procedural/instances.hpp"
#include "procedural/nodes.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>

namespace atelier::editor {
using namespace atelier::literals;

namespace {

auto graph_error(Utf8Text message, ErrorCode code = ErrorCode::argument_mismatch) {
    return std::unexpected(Error{code, std::move(message)});
}

String lossy_utf8(StringView text) {
    return String::from_utf8(text).value_or("<invalid utf-8>"_s);
}

String default_node_stem(StringView type_name) {
    if (const auto dot = type_name.rfind('.'); dot != StringView::npos)
        type_name = type_name.slice(dot + 1);
    String stem;
    for (std::size_t i = 0; i < type_name.size(); ++i) {
        const char c = type_name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0 && type_name[i - 1] >= 'a' && type_name[i - 1] <= 'z')
                stem.append_ascii('_');
            stem.append_ascii(static_cast<char>(c - 'A' + 'a'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            stem.append_ascii(c);
        }
    }
    if (stem.empty() || (stem[0] >= '0' && stem[0] <= '9'))
        stem = String::concat("n", stem);
    return stem;
}

const EncodeReference no_graph_references = [](ObjectId) -> Result<json::Document> {
    return graph_error("Graph files keep no object references.", ErrorCode::unsupported_operation);
};

Result<json::Document> capture_object_properties(Runtime& runtime, ObjectId object) {
    ATELIER_TRY_BIND(type, runtime.type_of(object));
    ATELIER_TRY_BIND(fields, detail::author_fields(runtime, *type));
    return encode_properties(runtime, object, *fields, no_graph_references);
}

Result<void> apply_object_properties(Runtime& runtime, ObjectId object, json::Value properties) {
    ATELIER_TRY_BIND(type, runtime.type_of(object));
    return decode_properties(runtime, {*type, object}, properties);
}

Result<void> replace_node_inputs(Runtime& runtime, ObjectId graph, ObjectId to,
                                 Span<const procedural::Input> desired) {
    ATELIER_TRY_BIND(listed, procedural::contents(runtime, graph));
    ATELIER_TRY_BIND(current, procedural::inputs(runtime, *listed, to));
    for (auto it = current->rbegin(); it != current->rend(); ++it)
        ATELIER_TRY(procedural::disconnect(runtime, it->from, it->output, to, it->input));
    for (const auto& item : desired)
        ATELIER_TRY(procedural::connect(runtime, graph, item.from, item.output, to, item.input));
    return {};
}

class GraphResources final : public procedural::Resources {
public:
    void bind(Project* project) noexcept {
        if (project_ != project) {
            project_ = project;
            cached_.clear();
            cached_revision_ = 0;
            ++epoch_;
        }
    }

    [[nodiscard]] Project* project() const noexcept { return project_; }

    void bind_temporary_reader(assets::Reader* reader) noexcept {
        if (temporary_reader_ != reader) {
            temporary_reader_ = reader;
            cached_.clear();
            cached_revision_ = 0;
            ++epoch_;
        }
    }

    void invalidate() noexcept {
        cached_.clear();
        cached_revision_ = 0;
        ++epoch_;
    }

    [[nodiscard]] Result<assets::MeshAsset> resolve_mesh_asset(StringView key) const {
        assets::Reader& active = temporary_reader_ ? *temporary_reader_ : project_ ? project_->assets() : builtin_;
        const std::uint64_t rev = active.revision();
        if (cached_revision_ != rev) {
            cached_.clear();
            cached_revision_ = rev;
        }
        if (const auto* cached = cached_.find(key))
            return *cached;
        ATELIER_TRY_BIND(resolved, active.mesh(key));
        // Successful reads identify a built-in mesh or a validated catalog entry.
        cached_.try_emplace(String::from_utf8_validated(key), *resolved);
        return *resolved;
    }

    [[nodiscard]] Result<geometry::MeshRef> mesh(StringView key) const noexcept override {
        ATELIER_TRY_BIND(resolved, resolve_mesh_asset(key));
        return std::move(resolved->mesh);
    }

    [[nodiscard]] std::uint64_t revision() const noexcept override {
        const std::uint64_t reader_rev = temporary_reader_ ? temporary_reader_->revision()
                                         : project_        ? project_->revision()
                                                           : 0ULL;
        return (epoch_ << 32) ^ reader_rev;
    }

private:
    Project* project_{};

    assets::Reader* temporary_reader_{};

    mutable assets::Reader builtin_{};

    mutable Map<String, assets::MeshAsset> cached_;

    mutable std::uint64_t cached_revision_{};

    std::uint64_t epoch_{1};
};

struct GraphScalarChange {
    ObjectRoot target;

    Array<world::FieldPath> paths;

    Array<Scalar> before;

    Array<Scalar> after;

    json::Document before_kept;

    json::Document after_kept;
};

struct GraphPropertyChange {
    ObjectRoot target;

    json::Document before;

    json::Document after;

    json::Document before_kept;

    json::Document after_kept;
};

struct GraphDisplayChange {
    ObjectId before{};

    ObjectId after{};

    json::Document before_kept;

    json::Document after_kept;
};

struct GraphTemplateChange {
    ObjectId before{};

    ObjectId after{};

    json::Document before_kept;

    json::Document after_kept;
};

struct GraphBypassChange {
    ObjectRoot target;

    bool before{};

    bool after{};
};

struct GraphFreezeChange {
    ObjectRoot target;

    std::shared_ptr<const procedural::FrozenOutput> snapshot;

    bool enabled_after{};
};

struct GraphAppearanceChange {
    struct Entry {
        String name;

        GraphNodeShape before_shape{GraphNodeShape::auto_role};

        GraphNodeShape after_shape{GraphNodeShape::auto_role};

        Optional<std::uint32_t> before_color{};

        Optional<std::uint32_t> after_color{};

        String before_comment{};

        String after_comment{};
    };

    Array<Entry> entries;
};

struct GraphAnnotationChange {
    Array<GraphStickyNote> before_notes;

    Array<GraphStickyNote> after_notes;

    Array<GraphNetworkBox> before_boxes;

    Array<GraphNetworkBox> after_boxes;
};

struct GraphRenameChange {
    String before;

    String after;

    struct Retained {
        String owner; // Empty for graph settings. Node names are recorded before the rename.

        json::Document before;

        json::Document after;
    };

    Array<Retained> references;

    Array<GraphPromotedParameter> before_parameters{};

    Array<GraphPromotedParameter> after_parameters{};
};

struct GraphMoveChange {
    struct Move {
        String name;

        std::array<float, 2> before{};

        std::array<float, 2> after{};
    };

    Array<Move> moves;

    Array<Move> note_moves;

    Array<Move> box_moves;
};

struct GraphConnectionChange {
    struct TargetDelta {
        ObjectRoot target;

        Array<procedural::Input> before;

        Array<procedural::Input> after;

        json::Document before_kept;

        json::Document after_kept;
    };

    Array<TargetDelta> targets;

    Array<GraphMoveChange::Move> moves{};
};

struct GraphNodesChange {
    struct SavedInputs {
        ObjectId to{};

        Array<procedural::Input> inputs;

        Array<procedural::Input> detached_inputs{};
    };

    struct RetainedNodeKept {
        String owner;

        json::Document before;

        json::Document after;
    };

    bool added{};

    Array<GraphNode> entries;

    Array<SavedInputs> saved_inputs;

    ObjectId before_display{};

    ObjectId after_display{};

    ObjectId before_template{};

    ObjectId after_template{};

    json::Document before_kept;

    json::Document after_kept;

    Array<RetainedNodeKept> retained_node_kept{};

    Array<GraphPromotedParameter> before_parameters{};

    Array<GraphPromotedParameter> after_parameters{};
};

struct GraphParameterSchemaChange {
    struct NodeSnapshot {
        ObjectRoot target;

        json::Document before_props;

        json::Document after_props;

        json::Document before_kept;

        json::Document after_kept;
    };

    Array<GraphPromotedParameter> before_parameters;

    Array<GraphPromotedParameter> after_parameters;

    Array<NodeSnapshot> synced_nodes{};
};

struct GraphAssetOverrideChange {
    String node_name;

    json::Document before_overrides;

    json::Document after_overrides;

    String before_asset{};

    String after_asset{};

    bool asset_changed{};

    procedural::Ports before_ports{};

    procedural::Ports after_ports{};

    Array<GraphConnectionChange::TargetDelta> connection_deltas{};
};

struct GraphExtractChange {
    Array<GraphNode> removed_nodes;

    GraphNode added_asset_node;

    Array<GraphNodesChange::SavedInputs> before_inputs;

    Array<GraphNodesChange::SavedInputs> after_inputs;

    ObjectId before_display{};

    ObjectId after_display{};

    ObjectId before_template{};

    ObjectId after_template{};

    Array<GraphNodesChange::RetainedNodeKept> retained_node_kept{};

    Array<GraphPromotedParameter> before_parameters{};

    Array<GraphPromotedParameter> after_parameters{};
};

using GraphChange = std::variant<GraphScalarChange, GraphPropertyChange, GraphDisplayChange, GraphTemplateChange,
                                 GraphBypassChange, GraphFreezeChange, GraphAppearanceChange, GraphAnnotationChange,
                                 GraphRenameChange, GraphMoveChange, GraphConnectionChange, GraphNodesChange,
                                 GraphParameterSchemaChange, GraphAssetOverrideChange, GraphExtractChange>;

void erase_kept_input(json::Document& kept, StringView port) {
    json::Builder next_kept(kept);
    if (const auto kept_inputs = *next_kept.root().find("inputs");
        kept_inputs && kept_inputs->kind() == json::Kind::object) {
        next_kept.erase(*kept_inputs, port);
        if (!kept_inputs->size())
            next_kept.erase(next_kept.root(), "inputs");
    }
    kept = next_kept.finish();
}

void sort_graph_nodes(Array<GraphNode>& nodes) {
    std::ranges::sort(nodes, {}, &GraphNode::name);
}

void sort_graph_notes(Array<GraphStickyNote>& notes) {
    std::ranges::sort(notes, {}, &GraphStickyNote::name);
}

void sort_graph_boxes(Array<GraphNetworkBox>& boxes) {
    std::ranges::sort(boxes, {}, &GraphNetworkBox::name);
}

GraphNode* find_node(GraphDocument& doc, StringView name) noexcept {
    const auto at = std::ranges::lower_bound(doc.nodes, name, {}, &GraphNode::name);
    return at != doc.nodes.end() && at->name == name ? &*at : nullptr;
}

GraphStickyNote* find_note(GraphDocument& doc, StringView name) noexcept {
    const auto at = std::ranges::lower_bound(doc.notes, name, {}, &GraphStickyNote::name);
    return at != doc.notes.end() && at->name == name ? &*at : nullptr;
}

GraphNetworkBox* find_box(GraphDocument& doc, StringView name) noexcept {
    const auto at = std::ranges::lower_bound(doc.boxes, name, {}, &GraphNetworkBox::name);
    return at != doc.boxes.end() && at->name == name ? &*at : nullptr;
}

GraphNode* find_node_by_id(GraphDocument& doc, ObjectId id) noexcept {
    const auto at = std::ranges::find_if(
        doc.nodes, [&](const GraphNode& item) { return item.node.registered() && item.node.id() == id; });
    return at == doc.nodes.end() ? nullptr : &*at;
}

const GraphNode* find_node_by_id(const GraphDocument& doc, ObjectId id) noexcept {
    const auto at = std::ranges::find_if(
        doc.nodes, [&](const GraphNode& item) { return item.node.registered() && item.node.id() == id; });
    return at == doc.nodes.end() ? nullptr : &*at;
}

String resolve_root_error_node(const GraphDocument& doc, const procedural::NodeStatus& st, ObjectId self_id,
                               const String& fallback_name = {}) {
    const ObjectId cause = st.root_node != ObjectId{} ? st.root_node : self_id;
    if (const auto* cause_node = find_node_by_id(doc, cause))
        return cause_node->name;
    if (!fallback_name.empty())
        return fallback_name;
    if (const auto* self_node = find_node_by_id(doc, self_id))
        return self_node->name;
    return {};
}

Result<Array<String>> format_live_port_endpoints(Runtime& runtime, const GraphDocument& doc,
                                                 Span<const procedural::Input> node_inputs,
                                                 std::uint32_t port_index) {
    Array<String> out;
    for (const auto& in : node_inputs) {
        if (in.input != port_index)
            continue;
        const auto* src = find_node_by_id(doc, in.from);
        if (!src)
            continue;
        ATELIER_TRY_BIND(src_ports, procedural::ports(runtime, in.from));
        if (in.output < src_ports->outputs.size())
            out.add(String::concat(src->name, ".", src_ports->outputs[in.output]));
    }
    return out;
}

template <class Pred>
bool kept_port_has_match(const json::Document& kept, StringView port_filter, Pred&& matches) {
    const auto cur_inputs = *kept.root().find("inputs");
    if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
        return false;
    const auto check_val = [&](json::Value val) {
        if (const auto s = val.string())
            return matches(*s);
        if (val.kind() == json::Kind::array) {
            for (const auto elem : *val.array())
                if (const auto s = elem.string(); s && matches(*s))
                    return true;
        }
        return false;
    };
    if (!port_filter.empty()) {
        const auto cur_port = *cur_inputs->find(port_filter);
        return cur_port && check_val(*cur_port);
    }
    for (const auto& [_, val] : *cur_inputs->object_items()) {
        if (check_val(val))
            return true;
    }
    return false;
}

// Known ports keep their full ordered input list only while some endpoints are unresolved.
template <class Edit>
void edit_kept_port(json::Document& kept, StringView port, bool multiple, Span<const String> live_after,
                    Edit edit) {
    const auto inputs = *kept.root().find("inputs");
    if (!inputs)
        return;
    const auto value = *inputs->find(port);
    if (!value)
        return;
    const bool array = value->kind() == json::Kind::array;
    Array<String> items;
    if (array) {
        for (const auto item : *value->array())
            items.add(String::from_utf8_validated(*item.string()));
    } else {
        items.add(String::from_utf8_validated(*value->string()));
    }
    if (!edit(items, array))
        return;
    if (items.empty() || (multiple && std::ranges::equal(items, live_after))) {
        erase_kept_input(kept, port);
        return;
    }
    json::Builder next(kept);
    const auto record = *next.root().at("inputs");
    next.erase(record, port);
    const auto output = *next.array(record, port);
    for (const auto& item : items)
        static_cast<void>(next.append(output, item));
    kept = next.finish();
}

template <class Pred>
void filter_kept_port_endpoints(json::Document& kept, StringView port, bool multiple,
                                Span<const String> live_after, Pred should_remove) {
    edit_kept_port(kept, port, multiple, live_after,
                   [&](auto& items, bool) { return items.remove_if(should_remove) != 0; });
}

void append_kept_port_endpoint(json::Document& kept, StringView port, bool multiple, const String& endpoint,
                               Span<const String> live_after) {
    edit_kept_port(kept, port, multiple, live_after, [&](auto& items, bool array) {
        if (multiple && array) {
            // connect() appends even if this endpoint already had an unresolved slot on disk.
            items.remove_if([&](const String& s) { return s == endpoint; });
            items.add(endpoint);
        } else {
            items.clear();
        }
        return true;
    });
}

void replace_kept_port_endpoint(json::Document& kept, StringView port, bool multiple,
                                const String& old_endpoint, const String& new_endpoint, bool already_live,
                                Span<const String> live_after) {
    edit_kept_port(kept, port, multiple, live_after, [&](auto& items, bool array) {
        if (!multiple || !array) {
            items.clear();
            return true;
        }
        if (!already_live) {
            items.remove_if([&](const String& s) { return s == new_endpoint; });
            if (auto slot = std::ranges::find(items, old_endpoint); slot != items.end())
                *slot = new_endpoint;
        }
        items.remove_if([&](const String& s) { return s == old_endpoint; });
        return true;
    });
}

String format_number(double v) {
    if (!std::isfinite(v))
        return std::isnan(v) ? "nan"_s : (v < 0 ? "-inf"_s : "inf"_s);
    if (std::abs(v) < 1e-9)
        v = 0.0;
    return String::from_float_general(v, 4);
}

template <class T>
void summarize_numeric_lanes(Span<const T> values, std::uint32_t lanes, auto&& lane_at, GraphAttributeStats& out,
                             std::size_t max_samples) {
    out.components = lanes;
    out.count = values.size();
    if constexpr (std::same_as<T, bool>)
        out.selected_count = 0;
    if (values.empty())
        return;
    out.minimum.assign(lanes, std::numeric_limits<double>::infinity());
    out.maximum.assign(lanes, -std::numeric_limits<double>::infinity());
    out.mean.assign(lanes, 0.0);
    std::size_t finite_rows = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        bool row_finite = true;
        for (std::uint32_t c = 0; c < lanes; ++c) {
            const double v = static_cast<double>(lane_at(values[i], c));
            if (!std::isfinite(v))
                row_finite = false;
        }
        if (!row_finite) {
            ++out.non_finite_count;
        } else {
            ++finite_rows;
            if constexpr (std::same_as<T, bool>)
                *out.selected_count += values[i];
            for (std::uint32_t c = 0; c < lanes; ++c) {
                const double v = static_cast<double>(lane_at(values[i], c));
                out.minimum[c] = std::min(out.minimum[c], v);
                out.maximum[c] = std::max(out.maximum[c], v);
                out.mean[c] += v;
            }
        }
        if (i < max_samples) {
            if (lanes == 1) {
                out.sample_values.add(format_number(static_cast<double>(lane_at(values[i], 0))));
            } else {
                String tuple = "["_s;
                for (std::uint32_t c = 0; c < lanes; ++c) {
                    if (c)
                        tuple.append_literal<", ">();
                    tuple += format_number(static_cast<double>(lane_at(values[i], c)));
                }
                tuple.append_literal<"]">();
                out.sample_values.add(std::move(tuple));
            }
        }
    }
    if (finite_rows > 0) {
        for (std::uint32_t c = 0; c < lanes; ++c)
            out.mean[c] /= static_cast<double>(finite_rows);
    } else {
        out.minimum.clear();
        out.maximum.clear();
        out.mean.clear();
    }
}

void detach_asset_node_subgraph(Runtime& runtime, ObjectId node_id) noexcept {
    if (node_id == ObjectId{})
        return;
    const auto asset_type = runtime.native_type<procedural::AssetNode>();
    if (!asset_type || runtime.type_of(node_id) != *asset_type)
        return;
    if (auto sub = procedural::asset_subgraph(runtime, node_id)) {
        if (sub->graph.registered()) {
            sub->graph = {};
            sub->input_nodes.clear();
            sub->output_nodes.clear();
            (void)procedural::set_asset_subgraph(runtime, node_id, std::move(*sub));
        }
    }
}

struct CachedPromotedParam {
    String name;

    ResolvedPromotedSchema schema;

    std::variant<Array<Scalar>, String> default_value;
};

struct BoundAssetInstance {
    Runtime* owner_runtime{};

    String asset_key;

    std::uint64_t source_epoch{};

    std::uint64_t code_version{};

    json::Document applied_overrides;

    bool overrides_initialized{};

    std::uint64_t overrides_hash{};

    std::uint64_t content_revision{};

    GraphDocument cloned_doc;

    Array<CachedPromotedParam> cached_params;

    Array<world::Diagnostic> override_issues;

    Map<ObjectId, std::unique_ptr<BoundAssetInstance>> children;

    ~BoundAssetInstance() {
        children.clear();
        if (owner_runtime)
            destroy_private_graph_document(*owner_runtime, cloned_doc);
    }
};

} // namespace

struct Session::OpenGraph {
    String key;

    bool scratch{};

    GraphDocument document;

    String saved_text;

    using Step = std::variant<GraphChange, Array<GraphChange>>;

    Array<Step> history;

    Array<GraphChange>* pending{};

    Result<void> replay(Session& session, GraphChange& change, bool redo);

    Result<void> replay(Session& session, Span<GraphChange> changes, bool redo);

    std::size_t cursor{};

    GraphResources resources;

    const source::Program* bound_program{};

    std::unique_ptr<procedural::Evaluator> evaluator;

    Map<ObjectId, std::unique_ptr<BoundAssetInstance>> asset_instances;

    Array<world::Diagnostic> asset_issues;

    std::shared_ptr<const GraphInspectView> topology;

    std::uint64_t evaluation_generation{};

    std::uint64_t topology_revision{};

    std::uint64_t topology_generation{};

    std::uint64_t shallow_ports_revision{};

    std::uint64_t mutation_epoch{1};

    mutable std::uint64_t dirty_revision{};

    mutable bool dirty_cached{};

    void push_change(GraphChange change) {
        if (pending)
            pending->add(std::move(change));
        else
            publish(std::move(change));
    }

    void publish(Step change) {
        history.truncate(cursor);
        history.add(std::move(change));
        ++cursor;
        if (history.size() > Session::history_limit) {
            history.remove_at(0);
            --cursor;
        }
        ++mutation_epoch;
        dirty_revision = 0;
    }

    bool owns(ObjectId id) const noexcept {
        if (id == ObjectId{})
            return false;
        if (document.graph.registered() && document.graph.id() == id)
            return true;
        return find_node_by_id(document, id) != nullptr;
    }
};

struct Session::GeneratorRuntimeCache {
    struct GraphSlot {
        Runtime* owner_runtime{};

        Optional<GraphDocument> closed_doc;

        std::uint64_t closed_project_revision{};

        ObjectId bound_graph_id{};

        const source::Program* bound_program{};

        std::uint64_t code_version{};

        std::unique_ptr<procedural::Evaluator> evaluator;

        Map<ObjectId, std::unique_ptr<BoundAssetInstance>> asset_instances;

        GraphSlot() = default;

        GraphSlot(const GraphSlot&) = delete;

        GraphSlot& operator=(const GraphSlot&) = delete;

        GraphSlot(GraphSlot&& other) noexcept
            : owner_runtime(std::exchange(other.owner_runtime, nullptr)), closed_doc(std::move(other.closed_doc)),
              closed_project_revision(other.closed_project_revision), bound_graph_id(other.bound_graph_id),
              bound_program(other.bound_program), code_version(other.code_version),
              evaluator(std::move(other.evaluator)), asset_instances(std::move(other.asset_instances)) {}

        GraphSlot& operator=(GraphSlot&& other) noexcept {
            if (this != &other) {
                reset();
                owner_runtime = std::exchange(other.owner_runtime, nullptr);
                closed_doc = std::move(other.closed_doc);
                closed_project_revision = other.closed_project_revision;
                bound_graph_id = other.bound_graph_id;
                bound_program = other.bound_program;
                code_version = other.code_version;
                evaluator = std::move(other.evaluator);
                asset_instances = std::move(other.asset_instances);
            }
            return *this;
        }

        ~GraphSlot() { reset(); }

        void reset() noexcept {
            evaluator.reset();
            bound_graph_id = {};
            bound_program = nullptr;
            asset_instances.clear();
            if (closed_doc && owner_runtime)
                destroy_private_graph_document(*owner_runtime, *closed_doc);
            closed_doc.reset();
        }
    };

    struct PrivateInstance {
        Runtime* owner_runtime{};

        String graph_key;

        std::uint64_t source_epoch{};

        std::uint64_t code_version{};

        json::Document applied_overrides;

        bool overrides_initialized{};

        GraphDocument cloned_doc;

        Array<CachedPromotedParam> cached_params;

        Map<ObjectId, std::unique_ptr<BoundAssetInstance>> asset_instances;

        const source::Program* bound_program{};

        std::unique_ptr<procedural::Evaluator> evaluator;

        ~PrivateInstance() {
            evaluator.reset();
            asset_instances.clear();
            if (owner_runtime)
                destroy_private_graph_document(*owner_runtime, cloned_doc);
        }
    };

    struct Entry {
        String graph_key;

        float chunk_size{};

        // Disengaged if skipped. Presentation can retain the previous output after a failed cook.
        Optional<Result<void>> latest_cook;

        std::uint64_t next_output_generation{1};

        std::shared_ptr<const presentation::ProceduralOutput> output;

        Array<presentation::ProceduralInstanceAsset> resolved_assets;

        std::unique_ptr<PrivateInstance> private_instance;
    };

    GraphResources resources;

    assets::Reader* fallback_reader{};

    Map<String, GraphSlot> graph_slots;

    Map<ObjectId, Entry> entries;

    Array<ObjectId> scratch_changed;

    bool idle_valid{};

    bool has_errors{};

    std::uint64_t idle_world_revision{};

    std::uint64_t idle_session_revision{};

    const Project* idle_project{};

    std::uint64_t idle_project_revision{};

    std::uint64_t idle_code_version{};

    std::uint64_t idle_runtime_cursor{};

    void clear() noexcept {
        for (auto [_, slot] : graph_slots)
            slot.reset();
        graph_slots.clear();
        for (auto [_, entry] : entries) {
            entry.private_instance.reset();
            entry.resolved_assets.clear();
            entry.output.reset();
        }
        entries.clear();
        idle_valid = false;
        has_errors = false;
    }
};

struct Session::AssetSchemaCache {
    struct Entry {
        Runtime* owner_runtime{};

        const Project* project{};

        std::uint64_t project_revision{};

        std::uint64_t code_version{};

        GraphDocument document;

        Entry() = default;

        Entry(Runtime* rt, const Project* proj, std::uint64_t prev, std::uint64_t cver, GraphDocument doc)
            : owner_runtime(rt), project(proj), project_revision(prev), code_version(cver), document(std::move(doc)) {}

        Entry(const Entry&) = delete;

        Entry& operator=(const Entry&) = delete;

        Entry(Entry&& other) noexcept
            : owner_runtime(std::exchange(other.owner_runtime, nullptr)), project(other.project),
              project_revision(other.project_revision), code_version(other.code_version),
              document(std::move(other.document)) {}

        Entry& operator=(Entry&& other) noexcept {
            if (this != &other) {
                if (owner_runtime)
                    destroy_private_graph_document(*owner_runtime, document);
                owner_runtime = std::exchange(other.owner_runtime, nullptr);
                project = other.project;
                project_revision = other.project_revision;
                code_version = other.code_version;
                document = std::move(other.document);
            }
            return *this;
        }

        ~Entry() {
            if (owner_runtime)
                destroy_private_graph_document(*owner_runtime, document);
        }
    };

    Map<String, Entry> schemas;

    Set<String> resolving;

    bool catalog_valid{};

    std::size_t catalog_type_count{};

    const Project* catalog_project{};

    std::uint64_t catalog_project_revision{};

    std::uint64_t catalog_open_epoch{};

    Array<GraphNodeCatalogEntry> full_catalog;
};

void Session::OpenGraphDeleter::operator()(OpenGraph* ptr) const noexcept {
    delete ptr;
}

void Session::GeneratorCacheDeleter::operator()(GeneratorRuntimeCache* ptr) const noexcept {
    delete ptr;
}

void Session::AssetSchemaCacheDeleter::operator()(AssetSchemaCache* ptr) const noexcept {
    delete ptr;
}

void Session::bind_project_code(const ProjectCode* code) noexcept {
    if (project_code_ == code)
        return;
    project_code_ = code;
    asset_schema_cache_.reset();
    for (auto [_, open] : graphs_) {
        if (open) {
            open->evaluator.reset();
            open->asset_instances.clear();
            open->bound_program = nullptr;
        }
    }
    if (generator_cache_) {
        for (auto [_, slot] : generator_cache_->graph_slots) {
            slot.evaluator.reset();
            slot.asset_instances.clear();
            slot.bound_program = nullptr;
        }
        for (auto [_, entry] : generator_cache_->entries) {
            if (entry.private_instance) {
                entry.private_instance->evaluator.reset();
                entry.private_instance->asset_instances.clear();
                entry.private_instance.reset();
            }
        }
        generator_cache_->idle_valid = false;
    }
}

namespace {

double percentile_sorted(Span<const double> sorted, double q) noexcept {
    if (sorted.empty())
        return 0.0;
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(sorted.size() - 1, lo + 1);
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

bool finite_v3(math::Vector3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

math::Vector3 transform_affine_point(math::Vector3 p, const math::Matrix4& m) noexcept {
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

Optional<GraphTopologyHealth> compute_topology_health(const procedural::Geometry& geometry) {
    const std::size_t prim_count = geometry.count(procedural::Layer::primitive);
    if (prim_count == 0)
        return {};
    GraphTopologyHealth health;
    const std::size_t vert_count = geometry.count(procedural::Layer::vertex);
    constexpr std::size_t max_diagnostic_vertices = 500'000;
    if (vert_count > max_diagnostic_vertices) {
        health.skipped = true;
        for (std::size_t p = 0; p < prim_count; ++p) {
            if (geometry.primitive(static_cast<std::uint32_t>(p)).kind == procedural::PrimitiveKind::polyline)
                ++health.polyline_count;
            else
                ++health.polygon_count;
        }
        return health;
    }

    struct DirectedEdge {
        std::uint32_t lo{};

        std::uint32_t hi{};

        std::int8_t dir{};
    };

    const auto& positions = geometry.positions();
    const auto& vpts = geometry.vertex_points();
    Array<DirectedEdge> edges;
    edges.reserve(vert_count);
    Array<std::uint32_t> ring;

    for (std::size_t p = 0; p < prim_count; ++p) {
        const auto prim = geometry.primitive(static_cast<std::uint32_t>(p));
        if (prim.kind == procedural::PrimitiveKind::polyline) {
            ++health.polyline_count;
            bool degenerate = prim.count < 2 || static_cast<std::size_t>(prim.first) + prim.count > vpts.size();
            if (!degenerate) {
                for (std::uint32_t j = 0; j < prim.count; ++j) {
                    const auto idx = vpts[prim.first + j];
                    if (idx >= positions.size() || !finite_v3(positions[idx])) {
                        degenerate = true;
                        break;
                    }
                    if (j > 0) {
                        const auto prev = vpts[prim.first + j - 1];
                        const math::Vector3d d = math::Vector3d(positions[idx]) - math::Vector3d(positions[prev]);
                        if (!(math::dot(d, d) > 1e-16)) {
                            degenerate = true;
                            break;
                        }
                    }
                }
            }
            if (degenerate)
                ++health.degenerate_primitives;
            continue;
        }

        ++health.polygon_count;
        if (prim.count < 3 || static_cast<std::size_t>(prim.first) + prim.count > vpts.size()) {
            ++health.degenerate_primitives;
            continue;
        }

        bool valid_pts = true;
        ring.assign(Span<const std::uint32_t>(vpts).slice(prim.first, prim.count));
        for (const auto idx : ring) {
            if (idx >= positions.size() || !finite_v3(positions[idx])) {
                valid_pts = false;
                break;
            }
        }
        std::ranges::sort(ring);
        const bool has_dup = std::ranges::adjacent_find(ring) != ring.end();
        bool zero_area = true;
        if (valid_pts) {
            math::Vector3d area_sum{};
            const math::Vector3d p0(positions[vpts[prim.first]]);
            for (std::uint32_t j = 1; j + 1 < prim.count; ++j) {
                const math::Vector3d a = math::Vector3d(positions[vpts[prim.first + j]]) - p0;
                const math::Vector3d b = math::Vector3d(positions[vpts[prim.first + j + 1]]) - p0;
                area_sum = area_sum + math::cross(a, b);
            }
            zero_area = !(math::length(area_sum) > 1e-12);
        }
        if (!valid_pts || has_dup || zero_area)
            ++health.degenerate_primitives;

        for (std::uint32_t j = 0; j < prim.count; ++j) {
            const std::uint32_t u = vpts[prim.first + j];
            const std::uint32_t v = vpts[prim.first + ((j + 1 == prim.count) ? 0u : j + 1u)];
            if (u == v)
                continue;
            if (u < v)
                edges.add({u, v, std::int8_t{1}});
            else
                edges.add({v, u, std::int8_t{-1}});
        }
    }

    std::ranges::sort(
        edges, [](const DirectedEdge& a, const DirectedEdge& b) { return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi; });
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t end = i + 1;
        while (end < edges.size() && edges[end].lo == edges[i].lo && edges[end].hi == edges[i].hi)
            ++end;
        const std::size_t count = end - i;
        if (count == 1) {
            ++health.open_boundary_edges;
        } else if (count > 2 || (edges[i].dir + edges[i + 1].dir != 0)) {
            ++health.non_manifold_edges;
        }
        i = end;
    }
    return health;
}

Optional<GraphSpacingStats> compute_spacing_stats(const procedural::Geometry& geometry) {
    Array<math::Vector3> pts;
    String domain;
    if (geometry.count(procedural::Layer::instance) >= 2) {
        domain = "instance"_s;
        const auto& transforms = geometry.instance_transforms();
        pts.reserve(transforms.size());
        for (const auto& m : transforms) {
            const math::Vector3 p{m[12], m[13], m[14]};
            if (finite_v3(p))
                pts.add(p);
        }
    } else if (geometry.count(procedural::Layer::primitive) == 0 && geometry.count(procedural::Layer::point) >= 2) {
        domain = "point"_s;
        const auto& positions = geometry.positions();
        pts.reserve(positions.size());
        for (const auto& p : positions) {
            if (finite_v3(p))
                pts.add(p);
        }
    } else {
        return {};
    }
    if (pts.size() < 2)
        return {};

    const std::size_t n = pts.size();
    constexpr std::size_t max_queries = 4096;
    const std::size_t q_count = std::min(n, max_queries);
    Array<double> nearest;
    nearest.resize(q_count, std::numeric_limits<double>::infinity());

    math::Vector3 low = pts.front();
    math::Vector3 high = pts.front();
    for (const auto& p : pts) {
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        low.z = std::min(low.z, p.z);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
        high.z = std::max(high.z, p.z);
    }
    const math::Vector3d span = math::Vector3d(high) - math::Vector3d(low);
    const double max_span = std::max({span.x, span.y, span.z});

    if (n <= 512 || !(max_span > 1e-6)) {
        for (std::size_t qi = 0; qi < q_count; ++qi) {
            const std::size_t i = q_count == n ? qi : (qi * n) / q_count;
            const math::Vector3d pi(pts[i]);
            double best_d2 = std::numeric_limits<double>::infinity();
            if (!(max_span > 1e-6)) {
                best_d2 = 0.0;
            } else {
                for (std::size_t j = 0; j < n; ++j) {
                    if (j == i)
                        continue;
                    const math::Vector3d d = math::Vector3d(pts[j]) - pi;
                    best_d2 = std::min(best_d2, math::dot(d, d));
                }
            }
            nearest[qi] = std::sqrt(best_d2);
        }
    } else {
        const int base_dim =
            std::clamp(static_cast<int>(std::cbrt(static_cast<double>(std::min<std::size_t>(n, 16384)))), 4, 24);
        const int gx = span.x > 1e-6 ? std::max(1, static_cast<int>(std::round(base_dim * (span.x / max_span)))) : 1;
        const int gy = span.y > 1e-6 ? std::max(1, static_cast<int>(std::round(base_dim * (span.y / max_span)))) : 1;
        const int gz = span.z > 1e-6 ? std::max(1, static_cast<int>(std::round(base_dim * (span.z / max_span)))) : 1;
        const double inv_x = span.x > 1e-6 ? static_cast<double>(gx) / span.x : 0.0;
        const double inv_y = span.y > 1e-6 ? static_cast<double>(gy) / span.y : 0.0;
        const double inv_z = span.z > 1e-6 ? static_cast<double>(gz) / span.z : 0.0;
        const double step_x = span.x > 1e-6 ? span.x / static_cast<double>(gx) : 0.0;
        const double step_y = span.y > 1e-6 ? span.y / static_cast<double>(gy) : 0.0;
        const double step_z = span.z > 1e-6 ? span.z / static_cast<double>(gz) : 0.0;

        const auto cell_of = [&](math::Vector3 p) -> std::array<int, 3> {
            const int cx =
                inv_x > 0.0 ? std::clamp(static_cast<int>((static_cast<double>(p.x) - low.x) * inv_x), 0, gx - 1) : 0;
            const int cy =
                inv_y > 0.0 ? std::clamp(static_cast<int>((static_cast<double>(p.y) - low.y) * inv_y), 0, gy - 1) : 0;
            const int cz =
                inv_z > 0.0 ? std::clamp(static_cast<int>((static_cast<double>(p.z) - low.z) * inv_z), 0, gz - 1) : 0;
            return {cx, cy, cz};
        };
        const auto flat_idx = [&](int cx, int cy, int cz) noexcept {
            return static_cast<std::size_t>((cx * gy + cy) * gz + cz);
        };

        Array<std::int32_t> head;
        head.resize(static_cast<std::size_t>(gx * gy * gz), -1);
        Array<std::int32_t> next;
        next.resize(n, -1);
        for (std::size_t idx = 0; idx < n; ++idx) {
            const auto [cx, cy, cz] = cell_of(pts[idx]);
            const auto cell = flat_idx(cx, cy, cz);
            next[idx] = head[cell];
            head[cell] = static_cast<std::int32_t>(idx);
        }

        const auto axis_dist_sq = [](double coord, double origin, double step, int c, int dim) noexcept {
            if (dim <= 1 || step <= 0.0)
                return 0.0;
            const double lo_b = origin + static_cast<double>(c) * step;
            const double hi_b = c + 1 == dim ? origin + static_cast<double>(dim) * step : lo_b + step;
            if (coord < lo_b) {
                const double d = lo_b - coord;
                return d * d;
            }
            if (coord > hi_b) {
                const double d = coord - hi_b;
                return d * d;
            }
            return 0.0;
        };

        const int max_radius = std::max({gx, gy, gz});
        for (std::size_t qi = 0; qi < q_count; ++qi) {
            const std::size_t i = q_count == n ? qi : (qi * n) / q_count;
            const math::Vector3d pi(pts[i]);
            const auto [qx, qy, qz] = cell_of(pts[i]);
            double best_d2 = std::numeric_limits<double>::infinity();

            for (int r = 0; r < max_radius; ++r) {
                if (r > 1 && best_d2 < std::numeric_limits<double>::infinity()) {
                    double shell_lb_sq = std::numeric_limits<double>::infinity();
                    if (qx - r >= 0)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.x, low.x, step_x, qx - r, gx));
                    if (qx + r < gx)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.x, low.x, step_x, qx + r, gx));
                    if (qy - r >= 0)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.y, low.y, step_y, qy - r, gy));
                    if (qy + r < gy)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.y, low.y, step_y, qy + r, gy));
                    if (qz - r >= 0)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.z, low.z, step_z, qz - r, gz));
                    if (qz + r < gz)
                        shell_lb_sq = std::min(shell_lb_sq, axis_dist_sq(pi.z, low.z, step_z, qz + r, gz));
                    if (shell_lb_sq >= best_d2)
                        break;
                }
                const int x0 = std::max(0, qx - r);
                const int x1 = std::min(gx - 1, qx + r);
                const int y0 = std::max(0, qy - r);
                const int y1 = std::min(gy - 1, qy + r);
                const int z0 = std::max(0, qz - r);
                const int z1 = std::min(gz - 1, qz + r);
                for (int cx = x0; cx <= x1; ++cx) {
                    const double dx2 = axis_dist_sq(pi.x, low.x, step_x, cx, gx);
                    if (dx2 >= best_d2)
                        continue;
                    for (int cy = y0; cy <= y1; ++cy) {
                        const double dxy2 = dx2 + axis_dist_sq(pi.y, low.y, step_y, cy, gy);
                        if (dxy2 >= best_d2)
                            continue;
                        for (int cz = z0; cz <= z1; ++cz) {
                            if (r > 0 && std::max({std::abs(cx - qx), std::abs(cy - qy), std::abs(cz - qz)}) != r)
                                continue;
                            if (dxy2 + axis_dist_sq(pi.z, low.z, step_z, cz, gz) >= best_d2)
                                continue;
                            for (std::int32_t cur = head[flat_idx(cx, cy, cz)]; cur != -1;
                                 cur = next[static_cast<std::size_t>(cur)]) {
                                const auto j = static_cast<std::size_t>(cur);
                                if (j == i)
                                    continue;
                                const math::Vector3d d = math::Vector3d(pts[j]) - pi;
                                best_d2 = std::min(best_d2, math::dot(d, d));
                            }
                        }
                    }
                }
            }
            nearest[qi] = std::sqrt(best_d2);
        }
    }

    GraphSpacingStats stats;
    stats.domain = std::move(domain);
    stats.element_count = n;
    stats.sample_count = q_count;
    for (const double d : nearest) {
        if (d <= 1e-4)
            ++stats.coincident_count;
    }
    std::ranges::sort(nearest);
    stats.minimum = nearest.front();
    stats.p5 = percentile_sorted(nearest, 0.05);
    stats.p50 = percentile_sorted(nearest, 0.50);
    stats.p95 = percentile_sorted(nearest, 0.95);
    return stats;
}

Optional<GraphInstanceOverlapStats> compute_instance_overlaps(Span<const math::Bounds> boxes) {
    if (boxes.size() < 2)
        return {};
    constexpr std::size_t max_tested = 65'536;
    const std::size_t m = std::min(boxes.size(), max_tested);

    math::Vector3 c_low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
    math::Vector3 c_high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()};
    for (std::size_t i = 0; i < m; ++i) {
        const math::Vector3 c = (boxes[i].minimum + boxes[i].maximum) * 0.5f;
        c_low.x = std::min(c_low.x, c.x);
        c_low.y = std::min(c_low.y, c.y);
        c_low.z = std::min(c_low.z, c.z);
        c_high.x = std::max(c_high.x, c.x);
        c_high.y = std::max(c_high.y, c.y);
        c_high.z = std::max(c_high.z, c.z);
    }
    const math::Vector3 span = c_high - c_low;
    const std::size_t axis = (span.x >= span.y && span.x >= span.z) ? 0u : (span.y >= span.z ? 1u : 2u);

    Array<std::uint32_t> order;
    order.resize_uninitialized(m);
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::sort(
        order, [&](std::uint32_t a, std::uint32_t b) { return boxes[a].minimum[axis] < boxes[b].minimum[axis]; });

    constexpr float overlap_eps = 1e-5f;
    constexpr std::size_t max_pairs = 100'000;
    GraphInstanceOverlapStats stats;
    stats.tested_instances = m;
    Array<std::uint8_t> overlapped;
    overlapped.resize(m, 0);

    for (std::size_t i = 0; i < m && !stats.pairs_capped; ++i) {
        const auto& bi = boxes[order[i]];
        const float limit = bi.maximum[axis] - overlap_eps;
        for (std::size_t j = i + 1; j < m; ++j) {
            const auto& bj = boxes[order[j]];
            if (bj.minimum[axis] >= limit)
                break;
            const float dx = std::min(bi.maximum.x, bj.maximum.x) - std::max(bi.minimum.x, bj.minimum.x);
            if (dx <= overlap_eps)
                continue;
            const float dy = std::min(bi.maximum.y, bj.maximum.y) - std::max(bi.minimum.y, bj.minimum.y);
            if (dy <= overlap_eps)
                continue;
            const float dz = std::min(bi.maximum.z, bj.maximum.z) - std::max(bi.minimum.z, bj.minimum.z);
            if (dz <= overlap_eps)
                continue;
            overlapped[order[i]] = 1;
            overlapped[order[j]] = 1;
            ++stats.overlapping_pairs;
            stats.max_penetration_depth =
                std::max(stats.max_penetration_depth, static_cast<double>(std::min({dx, dy, dz})));
            if (stats.overlapping_pairs >= max_pairs) {
                stats.pairs_capped = true;
                break;
            }
        }
    }

    for (const auto flag : overlapped)
        stats.overlapping_instances += flag;
    stats.overlapping_ratio = static_cast<double>(stats.overlapping_instances) / static_cast<double>(m);
    return stats;
}

Optional<GraphSurfaceClearanceStats> compute_surface_clearance(const procedural::Geometry& geometry,
                                                               const procedural::Bvh& ref_bvh,
                                                               StringView reference_name) {
    Array<math::Vector3> anchors;
    String domain;
    const auto refs = geometry.references();
    const auto& inst_indices = geometry.instance_references();
    const auto& inst_transforms = geometry.instance_transforms();
    if (!inst_indices.empty()) {
        domain = "instance"_s;
        anchors.reserve(inst_indices.size());
        for (std::size_t i = 0; i < inst_indices.size(); ++i) {
            const auto ref_idx = inst_indices[i];
            const auto& world = inst_transforms[i];
            math::Vector3 anchor{world[12], world[13], world[14]};
            if (ref_idx < refs.size() && refs[ref_idx].mesh && !refs[ref_idx].mesh->vertices().empty()) {
                const auto& mb = refs[ref_idx].mesh->bounds();
                const math::Vector3 local_base{(mb.minimum.x + mb.maximum.x) * 0.5f, mb.minimum.y,
                                               (mb.minimum.z + mb.maximum.z) * 0.5f};
                anchor = transform_affine_point(local_base, world);
            }
            if (finite_v3(anchor))
                anchors.add(anchor);
        }
    } else if (geometry.count(procedural::Layer::point) > 0) {
        domain = "point"_s;
        const auto& positions = geometry.positions();
        anchors.reserve(positions.size());
        for (const auto& p : positions) {
            if (finite_v3(p))
                anchors.add(p);
        }
    } else {
        return {};
    }
    if (anchors.empty())
        return {};

    constexpr std::size_t max_tested = 8192;
    const std::size_t total = anchors.size();
    const std::size_t q_count = std::min(total, max_tested);
    constexpr float max_ray_dist = 1e6f;
    constexpr double ground_tol = 0.05;

    GraphSurfaceClearanceStats stats;
    stats.reference_node = lossy_utf8(reference_name);
    stats.domain = std::move(domain);
    stats.tested_count = q_count;
    Array<double> gaps;
    gaps.reserve(q_count);

    for (std::size_t qi = 0; qi < q_count; ++qi) {
        const math::Vector3 q = anchors[q_count == total ? qi : (qi * total) / q_count];
        const auto down = ref_bvh.ray(q, {0.f, -1.f, 0.f}, max_ray_dist);
        const auto up = ref_bvh.ray(q, {0.f, 1.f, 0.f}, max_ray_dist);
        double gap{};
        if (down && (!up || down->distance <= up->distance)) {
            gap = static_cast<double>(down->distance);
        } else if (up) {
            gap = -static_cast<double>(up->distance);
        } else if (const auto near = ref_bvh.closest(q, 1e-3f);
                   near && std::hypot(q.x - near->position.x, q.z - near->position.z) <= 1e-4f) {
            gap = static_cast<double>(q.y) - static_cast<double>(near->position.y);
        } else {
            ++stats.off_surface_count;
            continue;
        }

        if (std::abs(gap) <= ground_tol)
            ++stats.grounded_count;
        else if (gap > ground_tol)
            ++stats.floating_count;
        else
            ++stats.buried_count;
        gaps.add(gap);
    }

    if (!gaps.empty()) {
        std::ranges::sort(gaps);
        stats.has_gaps = true;
        stats.gap_min = gaps.front();
        stats.gap_p5 = percentile_sorted(gaps, 0.05);
        stats.gap_p50 = percentile_sorted(gaps, 0.50);
        stats.gap_p95 = percentile_sorted(gaps, 0.95);
        stats.gap_max = gaps.back();
    }
    return stats;
}

GraphGeometrySummary summarize_geometry_impl(const procedural::Geometry* geometry, std::size_t max_samples,
                                             bool include_spatial, const procedural::Bvh* reference_bvh,
                                             StringView reference_name) {
    GraphGeometrySummary summary;
    if (!geometry)
        return summary;
    summary.valid = true;
    constexpr std::array layers{procedural::Layer::point, procedural::Layer::vertex, procedural::Layer::primitive,
                                procedural::Layer::detail, procedural::Layer::instance};
    for (std::size_t i = 0; i < layers.size(); ++i)
        summary.counts[i] = geometry->count(layers[i]);

    math::Vector3 low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
    math::Vector3 high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()};
    bool any_bounds = false;
    const auto include_point = [&](math::Vector3 p) {
        if (!finite_v3(p))
            return;
        low.x = std::min(low.x, p.x);
        low.y = std::min(low.y, p.y);
        low.z = std::min(low.z, p.z);
        high.x = std::max(high.x, p.x);
        high.y = std::max(high.y, p.y);
        high.z = std::max(high.z, p.z);
        any_bounds = true;
    };
    for (const auto& p : geometry->positions())
        include_point(p);

    const auto refs = geometry->references();
    const auto& inst_indices = geometry->instance_references();
    const auto& inst_transforms = geometry->instance_transforms();
    Array<std::size_t> ref_counts;
    ref_counts.resize(refs.size(), 0);
    Array<math::Bounds> instance_boxes;
    if (include_spatial && inst_indices.size() >= 2)
        instance_boxes.reserve(std::min<std::size_t>(inst_indices.size(), 65'536));
    for (std::size_t i = 0; i < inst_indices.size(); ++i) {
        const auto ref_idx = inst_indices[i];
        if (ref_idx >= refs.size())
            continue;
        ++ref_counts[ref_idx];
        const auto& mesh = refs[ref_idx].mesh;
        const auto& world = inst_transforms[i];
        if (mesh && !mesh->vertices().empty()) {
            const auto bounds = math::transform_bounds(mesh->bounds(), world);
            include_point(bounds.minimum);
            include_point(bounds.maximum);
            if (include_spatial && instance_boxes.size() < 65'536 && finite_v3(bounds.minimum) &&
                finite_v3(bounds.maximum))
                instance_boxes.add(bounds);
        } else {
            include_point({world[12], world[13], world[14]});
        }
    }
    if (any_bounds) {
        summary.has_bounds = true;
        summary.bounds_min = low;
        summary.bounds_max = high;
    }
    for (std::size_t i = 0; i < refs.size(); ++i)
        summary.instance_references.emplace(refs[i].key, ref_counts[i]);

    for (const auto& attr : geometry->attributes()) {
        GraphAttributeStats stats;
        stats.name = attr.name;
        std::visit(
            [&](const auto& col) {
                using Shared = std::decay_t<decltype(col)>;
                using T = typename Shared::Value;
                const Span<const T> values = col.array();
                if constexpr (std::same_as<T, bool>) {
                    stats.type = "bool"_s;
                    summarize_numeric_lanes(
                        values, 1, [](bool v, std::uint32_t) { return v ? 1.0 : 0.0; }, stats, max_samples);
                } else if constexpr (std::same_as<T, std::int32_t>) {
                    stats.type = "int"_s;
                    summarize_numeric_lanes(
                        values, 1, [](std::int32_t v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, std::uint32_t>) {
                    stats.type = "uint"_s;
                    summarize_numeric_lanes(
                        values, 1, [](std::uint32_t v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, float>) {
                    stats.type = "float"_s;
                    summarize_numeric_lanes(values, 1, [](float v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector2>) {
                    stats.type = "vector2"_s;
                    summarize_numeric_lanes(
                        values, 2, [](const math::Vector2& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector3>) {
                    stats.type = "vector3"_s;
                    summarize_numeric_lanes(
                        values, 3, [](const math::Vector3& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector4>) {
                    stats.type = "vector4"_s;
                    summarize_numeric_lanes(
                        values, 4, [](const math::Vector4& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Quaternion>) {
                    stats.type = "quaternion"_s;
                    summarize_numeric_lanes(
                        values, 4,
                        [](const math::Quaternion& q, std::uint32_t c) {
                            return c == 0 ? q.x : c == 1 ? q.y : c == 2 ? q.z : q.w;
                        },
                        stats, max_samples);
                } else if constexpr (std::same_as<T, math::Matrix4>) {
                    stats.type = "matrix4"_s;
                    summarize_numeric_lanes(
                        values, 16, [](const math::Matrix4& m, std::uint32_t c) { return m[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, String>) {
                    stats.type = "string"_s;
                    stats.components = 0;
                    stats.count = values.size();
                    for (std::size_t i = 0; i < std::min(values.size(), max_samples); ++i)
                        stats.sample_values.add(values[i]);
                }
            },
            attr.data);
        const auto layer_idx = static_cast<std::size_t>(attr.layer);
        if (layer_idx < summary.attributes.size())
            summary.attributes[layer_idx].add(std::move(stats));
    }

    if (include_spatial) {
        GraphSpatialDiagnostics diag;
        diag.topology = compute_topology_health(*geometry);
        diag.spacing = compute_spacing_stats(*geometry);
        diag.instance_overlaps = compute_instance_overlaps(instance_boxes);
        if (reference_bvh)
            diag.surface_clearance = compute_surface_clearance(*geometry, *reference_bvh, reference_name);
        summary.spatial = std::move(diag);
    }
    return summary;
}

} // namespace

GraphGeometrySummary summarize_geometry(const procedural::Geometry* geometry, std::size_t max_samples,
                                        bool include_spatial, const procedural::Geometry* reference_surface,
                                        StringView reference_name) {
    std::shared_ptr<const procedural::Bvh> ref_bvh;
    if (include_spatial && reference_surface) {
        if (auto acquired = procedural::Bvh::acquire(*reference_surface))
            ref_bvh = std::move(*acquired);
    }
    return summarize_geometry_impl(geometry, max_samples, include_spatial, ref_bvh.get(), reference_name);
}

void Session::close_graphs() noexcept {
    graph_param_edit_.reset();
    asset_schema_cache_.reset();
    if (generator_cache_) {
        generator_cache_->clear();
        generator_cache_.reset();
    }
    for (auto [_, open] : graphs_) {
        if (open) {
            open->evaluator.reset();
            open->asset_instances.clear();
        }
    }
    graphs_.clear();
    procedural_cache_pool_.reset();
}

std::shared_ptr<procedural::CachePool> Session::ensure_procedural_cache_pool() {
    if (!procedural_cache_pool_)
        procedural_cache_pool_ = std::make_shared<procedural::CachePool>(std::size_t{256} << 20);
    return procedural_cache_pool_;
}

Result<const GraphDocument*> Session::resolve_target_graph_document(StringView asset_key,
                                                                    Project* project) const {
    if (asset_key.empty())
        return static_cast<const GraphDocument*>(nullptr);
    if (const auto* open_it = graphs_.find(asset_key); open_it && *open_it)
        return &(*open_it)->document;

    if (project) {
        const std::uint64_t proj_rev = project->revision();
        const std::uint64_t code_version =
            project_code_ && project_code_->report() ? project_code_->report()->published_version : 0ULL;
        if (!asset_schema_cache_)
            asset_schema_cache_.reset(new AssetSchemaCache());
        auto* cached = asset_schema_cache_->schemas.find(asset_key);
        if (!cached || cached->project != project || cached->project_revision != proj_rev ||
            cached->code_version != code_version) {
            auto key_str = String::from_utf8(asset_key);
            if (!key_str)
                return graph_error("Invalid UTF-8 procedural graph key.", ErrorCode::invalid_storage);
            if (!asset_schema_cache_->resolving.add(*key_str))
                return static_cast<const GraphDocument*>(nullptr);
            ScopeExit guard{[&]() noexcept { asset_schema_cache_->resolving.remove(asset_key); }};

            auto asset = project->assets().procedural_graph(asset_key);
            if (!asset)
                return std::unexpected(asset.error());
            // Boundary GraphInput/GraphOutput ports and promoted parameter target schemas depend only on
            // the graph's own local nodes, never on resolving nested AssetNode connections.
            auto parsed = load_graph(world_.runtime(), asset->text, {});
            if (!parsed)
                return std::unexpected(parsed.error());
            AssetSchemaCache::Entry entry{&world_.runtime(), project, proj_rev, code_version, std::move(*parsed)};
            cached = asset_schema_cache_->schemas.insert_or_assign(std::move(*key_str), std::move(entry)).value;
        }
        return &cached->document;
    }

    if (asset_schema_cache_) {
        if (const auto* cached = asset_schema_cache_->schemas.find(asset_key))
            return &cached->document;
    }
    if (generator_cache_) {
        if (const auto* slot = generator_cache_->graph_slots.find(asset_key); slot && slot->closed_doc)
            return &*slot->closed_doc;
    }
    return static_cast<const GraphDocument*>(nullptr);
}

AssetPortResolver Session::make_asset_port_resolver(Project* project) const {
    return [this, project](StringView asset_key) -> Result<procedural::Ports> {
        ATELIER_TRY_BIND(doc, resolve_target_graph_document(asset_key, project));
        if (!*doc)
            return graph_error(String::concat("No graph available to resolve '", lossy_utf8(asset_key), "'."),
                               ErrorCode::invalid_storage);
        return extract_graph_ports(world_.runtime(), **doc);
    };
}

Result<const source::Program*> Session::procedural_source_program() {
    if (world_.closed())
        return graph_error("Procedural library inspection requires an open Session.", ErrorCode::invalid_definition);
    if (project_code_) {
        if (project_code_->program() && project_code_->program()->revision() > 0)
            return project_code_->program();
        if (project_code_->report() && project_code_->report()->error)
            return std::unexpected(*project_code_->report()->error);
        return graph_error("Bound ProjectCode has not published a Source program.", ErrorCode::invalid_definition);
    }
    if (engine_program_)
        return engine_program_.get();
    while (runtime_.collection_phase() != CollectionPhase::idle) {
        ATELIER_TRY(runtime_.collect_step({1024}));
    }
    ATELIER_TRY_BIND(engine_files, source::engine_sources(source::default_engine_root()));
    source::SourceSnapshot snapshot;
    snapshot.name = "engine"_s;
    snapshot.version = 14695981039346656037ULL;
    for (auto& file : *engine_files) {
        snapshot.version = source::content_hash(file.path, source::content_hash(file.source, snapshot.version));
        snapshot.files.add(std::move(file));
    }
    ATELIER_TRY_BIND(program, source::Program::create(runtime_));
    ATELIER_TRY_BIND(request, (*program)->request(snapshot));
    ATELIER_TRY_BIND(candidate, source::Program::prepare(*request));
    ATELIER_TRY((*program)->try_publish(*candidate));
    engine_program_ = std::shared_ptr<source::Program>(std::move(*program));
    return engine_program_.get();
}

Result<void> Session::remember_graph_scalar_edit(ObjectRoot target, Array<world::FieldPath> paths,
                                                 Array<Scalar> before, Array<Scalar> after) {
    for (auto [_, open] : graphs_) {
        if (!open || !open->owns(target.id()))
            continue;
        auto* node = find_node_by_id(open->document, target.id());
        auto& kept = node ? node->kept : open->document.kept;
        auto old_kept = kept;
        bool repaired = false;
        if (kept.root().size()) {
            ATELIER_TRY_BIND(type, runtime_.type_of(target.id()));
            ATELIER_TRY_BIND(fields, detail::author_fields(runtime_, *type));
            json::Builder next(kept);
            for (const auto& path : paths) {
                const auto field = std::ranges::find(*fields, path, &detail::ScalarField::path);
                if (field == fields->end())
                    continue;
                auto names = field->names;
                if (node)
                    names.insert(0, "properties"_s);
                repaired |= detail::erase_path(next, next.root(), names);
            }
            if (repaired)
                kept = next.finish();
        }
        if (before == after && !repaired)
            return {};
        open->push_change(GraphScalarChange{std::move(target), std::move(paths), std::move(before), std::move(after),
                                            std::move(old_kept), kept});
        ++revision_;
        return {};
    }
    return graph_error("Graph edit target is no longer in an open document.");
}

void Session::bump_graph_object_epoch(ObjectId object) noexcept {
    if (object == ObjectId{})
        return;
    for (auto [_, open] : graphs_) {
        if (open && open->owns(object)) {
            ++open->mutation_epoch;
            open->dirty_revision = 0;
            return;
        }
    }
}

namespace {
Result<Optional<GraphNodeCatalogEntry>> describe_sop(Runtime& runtime, TypeId type, TypeId base,
                                                     TypeId asset_node) {
    if (type == base || type == asset_node)
        return Optional<GraphNodeCatalogEntry>{};
    ATELIER_TRY_BIND(info, runtime.describe(type));
    if ((*info)->kind != TypeKind::object)
        return Optional<GraphNodeCatalogEntry>{};
    ATELIER_TRY_BIND(derived, runtime.is_a(type, base));
    if (!*derived)
        return Optional<GraphNodeCatalogEntry>{};
    auto schema = procedural::node_interface(runtime, type);
    if (!schema)
        return Optional<GraphNodeCatalogEntry>{};
    String help;
    for (const auto& port : schema->port_descriptions) {
        if (!help.empty())
            help.append_ascii('\n');
        help += String::concat(port.output ? "output"_s : "input"_s, " ", port.port, " · ", port.description);
    }
    for (const auto& rule : schema->attributes) {
        if (!help.empty())
            help.append_ascii('\n');
        const String layer_text =
            rule.layer.empty()
                ? (rule.layer_selector.empty() ? "<column>"_s : String::concat("$", rule.layer_selector))
                : rule.layer;
        const String attr_text = rule.selector.empty() ? rule.name : String::concat("$", rule.selector);
        help += String::concat(String::validated(procedural::access_name(rule.access)), " ", layer_text, ".", attr_text,
                               " · ", rule.type, " · ", String::validated(rule.missing));
        if (!rule.when.field.empty())
            help += String::concat(" · when ", String::validated(rule.when.field), rule.when.exclude ? "!="_s : "="_s,
                                   String::validated(rule.when.value));
        if (!rule.unless_present.empty())
            help += String::concat(" · unless ", String::validated(rule.unless_present));
        if (!rule.needs_present.empty())
            help += String::concat(" · requires ", String::validated(rule.needs_present));
        if (!rule.purpose.empty())
            help += String::concat(" · ", rule.purpose);
    }
    auto& ports = schema->ports;
    return Optional<GraphNodeCatalogEntry>{GraphNodeCatalogEntry{
        type,
        (*info)->name,
        (*info)->description,
        (*info)->native_storage,
        {},
        {},
        {std::move(ports.inputs), std::move(ports.multiple), std::move(ports.outputs)},
        help.empty() ? nullptr
                     : std::make_shared<GraphNodeHelp>(GraphNodeHelp{std::move(schema->port_descriptions),
                                                                     std::move(schema->attributes), std::move(help)})}};
}

GraphNodeCatalogEntry describe_graph_asset(const assets::ProjectEntry& entry, TypeId asset_node,
                                           const AssetPortResolver& resolve) {
    GraphPortInfo ports;
    if (auto resolved = resolve(entry.key))
        ports = {std::move(resolved->inputs), std::move(resolved->multiple), std::move(resolved->outputs)};
    String label = platform::utf16_to_utf8(entry.path.stem().wstring()).value_or(String{});
    if (label.empty())
        label = entry.key;
    const String generic_path = platform::utf16_to_utf8(entry.path.generic_wstring()).value_or(entry.key);
    return {asset_node,
            String::concat("asset:", entry.key),
            String::concat("Digital Asset · ", generic_path),
            false,
            entry.key,
            std::move(label),
            std::move(ports)};
}
} // namespace

Result<Optional<GraphNodeCatalogEntry>> Session::procedural_node_type(StringView name, Project* project) {
    if (world_.closed())
        return graph_error("Procedural node catalog requires an open Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(procedural_source_program());
    ATELIER_TRY_BIND(asset_node, runtime_.native_type<procedural::AssetNode>());
    if (name.starts_with("asset:")) {
        const auto* entry = project ? project->entry(name.slice(6)) : nullptr;
        if (!entry || entry->kind != assets::AssetKind::procedural_graph)
            return Optional<GraphNodeCatalogEntry>{};
        return Optional<GraphNodeCatalogEntry>{
            describe_graph_asset(*entry, *asset_node, make_asset_port_resolver(project))};
    }
    const auto type = runtime_.find_type(name);
    if (!type)
        return Optional<GraphNodeCatalogEntry>{};
    ATELIER_TRY_BIND(base, runtime_.native_type<procedural::Node>());
    return describe_sop(runtime_, *type, *base, *asset_node);
}

Result<Array<GraphNodeCatalogEntry>> Session::procedural_node_types(Project* project,
                                                                    StringView exclude_graph_key) {
    if (world_.closed())
        return graph_error("Procedural node catalog requires an open Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(procedural_source_program());
    if (!asset_schema_cache_)
        asset_schema_cache_.reset(new AssetSchemaCache());

    const std::size_t type_count = runtime_.type_count();
    const std::uint64_t proj_rev = project ? project->revision() : 0ULL;
    std::uint64_t open_epoch = 14695981039346656037ULL;
    for (const auto& [k, open] : graphs_) {
        if (open && !open->scratch) {
            open_epoch ^= source::content_hash(k, open->mutation_epoch);
            open_epoch *= 1099511628211ULL;
        }
    }

    auto& cache = *asset_schema_cache_;
    if (!cache.catalog_valid || cache.catalog_type_count != type_count || cache.catalog_project != project ||
        cache.catalog_project_revision != proj_rev || cache.catalog_open_epoch != open_epoch) {
        ATELIER_TRY_BIND(node_base, runtime_.native_type<procedural::Node>());
        ATELIER_TRY_BIND(asset_node_type, runtime_.native_type<procedural::AssetNode>());
        Array<GraphNodeCatalogEntry> catalog;
        for (const auto type : runtime_.types()) {
            ATELIER_TRY_BIND(entry, describe_sop(runtime_, type, *node_base, *asset_node_type));
            if (*entry)
                catalog.add(std::move(**entry));
        }
        std::ranges::sort(catalog, {}, &GraphNodeCatalogEntry::name);

        if (project) {
            const auto resolver = make_asset_port_resolver(project);
            Array<GraphNodeCatalogEntry> asset_entries;
            for (const auto& entry : project->entries()) {
                if (entry.kind != assets::AssetKind::procedural_graph)
                    continue;
                asset_entries.add(describe_graph_asset(entry, *asset_node_type, resolver));
            }
            std::ranges::sort(asset_entries, {}, &GraphNodeCatalogEntry::display_label);
            catalog.reserve(catalog.size() + asset_entries.size());
            for (auto& entry : asset_entries)
                catalog.add(std::move(entry));
        }
        cache.full_catalog = std::move(catalog);
        cache.catalog_type_count = type_count;
        cache.catalog_project = project;
        cache.catalog_project_revision = proj_rev;
        cache.catalog_open_epoch = open_epoch;
        cache.catalog_valid = true;
    }

    if (exclude_graph_key.empty())
        return cache.full_catalog;
    Array<GraphNodeCatalogEntry> filtered;
    filtered.reserve(cache.full_catalog.size());
    for (const auto& item : cache.full_catalog) {
        if (item.asset_key.empty() || item.asset_key != exclude_graph_key)
            filtered.add(item);
    }
    return filtered;
}

Result<ObjectId> Session::open_graph(Project& project, StringView key) {
    if (world_.closed())
        return graph_error("Opening a procedural graph requires a live Session.", ErrorCode::invalid_definition);
    if (const auto* existing = graphs_.find(key))
        return (*existing)->document.graph.id();
    ATELIER_TRY(procedural_source_program());
    ATELIER_TRY_BIND(asset, project.assets().procedural_graph(key));
    ATELIER_TRY_BIND(loaded, load_graph(runtime_, asset->text, make_asset_port_resolver(&project)));
    ATELIER_TRY_BIND(canonical, editor::save_graph(runtime_, *loaded));
    std::unique_ptr<OpenGraph, OpenGraphDeleter> open(new OpenGraph());
    open->key = asset->key;
    open->document = std::move(*loaded);
    open->saved_text = std::move(*canonical);
    open->resources.bind(&project);
    for (const auto& issue : open->document.issues)
        world_.report(issue);
    const auto id = open->document.graph.id();
    unavailable_.remove(std::pair{assets::AssetKind::procedural_graph, open->key});
    String graph_key = open->key;
    graphs_.try_emplace(std::move(graph_key), std::move(open));
    ++revision_;
    return id;
}

Result<ObjectId> Session::create_scratch_graph(StringView key) {
    if (world_.closed() || key.empty())
        return graph_error("Creating a scratch graph requires a live Session and non-empty key.",
                           ErrorCode::invalid_definition);
    if (const auto* existing = graphs_.find(key))
        return (*existing)->document.graph.id();
    auto key_str = String::from_utf8(key);
    if (!key_str)
        return graph_error("Scratch graph key must be valid UTF-8.", ErrorCode::invalid_definition);
    ATELIER_TRY(procedural_source_program());
    ATELIER_TRY_BIND(graph_type, runtime_.native_type<procedural::Graph>());
    ATELIER_TRY_BIND(graph_root, runtime_.create(*graph_type));
    std::unique_ptr<OpenGraph, OpenGraphDeleter> open(new OpenGraph());
    open->key = std::move(*key_str);
    open->scratch = true;
    open->document.graph = std::move(*graph_root);
    ATELIER_TRY_BIND(canonical, editor::save_graph(runtime_, open->document));
    open->saved_text = std::move(*canonical);
    const auto id = open->document.graph.id();
    String graph_key = open->key;
    graphs_.try_emplace(std::move(graph_key), std::move(open));
    ++revision_;
    return id;
}

bool Session::has_graph(StringView key) const noexcept {
    return graphs_.contains(key);
}

bool Session::is_scratch_graph(StringView key) const noexcept {
    if (key == "scratch" || key.starts_with("scratch/"))
        return true;
    const auto* at = graphs_.find(key);
    return at && *at && (*at)->scratch;
}

bool Session::has_graph_placeholders() const noexcept {
    for (const auto& [_, open] : graphs_) {
        if (!open)
            continue;
        if (open->document.kept.root().size() > 0)
            return true;
        for (const auto& item : open->document.nodes) {
            if (!item.node.registered() || item.kept.root().size() > 0)
                return true;
        }
    }
    return false;
}

Result<Array<OpenGraphSnapshot>> Session::snapshot_open_graphs() const {
    auto& runtime = world_.runtime();
    const auto keys = open_graphs();
    Array<OpenGraphSnapshot> result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        const auto* at = graphs_.find(key);
        if (!at || !*at)
            continue;
        ATELIER_TRY_BIND(encoded, editor::save_graph(runtime, (*at)->document));
        result.add({key, (*at)->scratch, std::move(*encoded), (*at)->saved_text});
    }
    return result;
}

Result<void> Session::restore_open_graphs(Project* project, Span<const OpenGraphSnapshot> snapshots) {
    if (world_.closed())
        return graph_error("Restoring procedural graphs requires an open Session.", ErrorCode::invalid_definition);
    if (snapshots.empty())
        return {};
    ATELIER_TRY(procedural_source_program());
    const auto fallback_resolver = make_asset_port_resolver(project);
    const AssetPortResolver snapshot_resolver = [&](StringView asset_key) -> Result<procedural::Ports> {
        const auto in_snap = std::ranges::find(snapshots, asset_key, &OpenGraphSnapshot::key);
        if (in_snap != snapshots.end()) {
            ATELIER_TRY_BIND(temp, load_graph(runtime_, in_snap->text, {}));
            auto ports = extract_graph_ports(runtime_, *temp);
            destroy_private_graph_document(runtime_, *temp);
            return ports;
        }
        return fallback_resolver(asset_key);
    };
    for (const auto& snap : snapshots) {
        ATELIER_TRY_BIND(loaded, load_graph(runtime_, snap.text, snapshot_resolver));
        std::unique_ptr<OpenGraph, OpenGraphDeleter> open(new OpenGraph());
        open->key = snap.key;
        open->scratch = snap.scratch;
        open->document = std::move(*loaded);
        open->saved_text = snap.saved_text;
        if (!snap.scratch && project) {
            open->resources.bind(project);
            unavailable_.remove(std::pair{assets::AssetKind::procedural_graph, open->key});
        }
        for (const auto& issue : open->document.issues)
            world_.report(issue);
        String graph_key = open->key;
        graphs_.insert_or_assign(std::move(graph_key), std::move(open));
    }
    if (generator_cache_)
        generator_cache_->idle_valid = false;
    ++revision_;
    return {};
}

Result<Optional<String>> Session::procedural_graph_snapshot(StringView key) const {
    const auto* at = graphs_.find(key);
    if (!at || !*at || (*at)->scratch)
        return Optional<String>{};
    const auto& open = **at;
    auto& runtime = world_.runtime();
    const auto& author = open.document;
    ATELIER_TRY_BIND(encoded, editor::save_graph(runtime, author));
    if (!generator_cache_)
        return Optional<String>{std::move(*encoded)};

    bool any_referencing = false;
    const source::Program* bound_program = nullptr;
    for (const auto& [_, entry] : generator_cache_->entries) {
        if (entry.graph_key == key && entry.latest_cook) {
            any_referencing = true;
            if (entry.private_instance && entry.private_instance->bound_program)
                bound_program = entry.private_instance->bound_program;
        }
    }
    if (!any_referencing)
        return Optional<String>{std::move(*encoded)};
    const auto* slot = generator_cache_->graph_slots.find(open.key);
    if (!bound_program && slot)
        bound_program = slot->bound_program;

    Array<ObjectId> freezes;
    for (const auto& item : author.nodes) {
        if (!item.node.registered())
            continue;
        auto frz = procedural::frozen(runtime, item.node.id());
        auto byp = procedural::bypassed(runtime, item.node.id());
        if (frz && *frz && byp && !*byp)
            freezes.add(item.node.id());
    }
    if (freezes.empty())
        return Optional<String>{std::move(*encoded)};

    // Load precisely what the bundle will reopen. An independent evaluator keeps the author and
    // its frozen outputs intact, including when a generator overrides the authored seed.
    ATELIER_TRY_BIND(copy, editor::load_graph(runtime, *encoded, make_asset_port_resolver(open.resources.project())));
    Map<ObjectId, std::unique_ptr<BoundAssetInstance>> copy_instances;
    ScopeExit copy_guard{[&]() noexcept {
        copy_instances.clear();
        destroy_private_graph_document(runtime, *copy);
    }};

    {
        OpenGraph temp_open;
        temp_open.key = open.key;
        temp_open.document = std::move(*copy);
        temp_open.asset_instances = std::move(copy_instances);
        if (open.resources.project())
            temp_open.resources.bind(open.resources.project());
        auto synced = const_cast<Session*>(this)->sync_open_graph_assets(temp_open, open.resources.project(), true);
        *copy = std::move(temp_open.document);
        copy_instances = std::move(temp_open.asset_instances);
        ATELIER_TRY(synced);
    }

    procedural::Evaluator replay(runtime, copy->graph, generator_cache_->resources, bound_program,
                                 procedural_cache_pool_);
    Set<Optional<std::uint32_t>> seeds;
    for (const auto& [component, entry] : generator_cache_->entries) {
        if (entry.graph_key != key || !entry.latest_cook)
            continue;
        if (!*entry.latest_cook)
            return std::unexpected(entry.latest_cook->error());
        auto seed = runtime.with_native<const presentation::ProceduralGeneratorComponent>(
            component, [](const auto& value) -> Result<Optional<std::uint32_t>> {
                return value.override_seed ? Optional<std::uint32_t>{static_cast<std::uint32_t>(value.seed)}
                                           : Optional<std::uint32_t>{};
            });
        if (!seed)
            return std::unexpected(seed.error());
        seeds.add(*seed);
    }
    for (const auto node : freezes) {
        const auto* original = find_node_by_id(author, node);
        if (!original)
            return graph_error("Frozen node is missing from its author graph.");
        const auto* restored = find_node(*copy, original->name);
        ATELIER_TRY_BIND(snapshot, procedural::frozen_output(runtime, node));
        for (const auto seed : seeds) {
            if (restored && restored->node.registered() && *snapshot) {
                auto cooked = replay.freeze_snapshot(restored->node.id(), seed);
                if (cooked &&
                    std::ranges::equal((*cooked)->outputs, (*snapshot)->outputs, &procedural::Geometry::same_value) &&
                    std::ranges::is_permutation((*cooked)->dependencies, (*snapshot)->dependencies))
                    continue;
            }
            return graph_error(String::concat("Graph '", open.key, "': frozen node '", original->name,
                                              "' differs from its authored replay. Unfreeze it before exporting."));
        }
    }
    return Optional<String>{std::move(*encoded)};
}

Result<ObjectId> Session::resolve_procedural_generator(ObjectId target, bool required) const {
    const auto missing = [required](Utf8Text message) -> Result<ObjectId> {
        if (required)
            return graph_error(message, ErrorCode::invalid_definition);
        return ObjectId{};
    };
    if (world_.closed())
        return graph_error("Procedural Generator resolution requires an open Session.", ErrorCode::invalid_definition);
    auto& runtime = world_.runtime();
    if (target == ObjectId{})
        target = selection_.object != ObjectId{} ? selection_.object : selection_.entity;
    if (target == ObjectId{})
        return missing("Select a Procedural Generator.");
    ATELIER_TRY_BIND(target_type, runtime.type_of(target));
    ATELIER_TRY_BIND(is_gen_comp, runtime.is_a(*target_type, presentation_types_.procedural_generator_component));
    if (*is_gen_comp) {
        ATELIER_TRY_BIND(state, world::inspect_component(runtime, target));
        if (!state->valid || state->owner == ObjectId{} || !world_.contains(state->owner))
            return graph_error("ProceduralGeneratorComponent is not attached to a World Entity.",
                               ErrorCode::invalid_member);
        return target;
    }
    ObjectId owner{};
    ATELIER_TRY_BIND(is_comp, runtime.is_a(*target_type, types_.component));
    if (*is_comp) {
        ATELIER_TRY_BIND(state, world::inspect_component(runtime, target));
        if (!state->valid || state->owner == ObjectId{} || !world_.contains(state->owner))
            return graph_error("Target component is not attached to a World Entity.", ErrorCode::invalid_member);
        owner = state->owner;
    } else {
        ATELIER_TRY_BIND(is_entity, runtime.is_a(*target_type, types_.entity));
        if (!*is_entity || !world_.contains(target))
            return missing("Target is not a Procedural Generator.");
        owner = target;
    }
    ATELIER_TRY_BIND(parts, world::components(runtime, owner));
    for (const auto& part : *parts) {
        ATELIER_TRY_BIND(part_type, runtime.type_of(part.id()));
        ATELIER_TRY_BIND(matches, runtime.is_a(*part_type, presentation_types_.procedural_generator_component));
        if (*matches)
            return part.id();
    }
    return missing("Selected Entity has no ProceduralGeneratorComponent.");
}

Result<void> Session::publish_procedural_mesh(assets::Reader& reader, const assets::MeshAsset&) {
    for (auto [_, open] : graphs_)
        if (open)
            open->resources.invalidate();
    if (!generator_cache_)
        generator_cache_.reset(new GeneratorRuntimeCache());
    generator_cache_->resources.invalidate();
    generator_cache_->idle_valid = false;
    generator_cache_->fallback_reader = &reader;
    generator_cache_->resources.bind_temporary_reader(&reader);
    ScopeExit reset{[&]() noexcept {
        generator_cache_->resources.bind_temporary_reader(nullptr);
        generator_cache_->fallback_reader = nullptr;
    }};
    return sync_procedural_generators(nullptr);
}

ObjectId Session::loaded_graph(StringView key) const noexcept {
    const auto* at = graphs_.find(key);
    return at && *at ? (*at)->document.graph.id() : ObjectId{};
}

bool Session::is_graph_object(ObjectId object) const noexcept {
    if (object == ObjectId{})
        return false;
    for (const auto& [_, open] : graphs_)
        if (open && open->owns(object))
            return true;
    return false;
}

Array<ObjectId> Session::graph_objects() const {
    Array<ObjectId> result;
    for (const auto& [_, open] : graphs_) {
        if (!open)
            continue;
        if (open->document.graph.registered())
            result.add(open->document.graph.id());
        for (const auto& node : open->document.nodes)
            if (node.node.registered())
                result.add(node.node.id());
    }
    return result;
}

Array<String> Session::open_graphs() const {
    Array<String> result;
    result.reserve(graphs_.size());
    for (const auto& [key, open] : graphs_)
        if (open)
            result.add(key);
    std::ranges::sort(result);
    return result;
}

Optional<String> Session::graph_of_object(ObjectId object) const {
    if (object == ObjectId{})
        return {};
    for (const auto& [key, open] : graphs_)
        if (open && open->owns(object))
            return key;
    return {};
}

Optional<String> Session::graph_node_name(ObjectId object) const {
    if (object == ObjectId{})
        return {};
    for (const auto& [_, open] : graphs_) {
        if (!open)
            continue;
        if (const auto* found = find_node_by_id(open->document, object))
            return found->name;
    }
    return {};
}

const GraphNode* Session::graph_node(StringView key, StringView name) const noexcept {
    const auto* at = graphs_.find(key);
    return at && *at ? find_node((*at)->document, name) : nullptr;
}

ObjectId Session::graph_node_id(StringView key, StringView name) const noexcept {
    const auto* found = graph_node(key, name);
    return found && found->node.registered() ? found->node.id() : ObjectId{};
}

const GraphDocument* Session::graph_document(StringView key) const noexcept {
    const auto* at = graphs_.find(key);
    return at && *at ? &(*at)->document : nullptr;
}

bool Session::graph_dirty(StringView key) const {
    const auto* at = graphs_.find(key);
    if (!at || !*at)
        return false;
    const auto& open = **at;
    if ((active_ && open.owns(active_->root.id())) || (group_ && open.owns(group_->root.id())) ||
        (graph_param_edit_ && graph_param_edit_->generator_root.id() == ObjectId{} &&
         graph_param_edit_->graph_key == key))
        return true;
    if (open.dirty_revision == revision_)
        return open.dirty_cached;
    const auto encoded = editor::save_graph(world_.runtime(), open.document);
    open.dirty_cached = !encoded || *encoded != open.saved_text;
    open.dirty_revision = revision_;
    return open.dirty_cached;
}

Array<String> Session::dirty_graphs() const {
    Array<String> result;
    for (const auto& [key, open] : graphs_) {
        if (open && !open->scratch && graph_dirty(key))
            result.add(key);
    }
    std::ranges::sort(result);
    return result;
}

bool Session::graph_can_undo(StringView key, bool redo) const {
    const auto* at = graphs_.find(key);
    return at && *at && (redo ? (*at)->cursor < (*at)->history.size() : (*at)->cursor > 0);
}

Result<void> Session::save_graph(Project& project, StringView key) {
    if (editing())
        return graph_error("Finish the active field edit before saving.", ErrorCode::invalid_definition);
    auto* at = graphs_.find(key);
    if (!at || !*at)
        return graph_error("Procedural graph asset is not open.", ErrorCode::invalid_definition);
    if ((*at)->scratch)
        return graph_error("Scratch procedural graphs have no project asset path.", ErrorCode::invalid_storage);
    ATELIER_TRY_BIND(encoded, editor::save_graph(runtime_, (*at)->document));
    ATELIER_TRY(project.save_procedural_graph(key, *encoded));
    (*at)->saved_text = std::move(*encoded);
    (*at)->dirty_revision = 0;
    unavailable_.remove(std::pair{assets::AssetKind::procedural_graph, key});
    ++revision_;
    return {};
}

Result<Session::OpenGraph*> Session::editable_graph(StringView key, bool need_program) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (need_program)
        ATELIER_TRY(procedural_source_program());
    auto* at = graphs_.find(key);
    if (!at || !*at)
        return graph_error(String::concat("Procedural graph is not open: ", lossy_utf8(key)),
                           ErrorCode::invalid_definition);
    return at->get();
}

Result<void> Session::OpenGraph::replay(Session& session, GraphChange& entry, bool redo) {
    auto& runtime_ = session.runtime_;
    auto& open = *this;
    const auto graph = open.document.graph.id();
    return std::visit(
        [&](auto& change) -> Result<void> {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::same_as<T, GraphScalarChange>) {
                const auto& values = redo ? change.after : change.before;
                ATELIER_TRY(session.write_group(change.target.id(), change.paths, values));
                auto* node = find_node_by_id(open.document, change.target.id());
                (node ? node->kept : open.document.kept) = redo ? change.after_kept : change.before_kept;
                return {};
            } else if constexpr (std::same_as<T, GraphPropertyChange>) {
                ATELIER_TRY(patch_object_properties(runtime_, change.target.id(),
                                                    (redo ? change.after : change.before).root(), no_graph_references));
                if (change.target.id() == graph)
                    open.document.kept = redo ? change.after_kept : change.before_kept;
                else if (auto* item = find_node_by_id(open.document, change.target.id()))
                    item->kept = redo ? change.after_kept : change.before_kept;
                return {};
            } else if constexpr (std::same_as<T, GraphDisplayChange>) {
                ATELIER_TRY(procedural::set_display(runtime_, graph, redo ? change.after : change.before));
                open.document.kept = redo ? change.after_kept : change.before_kept;
                return {};
            } else if constexpr (std::same_as<T, GraphTemplateChange>) {
                open.document.template_node = redo ? change.after : change.before;
                open.document.kept = redo ? change.after_kept : change.before_kept;
                return {};
            } else if constexpr (std::same_as<T, GraphBypassChange>) {
                return procedural::set_bypassed(runtime_, change.target.id(), redo ? change.after : change.before);
            } else if constexpr (std::same_as<T, GraphFreezeChange>) {
                const bool enabled = redo == change.enabled_after;
                return procedural::set_frozen(runtime_, change.target.id(), enabled,
                                              enabled ? change.snapshot : nullptr);
            } else if constexpr (std::same_as<T, GraphAppearanceChange>) {
                for (const auto& entry_delta : change.entries) {
                    if (auto* item = find_node(open.document, entry_delta.name)) {
                        item->shape = redo ? entry_delta.after_shape : entry_delta.before_shape;
                        item->color = redo ? entry_delta.after_color : entry_delta.before_color;
                        item->comment = redo ? entry_delta.after_comment : entry_delta.before_comment;
                    }
                }
                return {};
            } else if constexpr (std::same_as<T, GraphAnnotationChange>) {
                open.document.notes = redo ? change.after_notes : change.before_notes;
                open.document.boxes = redo ? change.after_boxes : change.before_boxes;
                sort_graph_notes(open.document.notes);
                sort_graph_boxes(open.document.boxes);
                return {};
            } else if constexpr (std::same_as<T, GraphRenameChange>) {
                const auto& from = redo ? change.before : change.after;
                const auto& to = redo ? change.after : change.before;
                auto* item = find_node(open.document, from);
                if (!item)
                    return graph_error("Renamed node is missing from graph history.");
                for (const auto& ref : change.references) {
                    const auto name = !redo && ref.owner == change.before ? change.after : ref.owner;
                    auto& kept = ref.owner.empty() ? open.document.kept : find_node(open.document, name)->kept;
                    kept = redo ? ref.after : ref.before;
                }
                item->name = to;
                sort_graph_nodes(open.document.nodes);
                open.document.parameters = redo ? change.after_parameters : change.before_parameters;
                return {};
            } else if constexpr (std::same_as<T, GraphMoveChange>) {
                for (const auto& move : change.moves) {
                    if (auto* item = find_node(open.document, move.name))
                        item->position = redo ? move.after : move.before;
                }
                for (const auto& move : change.note_moves) {
                    if (auto* item = find_note(open.document, move.name))
                        item->position = redo ? move.after : move.before;
                }
                for (const auto& move : change.box_moves) {
                    if (auto* item = find_box(open.document, move.name))
                        item->position = redo ? move.after : move.before;
                }
                return {};
            } else if constexpr (std::same_as<T, GraphConnectionChange>) {
                if (change.targets.size() > 1) {
                    for (const auto& item_delta : change.targets)
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, item_delta.target.id(), {}));
                }
                if (redo) {
                    for (const auto& item_delta : change.targets) {
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, item_delta.target.id(), item_delta.after));
                        if (auto* item = find_node_by_id(open.document, item_delta.target.id()))
                            item->kept = item_delta.after_kept;
                    }
                } else {
                    for (auto it = change.targets.rbegin(); it != change.targets.rend(); ++it) {
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, it->target.id(), it->before));
                        if (auto* item = find_node_by_id(open.document, it->target.id()))
                            item->kept = it->before_kept;
                    }
                }
                for (const auto& move : change.moves) {
                    if (auto* item = find_node(open.document, move.name))
                        item->position = redo ? move.after : move.before;
                }
                return {};
            } else if constexpr (std::same_as<T, GraphNodesChange>) {
                const bool attach = change.added == redo;
                if (attach) {
                    for (const auto& node_entry : change.entries) {
                        if (node_entry.node.registered())
                            ATELIER_TRY(procedural::restore_node(runtime_, graph, node_entry.node));
                        open.document.nodes.add(node_entry);
                    }
                    sort_graph_nodes(open.document.nodes);
                    for (const auto& saved : change.saved_inputs)
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, saved.inputs));
                    open.document.template_node = change.added ? change.after_template : change.before_template;
                    open.document.kept = change.added ? change.after_kept : change.before_kept;
                    for (const auto& ref : change.retained_node_kept) {
                        if (auto* item = find_node(open.document, ref.owner))
                            item->kept = change.added ? ref.after : ref.before;
                    }
                    open.document.parameters = change.added ? change.after_parameters : change.before_parameters;
                    return procedural::set_display(runtime_, graph,
                                                   change.added ? change.after_display : change.before_display);
                }
                for (const auto& node_entry : change.entries) {
                    if (node_entry.node.registered()) {
                        detach_asset_node_subgraph(runtime_, node_entry.node.id());
                        ATELIER_TRY(procedural::remove_node(runtime_, graph, node_entry.node.id()));
                    }
                    open.document.nodes.remove_if(
                        [&](const GraphNode& item) { return item.name == node_entry.name; });
                }
                if (change.added) {
                    for (const auto& saved : change.saved_inputs)
                        if (find_node_by_id(open.document, saved.to))
                            ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, saved.detached_inputs));
                }
                open.document.template_node = change.added ? change.before_template : change.after_template;
                open.document.kept = change.added ? change.before_kept : change.after_kept;
                for (const auto& ref : change.retained_node_kept) {
                    if (auto* item = find_node(open.document, ref.owner))
                        item->kept = change.added ? ref.before : ref.after;
                }
                open.document.parameters = change.added ? change.before_parameters : change.after_parameters;
                return procedural::set_display(runtime_, graph,
                                               change.added ? change.before_display : change.after_display);
            } else if constexpr (std::same_as<T, GraphParameterSchemaChange>) {
                open.document.parameters = redo ? change.after_parameters : change.before_parameters;
                for (const auto& snap : change.synced_nodes) {
                    ATELIER_TRY(apply_object_properties(runtime_, snap.target.id(),
                                                        (redo ? snap.after_props : snap.before_props).root()));
                    if (auto* item = find_node_by_id(open.document, snap.target.id()))
                        item->kept = redo ? snap.after_kept : snap.before_kept;
                }
                return {};
            } else if constexpr (std::same_as<T, GraphAssetOverrideChange>) {
                auto* item = find_node(open.document, change.node_name);
                if (!item)
                    return {};
                item->overrides = redo ? change.after_overrides : change.before_overrides;
                item->asset_key = redo ? change.after_asset : change.before_asset;
                if (change.asset_changed && item->node.registered()) {
                    for (const auto& delta : change.connection_deltas)
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, delta.target.id(), {}));
                    procedural::AssetSubgraph stub;
                    stub.asset_key = item->asset_key;
                    stub.ports = redo ? change.after_ports : change.before_ports;
                    ATELIER_TRY(procedural::set_asset_subgraph(runtime_, item->node.id(), std::move(stub)));
                    if (redo) {
                        for (const auto& delta : change.connection_deltas) {
                            ATELIER_TRY(replace_node_inputs(runtime_, graph, delta.target.id(), delta.after));
                            if (auto* dn = find_node_by_id(open.document, delta.target.id()))
                                dn->kept = delta.after_kept;
                        }
                    } else {
                        for (auto it = change.connection_deltas.rbegin(); it != change.connection_deltas.rend(); ++it) {
                            ATELIER_TRY(replace_node_inputs(runtime_, graph, it->target.id(), it->before));
                            if (auto* dn = find_node_by_id(open.document, it->target.id()))
                                dn->kept = it->before_kept;
                        }
                    }
                }
                return {};
            } else if constexpr (std::same_as<T, GraphExtractChange>) {
                if (redo) {
                    for (const auto& saved : change.before_inputs) {
                        if (find_node_by_id(open.document, saved.to))
                            ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, {}));
                    }
                    for (const auto& rem : change.removed_nodes) {
                        if (rem.node.registered()) {
                            detach_asset_node_subgraph(runtime_, rem.node.id());
                            ATELIER_TRY(procedural::remove_node(runtime_, graph, rem.node.id()));
                        }
                        open.document.nodes.remove_if(
                            [&](const GraphNode& item) { return item.name == rem.name; });
                    }
                    if (change.added_asset_node.node.registered())
                        ATELIER_TRY(procedural::restore_node(runtime_, graph, change.added_asset_node.node));
                    open.document.nodes.add(change.added_asset_node);
                    sort_graph_nodes(open.document.nodes);
                    for (const auto& saved : change.after_inputs)
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, saved.inputs));
                    for (const auto& ref : change.retained_node_kept) {
                        if (auto* item = find_node(open.document, ref.owner))
                            item->kept = ref.after;
                    }
                    open.document.template_node = change.after_template;
                    open.document.parameters = change.after_parameters;
                    ATELIER_TRY(procedural::set_display(runtime_, graph, change.after_display));
                    return session.sync_open_graph_assets(open, open.resources.project(), false);
                }
                for (const auto& saved : change.after_inputs) {
                    if (find_node_by_id(open.document, saved.to))
                        ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, {}));
                }
                if (change.added_asset_node.node.registered()) {
                    detach_asset_node_subgraph(runtime_, change.added_asset_node.node.id());
                    ATELIER_TRY(procedural::remove_node(runtime_, graph, change.added_asset_node.node.id()));
                }
                open.document.nodes.remove_if(
                    [&](const GraphNode& item) { return item.name == change.added_asset_node.name; });
                for (const auto& rem : change.removed_nodes) {
                    if (rem.node.registered())
                        ATELIER_TRY(procedural::restore_node(runtime_, graph, rem.node));
                    open.document.nodes.add(rem);
                }
                sort_graph_nodes(open.document.nodes);
                for (const auto& saved : change.before_inputs)
                    ATELIER_TRY(replace_node_inputs(runtime_, graph, saved.to, saved.inputs));
                for (const auto& ref : change.retained_node_kept) {
                    if (auto* item = find_node(open.document, ref.owner))
                        item->kept = ref.before;
                }
                open.document.template_node = change.before_template;
                open.document.parameters = change.before_parameters;
                return procedural::set_display(runtime_, graph, change.before_display);
            }
        },
        entry);
}

Result<void> Session::OpenGraph::replay(Session& session, Span<GraphChange> changes, bool redo) {
    const auto at = [&](std::size_t i) -> GraphChange& { return changes[redo ? i : changes.size() - i - 1]; };
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (auto applied = replay(session, at(i), redo); !applied) {
            while (i > 0) {
                if (auto restored = replay(session, at(--i), !redo); !restored) {
                    applied.error().message = String::concat(applied.error().message, " (restoring graph history: ",
                                                             restored.error().message, ")");
                    break;
                }
            }
            return applied;
        }
    }
    return {};
}

Result<void> Session::graph_undo(StringView key, bool redo) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    ATELIER_TRY(cancel_edit());
    auto& open = **open_ptr;
    if (!graph_can_undo(key, redo))
        return {};
    auto& entry = open.history[redo ? open.cursor : open.cursor - 1];
    if (auto* one = std::get_if<GraphChange>(&entry)) {
        ATELIER_TRY(open.replay(*this, *one, redo));
    } else {
        ATELIER_TRY(open.replay(*this, std::get<Array<GraphChange>>(entry), redo));
    }
    if (redo)
        ++open.cursor;
    else
        --open.cursor;
    ++open.mutation_epoch;
    open.dirty_revision = 0;
    ++revision_;
    return {};
}

Result<String> Session::add_graph_node_impl(StringView key, StringView type_name, StringView desired_name,
                                            std::array<float, 2> position, StringView from_node, StringView from_port,
                                            StringView to_node, StringView to_port, bool set_display,
                                            Project* project) {
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
        return graph_error("Node position must be finite.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    Project* active_project = project ? project : open.resources.project();
    String asset_key;
    String stem_source;
    String resolved_type_name;
    if (type_name.starts_with("asset:")) {
        const StringView raw_asset = type_name.slice(6);
        if (raw_asset.empty())
            return graph_error("AssetNode requires a non-empty .procgraph asset key.");
        auto ak = String::from_utf8(raw_asset);
        if (!ak)
            return graph_error("AssetNode asset key must be valid UTF-8.");
        asset_key = std::move(*ak);
        if (!open.scratch && is_scratch_graph(asset_key))
            return graph_error("A persistent .procgraph asset cannot reference a scratch graph.");
        if (!open.scratch && asset_key == open.key)
            return graph_error("A .procgraph asset cannot instantiate itself.");
        resolved_type_name = "procedural.AssetNode"_s;
        if (active_project) {
            if (const auto* asset_entry = active_project->entry(asset_key))
                stem_source = platform::utf16_to_utf8(asset_entry->path.stem().wstring()).value_or(String{});
        }
        if (stem_source.empty())
            stem_source = "asset"_s;
    } else {
        auto rt = String::from_utf8(type_name);
        if (!rt)
            return graph_error(String::concat("Unknown procedural node type: ", lossy_utf8(type_name)));
        resolved_type_name = std::move(*rt);
    }
    auto type = runtime_.find_type(resolved_type_name);
    if (!type && !resolved_type_name.contains('.')) {
        resolved_type_name = String::concat("procedural.", resolved_type_name);
        type = runtime_.find_type(resolved_type_name);
    }
    if (!type)
        return graph_error(String::concat("Unknown procedural node type: ", lossy_utf8(type_name)));
    ATELIER_TRY(procedural::ports(runtime_, *type));

    String name;
    if (desired_name.empty()) {
        String stem = default_node_stem(stem_source.empty() ? resolved_type_name.view() : stem_source.view());
        if (stem.size() > 118)
            stem = String::from_utf8_validated(stem.view().first(118));
        for (std::uint32_t index = 1;; ++index) {
            name = String::concat(stem, String::from_number(index));
            if (!find_node(open.document, name))
                break;
        }
    } else {
        if (!valid_graph_identifier(desired_name))
            return graph_error(
                String::concat("Invalid node name '", lossy_utf8(desired_name), "': expected [A-Za-z_][A-Za-z0-9_]*."));
        name = String::from_utf8_validated(desired_name);
        if (find_node(open.document, name))
            return graph_error(String::concat("A node named '", name, "' already exists in this graph."));
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(before_contents, procedural::contents(runtime_, graph));
    const bool make_initial_display = before_contents->nodes().empty() && before_contents->display() == ObjectId{} &&
                                      !open.document.kept.root().find("display")->has_value();

    ATELIER_TRY_BIND(created, procedural::add_node(runtime_, graph, *type));
    const ObjectId created_id = created->id();
    if (!asset_key.empty())
        ATELIER_TRY(procedural::set_asset_subgraph(
            runtime_, created_id, resolve_asset_binding(make_asset_port_resolver(active_project), asset_key)));
    ATELIER_TRY_BIND(new_ports, procedural::ports(runtime_, created_id));
    Array<GraphNodesChange::SavedInputs> saved_inputs;

    if (!from_node.empty() && !new_ports->inputs.empty()) {
        if (const auto* src = find_node(open.document, from_node); src && src->node.registered()) {
            if (const auto src_ports = procedural::ports(runtime_, src->node.id());
                src_ports && !src_ports->outputs.empty()) {
                std::uint32_t out_idx = 0;
                if (!from_port.empty()) {
                    const auto found = std::ranges::find(src_ports->outputs, from_port);
                    if (found != src_ports->outputs.end())
                        out_idx = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
                }
                if (procedural::connect(runtime_, graph, src->node.id(), out_idx, created_id, 0)) {
                    ATELIER_TRY_BIND(listed_now, procedural::contents(runtime_, graph));
                    ATELIER_TRY_BIND(after_in, procedural::inputs(runtime_, *listed_now, created_id));
                    saved_inputs.add({created_id, std::move(*after_in), {}});
                }
            }
        }
    }

    bool connected_to_dst = false;
    std::uint32_t dst_in_idx = 0;
    String dst_port_name;
    bool dst_port_multi = false;
    Array<procedural::Input> dst_after_snapshot;
    if (!to_node.empty() && !new_ports->outputs.empty()) {
        if (const auto* dst = find_node(open.document, to_node); dst && dst->node.registered()) {
            if (const auto dst_ports = procedural::ports(runtime_, dst->node.id());
                dst_ports && !dst_ports->inputs.empty()) {
                std::uint32_t in_idx = 0;
                if (!to_port.empty()) {
                    const auto found = std::ranges::find(dst_ports->inputs, to_port);
                    if (found != dst_ports->inputs.end())
                        in_idx = static_cast<std::uint32_t>(found - dst_ports->inputs.begin());
                }
                ATELIER_TRY_BIND(listed_before, procedural::contents(runtime_, graph));
                ATELIER_TRY_BIND(before_dst, procedural::inputs(runtime_, *listed_before, dst->node.id()));
                if (procedural::connect(runtime_, graph, created_id, 0, dst->node.id(), in_idx)) {
                    ATELIER_TRY_BIND(listed_now, procedural::contents(runtime_, graph));
                    ATELIER_TRY_BIND(after_dst, procedural::inputs(runtime_, *listed_now, dst->node.id()));
                    dst_after_snapshot = *after_dst;
                    dst_in_idx = in_idx;
                    dst_port_name = dst_ports->inputs[in_idx];
                    dst_port_multi = dst_ports->multiple[in_idx];
                    connected_to_dst = true;
                    saved_inputs.add({dst->node.id(), std::move(*after_dst), std::move(*before_dst)});
                }
            }
        }
    }

    const ObjectId after_display = (set_display || make_initial_display) ? created_id : before_contents->display();
    if (after_display != before_contents->display())
        ATELIER_TRY(procedural::set_display(runtime_, graph, after_display));

    GraphNode entry;
    entry.name = name;
    entry.node = std::move(*created);
    entry.asset_key = std::move(asset_key);
    entry.position = position;
    open.document.nodes.add(entry);
    sort_graph_nodes(open.document.nodes);

    Array<GraphNodesChange::RetainedNodeKept> retained_node_kept;
    if (connected_to_dst) {
        if (auto* dst_mut = find_node(open.document, to_node)) {
            auto before_dst_kept = dst_mut->kept;
            ATELIER_TRY_BIND(dst_live_after,
                             format_live_port_endpoints(runtime_, open.document, dst_after_snapshot, dst_in_idx));
            append_kept_port_endpoint(dst_mut->kept, dst_port_name, dst_port_multi,
                                      String::concat(name, ".", new_ports->outputs[0]), *dst_live_after);
            if (dst_mut->kept != before_dst_kept)
                retained_node_kept.add({dst_mut->name, std::move(before_dst_kept), dst_mut->kept});
        }
    }

    open.push_change(GraphNodesChange{true,
                                      {std::move(entry)},
                                      std::move(saved_inputs),
                                      before_contents->display(),
                                      after_display,
                                      open.document.template_node,
                                      open.document.template_node,
                                      open.document.kept,
                                      open.document.kept,
                                      std::move(retained_node_kept),
                                      open.document.parameters,
                                      open.document.parameters});
    ++revision_;
    return name;
}

Result<String> Session::graph_add_node(StringView key, StringView type_name, StringView desired_name,
                                       std::array<float, 2> position, Project* project) {
    return add_graph_node_impl(key, type_name, desired_name, position, {}, {}, {}, {}, false, project);
}

Result<String> Session::graph_place_node(StringView key, StringView type_name, std::array<float, 2> position,
                                         StringView from_node, StringView from_port, StringView to_node,
                                         StringView to_port, bool set_display, Project* project) {
    return add_graph_node_impl(key, type_name, {}, position, from_node, from_port, to_node, to_port, set_display,
                               project);
}

Result<void> Session::graph_remove_nodes(StringView key, Span<const String> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    Array<String> unique_names;
    Set<ObjectId> removed_ids;
    for (const auto& name : names) {
        if (std::ranges::find(unique_names, name) != unique_names.end())
            continue;
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error(String::concat("No node named '", name, "' in this graph."));
        unique_names.add(name);
        if (item->node.registered())
            removed_ids.add(item->node.id());
    }
    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));

    GraphNodesChange change;
    change.added = false;
    change.before_display = listed->display();
    change.after_display = removed_ids.contains(listed->display()) ? ObjectId{} : listed->display();
    change.before_template = open.document.template_node;
    change.after_template =
        removed_ids.contains(open.document.template_node) ? ObjectId{} : open.document.template_node;
    change.before_kept = open.document.kept;
    change.before_parameters = open.document.parameters;
    {
        auto next_params = open.document.parameters;
        for (auto& param : next_params) {
            param.targets.remove_if([&](const String& target) {
                const auto dot = target.find('.');
                if (dot == String::npos)
                    return false;
                return std::ranges::find(unique_names, target.view().slice(0, dot)) != unique_names.end();
            });
        }
        next_params.remove_if([](const GraphPromotedParameter& p) { return p.targets.empty(); });
        change.after_parameters = std::move(next_params);
    }
    {
        json::Builder next_kept(open.document.kept);
        if (const auto kd = open.document.kept.root().string({"display"});
            kd && std::ranges::find(unique_names, *kd) != unique_names.end())
            next_kept.erase(next_kept.root(), "display");
        if (const auto kt = open.document.kept.root().string({"template"});
            kt && std::ranges::find(unique_names, *kt) != unique_names.end())
            next_kept.erase(next_kept.root(), "template");
        change.after_kept = next_kept.finish();
    }

    // Capture full input order of every removed node and every surviving downstream consumer of a removed node.
    for (const auto node_id : listed->nodes()) {
        ATELIER_TRY_BIND(into, procedural::inputs(runtime_, *listed, node_id));
        const bool is_removed = removed_ids.contains(node_id);
        const bool reads_removed =
            std::ranges::any_of(*into, [&](const procedural::Input& in) { return removed_ids.contains(in.from); });
        if ((is_removed && !into->empty()) || (!is_removed && reads_removed))
            change.saved_inputs.add({node_id, std::move(*into)});
    }
    for (const auto& name : unique_names) {
        auto* item = find_node(open.document, name);
        change.entries.add(*item);
        if (item->node.registered()) {
            detach_asset_node_subgraph(runtime_, item->node.id());
            ATELIER_TRY(procedural::remove_node(runtime_, graph, item->node.id()));
        }
    }
    open.document.template_node = change.after_template;
    open.document.kept = change.after_kept;
    open.document.parameters = change.after_parameters;
    open.document.nodes.remove_if([&](const GraphNode& item) {
        return std::ranges::find(unique_names, item.name) != unique_names.end();
    });

    ATELIER_TRY_BIND(listed_after, procedural::contents(runtime_, graph));
    const auto uses_removed_node = [&](const String& ep) noexcept {
        const auto dot = ep.find('.');
        if (dot == String::npos)
            return false;
        const auto upstream = ep.view().slice(0, dot);
        return std::ranges::find(unique_names, upstream) != unique_names.end();
    };
    for (auto& surv : open.document.nodes) {
        const auto cur_inputs = *surv.kept.root().find("inputs");
        if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
            continue;
        auto before_surv_kept = surv.kept;
        Array<String> kept_ports;
        for (const auto& [pname, _] : *cur_inputs->object_items())
            kept_ports.add(String::from_utf8_validated(pname));
        if (surv.node.registered()) {
            ATELIER_TRY_BIND(surv_ports, procedural::ports(runtime_, surv.node.id()));
            ATELIER_TRY_BIND(surv_in, procedural::inputs(runtime_, *listed_after, surv.node.id()));
            for (const auto& pname : kept_ports) {
                const auto p_it = std::ranges::find(surv_ports->inputs, pname);
                if (p_it != surv_ports->inputs.end()) {
                    const auto p_idx = static_cast<std::uint32_t>(p_it - surv_ports->inputs.begin());
                    ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, *surv_in, p_idx));
                    filter_kept_port_endpoints(surv.kept, pname, surv_ports->multiple[p_idx], *live_after,
                                               uses_removed_node);
                } else {
                    filter_kept_port_endpoints(surv.kept, pname, false, {}, uses_removed_node);
                }
            }
        } else {
            for (const auto& pname : kept_ports)
                filter_kept_port_endpoints(surv.kept, pname, false, {}, uses_removed_node);
        }
        if (surv.kept != before_surv_kept)
            change.retained_node_kept.add({surv.name, std::move(before_surv_kept), surv.kept});
    }

    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<Array<String>>
Session::graph_duplicate_nodes(StringView key, Span<const String> names, std::array<float, 2> offset) {
    if (!std::isfinite(offset[0]) || !std::isfinite(offset[1]))
        return graph_error("Duplicate offset must be finite.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    if (names.empty())
        return Array<String>{};
    auto& open = **open_ptr;
    Array<String> unique_names;
    for (const auto& name : names) {
        if (std::ranges::find(unique_names, name) != unique_names.end())
            continue;
        if (!find_node(open.document, name))
            return graph_error(String::concat("No node named '", name, "' in this graph."));
        unique_names.add(name);
    }
    if (unique_names.empty())
        return Array<String>{};

    ATELIER_TRY(cancel_edit());
    const ObjectId graph = open.document.graph.id();
    ATELIER_TRY_BIND(before_contents, procedural::contents(runtime_, graph));

    Array<String> created_names;
    created_names.reserve(unique_names.size());
    Map<StringView, std::size_t> copy_name_indices;
    copy_name_indices.reserve(unique_names.size());
    for (const auto& src_name : unique_names) {
        StringView stem = src_name.view();
        while (stem.size() > 1 && stem.back() >= '0' && stem.back() <= '9')
            stem.remove_suffix(1);
        if (stem.size() > 118)
            stem = stem.first(118);
        String candidate;
        for (std::uint32_t idx = 1;; ++idx) {
            candidate = String::concat(String::validated(stem), String::from_number(idx));
            if (!find_node(open.document, candidate) &&
                std::ranges::find(created_names, candidate) == created_names.end())
                break;
        }
        copy_name_indices.try_emplace(src_name.view(), created_names.size());
        created_names.add(std::move(candidate));
    }

    Array<ObjectId> added_live_ids;
    ScopeExit rollback{[&]() noexcept {
        for (auto it = added_live_ids.rbegin(); it != added_live_ids.rend(); ++it)
            static_cast<void>(procedural::remove_node(runtime_, graph, *it));
    }};

    Array<GraphNode> copy_entries;
    copy_entries.reserve(unique_names.size());
    Map<ObjectId, ObjectId> id_remap;
    id_remap.reserve(unique_names.size());
    for (std::size_t i = 0; i < unique_names.size(); ++i) {
        const auto* src = find_node(open.document, unique_names[i]);
        GraphNode copy_entry;
        copy_entry.name = created_names[i];
        copy_entry.asset_key = src->asset_key;
        copy_entry.overrides = src->overrides;
        copy_entry.position = {std::round(src->position[0] + offset[0]), std::round(src->position[1] + offset[1])};
        copy_entry.shape = src->shape;
        copy_entry.color = src->color;
        copy_entry.comment = src->comment;
        copy_entry.kept = src->kept;
        if (copy_entry.kept.root().find("inputs")->has_value()) {
            json::Builder next(copy_entry.kept);
            const auto inputs = *next.root().at("inputs");
            bool changed = false;
            const auto remap_endpoint = [&](json::Value endpoint) {
                if (const auto text = endpoint.string()) {
                    const auto dot = text->find('.');
                    if (dot != StringView::npos) {
                        if (const auto* index = copy_name_indices.find(text->slice(0, dot))) {
                            next.set_validated(
                                endpoint, String::concat(created_names[*index], String::validated(text->slice(dot))));
                            changed = true;
                        }
                    }
                }
            };
            if (const auto ports = inputs.object_items()) {
                for (const auto& [_, value] : *ports) {
                    if (value.kind() == json::Kind::array) {
                        for (const auto endpoint : *value.array())
                            remap_endpoint(endpoint);
                    } else {
                        remap_endpoint(value);
                    }
                }
            }
            if (changed)
                copy_entry.kept = next.finish();
        }
        if (src->node.registered()) {
            ATELIER_TRY_BIND(type, runtime_.type_of(src->node.id()));
            ATELIER_TRY_BIND(props, capture_object_properties(runtime_, src->node.id()));
            ATELIER_TRY_BIND(is_byp, procedural::bypassed(runtime_, src->node.id()));
            ATELIER_TRY_BIND(is_frz, procedural::frozen(runtime_, src->node.id()));
            ATELIER_TRY_BIND(frz_snap, procedural::frozen_output(runtime_, src->node.id()));
            ATELIER_TRY_BIND(created_node, procedural::add_node(runtime_, graph, *type));
            const ObjectId dst_id = created_node->id();
            added_live_ids.add(dst_id);
            ATELIER_TRY(apply_object_properties(runtime_, dst_id, props->root()));
            if (auto src_sub = procedural::asset_subgraph(runtime_, src->node.id())) {
                procedural::AssetSubgraph stub;
                stub.asset_key = src_sub->asset_key;
                stub.ports = src_sub->ports;
                ATELIER_TRY(procedural::set_asset_subgraph(runtime_, dst_id, std::move(stub)));
            }
            if (*is_byp)
                ATELIER_TRY(procedural::set_bypassed(runtime_, dst_id, true));
            if (*is_frz)
                ATELIER_TRY(procedural::set_frozen(runtime_, dst_id, true, *frz_snap));
            id_remap.try_emplace(src->node.id(), dst_id);
            copy_entry.node = std::move(*created_node);
        }
        copy_entries.add(std::move(copy_entry));
    }

    Array<GraphNodesChange::SavedInputs> saved_inputs;
    for (std::size_t i = 0; i < unique_names.size(); ++i) {
        const auto* src = find_node(open.document, unique_names[i]);
        if (!src->node.registered() || !copy_entries[i].node.registered())
            continue;
        const ObjectId src_id = src->node.id();
        const ObjectId dst_id = copy_entries[i].node.id();
        ATELIER_TRY_BIND(src_inputs, procedural::inputs(runtime_, *before_contents, src_id));
        if (src_inputs->empty())
            continue;
        for (const auto& in : *src_inputs) {
            const auto* mapped_from = id_remap.find(in.from);
            const ObjectId from_id = mapped_from ? *mapped_from : in.from;
            ATELIER_TRY(procedural::connect(runtime_, graph, from_id, in.output, dst_id, in.input));
        }
        ATELIER_TRY_BIND(listed_now, procedural::contents(runtime_, graph));
        ATELIER_TRY_BIND(after_in, procedural::inputs(runtime_, *listed_now, dst_id));
        saved_inputs.add({dst_id, std::move(*after_in), {}});
    }

    rollback.release();
    for (const auto& entry : copy_entries)
        open.document.nodes.add(entry);
    sort_graph_nodes(open.document.nodes);
    open.push_change(GraphNodesChange{true,
                                      std::move(copy_entries),
                                      std::move(saved_inputs),
                                      before_contents->display(),
                                      before_contents->display(),
                                      open.document.template_node,
                                      open.document.template_node,
                                      open.document.kept,
                                      open.document.kept,
                                      {},
                                      open.document.parameters,
                                      open.document.parameters});
    ++revision_;
    return created_names;
}

Result<void> Session::graph_rename_node(StringView key, StringView old_name, StringView new_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!valid_graph_identifier(new_name))
        return graph_error(
            String::concat("Invalid node name '", lossy_utf8(new_name), "': expected [A-Za-z_][A-Za-z0-9_]*."));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, old_name);
    if (!item)
        return graph_error(String::concat("No node named '", lossy_utf8(old_name), "' in this graph."));
    if (old_name == new_name)
        return {};
    if (find_node(open.document, new_name))
        return graph_error(String::concat("A node named '", lossy_utf8(new_name), "' already exists in this graph."));
    ATELIER_TRY(cancel_edit());
    const String new_name_str = String::from_utf8_validated(new_name);
    GraphRenameChange change{item->name, new_name_str, {}};
    // Unresolved multi-input ports retain their entire ordered endpoint list, including live edges.
    for (auto& node : open.document.nodes) {
        if (!node.kept.root().find("inputs")->has_value())
            continue;
        json::Builder next(node.kept);
        const auto inputs = *next.root().at("inputs");
        bool changed = false;
        const auto rename_endpoint = [&](json::Value endpoint) {
            const auto text = *endpoint.string();
            const auto dot = text.find('.');
            if (dot != StringView::npos && text.slice(0, dot) == old_name) {
                next.set_validated(endpoint, String::concat(new_name_str, String::validated(text.slice(dot))));
                changed = true;
            }
        };
        const auto ports = *inputs.object_items();
        for (const auto& [port, value] : ports) {
            if (value.kind() == json::Kind::array) {
                for (const auto endpoint : *value.array())
                    rename_endpoint(endpoint);
            } else {
                rename_endpoint(value);
            }
        }
        if (changed) {
            change.references.add({node.name, node.kept, next.finish()});
            node.kept = change.references.back().after;
        }
    }
    const bool kept_disp = open.document.kept.root().string({"display"}).value_or("") == old_name;
    const bool kept_tmpl = open.document.kept.root().string({"template"}).value_or("") == old_name;
    if (kept_disp || kept_tmpl) {
        json::Builder next(open.document.kept);
        if (kept_disp)
            next.set_validated(next.root(), "display", new_name);
        if (kept_tmpl)
            next.set_validated(next.root(), "template", new_name);
        change.references.add({{}, open.document.kept, next.finish()});
        open.document.kept = change.references.back().after;
    }
    change.before_parameters = open.document.parameters;
    for (auto& param : open.document.parameters) {
        for (auto& target : param.targets) {
            const auto dot = target.find('.');
            if (dot != String::npos && target.view().slice(0, dot) == old_name)
                target = String::concat(new_name_str, String::validated(target.view().slice(dot)));
        }
    }
    change.after_parameters = open.document.parameters;
    item->name = new_name_str;
    sort_graph_nodes(open.document.nodes);
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_move_canvas_items(StringView key,
                                              Span<const std::pair<String, std::array<float, 2>>> node_moves,
                                              Span<const std::pair<String, std::array<float, 2>>> note_moves,
                                              Span<const std::pair<String, std::array<float, 2>>> box_moves) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    GraphMoveChange change;
    for (const auto& [name, next_pos] : node_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Node position must be finite.");
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error(String::concat("No node named '", name, "' in this graph."));
        if (item->position != next_pos)
            change.moves.add({name, item->position, next_pos});
    }
    for (const auto& [name, next_pos] : note_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Sticky note position must be finite.");
        const auto* item = find_note(open.document, name);
        if (!item)
            return graph_error(String::concat("No sticky note named '", name, "' in this graph."));
        if (item->position != next_pos)
            change.note_moves.add({name, item->position, next_pos});
    }
    for (const auto& [name, next_pos] : box_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Network box position must be finite.");
        const auto* item = find_box(open.document, name);
        if (!item)
            return graph_error(String::concat("No network box named '", name, "' in this graph."));
        if (item->position != next_pos)
            change.box_moves.add({name, item->position, next_pos});
    }
    if (change.moves.empty() && change.note_moves.empty() && change.box_moves.empty())
        return {};
    ATELIER_TRY(cancel_edit());
    for (const auto& move : change.moves)
        find_node(open.document, move.name)->position = move.after;
    for (const auto& move : change.note_moves)
        find_note(open.document, move.name)->position = move.after;
    for (const auto& move : change.box_moves)
        find_box(open.document, move.name)->position = move.after;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_move_nodes(StringView key,
                                       Span<const std::pair<String, std::array<float, 2>>> moves,
                                       bool amend_creation) {
    if (amend_creation && !moves.empty()) {
        ATELIER_TRY_BIND(open_ptr, editable_graph(key));
        auto& open = **open_ptr;
        if (open.cursor > 0) {
            if (auto* created = std::get_if<GraphNodesChange>(std::get_if<GraphChange>(&open.history[open.cursor - 1]));
                created && created->added) {
                const bool all_match = std::ranges::all_of(moves, [&](const auto& m) {
                    return std::isfinite(m.second[0]) && std::isfinite(m.second[1]) &&
                           find_node(open.document, m.first) != nullptr &&
                           std::ranges::any_of(created->entries, [&](const GraphNode& e) { return e.name == m.first; });
                });
                if (all_match) {
                    ATELIER_TRY(cancel_edit());
                    for (const auto& [name, next_pos] : moves) {
                        find_node(open.document, name)->position = next_pos;
                        for (auto& e : created->entries)
                            if (e.name == name)
                                e.position = next_pos;
                    }
                    ++open.mutation_epoch;
                    open.dirty_revision = 0;
                    ++revision_;
                    return {};
                }
            }
        }
    }
    return graph_move_canvas_items(key, moves, {}, {});
}

Result<void> Session::graph_auto_layout(StringView key) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const std::size_t n = open.document.nodes.size();
    if (n == 0)
        return {};

    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    Map<ObjectId, std::size_t> id_to_idx;
    id_to_idx.reserve(listed->nodes().size());
    for (std::size_t i = 0; i < n; ++i)
        if (open.document.nodes[i].node.registered())
            id_to_idx.try_emplace(open.document.nodes[i].node.id(), i);

    Array<Array<std::size_t>> parents;
    parents.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!open.document.nodes[i].node.registered())
            continue;
        ATELIER_TRY_BIND(into, procedural::inputs(runtime_, *listed, open.document.nodes[i].node.id()));
        for (const auto& edge : *into) {
            if (const auto* index = id_to_idx.find(edge.from); index && *index != i)
                parents[i].add(*index);
        }
    }

    Array<std::size_t> rank;
    rank.resize(n, 0);
    for (std::size_t pass = 0; pass < n; ++pass) {
        bool changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            for (const std::size_t p : parents[i]) {
                if (rank[i] < rank[p] + 1 && rank[p] + 1 < n) {
                    rank[i] = rank[p] + 1;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }

    const std::size_t max_rank = *std::ranges::max_element(rank);
    Array<Array<std::size_t>> layers;
    layers.resize(max_rank + 1);
    for (std::size_t i = 0; i < n; ++i)
        layers[rank[i]].add(i);

    Array<float> order_x;
    order_x.resize(n, 0.f);
    constexpr float x_pitch = 196.f;
    constexpr float y_pitch = 96.f;
    Array<std::pair<String, std::array<float, 2>>> moves;
    moves.reserve(n);

    for (std::size_t r = 0; r <= max_rank; ++r) {
        auto& layer = layers[r];
        if (r > 0) {
            std::ranges::sort(layer, [&](std::size_t a, std::size_t b) {
                const auto bary = [&](std::size_t idx) {
                    if (parents[idx].empty())
                        return open.document.nodes[idx].position[0];
                    float sum = 0.f;
                    for (const std::size_t p : parents[idx])
                        sum += order_x[p];
                    return sum / static_cast<float>(parents[idx].size());
                };
                const float ba = bary(a);
                const float bb = bary(b);
                if (std::abs(ba - bb) > 1e-3f)
                    return ba < bb;
                if (open.document.nodes[a].position[0] != open.document.nodes[b].position[0])
                    return open.document.nodes[a].position[0] < open.document.nodes[b].position[0];
                return open.document.nodes[a].name < open.document.nodes[b].name;
            });
        } else {
            std::ranges::sort(layer, [&](std::size_t a, std::size_t b) {
                if (open.document.nodes[a].position[0] != open.document.nodes[b].position[0])
                    return open.document.nodes[a].position[0] < open.document.nodes[b].position[0];
                return open.document.nodes[a].name < open.document.nodes[b].name;
            });
        }
        const float half = static_cast<float>(layer.size() - 1) * .5f;
        for (std::size_t pos = 0; pos < layer.size(); ++pos) {
            const std::size_t idx = layer[pos];
            const float x = (static_cast<float>(pos) - half) * x_pitch;
            const float y = static_cast<float>(r) * y_pitch;
            order_x[idx] = x;
            moves.emplace(open.document.nodes[idx].name, std::array{x, y});
        }
    }
    return graph_move_nodes(key, moves);
}

Result<void> Session::graph_connect(StringView key, StringView from_node, StringView from_port,
                                    StringView to_node, StringView to_port) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const auto* src = find_node(open.document, from_node);
    auto* dst = find_node(open.document, to_node);
    if (!src || !src->node.registered())
        return graph_error(String::concat("Upstream node '", lossy_utf8(from_node), "' is missing or unresolved."));
    if (!dst || !dst->node.registered())
        return graph_error(String::concat("Downstream node '", lossy_utf8(to_node), "' is missing or unresolved."));

    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, src->node.id()));
    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, dst->node.id()));
    if (src_ports->outputs.empty())
        return graph_error(String::concat("Node '", src->name, "' has no output ports."));
    if (dst_ports->inputs.empty())
        return graph_error(String::concat("Node '", dst->name, "' has no input ports."));

    std::uint32_t out_idx = 0;
    if (!from_port.empty()) {
        const auto found = std::ranges::find(src_ports->outputs, from_port);
        if (found == src_ports->outputs.end())
            return graph_error(
                String::concat("Node '", src->name, "' has no output port '", lossy_utf8(from_port), "'."));
        out_idx = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
    }
    std::uint32_t in_idx = 0;
    if (!to_port.empty()) {
        const auto found = std::ranges::find(dst_ports->inputs, to_port);
        if (found == dst_ports->inputs.end())
            return graph_error(String::concat("Node '", dst->name, "' has no input port '", lossy_utf8(to_port), "'."));
        in_idx = static_cast<std::uint32_t>(found - dst_ports->inputs.begin());
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(before_inputs, procedural::inputs(runtime_, *listed, dst->node.id()));

    // On multi-input ports, avoid duplicate identical edges.
    if (std::ranges::any_of(*before_inputs, [&](const procedural::Input& in) {
            return in.from == src->node.id() && in.output == out_idx && in.input == in_idx;
        }))
        return {};

    ATELIER_TRY(procedural::connect(runtime_, graph, src->node.id(), out_idx, dst->node.id(), in_idx));
    ATELIER_TRY_BIND(after_inputs, procedural::inputs(runtime_, *listed, dst->node.id()));
    const auto before_kept = dst->kept;
    if (const auto inputs = *dst->kept.root().find("inputs"); inputs && *inputs->find(dst_ports->inputs[in_idx])) {
        ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, *after_inputs, in_idx));
        append_kept_port_endpoint(dst->kept, dst_ports->inputs[in_idx], dst_ports->multiple[in_idx],
                                  String::concat(src->name, ".", src_ports->outputs[out_idx]), *live_after);
    }

    open.push_change(GraphConnectionChange{
        {{dst->node, std::move(*before_inputs), std::move(*after_inputs), before_kept, dst->kept}}});
    ++revision_;
    return {};
}

Result<void> Session::graph_disconnect(StringView key, StringView to_node, StringView to_port,
                                       StringView from_node, StringView from_port) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto* dst = find_node(open.document, to_node);
    if (!dst || !dst->node.registered())
        return graph_error(String::concat("Downstream node '", lossy_utf8(to_node), "' is missing or unresolved."));

    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, dst->node.id()));
    Optional<std::uint32_t> in_idx;
    if (!to_port.empty()) {
        const auto found = std::ranges::find(dst_ports->inputs, to_port);
        if (found == dst_ports->inputs.end())
            return graph_error(String::concat("Node '", dst->name, "' has no input port '", lossy_utf8(to_port), "'."));
        in_idx = static_cast<std::uint32_t>(found - dst_ports->inputs.begin());
    }

    const auto kept_endpoint_matches = [&](StringView ep) noexcept {
        if (from_node.empty())
            return true;
        const auto dot = ep.find('.');
        if (dot == StringView::npos || ep.slice(0, dot) != from_node)
            return false;
        return from_port.empty() || ep.slice(dot + 1) == from_port;
    };

    Optional<ObjectId> filter_from;
    Optional<std::uint32_t> filter_out;
    bool matches_kept_only = false;
    if (!from_node.empty()) {
        const auto* src = find_node(open.document, from_node);
        if (src && src->node.registered()) {
            filter_from = src->node.id();
            if (!from_port.empty()) {
                ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, src->node.id()));
                const auto found = std::ranges::find(src_ports->outputs, from_port);
                if (found == src_ports->outputs.end()) {
                    if (!kept_port_has_match(dst->kept, to_port, kept_endpoint_matches))
                        return graph_error(String::concat("Node '", src->name, "' has no output port '",
                                                          lossy_utf8(from_port), "'."));
                    matches_kept_only = true;
                } else {
                    filter_out = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
                }
            }
        } else if (kept_port_has_match(dst->kept, to_port, kept_endpoint_matches)) {
            matches_kept_only = true;
        } else {
            return graph_error(String::concat("Upstream node '", lossy_utf8(from_node), "' is missing or unresolved."));
        }
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(before_inputs, procedural::inputs(runtime_, *listed, dst->node.id()));

    Array<procedural::Input> remaining;
    if (matches_kept_only) {
        remaining = *before_inputs;
    } else {
        for (const auto& item : *before_inputs) {
            const bool match_in = !in_idx || item.input == *in_idx;
            const bool match_from = !filter_from || item.from == *filter_from;
            const bool match_out = !filter_out || item.output == *filter_out;
            if (!(match_in && match_from && match_out))
                remaining.add(item);
        }
    }
    const bool live_changed = remaining.size() != before_inputs->size();
    if (live_changed)
        ATELIER_TRY(replace_node_inputs(runtime_, graph, dst->node.id(), remaining));

    auto before_kept = dst->kept;
    if (dst->kept.root().find("inputs")->has_value()) {
        if (in_idx) {
            ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, remaining, *in_idx));
            filter_kept_port_endpoints(dst->kept, dst_ports->inputs[*in_idx], dst_ports->multiple[*in_idx], *live_after,
                                       kept_endpoint_matches);
        } else {
            const auto cur_inputs = *dst->kept.root().find("inputs");
            if (cur_inputs && cur_inputs->kind() == json::Kind::object) {
                Array<String> kept_ports;
                for (const auto& [pname, _] : *cur_inputs->object_items())
                    kept_ports.add(String::from_utf8_validated(pname));
                for (const auto& pname : kept_ports) {
                    const auto p_it = std::ranges::find(dst_ports->inputs, pname);
                    if (p_it != dst_ports->inputs.end()) {
                        const auto p_idx = static_cast<std::uint32_t>(p_it - dst_ports->inputs.begin());
                        ATELIER_TRY_BIND(live_after,
                                         format_live_port_endpoints(runtime_, open.document, remaining, p_idx));
                        filter_kept_port_endpoints(dst->kept, pname, dst_ports->multiple[p_idx], *live_after,
                                                   kept_endpoint_matches);
                    } else {
                        filter_kept_port_endpoints(dst->kept, pname, false, {}, kept_endpoint_matches);
                    }
                }
            }
        }
    }

    if (!live_changed && dst->kept == before_kept)
        return {};

    open.push_change(GraphConnectionChange{
        {{dst->node, std::move(*before_inputs), std::move(remaining), std::move(before_kept), dst->kept}}});
    ++revision_;
    return {};
}

Result<void> Session::graph_disconnect_edges(StringView key, Span<const GraphConnectionView> edges) {
    if (edges.empty())
        return {};
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));

    struct ResolvedCut {
        ObjectId from{};

        std::uint32_t out_idx{};

        ObjectId to{};

        std::uint32_t in_idx{};

        String endpoint;
    };
    Array<ResolvedCut> cuts;
    cuts.reserve(edges.size());
    for (const auto& edge : edges) {
        const auto* src = find_node(open.document, edge.from_node);
        const auto* dst = find_node(open.document, edge.to_node);
        if (!src || !src->node.registered() || !dst || !dst->node.registered())
            continue;
        ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, src->node.id()));
        ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, dst->node.id()));
        const auto out_it = std::ranges::find(src_ports->outputs, edge.from_port);
        const auto in_it = std::ranges::find(dst_ports->inputs, edge.to_port);
        if (out_it == src_ports->outputs.end() || in_it == dst_ports->inputs.end())
            continue;
        cuts.add({src->node.id(), static_cast<std::uint32_t>(out_it - src_ports->outputs.begin()), dst->node.id(),
                  static_cast<std::uint32_t>(in_it - dst_ports->inputs.begin()),
                  String::concat(src->name, ".", *out_it)});
    }
    if (cuts.empty())
        return {};

    GraphConnectionChange change;
    for (auto& dst : open.document.nodes) {
        if (!dst.node.registered())
            continue;
        const ObjectId to_id = dst.node.id();
        if (std::ranges::none_of(cuts, [&](const ResolvedCut& c) { return c.to == to_id; }))
            continue;
        ATELIER_TRY_BIND(before_inputs, procedural::inputs(runtime_, *listed, to_id));
        Array<procedural::Input> remaining;
        for (const auto& in : *before_inputs) {
            const bool cut = std::ranges::any_of(cuts, [&](const ResolvedCut& c) {
                return c.to == to_id && c.from == in.from && c.out_idx == in.output && c.in_idx == in.input;
            });
            if (!cut)
                remaining.add(in);
        }
        const bool live_changed = remaining.size() != before_inputs->size();
        if (live_changed)
            ATELIER_TRY(replace_node_inputs(runtime_, graph, to_id, remaining));

        auto before_kept = dst.kept;
        if (dst.kept.root().find("inputs")->has_value()) {
            ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, to_id));
            for (std::uint32_t p_idx = 0; p_idx < dst_ports->inputs.size(); ++p_idx) {
                if (std::ranges::none_of(cuts,
                                         [&](const ResolvedCut& c) { return c.to == to_id && c.in_idx == p_idx; }))
                    continue;
                ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, remaining, p_idx));
                filter_kept_port_endpoints(dst.kept, dst_ports->inputs[p_idx], dst_ports->multiple[p_idx], *live_after,
                                           [&](const String& ep) {
                                               return std::ranges::any_of(cuts, [&](const ResolvedCut& c) {
                                                   return c.to == to_id && c.in_idx == p_idx && c.endpoint == ep;
                                               });
                                           });
            }
        }
        if (!live_changed && dst.kept == before_kept)
            continue;
        change.targets.add(
            {dst.node, std::move(*before_inputs), std::move(remaining), std::move(before_kept), dst.kept});
    }
    if (!change.targets.empty()) {
        open.push_change(std::move(change));
        ++revision_;
    }
    return {};
}

Result<void> Session::graph_rewire(StringView key, const GraphConnectionView& remove_edge,
                                   StringView new_to_node, StringView new_to_port) {
    if (remove_edge.to_node == new_to_node && remove_edge.to_port == new_to_port)
        return {};
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const auto* src = find_node(open.document, remove_edge.from_node);
    auto* old_dst = find_node(open.document, remove_edge.to_node);
    auto* new_dst = find_node(open.document, new_to_node);
    if (!src || !src->node.registered() || !old_dst || !old_dst->node.registered() || !new_dst ||
        !new_dst->node.registered())
        return graph_error("Rewire nodes must exist and be resolved.");

    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, src->node.id()));
    ATELIER_TRY_BIND(old_ports, procedural::ports(runtime_, old_dst->node.id()));
    ATELIER_TRY_BIND(new_ports, procedural::ports(runtime_, new_dst->node.id()));
    const auto out_it = std::ranges::find(src_ports->outputs, remove_edge.from_port);
    const auto old_in_it = std::ranges::find(old_ports->inputs, remove_edge.to_port);
    const auto new_in_it = std::ranges::find(new_ports->inputs, new_to_port);
    if (out_it == src_ports->outputs.end() || old_in_it == old_ports->inputs.end() ||
        new_in_it == new_ports->inputs.end())
        return graph_error("Rewire port does not exist on node.");
    const auto out_idx = static_cast<std::uint32_t>(out_it - src_ports->outputs.begin());
    const auto old_in_idx = static_cast<std::uint32_t>(old_in_it - old_ports->inputs.begin());
    const auto new_in_idx = static_cast<std::uint32_t>(new_in_it - new_ports->inputs.begin());

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(old_before, procedural::inputs(runtime_, *listed, old_dst->node.id()));
    Array<procedural::Input> new_before_vec;
    if (new_dst != old_dst) {
        ATELIER_TRY_BIND(nb, procedural::inputs(runtime_, *listed, new_dst->node.id()));
        new_before_vec = std::move(*nb);
    }

    ATELIER_TRY(procedural::disconnect(runtime_, src->node.id(), out_idx, old_dst->node.id(), old_in_idx));
    const bool duplicate_on_new = [&] {
        const auto check_list = procedural::inputs(runtime_, *listed, new_dst->node.id());
        return check_list && std::ranges::any_of(*check_list, [&](const procedural::Input& in) {
                   return in.from == src->node.id() && in.output == out_idx && in.input == new_in_idx;
               });
    }();
    if (!duplicate_on_new) {
        if (auto connected =
                procedural::connect(runtime_, graph, src->node.id(), out_idx, new_dst->node.id(), new_in_idx);
            !connected) {
            static_cast<void>(replace_node_inputs(runtime_, graph, old_dst->node.id(), *old_before));
            return std::unexpected(connected.error());
        }
    }

    const auto old_before_kept = old_dst->kept;
    const auto new_before_kept = new_dst->kept;
    const String moved_ep = String::concat(src->name, ".", src_ports->outputs[out_idx]);

    ATELIER_TRY_BIND(old_after, procedural::inputs(runtime_, *listed, old_dst->node.id()));
    ATELIER_TRY_BIND(old_live_after, format_live_port_endpoints(runtime_, open.document, *old_after, old_in_idx));
    filter_kept_port_endpoints(old_dst->kept, old_ports->inputs[old_in_idx], old_ports->multiple[old_in_idx],
                               *old_live_after, [&](const String& ep) { return ep == moved_ep; });

    GraphConnectionChange change;
    if (old_dst == new_dst) {
        ATELIER_TRY_BIND(same_new_live, format_live_port_endpoints(runtime_, open.document, *old_after, new_in_idx));
        if (!duplicate_on_new)
            append_kept_port_endpoint(new_dst->kept, new_ports->inputs[new_in_idx], new_ports->multiple[new_in_idx],
                                      moved_ep, *same_new_live);
        change.targets.add(
            {old_dst->node, std::move(*old_before), std::move(*old_after), old_before_kept, new_dst->kept});
    } else {
        ATELIER_TRY_BIND(new_after, procedural::inputs(runtime_, *listed, new_dst->node.id()));
        ATELIER_TRY_BIND(new_live_after, format_live_port_endpoints(runtime_, open.document, *new_after, new_in_idx));
        if (!duplicate_on_new)
            append_kept_port_endpoint(new_dst->kept, new_ports->inputs[new_in_idx], new_ports->multiple[new_in_idx],
                                      moved_ep, *new_live_after);
        change.targets.add(
            {old_dst->node, std::move(*old_before), std::move(*old_after), old_before_kept, old_dst->kept});
        change.targets.add(
            {new_dst->node, std::move(new_before_vec), std::move(*new_after), new_before_kept, new_dst->kept});
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_splice_node(StringView key, StringView node_name, std::array<float, 2> position,
                                        const GraphConnectionView& wire) {
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
        return graph_error("Node position must be finite.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto* mid = find_node(open.document, node_name);
    const auto* src = find_node(open.document, wire.from_node);
    auto* dst = find_node(open.document, wire.to_node);
    if (!mid || !mid->node.registered() || !src || !src->node.registered() || !dst || !dst->node.registered())
        return graph_error("Splice nodes must exist and be resolved.");
    if (mid == src || mid == dst)
        return graph_error("Cannot splice a wire endpoint onto its own wire.");

    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, src->node.id()));
    ATELIER_TRY_BIND(mid_ports, procedural::ports(runtime_, mid->node.id()));
    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, dst->node.id()));
    if (mid_ports->inputs.empty() || mid_ports->outputs.empty())
        return graph_error("Splice node requires at least one input port and one output port.");
    const auto out_it = std::ranges::find(src_ports->outputs, wire.from_port);
    const auto in_it = std::ranges::find(dst_ports->inputs, wire.to_port);
    if (out_it == src_ports->outputs.end() || in_it == dst_ports->inputs.end())
        return graph_error("Splice target wire names a missing port.");
    const auto out_idx = static_cast<std::uint32_t>(out_it - src_ports->outputs.begin());
    const auto in_idx = static_cast<std::uint32_t>(in_it - dst_ports->inputs.begin());

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(mid_before, procedural::inputs(runtime_, *listed, mid->node.id()));
    ATELIER_TRY_BIND(dst_before, procedural::inputs(runtime_, *listed, dst->node.id()));
    const bool wire_exists = std::ranges::any_of(*dst_before, [&](const procedural::Input& in) {
        return in.from == src->node.id() && in.output == out_idx && in.input == in_idx;
    });
    if (!wire_exists)
        return graph_error("Target wire no longer exists.");

    Array<procedural::Input> mid_after;
    for (const auto& in : *mid_before) {
        if (mid_ports->multiple[0] || in.input != 0)
            mid_after.add(in);
    }
    const bool mid_has_source = std::ranges::any_of(mid_after, [&](const procedural::Input& in) {
        return in.from == src->node.id() && in.output == out_idx && in.input == 0;
    });
    if (!mid_has_source)
        mid_after.add({src->node.id(), out_idx, 0});

    Array<procedural::Input> dst_after;
    bool inserted = false;
    const bool already_has_mid = std::ranges::any_of(*dst_before, [&](const procedural::Input& in) {
        return in.from == mid->node.id() && in.output == 0 && in.input == in_idx;
    });
    for (const auto& in : *dst_before) {
        if (in.from == src->node.id() && in.output == out_idx && in.input == in_idx) {
            if (!already_has_mid && !inserted) {
                dst_after.add({mid->node.id(), 0, in_idx});
                inserted = true;
            }
            continue;
        }
        dst_after.add(in);
    }

    static_cast<void>(replace_node_inputs(runtime_, graph, dst->node.id(), {}));
    static_cast<void>(replace_node_inputs(runtime_, graph, mid->node.id(), {}));
    if (auto r1 = replace_node_inputs(runtime_, graph, mid->node.id(), mid_after); !r1) {
        static_cast<void>(replace_node_inputs(runtime_, graph, mid->node.id(), {}));
        static_cast<void>(replace_node_inputs(runtime_, graph, mid->node.id(), *mid_before));
        static_cast<void>(replace_node_inputs(runtime_, graph, dst->node.id(), *dst_before));
        return std::unexpected(r1.error());
    }
    if (auto r2 = replace_node_inputs(runtime_, graph, dst->node.id(), dst_after); !r2) {
        static_cast<void>(replace_node_inputs(runtime_, graph, dst->node.id(), {}));
        static_cast<void>(replace_node_inputs(runtime_, graph, mid->node.id(), {}));
        static_cast<void>(replace_node_inputs(runtime_, graph, mid->node.id(), *mid_before));
        static_cast<void>(replace_node_inputs(runtime_, graph, dst->node.id(), *dst_before));
        return std::unexpected(r2.error());
    }

    const String src_ep = String::concat(src->name, ".", src_ports->outputs[out_idx]);
    const String mid_ep = String::concat(mid->name, ".", mid_ports->outputs[0]);
    ATELIER_TRY_BIND(mid_live_after, format_live_port_endpoints(runtime_, open.document, mid_after, 0));
    ATELIER_TRY_BIND(dst_live_after, format_live_port_endpoints(runtime_, open.document, dst_after, in_idx));

    const auto mid_before_kept = mid->kept;
    if (!mid_has_source)
        append_kept_port_endpoint(mid->kept, mid_ports->inputs[0], mid_ports->multiple[0], src_ep, *mid_live_after);
    const auto dst_before_kept = dst->kept;
    replace_kept_port_endpoint(dst->kept, dst_ports->inputs[in_idx], dst_ports->multiple[in_idx], src_ep, mid_ep,
                               already_has_mid, *dst_live_after);

    GraphConnectionChange change;
    change.targets.add({mid->node, std::move(*mid_before), std::move(mid_after), mid_before_kept, mid->kept});
    change.targets.add({dst->node, std::move(*dst_before), std::move(dst_after), dst_before_kept, dst->kept});
    if (mid->position != position) {
        change.moves.add({mid->name, mid->position, position});
        mid->position = position;
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_set_display(StringView key, StringView node_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    ObjectId target{};
    if (!node_name.empty()) {
        const auto* item = find_node(open.document, node_name);
        if (!item || !item->node.registered())
            return graph_error(String::concat("Display target '", lossy_utf8(node_name), "' is missing or unresolved."));
        target = item->node.id();
    }
    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    const auto before_kept = open.document.kept;
    json::Builder next_kept(open.document.kept);
    next_kept.erase(next_kept.root(), "display");
    const auto after_kept = next_kept.finish();
    if (listed->display() == target && !before_kept.root().find("display")->has_value())
        return {};
    ATELIER_TRY(procedural::set_display(runtime_, graph, target));
    open.document.kept = after_kept;
    open.push_change(GraphDisplayChange{listed->display(), target, before_kept, after_kept});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_template(StringView key, StringView node_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    ObjectId target{};
    if (!node_name.empty()) {
        const auto* item = find_node(open.document, node_name);
        if (!item || !item->node.registered())
            return graph_error(
                String::concat("Template target '", lossy_utf8(node_name), "' is missing or unresolved."));
        target = item->node.id();
    }
    ATELIER_TRY(cancel_edit());
    const auto before = open.document.template_node;
    const auto before_kept = open.document.kept;
    json::Builder next_kept(open.document.kept);
    next_kept.erase(next_kept.root(), "template");
    const auto after_kept = next_kept.finish();
    if (before == target && !before_kept.root().find("template")->has_value())
        return {};
    open.document.template_node = target;
    open.document.kept = after_kept;
    open.push_change(GraphTemplateChange{before, target, before_kept, after_kept});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_bypass(StringView key, StringView node_name, bool bypassed) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, node_name);
    if (!item || !item->node.registered())
        return graph_error(String::concat("Bypass target '", lossy_utf8(node_name), "' is missing or unresolved."));
    ATELIER_TRY_BIND(before, procedural::bypassed(runtime_, item->node.id()));
    if (*before == bypassed)
        return {};
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY(procedural::set_bypassed(runtime_, item->node.id(), bypassed));
    open.push_change(GraphBypassChange{item->node, *before, bypassed});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_frozen(StringView key, StringView node_name, bool frozen, Project* project) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, frozen));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, node_name);
    if (!item || !item->node.registered())
        return graph_error(String::concat("Freeze target '", lossy_utf8(node_name), "' is missing or unresolved."));
    ATELIER_TRY_BIND(before, procedural::frozen(runtime_, item->node.id()));
    if (*before == frozen)
        return {};
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(snapshot, procedural::frozen_output(runtime_, item->node.id()));
    if (frozen) {
        ATELIER_TRY(prepare_graph_evaluator(open, project));
        ATELIER_TRY_BIND(captured, open.evaluator->freeze_snapshot(item->node.id()));
        *snapshot = std::move(*captured);
    }
    ATELIER_TRY(procedural::set_frozen(runtime_, item->node.id(), frozen, frozen ? *snapshot : nullptr));
    open.push_change(GraphFreezeChange{item->node, std::move(*snapshot), frozen});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_node_appearance(StringView key, Span<const String> names,
                                                Optional<GraphNodeShape> shape,
                                                Optional<Optional<std::uint32_t>> color,
                                                Optional<String> comment) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    if (shape && static_cast<std::uint8_t>(*shape) > static_cast<std::uint8_t>(GraphNodeShape::circle))
        return graph_error("Invalid node shape preset.");
    auto& open = **open_ptr;
    GraphAppearanceChange change;
    Array<String> seen;
    for (const auto& name : names) {
        if (std::ranges::find(seen, name) != seen.end())
            continue;
        seen.add(name);
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error(String::concat("No node named '", name, "' in this graph."));
        const GraphNodeShape next_shape = shape.value_or(item->shape);
        Optional<std::uint32_t> next_color = item->color;
        if (color) {
            next_color = *color ? Optional<std::uint32_t>{**color & 0xffffffu} : Optional<std::uint32_t>{};
        }
        const String next_comment = comment.value_or(item->comment);
        if (next_shape != item->shape || next_color != item->color || next_comment != item->comment) {
            change.entries.add(
                {name, item->shape, next_shape, item->color, next_color, item->comment, next_comment});
        }
    }
    if (change.entries.empty())
        return {};
    ATELIER_TRY(cancel_edit());
    for (const auto& delta : change.entries) {
        auto* item = find_node(open.document, delta.name);
        item->shape = delta.after_shape;
        item->color = delta.after_color;
        item->comment = delta.after_comment;
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<String> Session::graph_add_note(StringView key, std::array<float, 2> position, std::array<float, 2> size,
                                       StringView title, StringView text, std::uint32_t color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(size[0]) ||
        !std::isfinite(size[1]) || size[0] <= 0.f || size[1] <= 0.f)
        return graph_error("Sticky note position and size must be finite and positive.");
    auto title_str = String::from_utf8(title);
    auto text_str = String::from_utf8(text);
    if (!title_str || !text_str)
        return graph_error("Sticky note text must be valid UTF-8.");
    auto& open = **open_ptr;
    String name;
    for (std::uint32_t idx = 1;; ++idx) {
        name = String::concat("note", String::from_number(idx));
        if (!find_note(open.document, name))
            break;
    }
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    change.after_notes.add({name, position, size, std::move(*title_str), std::move(*text_str), color & 0xffffffu});
    sort_graph_notes(change.after_notes);
    open.document.notes = change.after_notes;
    open.push_change(std::move(change));
    ++revision_;
    return name;
}

Result<void> Session::graph_update_note(StringView key, StringView name, Optional<std::array<float, 2>> position,
                                        Optional<std::array<float, 2>> size, Optional<String> title,
                                        Optional<String> text, Optional<std::uint32_t> color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    const auto* existing = find_note(open.document, name);
    if (!existing)
        return graph_error(String::concat("No sticky note named '", lossy_utf8(name), "' in this graph."));
    GraphStickyNote updated = *existing;
    if (position) {
        if (!std::isfinite((*position)[0]) || !std::isfinite((*position)[1]))
            return graph_error("Sticky note position must be finite.");
        updated.position = *position;
    }
    if (size) {
        if (!std::isfinite((*size)[0]) || !std::isfinite((*size)[1]) || (*size)[0] <= 0.f || (*size)[1] <= 0.f)
            return graph_error("Sticky note size must be finite and positive.");
        updated.size = *size;
    }
    if (title)
        updated.title = *title;
    if (text)
        updated.text = *text;
    if (color)
        updated.color = *color & 0xffffffu;
    if (updated == *existing)
        return {};
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    for (auto& item : change.after_notes) {
        if (item.name == name)
            item = updated;
    }
    open.document.notes = change.after_notes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_remove_notes(StringView key, Span<const String> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    if (change.after_notes.remove_if([&](const GraphStickyNote& item) {
            return std::ranges::find(names, item.name) != names.end();
        }) == 0)
        return {};
    ATELIER_TRY(cancel_edit());
    open.document.notes = change.after_notes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<String> Session::graph_add_box(StringView key, std::array<float, 2> position, std::array<float, 2> size,
                                      StringView title, std::uint32_t color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(size[0]) ||
        !std::isfinite(size[1]) || size[0] <= 0.f || size[1] <= 0.f)
        return graph_error("Network box position and size must be finite and positive.");
    auto title_str = String::from_utf8(title);
    if (!title_str)
        return graph_error("Network box title must be valid UTF-8.");
    auto& open = **open_ptr;
    String name;
    for (std::uint32_t idx = 1;; ++idx) {
        name = String::concat("box_frame", String::from_number(idx));
        if (!find_box(open.document, name))
            break;
    }
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    change.after_boxes.add({name, position, size, std::move(*title_str), color & 0xffffffu});
    sort_graph_boxes(change.after_boxes);
    open.document.boxes = change.after_boxes;
    open.push_change(std::move(change));
    ++revision_;
    return name;
}

Result<void> Session::graph_update_box(StringView key, StringView name, Optional<std::array<float, 2>> position,
                                       Optional<std::array<float, 2>> size, Optional<String> title,
                                       Optional<std::uint32_t> color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    const auto* existing = find_box(open.document, name);
    if (!existing)
        return graph_error(String::concat("No network box named '", lossy_utf8(name), "' in this graph."));
    GraphNetworkBox updated = *existing;
    if (position) {
        if (!std::isfinite((*position)[0]) || !std::isfinite((*position)[1]))
            return graph_error("Network box position must be finite.");
        updated.position = *position;
    }
    if (size) {
        if (!std::isfinite((*size)[0]) || !std::isfinite((*size)[1]) || (*size)[0] <= 0.f || (*size)[1] <= 0.f)
            return graph_error("Network box size must be finite and positive.");
        updated.size = *size;
    }
    if (title)
        updated.title = *title;
    if (color)
        updated.color = *color & 0xffffffu;
    if (updated == *existing)
        return {};
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    for (auto& item : change.after_boxes) {
        if (item.name == name)
            item = updated;
    }
    open.document.boxes = change.after_boxes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_remove_boxes(StringView key, Span<const String> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    if (change.after_boxes.remove_if([&](const GraphNetworkBox& item) {
            return std::ranges::find(names, item.name) != names.end();
        }) == 0)
        return {};
    ATELIER_TRY(cancel_edit());
    open.document.boxes = change.after_boxes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

namespace {

bool same_scalar_bits(const Scalar& a, const Scalar& b) noexcept {
    if (a.index() != b.index())
        return false;
    return std::visit(
        [&](const auto& lhs) noexcept -> bool {
            using T = std::decay_t<decltype(lhs)>;
            const auto& rhs = std::get<T>(b);
            if constexpr (std::same_as<T, float>)
                return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs);
            else if constexpr (std::same_as<T, double>)
                return std::bit_cast<std::uint64_t>(lhs) == std::bit_cast<std::uint64_t>(rhs);
            else
                return lhs == rhs;
        },
        a);
}

Result<Scalar> decode_scalar_or_enum_json(Runtime& runtime, TypeId leaf_type, const FieldRange& range, json::Value val,
                                          bool tolerant_clamp, bool* was_clamped = nullptr) {
    if (was_clamped)
        *was_clamped = false;
    ATELIER_TRY_BIND(info, runtime.describe(leaf_type));
    if ((*info)->kind == TypeKind::enumeration) {
        if (!(*info)->enumeration)
            return graph_error("Missing enumeration metadata.");
        const auto& enumeration = *(*info)->enumeration;
        if (val.kind() == json::Kind::string) {
            const auto name = *val.string();
            const auto en = std::ranges::find(enumeration.values, name, &std::pair<String, std::uint32_t>::first);
            if (en == enumeration.values.end())
                return graph_error(String::concat("Unknown enumerator '", String::validated(name), "'."));
            return Scalar{EnumValue{leaf_type, en->second}};
        }
        if (enumeration.flags) {
            std::int64_t iv = -1;
            if (const auto exact_i = val.i64()) {
                iv = *exact_i;
            } else if (const auto num = val.number()) {
                if (std::isfinite(*num) && std::trunc(*num) == *num && *num >= 0.0 && *num <= UINT32_MAX)
                    iv = static_cast<std::int64_t>(*num);
            }
            if (iv >= 0 && iv <= UINT32_MAX && detail::valid_enum_bits(enumeration, static_cast<std::uint32_t>(iv)))
                return Scalar{EnumValue{leaf_type, static_cast<std::uint32_t>(iv)}};
        }
        return graph_error("Expected enumeration identifier string.");
    }
    if (leaf_type == runtime.native_type<bool>().value_or(TypeId{})) {
        if (val.kind() != json::Kind::boolean)
            return graph_error("Expected boolean value.");
        return Scalar{*val.boolean()};
    }
    if (leaf_type == runtime.native_type<float>().value_or(TypeId{})) {
        auto num = val.number();
        if (!num || !std::isfinite(*num))
            return graph_error("Expected finite float value.");
        const float f = static_cast<float>(*num);
        if (!std::isfinite(f))
            return graph_error("Float value overflowed.");
        auto [clamped_v, did_clamp] = detail::clamp_scalar(Scalar{f}, range);
        if (did_clamp && !tolerant_clamp)
            return graph_error("Value is outside promoted parameter hard bounds.");
        if (was_clamped)
            *was_clamped = did_clamp;
        return clamped_v;
    }
    if (leaf_type == runtime.native_type<double>().value_or(TypeId{})) {
        auto num = val.number();
        if (!num || !std::isfinite(*num))
            return graph_error("Expected finite double value.");
        auto [clamped_v, did_clamp] = detail::clamp_scalar(Scalar{*num}, range);
        if (did_clamp && !tolerant_clamp)
            return graph_error("Value is outside promoted parameter hard bounds.");
        if (was_clamped)
            *was_clamped = did_clamp;
        return clamped_v;
    }
    const bool is_i32 = leaf_type == runtime.native_type<std::int32_t>().value_or(TypeId{});
    const bool is_u32 = leaf_type == runtime.native_type<std::uint32_t>().value_or(TypeId{});
    if (is_i32 || is_u32) {
        std::int64_t iv = 0;
        if (const auto exact_i = val.i64()) {
            iv = *exact_i;
        } else if (const auto num = val.number()) {
            if (!std::isfinite(*num) || std::trunc(*num) != *num || *num < -9007199254740992.0 ||
                *num > 9007199254740992.0)
                return graph_error("Expected exact integer value.");
            iv = static_cast<std::int64_t>(*num);
        } else {
            return graph_error("Expected integer value.");
        }
        bool type_clamped = false;
        Scalar candidate;
        if (is_i32) {
            if (iv < INT32_MIN || iv > INT32_MAX) {
                if (!tolerant_clamp)
                    return graph_error("Integer value is outside int32 bounds.");
                iv = std::clamp<std::int64_t>(iv, INT32_MIN, INT32_MAX);
                type_clamped = true;
            }
            candidate = static_cast<std::int32_t>(iv);
        } else {
            if (iv < 0 || iv > UINT32_MAX) {
                if (!tolerant_clamp)
                    return graph_error("Integer value is outside uint32 bounds.");
                iv = std::clamp<std::int64_t>(iv, 0, UINT32_MAX);
                type_clamped = true;
            }
            candidate = static_cast<std::uint32_t>(iv);
        }
        auto [clamped_v, range_clamped] = detail::clamp_scalar(candidate, range);
        if ((type_clamped || range_clamped) && !tolerant_clamp)
            return graph_error("Integer value is outside promoted parameter hard bounds.");
        if (was_clamped)
            *was_clamped = type_clamped || range_clamped;
        return clamped_v;
    }
    return graph_error("Unsupported promoted parameter scalar type.");
}

Result<json::Document> encode_parameter_scalars_json(Runtime& runtime, bool is_vector,
                                                     Span<const detail::ScalarField> leaves,
                                                     Span<const Scalar> scalars) {
    json::Builder out;
    const auto encode_one = [&](json::Value dst, const Scalar& s) -> Result<void> {
        return std::visit(
            [&](const auto& v) -> Result<void> {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::same_as<T, bool>) {
                    out.set(dst, v);
                } else if constexpr (std::same_as<T, std::int32_t>) {
                    out.set(dst, static_cast<std::int64_t>(v));
                } else if constexpr (std::same_as<T, std::uint32_t>) {
                    out.set(dst, static_cast<std::uint64_t>(v));
                } else if constexpr (std::same_as<T, float> || std::same_as<T, double>) {
                    if (auto r = out.set(dst, v); !r)
                        return graph_error(r.error().describe());
                } else if constexpr (std::same_as<T, EnumValue>) {
                    ATELIER_TRY_BIND(info, runtime.describe(v.type));
                    if (!(*info)->enumeration)
                        return graph_error("Missing enumeration metadata.");
                    const auto& enumeration = *(*info)->enumeration;
                    const auto en =
                        std::ranges::find(enumeration.values, v.bits, &std::pair<String, std::uint32_t>::second);
                    if (en != enumeration.values.end()) {
                        out.set_validated(dst, en->first);
                    } else if (enumeration.flags && detail::valid_enum_bits(enumeration, v.bits)) {
                        out.set(dst, static_cast<std::uint64_t>(v.bits));
                    } else {
                        return graph_error("Invalid enumerator bits.");
                    }
                }
                return {};
            },
            s);
    };
    if (!is_vector) {
        ATELIER_TRY(encode_one(out.root(), scalars.front()));
        return out.finish();
    }
    for (std::size_t i = 0; i < leaves.size() && i < scalars.size(); ++i) {
        const auto& lane_name = leaves[i].names.back();
        const auto slot = out.object_validated(out.root(), lane_name);
        ATELIER_TRY(encode_one(slot, scalars[i]));
    }
    return out.finish();
}

Result<std::variant<Array<Scalar>, String>>
decode_promoted_value_json(Runtime& runtime, const ResolvedPromotedSchema& schema,
                           Span<const Scalar> fallback_scalars, json::Value val, bool tolerant_clamp,
                           bool* was_clamped = nullptr) {
    if (was_clamped)
        *was_clamped = false;
    if (!schema.primary.is_vector && schema.primary.leaves.front().kind == TypeKind::string) {
        if (val.kind() != json::Kind::string)
            return graph_error("Expected string value for parameter.");
        return String::from_utf8_validated(*val.string());
    }
    if (!schema.primary.is_vector) {
        ATELIER_TRY_BIND(s, decode_scalar_or_enum_json(runtime, schema.primary.leaves.front().type,
                                                       schema.ranges.front(), val, tolerant_clamp, was_clamped));
        return Array<Scalar>{*s};
    }
    Array<Scalar> lanes(fallback_scalars);
    lanes.resize(schema.primary.leaves.size(), Scalar{0.f});
    bool any_clamped = false;
    if (val.kind() == json::Kind::array) {
        const auto arr = *val.array();
        if (arr.size() != schema.primary.leaves.size())
            return graph_error("Vector override array length does not match component count.");
        std::size_t idx = 0;
        for (const auto elem : arr) {
            bool lane_clamped = false;
            ATELIER_TRY_BIND(s, decode_scalar_or_enum_json(runtime, schema.primary.leaves[idx].type, schema.ranges[idx],
                                                           elem, tolerant_clamp, &lane_clamped));
            lanes[idx++] = *s;
            any_clamped |= lane_clamped;
        }
    } else if (val.kind() == json::Kind::object) {
        for (const auto& [k, elem] : *val.object_items()) {
            const auto found = std::ranges::find_if(schema.primary.leaves,
                                                    [&](const detail::ScalarField& f) { return f.names.back() == k; });
            if (found == schema.primary.leaves.end())
                return graph_error(String::concat("Unknown vector component '", String::validated(k), "'."));
            const auto idx = static_cast<std::size_t>(found - schema.primary.leaves.begin());
            bool lane_clamped = false;
            ATELIER_TRY_BIND(s, decode_scalar_or_enum_json(runtime, found->type, schema.ranges[idx], elem,
                                                           tolerant_clamp, &lane_clamped));
            lanes[idx] = *s;
            any_clamped |= lane_clamped;
        }
    } else {
        return graph_error("Expected vector object or array value.");
    }
    if (was_clamped)
        *was_clamped = any_clamped;
    return lanes;
}

Result<void> write_promoted_targets_if_changed(Runtime& runtime, const ResolvedPromotedSchema& schema,
                                               const std::variant<Array<Scalar>, String>& desired,
                                               GraphDocument* clear_kept_doc = nullptr) {
    for (const auto& target : schema.all_targets) {
        if (const auto* str = std::get_if<String>(&desired)) {
            const auto& leaf = target.leaves.front();
            String cur;
            if (!runtime.read(target.node, leaf.path, ValueOutput{leaf.type, &cur}) || cur != *str) {
                ATELIER_TRY(runtime.write(target.node, leaf.path, ValueInput{leaf.type, str}));
            }
        } else {
            const auto& scalars = std::get<Array<Scalar>>(desired);
            for (std::size_t li = 0; li < target.leaves.size() && li < scalars.size(); ++li) {
                auto cur = runtime.read(target.node, target.leaves[li].path);
                if (!cur || !same_scalar_bits(*cur, scalars[li]))
                    ATELIER_TRY(runtime.write(target.node, target.leaves[li].path, scalars[li]));
            }
        }
        if (clear_kept_doc) {
            if (auto* gnode = find_node_by_id(*clear_kept_doc, target.node); gnode && gnode->kept.root().size() > 0) {
                json::Builder next(gnode->kept);
                bool erased = false;
                for (const auto& leaf : target.leaves) {
                    auto names = leaf.names;
                    names.insert(0, "properties"_s);
                    erased |= detail::erase_path(next, next.root(), names);
                }
                if (erased)
                    gnode->kept = next.finish();
            }
        }
    }
    return {};
}

Array<CachedPromotedParam> build_cached_promoted_params(Runtime& runtime, const GraphDocument& doc) {
    Array<CachedPromotedParam> out;
    out.reserve(doc.parameters.size());
    for (const auto& param : doc.parameters) {
        auto schema = resolve_promoted_parameter_schema(runtime, doc, param);
        if (!schema)
            continue;
        std::variant<Array<Scalar>, String> def_val;
        if (!schema->primary.is_vector && schema->primary.leaves.front().kind == TypeKind::string) {
            String s;
            if (runtime.read(schema->primary.node, schema->primary.leaves.front().path,
                             ValueOutput{schema->primary.leaves.front().type, &s}))
                def_val = std::move(s);
            else
                def_val = String{};
        } else {
            Array<Scalar> vals;
            vals.reserve(schema->primary.leaves.size());
            for (const auto& leaf : schema->primary.leaves) {
                Scalar s{0.f};
                if (auto rs = runtime.read(schema->primary.node, leaf.path))
                    s = *rs;
                vals.add(s);
            }
            def_val = std::move(vals);
        }
        out.add({param.name, std::move(*schema), std::move(def_val)});
    }
    return out;
}

template <class WarnFn>
Result<void> apply_cached_instance_overrides(Runtime& runtime, Span<const CachedPromotedParam> cached_params,
                                             const String& asset_key, const json::Document& overrides,
                                             WarnFn&& report_warn) {
    const json::Value ov_root = overrides.root();
    const bool has_ov = ov_root.kind() == json::Kind::object && ov_root.size() > 0;
    if (has_ov) {
        for (const auto& [ov_name_sv, ov_val] : *ov_root.object_items()) {
            if (std::ranges::find(cached_params, ov_name_sv, &CachedPromotedParam::name) == cached_params.end()) {
                const String ov_name = String::from_utf8_validated(ov_name_sv);
                report_warn(ov_name, ov_val, world::DiagnosticKind::unknown_field,
                            String::concat("Asset '", asset_key, "' has no promoted parameter '", ov_name,
                                           "'. The override is kept."));
            }
        }
    }
    for (const auto& cp : cached_params) {
        std::variant<Array<Scalar>, String> desired = cp.default_value;
        if (has_ov) {
            if (const auto ov_val = *ov_root.find(cp.name)) {
                const auto* def_scalars = std::get_if<Array<Scalar>>(&cp.default_value);
                const Span<const Scalar> fallback =
                    def_scalars ? Span<const Scalar>(*def_scalars) : Span<const Scalar>{};
                bool was_clamped = false;
                auto decoded = decode_promoted_value_json(runtime, cp.schema, fallback, *ov_val, true, &was_clamped);
                if (!decoded) {
                    report_warn(cp.name, *ov_val, world::DiagnosticKind::mismatch,
                                String::concat(decoded.error().message, " The override is kept."));
                } else {
                    if (was_clamped)
                        report_warn(cp.name, *ov_val, world::DiagnosticKind::invalid_value,
                                    "Override clamped to promoted parameter hard bounds."_s);
                    desired = std::move(*decoded);
                }
            }
        }
        ATELIER_TRY(write_promoted_targets_if_changed(runtime, cp.schema, desired));
    }
    return {};
}

Array<GraphPromotedParameterView> build_promoted_views(Runtime& runtime, const GraphDocument& doc,
                                                       const json::Value* overrides_obj) {
    Array<GraphPromotedParameterView> out;
    out.reserve(doc.parameters.size());
    for (const auto& param : doc.parameters) {
        GraphPromotedParameterView view;
        view.name = param.name;
        view.label = param.label;
        view.category = param.category;
        view.description = param.description;
        view.targets = param.targets;

        auto schema_res = resolve_promoted_parameter_schema(runtime, doc, param);
        if (!schema_res) {
            view.valid = false;
            view.invalid_reason = schema_res.error().message;
            if (overrides_obj && overrides_obj->kind() == json::Kind::object && *overrides_obj->find(param.name))
                view.overridden = true;
            out.add(std::move(view));
            continue;
        }
        auto& schema = *schema_res;
        view.valid = true;
        view.value_type = schema.primary.value_type;
        view.is_vector = schema.primary.is_vector;
        if (view.label.empty())
            view.label = schema.primary.label;
        if (view.category.empty())
            view.category = schema.primary.category;
        if (view.description.empty())
            view.description = schema.primary.description;

        if (!schema.primary.is_vector && schema.primary.leaves.front().kind == TypeKind::string) {
            const auto& leaf = schema.primary.leaves.front();
            String def_str;
            if (runtime.read(schema.primary.node, leaf.path, ValueOutput{leaf.type, &def_str}))
                view.default_string = std::move(def_str);
            String eff_str = view.default_string;
            if (overrides_obj && overrides_obj->kind() == json::Kind::object) {
                if (const auto ov = *overrides_obj->find(param.name)) {
                    view.overridden = true;
                    if (ov->kind() == json::Kind::string)
                        eff_str = String::from_utf8_validated(*ov->string());
                }
            }
            Property prop;
            prop.name = param.name;
            prop.path = leaf.path;
            prop.type = leaf.type;
            prop.value = Scalar{0.f};
            prop.label = view.label;
            prop.category = view.category;
            prop.description = view.description;
            prop.is_string = true;
            prop.string_value = std::move(eff_str);
            prop.default_string_value = view.default_string;
            view.properties.add(std::move(prop));
            out.add(std::move(view));
            continue;
        }

        for (const auto& leaf : schema.primary.leaves) {
            Scalar s{0.f};
            if (auto read_s = runtime.read(schema.primary.node, leaf.path))
                s = *read_s;
            view.default_scalars.add(s);
        }
        Array<Scalar> eff_scalars = view.default_scalars;
        if (overrides_obj && overrides_obj->kind() == json::Kind::object) {
            if (const auto ov = *overrides_obj->find(param.name)) {
                view.overridden = true;
                if (auto decoded =
                        decode_promoted_value_json(runtime, schema, view.default_scalars, *ov, true, nullptr)) {
                    if (const auto* sv = std::get_if<Array<Scalar>>(&*decoded))
                        eff_scalars = *sv;
                }
            }
        }
        for (std::size_t li = 0; li < schema.primary.leaves.size(); ++li) {
            const auto& leaf = schema.primary.leaves[li];
            Property prop;
            prop.name = schema.primary.is_vector ? String::concat(param.name, ".", leaf.names.back()) : param.name;
            prop.path = leaf.path;
            prop.type = leaf.type;
            prop.value = eff_scalars[li];
            prop.label = view.label;
            prop.category = view.category;
            prop.description = view.description;
            prop.range = schema.ranges[li];
            view.properties.add(std::move(prop));
        }
        out.add(std::move(view));
    }

    if (overrides_obj && overrides_obj->kind() == json::Kind::object) {
        for (const auto& [ov_name_sv, ov_val] : *overrides_obj->object_items()) {
            if (std::ranges::find(doc.parameters, ov_name_sv, &GraphPromotedParameter::name) != doc.parameters.end())
                continue;
            GraphPromotedParameterView stale;
            stale.name = String::from_utf8_validated(ov_name_sv);
            stale.label = String::concat(stale.name, " (Undeclared)");
            stale.valid = false;
            stale.overridden = true;
            stale.undeclared = true;
            stale.invalid_reason =
                String::concat("Override has no matching promoted parameter in the referenced graph (kept as \"",
                               assets::json_format::preview(ov_val), "\").");
            out.add(std::move(stale));
        }
    }
    return out;
}

Result<SavedGraphPortMap> capture_node_authored_inputs(Runtime& runtime, const GraphDocument& doc,
                                                       const procedural::Contents& listed, const GraphNode& target,
                                                       ObjectId overriding_upstream = {},
                                                       const procedural::Ports* overriding_upstream_ports = nullptr) {
    SavedGraphPortMap saved;
    if (const auto kept_in = *target.kept.root().find("inputs"); kept_in && kept_in->kind() == json::Kind::object) {
        for (const auto& [pname, val] : *kept_in->object_items()) {
            Array<String> eps;
            const bool is_arr = val.kind() == json::Kind::array;
            if (is_arr) {
                for (const auto item : *val.array())
                    if (const auto s = item.string())
                        eps.add(String::from_utf8_validated(*s));
            } else if (const auto s = val.string()) {
                eps.add(String::from_utf8_validated(*s));
            }
            saved.emplace(String::from_utf8_validated(pname), std::pair{is_arr, std::move(eps)});
        }
    }
    if (!target.node.registered())
        return saved;

    ATELIER_TRY_BIND(target_ports, procedural::ports(runtime, target.node.id()));
    ATELIER_TRY_BIND(conns, procedural::inputs(runtime, listed, target.node.id()));
    for (std::uint32_t p_idx = 0; p_idx < target_ports->inputs.size(); ++p_idx) {
        const auto& pname = target_ports->inputs[p_idx];
        if (saved.contains(pname))
            continue;
        Array<String> eps;
        for (const auto& in : *conns) {
            if (in.input != p_idx)
                continue;
            const auto* src = find_node_by_id(doc, in.from);
            if (!src)
                continue;
            procedural::Ports local_ports;
            const procedural::Ports* src_ports = nullptr;
            if (in.from == overriding_upstream && overriding_upstream_ports) {
                src_ports = overriding_upstream_ports;
            } else {
                ATELIER_TRY_BIND(resolved_p, procedural::ports(runtime, in.from));
                local_ports = std::move(*resolved_p);
                src_ports = &local_ports;
            }
            if (in.output < src_ports->outputs.size())
                eps.add(String::concat(src->name, ".", src_ports->outputs[in.output]));
        }
        if (!eps.empty())
            saved.emplace(pname, std::pair{target_ports->multiple[p_idx], std::move(eps)});
    }
    return saved;
}

Result<void> realign_asset_node_ports(Runtime& runtime, GraphDocument& doc, GraphNode& asset_entry,
                                      const procedural::Ports& old_ports, procedural::AssetSubgraph next_subgraph,
                                      Array<world::Diagnostic>* issues) {
    const ObjectId graph = doc.graph.id();
    const ObjectId asset_id = asset_entry.node.id();
    const bool ports_changed = old_ports != next_subgraph.ports;
    if (!ports_changed)
        return procedural::set_asset_subgraph(runtime, asset_id, std::move(next_subgraph));

    ATELIER_TRY_BIND(listed, procedural::contents(runtime, graph));
    ATELIER_TRY_BIND(asset_saved_in, capture_node_authored_inputs(runtime, doc, *listed, asset_entry));

    struct DownstreamCapture {
        String name;

        SavedGraphPortMap saved_in;
    };
    Array<DownstreamCapture> downstream;
    const String asset_prefix = String::concat(asset_entry.name, ".");
    for (const auto& item : doc.nodes) {
        if (!item.node.registered() || item.node.id() == asset_id)
            continue;
        ATELIER_TRY_BIND(conns, procedural::inputs(runtime, *listed, item.node.id()));
        const bool live_dep =
            std::ranges::any_of(*conns, [&](const procedural::Input& in) { return in.from == asset_id; });
        const bool kept_dep =
            kept_port_has_match(item.kept, {}, [&](StringView ep) { return ep.starts_with(asset_prefix); });
        if (!live_dep && !kept_dep)
            continue;
        ATELIER_TRY_BIND(captured, capture_node_authored_inputs(runtime, doc, *listed, item, asset_id, &old_ports));
        downstream.add({item.name, std::move(*captured)});
    }

    ATELIER_TRY(replace_node_inputs(runtime, graph, asset_id, {}));
    for (const auto& ds : downstream) {
        if (const auto* item = find_node(doc, ds.name); item && item->node.registered())
            ATELIER_TRY(replace_node_inputs(runtime, graph, item->node.id(), {}));
    }

    ATELIER_TRY(procedural::set_asset_subgraph(runtime, asset_id, std::move(next_subgraph)));
    ATELIER_TRY(apply_saved_graph_node_inputs(runtime, doc, asset_entry, *asset_saved_in, issues));
    for (const auto& ds : downstream) {
        if (auto* item = find_node(doc, ds.name))
            ATELIER_TRY(apply_saved_graph_node_inputs(runtime, doc, *item, ds.saved_in, issues));
    }
    return {};
}

template <class FindOpenGraphFn>
Result<std::uint64_t>
sync_graph_document_assets_impl(Runtime& runtime, GraphDocument& root_doc,
                                Map<ObjectId, std::unique_ptr<BoundAssetInstance>>& root_instances,
                                Project* active_project, std::uint64_t code_version, const AssetPortResolver& resolver,
                                FindOpenGraphFn&& find_open_graph, StringView root_asset_key,
                                bool materialize_subgraphs, bool realign_outer,
                                Array<world::Diagnostic>* root_issues) {
    const std::uint64_t proj_rev = active_project ? active_project->revision() : 0ULL;
    ATELIER_TRY_BIND(asset_node_type, runtime.native_type<procedural::AssetNode>());

    const auto sync_doc = [&](auto&& self, GraphDocument& doc,
                              Map<ObjectId, std::unique_ptr<BoundAssetInstance>>& instances,
                              Array<String>& stack, bool realign_this_doc,
                              Array<world::Diagnostic>* issues) -> Result<std::uint64_t> {
        Set<ObjectId> live_asset_ids;
        std::uint64_t combined_rev = 14695981039346656037ULL;
        const auto mix = [&](std::uint64_t v) noexcept {
            combined_rev ^= v;
            combined_rev *= 1099511628211ULL;
        };

        const auto bind_failed_subgraph = [&](GraphNode& item, const procedural::Ports& old_ports,
                                              procedural::AssetSubgraph failed_sub) -> Result<void> {
            instances.remove(item.node.id());
            mix(Hash<ObjectId>{}(item.node.id()));
            mix(source::content_hash(item.asset_key, 0));
            if (failed_sub.bind_error) {
                mix(static_cast<std::uint64_t>(failed_sub.bind_error->code));
                mix(source::content_hash(failed_sub.bind_error->message, 0));
            }
            if (realign_this_doc)
                return realign_asset_node_ports(runtime, doc, item, old_ports, std::move(failed_sub), issues);
            return procedural::set_asset_subgraph(runtime, item.node.id(), std::move(failed_sub));
        };

        for (auto& item : doc.nodes) {
            if (!item.node.registered())
                continue;
            ATELIER_TRY_BIND(type, runtime.type_of(item.node.id()));
            if (*type != *asset_node_type)
                continue;
            live_asset_ids.add(item.node.id());
            ATELIER_TRY_BIND(old_sub, procedural::asset_subgraph(runtime, item.node.id()));

            if (item.asset_key.empty()) {
                procedural::AssetSubgraph unbound;
                unbound.bind_error = Error{ErrorCode::invalid_definition, "AssetNode has no bound .procgraph asset."};
                ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(unbound)));
                continue;
            }

            const bool is_cycle = std::ranges::find(stack, item.asset_key) != stack.end();
            const bool too_deep = stack.size() >= procedural::Evaluator::max_subgraph_depth;
            if (is_cycle || too_deep) {
                procedural::AssetSubgraph failed_sub;
                failed_sub.asset_key = item.asset_key;
                if (auto p = resolver(item.asset_key))
                    failed_sub.ports = std::move(*p);
                failed_sub.bind_error = Error{
                    ErrorCode::invalid_definition,
                    is_cycle ? String::concat("Cyclic .procgraph asset reference involving '", item.asset_key, "'.")
                             : "Maximum .procgraph AssetNode nesting depth exceeded."_s};
                ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(failed_sub)));
                continue;
            }

            if (!materialize_subgraphs) {
                auto binding = resolve_asset_binding(resolver, item.asset_key);
                if (binding.bind_error) {
                    ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(binding)));
                } else if (binding.ports != old_sub->ports || old_sub->asset_key != item.asset_key ||
                           old_sub->bind_error) {
                    ATELIER_TRY(
                        realign_asset_node_ports(runtime, doc, item, old_sub->ports, std::move(binding), issues));
                }
                continue;
            }

            std::uint64_t src_epoch = 0;
            const auto [open_target_doc, open_mutation_epoch] = find_open_graph(item.asset_key);
            if (open_target_doc) {
                src_epoch = (1ULL << 61) ^ open_mutation_epoch;
            } else if (active_project) {
                src_epoch = (2ULL << 61) ^ proj_rev;
            } else {
                procedural::AssetSubgraph miss_sub;
                miss_sub.asset_key = item.asset_key;
                miss_sub.bind_error = Error{ErrorCode::invalid_storage,
                                            String::concat("No active Project to resolve '", item.asset_key, "'.")};
                ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(miss_sub)));
                continue;
            }

            auto& inst = *instances.try_emplace(item.node.id()).value;
            const bool need_clone = !inst || inst->asset_key != item.asset_key || inst->source_epoch != src_epoch ||
                                    inst->code_version != code_version;
            if (need_clone) {
                Result<GraphDocument> prepared = std::unexpected(Error{ErrorCode::invalid_storage, ""});
                if (open_target_doc) {
                    prepared = clone_graph_document(runtime, *open_target_doc);
                } else {
                    auto loaded_asset = active_project->assets().procedural_graph(item.asset_key);
                    if (!loaded_asset) {
                        procedural::AssetSubgraph miss_sub;
                        miss_sub.asset_key = item.asset_key;
                        miss_sub.bind_error = loaded_asset.error();
                        ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(miss_sub)));
                        continue;
                    }
                    prepared = load_graph(runtime, loaded_asset->text, resolver);
                }
                if (!prepared) {
                    procedural::AssetSubgraph bad_sub;
                    bad_sub.asset_key = item.asset_key;
                    bad_sub.bind_error = prepared.error();
                    ATELIER_TRY(bind_failed_subgraph(item, old_sub->ports, std::move(bad_sub)));
                    continue;
                }
                auto fresh = std::make_unique<BoundAssetInstance>();
                fresh->owner_runtime = &runtime;
                fresh->asset_key = item.asset_key;
                fresh->source_epoch = src_epoch;
                fresh->code_version = code_version;
                fresh->cloned_doc = std::move(*prepared);
                fresh->cached_params = build_cached_promoted_params(runtime, fresh->cloned_doc);
                inst = std::move(fresh);
            }

            if (!inst->overrides_initialized || inst->applied_overrides != item.overrides) {
                inst->override_issues.clear();
                ATELIER_TRY(apply_cached_instance_overrides(
                    runtime, inst->cached_params, item.asset_key, item.overrides,
                    [&](const String& ov_name, json::Value ov_val, world::DiagnosticKind dkind,
                        const String& detail_msg) {
                        world::Diagnostic diag;
                        diag.severity = world::Severity::warning;
                        diag.kind = dkind;
                        diag.phase = world::DiagnosticPhase::load;
                        diag.object = item.node.id();
                        diag.type = "procedural.AssetNode"_s;
                        diag.field = {"nodes"_s, item.name, "overrides"_s, ov_name};
                        diag.value = assets::json_format::preview(ov_val);
                        diag.message = String::concat(detail::dotted(diag.field), ": ", detail_msg);
                        inst->override_issues.add(std::move(diag));
                    }));
                inst->applied_overrides = item.overrides;
                inst->overrides_initialized = true;
                if (item.overrides.root().kind() == json::Kind::object && item.overrides.root().size() > 0) {
                    const auto raw = json::write(item.overrides.root());
                    inst->overrides_hash = source::content_hash(StringView{raw.data(), raw.size()}, 0);
                } else {
                    inst->overrides_hash = 0;
                }
            }
            if (issues) {
                for (auto diag : inst->override_issues) {
                    diag.serial = issues->size() + 1;
                    issues->add(std::move(diag));
                }
            }

            stack.add(item.asset_key);
            const auto first_child_issue = issues ? issues->size() : 0;
            ATELIER_TRY_BIND(child_rev, self(self, inst->cloned_doc, inst->children, stack, true, issues));
            stack.pop();
            if (issues && issues->size() > first_child_issue) {
                const Array<String> path{"nodes"_s, item.name, "asset"_s, item.asset_key};
                const String prefix = String::concat(detail::dotted(path), ".");
                for (auto& child_issue : issues->slice(first_child_issue)) {
                    child_issue.object = item.node.id();
                    child_issue.field.insert_range(0, path);
                    child_issue.message = String::concat(prefix, child_issue.message);
                }
            }

            Array<ObjectId> in_nodes;
            Array<ObjectId> out_nodes;
            procedural::Ports extracted_ports;
            Optional<Error> bind_err;
            if (auto p = extract_graph_ports(runtime, inst->cloned_doc, &in_nodes, &out_nodes)) {
                extracted_ports = std::move(*p);
            } else {
                bind_err = p.error();
            }

            std::uint64_t inst_rev = 14695981039346656037ULL;
            const auto mix_inst = [&](std::uint64_t v) noexcept {
                inst_rev ^= v;
                inst_rev *= 1099511628211ULL;
            };
            mix_inst(Hash<ObjectId>{}(item.node.id()));
            mix_inst(source::content_hash(item.asset_key, 0));
            mix_inst(src_epoch);
            mix_inst(code_version);
            mix_inst(inst->overrides_hash);
            mix_inst(*child_rev);
            if (bind_err) {
                mix_inst(static_cast<std::uint64_t>(bind_err->code));
                mix_inst(source::content_hash(bind_err->message, 0));
            }
            inst->content_revision = inst_rev;
            mix(inst_rev);

            if (need_clone || old_sub->revision != inst_rev || old_sub->ports != extracted_ports ||
                old_sub->graph.id() != inst->cloned_doc.graph.id()) {
                procedural::AssetSubgraph bound;
                bound.asset_key = item.asset_key;
                bound.ports = std::move(extracted_ports);
                bound.graph = inst->cloned_doc.graph;
                bound.input_nodes = std::move(in_nodes);
                bound.output_nodes = std::move(out_nodes);
                bound.bind_error = std::move(bind_err);
                bound.revision = inst_rev;
                if (realign_this_doc)
                    ATELIER_TRY(realign_asset_node_ports(runtime, doc, item, old_sub->ports, std::move(bound), issues));
                else
                    ATELIER_TRY(procedural::set_asset_subgraph(runtime, item.node.id(), std::move(bound)));
            }
        }

        instances.remove_if([&](const ObjectId id, const auto&) { return !live_asset_ids.contains(id); });
        return combined_rev;
    };

    Array<String> stack;
    if (!root_asset_key.empty())
        stack.add(String::from_utf8_validated(root_asset_key));
    return sync_doc(sync_doc, root_doc, root_instances, stack, realign_outer, root_issues);
}

} // namespace

Result<void> Session::sync_open_graph_assets(OpenGraph& open, Project* project, bool materialize_subgraphs) {
    Project* active_project = project ? project : open.resources.project();
    const std::uint64_t code_version =
        project_code_ && project_code_->report() ? project_code_->report()->published_version : 0ULL;
    const auto resolver = make_asset_port_resolver(active_project);

    Array<world::Diagnostic>* issues_sink = nullptr;
    if (materialize_subgraphs || !open.evaluator) {
        open.asset_issues.clear();
        issues_sink = &open.asset_issues;
    }

    const auto find_open = [&](StringView k) -> std::pair<const GraphDocument*, std::uint64_t> {
        if (const auto* it = graphs_.find(k); it && *it)
            return {&(*it)->document, (*it)->mutation_epoch};
        return {nullptr, 0ULL};
    };

    const StringView root_key = !open.scratch ? open.key.view() : StringView{};
    ATELIER_TRY(sync_graph_document_assets_impl(runtime_, open.document, open.asset_instances, active_project,
                                                code_version, resolver, find_open, root_key, materialize_subgraphs,
                                                true, issues_sink));
    return {};
}

Optional<String> Session::graph_controlling_parameter(StringView key, StringView node_name,
                                                      StringView field_path) const noexcept {
    if (node_name.empty() || field_path.empty())
        return {};
    const auto* at = graphs_.find(key);
    if (!at || !*at)
        return {};
    auto nn = String::from_utf8(node_name);
    auto fp = String::from_utf8(field_path);
    if (!nn || !fp)
        return {};
    const String candidate = String::concat(*nn, ".", *fp);
    for (const auto& param : (*at)->document.parameters) {
        for (const auto& target : param.targets) {
            if (graph_target_controls_path(target, candidate))
                return param.name;
        }
    }
    return {};
}

Optional<String> Session::graph_overlapping_parameter(StringView key, StringView node_name,
                                                      StringView field_path) const noexcept {
    if (node_name.empty() || field_path.empty())
        return {};
    const auto* at = graphs_.find(key);
    if (!at || !*at)
        return {};
    auto nn = String::from_utf8(node_name);
    auto fp = String::from_utf8(field_path);
    if (!nn || !fp)
        return {};
    const String candidate = String::concat(*nn, ".", *fp);
    for (const auto& param : (*at)->document.parameters) {
        for (const auto& target : param.targets) {
            if (graph_targets_overlap(target, candidate))
                return param.name;
        }
    }
    return {};
}

json::Document Session::generator_overrides(ObjectId component) const {
    const auto* at = generator_overrides_.find(component);
    return at ? *at : json::Document{};
}

Result<Array<GraphPromotedParameterView>> Session::generator_promoted_parameters(ObjectId component,
                                                                                 Project* project) const {
    if (world_.closed())
        return graph_error("Generator parameter inspection requires an open Session.", ErrorCode::invalid_definition);
    ATELIER_TRY_BIND(state, world::inspect_component(world_.runtime(), component));
    if (!state->valid || state->owner == ObjectId{})
        return graph_error("ProceduralGeneratorComponent is not attached to an Entity.", ErrorCode::invalid_member);
    ATELIER_TRY_BIND(is_gen, world_.runtime().is_a(state->type, presentation_types_.procedural_generator_component));
    if (!*is_gen)
        return graph_error("Target component is not a ProceduralGeneratorComponent.", ErrorCode::invalid_definition);
    ATELIER_TRY_BIND(proc_source, presentation::read_procedural_source(world_.runtime(), component));
    if (proc_source->source.empty())
        return Array<GraphPromotedParameterView>{};

    ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(proc_source->source, project));
    if (!*target_doc)
        return Array<GraphPromotedParameterView>{};

    const json::Document ov_doc = generator_overrides(component);
    const json::Value ov_root = ov_doc.root();
    const json::Value* ov_ptr = ov_root.kind() == json::Kind::object && ov_root.size() > 0 ? &ov_root : nullptr;
    return build_promoted_views(world_.runtime(), **target_doc, ov_ptr);
}

Result<Array<GraphPromotedParameterView>> Session::graph_promoted_parameters(StringView key,
                                                                             Project* project) const {
    const auto* target = graph_document(key);
    if (!target && project) {
        ATELIER_TRY_ASSIGN(target, resolve_target_graph_document(key, project));
    }
    if (!target)
        return graph_error(String::concat("Procedural graph is not open: ", lossy_utf8(key)),
                           ErrorCode::invalid_definition);
    return build_promoted_views(world_.runtime(), *target, nullptr);
}

Result<Array<GraphPromotedParameterView>>
Session::graph_asset_node_parameters(StringView key, StringView node_name, Project* project) const {
    const auto* at = graphs_.find(key);
    if (!at || !*at)
        return graph_error(String::concat("Procedural graph is not open: ", lossy_utf8(key)),
                           ErrorCode::invalid_definition);
    const auto* item = find_node((*at)->document, node_name);
    if (!item)
        return graph_error(String::concat("No node named '", lossy_utf8(node_name), "' in this graph."));
    if (item->asset_key.empty())
        return Array<GraphPromotedParameterView>{};

    Project* active_project = project ? project : (*at)->resources.project();
    ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(item->asset_key, active_project));
    if (!*target_doc)
        return Array<GraphPromotedParameterView>{};

    const json::Value ov_root = item->overrides.root();
    const json::Value* ov_ptr = ov_root.kind() == json::Kind::object && ov_root.size() > 0 ? &ov_root : nullptr;
    return build_promoted_views(world_.runtime(), **target_doc, ov_ptr);
}

Result<String> Session::graph_expose_parameter(StringView key, StringView node_name, StringView field_path,
                                               StringView desired_name, StringView label, StringView category,
                                               StringView description) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto nn = String::from_utf8(node_name);
    auto fp = String::from_utf8(field_path);
    if (!nn || !fp)
        return graph_error("Parameter target specification must be valid UTF-8.");
    const String target_spec = String::concat(*nn, ".", *fp);
    ATELIER_TRY_BIND(resolved, resolve_graph_parameter_target(runtime_, open.document, target_spec));
    if (const auto existing = graph_overlapping_parameter(key, node_name, field_path))
        return graph_error(
            String::concat("Field '", target_spec, "' overlaps with graph parameter '", *existing, "'."));
    auto label_str = String::from_utf8(label);
    auto cat_str = String::from_utf8(category);
    auto desc_str = String::from_utf8(description);
    if (!label_str || !cat_str || !desc_str)
        return graph_error("Parameter metadata must be valid UTF-8.");

    const auto is_param_taken = [&](StringView cand) noexcept {
        return std::ranges::find(open.document.parameters, cand, &GraphPromotedParameter::name) !=
               open.document.parameters.end();
    };
    String param_name;
    if (desired_name.empty()) {
        String stem;
        for (const char c : field_path) {
            if (static_cast<unsigned char>(c) < 0x80u)
                stem.append_ascii(c == '.' ? '_' : c);
        }
        if (!valid_graph_identifier(stem) || !is_param_taken(stem)) {
            param_name = make_unique_graph_identifier(stem, "param", is_param_taken);
        } else {
            const String qualified = String::concat(*nn, "_", stem);
            if (valid_graph_identifier(qualified) && !is_param_taken(qualified))
                param_name = qualified;
            else
                param_name = make_unique_graph_identifier(stem, "param", is_param_taken);
        }
    } else {
        if (!valid_graph_identifier(desired_name))
            return graph_error(String::concat("Invalid parameter name '", lossy_utf8(desired_name),
                                              "': expected [A-Za-z_][A-Za-z0-9_]* up to 128 characters."));
        param_name = String::from_utf8_validated(desired_name);
        if (is_param_taken(param_name))
            return graph_error(
                String::concat("A promoted parameter named '", param_name, "' already exists in this graph."));
    }

    ATELIER_TRY(cancel_edit());
    GraphParameterSchemaChange change;
    change.before_parameters = open.document.parameters;
    GraphPromotedParameter entry;
    entry.name = param_name;
    entry.targets = {target_spec};
    entry.label = !label_str->empty() ? std::move(*label_str) : resolved->label;
    entry.category = !cat_str->empty() ? std::move(*cat_str) : resolved->category;
    entry.description = !desc_str->empty() ? std::move(*desc_str) : resolved->description;
    open.document.parameters.add(std::move(entry));
    change.after_parameters = open.document.parameters;
    open.push_change(std::move(change));
    ++revision_;
    return param_name;
}

Result<void> Session::graph_bind_parameter_target(StringView key, StringView param_name, StringView node_name,
                                                  StringView field_path) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const auto p_it = std::ranges::find(open.document.parameters, param_name, &GraphPromotedParameter::name);
    if (p_it == open.document.parameters.end())
        return graph_error(
            String::concat("No promoted parameter named '", lossy_utf8(param_name), "' in this graph."));
    auto nn = String::from_utf8(node_name);
    auto fp = String::from_utf8(field_path);
    if (!nn || !fp)
        return graph_error("Parameter target specification must be valid UTF-8.");
    const String target_spec = String::concat(*nn, ".", *fp);
    if (std::ranges::find(p_it->targets, target_spec) != p_it->targets.end())
        return {};
    if (const auto existing = graph_overlapping_parameter(key, node_name, field_path))
        return graph_error(
            String::concat("Field '", target_spec, "' overlaps with graph parameter '", *existing, "'."));

    GraphPromotedParameter candidate = *p_it;
    candidate.targets.add(target_spec);
    ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, open.document, candidate));

    ATELIER_TRY(cancel_edit());
    GraphParameterSchemaChange change;
    change.before_parameters = open.document.parameters;

    // Synchronize all targets to primary's value (clamped to the intersected hard bounds).
    std::variant<Array<Scalar>, String> synced_val;
    if (!schema->primary.is_vector && schema->primary.leaves.front().kind == TypeKind::string) {
        String s;
        ATELIER_TRY(runtime_.read(schema->primary.node, schema->primary.leaves.front().path,
                                  ValueOutput{schema->primary.leaves.front().type, &s}));
        synced_val = std::move(s);
    } else {
        Array<Scalar> vals;
        for (std::size_t li = 0; li < schema->primary.leaves.size(); ++li) {
            ATELIER_TRY_BIND(s, runtime_.read(schema->primary.node, schema->primary.leaves[li].path));
            vals.add(detail::clamp_scalar(*s, schema->ranges[li]).first);
        }
        synced_val = std::move(vals);
    }

    Array<ObjectId> touched_nodes;
    for (const auto& t : schema->all_targets) {
        if (std::ranges::find(touched_nodes, t.node) == touched_nodes.end())
            touched_nodes.add(t.node);
    }
    for (const ObjectId nid : touched_nodes) {
        ATELIER_TRY_BIND(held, runtime_.hold(nid));
        ATELIER_TRY_BIND(before_p, capture_object_properties(runtime_, nid));
        const auto* gnode = find_node_by_id(open.document, nid);
        change.synced_nodes.add(
            {std::move(*held), std::move(*before_p), {}, gnode ? gnode->kept : json::Document{}, {}});
    }
    ATELIER_TRY(write_promoted_targets_if_changed(runtime_, *schema, synced_val, &open.document));
    for (auto& snap : change.synced_nodes) {
        ATELIER_TRY_BIND(after_p, capture_object_properties(runtime_, snap.target.id()));
        snap.after_props = std::move(*after_p);
        const auto* gnode = find_node_by_id(open.document, snap.target.id());
        snap.after_kept = gnode ? gnode->kept : json::Document{};
    }

    *p_it = std::move(candidate);
    change.after_parameters = open.document.parameters;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_unbind_parameter_target(StringView key, StringView node_name, StringView field_path) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    auto nn = String::from_utf8(node_name);
    auto fp = String::from_utf8(field_path);
    if (!nn || !fp)
        return {};
    const String target_spec = String::concat(*nn, ".", *fp);
    auto before = open.document.parameters;
    bool changed = false;
    for (auto& param : open.document.parameters) {
        if (param.targets.remove_if([&](const String& t) { return graph_targets_overlap(t, target_spec); }) > 0)
            changed = true;
    }
    if (!changed)
        return {};
    ATELIER_TRY(cancel_edit());
    open.document.parameters.remove_if([](const GraphPromotedParameter& p) { return p.targets.empty(); });
    open.push_change(GraphParameterSchemaChange{std::move(before), open.document.parameters, {}});
    ++revision_;
    return {};
}

Result<void> Session::graph_update_parameter(StringView key, StringView param_name, Optional<String> new_name,
                                             Optional<String> label, Optional<String> category,
                                             Optional<String> description, Optional<std::size_t> new_index) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    auto it = std::ranges::find(open.document.parameters, param_name, &GraphPromotedParameter::name);
    if (it == open.document.parameters.end())
        return graph_error(
            String::concat("No promoted parameter named '", lossy_utf8(param_name), "' in this graph."));
    auto next = open.document.parameters;
    const auto idx = static_cast<std::size_t>(it - open.document.parameters.begin());
    if (new_name && *new_name != param_name) {
        if (!valid_graph_identifier(*new_name))
            return graph_error(String::concat("Invalid parameter name '", *new_name, "'."));
        if (std::ranges::find(next, *new_name, &GraphPromotedParameter::name) != next.end())
            return graph_error(String::concat("A promoted parameter named '", *new_name, "' already exists."));
        next[idx].name = *new_name;
    }
    if (label)
        next[idx].label = *label;
    if (category)
        next[idx].category = *category;
    if (description)
        next[idx].description = *description;
    if (new_index && !next.empty()) {
        const std::size_t dst = std::min(*new_index, next.size() - 1);
        if (dst != idx)
            next.move(idx, dst);
    }
    if (next == open.document.parameters)
        return {};
    ATELIER_TRY(cancel_edit());
    auto before = std::move(open.document.parameters);
    open.document.parameters = std::move(next);
    open.push_change(GraphParameterSchemaChange{std::move(before), open.document.parameters, {}});
    ++revision_;
    return {};
}

Result<void> Session::graph_remove_parameter(StringView key, StringView param_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    auto it = std::ranges::find(open.document.parameters, param_name, &GraphPromotedParameter::name);
    if (it == open.document.parameters.end())
        return graph_error(
            String::concat("No promoted parameter named '", lossy_utf8(param_name), "' in this graph."));
    ATELIER_TRY(cancel_edit());
    auto before = open.document.parameters;
    open.document.parameters.remove_at(static_cast<std::size_t>(it - open.document.parameters.begin()));
    open.push_change(GraphParameterSchemaChange{std::move(before), open.document.parameters, {}});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_parameter_json(StringView key, StringView param_name, json::Value value) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const auto p_it = std::ranges::find(open.document.parameters, param_name, &GraphPromotedParameter::name);
    if (p_it == open.document.parameters.end())
        return graph_error(
            String::concat("No promoted parameter named '", lossy_utf8(param_name), "' in this graph."));
    ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, open.document, *p_it));

    Array<Scalar> current_scalars;
    if (schema->primary.is_vector || schema->primary.leaves.front().kind != TypeKind::string) {
        for (const auto& leaf : schema->primary.leaves) {
            ATELIER_TRY_BIND(s, runtime_.read(schema->primary.node, leaf.path));
            current_scalars.add(*s);
        }
    }
    ATELIER_TRY_BIND(decoded, decode_promoted_value_json(runtime_, *schema, current_scalars, value, false));

    ATELIER_TRY(cancel_edit());
    GraphParameterSchemaChange change{open.document.parameters, open.document.parameters, {}};
    Array<ObjectId> touched_nodes;
    for (const auto& t : schema->all_targets) {
        if (std::ranges::find(touched_nodes, t.node) == touched_nodes.end())
            touched_nodes.add(t.node);
    }
    for (const ObjectId nid : touched_nodes) {
        ATELIER_TRY_BIND(held, runtime_.hold(nid));
        ATELIER_TRY_BIND(before_p, capture_object_properties(runtime_, nid));
        const auto* gnode = find_node_by_id(open.document, nid);
        change.synced_nodes.add(
            {std::move(*held), std::move(*before_p), {}, gnode ? gnode->kept : json::Document{}, {}});
    }
    ATELIER_TRY(write_promoted_targets_if_changed(runtime_, *schema, *decoded, &open.document));
    bool any_changed = false;
    for (auto& snap : change.synced_nodes) {
        ATELIER_TRY_BIND(after_p, capture_object_properties(runtime_, snap.target.id()));
        const auto* gnode = find_node_by_id(open.document, snap.target.id());
        snap.after_kept = gnode ? gnode->kept : json::Document{};
        if (snap.before_props != *after_p || snap.before_kept != snap.after_kept)
            any_changed = true;
        snap.after_props = std::move(*after_p);
    }
    if (!any_changed)
        return {};
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

namespace {

Result<json::Document> canonicalize_promoted_override_json(Runtime& runtime, const GraphDocument& target_doc,
                                                           const String& asset_key, StringView param_name,
                                                           const json::Value* current_overrides_obj,
                                                           json::Value value) {
    const auto p_it = std::ranges::find(target_doc.parameters, param_name, &GraphPromotedParameter::name);
    if (p_it == target_doc.parameters.end())
        return graph_error(
            String::concat("Asset '", asset_key, "' has no promoted parameter '", lossy_utf8(param_name), "'."));
    ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime, target_doc, *p_it));
    if (!schema->primary.is_vector && schema->primary.leaves.front().kind == TypeKind::string) {
        if (value.kind() != json::Kind::string)
            return graph_error(String::concat("Expected string value for override '", lossy_utf8(param_name), "'."));
        json::Builder b;
        b.set_validated(b.root(), *value.string());
        return b.finish();
    }
    Array<Scalar> fallback_scalars;
    fallback_scalars.reserve(schema->primary.leaves.size());
    for (const auto& leaf : schema->primary.leaves) {
        Scalar s{0.f};
        if (auto rs = runtime.read(schema->primary.node, leaf.path))
            s = *rs;
        fallback_scalars.add(s);
    }
    if (current_overrides_obj && current_overrides_obj->kind() == json::Kind::object) {
        if (const auto existing_ov = *current_overrides_obj->find(param_name)) {
            if (auto existing_dec =
                    decode_promoted_value_json(runtime, *schema, fallback_scalars, *existing_ov, true, nullptr)) {
                if (const auto* sv = std::get_if<Array<Scalar>>(&*existing_dec))
                    fallback_scalars = *sv;
            }
        }
    }
    ATELIER_TRY_BIND(decoded_v, decode_promoted_value_json(runtime, *schema, fallback_scalars, value, false));
    const auto& vec_s = std::get<Array<Scalar>>(*decoded_v);
    return encode_parameter_scalars_json(runtime, schema->primary.is_vector, schema->primary.leaves, vec_s);
}

Result<Scalar> validate_and_clamp_promoted_scalar(Runtime& runtime, TypeId expected_type, const FieldRange& range,
                                                  const Scalar& raw) {
    if (!detail::finite_scalar(raw))
        return graph_error("Promoted parameter scalar must be finite.");
    if (const auto* ev = std::get_if<EnumValue>(&raw)) {
        if (ev->type != expected_type)
            return graph_error("Enumeration type mismatch.");
        ATELIER_TRY_BIND(info, runtime.describe(expected_type));
        if (!(*info)->enumeration || !detail::valid_enum_bits(*(*info)->enumeration, ev->bits))
            return graph_error("Invalid enumerator bits.");
        return raw;
    }
    const TypeId actual_type = std::visit(
        [&](const auto& v) -> TypeId {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, EnumValue>)
                return v.type;
            else
                return runtime.native_type<T>().value_or(TypeId{});
        },
        raw);
    if (actual_type == TypeId{} || actual_type != expected_type)
        return graph_error("Promoted parameter scalar type mismatch.");
    return detail::clamp_scalar(raw, range).first;
}

} // namespace

Result<void> Session::graph_set_asset_override_json(StringView key, StringView node_name, StringView param_name,
                                                    Optional<json::Value> value, Project* project) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, node_name);
    if (!item || !item->node.registered())
        return graph_error(
            String::concat("Target AssetNode '", lossy_utf8(node_name), "' is missing or unresolved."));
    ATELIER_TRY_BIND(asset_node_type, runtime_.native_type<procedural::AssetNode>());
    ATELIER_TRY_BIND(node_type, runtime_.type_of(item->node.id()));
    if (*node_type != *asset_node_type)
        return graph_error(String::concat("Node '", item->name, "' is not a procedural.AssetNode."));
    if (!valid_graph_identifier(param_name))
        return graph_error(String::concat("Invalid override parameter name '", lossy_utf8(param_name), "'."));

    const json::Document before_ov = item->overrides;
    json::Document canonical_val;
    if (value) {
        if (item->asset_key.empty())
            return graph_error(String::concat("AssetNode '", item->name, "' has no bound .procgraph asset."));
        Project* active_project = project ? project : open.resources.project();
        ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(item->asset_key, active_project));
        if (!*target_doc)
            return graph_error(String::concat("Unable to resolve .procgraph asset '", item->asset_key, "'."));
        const json::Value before_root = before_ov.root();
        const json::Value* before_ptr = before_root.kind() == json::Kind::object ? &before_root : nullptr;
        ATELIER_TRY_BIND(canon, canonicalize_promoted_override_json(runtime_, **target_doc, item->asset_key, param_name,
                                                                    before_ptr, *value));
        canonical_val = std::move(*canon);
    }

    ATELIER_TRY(cancel_edit());
    json::Builder next(before_ov.root().kind() == json::Kind::object ? before_ov : json::Document{});
    if (value)
        next.set_validated(next.root(), param_name, canonical_val.root());
    else
        next.erase(next.root(), param_name);
    const json::Document after_ov = next.root().size() > 0 ? next.finish() : json::Document{};
    if (before_ov == after_ov)
        return {};
    item->overrides = after_ov;
    open.push_change(GraphAssetOverrideChange{item->name, before_ov, after_ov, item->asset_key, item->asset_key});
    ++revision_;
    return {};
}

Result<void> Session::set_generator_override_json(Project* project, ObjectId component, StringView param_name,
                                                  Optional<json::Value> value) {
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(state, admit_component_edit(component));
    ATELIER_TRY_BIND(is_gen, runtime_.is_a(state->type, presentation_types_.procedural_generator_component));
    if (!*is_gen)
        return graph_error("Target component is not a ProceduralGeneratorComponent.");
    if (!valid_graph_identifier(param_name))
        return graph_error(String::concat("Invalid override parameter name '", lossy_utf8(param_name), "'."));
    ATELIER_TRY(procedural_source_program());
    ATELIER_TRY_BIND(current_src, presentation::read_procedural_source(runtime_, component));
    if (current_src->source.empty() && value.has_value())
        return graph_error("ProceduralGeneratorComponent has no bound .procgraph asset.");

    const json::Document before_ov = generator_overrides(component);
    json::Document canonical_val;
    if (value) {
        ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(current_src->source, project));
        if (!*target_doc)
            return graph_error(String::concat("Unable to resolve .procgraph asset '", current_src->source, "'."));
        const json::Value before_root = before_ov.root();
        const json::Value* before_ptr = before_root.kind() == json::Kind::object ? &before_root : nullptr;
        ATELIER_TRY_BIND(canon, canonicalize_promoted_override_json(runtime_, **target_doc, current_src->source,
                                                                    param_name, before_ptr, *value));
        canonical_val = std::move(*canon);
    }

    json::Builder next(before_ov.root().kind() == json::Kind::object ? before_ov : json::Document{});
    if (value)
        next.set_validated(next.root(), param_name, canonical_val.root());
    else
        next.erase(next.root(), param_name);
    const json::Document after_ov = next.root().size() > 0 ? next.finish() : json::Document{};
    if (before_ov == after_ov)
        return {};

    ATELIER_TRY_BIND(held, runtime_.hold(component));
    if (after_ov.root().size() > 0)
        generator_overrides_.insert_or_assign(component, after_ov);
    else
        generator_overrides_.remove(component);
    remember(ProceduralGraphChange{std::move(*held), state->owner, current_src->source, current_src->source, before_ov,
                                   after_ov});
    return sync_procedural_generators(project);
}

Result<std::pair<String, String>>
Session::graph_extract_asset(Project& project, StringView key, Span<const String> selected_nodes,
                             const std::filesystem::path& asset_relative_path, StringView desired_node_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    if (selected_nodes.empty())
        return graph_error("Select at least one node to extract into a Digital Asset.");
    if (asset_relative_path.empty())
        return graph_error("Specify a target .procgraph relative path inside Content/.");
    if (!desired_node_name.empty() && !valid_graph_identifier(desired_node_name))
        return graph_error(String::concat("Invalid AssetNode name '", lossy_utf8(desired_node_name),
                                          "': expected [A-Za-z_][A-Za-z0-9_]* up to 128 characters."));

    ATELIER_TRY_BIND(graph_in_type, runtime_.native_type<procedural::GraphInput>());
    ATELIER_TRY_BIND(graph_out_type, runtime_.native_type<procedural::GraphOutput>());
    ATELIER_TRY_BIND(asset_node_type, runtime_.native_type<procedural::AssetNode>());
    ATELIER_TRY_BIND(graph_type, runtime_.native_type<procedural::Graph>());

    Array<String> sel_names;
    Set<ObjectId> sel_ids;
    Set<String> sel_name_set;
    for (const auto& name : selected_nodes) {
        if (!sel_name_set.add(name))
            continue;
        const auto* item = find_node(open.document, name);
        if (!item || !item->node.registered())
            return graph_error(String::concat("Selected node '", name, "' is missing or unresolved."));
        ATELIER_TRY_BIND(ntype, runtime_.type_of(item->node.id()));
        if (*ntype == *graph_in_type || *ntype == *graph_out_type)
            return graph_error(
                "Boundary GraphInput / GraphOutput nodes belong to the outer graph and cannot be extracted.");
        sel_names.add(name);
        sel_ids.add(item->node.id());
    }
    std::ranges::sort(sel_names);

    ATELIER_TRY(cancel_edit());

    const ObjectId outer_graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, outer_graph));

    // Build full adjacency over live nodes to verify selection convexity (contracted graph remains a DAG).
    Map<ObjectId, Array<procedural::Input>> node_in_edges;
    node_in_edges.reserve(listed->nodes().size());
    Map<ObjectId, Array<ObjectId>> forward_adj;
    Map<ObjectId, Array<ObjectId>> reverse_adj;
    const auto add_dependency = [&](ObjectId from, ObjectId to) {
        forward_adj.try_emplace(from).value->add(to);
        reverse_adj.try_emplace(to).value->add(from);
    };
    for (const auto& item : open.document.nodes) {
        if (!item.node.registered())
            continue;
        ATELIER_TRY_BIND(ins, procedural::inputs(runtime_, *listed, item.node.id()));
        for (const auto& edge : *ins)
            add_dependency(edge.from, item.node.id());
        node_in_edges.try_emplace(item.node.id(), std::move(*ins));
    }

    struct SavedBoundary {
        const GraphNode* target;

        const GraphNode* source;

        std::uint32_t output;

        String endpoint;

        String port;

        Optional<std::size_t> slot;
    };
    Array<SavedBoundary> saved_boundaries;
    Set<StringView> outer_names;
    for (const auto& target : open.document.nodes) {
        const bool inside = sel_name_set.contains(target.name);
        if (!inside)
            outer_names.add(target.name.view());
        const auto inputs = *target.kept.root().find("inputs");
        if (!inputs)
            continue;
        const auto record = [&](StringView port, json::Value endpoint, Optional<std::size_t> slot) -> Result<void> {
            const auto text = endpoint.string();
            if (!text)
                return graph_error(String::concat("Invalid saved connection at ", target.name, ".",
                                                  String::validated(port)));
            const auto dot = text->find('.');
            const StringView source_name = text->slice(0, dot);
            if (sel_name_set.contains(source_name) == inside) {
                if (!inside)
                    outer_names.add(source_name);
                return {};
            }
            const auto* source = find_node(open.document, source_name);
            if (source && source->node.registered() && dot != StringView::npos) {
                ATELIER_TRY_BIND(ports, procedural::ports(runtime_, source->node.id()));
                const auto output = std::ranges::find(ports->outputs, text->slice(dot + 1));
                if (output != ports->outputs.end()) {
                    saved_boundaries.add({&target, source,
                                          static_cast<std::uint32_t>(output - ports->outputs.begin()),
                                          String::from_utf8_validated(*text), String::from_utf8_validated(port), slot});
                    if (target.node.registered())
                        add_dependency(source->node.id(), target.node.id());
                    return {};
                }
            }
            return graph_error(String::concat("Cannot extract saved connection '", String::validated(*text), "' at ",
                                              target.name, ".", String::validated(port),
                                              ": the boundary source port is unresolved."));
        };
        for (const auto& [port, saved] : *inputs->object_items()) {
            if (saved.kind() == json::Kind::array) {
                std::size_t slot{};
                for (const auto endpoint : *saved.array())
                    ATELIER_TRY(record(port, endpoint, slot++));
            } else {
                ATELIER_TRY(record(port, saved, {}));
            }
        }
    }

    // A retained source name is occupied even when its node is absent. Reusing it would
    // silently resolve an unrelated connection on the next load.
    if (!desired_node_name.empty() && outer_names.contains(desired_node_name))
        return graph_error(String::concat("AssetNode name '", String::validated(desired_node_name),
                                          "' is already used by an outer node or connection."));
    const String stem_utf8 = platform::utf16_to_utf8(asset_relative_path.stem().wstring()).value_or(String{});
    const String asset_node_name =
        desired_node_name.empty()
            ? make_unique_graph_identifier(stem_utf8, "asset", [&](StringView name) { return outer_names.contains(name); })
            : String::from_utf8_validated(desired_node_name);

    Set<ObjectId> reachable_from_sel;
    {
        Array<ObjectId> q(sel_ids);
        for (std::size_t qi = 0; qi < q.size(); ++qi) {
            if (const auto* downstream = forward_adj.find(q[qi])) {
                for (const ObjectId nxt : *downstream) {
                    if (!sel_ids.contains(nxt) && reachable_from_sel.add(nxt))
                        q.add(nxt);
                }
            }
        }
    }
    Set<ObjectId> can_reach_sel;
    {
        Array<ObjectId> q(sel_ids);
        for (std::size_t qi = 0; qi < q.size(); ++qi) {
            if (const auto* upstream = reverse_adj.find(q[qi])) {
                for (const ObjectId prv : *upstream) {
                    if (!sel_ids.contains(prv) && can_reach_sel.add(prv))
                        q.add(prv);
                }
            }
        }
    }
    for (const ObjectId ext : reachable_from_sel) {
        if (can_reach_sel.contains(ext)) {
            const auto* ext_node = find_node_by_id(open.document, ext);
            return graph_error(
                String::concat("Selected nodes do not form a convex subgraph: external node '",
                               ext_node ? ext_node->name : "?"_s,
                               "' lies both downstream and upstream of the selection, which would create a cycle."));
        }
    }

    struct BoundaryInGroup {
        ObjectId ext_from{};

        String ext_name;

        std::uint32_t ext_output{};

        String port_name;
    };

    struct BoundaryOutGroup {
        ObjectId sel_from{};

        String sel_name;

        std::uint32_t sel_output{};

        String port_name;
    };

    const auto alloc_port_name = [](StringView preferred, Set<String>& used, StringView fallback) {
        auto chosen = make_unique_graph_identifier(preferred, fallback, [&](StringView cand) { return used.contains(cand); });
        used.add(chosen);
        return chosen;
    };

    Array<BoundaryInGroup> incoming_groups;
    const auto add_in = [&](const GraphNode& src, std::uint32_t output, const String& input_name) {
        if (std::ranges::none_of(incoming_groups,
                                 [&](const auto& g) { return g.ext_from == src.node.id() && g.ext_output == output; }))
            incoming_groups.add({src.node.id(), src.name, output, input_name});
    };
    for (const auto& sname : sel_names) {
        const auto* sel_item = find_node(open.document, sname);
        ATELIER_TRY_BIND(ports, procedural::ports(runtime_, sel_item->node.id()));
        for (const auto& edge : *node_in_edges.find(sel_item->node.id()))
            if (!sel_ids.contains(edge.from))
                add_in(*find_node_by_id(open.document, edge.from), edge.output, ports->inputs[edge.input]);
    }
    for (const auto& saved : saved_boundaries)
        if (sel_name_set.contains(saved.target->name))
            add_in(*saved.source, saved.output, saved.port);
    std::ranges::sort(incoming_groups, [](const BoundaryInGroup& a, const BoundaryInGroup& b) {
        return a.ext_name != b.ext_name ? a.ext_name < b.ext_name : a.ext_output < b.ext_output;
    });
    Set<String> used_in_ports;
    for (auto& g : incoming_groups)
        g.port_name = alloc_port_name(g.port_name, used_in_ports, "in");

    Array<BoundaryOutGroup> outgoing_groups;
    Set<ObjectId> downstream_ext_ids;
    const auto add_out = [&](const GraphNode& src, std::uint32_t output, StringView name) {
        if (std::ranges::none_of(outgoing_groups,
                                 [&](const auto& g) { return g.sel_from == src.node.id() && g.sel_output == output; }))
            outgoing_groups.add({src.node.id(), src.name, output, String::from_utf8_validated(name)});
    };
    for (const auto& item : open.document.nodes) {
        if (!item.node.registered() || sel_ids.contains(item.node.id()))
            continue;
        for (const auto& edge : *node_in_edges.find(item.node.id())) {
            if (!sel_ids.contains(edge.from))
                continue;
            downstream_ext_ids.add(item.node.id());
            ATELIER_TRY_BIND(ports, procedural::ports(runtime_, edge.from));
            add_out(*find_node_by_id(open.document, edge.from), edge.output, ports->outputs[edge.output]);
        }
    }
    for (const auto& saved : saved_boundaries)
        if (!sel_name_set.contains(saved.target->name))
            add_out(*saved.source, saved.output, saved.endpoint.view().slice(saved.endpoint.find('.') + 1));
    std::ranges::sort(outgoing_groups, [](const BoundaryOutGroup& a, const BoundaryOutGroup& b) {
        return a.sel_name != b.sel_name ? a.sel_name < b.sel_name : a.sel_output < b.sel_output;
    });
    Set<String> used_out_ports;
    for (auto& g : outgoing_groups)
        g.port_name = alloc_port_name(g.port_name, used_out_ports, "out");

    const ObjectId before_display = listed->display();
    const ObjectId before_template = open.document.template_node;

    if (outgoing_groups.empty()) {
        ObjectId fallback_sink{};
        if (before_display != ObjectId{} && sel_ids.contains(before_display)) {
            fallback_sink = before_display;
        } else {
            for (const auto& sname : sel_names) {
                const auto* sel_item = find_node(open.document, sname);
                const auto* downstream = forward_adj.find(sel_item->node.id());
                const bool has_internal_downstream =
                    downstream && std::ranges::any_of(*downstream, [&](ObjectId d) { return sel_ids.contains(d); });
                ATELIER_TRY_BIND(sp, procedural::ports(runtime_, sel_item->node.id()));
                if (!has_internal_downstream && !sp->outputs.empty()) {
                    fallback_sink = sel_item->node.id();
                    break;
                }
            }
        }
        if (fallback_sink != ObjectId{}) {
            const auto* sink_item = find_node_by_id(open.document, fallback_sink);
            outgoing_groups.add({fallback_sink, sink_item->name, 0u, "out"_s});
        }
    }

    // Compute selection centroid & bounding box for clean internal/external layout.
    float sum_x = 0.f;
    float sum_y = 0.f;
    float min_y = std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();
    for (const auto& sname : sel_names) {
        const auto* sel_item = find_node(open.document, sname);
        sum_x += sel_item->position[0];
        sum_y += sel_item->position[1];
        min_y = std::min(min_y, sel_item->position[1]);
        max_y = std::max(max_y, sel_item->position[1]);
    }
    const float cx = std::round(sum_x / static_cast<float>(sel_names.size()));
    const float cy = std::round(sum_y / static_cast<float>(sel_names.size()));

    // Assemble extracted GraphDocument in memory and serialize before mutating outer graph; destroy temporary
    // Runtime objects deterministically on scope exit.
    GraphDocument extracted;
    ScopeExit extracted_guard{[&]() noexcept { destroy_private_graph_document(runtime_, extracted); }};

    ATELIER_TRY_BIND(ext_graph_root, runtime_.create(*graph_type));
    extracted.graph = std::move(*ext_graph_root);
    const ObjectId ext_graph_id = extracted.graph.id();

    Map<ObjectId, ObjectId> inner_ids;
    inner_ids.reserve(sel_names.size());
    for (const auto& sname : sel_names) {
        const auto* src = find_node(open.document, sname);
        ATELIER_TRY_BIND(ntype, runtime_.type_of(src->node.id()));
        ATELIER_TRY_BIND(props, capture_object_properties(runtime_, src->node.id()));
        ATELIER_TRY_BIND(is_byp, procedural::bypassed(runtime_, src->node.id()));
        ATELIER_TRY_BIND(created, procedural::add_node(runtime_, ext_graph_id, *ntype));
        const ObjectId cid = created->id();
        ATELIER_TRY(apply_object_properties(runtime_, cid, props->root()));
        if (auto src_sub = procedural::asset_subgraph(runtime_, src->node.id())) {
            procedural::AssetSubgraph stub;
            stub.asset_key = src_sub->asset_key;
            stub.ports = src_sub->ports;
            ATELIER_TRY(procedural::set_asset_subgraph(runtime_, cid, std::move(stub)));
        }
        if (*is_byp)
            ATELIER_TRY(procedural::set_bypassed(runtime_, cid, true));
        GraphNode copy_node = *src;
        copy_node.node = std::move(*created);
        copy_node.position = {std::round(src->position[0] - cx), std::round(src->position[1] - cy)};
        inner_ids.try_emplace(src->node.id(), cid);
        extracted.nodes.add(std::move(copy_node));
    }
    sort_graph_nodes(extracted.nodes);

    const auto unique_inner_name = [&](StringView stem, StringView fallback) {
        return make_unique_graph_identifier(
            stem, fallback, [&](StringView cand) noexcept { return find_node(extracted, cand) != nullptr; });
    };

    Array<ObjectId> created_in_pins;
    for (std::size_t i = 0; i < incoming_groups.size(); ++i) {
        ATELIER_TRY_BIND(pin, procedural::add_node(runtime_, ext_graph_id, *graph_in_type));
        const ObjectId pid = pin->id();
        ATELIER_TRY(runtime_.with_native<procedural::GraphInput>(pid, [&](procedural::GraphInput& gi) -> Result<void> {
            gi.port_name = incoming_groups[i].port_name;
            gi.order = static_cast<std::int32_t>(i);
            return {};
        }));
        const float half = static_cast<float>(incoming_groups.size() - 1) * 0.5f;
        GraphNode pin_node;
        const String in_stem = String::concat("input_", incoming_groups[i].port_name);
        pin_node.name = unique_inner_name(in_stem, "input");
        pin_node.node = std::move(*pin);
        pin_node.position = {std::round((static_cast<float>(i) - half) * 196.f), std::round((min_y - cy) - 120.f)};
        created_in_pins.add(pid);
        extracted.nodes.add(std::move(pin_node));
        sort_graph_nodes(extracted.nodes);
    }

    Array<ObjectId> created_out_pins;
    for (std::size_t i = 0; i < outgoing_groups.size(); ++i) {
        ATELIER_TRY_BIND(pin, procedural::add_node(runtime_, ext_graph_id, *graph_out_type));
        const ObjectId pid = pin->id();
        ATELIER_TRY(
            runtime_.with_native<procedural::GraphOutput>(pid, [&](procedural::GraphOutput& go) -> Result<void> {
                go.port_name = outgoing_groups[i].port_name;
                go.order = static_cast<std::int32_t>(i);
                return {};
            }));
        const float half = static_cast<float>(outgoing_groups.size() - 1) * 0.5f;
        GraphNode pin_node;
        const String out_stem = String::concat("output_", outgoing_groups[i].port_name);
        pin_node.name = unique_inner_name(out_stem, "output");
        pin_node.node = std::move(*pin);
        pin_node.position = {std::round((static_cast<float>(i) - half) * 196.f), std::round((max_y - cy) + 120.f)};
        created_out_pins.add(pid);
        extracted.nodes.add(std::move(pin_node));
        sort_graph_nodes(extracted.nodes);
    }

    // Wire internal edges and GraphInput fanouts in exact authored multi-input slot order, then GraphOutput sinks.
    for (const auto& sname : sel_names) {
        const auto* orig = find_node(open.document, sname);
        const ObjectId dst_inner = *inner_ids.find(orig->node.id());
        for (const auto& edge : *node_in_edges.find(orig->node.id())) {
            if (sel_ids.contains(edge.from)) {
                ATELIER_TRY(procedural::connect(runtime_, ext_graph_id, *inner_ids.find(edge.from), edge.output,
                                                dst_inner, edge.input));
            } else {
                const auto gi = std::ranges::find_if(incoming_groups, [&](const BoundaryInGroup& g) {
                    return g.ext_from == edge.from && g.ext_output == edge.output;
                });
                if (gi != incoming_groups.end()) {
                    const auto pin_idx = static_cast<std::size_t>(gi - incoming_groups.begin());
                    ATELIER_TRY(procedural::connect(runtime_, ext_graph_id, created_in_pins[pin_idx], 0, dst_inner,
                                                    edge.input));
                }
            }
        }
    }
    for (std::size_t i = 0; i < outgoing_groups.size(); ++i) {
        ATELIER_TRY(procedural::connect(runtime_, ext_graph_id, *inner_ids.find(outgoing_groups[i].sel_from),
                                        outgoing_groups[i].sel_output, created_out_pins[i], 0));
    }
    if (!created_out_pins.empty())
        ATELIER_TRY(procedural::set_display(runtime_, ext_graph_id, created_out_pins.front()));
    else if (before_display != ObjectId{} && sel_ids.contains(before_display))
        ATELIER_TRY(procedural::set_display(runtime_, ext_graph_id, *inner_ids.find(before_display)));

    // Preserve promoted parameters whose targets lie completely inside the extracted subgraph.
    for (const auto& param : open.document.parameters) {
        const bool all_inside = std::ranges::all_of(param.targets, [&](const String& t) {
            const auto dot = t.find('.');
            return dot != String::npos && sel_name_set.contains(t.view().slice(0, dot));
        });
        if (all_inside)
            extracted.parameters.add(param);
    }

    GraphExtractChange change;
    // Retained multi-input arrays are the authoritative full order (live and unresolved slots).
    // Entries were gathered by target, so rewrite only the crossing slots on one copy per node.
    for (std::size_t i = 0; i < saved_boundaries.size();) {
        const auto& owner = *saved_boundaries[i].target;
        const bool inside = sel_name_set.contains(owner.name);
        json::Builder kept(owner.kept);
        const auto inputs = *kept.root().at("inputs");
        do {
            const auto& saved = saved_boundaries[i++];
            auto cell = *inputs.at(saved.port);
            if (saved.slot)
                cell = *cell.at(*saved.slot);
            if (inside) {
                const auto g = std::ranges::find_if(incoming_groups, [&](const auto& pin) {
                    return pin.ext_from == saved.source->node.id() && pin.ext_output == saved.output;
                });
                const auto* pin = find_node_by_id(extracted, created_in_pins[g - incoming_groups.begin()]);
                kept.set_validated(cell, String::concat(pin->name, ".output"));
            } else {
                const auto g = std::ranges::find_if(outgoing_groups, [&](const auto& pin) {
                    return pin.sel_from == saved.source->node.id() && pin.sel_output == saved.output;
                });
                kept.set_validated(cell, String::concat(asset_node_name, ".", g->port_name));
            }
        } while (i < saved_boundaries.size() && saved_boundaries[i].target == &owner);
        if (inside)
            find_node(extracted, owner.name)->kept = kept.finish();
        else
            change.retained_node_kept.add({owner.name, owner.kept, kept.finish()});
    }
    ATELIER_TRY_BIND(extracted_ports, extract_graph_ports(runtime_, extracted));
    ATELIER_TRY_BIND(extracted_text, editor::save_graph(runtime_, extracted));
    ATELIER_TRY_BIND(created_asset, project.create_procedural_graph(asset_relative_path, *extracted_text));

    change.before_display = before_display;
    change.before_template = before_template;
    change.before_parameters = open.document.parameters;

    for (const auto& item : open.document.nodes) {
        if (item.node.registered())
            change.before_inputs.add({item.node.id(), *node_in_edges.find(item.node.id()), {}});
    }

    for (const ObjectId sid : sel_ids)
        ATELIER_TRY(replace_node_inputs(runtime_, outer_graph, sid, {}));
    for (const ObjectId ext_id : downstream_ext_ids)
        ATELIER_TRY(replace_node_inputs(runtime_, outer_graph, ext_id, {}));

    for (const auto& sname : sel_names) {
        auto it = std::ranges::find(open.document.nodes, sname, &GraphNode::name);
        if (it != open.document.nodes.end()) {
            if (it->node.registered()) {
                detach_asset_node_subgraph(runtime_, it->node.id());
                ATELIER_TRY(procedural::remove_node(runtime_, outer_graph, it->node.id()));
            }
            change.removed_nodes.add(std::move(*it));
            open.document.nodes.remove_at(static_cast<std::size_t>(it - open.document.nodes.begin()));
        }
    }

    ATELIER_TRY_BIND(new_asset_root, procedural::add_node(runtime_, outer_graph, *asset_node_type));
    const ObjectId new_asset_id = new_asset_root->id();
    {
        procedural::AssetSubgraph stub;
        stub.asset_key = *created_asset;
        stub.ports = *extracted_ports;
        ATELIER_TRY(procedural::set_asset_subgraph(runtime_, new_asset_id, std::move(stub)));
    }

    GraphNode asset_entry;
    asset_entry.name = asset_node_name;
    asset_entry.node = std::move(*new_asset_root);
    asset_entry.asset_key = *created_asset;
    asset_entry.position = {cx, cy};
    open.document.nodes.add(asset_entry);
    sort_graph_nodes(open.document.nodes);
    change.added_asset_node = asset_entry;

    // Wire incoming boundary groups -> AssetNode, and restore downstream consumers preserving multi-input slot order.
    for (std::size_t i = 0; i < incoming_groups.size(); ++i) {
        ATELIER_TRY(procedural::connect(runtime_, outer_graph, incoming_groups[i].ext_from,
                                        incoming_groups[i].ext_output, new_asset_id, static_cast<std::uint32_t>(i)));
    }
    for (const ObjectId ext_id : downstream_ext_ids) {
        Array<procedural::Input> desired;
        const auto& orig_edges = *node_in_edges.find(ext_id);
        desired.reserve(orig_edges.size());
        for (const auto& edge : orig_edges) {
            if (!sel_ids.contains(edge.from)) {
                desired.add(edge);
                continue;
            }
            const auto og = std::ranges::find_if(outgoing_groups, [&](const BoundaryOutGroup& g) {
                return g.sel_from == edge.from && g.sel_output == edge.output;
            });
            if (og == outgoing_groups.end())
                continue;
            const procedural::Input mapped{new_asset_id, static_cast<std::uint32_t>(og - outgoing_groups.begin()),
                                           edge.input};
            if (std::ranges::find(desired, mapped) == desired.end())
                desired.add(mapped);
        }
        ATELIER_TRY(replace_node_inputs(runtime_, outer_graph, ext_id, desired));
    }

    ObjectId after_display = before_display;
    if (before_display != ObjectId{} && sel_ids.contains(before_display)) {
        after_display = !extracted_ports->outputs.empty() ? new_asset_id : ObjectId{};
        ATELIER_TRY(procedural::set_display(runtime_, outer_graph, after_display));
    }
    ObjectId after_template = before_template;
    if (before_template != ObjectId{} && sel_ids.contains(before_template)) {
        after_template = !extracted_ports->outputs.empty() ? new_asset_id : ObjectId{};
        open.document.template_node = after_template;
    }
    change.after_display = after_display;
    change.after_template = after_template;

    // Update outer graph promoted parameters & surviving kept inputs.
    for (auto& param : open.document.parameters) {
        param.targets.remove_if([&](const String& t) {
            const auto dot = t.find('.');
            return dot != String::npos && sel_name_set.contains(t.view().slice(0, dot));
        });
    }
    open.document.parameters.remove_if([](const GraphPromotedParameter& p) { return p.targets.empty(); });
    change.after_parameters = open.document.parameters;

    for (const auto& retained : change.retained_node_kept)
        find_node(open.document, retained.owner)->kept = retained.after;
    ATELIER_TRY_BIND(after_listed, procedural::contents(runtime_, outer_graph));
    for (const auto& node : open.document.nodes) {
        if (node.node.registered()) {
            ATELIER_TRY_BIND(inputs, procedural::inputs(runtime_, *after_listed, node.node.id()));
            change.after_inputs.add({node.node.id(), std::move(*inputs), {}});
        }
    }

    open.push_change(std::move(change));
    ATELIER_TRY(sync_open_graph_assets(open, &project, false));
    ++revision_;
    return std::pair{*created_asset, asset_node_name};
}

Result<void> Session::graph_set_node_asset(StringView key, StringView node_name, StringView target_asset_key,
                                           Project* project) {
    if (!target_asset_key.empty() && target_asset_key == key)
        return graph_error("A procedural graph cannot instantiate itself as an AssetNode.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto target_str = String::from_utf8(target_asset_key);
    if (!target_str)
        return graph_error("Target .procgraph asset key must be valid UTF-8.");
    if (!open.scratch && !target_asset_key.empty() && is_scratch_graph(target_asset_key))
        return graph_error("Persistent .procgraph assets cannot reference scratch graphs.");
    auto* item = find_node(open.document, node_name);
    ATELIER_TRY_BIND(asset_node_type, runtime_.native_type<procedural::AssetNode>());
    if (!item || !item->node.registered() || runtime_.type_of(item->node.id()) != *asset_node_type)
        return graph_error("Target node is not a procedural.AssetNode.");
    if (item->asset_key == target_asset_key)
        return {};
    ATELIER_TRY(cancel_edit());

    const ObjectId graph = open.document.graph.id();
    ATELIER_TRY_BIND(before_sub, procedural::asset_subgraph(runtime_, item->node.id()));
    ATELIER_TRY_BIND(before_listed, procedural::contents(runtime_, graph));

    struct PreConnectionState {
        ObjectRoot target;

        Array<procedural::Input> inputs;

        json::Document kept;
    };
    Array<PreConnectionState> pre_states;
    for (const auto& gn : open.document.nodes) {
        if (!gn.node.registered())
            continue;
        ATELIER_TRY_BIND(ins, procedural::inputs(runtime_, *before_listed, gn.node.id()));
        pre_states.add({gn.node, std::move(*ins), gn.kept});
    }

    const String before_asset = item->asset_key;
    item->asset_key = std::move(*target_str);
    if (auto synced = sync_open_graph_assets(open, project, false); !synced) {
        item->asset_key = before_asset;
        return synced;
    }

    ATELIER_TRY_BIND(after_sub, procedural::asset_subgraph(runtime_, item->node.id()));
    ATELIER_TRY_BIND(after_listed, procedural::contents(runtime_, graph));

    GraphAssetOverrideChange change;
    change.node_name = item->name;
    change.before_overrides = item->overrides;
    change.after_overrides = item->overrides;
    change.before_asset = before_asset;
    change.after_asset = item->asset_key;
    change.asset_changed = true;
    change.before_ports = std::move(before_sub->ports);
    change.after_ports = std::move(after_sub->ports);

    for (auto& pre : pre_states) {
        const auto* gn = find_node_by_id(open.document, pre.target.id());
        if (!gn)
            continue;
        ATELIER_TRY_BIND(after_ins, procedural::inputs(runtime_, *after_listed, pre.target.id()));
        if (pre.inputs != *after_ins || pre.kept != gn->kept) {
            change.connection_deltas.add(
                {std::move(pre.target), std::move(pre.inputs), std::move(*after_ins), std::move(pre.kept), gn->kept});
        }
    }

    open.push_change(std::move(change));
    ++revision_;
    return {};
}

bool Session::graph_parameter_editing(std::uint64_t control_session) const noexcept {
    return graph_param_edit_ && graph_param_edit_->control == control_session;
}

Result<void> Session::begin_graph_parameter_edit(StringView key, StringView node_name, StringView param_name,
                                                 Optional<std::size_t> lane, std::uint64_t control_session,
                                                 Project* project, ObjectId generator_component) {
    if (editing())
        return graph_error("A field edit is already active.");
    if (!control_session)
        return graph_error("A field edit requires a control session.");
    auto param_str = String::from_utf8(param_name);
    if (!param_str)
        return graph_error("Parameter name must be valid UTF-8.");

    GraphParameterEdit edit;
    edit.control = control_session;
    edit.param_name = std::move(*param_str);
    edit.lane = lane;
    edit.edit_project = project;

    const auto populate_from_schema = [&](const ResolvedPromotedSchema& schema,
                                          const json::Value* overrides_obj) -> Result<void> {
        if (!schema.primary.is_vector && schema.primary.leaves.front().kind == TypeKind::string)
            return graph_error("String parameters use set_promoted_parameter_string.");
        edit.value_type = schema.primary.value_type;
        edit.is_vector = schema.primary.is_vector;
        edit.lane_fields = schema.primary.leaves;
        edit.lane_ranges = schema.ranges;
        edit.current_scalars.reserve(schema.primary.leaves.size());
        for (const auto& leaf : schema.primary.leaves) {
            ATELIER_TRY_BIND(s, runtime_.read(schema.primary.node, leaf.path));
            edit.current_scalars.add(*s);
        }
        if (overrides_obj && overrides_obj->kind() == json::Kind::object) {
            if (const auto ov = *overrides_obj->find(param_name)) {
                if (auto dec = decode_promoted_value_json(runtime_, schema, edit.current_scalars, *ov, true, nullptr)) {
                    if (const auto* sv = std::get_if<Array<Scalar>>(&*dec))
                        edit.current_scalars = *sv;
                }
            }
        }
        if (lane && *lane >= edit.current_scalars.size())
            return graph_error("Vector lane index out of bounds.");
        edit.range = lane ? edit.lane_ranges[*lane] : edit.lane_ranges.front();
        return {};
    };

    if (generator_component != ObjectId{}) {
        ATELIER_TRY_BIND(comp_state, admit_component_edit(generator_component));
        ATELIER_TRY_BIND(is_gen, runtime_.is_a(comp_state->type, presentation_types_.procedural_generator_component));
        if (!*is_gen)
            return graph_error("Target component is not a ProceduralGeneratorComponent.");
        ATELIER_TRY(procedural_source_program());
        ATELIER_TRY_BIND(proc_src, presentation::read_procedural_source(runtime_, generator_component));
        if (proc_src->source.empty())
            return graph_error("ProceduralGeneratorComponent has no bound .procgraph asset.");
        ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(proc_src->source, project));
        if (!*target_doc)
            return graph_error(String::concat("Unable to resolve .procgraph asset '", proc_src->source, "'."));
        const auto p_it = std::ranges::find((*target_doc)->parameters, param_name, &GraphPromotedParameter::name);
        if (p_it == (*target_doc)->parameters.end())
            return graph_error(String::concat("Unknown generator parameter '", edit.param_name, "'."));
        ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, **target_doc, *p_it));

        edit.before_overrides = generator_overrides(generator_component);
        const json::Value ov_root = edit.before_overrides.root();
        ATELIER_TRY(populate_from_schema(*schema, ov_root.kind() == json::Kind::object ? &ov_root : nullptr));
        ATELIER_TRY_BIND(held, runtime_.hold(generator_component));
        edit.generator_root = std::move(*held);
        edit.generator_owner = comp_state->owner;
        graph_param_edit_ = std::move(edit);
        return {};
    }

    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    edit.graph_key = open.key;
    if (!edit.edit_project)
        edit.edit_project = open.resources.project();

    if (node_name.empty()) {
        const auto p_it = std::ranges::find(open.document.parameters, param_name, &GraphPromotedParameter::name);
        if (p_it == open.document.parameters.end())
            return graph_error(String::concat("No promoted parameter named '", edit.param_name, "'."));
        ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, open.document, *p_it));
        ATELIER_TRY(populate_from_schema(*schema, nullptr));

        Array<ObjectId> touched;
        for (const auto& t : schema->all_targets) {
            if (std::ranges::find(touched, t.node) == touched.end())
                touched.add(t.node);
        }
        for (const ObjectId nid : touched) {
            ATELIER_TRY_BIND(held, runtime_.hold(nid));
            ATELIER_TRY_BIND(before_p, capture_object_properties(runtime_, nid));
            const auto* gnode = find_node_by_id(open.document, nid);
            edit.node_backups.add(
                {std::move(*held), std::move(*before_p), gnode ? gnode->kept : json::Document{}});
        }
    } else {
        auto* item = find_node(open.document, node_name);
        if (!item || !item->node.registered())
            return graph_error(String::concat("Target AssetNode '", lossy_utf8(node_name), "' is missing."));
        edit.node_name = item->name;
        ATELIER_TRY_BIND(asset_node_type, runtime_.native_type<procedural::AssetNode>());
        if (runtime_.type_of(item->node.id()) != *asset_node_type)
            return graph_error(String::concat("Node '", item->name, "' is not a procedural.AssetNode."));
        if (item->asset_key.empty())
            return graph_error(String::concat("AssetNode '", item->name, "' has no bound .procgraph asset."));
        ATELIER_TRY_BIND(target_doc, resolve_target_graph_document(item->asset_key, edit.edit_project));
        if (!*target_doc)
            return graph_error(String::concat("Unable to resolve .procgraph asset '", item->asset_key, "'."));
        const auto p_it = std::ranges::find((*target_doc)->parameters, param_name, &GraphPromotedParameter::name);
        if (p_it == (*target_doc)->parameters.end())
            return graph_error(String::concat("Unknown AssetNode parameter '", edit.param_name, "'."));
        ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, **target_doc, *p_it));

        edit.before_overrides = item->overrides;
        const json::Value ov_root = edit.before_overrides.root();
        ATELIER_TRY(populate_from_schema(*schema, ov_root.kind() == json::Kind::object ? &ov_root : nullptr));
    }

    graph_param_edit_ = std::move(edit);
    return {};
}

Result<void> Session::preview_graph_parameter_edit(std::uint64_t control_session, Span<const Scalar> values) {
    if (!graph_parameter_editing(control_session))
        return graph_error("No matching graph parameter edit.");
    auto& edit = *graph_param_edit_;

    Array<Scalar> next_scalars = edit.current_scalars;
    if (edit.lane) {
        if (values.size() != 1)
            return graph_error("Single-lane preview expects exactly 1 scalar.");
        const std::size_t li = *edit.lane;
        ATELIER_TRY_BIND(validated, validate_and_clamp_promoted_scalar(runtime_, edit.lane_fields[li].type,
                                                                       edit.lane_ranges[li], values.front()));
        next_scalars[li] = *validated;
    } else {
        if (values.size() != next_scalars.size())
            return graph_error("Parameter preview scalar count mismatch.");
        for (std::size_t i = 0; i < values.size(); ++i) {
            ATELIER_TRY_BIND(validated, validate_and_clamp_promoted_scalar(runtime_, edit.lane_fields[i].type,
                                                                           edit.lane_ranges[i], values[i]));
            next_scalars[i] = *validated;
        }
    }

    if (edit.generator_root.id() != ObjectId{}) {
        const ObjectId comp_id = edit.generator_root.id();
        ATELIER_TRY_BIND(comp_state, admit_component_edit(comp_id));
        if (comp_state->owner != edit.generator_owner)
            return graph_error("The procedural generator's owning Entity changed.");
        ATELIER_TRY_BIND(enc, encode_parameter_scalars_json(runtime_, edit.is_vector, edit.lane_fields, next_scalars));
        const json::Document cur_ov = generator_overrides(comp_id);
        json::Builder next_b(cur_ov.root().kind() == json::Kind::object ? cur_ov : json::Document{});
        next_b.set_validated(next_b.root(), edit.param_name, enc->root());
        const json::Document after_ov = next_b.finish();
        edit.current_scalars = std::move(next_scalars);
        if (after_ov == cur_ov)
            return {};
        generator_overrides_.insert_or_assign(comp_id, after_ov);
        ++revision_;
        forward_edit();
        return sync_procedural_generators(edit.edit_project);
    }

    ATELIER_TRY_BIND(open_ptr, editable_graph(edit.graph_key, true));
    auto& open = **open_ptr;

    if (edit.node_name.empty()) {
        const auto p_it = std::ranges::find(open.document.parameters, edit.param_name, &GraphPromotedParameter::name);
        if (p_it == open.document.parameters.end())
            return graph_error("Promoted parameter disappeared during edit.");
        ATELIER_TRY_BIND(schema, resolve_promoted_parameter_schema(runtime_, open.document, *p_it));
        for (std::size_t i = 0; i < next_scalars.size() && i < schema->ranges.size(); ++i)
            next_scalars[i] = detail::clamp_scalar(next_scalars[i], schema->ranges[i]).first;
        ATELIER_TRY(write_promoted_targets_if_changed(
            runtime_, *schema, std::variant<Array<Scalar>, String>{next_scalars}, &open.document));
        edit.current_scalars = std::move(next_scalars);
        ++open.mutation_epoch;
        open.dirty_revision = 0;
        ++revision_;
        return {};
    }

    auto* item = find_node(open.document, edit.node_name);
    if (!item)
        return graph_error("AssetNode disappeared during edit.");
    ATELIER_TRY_BIND(enc, encode_parameter_scalars_json(runtime_, edit.is_vector, edit.lane_fields, next_scalars));
    json::Builder next_b(item->overrides.root().kind() == json::Kind::object ? item->overrides : json::Document{});
    next_b.set_validated(next_b.root(), edit.param_name, enc->root());
    const json::Document after_ov = next_b.finish();
    edit.current_scalars = std::move(next_scalars);
    if (after_ov == item->overrides)
        return {};
    item->overrides = after_ov;
    ++open.mutation_epoch;
    open.dirty_revision = 0;
    ++revision_;
    return {};
}

Result<void> Session::commit_graph_parameter_edit(std::uint64_t control_session) {
    if (!graph_parameter_editing(control_session))
        return graph_error("No matching graph parameter edit.");
    auto edit = std::move(*graph_param_edit_);
    graph_param_edit_.reset();

    if (edit.generator_root.id() != ObjectId{}) {
        const ObjectId comp_id = edit.generator_root.id();
        ATELIER_TRY_BIND(comp_state, admit_component_edit(comp_id));
        if (comp_state->owner != edit.generator_owner)
            return graph_error("The procedural generator's owning Entity changed.");
        const json::Document after_ov = generator_overrides(comp_id);
        if (after_ov != edit.before_overrides) {
            ATELIER_TRY_BIND(proc_src, presentation::read_procedural_source(runtime_, comp_id));
            remember(ProceduralGraphChange{std::move(edit.generator_root), edit.generator_owner, proc_src->source,
                                           proc_src->source, std::move(edit.before_overrides), after_ov});
        }
        return {};
    }

    ATELIER_TRY_BIND(open_ptr, editable_graph(edit.graph_key));
    auto& open = **open_ptr;

    if (edit.node_name.empty()) {
        GraphParameterSchemaChange change{open.document.parameters, open.document.parameters, {}};
        bool any_changed = false;
        for (auto& bk : edit.node_backups) {
            ATELIER_TRY_BIND(after_p, capture_object_properties(runtime_, bk.target.id()));
            const auto* gnode = find_node_by_id(open.document, bk.target.id());
            json::Document after_k = gnode ? gnode->kept : json::Document{};
            if (bk.before_props != *after_p || bk.before_kept != after_k)
                any_changed = true;
            change.synced_nodes.add({std::move(bk.target), std::move(bk.before_props), std::move(*after_p),
                                     std::move(bk.before_kept), std::move(after_k)});
        }
        if (any_changed) {
            open.push_change(std::move(change));
            ++revision_;
        }
        return {};
    }

    auto* item = find_node(open.document, edit.node_name);
    if (item && item->overrides != edit.before_overrides) {
        open.push_change(GraphAssetOverrideChange{edit.node_name, std::move(edit.before_overrides), item->overrides,
                                                  item->asset_key, item->asset_key});
        ++revision_;
    }
    return {};
}

Result<void> Session::cancel_graph_parameter_edit() {
    if (!graph_param_edit_)
        return {};
    auto edit = std::move(*graph_param_edit_);
    graph_param_edit_.reset();

    if (edit.generator_root.id() != ObjectId{}) {
        const ObjectId comp_id = edit.generator_root.id();
        if (edit.before_overrides.root().kind() == json::Kind::object && edit.before_overrides.root().size() > 0)
            generator_overrides_.insert_or_assign(comp_id, std::move(edit.before_overrides));
        else
            generator_overrides_.remove(comp_id);
        ++revision_;
        forward_edit();
        static_cast<void>(sync_procedural_generators(edit.edit_project));
        return {};
    }

    auto* at = graphs_.find(edit.graph_key);
    if (!at || !*at)
        return {};
    auto& open = **at;
    if (edit.node_name.empty()) {
        for (const auto& bk : edit.node_backups) {
            ATELIER_TRY(apply_object_properties(runtime_, bk.target.id(), bk.before_props.root()));
            if (auto* gnode = find_node_by_id(open.document, bk.target.id()))
                gnode->kept = bk.before_kept;
        }
    } else if (auto* item = find_node(open.document, edit.node_name)) {
        item->overrides = std::move(edit.before_overrides);
    }
    ++open.mutation_epoch;
    open.dirty_revision = 0;
    ++revision_;
    return {};
}

Result<void> Session::set_promoted_parameter_scalars(const PromotedParameterTarget& target, StringView param_name,
                                                     Span<const Scalar> values, Project* project) {
    ATELIER_TRY(cancel_edit());
    constexpr std::uint64_t oneshot_session = std::numeric_limits<std::uint64_t>::max();
    ATELIER_TRY(begin_graph_parameter_edit(target.graph_key, target.node_name, param_name, {},
                                           oneshot_session, project, target.generator_component));
    auto changed = preview_graph_parameter_edit(oneshot_session, values);
    if (changed)
        changed = commit_graph_parameter_edit(oneshot_session);
    if (changed)
        return {};
    auto restored = cancel_graph_parameter_edit();
    return restored ? changed : restored;
}

Result<void> Session::set_promoted_parameter_string(const PromotedParameterTarget& target, StringView param_name,
                                                    StringView value, Project* project) {
    if (!value.is_valid_utf8())
        return graph_error("Parameter string must be valid UTF-8.");
    json::Builder b;
    b.set_validated(b.root(), value);
    if (target.generator_component != ObjectId{})
        return set_generator_override_json(project, target.generator_component, param_name, b.root());
    if (target.node_name.empty())
        return graph_set_parameter_json(target.graph_key, param_name, b.root());
    return graph_set_asset_override_json(target.graph_key, target.node_name, param_name, b.root(), project);
}

Result<void> Session::clear_promoted_parameter_override(const PromotedParameterTarget& target, StringView param_name,
                                                        Project* project) {
    if (target.generator_component != ObjectId{})
        return set_generator_override_json(project, target.generator_component, param_name, {});
    if (target.node_name.empty())
        return graph_error("Graph root parameters are authored defaults, not overrides.");
    return graph_set_asset_override_json(target.graph_key, target.node_name, param_name, {}, project);
}

Result<bool> Session::graph_apply(StringView key, json::Value patch, Project* project) {
    ATELIER_TRY_BIND(target, editable_graph(key, true));
    auto& open = **target;
    if (editing() || open.pending)
        return graph_error("Finish the active edit before applying a graph patch.");

    // Session edits are synchronous. Keep the undo/redo cursor and the retained history intact
    // until every delta has succeeded, including for patches longer than the history limit.
    const auto revision = revision_;
    Array<GraphChange> changes;
    open.pending = &changes;
    auto applied = apply_graph_patch(key, patch, project);
    open.pending = nullptr;
    revision_ = revision;
    if (!applied) {
        if (auto restored = open.replay(*this, changes, false); !restored)
            applied.error().message =
                String::concat(applied.error().message, " (rollback: ", restored.error().message, ")");
        return std::unexpected(std::move(applied).error());
    }
    if (changes.empty())
        return false;
    open.publish(std::move(changes));
    ++revision_;
    return true;
}

Result<json::Document> Session::graph_node_properties(StringView key, StringView name) {
    const auto id = graph_node_id(key, name);
    if (id == ObjectId{})
        return graph_error(String::concat("Missing or unresolved graph node: ", lossy_utf8(name)));
    return capture_object_properties(runtime_, id);
}

Result<void> Session::graph_set_properties(StringView key, StringView node_name, json::Value properties) {
    if (properties.kind() != json::Kind::object)
        return graph_error("Properties patch must be a JSON object.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    ObjectRoot target_root;
    GraphNode* target_node = nullptr;
    if (node_name.empty()) {
        target_root = open.document.graph;
    } else {
        target_node = find_node(open.document, node_name);
        if (!target_node || !target_node->node.registered())
            return graph_error(String::concat("Target node '", lossy_utf8(node_name), "' is missing or unresolved."));
        target_root = target_node->node;

        Array<String> check_path;
        const auto check_controlled = [&](auto&& self, json::Value patch) -> Result<void> {
            for (const auto& [member, val] : *patch.object_items()) {
                check_path.add(String::from_utf8_validated(member));
                if (val.kind() == json::Kind::object && val.size() > 0) {
                    ATELIER_TRY(self(self, val));
                } else {
                    const String dotted = detail::dotted(check_path);
                    if (const auto ctrl = graph_controlling_parameter(key, target_node->name, dotted)) {
                        return graph_error(String::concat(
                            "Field '", target_node->name, ".", dotted, "' is controlled by promoted graph parameter '",
                            *ctrl, "'. Use Set Graph Parameter with parameter='", *ctrl,
                            "' to update all its bound targets, or unbind this field first."));
                    }
                }
                check_path.pop();
            }
            return {};
        };
        ATELIER_TRY(check_controlled(check_controlled, properties));
    }

    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(before, capture_object_properties(runtime_, target_root.id()));
    json::Document after;
    ATELIER_TRY(patch_object_properties(runtime_, target_root.id(), properties, no_graph_references, &after));

    const auto before_kept = target_node ? target_node->kept : open.document.kept;
    json::Builder next_kept(before_kept);
    Array<String> path;
    if (target_node)
        path.add("properties"_s);
    const auto clear_edited = [&](auto&& self, json::Value patch) -> void {
        const auto members = *patch.object_items();
        for (const auto& [member, value] : members) {
            path.add(String::from_utf8_validated(member));
            if (value.kind() == json::Kind::object && value.size() > 0)
                self(self, value);
            else
                detail::erase_path(next_kept, next_kept.root(), path);
            path.pop();
        }
    };
    clear_edited(clear_edited, properties);
    const auto after_kept = next_kept.finish();

    const auto before_bytes = json::write(before->root());
    const auto after_bytes = json::write(after.root());
    if (std::ranges::equal(before_bytes, after_bytes) && before_kept == after_kept)
        return {};

    if (target_node)
        target_node->kept = after_kept;
    else
        open.document.kept = after_kept;
    open.push_change(
        GraphPropertyChange{std::move(target_root), std::move(*before), std::move(after), before_kept, after_kept});
    ++revision_;
    return {};
}

Result<void> Session::set_string_property(ObjectId object, Span<const FieldId> path, StringView value) {
    if (world_.running() || world_.closed())
        return graph_error("Properties require an open, stopped document.", ErrorCode::invalid_definition);
    const auto graph_key = graph_of_object(object);
    if (!graph_key)
        return graph_error("String property editing requires a procedural graph object.",
                           ErrorCode::invalid_definition);
    if (!value.is_valid_utf8())
        return graph_error("Property string must be valid UTF-8.");
    ATELIER_TRY(reject_if_graph_controlled_field(object, path));
    ATELIER_TRY_BIND(string_type, runtime_.native_type<String>());
    ATELIER_TRY_BIND(type, runtime_.type_of(object));
    Array<String> names;
    for (const auto step : path) {
        ATELIER_TRY_BIND(info, runtime_.describe(*type));
        const auto field = std::ranges::find((*info)->fields, step, &FieldInfo::id);
        if (field == (*info)->fields.end())
            return graph_error("Unknown property field.");
        if (field->read_only)
            return graph_error("Read-only fields cannot be edited in the inspector.", ErrorCode::invalid_definition);
        names.add(field->name);
        type = field->value.type;
    }
    if (*type != *string_type)
        return graph_error("Target field is not a String.", ErrorCode::type_mismatch);

    json::Builder patch;
    const auto leaf = patch.object_path_validated(patch.root(), names);
    patch.set_validated(leaf, value);
    const auto node_name = graph_node_name(object);
    return graph_set_properties(*graph_key, node_name ? node_name->view() : StringView{}, patch.root());
}

Result<procedural::Evaluator*> Session::prepare_graph_evaluator(OpenGraph& open, Project* project) {
    ATELIER_TRY_BIND(program, procedural_source_program());
    if (project)
        open.resources.bind(project);
    ATELIER_TRY(sync_open_graph_assets(open, project ? project : open.resources.project(), true));
    open.shallow_ports_revision = revision_;
    if (!open.evaluator || open.bound_program != *program) {
        open.bound_program = *program;
        open.evaluator = std::make_unique<procedural::Evaluator>(runtime_, open.document.graph, open.resources,
                                                                 *program, ensure_procedural_cache_pool());
    }
    return open.evaluator.get();
}

Result<std::shared_ptr<const procedural::Bvh>> Session::graph_node_surface(StringView key, StringView name,
                                                                           StringView output, Project* project) {
    const auto id = graph_node_id(key, name);
    if (id == ObjectId{})
        return graph_error(String::concat("Missing or unresolved graph node: ", lossy_utf8(name)));
    auto& open = **graphs_.find(key);
    ATELIER_TRY_BIND(evaluator, prepare_graph_evaluator(open, project));
    ATELIER_TRY_BIND(ports, procedural::ports(runtime_, id));
    const auto port = std::ranges::find(ports->outputs, output);
    if (port == ports->outputs.end())
        return graph_error(
            String::concat("Unknown output port on ", lossy_utf8(name), ": ", lossy_utf8(output)));
    ATELIER_TRY((*evaluator)->evaluate_node(id));
    if (auto cause = (*evaluator)->target_error(id))
        return std::unexpected(std::move(*cause));
    const auto* geometry = (*evaluator)->node_output(id, static_cast<std::uint32_t>(port - ports->outputs.begin()));
    if (!geometry)
        return graph_error(String::concat("The evaluated node has no geometry on output ", lossy_utf8(output)));
    if (geometry->count(procedural::Layer::instance))
        return graph_error("Raycast requires polygon geometry; realize instances before querying this output.");
    return procedural::Bvh::acquire(*geometry);
}

Result<procedural::NodeBindings> Session::graph_node_bindings(StringView key, StringView name, Project* project) {
    const auto id = graph_node_id(key, name);
    if (id == ObjectId{})
        return graph_error(String::concat("Missing or unresolved graph node: ", lossy_utf8(name)));
    auto& open = **graphs_.find(key);
    ATELIER_TRY_BIND(evaluator, prepare_graph_evaluator(open, project));
    return (*evaluator)->inspect_bindings(id);
}

Result<GraphEvaluation> Session::evaluate_graph(StringView key, Project* project, ObjectId inspected_node,
                                                bool record_timings) {
    if (world_.closed())
        return graph_error("Graph evaluation requires an open Session.", ErrorCode::invalid_definition);
    auto* at = graphs_.find(key);
    if (!at || !*at)
        return graph_error(String::concat("Procedural graph is not open: ", lossy_utf8(key)),
                           ErrorCode::invalid_definition);
    auto& open = **at;
    ATELIER_TRY(prepare_graph_evaluator(open, project));
    ATELIER_TRY(open.evaluator->evaluate(record_timings));

    GraphEvaluation result;
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, open.document.graph.id()));
    result.display_node = listed->display();
    if (result.display_node != ObjectId{}) {
        if (const auto* disp = find_node_by_id(open.document, result.display_node))
            result.display_name = disp->name;
    }
    result.display_output = open.evaluator->output();
    result.preview_output = open.evaluator->retain_last_valid_output();
    if (result.display_node != ObjectId{}) {
        if (const auto* st = open.evaluator->status(result.display_node); st && st->error) {
            result.display_error = st->error;
            result.display_error_node =
                resolve_root_error_node(open.document, *st, result.display_node, result.display_name);
        }
    }

    if (open.document.template_node != ObjectId{} && listed->contains(open.document.template_node)) {
        result.template_node = open.document.template_node;
        if (const auto* tmpl = find_node_by_id(open.document, result.template_node))
            result.template_name = tmpl->name;
        if (result.template_node != result.display_node)
            ATELIER_TRY(open.evaluator->evaluate_node(result.template_node, record_timings));
        result.template_output = open.evaluator->node_output(result.template_node);
    }

    if (inspected_node != ObjectId{} && listed->contains(inspected_node)) {
        if (inspected_node != result.display_node && inspected_node != result.template_node)
            ATELIER_TRY(open.evaluator->evaluate_node(inspected_node, record_timings));
        result.inspected_output = open.evaluator->node_output(inspected_node);
        if (const auto* st = open.evaluator->status(inspected_node); st && st->error) {
            result.inspected_error = st->error;
            result.inspected_error_node = resolve_root_error_node(open.document, *st, inspected_node);
        }
    }
    std::uint64_t gen = 14695981039346656037ULL;
    const auto mix = [&](std::uint64_t v) {
        gen ^= v;
        gen *= 1099511628211ULL;
    };
    mix(revision_);
    mix(open.resources.revision());
    mix(project_code_ && project_code_->report() ? project_code_->report()->published_version : 0);
    for (const auto& item : open.document.nodes)
        if (item.node.registered())
            if (const auto* st = open.evaluator->status(item.node.id())) {
                mix(st->evaluations);
                mix(st->cache_hits);
            }
    open.evaluation_generation = gen;
    result.generation = gen;
    return result;
}

Result<std::shared_ptr<const GraphInspectView>> Session::graph_topology(StringView key) {
    auto* at = graphs_.find(key);
    if (!at)
        return graph_error("Procedural graph is not open.");
    auto& open = **at;
    if (open.topology && open.topology_revision == revision_ && open.topology_generation == open.evaluation_generation)
        return open.topology;
    if (open.shallow_ports_revision != revision_) {
        ATELIER_TRY(sync_open_graph_assets(open, open.resources.project(), false));
        open.shallow_ports_revision = revision_;
    }
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(graph_type, runtime_.native_type<procedural::Graph>());
    ATELIER_TRY_BIND(seed_field, runtime_.find_field(*graph_type, "seed"));
    ATELIER_TRY_BIND(seed_val, runtime_.read(graph, *seed_field));

    auto snapshot = std::make_shared<GraphInspectView>();
    auto& view = *snapshot;
    view.key = open.key;
    view.graph = graph;
    view.seed = std::get<std::uint32_t>(*seed_val);
    view.can_undo = graph_can_undo(key, false);
    view.can_redo = graph_can_undo(key, true);
    if (auto ext_p = extract_graph_ports(runtime_, open.document))
        view.external_ports = {std::move(ext_p->inputs), std::move(ext_p->multiple), std::move(ext_p->outputs)};
    view.parameters = open.document.parameters;
    view.issues = open.document.issues;
    view.issues.append(open.asset_issues);
    view.notes = open.document.notes;
    view.boxes = open.document.boxes;

    if (listed->display() != ObjectId{}) {
        if (const auto* disp_node = find_node_by_id(open.document, listed->display()))
            view.display = disp_node->name;
    } else if (const auto kept_disp = open.document.kept.root().string({"display"})) {
        view.display = String::from_utf8_validated(*kept_disp);
    }
    if (open.document.template_node != ObjectId{}) {
        if (const auto* tmpl_node = find_node_by_id(open.document, open.document.template_node))
            view.template_node = tmpl_node->name;
    } else if (const auto kept_tmpl = open.document.kept.root().string({"template"})) {
        view.template_node = String::from_utf8_validated(*kept_tmpl);
    }

    Project* active_project = open.resources.project();
    ObjectId cached_error_cause{};
    StringView cached_error_name{};
    for (const auto& item : open.document.nodes) {
        GraphNodeView node_view;
        node_view.name = item.name;
        node_view.asset_key = item.asset_key;
        if (!item.asset_key.empty() && active_project) {
            if (const auto* entry = active_project->entry(item.asset_key))
                node_view.asset_label = platform::utf16_to_utf8(entry->path.stem().wstring()).value_or(String{});
        }
        node_view.overrides = item.overrides;
        node_view.position = item.position;
        node_view.shape = item.shape;
        node_view.color = item.color;
        node_view.comment = item.comment;
        if (!item.node.registered()) {
            node_view.placeholder = true;
            node_view.is_display = item.name == view.display;
            node_view.is_template = item.name == view.template_node;
            if (const auto saved_type = item.kept.root().string({"type"}))
                node_view.type = String::from_utf8_validated(*saved_type);
            if (const auto saved_bypass = item.kept.root().boolean({"bypass"}))
                node_view.is_bypassed = *saved_bypass;
            view.nodes.add(std::move(node_view));
            continue;
        }
        node_view.id = item.node.id();
        node_view.is_display = item.node.id() == listed->display();
        node_view.is_template = item.node.id() == open.document.template_node;
        ATELIER_TRY_BIND(is_bypassed, procedural::bypassed(runtime_, item.node.id()));
        node_view.is_bypassed = *is_bypassed;
        ATELIER_TRY_BIND(is_frozen, procedural::frozen(runtime_, item.node.id()));
        node_view.is_frozen = *is_frozen;
        ATELIER_TRY_BIND(type, runtime_.type_of(item.node.id()));
        ATELIER_TRY_BIND(info, runtime_.describe(*type));
        node_view.type = (*info)->name;
        ATELIER_TRY_BIND(ports, procedural::ports(runtime_, item.node.id()));
        node_view.ports = {std::move(ports->inputs), std::move(ports->multiple), std::move(ports->outputs)};
        if (open.evaluator) {
            if (const auto* st = open.evaluator->status(item.node.id())) {
                node_view.status = *st;
                if (st->error) {
                    const ObjectId cause = st->root_node != ObjectId{} ? st->root_node : item.node.id();
                    if (cause == item.node.id()) {
                        cached_error_cause = cause;
                        cached_error_name = item.name.view();
                        node_view.root_error_node = item.name;
                    } else if (cause == cached_error_cause && !cached_error_name.empty()) {
                        node_view.root_error_node = String::from_utf8_validated(cached_error_name);
                    } else if (const auto* cause_node = find_node_by_id(open.document, cause)) {
                        cached_error_cause = cause;
                        cached_error_name = cause_node->name.view();
                        node_view.root_error_node = cause_node->name;
                    } else {
                        node_view.root_error_node = item.name;
                    }
                }
            }
        }

        ATELIER_TRY_BIND(conns, procedural::inputs(runtime_, *listed, item.node.id()));
        for (const auto& conn : *conns) {
            const auto* src = find_node_by_id(open.document, conn.from);
            if (!src)
                continue;
            ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, conn.from));
            const String from_port = conn.output < src_ports->outputs.size() ? src_ports->outputs[conn.output]
                                                                             : String::from_number(conn.output);
            const String to_port = conn.input < node_view.ports.inputs.size() ? node_view.ports.inputs[conn.input]
                                                                              : String::from_number(conn.input);
            view.connections.add({src->name, from_port, conn.output, item.name, to_port, conn.input});
        }
        view.nodes.add(std::move(node_view));
    }

    open.topology_revision = revision_;
    open.topology_generation = open.evaluation_generation;
    open.topology = std::move(snapshot);
    return open.topology;
}

Result<GraphInspectView> Session::inspect_graph(StringView key, Project* project, ObjectId inspected_node,
                                                bool record_timings, std::size_t max_samples, bool include_spatial,
                                                ObjectId reference_node, StringView reference_name) {
    ATELIER_TRY_BIND(eval, evaluate_graph(key, project, inspected_node, record_timings));
    std::shared_ptr<const procedural::Bvh> ref_bvh;
    if (include_spatial && reference_node != ObjectId{}) {
        auto* at = graphs_.find(key);
        if (!at || !*at || !(*at)->evaluator)
            return graph_error("Procedural graph evaluator is unavailable.");
        auto& open = **at;
        ATELIER_TRY(open.evaluator->evaluate_node(reference_node, false));
        if (const auto* st = open.evaluator->status(reference_node); st && st->error)
            return graph_error(
                String::concat("Reference node '", lossy_utf8(reference_name), "' failed: ", st->error->message));
        const auto* ref_geom = open.evaluator->node_output(reference_node, 0);
        if (!ref_geom)
            return graph_error(
                String::concat("Reference node '", lossy_utf8(reference_name), "' produced no geometry output."));
        auto acquired = procedural::Bvh::acquire(*ref_geom);
        if (!acquired)
            return graph_error(String::concat("Reference node '", lossy_utf8(reference_name),
                                              "' has no valid polygon surface: ", acquired.error().message));
        ref_bvh = std::move(*acquired);
    }
    ATELIER_TRY_BIND(topology, graph_topology(key));
    GraphInspectView view = **topology;
    view.display_error = eval->display_error;
    view.display_error_node = eval->display_error_node;
    view.inspected_error = eval->inspected_error;
    view.inspected_error_node = eval->inspected_error_node;
    view.display_summary =
        summarize_geometry_impl(eval->display_output, max_samples, include_spatial, ref_bvh.get(), reference_name);
    if (inspected_node != ObjectId{} &&
        std::ranges::find(view.nodes, inspected_node, &GraphNodeView::id) != view.nodes.end()) {
        view.inspected_summary = inspected_node == eval->display_node
                                     ? view.display_summary
                                     : summarize_geometry_impl(eval->inspected_output, max_samples, include_spatial,
                                                               ref_bvh.get(), reference_name);
    }
    return view;
}

namespace {

std::wstring truncate_utf16(std::wstring_view text, std::size_t max_units) {
    if (text.size() <= max_units)
        return std::wstring(text);
    auto out = std::wstring(text.substr(0, max_units));
    if (!out.empty() && out.back() >= 0xd800 && out.back() <= 0xdbff)
        out.pop_back();
    return out;
}

Result<world::Trs> decompose_procedural_instance(const std::array<float, 12>& affine) {
    const math::Matrix4 mat = presentation::procedural_affine_matrix(affine);
    if (auto exact = world::relative_transform(mat, math::Matrix4::identity()))
        return exact;

    for (const float v : affine)
        if (!std::isfinite(v))
            return graph_error("Procedural instance transform contains non-finite values.",
                               ErrorCode::invalid_definition);

    const math::Vector3d row0{affine[0], affine[1], affine[2]};
    const math::Vector3d row1{affine[3], affine[4], affine[5]};
    const math::Vector3d row2{affine[6], affine[7], affine[8]};
    const math::Vector3 pos{affine[9], affine[10], affine[11]};

    double sx = std::hypot(row0.x, row0.y, row0.z);
    double sy = std::hypot(row1.x, row1.y, row1.z);
    double sz = std::hypot(row2.x, row2.y, row2.z);
    const auto valid_scale = [](double s) {
        const float fs = static_cast<float>(s);
        return std::isfinite(s) && s > 0. && std::isfinite(fs) && fs != 0.f;
    };
    if (!valid_scale(sx) || !valid_scale(sy) || !valid_scale(sz))
        return graph_error("Procedural instance transform has a degenerate scale.", ErrorCode::invalid_definition);

    const double det = row0.x * (row1.y * row2.z - row1.z * row2.y) - row0.y * (row1.x * row2.z - row1.z * row2.x) +
                       row0.z * (row1.x * row2.y - row1.y * row2.x);
    if (!std::isfinite(det) || det == 0.)
        return graph_error("Procedural instance transform is singular.", ErrorCode::invalid_definition);
    if (det < 0.)
        sx = -sx;

    const math::Vector3d u0 = row0 * (1. / sx);
    const math::Vector3d u1 = row1 * (1. / sy);
    const math::Vector3d u2 = row2 * (1. / sz);
    constexpr double shear_limit = 1e-4;
    if (std::abs(math::dot(u0, u1)) > shear_limit || std::abs(math::dot(u0, u2)) > shear_limit ||
        std::abs(math::dot(u1, u2)) > shear_limit) {
        return graph_error(
            "Procedural instance transform contains shear, which StaticMeshEntity TRS cannot represent; realize instances in the graph before baking.",
            ErrorCode::invalid_definition);
    }

    const math::Vector3d r0 = u0;
    const auto r1_opt = math::try_normalize(u1 - r0 * math::dot(u1, r0));
    if (!r1_opt)
        return graph_error("Procedural instance basis is collinear.", ErrorCode::invalid_definition);
    const math::Vector3d r1 = *r1_opt;
    const math::Vector3d r2 = math::cross(r0, r1);

    math::Quaterniond qd{};
    const double trace = r0.x + r1.y + r2.z;
    if (trace > 0.) {
        const double s = 2. * std::sqrt(1. + trace);
        qd.w = 0.25 * s;
        qd.x = (r1.z - r2.y) / s;
        qd.y = (r2.x - r0.z) / s;
        qd.z = (r0.y - r1.x) / s;
    } else if (r0.x > r1.y && r0.x > r2.z) {
        const double s = 2. * std::sqrt(std::max(0., 1. + r0.x - r1.y - r2.z));
        qd.x = 0.25 * s;
        qd.w = (r1.z - r2.y) / s;
        qd.y = (r0.y + r1.x) / s;
        qd.z = (r0.z + r2.x) / s;
    } else if (r1.y > r2.z) {
        const double s = 2. * std::sqrt(std::max(0., 1. + r1.y - r0.x - r2.z));
        qd.y = 0.25 * s;
        qd.w = (r2.x - r0.z) / s;
        qd.x = (r0.y + r1.x) / s;
        qd.z = (r1.z + r2.y) / s;
    } else {
        const double s = 2. * std::sqrt(std::max(0., 1. + r2.z - r0.x - r1.y));
        qd.z = 0.25 * s;
        qd.w = (r0.y - r1.x) / s;
        qd.x = (r0.z + r2.x) / s;
        qd.y = (r1.z + r2.y) / s;
    }
    return world::canonical_transform({pos,
                                       static_cast<math::Quaternion>(qd),
                                       {static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sz)}});
}

} // namespace

Result<void> Session::sync_procedural_generators(Project* project) {
    if (world_.closed())
        return {};
    const std::uint64_t proj_rev = project ? project->revision() : 0ULL;
    const std::uint64_t code_version =
        project_code_ && project_code_->report() ? project_code_->report()->published_version : 0ULL;

    if (generator_cache_ && generator_cache_->idle_valid && !generator_cache_->has_errors &&
        generator_cache_->idle_world_revision == world_.revision() &&
        generator_cache_->idle_session_revision == revision_ && generator_cache_->idle_project == project &&
        generator_cache_->idle_project_revision == proj_rev && generator_cache_->idle_code_version == code_version) {
        runtime_.changed_objects(generator_cache_->idle_runtime_cursor, generator_cache_->scratch_changed);
        if (generator_cache_->scratch_changed.empty())
            return {};
    }

    ATELIER_TRY_BIND(generators, world_.query(presentation_types_.procedural_generator_component));
    if (generators->empty()) {
        if (generator_cache_) {
            generator_cache_->clear();
            if (auto now = runtime_.change_cursor()) {
                generator_cache_->idle_valid = true;
                generator_cache_->idle_world_revision = world_.revision();
                generator_cache_->idle_session_revision = revision_;
                generator_cache_->idle_project = project;
                generator_cache_->idle_project_revision = proj_rev;
                generator_cache_->idle_code_version = code_version;
                generator_cache_->idle_runtime_cursor = *now;
            }
        }
        return {};
    }

    if (!generator_cache_)
        generator_cache_.reset(new GeneratorRuntimeCache());
    auto& cache = *generator_cache_;
    cache.idle_valid = false;
    cache.has_errors = false;
    if (project)
        cache.resources.bind(project);

    Set<ObjectId> live_components(*generators);
    cache.entries.remove_if([&](const ObjectId id, const auto&) { return !live_components.contains(id); });

    const auto contain_error = [&](const Error& err, ObjectId component, ObjectId owner,
                                   Array<String> field = {"graph"_s},
                                   world::DiagnosticKind kind = world::DiagnosticKind::invalid_value) -> Result<void> {
        cache.has_errors = true;
        cache.entries.at(component).latest_cook.emplace(std::unexpected(err));
        world::Diagnostic diag;
        diag.kind = kind;
        diag.phase = world::DiagnosticPhase::presentation;
        diag.object = component;
        diag.entity = owner;
        diag.field = std::move(field);
        if (!world_.contain(err, std::move(diag)))
            return std::unexpected(err);
        return {};
    };

    auto program_res = procedural_source_program();
    assets::Reader builtin_reader;
    assets::Reader& active_reader =
        project ? project->assets() : (cache.fallback_reader ? *cache.fallback_reader : builtin_reader);
    Set<String> referenced_graphs;

    for (const ObjectId component : *generators) {
        ATELIER_TRY_BIND(comp_state, world::inspect_component(runtime_, component));
        if (!comp_state->valid || comp_state->owner == ObjectId{} || !world_.contains(comp_state->owner)) {
            cache.entries.remove(component);
            continue;
        }
        ATELIER_TRY_BIND(proc_source, presentation::read_procedural_source(runtime_, component));
        if (proc_source->source.empty()) {
            cache.entries.remove(component);
            if (proc_source->output)
                ATELIER_TRY(presentation::set_procedural_binding(runtime_, component, {{}, nullptr, {}}));
            continue;
        }

        auto& entry = *cache.entries.try_emplace(component).value;
        entry.latest_cook.reset();
        ATELIER_TRY_BIND(owner_enabled, world::effective_enabled(runtime_, comp_state->owner));
        if (!comp_state->enabled || !*owner_enabled) {
            entry.output.reset();
            entry.resolved_assets.clear();
            if (proc_source->output)
                ATELIER_TRY(
                    presentation::set_procedural_binding(runtime_, component, {proc_source->source, nullptr, {}}));
            continue;
        }

        referenced_graphs.add(proc_source->source);
        if (!program_res) {
            ATELIER_TRY(contain_error(program_res.error(), component, comp_state->owner));
            continue;
        }
        const source::Program* program = *program_res;

        bool override_seed = false;
        std::int32_t seed = 0;
        float chunk_size = 32.f;
        ATELIER_TRY(runtime_.with_native<const presentation::ProceduralGeneratorComponent>(
            component, [&](const presentation::ProceduralGeneratorComponent& value) -> Result<void> {
                override_seed = value.override_seed;
                seed = value.seed;
                chunk_size = value.chunk_size;
                return {};
            }));
        if (!std::isfinite(chunk_size) || chunk_size < 0.1f || chunk_size > 100000.f) {
            ATELIER_TRY(contain_error(
                Error{ErrorCode::argument_mismatch, "Generator chunk size must stay within [0.1, 100000] metres."},
                component, comp_state->owner, {"chunk_size"_s}));
            continue;
        }

        if (entry.graph_key != proc_source->source) {
            entry.graph_key = proc_source->source;
            entry.output.reset();
            entry.resolved_assets.clear();
        }

        const auto drop_component_output = [&]() -> Result<void> {
            entry.output.reset();
            entry.resolved_assets.clear();
            if (proc_source->output)
                return presentation::set_procedural_binding(runtime_, component, {proc_source->source, nullptr, {}});
            return {};
        };

        const auto find_open = [&](StringView k) -> std::pair<const GraphDocument*, std::uint64_t> {
            if (const auto* it = graphs_.find(k); it && *it)
                return {&(*it)->document, (*it)->mutation_epoch};
            return {nullptr, 0ULL};
        };
        const auto resolver = make_asset_port_resolver(project);

        const json::Document comp_ov = generator_overrides(component);
        const bool has_ov = comp_ov.root().kind() == json::Kind::object && comp_ov.root().size() > 0;
        Array<world::Diagnostic> instance_issues;
        const auto* effective_issues = &instance_issues;
        auto& slot = *cache.graph_slots.try_emplace(proc_source->source).value;
        slot.owner_runtime = &runtime_;
        ObjectRoot source_graph_root;
        const GraphDocument* source_doc = nullptr;
        std::uint64_t src_epoch = 0;
        if (auto* open_it = graphs_.find(proc_source->source); open_it && *open_it) {
            slot.reset();
            slot.owner_runtime = &runtime_;
            ATELIER_TRY(sync_open_graph_assets(**open_it, project, true));
            (*open_it)->shallow_ports_revision = revision_;
            if (!has_ov)
                effective_issues = &(*open_it)->asset_issues;
            source_doc = &(*open_it)->document;
            source_graph_root = (*open_it)->document.graph;
            src_epoch = (1ULL << 61) ^ (*open_it)->mutation_epoch;
        } else {
            if (!slot.closed_doc || (project && slot.closed_project_revision != proj_rev)) {
                if (!project) {
                    if (!slot.closed_doc)
                        continue;
                } else {
                    auto loaded_asset = project->assets().procedural_graph(proc_source->source);
                    if (!loaded_asset) {
                        slot.reset();
                        slot.owner_runtime = &runtime_;
                        const auto& miss = remember_unavailable(project->assets(), assets::AssetKind::procedural_graph,
                                                                proc_source->source, loaded_asset.error());
                        ATELIER_TRY(drop_component_output());
                        ATELIER_TRY(contain_error(miss.error, component, comp_state->owner, {"graph"_s}, miss.kind));
                        continue;
                    }
                    auto parsed_doc = load_graph(runtime_, loaded_asset->text, resolver);
                    if (!parsed_doc) {
                        slot.reset();
                        slot.owner_runtime = &runtime_;
                        const Error format_err{ErrorCode::invalid_storage, parsed_doc.error().message};
                        const auto& miss = remember_unavailable(project->assets(), assets::AssetKind::procedural_graph,
                                                                proc_source->source, format_err);
                        ATELIER_TRY(drop_component_output());
                        ATELIER_TRY(contain_error(miss.error, component, comp_state->owner, {"graph"_s}, miss.kind));
                        continue;
                    }
                    unavailable_.remove(std::pair{assets::AssetKind::procedural_graph, proc_source->source});
                    slot.reset();
                    slot.owner_runtime = &runtime_;
                    slot.closed_doc = std::move(*parsed_doc);
                    slot.closed_project_revision = proj_rev;
                }
            }
            if ((project || !graphs_.empty()) && slot.closed_doc) {
                auto synced_sub = sync_graph_document_assets_impl(
                    runtime_, *slot.closed_doc, slot.asset_instances, project, code_version, resolver, find_open,
                    proc_source->source, true, true, has_ov ? nullptr : &instance_issues);
                if (!synced_sub) {
                    ATELIER_TRY(contain_error(synced_sub.error(), component, comp_state->owner));
                    continue;
                }
            }
            source_doc = &*slot.closed_doc;
            source_graph_root = slot.closed_doc->graph;
            src_epoch = (2ULL << 61) ^ slot.closed_project_revision;
        }

        const GraphDocument* target_doc = source_doc;
        procedural::Evaluator* target_evaluator = nullptr;

        if (!has_ov) {
            entry.private_instance.reset();
            if (!slot.evaluator || slot.bound_graph_id != source_graph_root.id() || slot.bound_program != program ||
                slot.code_version != code_version) {
                slot.evaluator = std::make_unique<procedural::Evaluator>(runtime_, source_graph_root, cache.resources,
                                                                         program, ensure_procedural_cache_pool());
                slot.bound_graph_id = source_graph_root.id();
                slot.bound_program = program;
                slot.code_version = code_version;
            }
            target_evaluator = slot.evaluator.get();
        } else {
            const bool need_clone = !entry.private_instance ||
                                    entry.private_instance->graph_key != proc_source->source ||
                                    entry.private_instance->source_epoch != src_epoch ||
                                    entry.private_instance->code_version != code_version;
            if (need_clone) {
                auto cloned = clone_graph_document(runtime_, *source_doc);
                if (!cloned) {
                    ATELIER_TRY(contain_error(cloned.error(), component, comp_state->owner));
                    continue;
                }
                auto fresh = std::make_unique<GeneratorRuntimeCache::PrivateInstance>();
                fresh->owner_runtime = &runtime_;
                fresh->graph_key = proc_source->source;
                fresh->source_epoch = src_epoch;
                fresh->code_version = code_version;
                fresh->cloned_doc = std::move(*cloned);
                fresh->cached_params = build_cached_promoted_params(runtime_, fresh->cloned_doc);
                entry.private_instance = std::move(fresh);
            }

            if (!entry.private_instance->overrides_initialized ||
                entry.private_instance->applied_overrides != comp_ov) {
                auto applied = apply_cached_instance_overrides(
                    runtime_, entry.private_instance->cached_params, proc_source->source, comp_ov,
                    [&](const String& ov_name, json::Value ov_val, world::DiagnosticKind dkind,
                        const String& detail_msg) {
                        world::Diagnostic diag;
                        diag.severity = world::Severity::warning;
                        diag.kind = dkind;
                        diag.phase = world::DiagnosticPhase::presentation;
                        diag.object = component;
                        diag.entity = comp_state->owner;
                        diag.field = {"overrides"_s, ov_name};
                        diag.value = assets::json_format::preview(ov_val);
                        diag.message = String::concat("overrides.", ov_name, ": ", detail_msg);
                        world_.report(std::move(diag));
                    });
                if (!applied) {
                    ATELIER_TRY(contain_error(applied.error(), component, comp_state->owner, {"overrides"_s}));
                    continue;
                }
                entry.private_instance->applied_overrides = comp_ov;
                entry.private_instance->overrides_initialized = true;
            }

            if (project || !graphs_.empty()) {
                auto synced_sub = sync_graph_document_assets_impl(
                    runtime_, entry.private_instance->cloned_doc, entry.private_instance->asset_instances, project,
                    code_version, resolver, find_open, proc_source->source, true, true, &instance_issues);
                if (!synced_sub) {
                    ATELIER_TRY(contain_error(synced_sub.error(), component, comp_state->owner));
                    continue;
                }
            }
            if (!entry.private_instance->evaluator || entry.private_instance->bound_program != program) {
                entry.private_instance->evaluator =
                    std::make_unique<procedural::Evaluator>(runtime_, entry.private_instance->cloned_doc.graph,
                                                            cache.resources, program, ensure_procedural_cache_pool());
                entry.private_instance->bound_program = program;
            }
            target_doc = &entry.private_instance->cloned_doc;
            target_evaluator = entry.private_instance->evaluator.get();
        }

        Array<ObjectId> out_nodes;
        const std::array prefix_fields{"graph"_s, proc_source->source};
        for (auto issue : *effective_issues) {
            issue.phase = world::DiagnosticPhase::presentation;
            issue.object = component;
            issue.entity = comp_state->owner;
            issue.type.clear();
            issue.field.insert_range(0, prefix_fields);
            issue.message = String::concat("graph.", proc_source->source, ".", issue.message);
            world_.report(std::move(issue));
        }
        auto ext_ports = extract_graph_ports(runtime_, *target_doc, nullptr, &out_nodes);
        if (!ext_ports) {
            if (!entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(ext_ports.error(), component, comp_state->owner));
            continue;
        }
        if (out_nodes.empty()) {
            ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(
                Error{ErrorCode::invalid_definition, "The procedural graph has no GraphOutput or display node."},
                component, comp_state->owner));
            continue;
        }
        const ObjectId target_out = out_nodes.front();

        const Optional<std::uint32_t> seed_override =
            override_seed ? Optional<std::uint32_t>{static_cast<std::uint32_t>(seed)} : Optional<std::uint32_t>{};
        if (auto evaluated = target_evaluator->evaluate_targets({&target_out, 1}, false, seed_override); !evaluated) {
            if (!entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(evaluated.error(), component, comp_state->owner));
            continue;
        }

        if (auto node_err = target_evaluator->target_error(target_out)) {
            if (!entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(*node_err, component, comp_state->owner));
            continue;
        }

        const auto retained_geom = target_evaluator->retain_node_output(target_out, 0);
        if (!retained_geom) {
            ATELIER_TRY(drop_component_output());
            continue;
        }

        const bool same_geom = entry.output && entry.output->geometry == retained_geom;
        const bool same_chunk =
            std::bit_cast<std::uint32_t>(entry.chunk_size) == std::bit_cast<std::uint32_t>(chunk_size);
        if (same_geom && same_chunk) {
            if (proc_source->output != entry.output) {
                if (auto rebound = presentation::set_procedural_binding(
                        runtime_, component, {proc_source->source, entry.output, entry.resolved_assets});
                    !rebound) {
                    ATELIER_TRY(contain_error(rebound.error(), component, comp_state->owner));
                    continue;
                }
            }
            entry.latest_cook.emplace();
            continue;
        }

        const procedural::Publication* prev_pub = entry.output ? &entry.output->publication : nullptr;
        auto published = procedural::publish(*retained_geom, chunk_size, prev_pub);
        if (!published) {
            if (!entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(published.error(), component, comp_state->owner));
            continue;
        }

        geometry::MeshRef surface_mesh =
            same_geom ? entry.output->surface_mesh : presentation::build_procedural_surface_mesh(*retained_geom);

        Array<presentation::ProceduralMeshDependency> mesh_dependencies;
        for (const auto& dep : target_evaluator->target_dependencies({&target_out, 1})) {
            const String& dep_key = dep.key;
            if (dep_key.empty())
                continue;
            if (auto mesh_asset = cache.resources.resolve_mesh_asset(dep_key); mesh_asset && mesh_asset->mesh) {
                Array<std::pair<String, String>> slots;
                slots.reserve(mesh_asset->materials.size());
                for (const auto& sm : mesh_asset->materials)
                    slots.emplace(sm.slot, sm.key);
                mesh_dependencies.add({dep_key, mesh_asset->mesh, std::move(slots)});
            } else if (dep.mesh) {
                mesh_dependencies.add({dep_key, dep.mesh, {}});
            }
        }

        Array<presentation::ProceduralInstanceAsset> resolved_assets;
        Set<StringView> seen_keys;
        for (const auto& col : published->collections) {
            const String& key = col.reference.key;
            if (key.empty() || !seen_keys.add(key.view()))
                continue;
            auto mesh_asset = cache.resources.resolve_mesh_asset(key);
            if (!mesh_asset || !mesh_asset->mesh) {
                if (col.reference.mesh)
                    resolved_assets.add({key, col.reference.mesh, {}});
                continue;
            }
            Array<MissingResource> ignored_missing;
            auto defaults = mesh_defaults(active_reader, *mesh_asset, &ignored_missing);
            resolved_assets.add({key, mesh_asset->mesh,
                                 defaults ? std::move(*defaults) : Array<presentation::MaterialBinding>{}});
        }

        presentation::ProceduralOutput draft;
        draft.generation = entry.next_output_generation++;
        draft.geometry = retained_geom;
        draft.surface_mesh = std::move(surface_mesh);
        draft.publication = std::move(*published);
        draft.mesh_dependencies = std::move(mesh_dependencies);

        auto committed = presentation::publish_procedural_binding(runtime_, component, proc_source->source,
                                                                  std::move(draft), resolved_assets);
        if (!committed) {
            ATELIER_TRY(contain_error(committed.error(), component, comp_state->owner));
            continue;
        }

        entry.chunk_size = chunk_size;
        entry.output = std::move(*committed);
        entry.resolved_assets = std::move(resolved_assets);
        entry.latest_cook.emplace();
    }

    cache.graph_slots.remove_if([&](const String& k, const auto&) { return !referenced_graphs.contains(k); });

    if (auto now = runtime_.change_cursor()) {
        cache.idle_valid = true;
        cache.idle_world_revision = world_.revision();
        cache.idle_session_revision = revision_;
        cache.idle_project = project;
        cache.idle_project_revision = proj_rev;
        cache.idle_code_version = code_version;
        cache.idle_runtime_cursor = *now;
    }
    return {};
}

const Error* Session::procedural_generator_error(ObjectId component) const noexcept {
    if (!generator_cache_)
        return nullptr;
    const auto* it = generator_cache_->entries.find(component);
    if (!it)
        return nullptr;
    const auto& cook = it->latest_cook;
    return cook && !*cook ? &cook->error() : nullptr;
}

Result<world::Commit> Session::bake_procedural_generator(Project* project, ObjectId target) {
    if (world_.running() || world_.closed())
        return graph_error("Baking a procedural generator requires an open, stopped document.",
                           ErrorCode::invalid_definition);
    ATELIER_TRY(cancel_edit());

    ATELIER_TRY_BIND(component_id, resolve_procedural_generator(target));
    const ObjectId component = *component_id;
    ATELIER_TRY_BIND(comp_state, admit_component_edit(component));
    const ObjectId owner = comp_state->owner;

    ATELIER_TRY(sync_procedural_generators(project));
    if (const auto* failure = procedural_generator_error(component))
        return std::unexpected(*failure);
    ATELIER_TRY_BIND(binding, presentation::read_procedural_binding(runtime_, component));
    if (!binding->output || !generator_cache_) {
        return graph_error(
            "Procedural Bake converts CopyToPoints mesh instances into StaticMeshEntity objects; this generator currently has no cooked output.",
            ErrorCode::invalid_definition);
    }
    const auto* cached = generator_cache_->entries.find(component);
    if (!cached || !cached->latest_cook || cached->output != binding->output)
        return graph_error("Procedural Bake requires a successful current cook.", ErrorCode::invalid_definition);

    struct ResolvedAssetInfo {
        String key;

        geometry::MeshRef mesh;

        Array<presentation::MaterialBinding> defaults;

        std::wstring stem;
    };

    Array<ResolvedAssetInfo> resolved_assets;
    Map<String, Optional<std::size_t>> asset_table;
    assets::Reader builtin_reader;
    assets::Reader& active_reader = project ? project->assets() : builtin_reader;
    const auto resolve_asset_info = [&](const String& key) -> Result<Optional<std::size_t>> {
        if (key.empty())
            return Optional<std::size_t>{};
        if (const auto* it = asset_table.find(key))
            return *it;
        ATELIER_TRY_BIND(live_mesh, active_reader.mesh(key));
        if (!live_mesh->mesh) {
            asset_table.try_emplace(key, Optional<std::size_t>{});
            return Optional<std::size_t>{};
        }
        ResolvedAssetInfo info{key,
                               live_mesh->mesh,
                               {},
                               live_mesh->name.empty() ? std::wstring{L"Instance"} : live_mesh->name};
        bool found_defaults = false;
        for (const auto& asset : binding->resolved_assets) {
            if (asset.key == key) {
                info.defaults = asset.defaults;
                found_defaults = true;
                break;
            }
        }
        if (!found_defaults) {
            Array<MissingResource> ignored_missing;
            if (auto defaults = mesh_defaults(active_reader, *live_mesh, &ignored_missing))
                info.defaults = std::move(*defaults);
        }
        if (project) {
            if (const auto* entry = project->assets().entry(key)) {
                if (const auto s = entry->path.stem().wstring(); !s.empty())
                    info.stem = s;
            }
        }
        const std::size_t idx = resolved_assets.size();
        resolved_assets.add(std::move(info));
        asset_table.try_emplace(key, idx);
        return Optional<std::size_t>{idx};
    };

    struct PendingInstance {
        std::size_t asset_index{};

        world::Trs trs{};
    };
    Array<PendingInstance> pending;
    for (const auto& col : binding->output->publication.collections) {
        if (!col.transforms || col.transforms->empty())
            continue;
        const String& key = col.reference.key;
        ATELIER_TRY_BIND(asset_idx_opt, resolve_asset_info(key));
        if (!*asset_idx_opt)
            continue;
        const std::size_t asset_idx = **asset_idx_opt;
        if (resolved_assets[asset_idx].mesh != col.reference.mesh)
            return graph_error(String::concat(
                "Cannot bake '", key,
                "': the published instance mesh differs from the current project asset. Recook it first."));
        for (const auto& affine : *col.transforms) {
            ATELIER_TRY_BIND(trs, decompose_procedural_instance(affine));
            pending.add({asset_idx, *trs});
        }
    }

    if (pending.empty()) {
        return graph_error(
            "Procedural Bake converts CopyToPoints mesh instances into StaticMeshEntity objects; this generator currently publishes no mesh instances.",
            ErrorCode::invalid_definition);
    }

    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() + 1 + pending.size() > maximum_entities) {
        return graph_error(String::concat("Procedural Bake exceeds the Editor Entity limit (",
                                          String::from_number(maximum_entities), ")."),
                           ErrorCode::invalid_definition);
    }

    ATELIER_TRY_BIND(owner_parent, world::parent_entity(runtime_, owner));
    ATELIER_TRY_BIND(owner_has_space, world::has_transform(runtime_, owner));
    world::Trs owner_trs{};
    Optional<ObjectId> owner_space_ref;
    if (*owner_has_space) {
        ATELIER_TRY_BIND(t, world::read_transform(runtime_, owner));
        owner_trs = *t;
        ATELIER_TRY_BIND(sr, world::space_reference(runtime_, owner));
        owner_space_ref = *sr;
    }

    Array<ObjectRoot> cleanup_entities;
    ScopeExit cleanup{[&]() noexcept {
        for (auto it = cleanup_entities.rbegin(); it != cleanup_entities.rend(); ++it) {
            if (it->id() != ObjectId{} && runtime_.type_of(it->id()))
                (void)world::destroy_entity(runtime_, it->id());
        }
    }};

    ATELIER_TRY_BIND(group_root, world::create_entity(runtime_));
    const ObjectId group_id = group_root->id();
    cleanup_entities.add(std::move(*group_root));
    if (*owner_has_space)
        ATELIER_TRY(world_.set_transform(group_id, owner_trs));

    Array<std::pair<ObjectId, std::wstring>> child_labels;
    child_labels.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& inst = pending[i];
        const auto& asset = resolved_assets[inst.asset_index];
        ATELIER_TRY_BIND(child, presentation::create_static_mesh_entity(
                                    world_, {asset.mesh, inst.trs, asset.key, asset.defaults}));
        const ObjectId child_id = child->id();
        cleanup_entities.add(std::move(*child));
        ATELIER_TRY(world_.reparent(child_id, group_id, world::ReparentRule::keep_local));

        const std::wstring suffix = L" " + std::to_wstring(i + 1);
        std::wstring child_label = truncate_utf16(asset.stem, 256 - suffix.size()) + suffix;
        ATELIER_TRY(validate_name(child_label));
        child_labels.emplace(child_id, std::move(child_label));
    }

    std::wstring base_name(name(owner));
    if (base_name.empty())
        base_name = L"Procedural Generator";
    base_name += L" Baked";

    std::wstring group_label = truncate_utf16(base_name, 256);
    for (std::size_t suffix = 2; std::ranges::any_of(names_, [&](const auto& kv) { return kv.value == group_label; });
         ++suffix) {
        const auto tail = L" " + std::to_wstring(suffix);
        group_label = truncate_utf16(base_name, 256 - tail.size()) + tail;
    }
    ATELIER_TRY(validate_name(group_label));

    EntityChange change;
    change.added = true;
    {
        world::EntityRemoval removal;
        ATELIER_TRY_BIND(held, runtime_.hold(group_id));
        removal.root = std::move(*held);
        change.removals.add(std::move(removal));
    }

    Array<world::SpaceReference> spaces;
    if (*owner_has_space && owner_space_ref.has_value())
        spaces.add({group_id, owner_space_ref});

    const std::array attachments{world::EntityAttachment{group_id, *owner_parent, world::append}};
    ATELIER_TRY_BIND(attached, world_.attach_batch(attachments, spaces));
    cleanup.release();

    names_.try_emplace(group_id, std::move(group_label));
    for (auto& [cid, clabel] : child_labels)
        names_.try_emplace(cid, std::move(clabel));

    if (attached->changed) {
        set_selection({group_id, group_id});
        remember(std::move(change));
    }
    return std::move(*attached);
}

} // namespace atelier::editor
