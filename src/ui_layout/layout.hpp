#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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
    LayoutNodeId(std::uint64_t document, std::uint32_t slot, std::uint32_t generation)
        : document_(document), slot_(slot), generation_(generation) {}
    std::uint64_t document_{};
    std::uint32_t slot_{};
    std::uint32_t generation_{};
};

enum class ErrorCode {
    invalid_input, invalid_node, invalid_structure, root_operation,
    capacity_exceeded, unsupported_dependency, busy, measurement_failed,
    invalid_measurement, numeric_overflow, revision_exhausted
};
struct Error {
    ErrorCode code;
    LayoutNodeId node{};
    std::uint32_t provider_code{};
};
template<class T> using Result = std::expected<T, Error>;

struct Size {
    float width{}, height{};
    bool operator==(const Size&) const = default;
};
struct Insets {
    float left{}, top{}, right{}, bottom{};
    bool operator==(const Insets&) const = default;
};
enum class SizeRule { fixed, content, expand };
struct Axis {
    SizeRule rule{SizeRule::content};
    // Fixed size, or positive Expand weight; zero for Content.
    float value{};
    float minimum{};
    std::optional<float> maximum;
    static Axis fixed(float size) { return {SizeRule::fixed, size, 0, {}}; }
    static Axis content(float minimum = 0, std::optional<float> maximum = {}) {
        return {SizeRule::content, 0, minimum, maximum};
    }
    static Axis expand(float weight = 1, float minimum = 0,
                       std::optional<float> maximum = {}) {
        return {SizeRule::expand, weight, minimum, maximum};
    }
    bool operator==(const Axis&) const = default;
};
enum class Kind { row, column, overlay, box, measured };
struct NodeInput {
    Kind kind{Kind::box};
    Axis width{Axis::expand()}, height{Axis::content()};
    Insets padding{};
    float gap{};
    bool out_of_flow{}, fill_width{true}, fill_height{true};
    float align_x{}, align_y{};
    float anchor_x{}, anchor_y{}, pivot_x{}, pivot_y{}, offset_x{}, offset_y{};
    std::uint64_t content_key{}, content_revision{};
    bool operator==(const NodeInput&) const = default;
};
// The root's size comes only from compute(viewport), never a second axis rule.
struct RootInput {
    Kind kind{Kind::column};
    Insets padding{};
    float gap{};
    bool operator==(const RootInput&) const = default;
};
struct Limits {
    // Includes the root and permanently retired generation-exhausted slots.
    std::uint32_t max_nodes{};
    std::uint32_t max_depth{}; // Root depth is zero.
};
struct MeasureQuery {
    std::uint64_t content_key{}, content_revision{}, provider_revision{};
    // Absent: unwrapped intrinsic size. Present: natural size at content width.
    // No implicit viewport-height limit is passed to the provider.
    std::optional<float> width;
    bool operator==(const MeasureQuery&) const = default;
};
struct MeasureError { std::uint32_t code{}; };
using MeasureResult = std::expected<Size, MeasureError>;
using Measure = MeasureResult (*)(void*, const MeasureQuery&);

struct LayoutBox {
    LayoutNodeId node{}, parent{};
    float x{}, y{}, width{}, height{}; // Parent-local, unrounded outer rectangle.
    bool operator==(const LayoutBox&) const = default;
};
struct LayoutView {
    std::uint64_t publication{}, input_revision{}, provider_revision{};
    Size viewport{};
    std::span<const LayoutBox> boxes; // Ordered preorder; root first.
};
struct LayoutSnapshot {
    std::uint64_t publication{}, input_revision{}, provider_revision{};
    Size viewport{};
    std::vector<LayoutBox> boxes; // Geometry only; does not retain text/resources.
};
struct Stats {
    std::uint64_t computes{}, publications{}, measure_calls{}, intrinsic_queries{},
        width_queries{}, cache_hits{}, node_visits{};
};
inline constexpr std::size_t append_position = std::numeric_limits<std::size_t>::max();

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
    Result<LayoutNodeId> create(LayoutNodeId parent, const NodeInput&,
                                std::size_t position = append_position);
    Result<NodeInput> input(LayoutNodeId) const;
    Result<void> update(LayoutNodeId, const NodeInput&);
    // Position is interpreted in the destination AFTER removing the moved node.
    Result<void> move(LayoutNodeId, LayoutNodeId parent,
                      std::size_t position = append_position);
    Result<void> remove(LayoutNodeId); // Removes the entire subtree, never root.

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

} // namespace atelier::ui_layout
