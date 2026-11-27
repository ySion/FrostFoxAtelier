#include "detail/shader_compiler.hpp"
#include "foundation/sha256.hpp"
#include "platform/utf.hpp"

#include <combaseapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace atelier::rhi::detail {
namespace {
using Microsoft::WRL::ComPtr;

using Digest = std::array<std::uint8_t, 32>;

constexpr std::uint32_t cache_format = 4;

constexpr std::array<char, 4> cache_magic{'A', 'S', 'H', 'C'};

constexpr std::uintmax_t cache_limit = 512ull << 20;

constexpr std::uintmax_t cache_trim_target = 256ull << 20;

constexpr std::uint64_t max_cached_bytecode_bytes = 64ull << 20;

constexpr std::uint64_t trim_write_interval_bytes = 64ull << 20;

struct CacheHeader {
    std::array<char, 4> magic;

    std::uint32_t format;

    std::uint64_t bytecode_size;

    Digest checksum;
};

static_assert(sizeof(CacheHeader) == 48);

std::uint8_t hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9')
        return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<std::uint8_t>(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F')
        return static_cast<std::uint8_t>(10 + (c - 'A'));
    return 0u;
}

Digest hex_to_digest(std::string_view hex) noexcept {
    Digest out{};
    if (hex.size() < 64u)
        return out;
    for (std::size_t i = 0; i < 32u; ++i)
        out[i] = static_cast<std::uint8_t>((hex_nibble(hex[i * 2u]) << 4u) | hex_nibble(hex[i * 2u + 1u]));
    return out;
}

template <class Char>
void hash_text(foundation::Sha256& hash, std::basic_string_view<Char> text) noexcept {
    const std::uint64_t size = text.size();
    hash.add(std::as_bytes(std::span{&size, 1u}));
    hash.add(std::as_bytes(std::span{text.data(), text.size()}));
}

Digest checksum(std::string_view bytecode) noexcept {
    return hex_to_digest(foundation::sha256(std::as_bytes(std::span{bytecode.data(), bytecode.size()})));
}

std::string_view bytes(IDxcBlob& blob) noexcept {
    return {static_cast<const char*>(blob.GetBufferPointer()), blob.GetBufferSize()};
}

class IncludeProbe final : public IDxcIncludeHandler {
public:
    explicit IncludeProbe(IDxcIncludeHandler& inner) : inner_(inner) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR file, IDxcBlob** source) override {
        used = true;
        return inner_.LoadSource(file, source);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == __uuidof(IDxcIncludeHandler) || id == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }

    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    bool used{};

private:
    IDxcIncludeHandler& inner_;
};

void trim(const std::filesystem::path& directory) {
    std::error_code error;
    struct Entry {
        std::filesystem::file_time_type time;

        std::filesystem::path path;

        std::uintmax_t size;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error))
            continue;
        const auto time = it->last_write_time(entry_error);
        const auto size = it->file_size(entry_error);
        if (entry_error)
            continue;
        // Skip active temporary files written by concurrent worker threads; only reap stale orphans (> 1 hour).
        if (it->path().extension() == L".tmp" && (now - time) < std::chrono::hours(1))
            continue;
        entries.push_back({time, it->path(), size});
        total += size;
    }
    if (total <= cache_limit)
        return;
    std::ranges::sort(entries, {}, &Entry::time);
    for (const auto& entry : entries) {
        if (total <= cache_trim_target)
            break;
        if (std::filesystem::remove(entry.path, error))
            total -= entry.size;
    }
}

std::optional<CompiledShader> load(IDxcUtils& utils, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return std::nullopt;
    const auto end_pos = in.tellg();
    if (end_pos <= 0)
        return std::nullopt;
    const auto size = static_cast<std::uint64_t>(end_pos);
    CacheHeader header{};
    if (size < sizeof header || !in.seekg(0).read(reinterpret_cast<char*>(&header), sizeof header) ||
        header.magic != cache_magic || header.format != cache_format || !header.bytecode_size ||
        header.bytecode_size > max_cached_bytecode_bytes || header.bytecode_size != size - sizeof header)
        return std::nullopt;
    std::string payload(static_cast<std::size_t>(header.bytecode_size), '\0');
    if (!in.read(payload.data(), static_cast<std::streamsize>(payload.size())))
        return std::nullopt;
    if (checksum(payload) != header.checksum)
        return std::nullopt;
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(utils.CreateBlob(payload.data(), static_cast<UINT32>(payload.size()), DXC_CP_ACP, &blob)))
        return std::nullopt;
    CompiledShader cached;
    cached.bytecode = std::move(blob);
    return cached;
}

void store(const std::filesystem::path& directory, const std::filesystem::path& file, std::string_view bytecode) {
    if (bytecode.empty() || bytecode.size() > max_cached_bytecode_bytes)
        return;
    static std::atomic<bool> first_trim_done{false};
    static std::atomic<std::uint64_t> bytes_since_trim{0};
    if (!first_trim_done.exchange(true) ||
        bytes_since_trim.fetch_add(bytecode.size(), std::memory_order_relaxed) >= trim_write_interval_bytes) {
        bytes_since_trim.store(0, std::memory_order_relaxed);
        trim(directory);
    }

    const CacheHeader header{cache_magic, cache_format, bytecode.size(), checksum(bytecode)};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    auto temporary = file;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof header);
        out.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
        if (!out.good()) {
            out.close();
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error)
        std::filesystem::remove(temporary, error);
}
} // namespace

Result<ShaderCompiler> ShaderCompiler::open(std::filesystem::path cache) {
    ShaderCompiler compiler;
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&compiler.utils_));
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::creation_failed, hr, "DXC utils creation failed"});
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler.compiler_));
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::creation_failed, hr, "DXC compiler creation failed"});
    hr = compiler.utils_->CreateDefaultIncludeHandler(&compiler.includes_);
    if (FAILED(hr))
        return std::unexpected(Error{ErrorCode::creation_failed, hr, "DXC include handler creation failed"});
    ComPtr<IDxcVersionInfo> version;
    ComPtr<IDxcVersionInfo2> commit;
    UINT32 major = 0;
    UINT32 minor = 0;
    UINT32 commits = 0;
    char* hash = nullptr;
    if (SUCCEEDED(compiler.compiler_.As(&version)) && SUCCEEDED(version->GetVersion(&major, &minor)) &&
        SUCCEEDED(version.As(&commit)) && SUCCEEDED(commit->GetCommitInfo(&commits, &hash)) && hash)
        compiler.identity_ =
            std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(commits) + "." + hash;
    CoTaskMemFree(hash);
    if (!compiler.identity_.empty())
        compiler.cache_ = std::move(cache);
    return compiler;
}

Result<CompiledShader> ShaderCompiler::compile(const ShaderSource& source) {
    std::vector<LPCWSTR> arguments{source.name.c_str(), L"-enable-16bit-types"};
    for (LPCWSTR argument : {L"-I", L".", L"-T", source.profile.c_str(), L"-HV 2021", L"-Wno-parameter-usage",
                             L"-fdiagnostics-format=msvc", DXC_ARG_OPTIMIZATION_LEVEL3, DXC_ARG_RESOURCES_MAY_ALIAS,
                             L"-E", source.entry.c_str()})
        arguments.push_back(argument);

    std::filesystem::path entry;
    if (!cache_.empty()) {
        foundation::Sha256 key;
        key.add(std::as_bytes(std::span{&cache_format, 1u}));
        hash_text(key, std::string_view(identity_));
        // Skip arguments[0] (diagnostic source name) so identical stages shared across PSOs hit the disk cache.
        for (std::size_t i = 1u; i < arguments.size(); ++i)
            hash_text(key, std::wstring_view(arguments[i]));
        hash_text(key, std::string_view(source.text));
        entry = cache_ / (key.finish() + ".bin");
        if (auto cached = load(*utils_.Get(), entry))
            return std::move(*cached);
    }

    const auto name = platform::utf16_to_utf8(source.name);
    if (!name)
        return std::unexpected(Error{ErrorCode::shader_failed, E_INVALIDARG, "shader source name is not valid UTF-16"});
    const auto entry_point = platform::utf16_to_utf8(source.entry);
    if (!entry_point)
        return std::unexpected(Error{ErrorCode::shader_failed, E_INVALIDARG, "shader entry point is not valid UTF-16"});
    const DxcBuffer buffer{source.text.data(), source.text.size(), DXC_CP_UTF8};
    IncludeProbe probe(*includes_.Get());
    ComPtr<IDxcResult> result;
    const HRESULT compile_hr = compiler_->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()),
                                                  &probe, IID_PPV_ARGS(&result));
    if (FAILED(compile_hr) || !result)
        return std::unexpected(Error{ErrorCode::shader_failed, compile_hr, "DXC could not run for `" + *name + "'"});
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlobEncoding> diagnostics;
    result->GetErrorBuffer(&diagnostics);
    std::string text = diagnostics ? std::string(bytes(*diagnostics.Get())) : std::string{};
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    const auto described = "`" + *name + "' for entry point `" + *entry_point + "'";
    if (FAILED(status))
        return std::unexpected(Error{ErrorCode::shader_failed, status,
                                     "Failed to compile " + described + (text.empty() ? "" : ":\n" + text)});
    if (!text.empty())
        std::fprintf(stderr, "Compiled %s with warning(s):\n%s\n", described.c_str(), text.c_str());
    CompiledShader compiled;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compiled.bytecode), nullptr);
    if (!compiled.bytecode)
        return std::unexpected(Error{ErrorCode::shader_failed, E_FAIL, "DXC returned no bytecode for " + described});
    if (!entry.empty() && !probe.used)
        store(cache_, entry, bytes(*compiled.bytecode.Get()));
    return compiled;
}

} // namespace atelier::rhi::detail
