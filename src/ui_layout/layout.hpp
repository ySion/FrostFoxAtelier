#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atelier::ui_layout {

class LayoutDocument;

// Document-local resource identity, not a business object or serialization key.
class LayoutNodeId {
public:
    LayoutNodeId() = default;
    bool operator==(const LayoutNodeId&) const = default;
    explicit operator bool() const noexcept { return document_ != 0; }

private:
    friend class LayoutDocument;
    friend struct LayoutView;
    LayoutNodeId(std::uint64_t document, std::uint32_t slot, std::uint32_t generation)
        : document_(document), slot_(slot), generation_(generation) {}
    std::uint64_t document_{};
    std::uint32_t slot_{};
    std::uint32_t generation_{};
};

enum class ErrorCode {
    invalid_input,
    invalid_node,
    invalid_structure,
    root_operation,
    capacity_exceeded,
    unsupported_dependency,
    busy,
    measurement_failed,
    invalid_measurement,
    numeric_overflow,
    revision_exhausted
};
enum class InputField {
    none,
    kind,
    width,
    height,
    padding,
    gap,
    alignment,
    anchor,
    content,
    columns,
    table_row,
    wrap,
    flow,
    grid,
    cell
};
struct Error {
    ErrorCode code;
    LayoutNodeId node{};
    std::uint32_t provider_code{};
    InputField field{};
    // set_tree entry responsible for a validation error, when available.
    std::optional<std::size_t> tree_index{};
};
std::string_view describe(ErrorCode) noexcept;
std::string_view describe(InputField) noexcept;
template <class T>
using Result = std::expected<T, Error>;

struct Size {
    float width{};
    float height{};
    bool operator==(const Size&) const = default;
};
struct Insets {
    float left{};
    float top{};
    float right{};
    float bottom{};
    bool operator==(const Insets&) const = default;
};
enum class SizeRule { fixed, content, expand, flex };
struct Basis {
    bool natural{};
    float value{};
    static Basis fixed(float value) { return {false, value}; }
    static Basis content() { return {true, 0}; }
    bool operator==(const Basis&) const = default;
};
struct Axis {
    SizeRule rule{SizeRule::content};
    // Fixed size, or positive Expand weight; zero for Content.
    float value{};
    float minimum{};
    std::optional<float> maximum;
    Basis basis{};
    float grow_weight{};
    float shrink_weight{};
    static Axis fixed(float size) { return {SizeRule::fixed, size, 0, {}}; }
    static Axis content(float minimum = 0, std::optional<float> maximum = {}) {
        return {SizeRule::content, 0, minimum, maximum};
    }
    static Axis expand(float weight = 1, float minimum = 0, std::optional<float> maximum = {}) {
        return {SizeRule::expand, weight, minimum, maximum};
    }
    static Axis flex(Basis basis, float grow_weight = 1, float shrink_weight = 1, float minimum = 0,
                     std::optional<float> maximum = {}) {
        return {SizeRule::flex, 0, minimum, maximum, basis, grow_weight, shrink_weight};
    }
    bool operator==(const Axis&) const = default;
};
struct Alignment {
    bool fill{true};
    float offset{};
    // Default inherits the parent's Flex cross alignment, or stretches otherwise.
    // Use the factories for an explicit override.
    bool first_baseline{};
    bool inherited{true};
    static Alignment stretch(float offset = 0) { return {true, offset, false, false}; }
    static Alignment fit(float offset = 0) { return {false, offset, false, false}; }
    static Alignment baseline() { return {false, 0, true, false}; }
    bool operator==(const Alignment&) const = default;
};
enum class Distribution { start, center, end, space_between, space_around, space_evenly, stretch };
enum class Wrap { no_wrap, wrap, wrap_reverse };
struct FlexSpec {
    Wrap wrap{Wrap::no_wrap};
    Distribution justify{Distribution::start};
    Distribution align_lines{Distribution::start};
    Alignment align_items{Alignment::stretch()};
    bool reverse{};
    bool operator==(const FlexSpec&) const = default;
};
struct Cell {
    std::optional<std::uint32_t> row{};
    std::optional<std::uint32_t> column{};
    std::uint32_t row_span{1};
    std::uint32_t column_span{1};
    bool operator==(const Cell&) const = default;
};
enum class AutoFlow { row, column };
struct GridSpec {
    std::vector<Axis> columns{};
    std::vector<Axis> rows{};
    float column_gap{};
    float row_gap{};
    AutoFlow auto_flow{AutoFlow::row};
    Axis implicit_column{Axis::content()};
    Axis implicit_row{Axis::content()};
    bool operator==(const GridSpec&) const = default;
};
enum class Kind { row, column, overlay, box, measured, table, wrap, grid };
struct NodeInput {
    Kind kind{Kind::box};
    Axis width{Axis::expand()};
    Axis height{Axis::content()};
    Insets padding{};
    float gap{};
    bool out_of_flow{};
    Alignment horizontal_alignment{};
    Alignment vertical_alignment{};
    float anchor_x{};
    float anchor_y{};
    float pivot_x{};
    float pivot_y{};
    float offset_x{};
    float offset_y{};
    std::uint64_t content_key{};
    std::uint64_t content_revision{};
    // Collapsing preserves identity but excludes the entire subtree from layout.
    bool collapsed{};
    // Table: column gap. Wrap/Flex: line gap. Grid uses its own typed spec.
    float cross_gap{};
    // Table tracks share widths across ordinary row children.
    std::vector<Axis> columns;
    // Wrap: zero uses individual natural widths; positive uses repeated slots.
    float item_width{};
    bool stretch_items{};
    float line_align{}; // [0,1], applies independently to every wrap line.
    std::optional<FlexSpec> flow{}; // Only row/column. Legacy wrap remains natural packing.
    std::optional<GridSpec> grid_spec{};
    Cell cell{};
    static NodeInput row(float gap = 0);
    static NodeInput column(float gap = 0);
    static NodeInput overlay();
    static NodeInput box(float width, float height);
    static NodeInput measured(std::uint64_t key, std::uint64_t revision = 1);
    static NodeInput table(std::vector<Axis> columns, float column_gap = 0, float row_gap = 0);
    static NodeInput wrap(float item_gap = 0, float line_gap = 0);
    static NodeInput grid(float minimum_item_width, float item_gap = 0, float line_gap = 0);
    static NodeInput grid(GridSpec);
    bool operator==(const NodeInput&) const = default;
};
// The root's size comes only from compute(viewport), never a second axis rule.
struct RootInput {
    Kind kind{Kind::column};
    Insets padding{};
    float gap{};
    std::optional<FlexSpec> flow{};
    bool operator==(const RootInput&) const = default;
};
struct Limits {
    // Includes the root and permanently retired generation-exhausted slots.
    std::uint32_t max_nodes{};
    std::uint32_t max_depth{}; // Root depth is zero.
    std::uint32_t max_grid_tracks{4096};
    std::size_t max_grid_cells{1048576}; // Auto-placement workspace, not logical items.
};
struct MeasureQuery {
    std::uint64_t content_key{};
    std::uint64_t content_revision{};
    std::uint64_t provider_revision{};
    // Absent: unwrapped intrinsic size. Present: natural size at content width.
    // No implicit viewport-height limit is passed to the provider.
    std::optional<float> width;
    bool first_baseline{};
    bool operator==(const MeasureQuery&) const = default;
};
struct MeasureError {
    std::uint32_t code{};
};
struct MeasuredSize {
    float width{};
    float height{};
    std::optional<float> first_baseline;
    MeasuredSize() = default;
    MeasuredSize(Size size) : width(size.width), height(size.height) {}
    MeasuredSize(float width, float height, std::optional<float> baseline = {})
        : width(width), height(height), first_baseline(baseline) {}
};
using MeasureResult = std::expected<MeasuredSize, MeasureError>;
using Measure = MeasureResult (*)(void*, const MeasureQuery&);

struct LayoutBox {
    LayoutNodeId node{};
    LayoutNodeId parent{};
    float x{};
    float y{};
    float width{};
    float height{}; // Parent-local, unrounded outer rectangle.
    bool operator==(const LayoutBox&) const = default;
};
struct LayoutView {
    std::uint64_t publication{};
    std::uint64_t input_revision{};
    std::uint64_t provider_revision{};
    Size viewport{};
    std::span<const LayoutBox> boxes; // Ordered preorder; root first.
    // O(1). Uses this publication, including nodes removed since it was published.
    const LayoutBox* find(LayoutNodeId) const noexcept;
    LayoutView() = default;

private:
    friend class LayoutDocument;
    friend struct LayoutSnapshot;
    LayoutView(std::uint64_t publication, std::uint64_t input, std::uint64_t provider, Size viewport,
               std::span<const LayoutBox> boxes, std::span<const std::uint32_t> index)
        : publication(publication), input_revision(input), provider_revision(provider), viewport(viewport),
          boxes(boxes), index_(index) {}
    std::span<const std::uint32_t> index_;
};
struct LayoutSnapshot {
    std::uint64_t publication{};
    std::uint64_t input_revision{};
    std::uint64_t provider_revision{};
    Size viewport{};
    std::vector<LayoutBox> boxes; // Geometry only; does not retain text/resources.
    LayoutView view() const noexcept;
    const LayoutBox* find(LayoutNodeId node) const noexcept { return view().find(node); }

private:
    friend class LayoutDocument;
    std::vector<std::uint32_t> index_;
};
struct Stats {
    std::uint64_t computes{};
    std::uint64_t publications{};
    std::uint64_t measure_calls{};
    std::uint64_t intrinsic_queries{};
    std::uint64_t width_queries{};
    std::uint64_t cache_hits{};
    std::uint64_t node_visits{};
    // Internal node/work buffers; excludes public track descriptors, provider,
    // owned snapshots, returned identity arrays and the owner allocation itself.
    std::uint64_t buffer_allocations{};
    std::uint64_t buffer_bytes_allocated{};
    std::uint64_t buffer_bytes{};
    std::uint64_t peak_buffer_bytes{};
    std::uint64_t geometry_reuses{};
    // Changed complete-tree commits and child links written in final sequences.
    std::uint64_t topology_commits{};
    std::uint64_t topology_links_written{};
};
inline constexpr std::size_t append_position = std::numeric_limits<std::size_t>::max();

struct TreeNodeInput {
    LayoutNodeId node{}; // Empty creates; a live identity retains its node.
    std::optional<std::size_t> parent{}; // Earlier entry; absent means implicit root.
    NodeInput input{};
};

// Synchronous single writer. The caller keeps the document, provider and content
// alive and stable throughout compute; destruction from a callback is forbidden.
class LayoutDocument {
public:
    // Invalid capacity throws invalid_argument; allocation failure propagates.
    LayoutDocument(Measure measure, void* context, Limits limits);
    ~LayoutDocument();
    LayoutDocument(const LayoutDocument&) = delete;
    LayoutDocument& operator=(const LayoutDocument&) = delete;
    LayoutDocument(LayoutDocument&&) = delete;
    LayoutDocument& operator=(LayoutDocument&&) = delete;

    LayoutNodeId root() const noexcept;
    std::size_t node_count() const noexcept;
    std::uint64_t input_revision() const noexcept;
    Result<void> set_root(const RootInput&);
    Result<LayoutNodeId> create(LayoutNodeId parent, const NodeInput&, std::size_t position = append_position);
    Result<NodeInput> input(LayoutNodeId) const;
    Result<void> update(LayoutNodeId, const NodeInput&);
    // Position is interpreted in the destination AFTER removing the moved node.
    Result<void> move(LayoutNodeId, LayoutNodeId parent, std::size_t position = append_position);
    Result<void> remove(LayoutNodeId); // Removes the entire subtree, never root.
    // Replace the complete tree below root in one input transaction. Entry order
    // defines sibling order; omitted identities retire, retained identities survive.
    // Validate final relationships and reserve before commit, including replacement
    // at capacity. Returns identities in entry order. The old publication survives.
    Result<std::vector<LayoutNodeId>> set_tree(std::span<const TreeNodeInput>);

    Result<LayoutView> compute(Size viewport, std::uint64_t provider_revision);
    // Edits and failed computes preserve this borrow. A successful publication
    // or document destruction ends it. Publication zero means no result yet.
    LayoutView view() const noexcept;
    LayoutSnapshot snapshot() const;
    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Logical collection geometry. Positional rows do not replace business identities.
// Instantiate the returned range through the existing LayoutDocument Interface.
struct VisibleRange {
    std::size_t begin{};
    std::size_t end{};
    // Extent sums caller-supplied heights, including estimates for unmeasured rows.
    double local_origin{};
    double scroll{};
    double extent{};
    std::uint64_t revision{};
};
class VirtualList {
public:
    explicit VirtualList(std::size_t max_items, double gap = 0);
    Result<void> reset(std::size_t count, double estimated_height);
    Result<void> set_viewport(double scroll, double height, double overscan = 0);
    Result<void> set_height(std::size_t row, double height);
    // Width/font changes invalidate cached heights; preserve the top row anchor.
    Result<void> invalidate_heights(double estimated_height);
    Result<void> insert(std::size_t position, std::size_t count, double estimated_height);
    Result<void> erase(std::size_t position, std::size_t count);
    VisibleRange range() const noexcept;
    Result<double> offset(std::size_t row) const;
    std::size_t size() const noexcept { return heights_.size(); }
    std::size_t storage_bytes() const noexcept { return (heights_.capacity() + sums_.capacity()) * sizeof(double); }

private:
    double prefix(std::size_t count) const noexcept;
    double extent() const noexcept;
    std::size_t locate(double offset) const noexcept;
    void restore_anchor(std::size_t row, double within) noexcept;
    void rebuild(std::vector<double>& sums, const std::vector<double>& heights) const noexcept;
    Result<void> revision_available() const;
    std::size_t max_items_;
    double gap_;
    double scroll_{};
    double viewport_{};
    double overscan_{};
    std::uint64_t revision_{1};
    std::vector<double> heights_;
    std::vector<double> sums_;
};

} // namespace atelier::ui_layout
