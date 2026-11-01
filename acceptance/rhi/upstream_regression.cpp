#include "support.hpp"
#include "shaders.hpp"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <utility>

using namespace rhi_acceptance;

namespace {
struct NativeOwner {
    GfxContext value;

    ~NativeOwner() {
        if (value && gfxDestroyContext(value) != kGfxResult_NoError)
            std::abort();
    }

    void close() {
        check(gfxDestroyContext(value));
        value = {};
    }
};

void null_list() {
    enable_validation(false);
    NativeOwner original{open_native()};
    ComPtr<ID3D12Device> device = gfxGetDevice(original.value);
    auto debug = debug_queue(original.value);
    original.close();
    NativeOwner interop{gfxCreateContext(device.Get())};
    require(interop.value && !gfxGetCommandList(interop.value),
            "expected legal interop context without a command list");
    GfxProgramDesc desc;
    desc.cs = increment_source;
    auto program = gfxCreateProgram(interop.value, desc, "null-list");
    auto kernel = gfxCreateComputeKernel(interop.value, program);
    require(program && kernel && gfxKernelGetPipelineState(interop.value, kernel), "CPU pipeline preparation failed");
    // Deliberately retain the kernel until context cleanup, exercising terminate.
    interop.close();
    check_debug(debug.Get());
    std::puts("null-list: CPU pipeline created; context cleanup PASS; debug-warnings=0 debug-errors=0");
}

void mesh_pixel() {
    enable_validation(true);
    NativeOwner owner{open_native()};
    const auto gfx = owner.value;
    require(gfxIsMeshShaderSupported(gfx), "mesh support required");
    auto debug = debug_queue(gfx);
    print_adapter(gfx);
    GfxProgramDesc compute_desc;
    compute_desc.cs = increment_source;
    auto compute_program = gfxCreateProgram(gfx, compute_desc, "mesh-pixel-producer");
    auto compute = gfxCreateComputeKernel(gfx, compute_program);
    GfxProgramDesc mesh_desc;
    mesh_desc.ms = mesh_source;
    mesh_desc.ps = mesh_pixel_source;
    auto mesh_program = gfxCreateProgram(gfx, mesh_desc, "mesh-pixel-consumer");
    GfxDrawState state;
    check(gfxDrawStateSetColorTarget(state, 0, DXGI_FORMAT_R8G8B8A8_UNORM));
    check(gfxDrawStateSetCullMode(state, D3D12_CULL_MODE_NONE));
    auto mesh = gfxCreateMeshKernel(gfx, mesh_program, state);
    require(compute && mesh && gfxKernelGetPipelineState(gfx, mesh), "regression pipeline preparation failed");
    std::uint32_t zero = 0;
    auto counter = gfxCreateBuffer<std::uint32_t>(gfx, 1, &zero);
    auto target = gfxCreateTexture2D(gfx, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto readback = gfxCreateBuffer(gfx, 64 * 32 * 4, nullptr, kGfxCpuAccess_Read);
    require(counter && target && readback, "regression resources unavailable");
    check(gfxProgramSetParameter(gfx, compute_program, "counter", counter));
    check(gfxCommandBindKernel(gfx, compute));
    check(gfxCommandDispatch(gfx, 1, 1, 1));
    check(gfxCommandClearTexture(gfx, target));
    check(gfxCommandBindColorTarget(gfx, 0, target));
    check(gfxProgramSetParameter(gfx, mesh_program, "values", counter));
    check(gfxCommandBindKernel(gfx, mesh));
    check(gfxCommandDrawMesh(gfx, 1, 1, 1));
    check(gfxCommandCopyTextureToBuffer(gfx, readback, target));
    check(gfxFinish(gfx));
    const auto* pixels = gfxBufferGetData<std::uint8_t>(gfx, readback);
    require(pixels != nullptr, "regression readback unavailable");
    for (std::uint32_t pixel = 0; pixel < 64 * 32; ++pixel)
        require(pixels[pixel * 4] == 1 && pixels[pixel * 4 + 1] == 0 && pixels[pixel * 4 + 2] == 0 &&
                    pixels[pixel * 4 + 3] == 255,
                "mesh PS did not consume the compute-produced SRV");
    check(gfxDestroyBuffer(gfx, counter));
    check(gfxDestroyBuffer(gfx, readback));
    check(gfxDestroyTexture(gfx, target));
    check(gfxDestroyKernel(gfx, mesh));
    check(gfxDestroyKernel(gfx, compute));
    check(gfxDestroyProgram(gfx, mesh_program));
    check(gfxDestroyProgram(gfx, compute_program));
    owner.close();
    check_debug(debug.Get());
    std::puts("mesh-pixel-state: 2048 pixels PASS; gpu-validation=1 debug-warnings=0 debug-errors=0");
}

void timeout_once() {
    enable_validation(false);
    NativeOwner owner{open_native()};
    const auto gfx = owner.value;
    auto debug = debug_queue(gfx);
    auto* device = gfxGetDevice(gfx);
    ComPtr<ID3D12Fence> gate;
    check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    struct GateRelease {
        ID3D12Fence* fence;

        ~GateRelease() { fence->Signal(1); }
    } release{gate.Get()};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(std::uint32_t);
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> external_readback;
    check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&external_readback)));
    auto readback = gfxCreateBuffer<std::uint32_t>(gfx, external_readback.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    std::uint32_t zero = 0;
    auto counter = gfxCreateBuffer<std::uint32_t>(gfx, 1, &zero);
    GfxProgramDesc source;
    source.cs = increment_source;
    auto program = gfxCreateProgram(gfx, source, "exactly-once");
    auto kernel = gfxCreateComputeKernel(gfx, program);
    require(counter && readback && program && kernel && gfxKernelGetPipelineState(gfx, kernel), "timeout setup failed");
    // Commit initialization separately. The blocked list may only increment once.
    check(gfxFinish(gfx));
    check_hr(gfxGetCommandQueue(gfx)->Wait(gate.Get(), 1));
    check(gfxSetWaitTimeout(gfx, 1));
    check(gfxProgramSetParameter(gfx, program, "counter", counter));
    check(gfxCommandBindKernel(gfx, kernel));
    check(gfxCommandDispatch(gfx, 1, 1, 1));
    check(gfxCommandCopyBuffer(gfx, readback, counter));
    const ULONGLONG begin = GetTickCount64();
    require(gfxFinish(gfx) == kGfxResult_Timeout, "blocked finish did not time out");
    require(GetTickCount64() - begin < 2000, "GPU wait was not bounded");
    require(gfxFinish(gfx) == kGfxResult_Timeout, "failed context accepted another finish");
    require(gfxFrame(gfx) == kGfxResult_Timeout, "failed context advanced a frame");
    require(gfxExecute(gfx) == kGfxResult_Timeout, "failed context accepted another submission");
    require(gfxResetCommandList(gfx) == kGfxResult_Timeout, "failed context reset its command list");
    require(gfxCommandDispatch(gfx, 1, 1, 1) == kGfxResult_InvalidOperation, "failed context accepted encoding");
    require(gfxDestroyContext(gfx) == kGfxResult_Timeout, "blocked destruction did not retain the context");
    require(gfxContextIsValid(gfx), "timeout discarded a valid device");
    check_hr(gate->Signal(1));
    check(gfxSetWaitTimeout(gfx, 10000));
    check(gfxDestroyBuffer(gfx, counter));
    check(gfxDestroyBuffer(gfx, readback));
    check(gfxDestroyKernel(gfx, kernel));
    check(gfxDestroyProgram(gfx, program));
    owner.close();
    void* data = nullptr;
    const D3D12_RANGE read_range{0, sizeof(std::uint32_t)};
    check_hr(external_readback->Map(0, &read_range, &data));
    const std::uint32_t increments = *static_cast<const std::uint32_t*>(data);
    const D3D12_RANGE written{0, 0};
    external_readback->Unmap(0, &written);
    require(increments == 1, "destroy replayed or discarded already-submitted work");
    check_debug(debug.Get());
    std::printf(
        "timeout: bounded=PASS retained=PASS poisoned-admission=PASS drain-only=PASS increments=%u debug-errors=0\n",
        increments);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected --null-list, --mesh-pixel-state or --timeout");
        if (std::strcmp(argv[1], "--null-list") == 0)
            null_list();
        else if (std::strcmp(argv[1], "--mesh-pixel-state") == 0)
            mesh_pixel();
        else if (std::strcmp(argv[1], "--timeout") == 0)
            timeout_once();
        else
            throw std::runtime_error("unknown regression case");
        std::puts("gfx regression: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gfx regression: FAIL: %s\n", error.what());
        return 1;
    }
}
