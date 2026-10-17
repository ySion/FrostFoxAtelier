#pragma once

#include "renderer.hpp"
#include "light_frame.hpp"

#include <memory>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t shadow_atlas_extent=4096;
inline constexpr std::uint32_t shadow_tile_border=2;
// Bound as the same shader uniform by SceneLit. PCSS direction perturbations
// must not exceed this CPU residency support.
inline constexpr float shadow_filter_tangent_limit=0.25f;
inline constexpr float shadow_near_ratio=1e-6f;
inline constexpr float shadow_maximum_near=1e-5f;

struct ShadowBounds {
    std::array<float,3> minimum{},maximum{};
};
// Derived from the existing SceneLit object/geometry cache. All current scene
// casters are supplied, including objects outside the main camera. A dynamic
// object stays dynamic for this scene after its first shadow-affecting edit.
struct ShadowCaster {
    std::uint32_t instance{};
    ShadowBounds bounds;
    bool dynamic{};
};
// Emit before overwriting/removing the owning Object's previous bounds. A
// static-to-dynamic migration dirties static at old_bounds and dynamic at both
// bounds. Color/roughness edits do not produce a change.
struct ShadowChange {
    ShadowBounds old_bounds,new_bounds;
    bool old_present{},new_present{};
    bool static_changed{},dynamic_changed{};
};

// HLSL StructuredBuffer layout, row vectors, D3D reversed-Z. Point face order:
// +X,-X,+Y,-Y,+Z,-Z. Point lights reserve six descriptor entries even if some
// directions have no receiver demand; directional lights reserve four.
struct GpuShadowFace {
    std::array<float,16> view_projection{};
    // Interior UV scale XY and bias ZW in the atlas. Transform NDC to [0,1]
    // first, flipping Y. Guard texels are rasterized by an expanded projection.
    std::array<float,4> atlas{};
    // Near, far (light view Z), inverse interior resolution, directional
    // receiver camera-view near boundary (point: zero).
    std::array<float,4> depth{};
    // Valid=bit0, dynamic layer nonempty=bit1, near-plane caster=bit2;
    // generation, source, face ordinal. Bit2 marks the center-map limitation.
    std::array<std::uint32_t,4> meta{};
};
struct GpuShadowLight {
    // Kind: 0=not cached (existing visibility), 1=point, 2=directional.
    // First face descriptor, face count, flags (bit0: shade in this batch).
    std::array<std::uint32_t,4> meta{};
    // Directional cascade far boundaries in unjittered camera view Z.
    std::array<float,4> splits{};
};
static_assert(sizeof(GpuShadowFace)==112);
static_assert(sizeof(GpuShadowLight)==32);

enum class ShadowLayer : std::uint32_t { stationary, dynamic };
struct ShadowUpdate {
    std::uint32_t face{}; // Index into this batch's faces.
    ShadowLayer layer{};
    std::array<std::uint32_t,4> rect{}; // x,y,width,height, including guard.
    // Expanded projection used with the entire rect as viewport/scissor.
    std::array<float,16> view_projection{};
    std::vector<std::uint32_t> instances;
};
struct ShadowBatch {
    // Shade only these analytic source indices, once. Rectangle/no-shadow
    // sources are included and retain their existing visibility behavior.
    std::vector<std::uint32_t> sources;
    // Compact analytic prefix + directional tail, excluding emitter triangles.
    // analytic=local_count-emitter_count; index=source<analytic ? source :
    // analytic+(source-local_count). Never index this array with an emitter.
    std::vector<GpuShadowLight> lights;
    std::vector<GpuShadowFace> faces;
    std::vector<ShadowUpdate> updates;
    bool include_emitters{}; // Exactly the first batch, including empty scenes.
    // Rectangle integration (including unshadowed sources), or conservative
    // sphere-interior receivers of shadow-casting points need the endpoint kernel.
    // Emitters are selected independently by include_emitters and their count.
    bool endpoint_visibility{};
};
struct ShadowPlanStats {
    std::uint32_t requested_faces{},reused_faces{},static_updates{},dynamic_updates{};
    std::uint32_t rendered_instances{},resolution_downgrades{},pressure_batches{};
    // Requested point faces intersecting a caster at/before the minimum near
    // plane. A center shadow map cannot represent that part of visibility.
    std::uint32_t near_plane_faces{};
};
struct ShadowFramePlan {
    std::vector<ShadowBatch> batches;
    ShadowPlanStats stats;
};

// CPU-only owner of derived atlas residency. SceneLit owns the two D32_FLOAT
// textures, clears update rects to 0, draws each update with GREATER depth, then
// unbinds both atlases before shading the batch. Later batches may overwrite
// earlier regions: upload/consume each batch's descriptors before the next one.
class ShadowCache {
public:
    ShadowCache();
    ~ShadowCache();
    ShadowCache(const ShadowCache&)=delete;
    ShadowCache& operator=(const ShadowCache&)=delete;
    const ShadowFramePlan& prepare(const Frame&,const LightFrame&,
        std::span<const ShadowCaster>,std::span<const ShadowChange>,std::uint64_t sequence);
    // Call only after every update and corresponding shade has been encoded.
    // No GPU wait is required: these resources stay on the existing queue.
    void commit() noexcept;
    void invalidate() noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
