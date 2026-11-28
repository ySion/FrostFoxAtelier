#include "detail/shader_compiler.hpp"
#include "foundation/array.hpp"
#include "foundation/inline_array.hpp"
#include "foundation/optional.hpp"
#include "platform/utf.hpp"

#include <bcrypt.h>
#include <combaseapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>

namespace atelier::rhi::detail {
namespace {
using Microsoft::WRL::ComPtr;

using Digest = std::array<std::uint8_t, 32>;

// Changing the entry layout or key composition bumps this; older entries then
// never match and age out through trimming.
constexpr std::uint32_t cache_format = 1;

constexpr std::array<char, 4> cache_magic{'A', 'S', 'H', 'C'};

constexpr std::uintmax_t cache_limit = 512ull << 20;

constexpr std::uintmax_t cache_trim_target = 256ull << 20;

struct CacheHeader {
    std::array<char, 4> magic;

    std::uint32_t format;

    std::uint64_t bytecode_size;

    std::uint64_t reflection_size;

    Digest checksum; // Bytecode followed by reflection.
};

static_assert(sizeof(CacheHeader) == 56);

class Sha256 {
public:
    Sha256() { ok_ = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash_, nullptr, 0, nullptr, 0, 0)); }

    ~Sha256() {
        if (hash_)
            BCryptDestroyHash(hash_);
    }

    Sha256(const Sha256&) = delete;

    Sha256& operator=(const Sha256&) = delete;

    void add(const void* data, std::size_t size) {
        auto* bytes = static_cast<const UCHAR*>(data);
        while (ok_ && size) {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(size, 1u << 30));
            ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0));
            bytes += chunk;
            size -= chunk;
        }
    }

    void add_bytes(Span<const std::byte> data) {
        const std::uint64_t size = data.size();
        add(&size, sizeof size);
        add(data.data(), data.size());
    }

    void add_wide(std::wstring_view text) {
        const std::uint64_t size = text.size();
        add(&size, sizeof size);
        add(text.data(), text.size() * sizeof(wchar_t));
    }

    Optional<Digest> finish() {
        Digest digest{};
        if (!ok_ || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0)))
            return {};
        return digest;
    }

private:
    BCRYPT_HASH_HANDLE hash_{};

    bool ok_{};
};

Optional<Digest> checksum(Span<const std::byte> bytecode, Span<const std::byte> reflection) {
    Sha256 hash;
    hash.add(bytecode.data(), bytecode.size());
    hash.add(reflection.data(), reflection.size());
    return hash.finish();
}

Span<const std::byte> bytes(IDxcBlob& blob) {
    return {static_cast<const std::byte*>(blob.GetBufferPointer()), blob.GetBufferSize()};
}

// Records whether compilation loaded any file; only include-free results are
// fully described by the source text and may be shared through the cache.
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

// Oldest entries go first once the directory exceeds its limit. Files another
// process holds open simply stay; every failure here leaves compilation intact.
void trim(const std::filesystem::path& directory) {
    std::error_code error;
    struct Entry {
        std::filesystem::file_time_type time;

        std::filesystem::path path;

        std::uintmax_t size;
    };
    Array<Entry> entries;
    std::uintmax_t total = 0;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error))
            continue;
        const auto size = it->file_size(entry_error);
        const auto time = it->last_write_time(entry_error);
        if (entry_error)
            continue;
        entries.add({time, it->path(), size});
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

// Any unreadable, truncated or inconsistent entry is a miss; compilation then overwrites it.
Optional<CompiledShader> load(IDxcUtils& utils, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const auto size = static_cast<std::uint64_t>(in.tellg());
    CacheHeader header{};
    if (size < sizeof header || !in.seekg(0).read(reinterpret_cast<char*>(&header), sizeof header) ||
        header.magic != cache_magic || header.format != cache_format || !header.bytecode_size ||
        !header.reflection_size || header.bytecode_size >= size - sizeof header ||
        header.reflection_size != size - sizeof header - header.bytecode_size)
        return {};
    Array<std::byte> payload;
    payload.resize_uninitialized(static_cast<std::size_t>(size - sizeof header));
    if (!in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())))
        return {};
    const auto bytecode = payload.view().first(header.bytecode_size);
    const auto reflection = payload.view().slice(header.bytecode_size, header.reflection_size);
    CompiledShader cached;
    ComPtr<IDxcBlobEncoding> blob;
    const DxcBuffer reflection_buffer{reflection.data(), reflection.size(), DXC_CP_ACP};
    if (checksum(bytecode, reflection) != header.checksum ||
        FAILED(utils.CreateBlob(bytecode.data(), static_cast<UINT32>(bytecode.size()), DXC_CP_ACP, &blob)) ||
        FAILED(utils.CreateReflection(&reflection_buffer, IID_PPV_ARGS(&cached.reflection))))
        return {};
    cached.bytecode = std::move(blob);
    return cached;
}

void store(const std::filesystem::path& directory, const std::filesystem::path& file, Span<const std::byte> bytecode,
           Span<const std::byte> reflection) {
    static std::once_flag trimmed;
    std::call_once(trimmed, [&] { trim(directory); });
    const auto digest = checksum(bytecode, reflection);
    if (!digest)
        return;
    const CacheHeader header{cache_magic, cache_format, bytecode.size(), reflection.size(), *digest};
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    auto temporary = file;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof header);
        out.write(reinterpret_cast<const char*>(bytecode.data()), static_cast<std::streamsize>(bytecode.size()));
        out.write(reinterpret_cast<const char*>(reflection.data()), static_cast<std::streamsize>(reflection.size()));
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
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&compiler.utils_))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler.compiler_))) ||
        FAILED(compiler.utils_->CreateDefaultIncludeHandler(&compiler.includes_)))
        return std::unexpected(
            Error{ErrorCode::creation_failed, kGfxResult_InternalError, "DXC instance creation failed"});
    ComPtr<IDxcVersionInfo> version;
    ComPtr<IDxcVersionInfo2> commit;
    UINT32 major = 0;
    UINT32 minor = 0;
    UINT32 commits = 0;
    char* hash = nullptr;
    if (SUCCEEDED(compiler.compiler_.As(&version)) && SUCCEEDED(version->GetVersion(&major, &minor)) &&
        SUCCEEDED(version.As(&commit)) && SUCCEEDED(commit->GetCommitInfo(&commits, &hash)) && hash) {
        if (const auto valid_hash = String::from_utf8(hash))
            compiler.identity_ = String::concat(major, ".", minor, ".", commits, ".", *valid_hash);
    }
    CoTaskMemFree(hash);
    // Without a compiler identity a cached entry could outlive the compiler that produced it.
    if (!compiler.identity_.empty())
        compiler.cache_ = std::move(cache);
    return compiler;
}

Result<CompiledShader> ShaderCompiler::compile(const ShaderSource& source) {
    InlineArray<LPCWSTR, 16> arguments{
        source.name.c_str(),     L"-enable-16bit-types",     L"-I",  L".",
        L"-T",                   source.profile.c_str(),     L"-HV 2021",
        L"-Wno-parameter-usage", L"-fdiagnostics-format=msvc", DXC_ARG_ALL_RESOURCES_BOUND,
        DXC_ARG_RESOURCES_MAY_ALIAS, L"-E",                  source.entry.c_str()};

    std::filesystem::path entry;
    if (!cache_.empty()) {
        Sha256 key;
        key.add(&cache_format, sizeof cache_format);
        key.add_bytes(as_bytes(identity_.view()));
        for (const auto* argument : arguments)
            key.add_wide(std::wstring_view(argument));
        key.add_bytes(as_bytes(source.text.view()));
        if (const auto digest = key.finish()) {
            wchar_t file_name[68]{};
            for (std::size_t i = 0; i < digest->size(); ++i) {
                constexpr wchar_t digits[] = L"0123456789abcdef";
                const auto byte = (*digest)[i];
                file_name[i * 2] = digits[byte >> 4];
                file_name[i * 2 + 1] = digits[byte & 15];
            }
            std::memcpy(file_name + 64, L".bin", 4 * sizeof(wchar_t));
            entry = cache_ / std::wstring_view{file_name, 68};
            if (auto cached = load(*utils_.Get(), entry))
                return std::move(*cached);
        }
    }

    const auto name = platform::utf16_to_utf8(source.name);
    if (!name)
        return std::unexpected(
            Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter, "shader source name is not valid UTF-16"});
    const auto entry_point = platform::utf16_to_utf8(source.entry);
    if (!entry_point)
        return std::unexpected(
            Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter, "shader entry point is not valid UTF-16"});
    const DxcBuffer buffer{source.text.data(), source.text.size(), DXC_CP_ACP};
    IncludeProbe probe(*includes_.Get());
    ComPtr<IDxcResult> result;
    if (FAILED(compiler_->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()), &probe,
                                  IID_PPV_ARGS(&result))) ||
        !result)
        return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InternalError,
                                     String::concat("DXC could not run for `", *name, "'")});
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    ComPtr<IDxcBlobEncoding> diagnostics;
    result->GetErrorBuffer(&diagnostics);
    StringView raw_diagnostics =
        diagnostics ? StringView{static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()}
                    : StringView{};
    while (!raw_diagnostics.empty() &&
           (raw_diagnostics.back() == '\0' || raw_diagnostics.back() == '\n' || raw_diagnostics.back() == '\r'))
        raw_diagnostics.remove_suffix(1);
    String text;
    if (!raw_diagnostics.empty())
        text = String::from_utf8(raw_diagnostics).value_or("<non-UTF-8 DXC diagnostic>"_s);
    const auto described = String::concat("`", *name, "' for entry point `", *entry_point, "'");
    if (FAILED(status))
        return std::unexpected(
            Error{ErrorCode::shader_failed, kGfxResult_InvalidParameter,
                  text.empty() ? String::concat("Failed to compile ", described)
                               : String::concat("Failed to compile ", described, ":\n", text)});
    if (!text.empty())
        std::fprintf(stderr, "Compiled %s with warning(s):\n%s\n", described.c_str(), text.c_str());
    CompiledShader compiled;
    ComPtr<IDxcBlob> reflection;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compiled.bytecode), nullptr);
    result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflection), nullptr);
    if (!compiled.bytecode || !reflection)
        return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InternalError,
                                     String::concat("DXC returned no bytecode or reflection for ", described)});
    const DxcBuffer reflection_buffer{reflection->GetBufferPointer(), reflection->GetBufferSize(), DXC_CP_ACP};
    if (FAILED(utils_->CreateReflection(&reflection_buffer, IID_PPV_ARGS(&compiled.reflection))))
        return std::unexpected(Error{ErrorCode::shader_failed, kGfxResult_InternalError,
                                     String::concat("DXC reflection is unreadable for ", described)});
    if (!entry.empty() && !probe.used)
        store(cache_, entry, bytes(*compiled.bytecode.Get()), bytes(*reflection.Get()));
    return compiled;
}

} // namespace atelier::rhi::detail
