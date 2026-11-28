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
            return std::unexpected(Error{ErrorCode::invalid_storage, String::concat("PNG encoding failed: ", result)});
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

Result<Array<std::byte>> encode_png_bytes(std::uint32_t width, std::uint32_t height, Span<const std::uint8_t> rgba) {
    const auto pixels = std::uint64_t{width} * height;
    if (!pixels || pixels > UINT_MAX / 4 || rgba.size() != pixels * 4)
        return std::unexpected(Error{ErrorCode::argument_mismatch, "Invalid RGBA image extent or payload."});
    using Microsoft::WRL::ComPtr;
    const auto check = [](HRESULT result) -> Result<void> {
        if (FAILED(result))
            return std::unexpected(
                Error{ErrorCode::invalid_storage, String::concat("PNG encoding failed: ", static_cast<std::int32_t>(result))});
        return {};
    };
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return std::unexpected(Error{ErrorCode::invalid_storage, "Cannot initialize PNG encoder COM apartment."});
    struct Apartment {
        bool owns;

        ~Apartment() {
            if (owns)
                CoUninitialize();
        }
    } apartment{SUCCEEDED(init)};

    ComPtr<IWICImagingFactory> factory;
    ATELIER_TRY(
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
    ComPtr<IStream> mem_stream;
    ATELIER_TRY(check(CreateStreamOnHGlobal(nullptr, TRUE, &mem_stream)));
    ComPtr<IWICBitmapEncoder> encoder;
    ATELIER_TRY(check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    ATELIER_TRY(check(encoder->Initialize(mem_stream.Get(), WICBitmapEncoderNoCache)));
    ComPtr<IWICBitmapFrameEncode> frame;
    ATELIER_TRY(check(encoder->CreateNewFrame(&frame, nullptr)));
    ATELIER_TRY(check(frame->Initialize(nullptr)));
    ATELIER_TRY(check(frame->SetSize(width, height)));
    auto format = GUID_WICPixelFormat32bppBGRA;
    ATELIER_TRY(check(frame->SetPixelFormat(&format)));
    if (format != GUID_WICPixelFormat32bppBGRA)
        return std::unexpected(Error{ErrorCode::invalid_storage, "PNG codec changed BGRA format"});
    Array<std::uint8_t> bgra(rgba);
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    ATELIER_TRY(check(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data())));
    ATELIER_TRY(check(frame->Commit()));
    ATELIER_TRY(check(encoder->Commit()));

    STATSTG stat{};
    ATELIER_TRY(check(mem_stream->Stat(&stat, STATFLAG_NONAME)));
    if (stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > (256ull << 20))
        return std::unexpected(Error{ErrorCode::invalid_storage, "Encoded PNG stream size out of bounds."});
    Array<std::byte> out;
    out.resize_uninitialized(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ATELIER_TRY(check(mem_stream->Seek(zero, STREAM_SEEK_SET, nullptr)));
    ULONG read_bytes = 0;
    ATELIER_TRY(check(mem_stream->Read(out.data(), static_cast<ULONG>(out.size()), &read_bytes)));
    if (read_bytes != out.size())
        return std::unexpected(Error{ErrorCode::invalid_storage, "Truncated PNG memory stream read."});
    return out;
}
}
