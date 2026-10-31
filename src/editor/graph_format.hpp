#pragma once
#include "assets/json_format.hpp"
#include "runtime/runtime.hpp"
#include "world/diagnostic.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace atelier::editor {
// A node of a procedural graph document. Its name and canvas position are document data, not
// node fields, so moving a node on the canvas does not stamp it.
struct GraphNode {
    std::string name; // [A-Za-z_][A-Za-z0-9_]*, unique in its document.
    ObjectRoot node; // Null when the Runtime has no node type of the saved name.
    std::array<float, 2> position{};
    // Saved JSON written over the node's own on save: its whole entry when `node` is null,
    // otherwise the property leaves that did not decode and the input ports that did not connect.
    assets::json_format::Json kept = assets::json_format::Json::object();
};
// A procedural graph file opened into a Runtime. Its roots must not outlive that Runtime.
struct GraphDocument {
    ObjectRoot graph;
    std::vector<GraphNode> nodes;
    // Saved settings written over the graph's own on save: leaves that did not decode, and a
    // display naming no loaded node.
    assets::json_format::Json kept = assets::json_format::Json::object();
    std::vector<world::Diagnostic> issues; // From the load.
};

// Opens a version 1 graph file into `runtime`, which has the procedural types. Loading is
// tolerant, as for scenes: a node of an unknown type, a property leaf that does not fit its field,
// and a connection or display naming no loaded node or port are kept, reported, and written back
// on save. Structural errors fail: the version, an unknown key, `nodes` that is not a record, an
// invalid node name and malformed `inputs`.
[[nodiscard]] Result<GraphDocument> load_graph(Runtime& runtime, std::string_view text);
// The canonical version 1 text: nodes by name, every parameter, two-space indent and a final
// newline, so saving an unchanged document again gives the same bytes. Every node of the graph
// needs exactly one entry, and every loaded entry must be in the graph.
[[nodiscard]] Result<std::string> save_graph(Runtime& runtime, const GraphDocument& document);
}
