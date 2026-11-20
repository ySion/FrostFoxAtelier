#include "editor/session.hpp"
#include "editor/fields.hpp"
#include "editor/graph_format.hpp"
#include "editor/project.hpp"
#include "editor/project_code.hpp"
#include "editor/properties.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/graph.hpp"
#include "source/program.hpp"
#include "source/snapshot.hpp"
#include <algorithm>
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
            ++epoch_;
        }
    }

    [[nodiscard]] Result<geometry::MeshRef> mesh(StringView key) const noexcept override {
        try {
            auto resolved = project_ ? project_->assets().mesh(key.std_view()) : builtin_.mesh(key.std_view());
            if (!resolved)
                return std::unexpected(resolved.error());
            return std::move(resolved->mesh);
        } catch (const std::exception& e) {
            return std::unexpected(Error{ErrorCode::invalid_storage, e.what()});
        }
    }

    [[nodiscard]] std::uint64_t revision() const noexcept override {
        return (epoch_ << 32) ^ (project_ ? project_->revision() : 0ULL);
    }

private:
    Project* project_{};

    mutable assets::Reader builtin_{};

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
};

struct GraphNodesChange {
    struct SavedInputs {
        ObjectId to{};

        std::vector<procedural::Input> inputs;

        std::vector<procedural::Input> detached_inputs{};
    };

    bool added{};

    std::vector<GraphNode> entries;

    std::vector<SavedInputs> saved_inputs;

    ObjectId before_display{};

    ObjectId after_display{};
};

using GraphChange = std::variant<GraphScalarChange, GraphPropertyChange, GraphDisplayChange, GraphRenameChange,
                                 GraphMoveChange, GraphConnectionChange, GraphNodesChange>;

void sort_graph_nodes(std::vector<GraphNode>& nodes) {
    std::ranges::sort(nodes, {}, &GraphNode::name);
}

GraphNode* find_node(GraphDocument& doc, std::string_view name) noexcept {
    const auto at = std::ranges::lower_bound(doc.nodes, name, {}, &GraphNode::name);
    return at != doc.nodes.end() && at->name == name ? &*at : nullptr;
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
    auto open = std::make_shared<OpenGraph>();
    open->key = asset->key;
    open->document = std::move(*loaded);
    open->saved_text = std::move(*canonical);
    for (const auto& issue : open->document.issues)
        world_.report(issue);
    const auto id = open->document.graph.id();
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
    auto open = std::make_shared<OpenGraph>();
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
    ++revision_;
    return {};
}

Result<void> Session::graph_undo(std::string_view key, bool redo) {
    if (world_.running() || world_.closed())
        return graph_error("Graph undo/redo requires an open, stopped Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(cancel_edit());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open.", ErrorCode::invalid_definition);
    auto& open = *at->second;
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
                return {};
            } else if constexpr (std::same_as<T, GraphConnectionChange>) {
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
                return procedural::set_display(runtime_, graph,
                                               change.added ? change.before_display : change.after_display);
            }
        },
        entry));
    if (redo)
        ++open.cursor;
    else
        --open.cursor;
    open.dirty_revision = 0;
    ++revision_;
    return {};
}

Result<std::string> Session::graph_add_node(std::string_view key, std::string_view type_name,
                                            std::string_view desired_name, std::array<float, 2> position) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
        return graph_error("Node position must be finite.");
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
    std::string resolved_type_name(type_name);
    auto type = runtime_.find_type(resolved_type_name);
    if (!type && resolved_type_name.find('.') == std::string::npos) {
        resolved_type_name = "procedural." + resolved_type_name;
        type = runtime_.find_type(resolved_type_name);
    }
    if (!type)
        return graph_error("Unknown procedural node type: " + std::string(type_name));
    ATELIER_TRY(procedural::ports(runtime_, *type));

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
    const ObjectId after_display = make_initial_display ? created->id() : before_contents->display();
    if (make_initial_display)
        ATELIER_TRY(procedural::set_display(runtime_, graph, after_display));

    GraphNode entry{name, std::move(*created), position, {}};
    open.document.nodes.push_back(entry);
    sort_graph_nodes(open.document.nodes);
    open.push_change(GraphNodesChange{true, {std::move(entry)}, {}, before_contents->display(), after_display});
    ++revision_;
    return name;
}

Result<std::string> Session::graph_place_node(std::string_view key, std::string_view type_name,
                                              std::array<float, 2> position, std::string_view from_node,
                                              std::string_view from_port, std::string_view to_node,
                                              std::string_view to_port, bool set_display) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (!std::isfinite(position[0]) || !std::isfinite(position[1]))
        return graph_error("Node position must be finite.");
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
    std::string resolved_type_name(type_name);
    auto type = runtime_.find_type(resolved_type_name);
    if (!type && resolved_type_name.find('.') == std::string::npos) {
        resolved_type_name = "procedural." + resolved_type_name;
        type = runtime_.find_type(resolved_type_name);
    }
    if (!type)
        return graph_error("Unknown procedural node type: " + std::string(type_name));
    ATELIER_TRY_BIND(new_ports, procedural::ports(runtime_, *type));

    std::string name;
    const auto stem = default_node_stem(resolved_type_name);
    for (std::uint32_t index = 1;; ++index) {
        name = stem + std::to_string(index);
        if (!find_node(open.document, name))
            break;
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
                    saved_inputs.push_back({dst->node.id(), std::move(*after_dst), std::move(*before_dst)});
                }
            }
        }
    }

    const ObjectId after_display = (set_display || make_initial_display) ? created_id : before_contents->display();
    if (after_display != before_contents->display())
        ATELIER_TRY(procedural::set_display(runtime_, graph, after_display));

    GraphNode entry{name, std::move(*created), position, {}};
    open.document.nodes.push_back(entry);
    sort_graph_nodes(open.document.nodes);
    open.push_change(
        GraphNodesChange{true, {std::move(entry)}, std::move(saved_inputs), before_contents->display(), after_display});
    ++revision_;
    return name;
}

Result<void> Session::graph_remove_nodes(std::string_view key, std::span<const std::string> names) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    if (names.empty())
        return {};
    auto& open = *at->second;
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
    std::erase_if(open.document.nodes, [&](const GraphNode& item) {
        return std::ranges::find(unique_names, item.name) != unique_names.end();
    });
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_rename_node(std::string_view key, std::string_view old_name, std::string_view new_name) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    if (!valid_node_identifier(new_name))
        return graph_error("Invalid node name '" + std::string(new_name) + "': expected [A-Za-z_][A-Za-z0-9_]*.");
    auto& open = *at->second;
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
    if (open.document.kept.root().string({"display"}).value_or("") == old_name) {
        json::Builder next(open.document.kept);
        next.set_validated(next.root(), "display", new_name);
        change.references.push_back({{}, open.document.kept, next.finish()});
        open.document.kept = change.references.back().after;
    }
    item->name = std::string(new_name);
    sort_graph_nodes(open.document.nodes);
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_move_nodes(std::string_view key,
                                       std::span<const std::pair<std::string, std::array<float, 2>>> moves) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
    GraphMoveChange change;
    for (const auto& [name, next_pos] : moves) {
        if (!std::isfinite(next_pos[0]) || !std::isfinite(next_pos[1]))
            return graph_error("Node position must be finite.");
        const auto* item = find_node(open.document, name);
        if (!item)
            return graph_error("No node named '" + name + "' in this graph.");
        if (item->position != next_pos)
            change.moves.push_back({name, item->position, next_pos});
    }
    if (change.moves.empty())
        return {};
    ATELIER_TRY(cancel_edit());
    for (const auto& move : change.moves)
        find_node(open.document, move.name)->position = move.after;
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_auto_layout(std::string_view key) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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

    const auto before_kept = dst->kept;
    json::Builder next_kept(dst->kept);
    if (const auto kept_inputs = *next_kept.root().find("inputs");
        kept_inputs && kept_inputs->kind() == json::Kind::object) {
        next_kept.erase(*kept_inputs, dst_ports->inputs[in_idx]);
        if (!kept_inputs->size())
            next_kept.erase(next_kept.root(), "inputs");
    }
    dst->kept = next_kept.finish();

    open.push_change(GraphConnectionChange{
        {{dst->node, std::move(*before_inputs), std::move(*after_inputs), before_kept, dst->kept}}});
    ++revision_;
    return {};
}

Result<void> Session::graph_disconnect(std::string_view key, std::string_view to_node, std::string_view to_port,
                                       std::string_view from_node, std::string_view from_port) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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

    std::optional<ObjectId> filter_from;
    std::optional<std::uint32_t> filter_out;
    if (!from_node.empty()) {
        const auto* src = find_node(open.document, from_node);
        if (!src || !src->node.registered())
            return graph_error("Upstream node '" + std::string(from_node) + "' is missing or unresolved.");
        filter_from = src->node.id();
        if (!from_port.empty()) {
            ATELIER_TRY_BIND(src_type, runtime_.type_of(src->node.id()));
            ATELIER_TRY_BIND(src_ports, procedural::ports(runtime_, *src_type));
            const auto found = std::ranges::find(src_ports->outputs, from_port);
            if (found == src_ports->outputs.end())
                return graph_error("Node '" + src->name + "' has no output port '" + std::string(from_port) + "'.");
            filter_out = static_cast<std::uint32_t>(found - src_ports->outputs.begin());
        }
    }

    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));
    ATELIER_TRY_BIND(before_inputs, procedural::inputs(runtime_, *listed, dst->node.id()));

    std::vector<procedural::Input> remaining;
    for (const auto& item : *before_inputs) {
        const bool match_in = !in_idx || item.input == *in_idx;
        const bool match_from = !filter_from || item.from == *filter_from;
        const bool match_out = !filter_out || item.output == *filter_out;
        if (!(match_in && match_from && match_out))
            remaining.push_back(item);
    }
    if (remaining.size() == before_inputs->size())
        return {};

    ATELIER_TRY(replace_node_inputs(runtime_, graph, dst->node.id(), remaining));
    open.push_change(
        GraphConnectionChange{{{dst->node, std::move(*before_inputs), std::move(remaining), dst->kept, dst->kept}}});
    ++revision_;
    return {};
}

Result<void> Session::graph_disconnect_edges(std::string_view key, std::span<const GraphConnectionView> edges) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (edges.empty())
        return {};
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
    ATELIER_TRY(cancel_edit());
    const auto graph = open.document.graph.id();
    ATELIER_TRY_BIND(listed, procedural::contents(runtime_, graph));

    struct ResolvedCut {
        ObjectId from{};

        std::uint32_t out_idx{};

        ObjectId to{};

        std::uint32_t in_idx{};
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
                        static_cast<std::uint32_t>(in_it - dst_ports->inputs.begin())});
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
        if (remaining.size() == before_inputs->size())
            continue;
        ATELIER_TRY(replace_node_inputs(runtime_, graph, to_id, remaining));
        change.targets.push_back({dst.node, std::move(*before_inputs), std::move(remaining), dst.kept, dst.kept});
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
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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

    const auto new_before_kept = new_dst->kept;
    json::Builder next_kept(new_dst->kept);
    if (const auto kept_inputs = *next_kept.root().find("inputs");
        kept_inputs && kept_inputs->kind() == json::Kind::object) {
        next_kept.erase(*kept_inputs, new_ports->inputs[new_in_idx]);
        if (!kept_inputs->size())
            next_kept.erase(next_kept.root(), "inputs");
    }
    new_dst->kept = next_kept.finish();

    GraphConnectionChange change;
    ATELIER_TRY_BIND(old_after, procedural::inputs(runtime_, *listed, old_dst->node.id()));
    if (old_dst == new_dst) {
        change.targets.push_back(
            {old_dst->node, std::move(*old_before), std::move(*old_after), new_before_kept, new_dst->kept});
    } else {
        ATELIER_TRY_BIND(new_after, procedural::inputs(runtime_, *listed, new_dst->node.id()));
        change.targets.push_back(
            {old_dst->node, std::move(*old_before), std::move(*old_after), old_dst->kept, old_dst->kept});
        change.targets.push_back(
            {new_dst->node, std::move(new_before_vec), std::move(*new_after), new_before_kept, new_dst->kept});
    }
    open.push_change(std::move(change));
    ++revision_;
    return {};
}

Result<void> Session::graph_set_display(std::string_view key, std::string_view node_name) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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

Result<void> Session::graph_set_properties(std::string_view key, std::string_view node_name, json::Value properties) {
    if (world_.running() || world_.closed())
        return graph_error("Graph editing requires an open, stopped Session.", ErrorCode::invalid_definition);
    if (properties.kind() != json::Kind::object)
        return graph_error("Properties patch must be a JSON object.");
    ATELIER_TRY(ensure_procedural_program());
    const auto at = graphs_.find(std::string(key));
    if (at == graphs_.end() || !at->second)
        return graph_error("Procedural graph is not open: " + std::string(key), ErrorCode::invalid_definition);
    auto& open = *at->second;
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

    if (inspected_node != ObjectId{} && listed->contains(inspected_node)) {
        if (inspected_node != result.display_node)
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

    if (listed->display() != ObjectId{}) {
        if (const auto* disp_node = find_node_by_id(open.document, listed->display()))
            view.display = disp_node->name;
    }

    for (const auto& item : open.document.nodes) {
        GraphNodeView node_view;
        node_view.name = item.name;
        node_view.position = item.position;
        if (!item.node.registered()) {
            node_view.placeholder = true;
            if (const auto saved_type = item.kept.root().string({"type"}))
                node_view.type = std::string(*saved_type);
            view.nodes.push_back(std::move(node_view));
            continue;
        }
        node_view.id = item.node.id();
        node_view.is_display = item.node.id() == listed->display();
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

} // namespace atelier::editor
