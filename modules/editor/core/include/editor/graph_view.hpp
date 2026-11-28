#pragma once

#include "editor/graph_format.hpp"
#include "foundation/array.hpp"
#include "foundation/optional.hpp"
#include "foundation/string.hpp"
#include "procedural/evaluator.hpp"
#include "procedural/geometry.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace atelier::editor {

struct GraphPortInfo {
    Array<String> inputs;

    Array<bool> multiple;

    Array<String> outputs;

    bool operator==(const GraphPortInfo&) const = default;
};

struct GraphNodeHelp {
    Array<procedural::PortDescription> ports;

    Array<procedural::AttributeRule> attributes;

    String text;
};

struct GraphNodeCatalogEntry {
    TypeId type{};

    String name;

    String description;

    bool native{};

    String asset_key{};

    String display_label{};

    GraphPortInfo ports;

    std::shared_ptr<const GraphNodeHelp> help{};

    bool operator==(const GraphNodeCatalogEntry&) const = default;
};

struct GraphConnectionView {
    String from_node;

    String from_port;

    std::uint32_t from_index{};

    String to_node;

    String to_port;

    std::uint32_t to_index{};
};

struct GraphNodeView {
    String name;

    ObjectId id{};

    String type;

    String asset_key{};

    String asset_label{};

    json::Document overrides{};

    bool placeholder{};

    bool is_display{};

    bool is_template{};

    bool is_bypassed{};

    bool is_frozen{};

    std::array<float, 2> position{};

    GraphNodeShape shape{GraphNodeShape::auto_role};

    Optional<std::uint32_t> color{};

    String comment{};

    GraphPortInfo ports;

    Optional<procedural::NodeStatus> status;

    String root_error_node{};
};

struct GraphAttributeStats {
    Optional<std::size_t> selected_count; // Exact membership for boolean columns.

    String name;

    String type;

    std::uint32_t components{};

    std::size_t count{};

    std::size_t non_finite_count{};

    Array<double> minimum;

    Array<double> maximum;

    Array<double> mean;

    Array<String> sample_values;
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
    String domain;

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
    String reference_node;

    String domain;

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
    String key;

    bool scratch{};

    String text;

    String saved_text;
};

struct GraphGeometrySummary {
    bool valid{};

    std::array<std::size_t, 5> counts{};

    bool has_bounds{};

    math::Vector3 bounds_min{};

    math::Vector3 bounds_max{};

    std::array<Array<GraphAttributeStats>, 5> attributes{};

    Array<std::pair<String, std::size_t>> instance_references;

    Optional<GraphSpatialDiagnostics> spatial;
};

struct GraphEvaluation {
    const procedural::Geometry* display_output{};

    std::shared_ptr<const procedural::Geometry> preview_output; // Current display or the retained last valid cook.

    const procedural::Geometry* template_output{};

    const procedural::Geometry* inspected_output{};

    ObjectId display_node{};

    String display_name;

    ObjectId template_node{};

    String template_name;

    std::uint64_t generation{};

    Optional<Error> display_error;

    String display_error_node{};

    Optional<Error> inspected_error;

    String inspected_error_node{};
};

struct GraphInspectView {
    String key;

    ObjectId graph{};

    std::uint32_t seed{1};

    String display;

    String template_node;

    bool can_undo{};

    bool can_redo{};

    GraphPortInfo external_ports;

    Array<GraphPromotedParameter> parameters;

    Array<GraphNodeView> nodes; // Document name order.

    const GraphNodeView* find_node(StringView name) const noexcept {
        const auto it = std::ranges::lower_bound(nodes, name, {}, &GraphNodeView::name);
        return it != nodes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphStickyNote> notes;

    const GraphStickyNote* find_note(StringView name) const noexcept {
        const auto it = std::ranges::lower_bound(notes, name, {}, &GraphStickyNote::name);
        return it != notes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphNetworkBox> boxes;

    const GraphNetworkBox* find_box(StringView name) const noexcept {
        const auto it = std::ranges::lower_bound(boxes, name, {}, &GraphNetworkBox::name);
        return it != boxes.end() && it->name == name ? &*it : nullptr;
    }

    Array<GraphConnectionView> connections;

    Array<world::Diagnostic> issues;

    Optional<Error> display_error;

    String display_error_node{};

    Optional<Error> inspected_error;

    String inspected_error_node{};

    GraphGeometrySummary display_summary;

    Optional<GraphGeometrySummary> inspected_summary;
};

inline void append_error_coordinates(String& out, const Error& error) {
    if (error.element) {
        out.append_literal<" [">();
        const auto layer = error.element->layer.empty() ? StringView{"element"} : error.element->layer.view();
        if (!out.append(layer))
            out.append_literal<"(invalid UTF-8)">();
        out.append_ascii(' ');
        out.append_number(error.element->index);
        out.append_ascii(']');
    }
    if (error.source && !error.source->path.empty()) {
        constexpr ByteSearchSet path_separators{"/\\"};

        StringView file = error.source->path;
        if (const auto slash = file.find_last_of(path_separators); slash != StringView::npos)
            file = file.slice(slash + 1);
        out.append_literal<" (">();
        out.append_validated(file);
        out.append_ascii(':');
        out.append_number(error.source->line);
        out.append_ascii(':');
        out.append_number(error.source->column);
        out.append_ascii(')');
    }
}

inline void append_error(String& out, const Error& error) {
    out += error.message;
    append_error_coordinates(out, error);
}

[[nodiscard]] inline String format_error_coordinates(const Error& error) {
    String out;
    append_error_coordinates(out, error);
    return out;
}

[[nodiscard]] inline String format_error(const Error& error) {
    String out;
    out.reserve(error.message.size() + 32);
    append_error(out, error);
    return out;
}

[[nodiscard]] GraphGeometrySummary summarize_geometry(const procedural::Geometry* geometry, std::size_t max_samples = 4,
                                                      bool include_spatial = false,
                                                      const procedural::Geometry* reference_surface = nullptr,
                                                      StringView reference_name = {});

} // namespace atelier::editor
