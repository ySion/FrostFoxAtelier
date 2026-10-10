#pragma once
#include "renderer/renderer.hpp"
#include <compare>

namespace atelier::renderer::detail {
struct GpuTextureInput {
    std::array<float,4> scale_offset{}, rotation_size{};
    std::array<std::uint32_t,4> meta{}; // texture, channel, address, present
    auto operator<=>(const GpuTextureInput&) const = default;
};
struct GpuSurface {
    std::array<float,4> base_roughness{}, emissive_metallic{}, coverage_normal{};
    std::array<std::uint32_t,4> flags{}; // mask, double-sided, two-sided emission, reserved
    std::array<GpuTextureInput,material::input_count> inputs{};
    auto operator<=>(const GpuSurface&) const = default;
};
static_assert(sizeof(GpuSurface)==352 && sizeof(GpuTextureInput)==48);

// Owned by Renderer; all raster/ray passes borrow the same captured content and
// GPU table until encoding ends. Slot indices are scoped to this frame only.
struct MaterialFrame {
    std::vector<material::Description> descriptions;
    std::vector<std::uint32_t> offsets, indices;
    std::vector<GpuSurface> surfaces;
    std::vector<GfxTexture> textures;
    GfxBuffer surface_buffer{}, index_buffer{}, offset_buffer{};
    std::array<GfxSamplerState,3> samplers{};
    const material::Description& surface(std::size_t instance,std::size_t slot) const {
        return descriptions[offsets[instance]+slot];
    }
};
inline void bind_materials(GfxContext gfx,GfxProgram program,const MaterialFrame& frame) {
    const auto check=[](GfxResult value) {
        if(value!=kGfxResult_NoError)
            throw Error{{rhi::ErrorCode::execution_failed,value,"Bind material frame"},true};
    };
    check(gfxProgramSetParameter(gfx,program,"material_surfaces",frame.surface_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_indices",frame.index_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_offsets",frame.offset_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_textures",frame.textures.data(),static_cast<std::uint32_t>(frame.textures.size())));
    constexpr const char* names[]{"material_repeat_sampler","material_clamp_sampler","material_mirror_sampler"};
    for(std::size_t i=0;i<frame.samplers.size();++i)check(gfxProgramSetParameter(gfx,program,names[i],frame.samplers[i]));
}
inline material::Description resolved_surface(const MeshInstance& mesh,std::size_t slot) {
    if(mesh.surfaces) return (*mesh.surfaces)[slot]->description();
    auto result=mesh.geometry->materials()[slot].surface->description();
    auto& p=result.parameters;
    p.base_color[0]*=mesh.color.r;p.base_color[1]*=mesh.color.g;p.base_color[2]*=mesh.color.b;p.base_color[3]*=mesh.color.a;
    p.roughness=mesh.material.roughness;p.metallic=mesh.material.metallic;p.emissive=mesh.material.emissive;
    return result;
}
}
