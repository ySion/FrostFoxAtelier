#pragma once

#include "editor/graph_format.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/geometry.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace atelier::editor {

struct GraphPortInfo {
    Array<std::string> inputs;

    Array<bool> multiple;

    Array<std::string> outputs;

    bool operator==(const GraphPortInfo&) const = default;
};

struct GraphNodeHelp {
    Array<procedural::PortDescription> ports;

    Array<procedural::AttributeRule> attributes;

    std::string text;
};

struct GraphNodeCatalogEntry {
    TypeId type{};

    std::string name;

    std::string description;

    bool native{};

    std::string asset_key{};

    std::string display_label{};

    GraphPortInfo ports;

    std::shared_ptr<const GraphNodeHelp> help{};

    bool operator==(const GraphNodeCatalogEntry&) const = default;
};

struct GraphConnectionView {
    std::string from_node;

    std::string from_port;

    std::uint32_t from_index{};

    std::string to_node;

    std::string to_port;

    std::uint32_t to_index{};
};

struct GraphNodeView {
    std::string name;

    ObjectId id{};

    std::string type;

    std::string asset_key{};

    std::string asset_label{};

    json::Document overrides{};

    bool placeholder{};

    bool is_display{};

    bool is_template{};

    bool is_bypassed{};

    bool is_frozen{};

    std::array<float, 2> position{};

    GraphNodeShape shape{GraphNodeShape::auto_role};

    Optional<std::uint32_t> color{};

    std::string comment{};

    GraphPortInfo ports;

    Optional<procedural::NodeStatus> status;

    std::string root_error_node{};
};

struct GraphAttributeStats {
    Optional<std::size_t> selected_count; // Exact membership for boolean columns.

    std::string name;

    std::string type;

    std::uint32_t components{};

    std::size_t count{};

    std::size_t non_finite_count{};

    Array<double> minimum;

    Array<double> maximum;

    Array<double> mean;

    Array<std::string> sample_values;
};

struct GraphTopologyHealth {
    bool skipped{};

    std::size_t polygon_count{};

    std::size_t polyline_count{};

    std::size_t degenerate_primitives{};

    std::size_t open_boundary_edges{};

    std::size_t non_manifold_edges{};
};

struct GraphSpacingStats {
    std::string domain;

    std::size_t element_count{};

    std::size_t sample_count{};

    std::size_t coincident_count{};

    double minimum{};

    double p5{};

    double p50{};

    double p95{};
};

struct GraphInstanceOverlapStats {
    std::size_t tested_instances{};

    std::size_t overlapping_pairs{};

    bool pairs_capped{};

    std::size_t overlapping_instances{};

    double overlapping_ratio{};

    double max_penetration_depth{};
};

struct GraphSurfaceClearanceStats {
    std::string reference_node;

    std::string domain;

    std::size_t tested_count{};

    std::size_t grounded_count{};

    std::size_t floating_count{};

    std::size_t buried_count{};

    std::size_t off_surface_count{};

    bool has_gaps{};

    double gap_min{};

    double gap_p5{};

    double gap_p50{};

    double gap_p95{};

    double gap_max{};
};

struct GraphSpatialDiagnostics {
    Optional<GraphTopologyHealth> topology;

    Optional<GraphSpacingStats> spacing;

    Optional<GraphInstanceOverlapStats> instance_overlaps;

    Optional<GraphSurfaceClearanceStats> surface_clearance;
};

struct OpenGraphSnapshot {
    std::string key;

    bool scratch{};

    std::string text;

    std::string saved_text;
};

struct GraphGeometrySummary {
    bool valid{};

    std::array<std::size_t, 5> counts{};

    bool has_bounds{};

    math::Vector3 bounds_min{};

    math::Vector3 bounds_max{};

    std::array<Array<GraphAttributeStats>, 5> attributes{};

    Array<std::pair<std::string, std::size_t>> instance_references;

    Optional<GraphSpatialDiagnostics> spatial;
};

struct GraphEvaluation {
    const procedural::Geometry* display_output{};

    std::shared_ptr<const procedural::Geometry> preview_output; // Current display or the retained last valid cook.

    const procedural::Geometry* template_output{};

    const procedural::Geometry* inspected_output{};

    ObjectId display_node{};

    std::string display_name;

    ObjectId template_node{};

    std::string template_name;

    std::uint64_t generation{};

    Optional<Error> display_error;

    std::string display_error_node{};

    Optional<Error> inspected_error;

    std::string inspected_error_node{};
};

struct GraphInspectView {
    std::string key;

    ObjectId graph{};

    std::uint32_t seed{1};

    std::string display;

    std::string template_node;

    bool can_undo{};

    bool can_redo{};

    GraphPortInfo external_ports;

    Array<GraphPromotedParameter> parameters;

    Array<GraphNodeView> nodes; // Document name order.

    const GraphNodeView* find_node(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(nodes, name, {}, &GraphNodeView::name);
        return it != nodes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphStickyNote> notes;

    const GraphStickyNote* find_note(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(notes, name, {}, &GraphStickyNote::name);
        return it != notes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphNetworkBox> boxes;

    const GraphNetworkBox* find_box(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(boxes, name, {}, &GraphNetworkBox::name);
        return it != boxes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphConnectionView> connections;

    Array<world::Diagnostic> issues;

    Optional<Error> display_error;

    std::string display_error_node{};

    Optional<Error> inspected_error;

    std::string inspected_error_node{};

    GraphGeometrySummary display_summary;

    Optional<GraphGeometrySummary> inspected_summary;
};

inline void append_error_coordinates(std::string& out, const Error& error) {
    if (error.element) {
        out += " [";
        out += error.element->layer.empty() ? std::string_view{"element"} : error.element->layer.view();
        out += ' ';
        out += std::to_string(error.element->index);
        out += ']';
    }
    if (error.source && !error.source->path.empty()) {
        std::string_view file = error.source->path;
        if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos)
            file = file.substr(slash + 1);
        out += " (";
        out += file;
        out += ':';
        out += std::to_string(error.source->line);
        out += ':';
        out += std::to_string(error.source->column);
        out += ')';
    }
}

inline void append_error(std::string& out, const Error& error) {
    out += error.message;
    append_error_coordinates(out, error);
}

[[nodiscard]] inline std::string format_error_coordinates(const Error& error) {
    std::string out;
    append_error_coordinates(out, error);
    return out;
}

[[nodiscard]] inline std::string format_error(const Error& error) {
    std::string out;
    out.reserve(error.message.size() + 32);
    append_error(out, error);
    return out;
}

[[nodiscard]] GraphGeometrySummary summarize_geometry(const procedural::Geometry* geometry, std::size_t max_samples = 4,
                                                      bool include_spatial = false,
                                                      const procedural::Geometry* reference_surface = nullptr,
                                                      std::string_view reference_name = {});

} // namespace atelier::editor
