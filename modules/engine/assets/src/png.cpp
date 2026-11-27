#include "assets/png.hpp"
#include "platform/error.hpp"
#include "platform/windows_path.hpp"
#include <wincodec.h>
#include <wrl/client.h>

namespace atelier::assets {
Result<void> write_png(std::uint32_t width, std::uint32_t height, Span<const std::uint8_t> rgba,
                       const std::filesystem::path& path) {
    const auto pixels = std::uint64_t{width} * height;
    if (!pixels || pixels > UINT_MAX / 4 || rgba.size() != pixels * 4)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid RGBA image extent or payload."});
    using Microsoft::WRL::ComPtr;
    const auto check = [](HRESULT result) -> Result<void> {
        if (FAILED(result))
            return std::unexpected(Error{ErrorCode::invalid_storage, "PNG encoding failed: " + std::to_string(result)});
        return {};
    };
    ComPtr<IWICImagingFactory> factory;
    ATELIER_TRY(
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
    ComPtr<IWICStream> stream;
    ATELIER_TRY(check(factory->CreateStream(&stream)));
    const auto native = platform::windows_file_path(path);
    if (!native)
        return std::unexpected(Error{ErrorCode::invalid_storage, platform::error_message(native.error())});
    ATELIER_TRY(check(stream->InitializeFromFilename(native->c_str(), GENERIC_WRITE)));
    ComPtr<IWICBitmapEncoder> encoder;
    ATELIER_TRY(check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    ATELIER_TRY(check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    ComPtr<IWICBitmapFrameEncode> frame;
    ATELIER_TRY(check(encoder->CreateNewFrame(&frame, nullptr)));
    ATELIER_TRY(check(frame->Initialize(nullptr)));
    ATELIER_TRY(check(frame->SetSize(width, height)));
    auto format = GUID_WICPixelFormat32bppBGRA;
    ATELIER_TRY(check(frame->SetPixelFormat(&format)));
    if (format != GUID_WICPixelFormat32bppBGRA)
        return std::unexpected(Error{ErrorCode::invalid_storage, "PNG codec changed BGRA format"});
    // Channel permutation only: Image bytes may be linear data or sRGB. Never
    // apply a transfer function, premultiply alpha, or quantize through a capture.
    Array<std::uint8_t> bgra;
    bgra.resize_uninitialized(rgba.size());
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4) {
        bgra[pixel] = rgba[pixel + 2];
        bgra[pixel + 1] = rgba[pixel + 1];
        bgra[pixel + 2] = rgba[pixel];
        bgra[pixel + 3] = rgba[pixel + 3];
    }
    ATELIER_TRY(check(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data())));
    ATELIER_TRY(check(frame->Commit()));
    ATELIER_TRY(check(encoder->Commit()));
    return {};
}
}
