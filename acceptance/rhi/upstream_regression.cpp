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
    require(gfxFinish(gfx) == kGfxResult_Timeout, "blocked finish did not time out");
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
    std::printf("timeout: retained=PASS drain-only=PASS increments=%u debug-errors=0\n", increments);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected --null-list or --timeout");
        if (std::strcmp(argv[1], "--null-list") == 0)
            null_list();
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
