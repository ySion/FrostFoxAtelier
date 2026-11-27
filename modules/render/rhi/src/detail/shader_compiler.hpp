#pragma once

#include "rhi/rhi.hpp"

#include <Windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

namespace atelier::rhi::detail {

struct ShaderSource {
    std::wstring name;

    std::string text;

    std::wstring profile;

    std::wstring entry;
};

struct CompiledShader {
    Microsoft::WRL::ComPtr<IDxcBlob> bytecode;
};

// Per-worker DXC SM 6.6+ instance with SHA-256 disk caching (ASHC v2).
// Universal bindless root layout eliminates shader reflection overhead completely.
class ShaderCompiler {
public:
    [[nodiscard]] static Result<ShaderCompiler> open(std::filesystem::path cache);

    [[nodiscard]] Result<CompiledShader> compile(const ShaderSource& source);

private:
    Microsoft::WRL::ComPtr<IDxcUtils> utils_;

    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler_;

    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includes_;

    std::string identity_;

    std::filesystem::path cache_;
};

} // namespace atelier::rhi::detail
