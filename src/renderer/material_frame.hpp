#pragma once
#include "renderer/renderer.hpp"
#include "material_programs.hpp"

namespace atelier::renderer::detail {
// Owned by Renderer; all raster/ray passes borrow the same captured content and
// GPU table until encoding ends. Slot indices are scoped to this frame only.
struct MaterialFrame {
    std::array<GfxSamplerState,3> samplers{};
    // Batch is borrowed only while encoding. Owned captures remain comparable
    // after the caller retires it; there is no live Runtime/parent capture here.
    const MaterialBatchState* batch{};
    std::vector<material::ProgramSurfaceRef> captured_programs, program_slots;
    std::vector<std::uint32_t> program_offsets, program_indices;
    GfxBuffer program_index_buffer{}, program_offset_buffer{}; // DeviceTable-owned.
    std::uint64_t revision{}; // Advances whenever captured slot content changes.
    material::Coverage coverage(std::size_t instance,std::size_t slot) const {
        return program_slots[program_offsets[instance]+slot]->policy().coverage;
    }
};
inline void bind_materials(GfxContext gfx,GfxProgram program,const MaterialFrame& frame) {
    const auto check=[](GfxResult value) {
        if(value!=kGfxResult_NoError)
            throw Error{{rhi::ErrorCode::execution_failed,value,"Bind material frame"},true};
    };
    if(!frame.batch)return; // Non-material compute passes may render an empty scene.
    check(gfxProgramSetParameter(gfx,program,"material_surfaces",frame.batch->surface_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_parameters",frame.batch->parameter_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_inputs",frame.batch->input_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_indices",frame.program_index_buffer));
    check(gfxProgramSetParameter(gfx,program,"material_offsets",frame.program_offset_buffer));
    const auto& textures=frame.batch->textures;
    check(gfxProgramSetParameter(gfx,program,"material_textures",textures.data(),static_cast<std::uint32_t>(textures.size())));
    constexpr const char* names[]{"material_repeat_sampler","material_clamp_sampler","material_mirror_sampler"};
    for(std::size_t i=0;i<frame.samplers.size();++i)check(gfxProgramSetParameter(gfx,program,names[i],frame.samplers[i]));
}
}
