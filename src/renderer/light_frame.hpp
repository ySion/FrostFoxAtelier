#pragma once

#include "renderer/renderer.hpp"
#include <limits>
#include <unordered_map>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t no_light_source = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::array<std::uint32_t, 3> light_grid_counts{16, 8, 16};
inline constexpr std::uint32_t light_grid_cell_capacity = 32;
inline constexpr std::uint32_t light_grid_cell_count =
    light_grid_counts[0] * light_grid_counts[1] * light_grid_counts[2];
inline constexpr float light_grid_spacing = 8.f;
// GPU source-slot decoding uses these fixed bit fields; they are not author settings.
static_assert(light_grid_counts == std::array<std::uint32_t, 3>{16, 8, 16} && light_grid_cell_capacity == 32);

struct GpuLightSource {
    std::array<float, 4> origin_range{}, direction_radius{}, radiance{}, edge1_area{}, edge2{};
    // kind: point=0, rectangle=1, directional=2. flags: two-sided=1, casts shadows=2; reserved.
    std::array<std::uint32_t, 4> meta{};
    bool operator==(const GpuLightSource&) const = default;
};
struct GpuLightProposal {
    float probability{}, accept{};
    std::uint32_t alias{}, reserved{};
    bool operator==(const GpuLightProposal&) const = default;
};
struct GpuLightCell {
    std::uint32_t offset{}, count{};
    float mass{};
    std::uint32_t reserved{};
};
struct GpuSpatialLightProposal {
    std::uint32_t source{};
    float accept{};
    std::uint32_t alias{}; // Relative slot within this cell.
    float probability{};
};
static_assert(sizeof(GpuLightSource) == 96 && sizeof(GpuLightProposal) == 16);
static_assert(sizeof(GpuLightCell) == 16 && sizeof(GpuSpatialLightProposal) == 16);

// CPU snapshot owner of the analytic lights in the renderer's validated Frame.
// It creates no GPU or World resources.
class LightFrame {
public:
    // Scene Lit's render sequence, not the potentially reused UI frame id.
    void update(const Frame&, std::uint64_t sequence);
    void clear();

    // Local analytic lights, then explicit directionals.
    std::vector<GpuLightSource> sources;
    std::vector<GpuLightProposal> proposals; // Local pool only; actual uploaded PMF.
    // Index of each source in the previously published layout, or no_light_source.
    // Becomes the identity on the first stationary frame after a change.
    std::vector<std::uint32_t> current_to_previous;
    // A bounded near-source proposal, never a light influence/culling list.
    // Empty cells disable local sampling; the global proposal always survives.
    std::array<float, 4> spatial_origin{}; // Minimum XYZ, cell spacing.
    std::vector<GpuLightCell> spatial_cells;
    std::vector<GpuSpatialLightProposal> spatial_proposals;
    std::vector<std::uint32_t> spatial_source_slots; // (cell << 5) | slot, or no_light_source.
    bool spatial_changed{};
    std::uint32_t spatial_nonempty_cells{};
    std::uint32_t local_count{}, directional_count{};
    bool changed{}; // Sources and proposals were rebuilt for this snapshot.
    // Current source content changed, independently of reordering. Stable
    // identities distinguish edits from reordering; unknown correspondence is
    // conservative. Repeated requests for the same snapshot preserve this flag.
    bool radiance_changed{};
    // Zero stable keys give a source no cross-frame identity.
    bool keyed(std::size_t source) const { return keys_[source].owner != 0; }

private:
    struct SourceKey {
        std::uint64_t owner{};
        std::uint32_t kind{};
        bool operator==(const SourceKey&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const SourceKey&) const noexcept;
    };
    std::vector<GpuLightSource> analytic_, next_analytic_, previous_sources_;
    std::vector<SourceKey> analytic_keys_, next_analytic_keys_, keys_, previous_keys_;
    std::unordered_map<SourceKey, std::uint32_t, KeyHash> previous_lookup_;
    std::vector<double> powers_, scaled_, actual_;
    std::vector<std::uint32_t> below_, above_;
    std::vector<std::array<std::uint32_t, light_grid_cell_capacity>> spatial_bins_;
    std::uint64_t scene_identity_{}, frame_id_{};
    std::uint64_t source_revision_{}, spatial_source_revision_{}, spatial_frame_id_{};
    bool valid_{}, advance_previous_{};
    bool spatial_initialized_{};

    void update_impl(const Frame&, std::uint64_t sequence);
    void build_proposals();
    void update_spatial(const std::array<float, 3>& focus, std::uint64_t sequence);
    void map_previous();
};

} // namespace atelier::renderer::detail
