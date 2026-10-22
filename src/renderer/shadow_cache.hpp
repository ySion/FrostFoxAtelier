#pragma once

#include "renderer.hpp"
#include "light_frame.hpp"

#include <memory>

namespace atelier::renderer::detail {

inline constexpr std::uint32_t shadow_atlas_extent=4096;
inline constexpr std::uint32_t shadow_tile_border=2;
inline constexpr std::uint32_t shadow_high_tier_lights=16,shadow_face_redraw_budget=24,shadow_fade_frames=8;
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
    // Kind: 0=no direct light this frame (dropped, or a face still undrawn),
    // 1=cube from the light centre, 2=directional cascades. First face
    // descriptor, face count, float bits of the fade-in factor.
    std::array<std::uint32_t,4> meta{};
    // Directional: cascade far boundaries in unjittered camera view Z.
    // Cube: xyz is the origin its faces were drawn from, which lags the
    // light while a redraw waits for budget.
    std::array<float,4> splits{};
};
static_assert(sizeof(GpuShadowFace)==112);
static_assert(sizeof(GpuShadowLight)==32);

enum class ShadowLayer : std::uint32_t { stationary, dynamic };
struct ShadowUpdate {
    std::uint32_t face{}; // Index into the plan's faces.
    ShadowLayer layer{};
    std::array<std::uint32_t,4> rect{}; // x,y,width,height, including guard.
    // Expanded projection used with the entire rect as viewport/scissor.
    std::array<float,16> view_projection{};
    std::vector<std::uint32_t> instances;
};
struct ShadowPlanStats {
    std::uint32_t requested_faces{},reused_faces{},static_updates{},dynamic_updates{};
    std::uint32_t rendered_instances{},resolution_downgrades{};
    // Shadow-casting lights the atlas could not hold even at the minimum
    // resolution. They contribute no direct light this frame.
    std::uint32_t dropped_lights{};
    // Requested point faces intersecting a caster at/before the minimum near
    // plane. A center shadow map cannot represent that part of visibility.
    std::uint32_t near_plane_faces{};
    // Faces the redraw budget left for a later frame (stale faces keep their
    // previous projection and contents), the longest consecutive deferral
    // among them in frames, the 128² cells held by all requested faces, and
    // resident lights withheld from direct light until every face is drawn.
    std::uint32_t deferred_faces{},deferral_frames{},atlas_cells{},pending_lights{};
};
// Every shadow-casting light that is lit this frame is resident at once.
// Reservation runs in importance order, directional lights first. Only the
// most important point lights use 256²/512² faces; the rest use 128². Cascades
// always redraw; at most shadow_face_redraw_budget other faces redraw per
// frame. A cube's faces redraw together, lights with undrawn faces first, then
// stale lights longest-waiting first; a deferred light keeps its previous
// origin, projections and contents, so shadows lag a frame or two. A light adds
// no direct light until all of its faces have contents; one that waited fades
// in over shadow_fade_frames.
struct ShadowFramePlan {
    // Indexed by light source. Non-casting and dropped lights keep kind 0.
    std::vector<GpuShadowLight> lights;
    std::vector<GpuShadowFace> faces;
    std::vector<ShadowUpdate> updates;
    ShadowPlanStats stats;
};

// CPU-only owner of derived atlas residency. SceneLit owns the two D32_FLOAT
// textures, clears update rects to 0, draws each update with GREATER depth, then
// unbinds both atlases before shading.
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
