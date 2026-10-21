#pragma once
#include "renderer.hpp"
#include "material_frame.hpp"

namespace atelier::renderer::detail {
struct SceneTriangle {
    std::array<geometry::Vertex, 3> vertices;
    std::uint32_t edge_mask;
    std::array<float, 3> face_normal;
    std::uint32_t material_slot{};
    std::array<std::uint32_t,3> reserved{};
};
// One draw per Renderer geometry group, in group order.
struct SceneLitDraw {
    GfxBuffer triangles;
    std::uint32_t vertex_count{};
    std::uint32_t first{}, count{}; // Visible instances in SceneLitDraws::draw_instances.
};
struct SceneLitDraws {
    std::span<const SceneLitDraw> draws;
    std::span<const std::uint32_t> instance_draws; // Every snapshot instance, culled shadow casters included.
    GfxBuffer draw_instances;                      // Original snapshot indices.
};
// Private pass owner; Renderer remains the sole frame/present/capture entry.
class SceneLit {
public:
    explicit SceneLit(rhi::Context& context);
    ~SceneLit();
    SceneLit(const SceneLit&)=delete;
    SceneLit& operator=(const SceneLit&)=delete;
    void validate(const Frame& frame) const;
    // See Renderer::preparation; only Lit views reach here.
    Result<Preparation> preparation(const ViewSettings& view);
    void draw(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,
              const SceneLitDraws& scene_draws,const MaterialFrame& materials,FrameStats& stats);
    // Called only after CPU preparation succeeds, inside the encoding phase.
    // Post-AA diagnostics use the public camera and preserve scene depth.
    void draw_debug(const Frame& frame,ui::PixelExtent extent,GfxTexture depth,FrameStats& stats);
    GfxTexture motion() const; // This frame's GBuffer surface motion, RGBA16F.
    void invalidate(bool empty_scene,FrameStats& stats);
    Result<void> close();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
