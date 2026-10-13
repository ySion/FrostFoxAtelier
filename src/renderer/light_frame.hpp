#pragma once

#include "material_frame.hpp"
#include <limits>
#include <unordered_map>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t no_light_source=std::numeric_limits<std::uint32_t>::max();

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
static_assert(sizeof(GpuLightSource)==128 && sizeof(GpuLightProposal)==16);

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
    std::uint32_t local_count{},directional_count{},emitter_count{};
    // Includes advancing previous_sources/maps on the first stationary frame
    // after a change. Subsequent unchanged frames publish nothing new.
    bool changed{};

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
    std::uint64_t scene_identity_{},frame_id_{};
    bool valid_{},advance_previous_{};

    void update_impl(const Frame&,const MaterialFrame&,std::uint64_t sequence);
    void rebuild_object(CachedObject&,const MeshInstance&,const MaterialFrame&,std::size_t);
    void build_proposals();
    void map_previous();
};

} // namespace atelier::renderer::detail
