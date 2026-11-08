#include "assets/image_decode.hpp"
#include "foundation/sha256.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <optional>
#include <mutex>
#include <thread>

namespace atelier::assets {
namespace {
// Pixels one decode_images call may return; charged before each allocation.
struct PixelBudget {
    std::atomic<std::uint64_t> left;

    std::uint64_t limit;

    Result<void> charge(std::uint64_t bytes) {
        for (auto value = left.load();;) {
            if (value < bytes)
                return std::unexpected(Error{ErrorCode::invalid_storage,
                                             "Decoded textures exceed " + std::to_string(limit >> 20) + " MiB."});
            if (left.compare_exchange_weak(value, value - bytes))
                return {};
        }
    }
};

Result<material::ImageRef> decode(std::string_view bytes, PixelBudget* budget) {
    using Microsoft::WRL::ComPtr;
    const auto require = [](bool valid, const char* message) -> Result<void> {
        if (!valid)
            return std::unexpected(Error{ErrorCode::invalid_storage, message});
        return {};
    };
    ATELIER_TRY(require(bytes.size() <= 128 * 1024 * 1024, "Texture source exceeds 128 MiB."));
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ATELIER_TRY(
        require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize texture decoder."));
    struct Apartment {
        bool owns;

        ~Apartment() {
            if (owns)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ATELIER_TRY(require(
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
        "Cannot create texture decoder."));
    const auto opened =
        require(SUCCEEDED(factory->CreateStream(&stream)) &&
                    SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                                           static_cast<DWORD>(bytes.size()))) &&
                    SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                                               &decoder)) &&
                    SUCCEEDED(decoder->GetFrame(0, &frame)),
                "Cannot decode texture.");
    if (!opened)
        return std::unexpected(opened.error());
    UINT width{};
    UINT height{};
    ATELIER_TRY(
        require(SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 8192 && height <= 8192,
                "Texture exceeds the 8192-pixel dimension limit."));
    if (budget)
        ATELIER_TRY(budget->charge(std::uint64_t(width) * height * 4));
    const auto converted =
        require(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                                    nullptr, 0, WICBitmapPaletteTypeCustom)),
                "Cannot convert texture to RGBA.");
    if (!converted)
        return std::unexpected(converted.error());
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
    ATELIER_TRY(
        require(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())),
                "Cannot read texture pixels."));
    return material::Image::create(width, height, std::move(pixels));
}
}

Result<material::ImageRef> decode_image(std::string_view bytes) {
    return decode(bytes, nullptr);
}

Result<std::vector<material::ImageRef>> decode_images(std::span<const std::string_view> sources, ImageCache* cache,
                                                      std::uint64_t pixel_budget) {
    std::vector<material::ImageRef> result(sources.size());
    if (sources.empty())
        return result;
    std::mutex guard; // Cache entries and the first failure.
    std::optional<Error> failure;
    std::atomic<std::size_t> next{};
    std::atomic<bool> stopped{};
    PixelBudget budget{pixel_budget, pixel_budget};
    // decode initializes COM on the calling thread, so every worker owns its apartment.
    const auto work = [&] {
        for (std::size_t i{}; !stopped && (i = next++) < sources.size();) {
            auto completed = [&]() -> Result<void> {
                std::string digest;
                if (cache) {
                    digest = foundation::sha256(std::as_bytes(std::span(sources[i].data(), sources[i].size())));
                    std::scoped_lock lock(guard);
                    if (const auto found = cache->entries.find(digest); found != cache->entries.end())
                        result[i] = found->second.lock();
                }
                if (result[i]) {
                    return budget.charge(result[i]->pixels().size());
                }
                ATELIER_TRY_BIND(decoded, decode(sources[i], &budget));
                if (cache) {
                    std::scoped_lock lock(guard);
                    auto& entry = cache->entries[digest];
                    if (auto shared = entry.lock())
                        *decoded = std::move(shared);
                    else
                        entry = *decoded;
                }
                result[i] = std::move(*decoded);
                return {};
            }();
            if (!completed) {
                std::scoped_lock lock(guard);
                if (!failure)
                    failure = std::move(completed.error());
                stopped = true;
            }
        }
    };
    {
        const auto threads =
            std::min<std::size_t>({sources.size(), 8, std::max(1u, std::thread::hardware_concurrency() / 2)});
        std::vector<std::jthread> workers; // Joined before the shared state above leaves scope.
        workers.reserve(threads - 1);
        // Only startup is fatal; decoding failures still join and report below.
        [&]() noexcept {
            for (std::size_t i = 1; i < threads; ++i)
                workers.emplace_back(work);
        }();
        work();
    }
    if (failure)
        return std::unexpected(std::move(*failure));
    return result;
}
}
