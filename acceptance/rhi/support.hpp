#pragma once
#include "../helpers.hpp"
#include <gfx.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace rhi_acceptance {
using Microsoft::WRL::ComPtr;

using atelier::acceptance::helpers::require;

using atelier::acceptance::helpers::take;

inline void check(GfxResult result) {
    require(result == kGfxResult_NoError, "gfx operation failed");
}

inline void check_hr(HRESULT result) {
    require(SUCCEEDED(result), "native D3D12 operation failed");
}

inline bool expected_gpu_validation() {
    char mode[8]{};
    const auto length = GetEnvironmentVariableA("ATELIER_GPU_VALIDATION", mode, sizeof(mode));
    return std::string_view(mode, length < sizeof(mode) ? length : 0) != "debug";
}

inline ComPtr<ID3D12InfoQueue> debug_queue(GfxContext context) {
    ComPtr<ID3D12InfoQueue> queue;
    check_hr(gfxGetDevice(context)->QueryInterface(IID_PPV_ARGS(&queue)));
    return queue;
}

inline void check_debug(ID3D12InfoQueue* queue) {
    std::size_t errors = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check_hr(queue->GetMessage(i, nullptr, &size));
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        check_hr(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            ++errors;
            std::fprintf(stderr, "D3D12 [%u]: %s\n", static_cast<unsigned>(message->ID), message->pDescription);
        }
    }
    require(errors == 0, "D3D12 debug layer reported warnings/errors");
}

inline void enable_validation(bool gpu) {
    ComPtr<ID3D12Debug1> debug;
    check_hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    debug->SetEnableGPUBasedValidation(gpu ? TRUE : FALSE);
}

inline GfxContext open_native() {
    ComPtr<IDXGIFactory6> factory;
    check_hr(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result =
            factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        check_hr(result);
        DXGI_ADAPTER_DESC1 desc{};
        check_hr(adapter->GetDesc1(&desc));
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        auto context = gfxCreateContext(64, 32, kGfxCreateContextFlag_EnableDebugLayer, adapter.Get());
        if (context)
            return context;
    }
    atelier::fatal("hardware gfx context unavailable");
}

inline void print_adapter(GfxContext context) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    check_hr(factory->EnumAdapterByLuid(gfxGetDevice(context)->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    LARGE_INTEGER version{};
    check_hr(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version));
    std::printf("adapter=%s vendor=0x%04x driver=%u.%u.%u.%u frames=%u\n", context.getName(), context.getVendorId(),
                static_cast<unsigned>(HIWORD(version.HighPart)), static_cast<unsigned>(LOWORD(version.HighPart)),
                static_cast<unsigned>(HIWORD(version.LowPart)), static_cast<unsigned>(LOWORD(version.LowPart)),
                gfxGetBackBufferCount(context));
}
} // namespace rhi_acceptance
