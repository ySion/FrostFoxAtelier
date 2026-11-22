#pragma once

#include "editor/graph_format.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/geometry.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atelier::editor {

struct GraphPortInfo {
    std::vector<std::string> inputs;

    std::vector<bool> multiple;

    std::vector<std::string> outputs;

    bool operator==(const GraphPortInfo&) const = default;
};

struct GraphNodeCatalogEntry {
    TypeId type{};

    std::string name;

    std::string description;

    bool native{};

    std::string asset_key{};

    std::string display_label{};

    GraphPortInfo ports;

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

    std::optional<std::uint32_t> color{};

    std::string comment{};

    GraphPortInfo ports;

    std::optional<procedural::NodeStatus> status;
};

struct GraphAttributeStats {
    std::string name;

    std::string type;

    std::uint32_t components{};

    std::size_t count{};

    std::size_t non_finite_count{};

    std::vector<double> minimum;

    std::vector<double> maximum;

    std::vector<double> mean;

    std::vector<std::string> sample_values;
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
    std::optional<GraphTopologyHealth> topology;

    std::optional<GraphSpacingStats> spacing;

    std::optional<GraphInstanceOverlapStats> instance_overlaps;

    std::optional<GraphSurfaceClearanceStats> surface_clearance;
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

    std::array<std::vector<GraphAttributeStats>, 5> attributes{};

    std::vector<std::pair<std::string, std::size_t>> instance_references;

    std::optional<GraphSpatialDiagnostics> spatial;
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

    std::optional<Error> display_error;

    std::optional<Error> inspected_error;
};

struct GraphInspectView {
    std::string key;

    ObjectId graph{};

    std::uint32_t seed{1};

    std::string display;

    std::string template_node;

    bool dirty{};

    bool can_undo{};

    bool can_redo{};

    GraphPortInfo external_ports;

    std::vector<GraphPromotedParameter> parameters;

    std::vector<GraphNodeView> nodes; // Document name order.

    const GraphNodeView* find_node(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(nodes, name, {}, &GraphNodeView::name);
        return it != nodes.end() && it->name == name ? &*it : nullptr;
    }

    std::vector<GraphStickyNote> notes;

    const GraphStickyNote* find_note(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(notes, name, {}, &GraphStickyNote::name);
        return it != notes.end() && it->name == name ? &*it : nullptr;
    }

    std::vector<GraphNetworkBox> boxes;

    const GraphNetworkBox* find_box(std::string_view name) const noexcept {
        const auto it = std::ranges::lower_bound(boxes, name, {}, &GraphNetworkBox::name);
        return it != boxes.end() && it->name == name ? &*it : nullptr;
    }

    std::vector<GraphConnectionView> connections;

    std::vector<world::Diagnostic> issues;

    std::optional<Error> display_error;

    std::optional<Error> inspected_error;

    GraphGeometrySummary display_summary;

    std::optional<GraphGeometrySummary> inspected_summary;
};

[[nodiscard]] GraphGeometrySummary summarize_geometry(const procedural::Geometry* geometry, std::size_t max_samples = 4,
                                                      bool include_spatial = false,
                                                      const procedural::Geometry* reference_surface = nullptr,
                                                      std::string_view reference_name = {});

} // namespace atelier::editor
