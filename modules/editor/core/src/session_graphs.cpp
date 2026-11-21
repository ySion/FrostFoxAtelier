#include "editor/session.hpp"
#include "editor/fields.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "editor/project_code.hpp"
#include "editor/properties.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/graph.hpp"
#include "procedural/instances.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <set>
#include <unordered_set>

namespace atelier::editor {
namespace {

auto graph_error(std::string message, ErrorCode code = ErrorCode::argument_mismatch) {
    return std::unexpected(Error{code, std::move(message)});
}

bool valid_node_identifier(std::string_view text) noexcept {
    const auto head = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    return !text.empty() && text.size() <= 128 && head(text.front()) &&
           std::ranges::all_of(text.substr(1), [&](char c) { return head(c) || (c >= '0' && c <= '9'); });
}

std::string default_node_stem(std::string_view type_name) {
    if (const auto dot = type_name.find_last_of('.'); dot != std::string_view::npos)
        type_name = type_name.substr(dot + 1);
    std::string stem;
    for (std::size_t i = 0; i < type_name.size(); ++i) {
        const char c = type_name[i];
        if (c >= 'A' && c <= 'Z') {
            if (i > 0 && type_name[i - 1] >= 'a' && type_name[i - 1] <= 'z')
                stem.push_back('_');
            stem.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            stem.push_back(c);
        }
    }
    if (stem.empty() || (stem.front() >= '0' && stem.front() <= '9'))
        stem.insert(stem.begin(), 'n');
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
                                 std::span<const procedural::Input> desired) {
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

    [[nodiscard]] Result<assets::MeshAsset> resolve_mesh_asset(std::string_view key) const {
        assets::Reader& active = temporary_reader_ ? *temporary_reader_ : project_ ? project_->assets() : builtin_;
        const std::uint64_t rev = active.revision();
        if (cached_revision_ != rev) {
            cached_.clear();
            cached_revision_ = rev;
        }
        const std::string lookup(key);
        if (const auto it = cached_.find(lookup); it != cached_.end())
            return it->second;
        auto resolved = active.mesh(key);
        if (!resolved)
            return std::unexpected(resolved.error());
        cached_.insert_or_assign(lookup, *resolved);
        return *resolved;
    }

    [[nodiscard]] Result<geometry::MeshRef> mesh(StringView key) const noexcept override {
        try {
            auto resolved = resolve_mesh_asset(key.std_view());
            if (!resolved)
                return std::unexpected(resolved.error());
            return std::move(resolved->mesh);
        } catch (const std::exception& e) {
            return std::unexpected(Error{ErrorCode::invalid_storage, e.what()});
        }
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

    mutable std::unordered_map<std::string, assets::MeshAsset> cached_;

    mutable std::uint64_t cached_revision_{};

    std::uint64_t epoch_{1};
};

struct GraphScalarChange {
    ObjectRoot target;

    std::vector<world::FieldPath> paths;

    std::vector<Scalar> before;

    std::vector<Scalar> after;

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

    bool before{};

    bool after{};
};

struct GraphAppearanceChange {
    struct Entry {
        std::string name;

        GraphNodeShape before_shape{GraphNodeShape::auto_role};

        GraphNodeShape after_shape{GraphNodeShape::auto_role};

        std::optional<std::uint32_t> before_color{};

        std::optional<std::uint32_t> after_color{};

        std::string before_comment{};

        std::string after_comment{};
    };

    std::vector<Entry> entries;
};

struct GraphAnnotationChange {
    std::vector<GraphStickyNote> before_notes;

    std::vector<GraphStickyNote> after_notes;

    std::vector<GraphNetworkBox> before_boxes;

    std::vector<GraphNetworkBox> after_boxes;
};

struct GraphRenameChange {
    std::string before;

    std::string after;

    struct Retained {
        std::string owner; // Empty for graph settings. Node names are recorded before the rename.

        json::Document before;

        json::Document after;
    };

    std::vector<Retained> references;
};

struct GraphMoveChange {
    struct Move {
        std::string name;

        std::array<float, 2> before{};

        std::array<float, 2> after{};
    };

    std::vector<Move> moves;

    std::vector<Move> note_moves;

    std::vector<Move> box_moves;
};

struct GraphConnectionChange {
    struct TargetDelta {
        ObjectRoot target;

        std::vector<procedural::Input> before;

        std::vector<procedural::Input> after;

        json::Document before_kept;

        json::Document after_kept;
    };

    std::vector<TargetDelta> targets;

    std::vector<GraphMoveChange::Move> moves{};
};

struct GraphNodesChange {
    struct SavedInputs {
        ObjectId to{};

        std::vector<procedural::Input> inputs;

        std::vector<procedural::Input> detached_inputs{};
    };

    struct RetainedNodeKept {
        std::string owner;

        json::Document before;

        json::Document after;
    };

    bool added{};

    std::vector<GraphNode> entries;

    std::vector<SavedInputs> saved_inputs;

    ObjectId before_display{};

    ObjectId after_display{};

    ObjectId before_template{};

    ObjectId after_template{};

    json::Document before_kept;

    json::Document after_kept;

    std::vector<RetainedNodeKept> retained_node_kept{};
};

using GraphChange = std::variant<GraphScalarChange, GraphPropertyChange, GraphDisplayChange, GraphTemplateChange,
                                 GraphBypassChange, GraphFreezeChange, GraphAppearanceChange, GraphAnnotationChange,
                                 GraphRenameChange, GraphMoveChange, GraphConnectionChange, GraphNodesChange>;

void erase_kept_input(json::Document& kept, std::string_view port) {
    json::Builder next_kept(kept);
    if (const auto kept_inputs = *next_kept.root().find("inputs");
        kept_inputs && kept_inputs->kind() == json::Kind::object) {
        next_kept.erase(*kept_inputs, port);
        if (!kept_inputs->size())
            next_kept.erase(next_kept.root(), "inputs");
    }
    kept = next_kept.finish();
}

void sort_graph_nodes(std::vector<GraphNode>& nodes) {
    std::ranges::sort(nodes, {}, &GraphNode::name);
}

void sort_graph_notes(std::vector<GraphStickyNote>& notes) {
    std::ranges::sort(notes, {}, &GraphStickyNote::name);
}

void sort_graph_boxes(std::vector<GraphNetworkBox>& boxes) {
    std::ranges::sort(boxes, {}, &GraphNetworkBox::name);
}

GraphNode* find_node(GraphDocument& doc, std::string_view name) noexcept {
    const auto at = std::ranges::lower_bound(doc.nodes, name, {}, &GraphNode::name);
    return at != doc.nodes.end() && at->name == name ? &*at : nullptr;
}

GraphStickyNote* find_note(GraphDocument& doc, std::string_view name) noexcept {
    const auto at = std::ranges::lower_bound(doc.notes, name, {}, &GraphStickyNote::name);
    return at != doc.notes.end() && at->name == name ? &*at : nullptr;
}

GraphNetworkBox* find_box(GraphDocument& doc, std::string_view name) noexcept {
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

Result<std::vector<std::string>> format_live_port_endpoints(Runtime& runtime, const GraphDocument& doc,
                                                            std::span<const procedural::Input> node_inputs,
                                                            std::uint32_t port_index) {
    std::vector<std::string> out;
    for (const auto& in : node_inputs) {
        if (in.input != port_index)
            continue;
        const auto* src = find_node_by_id(doc, in.from);
        if (!src)
            continue;
        ATELIER_TRY_BIND(src_type, runtime.type_of(in.from));
        ATELIER_TRY_BIND(src_ports, procedural::ports(runtime, *src_type));
        if (in.output < src_ports->outputs.size())
            out.push_back(src->name + "." + src_ports->outputs[in.output]);
    }
    return out;
}

template <class Pred>
bool kept_port_has_match(const json::Document& kept, std::string_view port_filter, Pred&& matches) {
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

template <class Pred>
void filter_kept_port_endpoints(json::Document& kept, std::string_view port_name, bool is_multi,
                                std::span<const std::string> live_after, Pred&& should_remove) {
    const auto cur_inputs = *kept.root().find("inputs");
    if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
        return;
    const auto cur_port = *cur_inputs->find(port_name);
    if (!cur_port)
        return;

    if (cur_port->kind() == json::Kind::string) {
        if (should_remove(*cur_port->string()))
            erase_kept_input(kept, port_name);
        return;
    }
    if (cur_port->kind() != json::Kind::array)
        return;

    std::vector<std::string> surviving;
    bool all_strings = true;
    std::size_t orig_count = 0;
    for (const auto elem : *cur_port->array()) {
        ++orig_count;
        if (const auto s = elem.string()) {
            if (!should_remove(*s))
                surviving.emplace_back(*s);
        } else {
            all_strings = false;
        }
    }
    if (all_strings && surviving.size() == orig_count)
        return;
    if (all_strings && (surviving.empty() || (is_multi && std::ranges::equal(surviving, live_after)))) {
        erase_kept_input(kept, port_name);
        return;
    }

    json::Builder next(kept);
    const auto next_inputs = *next.root().at("inputs");
    next.erase(next_inputs, port_name);
    const auto arr = *next.array(next_inputs, port_name);
    for (const auto elem : *cur_port->array()) {
        if (const auto s = elem.string()) {
            if (!should_remove(*s))
                static_cast<void>(next.append(arr, *s));
        } else {
            next.append(arr, elem);
        }
    }
    kept = next.finish();
}

void append_kept_port_endpoint(json::Document& kept, std::string_view port_name, bool is_multi,
                               std::string_view endpoint, std::span<const std::string> live_after) {
    if (!is_multi) {
        erase_kept_input(kept, port_name);
        return;
    }
    const auto cur_inputs = *kept.root().find("inputs");
    if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
        return;
    const auto cur_port = *cur_inputs->find(port_name);
    if (!cur_port)
        return;
    if (cur_port->kind() != json::Kind::array) {
        erase_kept_input(kept, port_name);
        return;
    }
    std::vector<std::string> items;
    bool all_strings = true;
    bool already_present = false;
    for (const auto elem : *cur_port->array()) {
        if (const auto s = elem.string()) {
            items.emplace_back(*s);
            if (*s == endpoint)
                already_present = true;
        } else {
            all_strings = false;
        }
    }
    if (!already_present)
        items.emplace_back(endpoint);
    if (all_strings && std::ranges::equal(items, live_after)) {
        erase_kept_input(kept, port_name);
        return;
    }
    if (already_present)
        return;
    json::Builder next(kept);
    const auto next_inputs = *next.root().at("inputs");
    const auto next_port = *next_inputs.at(port_name);
    static_cast<void>(next.append(next_port, endpoint));
    kept = next.finish();
}

void replace_kept_port_endpoint(json::Document& kept, std::string_view port_name, bool is_multi,
                                std::string_view old_endpoint, std::string_view new_endpoint,
                                std::span<const std::string> live_after) {
    if (!is_multi) {
        erase_kept_input(kept, port_name);
        return;
    }
    const auto cur_inputs = *kept.root().find("inputs");
    if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
        return;
    const auto cur_port = *cur_inputs->find(port_name);
    if (!cur_port)
        return;
    if (cur_port->kind() != json::Kind::array) {
        erase_kept_input(kept, port_name);
        return;
    }
    bool already_has_new = false;
    for (const auto elem : *cur_port->array()) {
        if (const auto s = elem.string(); s && *s == new_endpoint) {
            already_has_new = true;
            break;
        }
    }
    std::vector<std::string> updated;
    bool all_strings = true;
    bool inserted = false;
    bool changed = false;
    for (const auto elem : *cur_port->array()) {
        const auto s = elem.string();
        if (!s) {
            all_strings = false;
            continue;
        }
        if (*s == old_endpoint) {
            changed = true;
            if (!already_has_new && !inserted) {
                updated.emplace_back(new_endpoint);
                inserted = true;
            }
            continue;
        }
        updated.emplace_back(*s);
    }
    if (!changed)
        return;
    if (all_strings && (updated.empty() || std::ranges::equal(updated, live_after))) {
        erase_kept_input(kept, port_name);
        return;
    }
    json::Builder next(kept);
    const auto next_inputs = *next.root().at("inputs");
    next.erase(next_inputs, port_name);
    const auto arr = *next.array(next_inputs, port_name);
    inserted = false;
    for (const auto elem : *cur_port->array()) {
        if (const auto s = elem.string()) {
            if (*s == old_endpoint) {
                if (!already_has_new && !inserted) {
                    static_cast<void>(next.append(arr, new_endpoint));
                    inserted = true;
                }
                continue;
            }
            static_cast<void>(next.append(arr, *s));
        } else {
            next.append(arr, elem);
        }
    }
    kept = next.finish();
}

std::string format_number(double v) {
    if (!std::isfinite(v))
        return std::isnan(v) ? "nan" : (v < 0 ? "-inf" : "inf");
    if (std::abs(v) < 1e-9)
        v = 0.0;
    return std::format("{:.4g}", v);
}

template <class T>
void summarize_numeric_lanes(std::span<const T> values, std::uint32_t lanes, auto&& lane_at, GraphAttributeStats& out,
                             std::size_t max_samples) {
    out.components = lanes;
    out.count = values.size();
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
            for (std::uint32_t c = 0; c < lanes; ++c) {
                const double v = static_cast<double>(lane_at(values[i], c));
                out.minimum[c] = std::min(out.minimum[c], v);
                out.maximum[c] = std::max(out.maximum[c], v);
                out.mean[c] += v;
            }
        }
        if (i < max_samples) {
            if (lanes == 1) {
                out.sample_values.push_back(format_number(static_cast<double>(lane_at(values[i], 0))));
            } else {
                std::string tuple = "[";
                for (std::uint32_t c = 0; c < lanes; ++c) {
                    if (c)
                        tuple += ", ";
                    tuple += format_number(static_cast<double>(lane_at(values[i], c)));
                }
                tuple += "]";
                out.sample_values.push_back(std::move(tuple));
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

} // namespace

struct Session::OpenGraph {
    std::string key;

    bool scratch{};

    GraphDocument document;

    std::string saved_text;

    std::vector<GraphChange> history;

    std::size_t cursor{};

    GraphResources resources;

    const source::Program* bound_program{};

    std::unique_ptr<procedural::Evaluator> evaluator;

    std::shared_ptr<const GraphInspectView> topology;

    std::uint64_t evaluation_generation{};

    std::uint64_t topology_revision{};

    std::uint64_t topology_generation{};

    std::uint64_t mutation_epoch{1};

    mutable std::uint64_t dirty_revision{};

    mutable bool dirty_cached{};

    void push_change(GraphChange change) {
        history.erase(history.begin() + static_cast<std::ptrdiff_t>(cursor), history.end());
        history.push_back(std::move(change));
        ++cursor;
        if (history.size() > Session::history_limit) {
            history.erase(history.begin());
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
        std::optional<GraphDocument> closed_doc;

        std::uint64_t closed_project_revision{};

        ObjectId bound_graph_id{};

        const source::Program* bound_program{};

        std::uint64_t code_version{};

        std::unique_ptr<procedural::Evaluator> evaluator;
    };

    struct Entry {
        std::string graph_key;

        float chunk_size{};

        // Disengaged if skipped. Presentation can retain the previous output after a failed cook.
        std::optional<Result<void>> latest_cook;

        std::uint64_t next_output_generation{1};

        std::shared_ptr<const presentation::ProceduralOutput> output;

        std::vector<presentation::ProceduralInstanceAsset> resolved_assets;
    };

    GraphResources resources;

    assets::Reader* fallback_reader{};

    std::unordered_map<std::string, GraphSlot> graph_slots;

    std::unordered_map<ObjectId, Entry> entries;

    bool idle_valid{};

    bool has_errors{};

    std::uint64_t idle_world_revision{};

    std::uint64_t idle_session_revision{};

    const Project* idle_project{};

    std::uint64_t idle_project_revision{};

    std::uint64_t idle_code_version{};

    std::uint64_t idle_runtime_cursor{};

    void clear() noexcept {
        for (auto& [_, slot] : graph_slots) {
            slot.evaluator.reset();
            slot.closed_doc.reset();
        }
        graph_slots.clear();
        for (auto& [_, entry] : entries) {
            entry.resolved_assets.clear();
            entry.output.reset();
        }
        entries.clear();
        idle_valid = false;
        has_errors = false;
    }
};

void Session::OpenGraphDeleter::operator()(OpenGraph* ptr) const noexcept {
    delete ptr;
}

void Session::GeneratorCacheDeleter::operator()(GeneratorRuntimeCache* ptr) const noexcept {
    delete ptr;
}

void Session::bind_project_code(const ProjectCode* code) noexcept {
    if (project_code_ == code)
        return;
    project_code_ = code;
    for (auto& [_, open] : graphs_) {
        if (open) {
            open->evaluator.reset();
            open->bound_program = nullptr;
        }
    }
    if (generator_cache_) {
        for (auto& [_, slot] : generator_cache_->graph_slots) {
            slot.evaluator.reset();
            slot.bound_program = nullptr;
        }
        generator_cache_->idle_valid = false;
    }
}

GraphGeometrySummary summarize_geometry(const procedural::Geometry* geometry, std::size_t max_samples) {
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
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
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
    std::vector<std::size_t> ref_counts(refs.size(), 0);
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
        summary.instance_references.emplace_back(std::string(refs[i].key.view().std_view()), ref_counts[i]);

    for (const auto& attr : geometry->attributes()) {
        GraphAttributeStats stats;
        stats.name = std::string(attr.name.view().std_view());
        std::visit(
            [&](const auto& col) {
                using Shared = std::decay_t<decltype(col)>;
                using T = typename Shared::Value;
                const std::span<const T> values = col.array();
                if constexpr (std::same_as<T, bool>) {
                    stats.type = "bool";
                    summarize_numeric_lanes(
                        values, 1, [](bool v, std::uint32_t) { return v ? 1.0 : 0.0; }, stats, max_samples);
                } else if constexpr (std::same_as<T, std::int32_t>) {
                    stats.type = "int";
                    summarize_numeric_lanes(
                        values, 1, [](std::int32_t v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, std::uint32_t>) {
                    stats.type = "uint";
                    summarize_numeric_lanes(
                        values, 1, [](std::uint32_t v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, float>) {
                    stats.type = "float";
                    summarize_numeric_lanes(values, 1, [](float v, std::uint32_t) { return v; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector2>) {
                    stats.type = "vector2";
                    summarize_numeric_lanes(
                        values, 2, [](const math::Vector2& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector3>) {
                    stats.type = "vector3";
                    summarize_numeric_lanes(
                        values, 3, [](const math::Vector3& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Vector4>) {
                    stats.type = "vector4";
                    summarize_numeric_lanes(
                        values, 4, [](const math::Vector4& v, std::uint32_t c) { return v[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, math::Quaternion>) {
                    stats.type = "quaternion";
                    summarize_numeric_lanes(
                        values, 4,
                        [](const math::Quaternion& q, std::uint32_t c) {
                            return c == 0 ? q.x : c == 1 ? q.y : c == 2 ? q.z : q.w;
                        },
                        stats, max_samples);
                } else if constexpr (std::same_as<T, math::Matrix4>) {
                    stats.type = "matrix4";
                    summarize_numeric_lanes(
                        values, 16, [](const math::Matrix4& m, std::uint32_t c) { return m[c]; }, stats, max_samples);
                } else if constexpr (std::same_as<T, String>) {
                    stats.type = "string";
                    stats.components = 0;
                    stats.count = values.size();
                    for (std::size_t i = 0; i < std::min(values.size(), max_samples); ++i)
                        stats.sample_values.emplace_back(values[i].view().std_view());
                }
            },
            attr.data);
        const auto layer_idx = static_cast<std::size_t>(attr.layer);
        if (layer_idx < summary.attributes.size())
            summary.attributes[layer_idx].push_back(std::move(stats));
    }
    return summary;
}

void Session::close_graphs() noexcept {
    if (generator_cache_) {
        generator_cache_->clear();
        generator_cache_.reset();
    }
    for (auto& [_, open] : graphs_) {
        if (open)
            open->evaluator.reset();
    }
    graphs_.clear();
}

Result<const source::Program*> Session::ensure_procedural_program() {
    if (project_code_)
        return project_code_->program();
    if (engine_program_)
        return engine_program_.get();
    while (runtime_.collection_phase() != CollectionPhase::idle) {
        ATELIER_TRY(runtime_.collect_step({1024}));
    }
    ATELIER_TRY_BIND(engine_files, source::engine_sources(source::default_engine_root()));
    source::SourceSnapshot snapshot;
    snapshot.name = "engine";
    snapshot.version = 14695981039346656037ULL;
    for (auto& file : *engine_files) {
        snapshot.version = source::content_hash(file.path, source::content_hash(file.source, snapshot.version));
        snapshot.files.push_back(std::move(file));
    }
    ATELIER_TRY_BIND(program, source::Program::create(runtime_));
    ATELIER_TRY_BIND(request, (*program)->request(snapshot));
    ATELIER_TRY_BIND(candidate, source::Program::prepare(*request));
    ATELIER_TRY((*program)->try_publish(*candidate));
    engine_program_ = std::shared_ptr<source::Program>(std::move(*program));
    return engine_program_.get();
}

Result<void> Session::remember_graph_scalar_edit(ObjectRoot target, std::vector<world::FieldPath> paths,
                                                 std::vector<Scalar> before, std::vector<Scalar> after) {
    for (auto& [_, open] : graphs_) {
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
                    names.insert(names.begin(), "properties");
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

Result<std::vector<GraphNodeCatalogEntry>> Session::procedural_node_types() {
    if (world_.closed())
        return graph_error("Procedural node catalog requires an open Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(ensure_procedural_program());
    ATELIER_TRY_BIND(node_base, runtime_.native_type<procedural::Node>());
    std::vector<GraphNodeCatalogEntry> catalog;
    for (const auto type : runtime_.types()) {
        if (type == *node_base)
            continue;
        ATELIER_TRY_BIND(info, runtime_.describe(type));
        if ((*info)->kind != TypeKind::object)
            continue;
        ATELIER_TRY_BIND(derived, runtime_.is_a(type, *node_base));
        if (!*derived)
            continue;
        auto port_info = procedural::ports(runtime_, type);
        if (!port_info)
            continue;
        const bool native = (*info)->native_storage;
        catalog.push_back(
            {type,
             std::string((*info)->name),
             std::string((*info)->description),
             native,
             {std::move(port_info->inputs), std::move(port_info->multiple), std::move(port_info->outputs)}});
    }
    std::ranges::sort(catalog, {}, &GraphNodeCatalogEntry::name);
    return catalog;
}

Result<ObjectId> Session::open_graph(Project& project, std::string_view key) {
    if (world_.closed())
        return graph_error("Opening a procedural graph requires a live Session.", ErrorCode::invalid_definition);
    if (const auto existing = graphs_.find(std::string(key)); existing != graphs_.end())
        return existing->second->document.graph.id();
    ATELIER_TRY(ensure_procedural_program());
    ATELIER_TRY_BIND(asset, project.assets().procedural_graph(key));
    ATELIER_TRY_BIND(loaded, load_graph(runtime_, asset->text));
    ATELIER_TRY_BIND(canonical, editor::save_graph(runtime_, *loaded));
    std::unique_ptr<OpenGraph, OpenGraphDeleter> open(new OpenGraph());
    open->key = asset->key;
    open->document = std::move(*loaded);
    open->saved_text = std::move(*canonical);
    open->resources.bind(&project);
    for (const auto& issue : open->document.issues)
        world_.report(issue);
    const auto id = open->document.graph.id();
    unavailable_.erase(std::pair{assets::AssetKind::procedural_graph, open->key});
    graphs_.emplace(open->key, std::move(open));
    ++revision_;
    return id;
}

Result<ObjectId> Session::create_scratch_graph(std::string_view key) {
    if (world_.closed() || key.empty())
        return graph_error("Creating a scratch graph requires a live Session and non-empty key.",
                           ErrorCode::invalid_definition);
    if (const auto existing = graphs_.find(std::string(key)); existing != graphs_.end())
        return existing->second->document.graph.id();
    ATELIER_TRY(ensure_procedural_program());
    ATELIER_TRY_BIND(graph_type, runtime_.native_type<procedural::Graph>());
    ATELIER_TRY_BIND(graph_root, runtime_.create(*graph_type));
    std::unique_ptr<OpenGraph, OpenGraphDeleter> open(new OpenGraph());
    open->key = std::string(key);
    open->scratch = true;
    open->document.graph = std::move(*graph_root);
    ATELIER_TRY_BIND(canonical, editor::save_graph(runtime_, open->document));
    open->saved_text = std::move(*canonical);
    const auto id = open->document.graph.id();
    graphs_.emplace(open->key, std::move(open));
    ++revision_;
    return id;
}

bool Session::has_graph(std::string_view key) const noexcept {
    return graphs_.contains(std::string(key));
}

bool Session::is_scratch_graph(std::string_view key) const noexcept {
    if (key == "scratch" || key.starts_with("scratch/"))
        return true;
    const auto at = graphs_.find(std::string(key));
    return at != graphs_.end() && at->second && at->second->scratch;
}

std::optional<std::string> Session::procedural_graph_snapshot(std::string_view key) const {
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second || at->second->scratch)
        return std::nullopt;
    auto encoded = editor::save_graph(world_.runtime(), at->second->document);
    if (!encoded)
        return std::nullopt;
    return std::move(*encoded);
}

Result<ObjectId> Session::resolve_procedural_generator(ObjectId target) const {
    if (world_.closed())
        return graph_error("Procedural Generator resolution requires an open Session.", ErrorCode::invalid_definition);
    auto& runtime = world_.runtime();
    if (target == ObjectId{})
        target = selection_.object != ObjectId{} ? selection_.object : selection_.entity;
    if (target == ObjectId{})
        return graph_error("Select a Procedural Generator.", ErrorCode::invalid_definition);
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
            return graph_error("Target is not a Procedural Generator.", ErrorCode::invalid_definition);
        owner = target;
    }
    ATELIER_TRY_BIND(parts, world::components(runtime, owner));
    for (const auto& part : *parts) {
        ATELIER_TRY_BIND(part_type, runtime.type_of(part.id()));
        ATELIER_TRY_BIND(matches, runtime.is_a(*part_type, presentation_types_.procedural_generator_component));
        if (*matches)
            return part.id();
    }
    return graph_error("Selected Entity has no ProceduralGeneratorComponent.", ErrorCode::invalid_definition);
}

Result<void> Session::publish_procedural_mesh(assets::Reader& reader, const assets::MeshAsset&) {
    for (auto& [_, open] : graphs_)
        if (open)
            open->resources.invalidate();
    if (!generator_cache_)
        generator_cache_.reset(new GeneratorRuntimeCache());
    generator_cache_->resources.invalidate();
    generator_cache_->idle_valid = false;
    generator_cache_->fallback_reader = &reader;
    generator_cache_->resources.bind_temporary_reader(&reader);
    struct ResetReader {
        GeneratorRuntimeCache& cache;

        ~ResetReader() {
            cache.resources.bind_temporary_reader(nullptr);
            cache.fallback_reader = nullptr;
        }
    } reset{*generator_cache_};
    return sync_procedural_generators(nullptr);
}

ObjectId Session::loaded_graph(std::string_view key) const noexcept {
    const auto at = graphs_.find(std::string(key));
    return at == graphs_.end() ? ObjectId{} : at->second->document.graph.id();
}

bool Session::is_graph_object(ObjectId object) const noexcept {
    if (object == ObjectId{})
        return false;
    for (const auto& [_, open] : graphs_)
        if (open && open->owns(object))
            return true;
    return false;
}

std::vector<ObjectId> Session::graph_objects() const {
    std::vector<ObjectId> result;
    for (const auto& [_, open] : graphs_) {
        if (!open)
            continue;
        if (open->document.graph.registered())
            result.push_back(open->document.graph.id());
        for (const auto& node : open->document.nodes)
            if (node.node.registered())
                result.push_back(node.node.id());
    }
    return result;
}

std::vector<std::string> Session::open_graphs() const {
    std::vector<std::string> result;
    result.reserve(graphs_.size());
    for (const auto& [key, open] : graphs_)
        if (open)
            result.push_back(key);
    std::ranges::sort(result);
    return result;
}

std::optional<std::string> Session::graph_of_object(ObjectId object) const {
    if (object == ObjectId{})
        return std::nullopt;
    for (const auto& [key, open] : graphs_)
        if (open && open->owns(object))
            return key;
    return std::nullopt;
}

std::optional<std::string> Session::graph_node_name(ObjectId object) const {
    if (object == ObjectId{})
        return std::nullopt;
    for (const auto& [_, open] : graphs_) {
        if (!open)
            continue;
        if (const auto* found = find_node_by_id(open->document, object))
            return found->name;
    }
    return std::nullopt;
}

const GraphNode* Session::graph_node(std::string_view key, std::string_view name) const noexcept {
    const auto at = graphs_.find(std::string(key));
    return at == graphs_.end() ? nullptr : find_node(at->second->document, name);
}

ObjectId Session::graph_node_id(std::string_view key, std::string_view name) const noexcept {
    const auto* found = graph_node(key, name);
    return found && found->node.registered() ? found->node.id() : ObjectId{};
}

const GraphDocument* Session::graph_document(std::string_view key) const noexcept {
    const auto at = graphs_.find(std::string(key));
    return at == graphs_.end() || !at->second ? nullptr : &at->second->document;
}

bool Session::graph_dirty(std::string_view key) const {
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return false;
    const auto& open = *at->second;
    if ((active_ && open.owns(active_->root.id())) || (group_ && open.owns(group_->root.id())))
        return true;
    if (open.dirty_revision == revision_)
        return open.dirty_cached;
    const auto encoded = editor::save_graph(world_.runtime(), open.document);
    open.dirty_cached = !encoded || *encoded != open.saved_text;
    open.dirty_revision = revision_;
    return open.dirty_cached;
}

std::vector<std::string> Session::dirty_graphs() const {
    std::vector<std::string> result;
    for (const auto& [key, open] : graphs_) {
        if (open && !open->scratch && graph_dirty(key))
            result.push_back(key);
    }
    std::ranges::sort(result);
    return result;
}

bool Session::graph_can_undo(std::string_view key, bool redo) const {
    const auto at = graphs_.find(std::string(key));
    return at != graphs_.end() && at->second &&
           (redo ? at->second->cursor < at->second->history.size() : at->second->cursor > 0);
}

Result<void> Session::save_graph(Project& project, std::string_view key) {
    if (editing())
        return graph_error("Finish the active field edit before saving.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph asset is not open.", ErrorCode::invalid_definition);
    if (at->second->scratch)
        return graph_error("Scratch procedural graphs have no project asset path.", ErrorCode::invalid_storage);
    ATELIER_TRY_BIND(encoded, editor::save_graph(runtime_, at->second->document));
    ATELIER_TRY(project.save_procedural_graph(key, *encoded));
    at->second->saved_text = std::move(*encoded);
    at->second->dirty_revision = 0;
    unavailable_.erase(std::pair{assets::AssetKind::procedural_graph, std::string(key)});
    ++revision_;
    return {};
}

Result<Session::OpenGraph*> Session::editable_graph(std::string_view key, bool need_program) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (need_program)
        ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    return at->second.get();
}

Result<void> Session::graph_undo(std::string_view key, bool redo) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    ATELIER_TRY(cancel_edit());
    auto& open = **open_ptr;
    if (!graph_can_undo(key, redo))
        return {};
    auto& entry = open.history[redo ? open.cursor : open.cursor - 1];
    const auto graph = open.document.graph.id();
    ATELIER_TRY(std::visit(
        [&](auto& change) -> Result<void> {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::same_as<T, GraphScalarChange>) {
                const auto& values = redo ? change.after : change.before;
                ATELIER_TRY(write_group(change.target.id(), change.paths, values));
                auto* node = find_node_by_id(open.document, change.target.id());
                (node ? node->kept : open.document.kept) = redo ? change.after_kept : change.before_kept;
                return {};
            } else if constexpr (std::same_as<T, GraphPropertyChange>) {
                ATELIER_TRY(apply_object_properties(runtime_, change.target.id(),
                                                    (redo ? change.after : change.before).root()));
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
                const bool want = redo ? change.after : change.before;
                std::shared_ptr<const procedural::FrozenOutput> snapshot;
                if (want) {
                    if (const auto prog = ensure_procedural_program()) {
                        if (!open.evaluator || open.bound_program != *prog) {
                            open.bound_program = *prog;
                            open.evaluator = std::make_unique<procedural::Evaluator>(runtime_, open.document.graph,
                                                                                     open.resources, *prog);
                        }
                        if (open.evaluator->node_output_count(change.target.id()) == 0)
                            static_cast<void>(open.evaluator->evaluate_node(change.target.id(), false));
                        snapshot = open.evaluator->retain_node_output(change.target.id());
                    }
                }
                return procedural::set_frozen(runtime_, change.target.id(), want, std::move(snapshot));
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
                        open.document.nodes.push_back(node_entry);
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
                    return procedural::set_display(runtime_, graph,
                                                   change.added ? change.after_display : change.before_display);
                }
                for (const auto& node_entry : change.entries) {
                    if (node_entry.node.registered())
                        ATELIER_TRY(procedural::remove_node(runtime_, graph, node_entry.node.id()));
                    std::erase_if(open.document.nodes,
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
                return procedural::set_display(runtime_, graph,
                                               change.added ? change.before_display : change.after_display);
            }
        },
        entry));
    if (redo)
        ++open.cursor;
    else
        --open.cursor;
    ++open.mutation_epoch;
    open.dirty_revision = 0;
    ++revision_;
    return {};
}

Result<std::string> Session::add_graph_node_impl(std::string_view key, std::string_view type_name,
                                                 std::string_view desired_name, std::array<float, 2> position,
                                                 std::string_view from_node, std::string_view from_port,
                                                 std::string_view to_node, std::string_view to_port, bool set_display) {
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
        return graph_error("Node position must be finite.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    std::string resolved_type_name(type_name);
    auto type = runtime_.find_type(resolved_type_name);
    if (!type && resolved_type_name.find('.') == std::string::npos) {
        resolved_type_name = "procedural." + resolved_type_name;
        type = runtime_.find_type(resolved_type_name);
    }
    if (!type)
        return graph_error("Unknown procedural node type: " + std::string(type_name));
    ATELIER_TRY_BIND(new_ports, procedural::ports(runtime_, *type));

    std::string name(desired_name);
    if (name.empty()) {
        const auto stem = default_node_stem(resolved_type_name);
        for (std::uint32_t index = 1;; ++index) {
            name = stem + std::to_string(index);
            if (!find_node(open.document, name))
                break;
        }
    } else {
        if (!valid_node_identifier(name))
            return graph_error("Invalid node name '" + name + "': expected [A-Za-z_][A-Za-z0-9_]*.");
        if (find_node(open.document, name))
            return graph_error("A node named '" + name + "' already exists in this graph.");
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(before_contents, procedural::contents(runtime_, graph));
    const bool make_initial_display = before_contents->nodes().empty() && before_contents->display() == ObjectId{} &&
                                      !open.document.kept.root().find("display")->has_value();

    ATELIER_TRY_BIND(created, procedural::add_node(runtime_, graph, *type));
    const ObjectId created_id = created->id();
    std::vector<GraphNodesChange::SavedInputs> saved_inputs;

    if (!from_node.empty() && !new_ports->inputs.empty()) {
        if (const auto* src = find_node(open.document, from_node); src && src->node.registered()) {
            ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
            if (const auto src_ports = procedural::ports(runtime_, *src_type);
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
                    saved_inputs.push_back({created_id, std::move(*after_in), {}});
                }
            }
        }
    }

    bool connected_to_dst = false;
    std::uint32_t dst_in_idx = 0;
    std::string dst_port_name;
    bool dst_port_multi = false;
    std::vector<procedural::Input> dst_after_snapshot;
    if (!to_node.empty() && !new_ports->outputs.empty()) {
        if (const auto* dst = find_node(open.document, to_node); dst && dst->node.registered()) {
            ATELIER_TRY_BIND(dst_type, runtime_.type_of(dst->node.id()));
            if (const auto dst_ports = procedural::ports(runtime_, *dst_type);
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
                    saved_inputs.push_back({dst->node.id(), std::move(*after_dst), std::move(*before_dst)});
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
    entry.position = position;
    open.document.nodes.push_back(entry);
    sort_graph_nodes(open.document.nodes);

    std::vector<GraphNodesChange::RetainedNodeKept> retained_node_kept;
    if (connected_to_dst) {
        if (auto* dst_mut = find_node(open.document, to_node)) {
            auto before_dst_kept = dst_mut->kept;
            ATELIER_TRY_BIND(dst_live_after,
                             format_live_port_endpoints(runtime_, open.document, dst_after_snapshot, dst_in_idx));
            append_kept_port_endpoint(dst_mut->kept, dst_port_name, dst_port_multi, name + "." + new_ports->outputs[0],
                                      *dst_live_after);
            if (dst_mut->kept != before_dst_kept)
                retained_node_kept.push_back({dst_mut->name, std::move(before_dst_kept), dst_mut->kept});
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
                                      std::move(retained_node_kept)});
    ++revision_;
    return name;
}

Result<std::string> Session::graph_add_node(std::string_view key, std::string_view type_name,
                                            std::string_view desired_name, std::array<float, 2> position) {
    return add_graph_node_impl(key, type_name, desired_name, position, {}, {}, {}, {}, false);
}

Result<std::string> Session::graph_place_node(std::string_view key, std::string_view type_name,
                                              std::array<float, 2> position, std::string_view from_node,
                                              std::string_view from_port, std::string_view to_node,
                                              std::string_view to_port, bool set_display) {
    return add_graph_node_impl(key, type_name, {}, position, from_node, from_port, to_node, to_port, set_display);
}

Result<void> Session::graph_remove_nodes(std::string_view key, std::span<const std::string> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    std::vector<std::string> unique_names;
    std::unordered_set<ObjectId> removed_ids;
    for (const auto& name : names) {
        if (std::ranges::find(unique_names, name) != unique_names.end())
            continue;
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error("No node named '" + name + "' in this graph.");
        unique_names.push_back(name);
        if (item->node.registered())
            removed_ids.insert(item->node.id());
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
            change.saved_inputs.push_back({node_id, std::move(*into)});
    }
    for (const auto& name : unique_names) {
        auto* item = find_node(open.document, name);
        change.entries.push_back(*item);
        if (item->node.registered())
            ATELIER_TRY(procedural::remove_node(runtime_, graph, item->node.id()));
    }
    open.document.template_node = change.after_template;
    open.document.kept = change.after_kept;
    std::erase_if(open.document.nodes, [&](const GraphNode& item) {
        return std::ranges::find(unique_names, item.name) != unique_names.end();
    });

    ATELIER_TRY_BIND(listed_after, procedural::contents(runtime_, graph));
    const auto uses_removed_node = [&](std::string_view ep) noexcept {
        const auto dot = ep.find('.');
        if (dot == std::string_view::npos)
            return false;
        const auto upstream = ep.substr(0, dot);
        return std::ranges::find(unique_names, upstream) != unique_names.end();
    };
    for (auto& surv : open.document.nodes) {
        const auto cur_inputs = *surv.kept.root().find("inputs");
        if (!cur_inputs || cur_inputs->kind() != json::Kind::object)
            continue;
        auto before_surv_kept = surv.kept;
        std::vector<std::string> kept_ports;
        for (const auto& [pname, _] : *cur_inputs->object_items())
            kept_ports.emplace_back(pname);
        if (surv.node.registered()) {
            ATELIER_TRY_BIND(surv_type, runtime_.type_of(surv.node.id()));
            ATELIER_TRY_BIND(surv_ports, procedural::ports(runtime_, *surv_type));
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
            change.retained_node_kept.push_back({surv.name, std::move(before_surv_kept), surv.kept});
    }

    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<std::vector<std::string>>
Session::graph_duplicate_nodes(std::string_view key, std::span<const std::string> names, std::array<float, 2> offset) {
    if (!std::isfinite(offset[0]) || !std::isfinite(offset[1]))
        return graph_error("Duplicate offset must be finite.");
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    if (names.empty())
        return std::vector<std::string>{};
    auto& open = **open_ptr;
    std::vector<std::string> unique_names;
    for (const auto& name : names) {
        if (std::ranges::find(unique_names, name) != unique_names.end())
            continue;
        if (!find_node(open.document, name))
            return graph_error("No node named '" + name + "' in this graph.");
        unique_names.push_back(name);
    }
    if (unique_names.empty())
        return std::vector<std::string>{};

    ATELIER_TRY(cancel_edit());
    const ObjectId graph = open.document.graph.id();
    ATELIER_TRY_BIND(before_contents, procedural::contents(runtime_, graph));

    std::vector<std::string> created_names;
    created_names.reserve(unique_names.size());
    std::unordered_map<std::string, std::string> name_remap;
    for (const auto& src_name : unique_names) {
        std::string stem = src_name;
        while (stem.size() > 1 && stem.back() >= '0' && stem.back() <= '9')
            stem.pop_back();
        std::string candidate;
        for (std::uint32_t idx = 1;; ++idx) {
            candidate = stem + std::to_string(idx);
            if (!find_node(open.document, candidate) &&
                std::ranges::find(created_names, candidate) == created_names.end())
                break;
        }
        created_names.push_back(candidate);
        name_remap.emplace(src_name, std::move(candidate));
    }

    std::vector<ObjectId> added_live_ids;
    bool committed = false;
    struct RollbackGuard {
        Runtime& runtime;

        ObjectId graph{};

        std::vector<ObjectId>& ids;

        bool& ok;

        ~RollbackGuard() {
            if (!ok) {
                for (auto it = ids.rbegin(); it != ids.rend(); ++it)
                    static_cast<void>(procedural::remove_node(runtime, graph, *it));
            }
        }
    } rollback{runtime_, graph, added_live_ids, committed};

    std::vector<GraphNode> copy_entries;
    copy_entries.reserve(unique_names.size());
    std::unordered_map<ObjectId, ObjectId> id_remap;
    for (std::size_t i = 0; i < unique_names.size(); ++i) {
        const auto* src = find_node(open.document, unique_names[i]);
        GraphNode copy_entry;
        copy_entry.name = created_names[i];
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
                    if (dot != text->npos) {
                        const std::string upstream(text->substr(0, dot));
                        if (const auto mapped = name_remap.find(upstream); mapped != name_remap.end()) {
                            next.set_validated(endpoint, std::string(mapped->second).append(text->substr(dot)));
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
            added_live_ids.push_back(dst_id);
            ATELIER_TRY(apply_object_properties(runtime_, dst_id, props->root()));
            if (*is_byp)
                ATELIER_TRY(procedural::set_bypassed(runtime_, dst_id, true));
            if (*is_frz)
                ATELIER_TRY(procedural::set_frozen(runtime_, dst_id, true, *frz_snap));
            id_remap.emplace(src->node.id(), dst_id);
            copy_entry.node = std::move(*created_node);
        }
        copy_entries.push_back(std::move(copy_entry));
    }

    std::vector<GraphNodesChange::SavedInputs> saved_inputs;
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
            const auto mapped_from = id_remap.find(in.from);
            const ObjectId from_id = mapped_from != id_remap.end() ? mapped_from->second : in.from;
            ATELIER_TRY(procedural::connect(runtime_, graph, from_id, in.output, dst_id, in.input));
        }
        ATELIER_TRY_BIND(listed_now, procedural::contents(runtime_, graph));
        ATELIER_TRY_BIND(after_in, procedural::inputs(runtime_, *listed_now, dst_id));
        saved_inputs.push_back({dst_id, std::move(*after_in), {}});
    }

    committed = true;
    for (const auto& entry : copy_entries)
        open.document.nodes.push_back(entry);
    sort_graph_nodes(open.document.nodes);
    open.push_change(GraphNodesChange{
        true, std::move(copy_entries), std::move(saved_inputs), before_contents->display(), before_contents->display(),
        open.document.template_node, open.document.template_node, open.document.kept, open.document.kept});
    ++revision_;
    return created_names;
}

Result<void> Session::graph_rename_node(std::string_view key, std::string_view old_name, std::string_view new_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!valid_node_identifier(new_name))
        return graph_error("Invalid node name '" + std::string(new_name) + "': expected [A-Za-z_][A-Za-z0-9_]*.");
    auto& open = **open_ptr;
    auto* item = find_node(open.document, old_name);
    if (!item)
        return graph_error("No node named '" + std::string(old_name) + "' in this graph.");
    if (old_name == new_name)
        return {};
    if (find_node(open.document, new_name))
        return graph_error("A node named '" + std::string(new_name) + "' already exists in this graph.");
    ATELIER_TRY(cancel_edit());
    GraphRenameChange change{std::string(old_name), std::string(new_name), {}};
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
            if (dot != text.npos && text.substr(0, dot) == old_name) {
                next.set_validated(endpoint, std::string(new_name).append(text.substr(dot)));
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
            change.references.push_back({node.name, node.kept, next.finish()});
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
        change.references.push_back({{}, open.document.kept, next.finish()});
        open.document.kept = change.references.back().after;
    }
    item->name = std::string(new_name);
    sort_graph_nodes(open.document.nodes);
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_move_canvas_items(std::string_view key,
                                              std::span<const std::pair<std::string, std::array<float, 2>>> node_moves,
                                              std::span<const std::pair<std::string, std::array<float, 2>>> note_moves,
                                              std::span<const std::pair<std::string, std::array<float, 2>>> box_moves) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    GraphMoveChange change;
    for (const auto& [name, next_pos] : node_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Node position must be finite.");
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error("No node named '" + name + "' in this graph.");
        if (item->position != next_pos)
            change.moves.push_back({name, item->position, next_pos});
    }
    for (const auto& [name, next_pos] : note_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Sticky note position must be finite.");
        const auto* item = find_note(open.document, name);
        if (!item)
            return graph_error("No sticky note named '" + name + "' in this graph.");
        if (item->position != next_pos)
            change.note_moves.push_back({name, item->position, next_pos});
    }
    for (const auto& [name, next_pos] : box_moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Network box position must be finite.");
        const auto* item = find_box(open.document, name);
        if (!item)
            return graph_error("No network box named '" + name + "' in this graph.");
        if (item->position != next_pos)
            change.box_moves.push_back({name, item->position, next_pos});
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

Result<void> Session::graph_move_nodes(std::string_view key,
                                       std::span<const std::pair<std::string, std::array<float, 2>>> moves,
                                       bool amend_creation) {
    if (amend_creation && !moves.empty()) {
        ATELIER_TRY_BIND(open_ptr, editable_graph(key));
        auto& open = **open_ptr;
        if (open.cursor > 0) {
            if (auto* created = std::get_if<GraphNodesChange>(&open.history[open.cursor - 1]);
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

Result<void> Session::graph_auto_layout(std::string_view key) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const std::size_t n = open.document.nodes.size();
    if (n == 0)
        return {};

    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    std::unordered_map<ObjectId, std::size_t> id_to_idx;
    id_to_idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        if (open.document.nodes[i].node.registered())
            id_to_idx.emplace(open.document.nodes[i].node.id(), i);

    std::vector<std::vector<std::size_t>> parents(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!open.document.nodes[i].node.registered())
            continue;
        ATELIER_TRY_BIND(into, procedural::inputs(runtime_, *listed, open.document.nodes[i].node.id()));
        for (const auto& edge : *into) {
            if (const auto found = id_to_idx.find(edge.from); found != id_to_idx.end() && found->second != i)
                parents[i].push_back(found->second);
        }
    }

    std::vector<std::size_t> rank(n, 0);
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
    std::vector<std::vector<std::size_t>> layers(max_rank + 1);
    for (std::size_t i = 0; i < n; ++i)
        layers[rank[i]].push_back(i);

    std::vector<float> order_x(n, 0.f);
    constexpr float x_pitch = 196.f;
    constexpr float y_pitch = 96.f;
    std::vector<std::pair<std::string, std::array<float, 2>>> moves;
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
            moves.emplace_back(open.document.nodes[idx].name, std::array{x, y});
        }
    }
    return graph_move_nodes(key, moves);
}

Result<void> Session::graph_connect(std::string_view key, std::string_view from_node, std::string_view from_port,
                                    std::string_view to_node, std::string_view to_port) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    const auto* src = find_node(open.document, from_node);
    auto* dst = find_node(open.document, to_node);
    if (!src || !src->node.registered())
        return graph_error("Upstream node '" + std::string(from_node) + "' is missing or unresolved.");
    if (!dst || !dst->node.registered())
        return graph_error("Downstream node '" + std::string(to_node) + "' is missing or unresolved.");

    ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
    ATELIER_TRY_BIND(dst_type, runtime_.type_of(dst->node.id()));
    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, *dst_type));
    if (src_ports->outputs.empty())
        return graph_error("Node '" + src->name + "' has no output ports.");
    if (dst_ports->inputs.empty())
        return graph_error("Node '" + dst->name + "' has no input ports.");

    std::uint32_t out_idx = 0;
    if (!from_port.empty()) {
        const auto found = std::ranges::find(src_ports->outputs, from_port);
        if (found == src_ports->outputs.end())
            return graph_error("Node '" + src->name + "' has no output port '" + std::string(from_port) + "'.");
        out_idx = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
    }
    std::uint32_t in_idx = 0;
    if (!to_port.empty()) {
        const auto found = std::ranges::find(dst_ports->inputs, to_port);
        if (found == dst_ports->inputs.end())
            return graph_error("Node '" + dst->name + "' has no input port '" + std::string(to_port) + "'.");
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
    ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, *after_inputs, in_idx));

    const auto before_kept = dst->kept;
    append_kept_port_endpoint(dst->kept, dst_ports->inputs[in_idx], dst_ports->multiple[in_idx],
                              src->name + "." + src_ports->outputs[out_idx], *live_after);

    open.push_change(GraphConnectionChange{
        {{dst->node, std::move(*before_inputs), std::move(*after_inputs), before_kept, dst->kept}}});
    ++revision_;
    return {};
}

Result<void> Session::graph_disconnect(std::string_view key, std::string_view to_node, std::string_view to_port,
                                       std::string_view from_node, std::string_view from_port) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, true));
    auto& open = **open_ptr;
    auto* dst = find_node(open.document, to_node);
    if (!dst || !dst->node.registered())
        return graph_error("Downstream node '" + std::string(to_node) + "' is missing or unresolved.");

    ATELIER_TRY_BIND(dst_type, runtime_.type_of(dst->node.id()));
    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, *dst_type));
    std::optional<std::uint32_t> in_idx;
    if (!to_port.empty()) {
        const auto found = std::ranges::find(dst_ports->inputs, to_port);
        if (found == dst_ports->inputs.end())
            return graph_error("Node '" + dst->name + "' has no input port '" + std::string(to_port) + "'.");
        in_idx = static_cast<std::uint32_t>(found - dst_ports->inputs.begin());
    }

    const auto kept_endpoint_matches = [&](std::string_view ep) noexcept {
        if (from_node.empty())
            return true;
        const auto dot = ep.find('.');
        if (dot == std::string_view::npos || ep.substr(0, dot) != from_node)
            return false;
        return from_port.empty() || ep.substr(dot + 1) == from_port;
    };

    std::optional<ObjectId> filter_from;
    std::optional<std::uint32_t> filter_out;
    bool matches_kept_only = false;
    if (!from_node.empty()) {
        const auto* src = find_node(open.document, from_node);
        if (src && src->node.registered()) {
            filter_from = src->node.id();
            if (!from_port.empty()) {
                ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
                ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
                const auto found = std::ranges::find(src_ports->outputs, from_port);
                if (found == src_ports->outputs.end()) {
                    if (!kept_port_has_match(dst->kept, to_port, kept_endpoint_matches))
                        return graph_error("Node '" + src->name + "' has no output port '" + std::string(from_port) +
                                           "'.");
                    matches_kept_only = true;
                } else {
                    filter_out = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
                }
            }
        } else if (kept_port_has_match(dst->kept, to_port, kept_endpoint_matches)) {
            matches_kept_only = true;
        } else {
            return graph_error("Upstream node '" + std::string(from_node) + "' is missing or unresolved.");
        }
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(before_inputs, procedural::inputs(runtime_, *listed, dst->node.id()));

    std::vector<procedural::Input> remaining;
    if (matches_kept_only) {
        remaining = *before_inputs;
    } else {
        for (const auto& item : *before_inputs) {
            const bool match_in = !in_idx || item.input == *in_idx;
            const bool match_from = !filter_from || item.from == *filter_from;
            const bool match_out = !filter_out || item.output == *filter_out;
            if (!(match_in && match_from && match_out))
                remaining.push_back(item);
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
                std::vector<std::string> kept_ports;
                for (const auto& [pname, _] : *cur_inputs->object_items())
                    kept_ports.emplace_back(pname);
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

Result<void> Session::graph_disconnect_edges(std::string_view key, std::span<const GraphConnectionView> edges) {
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

        std::string endpoint;
    };
    std::vector<ResolvedCut> cuts;
    cuts.reserve(edges.size());
    for (const auto& edge : edges) {
        const auto* src = find_node(open.document, edge.from_node);
        const auto* dst = find_node(open.document, edge.to_node);
        if (!src || !src->node.registered() || !dst || !dst->node.registered())
            continue;
        ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
        ATELIER_TRY_BIND(dst_type, runtime_.type_of(dst->node.id()));
        ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
        ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, *dst_type));
        const auto out_it = std::ranges::find(src_ports->outputs, edge.from_port);
        const auto in_it = std::ranges::find(dst_ports->inputs, edge.to_port);
        if (out_it == src_ports->outputs.end() || in_it == dst_ports->inputs.end())
            continue;
        cuts.push_back({src->node.id(), static_cast<std::uint32_t>(out_it - src_ports->outputs.begin()), dst->node.id(),
                        static_cast<std::uint32_t>(in_it - dst_ports->inputs.begin()), src->name + "." + *out_it});
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
        std::vector<procedural::Input> remaining;
        for (const auto& in : *before_inputs) {
            const bool cut = std::ranges::any_of(cuts, [&](const ResolvedCut& c) {
                return c.to == to_id && c.from == in.from && c.out_idx == in.output && c.in_idx == in.input;
            });
            if (!cut)
                remaining.push_back(in);
        }
        const bool live_changed = remaining.size() != before_inputs->size();
        if (live_changed)
            ATELIER_TRY(replace_node_inputs(runtime_, graph, to_id, remaining));

        auto before_kept = dst.kept;
        if (dst.kept.root().find("inputs")->has_value()) {
            ATELIER_TRY_BIND(dst_type, runtime_.type_of(to_id));
            ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, *dst_type));
            for (std::uint32_t p_idx = 0; p_idx < dst_ports->inputs.size(); ++p_idx) {
                if (std::ranges::none_of(cuts,
                                         [&](const ResolvedCut& c) { return c.to == to_id && c.in_idx == p_idx; }))
                    continue;
                ATELIER_TRY_BIND(live_after, format_live_port_endpoints(runtime_, open.document, remaining, p_idx));
                filter_kept_port_endpoints(dst.kept, dst_ports->inputs[p_idx], dst_ports->multiple[p_idx], *live_after,
                                           [&](std::string_view ep) {
                                               return std::ranges::any_of(cuts, [&](const ResolvedCut& c) {
                                                   return c.to == to_id && c.in_idx == p_idx && c.endpoint == ep;
                                               });
                                           });
            }
        }
        if (!live_changed && dst.kept == before_kept)
            continue;
        change.targets.push_back(
            {dst.node, std::move(*before_inputs), std::move(remaining), std::move(before_kept), dst.kept});
    }
    if (!change.targets.empty()) {
        open.push_change(std::move(change));
        ++revision_;
    }
    return {};
}

Result<void> Session::graph_rewire(std::string_view key, const GraphConnectionView& remove_edge,
                                   std::string_view new_to_node, std::string_view new_to_port) {
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

    ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
    ATELIER_TRY_BIND(old_type, runtime_.type_of(old_dst->node.id()));
    ATELIER_TRY_BIND(new_type, runtime_.type_of(new_dst->node.id()));
    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
    ATELIER_TRY_BIND(old_ports, procedural::ports(runtime_, *old_type));
    ATELIER_TRY_BIND(new_ports, procedural::ports(runtime_, *new_type));
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
    std::vector<procedural::Input> new_before_vec;
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
    const std::string moved_ep = src->name + "." + src_ports->outputs[out_idx];

    ATELIER_TRY_BIND(old_after, procedural::inputs(runtime_, *listed, old_dst->node.id()));
    ATELIER_TRY_BIND(old_live_after, format_live_port_endpoints(runtime_, open.document, *old_after, old_in_idx));
    filter_kept_port_endpoints(old_dst->kept, old_ports->inputs[old_in_idx], old_ports->multiple[old_in_idx],
                               *old_live_after, [&](std::string_view ep) { return ep == moved_ep; });

    GraphConnectionChange change;
    if (old_dst == new_dst) {
        ATELIER_TRY_BIND(same_new_live, format_live_port_endpoints(runtime_, open.document, *old_after, new_in_idx));
        append_kept_port_endpoint(new_dst->kept, new_ports->inputs[new_in_idx], new_ports->multiple[new_in_idx],
                                  moved_ep, *same_new_live);
        change.targets.push_back(
            {old_dst->node, std::move(*old_before), std::move(*old_after), old_before_kept, new_dst->kept});
    } else {
        ATELIER_TRY_BIND(new_after, procedural::inputs(runtime_, *listed, new_dst->node.id()));
        ATELIER_TRY_BIND(new_live_after, format_live_port_endpoints(runtime_, open.document, *new_after, new_in_idx));
        append_kept_port_endpoint(new_dst->kept, new_ports->inputs[new_in_idx], new_ports->multiple[new_in_idx],
                                  moved_ep, *new_live_after);
        change.targets.push_back(
            {old_dst->node, std::move(*old_before), std::move(*old_after), old_before_kept, old_dst->kept});
        change.targets.push_back(
            {new_dst->node, std::move(new_before_vec), std::move(*new_after), new_before_kept, new_dst->kept});
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_splice_node(std::string_view key, std::string_view node_name, std::array<float, 2> position,
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

    ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
    ATELIER_TRY_BIND(mid_type, runtime_.type_of(mid->node.id()));
    ATELIER_TRY_BIND(dst_type, runtime_.type_of(dst->node.id()));
    ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
    ATELIER_TRY_BIND(mid_ports, procedural::ports(runtime_, *mid_type));
    ATELIER_TRY_BIND(dst_ports, procedural::ports(runtime_, *dst_type));
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

    std::vector<procedural::Input> mid_after;
    for (const auto& in : *mid_before) {
        if (mid_ports->multiple[0] || in.input != 0)
            mid_after.push_back(in);
    }
    if (!std::ranges::any_of(mid_after, [&](const procedural::Input& in) {
            return in.from == src->node.id() && in.output == out_idx && in.input == 0;
        })) {
        mid_after.push_back({src->node.id(), out_idx, 0});
    }

    std::vector<procedural::Input> dst_after;
    bool inserted = false;
    const bool already_has_mid = std::ranges::any_of(*dst_before, [&](const procedural::Input& in) {
        return in.from == mid->node.id() && in.output == 0 && in.input == in_idx;
    });
    for (const auto& in : *dst_before) {
        if (in.from == src->node.id() && in.output == out_idx && in.input == in_idx) {
            if (!already_has_mid && !inserted) {
                dst_after.push_back({mid->node.id(), 0, in_idx});
                inserted = true;
            }
            continue;
        }
        dst_after.push_back(in);
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

    const std::string src_ep = src->name + "." + src_ports->outputs[out_idx];
    const std::string mid_ep = mid->name + "." + mid_ports->outputs[0];
    ATELIER_TRY_BIND(mid_live_after, format_live_port_endpoints(runtime_, open.document, mid_after, 0));
    ATELIER_TRY_BIND(dst_live_after, format_live_port_endpoints(runtime_, open.document, dst_after, in_idx));

    const auto mid_before_kept = mid->kept;
    append_kept_port_endpoint(mid->kept, mid_ports->inputs[0], mid_ports->multiple[0], src_ep, *mid_live_after);
    const auto dst_before_kept = dst->kept;
    replace_kept_port_endpoint(dst->kept, dst_ports->inputs[in_idx], dst_ports->multiple[in_idx], src_ep, mid_ep,
                               *dst_live_after);

    GraphConnectionChange change;
    change.targets.push_back({mid->node, std::move(*mid_before), std::move(mid_after), mid_before_kept, mid->kept});
    change.targets.push_back({dst->node, std::move(*dst_before), std::move(dst_after), dst_before_kept, dst->kept});
    if (mid->position != position) {
        change.moves.push_back({mid->name, mid->position, position});
        mid->position = position;
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_set_display(std::string_view key, std::string_view node_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    ObjectId target{};
    if (!node_name.empty()) {
        const auto* item = find_node(open.document, node_name);
        if (!item || !item->node.registered())
            return graph_error("Display target '" + std::string(node_name) + "' is missing or unresolved.");
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

Result<void> Session::graph_set_template(std::string_view key, std::string_view node_name) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    ObjectId target{};
    if (!node_name.empty()) {
        const auto* item = find_node(open.document, node_name);
        if (!item || !item->node.registered())
            return graph_error("Template target '" + std::string(node_name) + "' is missing or unresolved.");
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

Result<void> Session::graph_set_bypass(std::string_view key, std::string_view node_name, bool bypassed) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, node_name);
    if (!item || !item->node.registered())
        return graph_error("Bypass target '" + std::string(node_name) + "' is missing or unresolved.");
    ATELIER_TRY_BIND(before, procedural::bypassed(runtime_, item->node.id()));
    if (*before == bypassed)
        return {};
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY(procedural::set_bypassed(runtime_, item->node.id(), bypassed));
    open.push_change(GraphBypassChange{item->node, *before, bypassed});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_frozen(std::string_view key, std::string_view node_name, bool frozen,
                                       Project* project) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key, frozen));
    auto& open = **open_ptr;
    auto* item = find_node(open.document, node_name);
    if (!item || !item->node.registered())
        return graph_error("Freeze target '" + std::string(node_name) + "' is missing or unresolved.");
    ATELIER_TRY_BIND(before, procedural::frozen(runtime_, item->node.id()));
    if (*before == frozen)
        return {};
    std::shared_ptr<const procedural::FrozenOutput> snapshot;
    if (frozen) {
        ATELIER_TRY_BIND(program, ensure_procedural_program());
        if (project)
            open.resources.bind(project);
        if (!open.evaluator || open.bound_program != *program) {
            open.bound_program = *program;
            open.evaluator =
                std::make_unique<procedural::Evaluator>(runtime_, open.document.graph, open.resources, *program);
        }
        if (open.evaluator->node_output_count(item->node.id()) == 0)
            ATELIER_TRY(open.evaluator->evaluate_node(item->node.id(), false));
        snapshot = open.evaluator->retain_node_output(item->node.id());
        if (!snapshot) {
            if (const auto* st = open.evaluator->status(item->node.id()); st && st->error)
                return std::unexpected(*st->error);
            return graph_error("Cannot freeze '" + std::string(node_name) + "' without a valid cooked output.");
        }
    }
    ATELIER_TRY(cancel_edit());
    ATELIER_TRY(procedural::set_frozen(runtime_, item->node.id(), frozen, std::move(snapshot)));
    open.push_change(GraphFreezeChange{item->node, *before, frozen});
    ++revision_;
    return {};
}

Result<void> Session::graph_set_node_appearance(std::string_view key, std::span<const std::string> names,
                                                std::optional<GraphNodeShape> shape,
                                                std::optional<std::optional<std::uint32_t>> color,
                                                std::optional<std::string> comment) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    if (shape && static_cast<std::uint8_t>(*shape) > static_cast<std::uint8_t>(GraphNodeShape::circle))
        return graph_error("Invalid node shape preset.");
    if (comment && !StringView(*comment).is_valid_utf8())
        return graph_error("Node comment must be valid UTF-8.");
    auto& open = **open_ptr;
    GraphAppearanceChange change;
    std::vector<std::string> seen;
    for (const auto& name : names) {
        if (std::ranges::find(seen, name) != seen.end())
            continue;
        seen.push_back(name);
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error("No node named '" + name + "' in this graph.");
        const GraphNodeShape next_shape = shape.value_or(item->shape);
        std::optional<std::uint32_t> next_color = item->color;
        if (color) {
            next_color = *color ? std::optional<std::uint32_t>{**color & 0xffffffu} : std::nullopt;
        }
        const std::string next_comment = comment.value_or(item->comment);
        if (next_shape != item->shape || next_color != item->color || next_comment != item->comment) {
            change.entries.push_back(
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

Result<std::string> Session::graph_add_note(std::string_view key, std::array<float, 2> position,
                                            std::array<float, 2> size, std::string_view title, std::string_view text,
                                            std::uint32_t color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(size[0]) ||
        !std::isfinite(size[1]) || size[0] <= 0.f || size[1] <= 0.f)
        return graph_error("Sticky note position and size must be finite and positive.");
    if (!StringView(title).is_valid_utf8() || !StringView(text).is_valid_utf8())
        return graph_error("Sticky note text must be valid UTF-8.");
    auto& open = **open_ptr;
    std::string name;
    for (std::uint32_t idx = 1;; ++idx) {
        name = "note" + std::to_string(idx);
        if (!find_note(open.document, name))
            break;
    }
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    change.after_notes.push_back({name, position, size, std::string(title), std::string(text), color & 0xffffffu});
    sort_graph_notes(change.after_notes);
    open.document.notes = change.after_notes;
    open.push_change(std::move(change));
    ++revision_;
    return name;
}

Result<void> Session::graph_update_note(std::string_view key, std::string_view name,
                                        std::optional<std::array<float, 2>> position,
                                        std::optional<std::array<float, 2>> size, std::optional<std::string> title,
                                        std::optional<std::string> text, std::optional<std::uint32_t> color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    const auto* existing = find_note(open.document, name);
    if (!existing)
        return graph_error("No sticky note named '" + std::string(name) + "' in this graph.");
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
    if (title) {
        if (!StringView(*title).is_valid_utf8())
            return graph_error("Sticky note title must be valid UTF-8.");
        updated.title = *title;
    }
    if (text) {
        if (!StringView(*text).is_valid_utf8())
            return graph_error("Sticky note text must be valid UTF-8.");
        updated.text = *text;
    }
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

Result<void> Session::graph_remove_notes(std::string_view key, std::span<const std::string> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    if (std::erase_if(change.after_notes, [&](const GraphStickyNote& item) {
            return std::ranges::find(names, item.name) != names.end();
        }) == 0)
        return {};
    ATELIER_TRY(cancel_edit());
    open.document.notes = change.after_notes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<std::string> Session::graph_add_box(std::string_view key, std::array<float, 2> position,
                                           std::array<float, 2> size, std::string_view title, std::uint32_t color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(size[0]) ||
        !std::isfinite(size[1]) || size[0] <= 0.f || size[1] <= 0.f)
        return graph_error("Network box position and size must be finite and positive.");
    if (!StringView(title).is_valid_utf8())
        return graph_error("Network box title must be valid UTF-8.");
    auto& open = **open_ptr;
    std::string name;
    for (std::uint32_t idx = 1;; ++idx) {
        name = "box_frame" + std::to_string(idx);
        if (!find_box(open.document, name))
            break;
    }
    ATELIER_TRY(cancel_edit());
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    change.after_boxes.push_back({name, position, size, std::string(title), color & 0xffffffu});
    sort_graph_boxes(change.after_boxes);
    open.document.boxes = change.after_boxes;
    open.push_change(std::move(change));
    ++revision_;
    return name;
}

Result<void> Session::graph_update_box(std::string_view key, std::string_view name,
                                       std::optional<std::array<float, 2>> position,
                                       std::optional<std::array<float, 2>> size, std::optional<std::string> title,
                                       std::optional<std::uint32_t> color) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    auto& open = **open_ptr;
    const auto* existing = find_box(open.document, name);
    if (!existing)
        return graph_error("No network box named '" + std::string(name) + "' in this graph.");
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
    if (title) {
        if (!StringView(*title).is_valid_utf8())
            return graph_error("Network box title must be valid UTF-8.");
        updated.title = *title;
    }
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

Result<void> Session::graph_remove_boxes(std::string_view key, std::span<const std::string> names) {
    ATELIER_TRY_BIND(open_ptr, editable_graph(key));
    if (names.empty())
        return {};
    auto& open = **open_ptr;
    GraphAnnotationChange change{open.document.notes, open.document.notes, open.document.boxes, open.document.boxes};
    if (std::erase_if(change.after_boxes, [&](const GraphNetworkBox& item) {
            return std::ranges::find(names, item.name) != names.end();
        }) == 0)
        return {};
    ATELIER_TRY(cancel_edit());
    open.document.boxes = change.after_boxes;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_set_properties(std::string_view key, std::string_view node_name, json::Value properties) {
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
            return graph_error("Target node '" + std::string(node_name) + "' is missing or unresolved.");
        target_root = target_node->node;
    }

    ATELIER_TRY(cancel_edit());
    ATELIER_TRY_BIND(before, capture_object_properties(runtime_, target_root.id()));
    auto applied = apply_object_properties(runtime_, target_root.id(), properties);
    if (!applied) {
        (void)apply_object_properties(runtime_, target_root.id(), before->root());
        return applied;
    }
    ATELIER_TRY_BIND(after, capture_object_properties(runtime_, target_root.id()));

    const auto before_kept = target_node ? target_node->kept : open.document.kept;
    json::Builder next_kept(before_kept);
    std::vector<std::string> path;
    if (target_node)
        path.emplace_back("properties");
    const auto clear_edited = [&](auto&& self, json::Value patch) -> void {
        const auto members = *patch.object_items();
        for (const auto& [member, value] : members) {
            path.emplace_back(member);
            if (value.kind() == json::Kind::object)
                self(self, value);
            else
                detail::erase_path(next_kept, next_kept.root(), path);
            path.pop_back();
        }
    };
    clear_edited(clear_edited, properties);
    const auto after_kept = next_kept.finish();

    const auto before_bytes = json::write(before->root());
    const auto after_bytes = json::write(after->root());
    const auto bk_bytes = json::write(before_kept.root());
    const auto ak_bytes = json::write(after_kept.root());
    if (std::ranges::equal(before_bytes, after_bytes) && std::ranges::equal(bk_bytes, ak_bytes))
        return {};

    if (target_node)
        target_node->kept = after_kept;
    else
        open.document.kept = after_kept;
    open.push_change(
        GraphPropertyChange{std::move(target_root), std::move(*before), std::move(*after), before_kept, after_kept});
    ++revision_;
    return {};
}

Result<void> Session::set_string_property(ObjectId object, std::span<const FieldId> path, std::string_view value) {
    if (world_.running() || world_.closed())
        return graph_error("Properties require an open, stopped document.", ErrorCode::invalid_definition);
    const auto graph_key = graph_of_object(object);
    if (!graph_key)
        return graph_error("String property editing requires a procedural graph object.",
                           ErrorCode::invalid_definition);
    if (!StringView(value).is_valid_utf8())
        return graph_error("Property string must be valid UTF-8.");
    ATELIER_TRY_BIND(string_type, runtime_.native_type<String>());
    ATELIER_TRY_BIND(type, runtime_.type_of(object));
    std::vector<std::string> names;
    for (const auto step : path) {
        ATELIER_TRY_BIND(info, runtime_.describe(*type));
        const auto field = std::ranges::find((*info)->fields, step, &FieldInfo::id);
        if (field == (*info)->fields.end())
            return graph_error("Unknown property field.");
        if (field->read_only)
            return graph_error("Read-only fields cannot be edited in the inspector.", ErrorCode::invalid_definition);
        names.emplace_back(field->name);
        type = field->value.type;
    }
    if (*type != *string_type)
        return graph_error("Target field is not a String.", ErrorCode::type_mismatch);

    json::Builder patch;
    const auto leaf = patch.object_path_validated(patch.root(), names);
    patch.set_validated(leaf, value);
    const auto node_name = graph_node_name(object);
    return graph_set_properties(*graph_key, node_name.value_or(""), patch.root());
}

Result<GraphEvaluation> Session::evaluate_graph(std::string_view key, Project* project, ObjectId inspected_node,
                                                bool record_timings) {
    if (world_.closed())
        return graph_error("Graph evaluation requires an open Session.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
    ATELIER_TRY_BIND(program, ensure_procedural_program());
    open.resources.bind(project);
    if (!open.evaluator || open.bound_program != *program) {
        open.bound_program = *program;
        open.evaluator =
            std::make_unique<procedural::Evaluator>(runtime_, open.document.graph, open.resources, *program);
    }
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
        if (const auto* st = open.evaluator->status(result.display_node); st && st->error)
            result.display_error = st->error;
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
        if (const auto* st = open.evaluator->status(inspected_node); st && st->error)
            result.inspected_error = st->error;
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

Result<std::shared_ptr<const GraphInspectView>> Session::graph_topology(std::string_view key) {
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end())
        return graph_error("Procedural graph is not open.");
    auto& open = *at->second;
    if (open.topology && open.topology_revision == revision_ && open.topology_generation == open.evaluation_generation)
        return open.topology;
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
    view.dirty = graph_dirty(key);
    view.can_undo = graph_can_undo(key, false);
    view.can_redo = graph_can_undo(key, true);
    view.issues = open.document.issues;
    view.notes = open.document.notes;
    view.boxes = open.document.boxes;

    if (listed->display() != ObjectId{}) {
        if (const auto* disp_node = find_node_by_id(open.document, listed->display()))
            view.display = disp_node->name;
    } else if (const auto kept_disp = open.document.kept.root().string({"display"})) {
        view.display = std::string(*kept_disp);
    }
    if (open.document.template_node != ObjectId{}) {
        if (const auto* tmpl_node = find_node_by_id(open.document, open.document.template_node))
            view.template_node = tmpl_node->name;
    } else if (const auto kept_tmpl = open.document.kept.root().string({"template"})) {
        view.template_node = std::string(*kept_tmpl);
    }

    for (const auto& item : open.document.nodes) {
        GraphNodeView node_view;
        node_view.name = item.name;
        node_view.position = item.position;
        node_view.shape = item.shape;
        node_view.color = item.color;
        node_view.comment = item.comment;
        if (!item.node.registered()) {
            node_view.placeholder = true;
            node_view.is_display = item.name == view.display;
            node_view.is_template = item.name == view.template_node;
            if (const auto saved_type = item.kept.root().string({"type"}))
                node_view.type = std::string(*saved_type);
            if (const auto saved_bypass = item.kept.root().boolean({"bypass"}))
                node_view.is_bypassed = *saved_bypass;
            view.nodes.push_back(std::move(node_view));
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
        node_view.type = std::string((*info)->name);
        if (auto p = procedural::ports(runtime_, *type)) {
            node_view.ports = {std::move(p->inputs), std::move(p->multiple), std::move(p->outputs)};
        }
        if (open.evaluator) {
            if (const auto* st = open.evaluator->status(item.node.id()))
                node_view.status = *st;
        }

        ATELIER_TRY_BIND(conns, procedural::inputs(runtime_, *listed, item.node.id()));
        for (const auto& conn : *conns) {
            const auto* src = find_node_by_id(open.document, conn.from);
            if (!src)
                continue;
            ATELIER_TRY_BIND(src_type, runtime_.type_of(conn.from));
            ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
            const std::string from_port =
                conn.output < src_ports->outputs.size() ? src_ports->outputs[conn.output] : std::to_string(conn.output);
            const std::string to_port = conn.input < node_view.ports.inputs.size() ? node_view.ports.inputs[conn.input]
                                                                                   : std::to_string(conn.input);
            view.connections.push_back({src->name, from_port, conn.output, item.name, to_port, conn.input});
        }
        view.nodes.push_back(std::move(node_view));
    }

    open.topology_revision = revision_;
    open.topology_generation = open.evaluation_generation;
    open.topology = std::move(snapshot);
    return open.topology;
}

Result<GraphInspectView> Session::inspect_graph(std::string_view key, Project* project, ObjectId inspected_node,
                                                bool record_timings, std::size_t max_samples) {
    ATELIER_TRY_BIND(eval, evaluate_graph(key, project, inspected_node, record_timings));
    ATELIER_TRY_BIND(topology, graph_topology(key));
    GraphInspectView view = **topology;
    view.dirty = graph_dirty(key);
    view.display_error = eval->display_error;
    view.inspected_error = eval->inspected_error;
    view.display_summary = summarize_geometry(eval->display_output, max_samples);
    if (inspected_node != ObjectId{} &&
        std::ranges::find(view.nodes, inspected_node, &GraphNodeView::id) != view.nodes.end()) {
        view.inspected_summary = inspected_node == eval->display_node
                                     ? view.display_summary
                                     : summarize_geometry(eval->inspected_output, max_samples);
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
        if (auto now = runtime_.change_cursor(); now && *now == generator_cache_->idle_runtime_cursor)
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

    std::unordered_set<ObjectId> live_components(generators->begin(), generators->end());
    std::erase_if(cache.entries, [&](const auto& item) { return !live_components.contains(item.first); });

    const auto contain_error = [&](const Error& err, ObjectId component, ObjectId owner,
                                   std::vector<std::string> field = {"graph"},
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

    auto program_res = ensure_procedural_program();
    assets::Reader builtin_reader;
    assets::Reader& active_reader =
        project ? project->assets() : (cache.fallback_reader ? *cache.fallback_reader : builtin_reader);
    std::unordered_set<std::string> referenced_graphs;

    for (const ObjectId component : *generators) {
        ATELIER_TRY_BIND(comp_state, world::inspect_component(runtime_, component));
        if (!comp_state->valid || comp_state->owner == ObjectId{} || !world_.contains(comp_state->owner)) {
            cache.entries.erase(component);
            continue;
        }
        ATELIER_TRY_BIND(proc_source, presentation::read_procedural_source(runtime_, component));
        if (proc_source->source.empty()) {
            cache.entries.erase(component);
            if (proc_source->output)
                ATELIER_TRY(presentation::set_procedural_binding(runtime_, component, {{}, nullptr, {}}));
            continue;
        }

        auto& entry = cache.entries[component];
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

        referenced_graphs.insert(proc_source->source);
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
                component, comp_state->owner, {"chunk_size"}));
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

        auto& slot = cache.graph_slots[proc_source->source];
        ObjectRoot target_graph_root;
        if (const auto open_it = graphs_.find(proc_source->source); open_it != graphs_.end() && open_it->second) {
            slot.closed_doc.reset();
            target_graph_root = open_it->second->document.graph;
        } else {
            if (!slot.closed_doc || (project && slot.closed_project_revision != proj_rev)) {
                if (!project) {
                    if (!slot.closed_doc)
                        continue;
                } else {
                    auto loaded_asset = project->assets().procedural_graph(proc_source->source);
                    if (!loaded_asset) {
                        slot.evaluator.reset();
                        slot.closed_doc.reset();
                        const auto& miss = remember_unavailable(project->assets(), assets::AssetKind::procedural_graph,
                                                                proc_source->source, loaded_asset.error());
                        ATELIER_TRY(drop_component_output());
                        ATELIER_TRY(contain_error(miss.error, component, comp_state->owner, {"graph"}, miss.kind));
                        continue;
                    }
                    auto parsed_doc = load_graph(runtime_, loaded_asset->text);
                    if (!parsed_doc) {
                        slot.evaluator.reset();
                        slot.closed_doc.reset();
                        const Error format_err{ErrorCode::invalid_storage, parsed_doc.error().message};
                        const auto& miss = remember_unavailable(project->assets(), assets::AssetKind::procedural_graph,
                                                                proc_source->source, format_err);
                        ATELIER_TRY(drop_component_output());
                        ATELIER_TRY(contain_error(miss.error, component, comp_state->owner, {"graph"}, miss.kind));
                        continue;
                    }
                    unavailable_.erase(std::pair{assets::AssetKind::procedural_graph, proc_source->source});
                    slot.evaluator.reset();
                    slot.closed_doc = std::move(*parsed_doc);
                    slot.closed_project_revision = proj_rev;
                }
            }
            target_graph_root = slot.closed_doc->graph;
        }

        if (!slot.evaluator || slot.bound_graph_id != target_graph_root.id() || slot.bound_program != program ||
            slot.code_version != code_version) {
            slot.evaluator = std::make_unique<procedural::Evaluator>(runtime_, target_graph_root, cache.resources,
                                                                     program, std::size_t{64} << 20);
            slot.bound_graph_id = target_graph_root.id();
            slot.bound_program = program;
            slot.code_version = code_version;
        }

        const std::optional<std::uint32_t> seed_override =
            override_seed ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(seed)) : std::nullopt;
        if (auto evaluated = slot.evaluator->evaluate(false, seed_override); !evaluated) {
            if (!slot.evaluator->has_display_node() || !entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(evaluated.error(), component, comp_state->owner));
            continue;
        }

        if (auto node_err = slot.evaluator->display_error()) {
            if (!slot.evaluator->has_display_node() || !entry.output)
                ATELIER_TRY(drop_component_output());
            ATELIER_TRY(contain_error(*node_err, component, comp_state->owner));
            continue;
        }

        const auto retained_geom = slot.evaluator->retain_output();
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

        std::vector<presentation::ProceduralMeshDependency> mesh_dependencies;
        for (const auto& dep : slot.evaluator->display_dependencies()) {
            const std::string dep_key(dep.key.view().std_view());
            if (dep_key.empty())
                continue;
            if (auto mesh_asset = cache.resources.resolve_mesh_asset(dep_key); mesh_asset && mesh_asset->mesh) {
                std::vector<std::pair<std::string, std::string>> slots;
                slots.reserve(mesh_asset->materials.size());
                for (const auto& sm : mesh_asset->materials)
                    slots.emplace_back(sm.slot, sm.key);
                mesh_dependencies.push_back({dep_key, mesh_asset->mesh, std::move(slots)});
            } else if (dep.mesh) {
                mesh_dependencies.push_back({dep_key, dep.mesh, {}});
            }
        }

        std::vector<presentation::ProceduralInstanceAsset> resolved_assets;
        std::set<std::string> seen_keys;
        for (const auto& col : published->collections) {
            const std::string key(col.reference.key.view().std_view());
            if (key.empty() || !seen_keys.insert(key).second)
                continue;
            auto mesh_asset = cache.resources.resolve_mesh_asset(key);
            if (!mesh_asset || !mesh_asset->mesh) {
                if (col.reference.mesh)
                    resolved_assets.push_back({key, col.reference.mesh, {}});
                continue;
            }
            std::vector<MissingResource> ignored_missing;
            auto defaults = mesh_defaults(active_reader, *mesh_asset, &ignored_missing);
            resolved_assets.push_back({key, mesh_asset->mesh,
                                       defaults ? std::move(*defaults) : std::vector<presentation::MaterialBinding>{}});
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

    std::erase_if(cache.graph_slots, [&](const auto& item) { return !referenced_graphs.contains(item.first); });

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
    const auto it = generator_cache_->entries.find(component);
    if (it == generator_cache_->entries.end())
        return nullptr;
    const auto& cook = it->second.latest_cook;
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
    const auto cached = generator_cache_->entries.find(component);
    if (cached == generator_cache_->entries.end() || !cached->second.latest_cook ||
        cached->second.output != binding->output)
        return graph_error("Procedural Bake requires a successful current cook.", ErrorCode::invalid_definition);

    struct ResolvedAssetInfo {
        std::string key;

        geometry::MeshRef mesh;

        std::vector<presentation::MaterialBinding> defaults;

        std::wstring stem;
    };

    std::unordered_map<std::string, ResolvedAssetInfo> asset_table;
    assets::Reader builtin_reader;
    assets::Reader& active_reader = project ? project->assets() : builtin_reader;
    const auto resolve_asset_info = [&](const std::string& key) -> Result<const ResolvedAssetInfo*> {
        if (key.empty())
            return static_cast<const ResolvedAssetInfo*>(nullptr);
        if (const auto it = asset_table.find(key); it != asset_table.end())
            return it->second.mesh ? &it->second : nullptr;
        ATELIER_TRY_BIND(live_mesh, active_reader.mesh(key));
        if (!live_mesh->mesh)
            return static_cast<const ResolvedAssetInfo*>(nullptr);
        ResolvedAssetInfo info{
            key, live_mesh->mesh, {}, live_mesh->name.empty() ? std::wstring{L"Instance"} : live_mesh->name};
        bool found_defaults = false;
        for (const auto& asset : binding->resolved_assets) {
            if (asset.key == key) {
                info.defaults = asset.defaults;
                found_defaults = true;
                break;
            }
        }
        if (!found_defaults) {
            std::vector<MissingResource> ignored_missing;
            if (auto defaults = mesh_defaults(active_reader, *live_mesh, &ignored_missing))
                info.defaults = std::move(*defaults);
        }
        if (project) {
            if (const auto* entry = project->assets().entry(key)) {
                if (const auto s = entry->path.stem().wstring(); !s.empty())
                    info.stem = s;
            }
        }
        auto [inserted, _] = asset_table.emplace(key, std::move(info));
        return inserted->second.mesh ? &inserted->second : nullptr;
    };

    struct PendingInstance {
        const ResolvedAssetInfo* asset{};

        world::Trs trs{};
    };
    std::vector<PendingInstance> pending;
    for (const auto& col : binding->output->publication.collections) {
        if (!col.transforms || col.transforms->empty())
            continue;
        const std::string key(col.reference.key.view().std_view());
        ATELIER_TRY_BIND(asset_info_opt, resolve_asset_info(key));
        const auto* asset_info = *asset_info_opt;
        if (!asset_info)
            continue;
        for (const auto& affine : *col.transforms) {
            ATELIER_TRY_BIND(trs, decompose_procedural_instance(affine));
            pending.push_back({asset_info, *trs});
        }
    }

    if (pending.empty()) {
        return graph_error(
            "Procedural Bake converts CopyToPoints mesh instances into StaticMeshEntity objects; this generator currently publishes no mesh instances.",
            ErrorCode::invalid_definition);
    }

    ATELIER_TRY_BIND(members, world_.members());
    if (members->size() + 1 + pending.size() > maximum_entities) {
        return graph_error("Procedural Bake exceeds the Editor Entity limit (" + std::to_string(maximum_entities) +
                               ").",
                           ErrorCode::invalid_definition);
    }

    ATELIER_TRY_BIND(owner_parent, world::parent_entity(runtime_, owner));
    ATELIER_TRY_BIND(owner_has_space, world::has_transform(runtime_, owner));
    world::Trs owner_trs{};
    std::optional<ObjectId> owner_space_ref;
    if (*owner_has_space) {
        ATELIER_TRY_BIND(t, world::read_transform(runtime_, owner));
        owner_trs = *t;
        ATELIER_TRY_BIND(sr, world::space_reference(runtime_, owner));
        owner_space_ref = *sr;
    }

    struct DetachedCleanup {
        Runtime& runtime;

        std::vector<ObjectRoot> entities;

        bool published{};

        ~DetachedCleanup() {
            if (published)
                return;
            for (auto it = entities.rbegin(); it != entities.rend(); ++it) {
                if (it->id() != ObjectId{} && runtime.type_of(it->id()))
                    (void)world::destroy_entity(runtime, it->id());
            }
        }
    } cleanup{runtime_, {}, false};

    ATELIER_TRY_BIND(group_root, world::create_entity(runtime_));
    const ObjectId group_id = group_root->id();
    cleanup.entities.push_back(std::move(*group_root));
    if (*owner_has_space)
        ATELIER_TRY(world_.set_transform(group_id, owner_trs));

    std::vector<std::pair<ObjectId, std::wstring>> child_labels;
    child_labels.reserve(pending.size());
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto& inst = pending[i];
        ATELIER_TRY_BIND(child, presentation::create_static_mesh_entity(
                                    world_, {inst.asset->mesh, inst.trs, inst.asset->key, inst.asset->defaults}));
        const ObjectId child_id = child->id();
        cleanup.entities.push_back(std::move(*child));
        ATELIER_TRY(world_.reparent(child_id, group_id, world::ReparentRule::keep_local));

        const std::wstring suffix = L" " + std::to_wstring(i + 1);
        std::wstring child_label = truncate_utf16(inst.asset->stem, 256 - suffix.size()) + suffix;
        ATELIER_TRY(validate_name(child_label));
        child_labels.emplace_back(child_id, std::move(child_label));
    }

    std::wstring base_name(name(owner));
    if (base_name.empty())
        base_name = L"Procedural Generator";
    base_name += L" Baked";

    std::wstring group_label = truncate_utf16(base_name, 256);
    for (std::size_t suffix = 2; std::ranges::any_of(names_, [&](const auto& kv) { return kv.second == group_label; });
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
        change.removals.push_back(std::move(removal));
    }

    std::vector<world::SpaceReference> spaces;
    if (*owner_has_space && owner_space_ref.has_value())
        spaces.push_back({group_id, owner_space_ref});

    const std::array attachments{world::EntityAttachment{group_id, *owner_parent, world::append}};
    ATELIER_TRY_BIND(attached, world_.attach_batch(attachments, spaces));
    cleanup.published = true;

    names_.emplace(group_id, std::move(group_label));
    for (auto& [cid, clabel] : child_labels)
        names_.emplace(cid, std::move(clabel));

    if (attached->changed) {
        set_selection({group_id, group_id});
        remember(std::move(change));
    }
    return std::move(*attached);
}

} // namespace atelier::editor
