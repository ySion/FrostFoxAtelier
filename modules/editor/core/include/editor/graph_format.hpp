#pragma once
#include "assets/json_format.hpp"
#include "editor/fields.hpp"
#include "procedural/graph.hpp"
#include "runtime/runtime.hpp"
#include "world/diagnostic.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::editor {

std::optional<std::pair<std::string_view, std::string_view>> graph_endpoint(json::Value value);

enum class GraphNodeShape : std::uint8_t {
    auto_role = 0,
    pill,
    rect,
    domed,
    trapezoid,
    hexagon,
    ribbon,
    diamond,
    circle,
};

[[nodiscard]] std::string_view graph_node_shape_name(GraphNodeShape shape) noexcept;

[[nodiscard]] std::optional<GraphNodeShape> parse_graph_node_shape(std::string_view text) noexcept;

[[nodiscard]] std::string format_graph_hex_color(std::uint32_t rgb);

[[nodiscard]] std::optional<std::uint32_t> parse_graph_hex_color(std::string_view text) noexcept;

[[nodiscard]] bool valid_graph_identifier(std::string_view text) noexcept;

[[nodiscard]] std::string make_unique_graph_identifier(std::string_view preferred, std::string_view fallback,
                                                       const std::function<bool(std::string_view)>& is_taken);

[[nodiscard]] bool valid_graph_target_path(std::string_view text) noexcept;

[[nodiscard]] bool graph_targets_overlap(std::string_view a, std::string_view b) noexcept;

// True when `promoted_target` equals `candidate` or is a strict dot-ancestor of `candidate`
// (for example "box1.size" controls "box1.size.x", whereas "box1.size.x" does not control "box1.size").
[[nodiscard]] bool graph_target_controls_path(std::string_view promoted_target, std::string_view candidate) noexcept;

struct GraphPromotedParameter {
    std::string name; // [A-Za-z_][A-Za-z0-9_]*, unique in `GraphDocument::parameters`.

    std::vector<std::string> targets; // Non-empty "node.field..." paths sharing one leaf/vector TypeId.

    std::string label;

    std::string category;

    std::string description;

    bool operator==(const GraphPromotedParameter&) const = default;
};

struct ResolvedGraphTarget {
    std::string node_name;

    std::string field_path;

    ObjectId node{};

    TypeId value_type{};

    bool is_vector{};

    std::string label;

    std::string category;

    std::string description;

    std::vector<detail::ScalarField> leaves;
};

struct GraphStickyNote {
    std::string name; // [A-Za-z_][A-Za-z0-9_]*, unique among notes.

    std::array<float, 2> position{};

    std::array<float, 2> size{210.f, 116.f};

    std::string title;

    std::string text;

    std::uint32_t color{0xd4a72c};

    bool operator==(const GraphStickyNote&) const = default;
};

struct GraphNetworkBox {
    std::string name; // [A-Za-z_][A-Za-z0-9_]*, unique among boxes.

    std::array<float, 2> position{};

    std::array<float, 2> size{320.f, 220.f};

    std::string title;

    std::uint32_t color{0x3b5b78};

    bool operator==(const GraphNetworkBox&) const = default;
};

// A node of a procedural graph document. Its name, canvas position, and visual annotations are
// document data, not node fields, so moving or styling a node on the canvas does not stamp it.
struct GraphNode {
    std::string name; // [A-Za-z_][A-Za-z0-9_]*, unique in its document.

    ObjectRoot node; // Null when the Runtime has no node type of the saved name.

    std::string asset_key{}; // On procedural.AssetNode: referenced .procgraph asset key ("project/...").

    json::Document overrides{}; // On procedural.AssetNode: sparse parameter overrides keyed by promoted name.

    std::array<float, 2> position{};

    GraphNodeShape shape{GraphNodeShape::auto_role};

    std::optional<std::uint32_t> color{};

    std::string comment{};

    // Saved JSON written over the node's own on save: its whole entry when `node` is null,
    // otherwise the property/override leaves that did not decode and the input ports that did not connect.
    json::Document kept{};
};

// A procedural graph file opened into a Runtime. Its roots must not outlive that Runtime.
struct GraphDocument {
    ObjectRoot graph;

    ObjectId template_node{};

    std::vector<GraphPromotedParameter> parameters;

    std::vector<GraphNode> nodes;

    std::vector<GraphStickyNote> notes;

    std::vector<GraphNetworkBox> boxes;

    // Saved settings written over the graph's own on save: leaves that did not decode, and a
    // display or template naming no loaded node.
    json::Document kept{};

    std::vector<world::Diagnostic> issues; // From the load.
};

struct ResolvedPromotedSchema {
    ResolvedGraphTarget primary;

    std::vector<ResolvedGraphTarget> all_targets;

    std::vector<FieldRange> ranges;
};

[[nodiscard]] Result<ResolvedGraphTarget>
resolve_graph_parameter_target(Runtime& runtime, const GraphDocument& document, std::string_view target_spec);

[[nodiscard]] Result<ResolvedPromotedSchema>
resolve_promoted_parameter_schema(Runtime& runtime, const GraphDocument& document, const GraphPromotedParameter& param);

[[nodiscard]] Result<FieldRange> intersect_field_ranges(const FieldRange& a, const FieldRange& b);

[[nodiscard]] Result<procedural::Ports> extract_graph_ports(Runtime& runtime, const GraphDocument& document,
                                                            std::vector<ObjectId>* input_nodes = nullptr,
                                                            std::vector<ObjectId>* output_nodes = nullptr);

// Clones an already-loaded GraphDocument in Runtime memory without JSON roundtrips, preserving
// session-level node bypass and frozen geometry snapshots.
[[nodiscard]] Result<GraphDocument> clone_graph_document(Runtime& runtime, const GraphDocument& source);

// Deterministically destroys all Runtime node and graph objects owned by a private cloned GraphDocument.
void destroy_private_graph_document(Runtime& runtime, GraphDocument& document) noexcept;

using SavedGraphPortMap = std::map<std::string, std::pair<bool, std::vector<std::string>>>;

[[nodiscard]] Result<void> apply_saved_graph_node_inputs(Runtime& runtime, GraphDocument& document, GraphNode& target,
                                                         const SavedGraphPortMap& saved,
                                                         std::vector<world::Diagnostic>* issues = nullptr);

using AssetPortResolver = std::function<Result<procedural::Ports>(std::string_view asset_key)>;

[[nodiscard]] procedural::AssetSubgraph resolve_asset_binding(const AssetPortResolver& resolver,
                                                              std::string_view asset_key);

// Opens a version 4 graph file into `runtime`, which has the procedural types. Loading is
// tolerant, as for scenes: a node of an unknown type, a property leaf that does not fit its field,
// and a connection, display or template naming no loaded node or port are kept, reported, and
// written back on save. Structural errors fail: the version, an unknown key, `nodes` that is not a
// record, an invalid identifier, malformed `inputs` or `parameters`, or invalid shape/color/annotation geometry.
[[nodiscard]] Result<GraphDocument> load_graph(Runtime& runtime, std::string_view text,
                                               const AssetPortResolver& asset_ports = {});

// The canonical version 4 text: nodes/notes/boxes by name, ordered promoted parameters, every node
// parameter, two-space indent and a final newline, so saving an unchanged document again gives the
// same bytes. Every node of the graph needs exactly one entry, and every loaded entry must be in the graph.
[[nodiscard]] Result<std::string> save_graph(Runtime& runtime, const GraphDocument& document);

} // namespace atelier::editor
