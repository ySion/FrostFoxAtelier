#pragma once

#include "rhi/rhi.hpp"

#include <d3d12shader.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

namespace atelier::rhi::detail {

struct ShaderSource {
    std::wstring name; // DXC main-file name: diagnostics and relative includes.
    std::string text;
    std::wstring profile, entry;
};

struct CompiledShader {
    Microsoft::WRL::ComPtr<IDxcBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
};

// One DXC instance set, used by one thread at a time. Arguments match gfx's
// own compilation so both paths produce the same DXIL. With a cache directory,
// stages that loaded no include file are stored by content key; unreadable,
// truncated or mismatched entries are misses and are overwritten.
class ShaderCompiler {
public:
    [[nodiscard]] static Result<ShaderCompiler> open(std::filesystem::path cache);
    [[nodiscard]] Result<CompiledShader> compile(const ShaderSource& source);

private:
    Microsoft::WRL::ComPtr<IDxcUtils> utils_;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler_;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includes_;
    std::string identity_; // Compiler version and commit; part of every cache key.
    std::filesystem::path cache_;
};

} // namespace atelier::rhi::detail
