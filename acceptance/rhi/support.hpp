#pragma once

#include "../helpers.hpp"
#include "rhi/backend.hpp"

#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

namespace rhi_acceptance {

using Microsoft::WRL::ComPtr;

using atelier::acceptance::helpers::require;

using atelier::acceptance::helpers::take;

inline void check_hr(HRESULT result) {
    require(SUCCEEDED(result), "native D3D12 operation failed");
}

inline void check_nri(nri::Result result) {
    require(result == nri::Result::SUCCESS, "NRI operation failed");
}

inline bool expected_gpu_validation() {
    char mode[8]{};
    const auto length = GetEnvironmentVariableA("ATELIER_GPU_VALIDATION", mode, sizeof(mode));
    return std::string_view(mode, length < sizeof(mode) ? length : 0) != "debug";
}

inline ComPtr<ID3D12InfoQueue> debug_queue(ID3D12Device& device) {
    ComPtr<ID3D12InfoQueue> queue;
    check_hr(device.QueryInterface(IID_PPV_ARGS(&queue)));
    return queue;
}

inline void check_debug(ID3D12InfoQueue* queue) {
    if (!queue)
        return;
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
    queue->ClearStoredMessages();
    require(errors == 0, "D3D12 debug layer reported warnings/errors");
}

inline void print_adapter(atelier::rhi::Device& device) {
    ID3D12Device& d3d = atelier::rhi::BackendAccess::d3d12_device(device);
    const nri::DeviceDesc& desc =
        atelier::rhi::BackendAccess::core(device).GetDeviceDesc(atelier::rhi::BackendAccess::nri_device(device));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    check_hr(factory->EnumAdapterByLuid(d3d.GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 dxgi_desc{};
    check_hr(adapter->GetDesc1(&dxgi_desc));
    LARGE_INTEGER version{};
    check_hr(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version));
    std::printf("adapter=%s vendor=0x%04x driver=%u.%u.%u.%u sm=%u frames=%u\n", device.info().adapter_name.c_str(),
                static_cast<unsigned>(dxgi_desc.VendorId), static_cast<unsigned>(HIWORD(version.HighPart)),
                static_cast<unsigned>(LOWORD(version.HighPart)), static_cast<unsigned>(HIWORD(version.LowPart)),
                static_cast<unsigned>(LOWORD(version.LowPart)), static_cast<unsigned>(desc.shaderModel),
                device.info().frames_in_flight);
}

} // namespace rhi_acceptance
