#pragma once

#include "material_frame.hpp"
#include <limits>
#include <unordered_map>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t no_light_source=std::numeric_limits<std::uint32_t>::max();
inline constexpr std::array<std::uint32_t,3> light_grid_counts{16,8,16};
inline constexpr std::uint32_t light_grid_cell_capacity=32;
inline constexpr std::uint32_t light_grid_cell_count=light_grid_counts[0]*light_grid_counts[1]*light_grid_counts[2];
inline constexpr float light_grid_spacing=8.f;
// GPU source-slot decoding uses these fixed bit fields; they are not author settings.
static_assert(light_grid_counts==std::array<std::uint32_t,3>{16,8,16}&&light_grid_cell_capacity==32);

struct GpuLightSource {
    std::array<float,4> origin_range{},direction_radius{},radiance{},edge1_area{},edge2{},uv01{},uv2{};
    // kind: point=0, rectangle=1, directional=2, triangle=3.
    // flags: emission two-sided=1, casts shadows=2; current material index; reserved.
    std::array<std::uint32_t,4> meta{0,0,no_light_source,0};
    bool operator==(const GpuLightSource&) const = default;
};
struct GpuLightProposal {
    float probability{},accept{};
    std::uint32_t alias{},reserved{};
    bool operator==(const GpuLightProposal&) const = default;
};
struct GpuLightCell {
    std::uint32_t offset{},count{};
    float mass{};
    std::uint32_t reserved{};
};
struct GpuSpatialLightProposal {
    std::uint32_t source{};
    float accept{};
    std::uint32_t alias{}; // Relative slot within this cell.
    float probability{};
};
static_assert(sizeof(GpuLightSource)==128 && sizeof(GpuLightProposal)==16);
static_assert(sizeof(GpuLightCell)==16 && sizeof(GpuSpatialLightProposal)==16);

// CPU snapshot owner. update consumes the renderer's validated Frame and the
// corresponding MaterialFrame; it creates no GPU or World resources.
class LightFrame {
public:
    // Scene Lit's render sequence, not the potentially reused UI frame id.
    void update(const Frame&,const MaterialFrame&,std::uint64_t sequence);
    void clear();

    // Local analytic lights, emissive triangles, then explicit directionals.
    std::vector<GpuLightSource> sources,previous_sources;
    std::vector<GpuLightProposal> proposals; // Local pool only; actual uploaded PMF.
    std::vector<std::uint32_t> current_to_previous,previous_to_current;
    // A bounded near-source proposal, never a light influence/culling list.
    // Empty cells disable local sampling; the global proposal always survives.
    std::array<float,4> spatial_origin{}; // Minimum XYZ, cell spacing.
    std::vector<GpuLightCell> spatial_cells;
    std::vector<GpuSpatialLightProposal> spatial_proposals;
    std::vector<std::uint32_t> spatial_source_slots; // (cell << 5) | slot, or no_light_source.
    bool spatial_changed{};
    std::uint32_t spatial_nonempty_cells{};
    std::uint32_t local_count{},directional_count{},emitter_count{};
    // Includes advancing previous_sources/maps on the first stationary frame
    // after a change. Subsequent unchanged frames publish nothing new.
    bool changed{};
    // Current source content changed, independently of upload/remap publication.
    // Stable identities and effective material descriptions distinguish edits
    // from reordering or material-index renumbering. Unknown correspondence is
    // conservative. Repeated requests for the same snapshot preserve this flag.
    bool radiance_changed{};

private:
    struct ObjectKey {
        std::uint64_t value{};
        bool legacy{};
        bool operator==(const ObjectKey&) const = default;
    };
    struct SourceKey {
        std::uint64_t owner{};
        const geometry::Mesh* geometry{};
        std::uint32_t primitive{},kind{};
        bool operator==(const SourceKey&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const ObjectKey&) const noexcept;
        std::size_t operator()(const SourceKey&) const noexcept;
    };
    struct Identity {
        SourceKey key;
        std::uint32_t material{no_light_source}; // Index in owned descriptions below.
    };
    struct CachedEmitter {
        GpuLightSource source;
        double power{};
        std::uint32_t primitive{},material_slot{};
    };
    struct CachedObject {
        geometry::MeshRef geometry;
        std::array<float,16> world{};
        std::vector<material::Description> descriptions;
        std::vector<std::uint32_t> material_indices;
        std::vector<CachedEmitter> emitters;
        bool used{};
    };
    std::unordered_map<ObjectKey,CachedObject,KeyHash> objects_;
    std::vector<ObjectKey> object_order_,next_object_order_;
    std::vector<GpuLightSource> analytic_,next_analytic_;
    std::vector<SourceKey> analytic_keys_,next_analytic_keys_;
    std::vector<Identity> identities_,previous_identities_;
    std::vector<material::Description> descriptions_,previous_descriptions_;
    // Identity contains raw geometry pointers; these captures prevent ABA.
    std::vector<geometry::MeshRef> geometry_owners_,previous_geometry_owners_;
    std::unordered_map<SourceKey,std::uint32_t,KeyHash> previous_lookup_;
    std::vector<double> powers_,scaled_,actual_;
    std::vector<std::uint32_t> below_,above_;
    std::vector<std::array<std::uint32_t,light_grid_cell_capacity>> spatial_bins_;
    std::uint64_t scene_identity_{},frame_id_{};
    std::uint64_t source_revision_{},spatial_source_revision_{},spatial_frame_id_{};
    bool valid_{},advance_previous_{};
    bool spatial_initialized_{};

    void update_impl(const Frame&,const MaterialFrame&,std::uint64_t sequence);
    void rebuild_object(CachedObject&,const MeshInstance&,const MaterialFrame&,std::size_t);
    void build_proposals();
    void update_spatial(const std::array<float,3>& focus,std::uint64_t sequence);
    void map_previous();
};

} // namespace atelier::renderer::detail
