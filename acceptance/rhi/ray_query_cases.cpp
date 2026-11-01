#include "rhi/rhi.hpp"
#include "support.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace rhi_acceptance {
namespace {
using namespace atelier::rhi;

template <class T>
T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void complete(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

struct Vertex {
    float x;

    float y;

    float z;
};

struct Hit {
    std::uint32_t hit;

    std::uint32_t instance;

    std::uint32_t primitive;

    std::uint32_t visible;

    float u;

    float v;

    float distance;

    float padding;
};

static_assert(sizeof(Hit) == 32);

constexpr std::uint32_t ray_count = 5;

void verify(const Hit* hits, float first_distance) {
    require(hits != nullptr, "inline RayQuery readback unavailable");
    for (std::uint32_t i = 0; i < ray_count; ++i) {
        const bool expected_hit = i < 2;
        require(hits[i].hit == std::uint32_t(expected_hit) && hits[i].visible == std::uint32_t(expected_hit),
                "inline RayQuery closest-hit/first-hit visibility mismatch");
        if (expected_hit) {
            require(hits[i].instance == (i == 0 ? 17u : 29u) && hits[i].primitive == 1,
                    "inline RayQuery instance/nearest primitive mismatch");
            require(std::abs(hits[i].u - 0.25f) < 0.0001f && std::abs(hits[i].v - 0.25f) < 0.0001f &&
                        std::abs(hits[i].distance - (first_distance + float(i))) < 0.0001f,
                    "inline RayQuery barycentric/closest distance mismatch");
        } else {
            require(hits[i].instance == UINT32_MAX && hits[i].primitive == UINT32_MAX && hits[i].distance == -1.0f,
                    "inline RayQuery miss metadata mismatch");
        }
    }
}
} // namespace

void ray_query_cases() {
    ContextOptions options;
    options.validation = Validation::gpu;
    auto owner = take(Context::open(options));
    const auto gfx = owner.borrow();
    auto debug = debug_queue(gfx);
    PipelineDesc desc;
    desc.kind = PipelineKind::compute;
    desc.shader_model = "6_5";
    desc.name = "inline-ray-query-acceptance";
    desc.source.cs = R"(
RaytracingAccelerationStructure scene;
struct Hit { uint hit, instance, primitive, visible; float u, v, distance, padding; };
RWStructuredBuffer<Hit> output_hits;
float origin_shift;
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    RayDesc ray;
    ray.Origin = float3(origin_shift + (i < 3 ? 8.0 * i : i == 4 ? -4.0 : 0.0), 0, 0);
    ray.Direction = float3(0,0,1);
    ray.TMin = 0.001;
    ray.TMax = 20;
    uint mask = i == 3 ? 2 : 1;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> closest;
    closest.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
    while (closest.Proceed()) {}
    Hit result;
    result.hit = closest.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.instance = 0xffffffff;
    result.primitive = 0xffffffff;
    result.u = result.v = 0;
    result.distance = -1;
    result.padding = 0;
    if (result.hit) {
        result.instance = closest.CommittedInstanceID();
        result.primitive = closest.CommittedPrimitiveIndex();
        float2 barycentric = closest.CommittedTriangleBarycentrics();
        result.u = barycentric.x;
        result.v = barycentric.y;
        result.distance = closest.CommittedRayT();
    }
    // First accepted hit is a binary occlusion result; its distance need not be nearest.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> visibility;
    visibility.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
    while (visibility.Proceed()) {}
    result.visible = visibility.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    output_hits[i] = result;
}
)";
    auto pipeline = take(owner.prepare(desc));
    // Far primitive comes first so nearest distance cannot be inferred from ordering.
    const std::array<Vertex, 6> initial_vertices = {
        {{-1, -1, 5}, {3, -1, 5}, {-1, 3, 5}, {-1, -1, 2}, {3, -1, 2}, {-1, 3, 2}}};
    auto vertices = gfxCreateBuffer<Vertex>(gfx, initial_vertices.size(), initial_vertices.data());
    // Geometry takes ownership of its buffer handles; a range shares mesh storage
    // while leaving the original mesh handle available to ordinary rendering.
    auto geometry_vertices = gfxCreateBufferRange(gfx, vertices, 0, vertices.getSize());
    auto geometry = gfxCreateGeometryTriangles(gfx, geometry_vertices, sizeof(Vertex));
    auto blas = gfxCreateBottomLevelAccelerationStructure(gfx);
    auto tlas = gfxCreateTopLevelAccelerationStructure(gfx);
    std::array<GfxTopLevelAccelerationStructureInstance, 2> instances = {
        gfxCreateTopLevelAccelerationStructureInstance(gfx), gfxCreateTopLevelAccelerationStructureInstance(gfx)};
    auto output = gfxCreateBuffer<Hit>(gfx, ray_count);
    auto initial_readback = gfxCreateBuffer<Hit>(gfx, ray_count, nullptr, kGfxCpuAccess_Read);
    auto readback = gfxCreateBuffer<Hit>(gfx, ray_count, nullptr, kGfxCpuAccess_Read);
    require(vertices && geometry_vertices && geometry && blas && tlas && instances[0] && instances[1] && output &&
                initial_readback && readback,
            "inline RayQuery resources unavailable");
    check(gfxGeometrySetOpaque(gfx, geometry, true));
    check(gfxBottomLevelAccelerationStructureAddGeometry(gfx, blas, geometry));
    const GfxBuildBottomLevelASFlags flags =
        kGfxBuildBottomLevelASFlag_Updateable | kGfxBuildBottomLevelASFlag_FastTrace;
    check(gfxBuildBottomLevelAccelerationStructures(gfx, &blas, &flags, 1));
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const std::array<float, 12> transform = {1, 0, 0, 8.0f * float(i), 0, 1, 0, 0, 0, 0, 1, float(i)};
        check(gfxTopLevelAccelerationStructureInstanceSetBottomLevelAccelerationStructure(gfx, instances[i], blas));
        check(gfxTopLevelAccelerationStructureInstanceSetTransform(gfx, instances[i], transform.data()));
        check(gfxTopLevelAccelerationStructureInstanceSetInstanceID(gfx, instances[i], i == 0 ? 17 : 29));
        check(gfxTopLevelAccelerationStructureInstanceSetInstanceMask(gfx, instances[i], 1));
        check(gfxTopLevelAccelerationStructureAddInstance(gfx, tlas, instances[i]));
    }
    check(gfxTopLevelAccelerationStructureBuild(gfx, tlas));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "scene", tlas));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "output_hits", output));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "origin_shift", 0.0f));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
    check(gfxCommandDispatch(gfx, ray_count, 1, 1));
    check(gfxCommandCopyBuffer(gfx, initial_readback, output));

    // Build, trace, update and trace share one submission; GPU ordering must suffice.
    auto updated_vertices = initial_vertices;
    for (auto& vertex : updated_vertices)
        vertex.z += 1.0f;
    auto replacement = gfxCreateBuffer<Vertex>(gfx, updated_vertices.size(), updated_vertices.data());
    require(replacement, "inline RayQuery updated geometry buffer unavailable");
    auto replacement_range = gfxCreateBufferRange(gfx, replacement, 0, replacement.getSize());
    require(replacement_range, "inline RayQuery updated geometry range unavailable");
    check(gfxGeometryTrianglesUpdate(gfx, geometry, replacement_range, sizeof(Vertex)));
    check(gfxUpdateBottomLevelAccelerationStructures(gfx, &blas, 1));
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        const std::array<float, 12> transform = {1, 0, 0, 4.0f + 8.0f * float(i), 0, 1, 0, 0, 0, 0, 1, 1.0f + float(i)};
        check(gfxTopLevelAccelerationStructureInstanceSetTransform(gfx, instances[i], transform.data()));
    }
    check(gfxTopLevelAccelerationStructureUpdate(gfx, tlas));
    check(gfxProgramSetParameter(gfx, pipeline.program(), "origin_shift", 4.0f));
    check(gfxCommandBindKernel(gfx, pipeline.kernel()));
    check(gfxCommandDispatch(gfx, ray_count, 1, 1));
    check(gfxCommandCopyBuffer(gfx, readback, output));
    // CPU identities retire now; encoded GPU work must retain their physical resources.
    check(gfxDestroyTopLevelAccelerationStructure(gfx, tlas));
    for (auto instance : instances)
        check(gfxDestroyTopLevelAccelerationStructureInstance(gfx, instance));
    check(gfxDestroyBottomLevelAccelerationStructure(gfx, blas));
    check(gfxDestroyGeometry(gfx, geometry));
    // Successful original-handle destruction also checks that AS range ownership
    // did not consume the mesh handles shared with the raster path.
    for (auto buffer : {vertices, replacement, output})
        check(gfxDestroyBuffer(gfx, buffer));
    pipeline = {};
    complete(owner.finish());
    verify(gfxBufferGetData<Hit>(gfx, initial_readback), 2.0f);
    verify(gfxBufferGetData<Hit>(gfx, readback), 4.0f);
    check(gfxDestroyBuffer(gfx, initial_readback));
    check(gfxDestroyBuffer(gfx, readback));
    complete(owner.close());
    check_debug(debug.Get());
    std::puts(
        "inline-ray-query: compute-SM6.5 BLAS/TLAS-build/trace/update/trace single-submission hit/miss/mask/instance/primitive/barycentric/closest-distance/first-hit-visibility retirement-before-finish=PASS");
}
} // namespace rhi_acceptance
