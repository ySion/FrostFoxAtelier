#pragma once
#include "assets/json_format.hpp"
#include "editor/fields.hpp"
#include "foundation/array.hpp"
#include "foundation/function_ref.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "procedural/graph.hpp"
#include "runtime/runtime.hpp"
#include "world/diagnostic.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>

namespace atelier::editor {

Optional<std::pair<StringView, StringView>> graph_endpoint(json::Value value);

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

[[nodiscard]] StringView graph_node_shape_name(GraphNodeShape shape) noexcept;

[[nodiscard]] Optional<GraphNodeShape> parse_graph_node_shape(StringView text) noexcept;

[[nodiscard]] String format_graph_hex_color(std::uint32_t rgb);

[[nodiscard]] Optional<std::uint32_t> parse_graph_hex_color(StringView text) noexcept;

[[nodiscard]] bool valid_graph_identifier(StringView text) noexcept;

[[nodiscard]] String make_unique_graph_identifier(StringView preferred, StringView fallback,
                                                  FunctionRef<bool(StringView)> is_taken);

[[nodiscard]] bool valid_graph_target_path(StringView text) noexcept;

[[nodiscard]] bool graph_targets_overlap(StringView a, StringView b) noexcept;

// True when `promoted_target` equals `candidate` or is a strict dot-ancestor of `candidate`
// (for example "box1.size" controls "box1.size.x", whereas "box1.size.x" does not control "box1.size").
[[nodiscard]] bool graph_target_controls_path(StringView promoted_target, StringView candidate) noexcept;

struct GraphPromotedParameter {
    String name; // [A-Za-z_][A-Za-z0-9_]*, unique in `GraphDocument::parameters`.

    Array<String> targets; // Non-empty "node.field..." paths sharing one leaf/vector TypeId.

    String label;

    String category;

    String description;

    bool operator==(const GraphPromotedParameter&) const = default;
};

struct ResolvedGraphTarget {
    String node_name;

    String field_path;

    ObjectId node{};

    TypeId value_type{};

    bool is_vector{};

    String label;

    String category;

    String description;

    Array<detail::ScalarField> leaves;
};

struct GraphStickyNote {
    String name; // [A-Za-z_][A-Za-z0-9_]*, unique among notes.

    std::array<float, 2> position{};

    std::array<float, 2> size{210.f, 116.f};

    String title;

    String text;

    std::uint32_t color{0xd4a72c};

    bool operator==(const GraphStickyNote&) const = default;
};

struct GraphNetworkBox {
    String name; // [A-Za-z_][A-Za-z0-9_]*, unique among boxes.

    std::array<float, 2> position{};

    std::array<float, 2> size{320.f, 220.f};

    String title;

    std::uint32_t color{0x3b5b78};

    bool operator==(const GraphNetworkBox&) const = default;
};

// A node of a procedural graph document. Its name, canvas position, and visual annotations are
// document data, not node fields, so moving or styling a node on the canvas does not stamp it.
struct GraphNode {
    String name; // [A-Za-z_][A-Za-z0-9_]*, unique in its document.

    ObjectRoot node; // Null when the Runtime has no node type of the saved name.

    String asset_key{}; // On procedural.AssetNode: referenced .procgraph asset key ("project/...").

    json::Document overrides{}; // On procedural.AssetNode: sparse parameter overrides keyed by promoted name.

    std::array<float, 2> position{};

    GraphNodeShape shape{GraphNodeShape::auto_role};

    Optional<std::uint32_t> color{};

    String comment{};

    // Saved JSON written over the node's own on save: its whole entry when `node` is null,
    // otherwise the property/override leaves that did not decode and the input ports that did not connect.
    json::Document kept{};
};

// A procedural graph file opened into a Runtime. Its roots must not outlive that Runtime.
struct GraphDocument {
    ObjectRoot graph;

    ObjectId template_node{};

    Array<GraphPromotedParameter> parameters;

    Array<GraphNode> nodes;

    Array<GraphStickyNote> notes;

    Array<GraphNetworkBox> boxes;

    // Saved settings written over the graph's own on save: leaves that did not decode, and a
    // display or template naming no loaded node.
    json::Document kept{};

    Array<world::Diagnostic> issues; // From the load.
};

struct ResolvedPromotedSchema {
    ResolvedGraphTarget primary;

    Array<ResolvedGraphTarget> all_targets;

    Array<FieldRange> ranges;
};

[[nodiscard]] Result<ResolvedGraphTarget>
resolve_graph_parameter_target(Runtime& runtime, const GraphDocument& document, StringView target_spec);

[[nodiscard]] Result<ResolvedPromotedSchema>
resolve_promoted_parameter_schema(Runtime& runtime, const GraphDocument& document, const GraphPromotedParameter& param);

[[nodiscard]] Result<FieldRange> intersect_field_ranges(const FieldRange& a, const FieldRange& b);

[[nodiscard]] Result<procedural::Ports> extract_graph_ports(Runtime& runtime, const GraphDocument& document,
                                                            Array<ObjectId>* input_nodes = nullptr,
                                                            Array<ObjectId>* output_nodes = nullptr);

// Clones an already-loaded GraphDocument in Runtime memory without JSON roundtrips, preserving
// session-level node bypass and frozen geometry snapshots.
[[nodiscard]] Result<GraphDocument> clone_graph_document(Runtime& runtime, const GraphDocument& source);

// Deterministically destroys all Runtime node and graph objects owned by a private cloned GraphDocument.
void destroy_private_graph_document(Runtime& runtime, GraphDocument& document) noexcept;

using SavedGraphPortMap = std::map<String, std::pair<bool, Array<String>>, std::less<>>;

[[nodiscard]] Result<void> apply_saved_graph_node_inputs(Runtime& runtime, GraphDocument& document, GraphNode& target,
                                                         const SavedGraphPortMap& saved,
                                                         Array<world::Diagnostic>* issues = nullptr);

using AssetPortResolver = std::function<Result<procedural::Ports>(StringView asset_key)>;

[[nodiscard]] procedural::AssetSubgraph resolve_asset_binding(const AssetPortResolver& resolver,
                                                              StringView asset_key);

// Opens a version 4 graph file into `runtime`, which has the procedural types. Loading is
// tolerant, as for scenes: a node of an unknown type, a property leaf that does not fit its field,
// and a connection, display or template naming no loaded node or port are kept, reported, and
// written back on save. Structural errors fail: the version, an unknown key, `nodes` that is not a
// record, an invalid identifier, malformed `inputs` or `parameters`, or invalid shape/color/annotation geometry.
[[nodiscard]] Result<GraphDocument> load_graph(Runtime& runtime, StringView text,
                                               const AssetPortResolver& asset_ports = {});

// The canonical version 4 text: nodes/notes/boxes by name, ordered promoted parameters, every node
// parameter, two-space indent and a final newline, so saving an unchanged document again gives the
// same bytes. Every node of the graph needs exactly one entry, and every loaded entry must be in the graph.
[[nodiscard]] Result<String> save_graph(Runtime& runtime, const GraphDocument& document);

} // namespace atelier::editor
