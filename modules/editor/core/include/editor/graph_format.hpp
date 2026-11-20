#pragma once
#include "assets/json_format.hpp"
#include "runtime/runtime.hpp"
#include "world/diagnostic.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::editor {

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

    std::array<float, 2> position{};

    GraphNodeShape shape{GraphNodeShape::auto_role};

    std::optional<std::uint32_t> color{};

    std::string comment{};

    // Saved JSON written over the node's own on save: its whole entry when `node` is null,
    // otherwise the property leaves that did not decode and the input ports that did not connect.
    json::Document kept{};
};

// A procedural graph file opened into a Runtime. Its roots must not outlive that Runtime.
struct GraphDocument {
    ObjectRoot graph;

    ObjectId template_node{};

    std::vector<GraphNode> nodes;

    std::vector<GraphStickyNote> notes;

    std::vector<GraphNetworkBox> boxes;

    // Saved settings written over the graph's own on save: leaves that did not decode, and a
    // display or template naming no loaded node.
    json::Document kept{};

    std::vector<world::Diagnostic> issues; // From the load.
};

// Opens a version 2 graph file into `runtime`, which has the procedural types. Loading is
// tolerant, as for scenes: a node of an unknown type, a property leaf that does not fit its field,
// and a connection, display or template naming no loaded node or port are kept, reported, and
// written back on save. Structural errors fail: the version, an unknown key, `nodes` that is not a
// record, an invalid identifier, malformed `inputs`, or invalid shape/color/annotation geometry.
[[nodiscard]] Result<GraphDocument> load_graph(Runtime& runtime, std::string_view text);

// The canonical version 2 text: nodes/notes/boxes by name, every parameter, two-space indent and a
// final newline, so saving an unchanged document again gives the same bytes. Every node of the
// graph needs exactly one entry, and every loaded entry must be in the graph.
[[nodiscard]] Result<std::string> save_graph(Runtime& runtime, const GraphDocument& document);

} // namespace atelier::editor
