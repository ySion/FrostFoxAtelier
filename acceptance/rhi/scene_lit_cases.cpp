#include "rhi/rhi.hpp"
#include "support.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void complete(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
}

void scene_lit_cases() {
    ContextOptions options;
    options.validation = Validation::gpu;
    auto owner = take(Context::open(options));
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc draw_desc;
    draw_desc.kind = PipelineKind::vertex;
    draw_desc.source.vs = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = id == 0 ? float2(-1,-1) : id == 1 ? float2(-1,3) : float2(3,-1);
    return float4(p, 0.25, 1);
})";
    draw_desc.source.ps = R"(
struct Output { float4 color : SV_Target0; float4 normal : SV_Target1; };
Output main() { Output o; o.color=float4(0.5,1,2,4); o.normal=float4(-1,0.25,0,1); return o; }
)";
    for (std::uint32_t i = 0; i < 2; ++i)
        check(gfxDrawStateSetColorTarget(draw_desc.draw_state, i, DXGI_FORMAT_R16G16B16A16_FLOAT));
    check(gfxDrawStateSetCullMode(draw_desc.draw_state, D3D12_CULL_MODE_NONE));
    check(gfxDrawStateSetDepthStencilTarget(draw_desc.draw_state, DXGI_FORMAT_D32_FLOAT));
    check(gfxDrawStateSetDepthFunction(draw_desc.draw_state, D3D12_COMPARISON_FUNC_LESS));
    check(gfxDrawStateSetDepthWriteMask(draw_desc.draw_state, D3D12_DEPTH_WRITE_MASK_ALL));
    auto draw = take(owner.prepare(draw_desc));
    PipelineDesc sample_desc;
    sample_desc.source.cs = R"(
Texture2D<float4> color_image; Texture2D<float4> normal_image; Texture2D<float> depth_image;
RWStructuredBuffer<uint> samples;
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint i=(id.y*64+id.x)*9;
    float4 c=color_image.Load(int3(id.xy,0)), n=normal_image.Load(int3(id.xy,0));
    samples[i]=asuint(c.x); samples[i+1]=asuint(c.y); samples[i+2]=asuint(c.z); samples[i+3]=asuint(c.w);
    samples[i+4]=asuint(n.x); samples[i+5]=asuint(n.y); samples[i+6]=asuint(n.z); samples[i+7]=asuint(n.w);
    samples[i+8]=asuint(depth_image.Load(int3(id.xy,0)));
})";
    auto sample = take(owner.prepare(sample_desc));
    PipelineDesc mip_write_desc;
    mip_write_desc.source.cs = R"(
RWTexture2D<uint> mip_image; uint base_value;
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { mip_image[id.xy]=base_value+id.y*16+id.x; }
)";
    auto mip_write = take(owner.prepare(mip_write_desc));
    PipelineDesc mip_read_desc;
    mip_read_desc.source.cs = R"(
Texture2D<uint> mip_image; RWStructuredBuffer<uint> mip_values;
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { mip_values[id.y*16+id.x]=mip_image.Load(int3(id.xy,0)); }
)";
    auto mip_read = take(owner.prepare(mip_read_desc));
    PipelineDesc args_desc;
    args_desc.source.cs = R"(
RWStructuredBuffer<uint> dispatch_args;
[numthreads(1,1,1)] void main() { dispatch_args[0]=8; dispatch_args[1]=1; dispatch_args[2]=1; }
)";
    auto generate_args = take(owner.prepare(args_desc));
    PipelineDesc indirect_desc;
    indirect_desc.source.cs = R"(
RWStructuredBuffer<uint> indirect_values; uint frame_tag;
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) { indirect_values[id.x]=id.x*7+19+frame_tag; }
)";
    auto indirect = take(owner.prepare(indirect_desc));
    auto color = gfxCreateTexture2D(gfx,64,32,DXGI_FORMAT_R16G16B16A16_FLOAT,1,nullptr,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto normal = gfxCreateTexture2D(gfx,64,32,DXGI_FORMAT_R16G16B16A16_FLOAT,1,nullptr,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const std::array<float,4> depth_clear = {1,0,0,0};
    auto depth = gfxCreateTexture2D(gfx,64,32,DXGI_FORMAT_D32_FLOAT,1,depth_clear.data(),D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto mip_texture = gfxCreateTexture2D(gfx,64,32,DXGI_FORMAT_R32_UINT,3,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto samples = gfxCreateBuffer<std::uint32_t>(gfx,64*32*9);
    auto sample_readback = gfxCreateBuffer<std::uint32_t>(gfx,64*32*9,nullptr,kGfxCpuAccess_Read);
    auto color_readback = gfxCreateBuffer<std::uint16_t>(gfx,64*32*4,nullptr,kGfxCpuAccess_Read);
    auto normal_readback = gfxCreateBuffer<std::uint16_t>(gfx,64*32*4,nullptr,kGfxCpuAccess_Read);
    auto mip_values = gfxCreateBuffer<std::uint32_t>(gfx,16*8);
    auto mip_readback = gfxCreateBuffer<std::uint32_t>(gfx,16*8,nullptr,kGfxCpuAccess_Read);
    auto args = gfxCreateBuffer<std::uint32_t>(gfx,3);
    auto indirect_values = gfxCreateBuffer<std::uint32_t>(gfx,64);
    auto indirect_readback = gfxCreateBuffer<std::uint32_t>(gfx,64,nullptr,kGfxCpuAccess_Read);
    auto timestamp = gfxCreateTimestampQuery(gfx);
    require(color && normal && depth && mip_texture && samples && sample_readback && color_readback && normal_readback &&
            mip_values && mip_readback && args && indirect_values && indirect_readback && timestamp,"Scene Lit resources unavailable");
    check(gfxCommandBeginTimestampQuery(gfx,timestamp));
    check(gfxCommandClearTexture(gfx,depth));
    check(gfxCommandBindColorTarget(gfx,0,color));
    check(gfxCommandBindColorTarget(gfx,1,normal));
    check(gfxCommandBindDepthStencilTarget(gfx,depth));
    check(gfxCommandBindKernel(gfx,draw.kernel()));
    check(gfxCommandDraw(gfx,3));
    check(gfxCommandBindColorTarget(gfx,0,{}));
    check(gfxCommandBindColorTarget(gfx,1,{}));
    check(gfxCommandBindDepthStencilTarget(gfx,{}));
    check(gfxProgramSetParameter(gfx,sample.program(),"color_image",color));
    check(gfxProgramSetParameter(gfx,sample.program(),"normal_image",normal));
    check(gfxProgramSetParameter(gfx,sample.program(),"depth_image",depth));
    check(gfxProgramSetParameter(gfx,sample.program(),"samples",samples));
    check(gfxCommandBindKernel(gfx,sample.kernel()));
    check(gfxCommandDispatch(gfx,8,4,1));
    check(gfxCommandCopyBuffer(gfx,sample_readback,samples));
    check(gfxCommandCopyTextureToBuffer(gfx,color_readback,color));
    check(gfxCommandCopyTextureToBuffer(gfx,normal_readback,normal));
    check(gfxProgramSetParameter(gfx,mip_write.program(),"mip_image",mip_texture,2));
    check(gfxProgramSetParameter(gfx,mip_read.program(),"mip_image",mip_texture,2));
    check(gfxProgramSetParameter(gfx,mip_read.program(),"mip_values",mip_values));
    // Whole-resource barriers permit sequential UAV/SRV use of the same texture.
    // Simultaneous SRV/UAV binding of distinct mips requires subresource state tracking.
    check(gfxProgramSetParameter(gfx,mip_write.program(),"base_value",std::uint32_t{113}));
    check(gfxCommandBindKernel(gfx,mip_write.kernel()));
    check(gfxCommandDispatch(gfx,2,1,1));
    check(gfxCommandBindKernel(gfx,mip_read.kernel()));
    check(gfxCommandDispatch(gfx,2,1,1));
    check(gfxCommandCopyBuffer(gfx,mip_readback,mip_values));
    check(gfxProgramSetParameter(gfx,generate_args.program(),"dispatch_args",args));
    check(gfxCommandBindKernel(gfx,generate_args.kernel()));
    check(gfxCommandDispatch(gfx,1,1,1));
    check(gfxProgramSetParameter(gfx,indirect.program(),"indirect_values",indirect_values));
    check(gfxProgramSetParameter(gfx,indirect.program(),"frame_tag",std::uint32_t{0}));
    check(gfxCommandBindKernel(gfx,indirect.kernel()));
    check(gfxCommandDispatchIndirect(gfx,args));
    check(gfxCommandCopyBuffer(gfx,indirect_readback,indirect_values));
    check(gfxCommandEndTimestampQuery(gfx,timestamp));
    for (std::uint32_t i=0;i<owner.info().frames_in_flight;++i) complete(owner.frame());
    complete(owner.finish());
    const auto* sample_data = gfxBufferGetData<std::uint32_t>(gfx,sample_readback);
    const auto* color_data = gfxBufferGetData<std::uint16_t>(gfx,color_readback);
    const auto* normal_data = gfxBufferGetData<std::uint16_t>(gfx,normal_readback);
    const auto* mip_data = gfxBufferGetData<std::uint32_t>(gfx,mip_readback);
    const auto* indirect_data = gfxBufferGetData<std::uint32_t>(gfx,indirect_readback);
    require(sample_data && color_data && normal_data && mip_data && indirect_data,"Scene Lit readback unavailable");
    constexpr std::array<float,9> expected = {0.5f,1,2,4,-1,0.25f,0,1,0.25f};
    constexpr std::array<std::uint16_t,4> color_half = {0x3800,0x3c00,0x4000,0x4400};
    constexpr std::array<std::uint16_t,4> normal_half = {0xbc00,0x3400,0,0x3c00};
    for (std::size_t pixel=0;pixel<64*32;++pixel) {
        for (std::size_t channel=0;channel<9;++channel)
            require(sample_data[pixel*9+channel]==std::bit_cast<std::uint32_t>(expected[channel]),"MRT/depth SRV sample mismatch");
        for (std::size_t channel=0;channel<4;++channel)
            require(color_data[pixel*4+channel]==color_half[channel] && normal_data[pixel*4+channel]==normal_half[channel],"FP16 MRT pixel mismatch");
    }
    for (std::uint32_t i=0;i<16*8;++i) require(mip_data[i]==113+i,"nonzero UAV/SRV mip mismatch");
    for (std::uint32_t i=0;i<64;++i) require(indirect_data[i]==i*7+19,"GPU-generated indirect dispatch mismatch");
    const float milliseconds=gfxTimestampQueryGetDuration(gfx,timestamp);
    require(std::isfinite(milliseconds) && milliseconds>0,"retired GPU timestamp unavailable");
    constexpr std::size_t query_count=8;
    constexpr std::uint32_t measured_frames=6;
    const auto slots=owner.info().frames_in_flight;
    require(slots>0,"timestamp acceptance requires frame slots");
    std::array<GfxTimestampQuery,query_count> queries{};
    std::array<GfxTimestampQuerySample,query_count> cached{};
    std::array<GfxBuffer,measured_frames> frame_readbacks{};
    std::array<std::uint64_t,measured_frames> recording_sequences{};
    for (auto& readback : frame_readbacks) {
        readback=gfxCreateBuffer<std::uint32_t>(gfx,64,nullptr,kGfxCpuAccess_Read);
        require(static_cast<bool>(readback),"timestamp-associated readback creation failed");
    }
    const auto before_finish=gfxGetRecordingFrameSequence(gfx);
    require(before_finish!=0,"owned recording frame has no sequence");
    complete(owner.finish()); // Diagnostic flush must not advance the recording-frame identity.
    require(gfxGetRecordingFrameSequence(gfx)==before_finish,"finish changed recording-frame identity");
    for (auto& query : queries) {
        query=gfxCreateTimestampQuery(gfx);
        require(static_cast<bool>(query),"timestamp batch creation failed");
        require(gfxTimestampQueryGetSample(gfx,query).frame_sequence==0,"new timestamp fabricated a completed sample");
    }
    std::uint64_t previous_sequence=0;
    for (std::uint32_t frame=0;frame<measured_frames+slots-1;++frame) {
        if (frame<measured_frames) {
            recording_sequences[frame]=gfxGetRecordingFrameSequence(gfx);
            require(recording_sequences[frame]!=0,"recording-frame sequence unavailable");
            check(gfxProgramSetParameter(gfx,indirect.program(),"frame_tag",
                                        static_cast<std::uint32_t>(recording_sequences[frame])));
            check(gfxCommandBindKernel(gfx,indirect.kernel()));
            for (auto query : queries) {
                check(gfxCommandBeginTimestampQuery(gfx,query));
                for (std::uint32_t dispatch=0;dispatch<4;++dispatch)
                    check(gfxCommandDispatch(gfx,8,1,1));
                check(gfxCommandEndTimestampQuery(gfx,query));
            }
            check(gfxCommandCopyBuffer(gfx,frame_readbacks[frame],indirect_values));
            require(gfxGetRecordingFrameSequence(gfx)==recording_sequences[frame],
                    "recording changed frame identity before submission");
        }
        for (std::size_t i=0;i<query_count;++i) {
            const auto sample=gfxTimestampQueryGetSample(gfx,queries[i]);
            require(sample.frame_sequence==cached[i].frame_sequence && sample.duration_ms==cached[i].duration_ms,
                    "timestamp getter changed before frame slot retirement");
        }
        complete(owner.frame());
        const bool retired=frame>=slots-1;
        bool positive=false;
        std::uint64_t sequence=0;
        for (std::size_t i=0;i<query_count;++i) {
            const auto sample=gfxTimestampQueryGetSample(gfx,queries[i]);
            const auto repeated=gfxTimestampQueryGetSample(gfx,queries[i]);
            require(sample.frame_sequence==repeated.frame_sequence && sample.duration_ms==repeated.duration_ms,
                    "timestamp getter consumed or changed completed sample");
            if (!retired) {
                require(sample.frame_sequence==0,"timestamp completed before its frame slot retired");
            } else {
                require(sample.frame_sequence!=0 && std::isfinite(sample.duration_ms) && sample.duration_ms>=0,
                        "retired raw timestamp sample invalid");
                if (i==0) sequence=sample.frame_sequence;
                require(sample.frame_sequence==sequence,"queries from one GPU frame have different sequences");
                positive=positive || sample.duration_ms>0;
                if (previous_sequence==0 && sample.duration_ms>0) {
                    const double smoothed=gfxTimestampQueryGetDuration(gfx,queries[i]);
                    const double expected_smoothed=sample.duration_ms*0.05;
                    require(std::abs(smoothed-expected_smoothed)<=1e-6+sample.duration_ms*1e-5,
                            "raw timestamp getter returned legacy smoothed duration or changed smoothing");
                }
            }
            cached[i]=sample;
        }
        if (retired) {
            require(positive,"completed timestamp frame contains no positive GPU duration");
            require(previous_sequence==0 || sequence==previous_sequence+1,"completed timestamp frame sequence did not advance once");
            previous_sequence=sequence;
            const auto completed_frame=frame-(slots-1);
            require(sequence==recording_sequences[completed_frame],
                    "completed timestamp does not identify its recorded readback");
            const auto* completed_data=gfxBufferGetData<std::uint32_t>(gfx,frame_readbacks[completed_frame]);
            require(completed_data!=nullptr,"completed timestamp-associated readback unavailable");
            for (std::uint32_t i=0;i<64;++i)
                require(completed_data[i]==i*7+19+static_cast<std::uint32_t>(sequence),
                        "timestamp/readback recording-frame association mismatch");
        }
    }
    complete(owner.frame());
    for (std::size_t i=0;i<query_count;++i) {
        const auto sample=gfxTimestampQueryGetSample(gfx,queries[i]);
        require(sample.frame_sequence==cached[i].frame_sequence && sample.duration_ms==cached[i].duration_ms,
                "idle GPU frame fabricated a timestamp sample");
        check(gfxDestroyTimestampQuery(gfx,queries[i]));
    }
    for (auto buffer : {samples,sample_readback,color_readback,normal_readback,mip_values,mip_readback,args,indirect_values,indirect_readback})
        check(gfxDestroyBuffer(gfx,buffer));
    for (auto readback : frame_readbacks) check(gfxDestroyBuffer(gfx,readback));
    for (auto texture : {color,normal,depth,mip_texture}) check(gfxDestroyTexture(gfx,texture));
    check(gfxDestroyTimestampQuery(gfx,timestamp));
    draw={}; sample={}; mip_write={}; mip_read={}; generate_args={}; indirect={};
    complete(owner.close());
    check_debug(debug.Get());
    std::printf("scene-lit-resources: FP16-MRT/D32-SRV/mip-2-UAV-SRV/GPU-indirect=PASS timestamp=%.6fms raw-queries=8 frames=6 retirement/cache/sequence/smoothing/recording-readback/finish-stability=PASS\n",milliseconds);
}
} // namespace rhi_acceptance
