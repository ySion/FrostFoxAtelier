#include "editor/image_decode.hpp"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace atelier::editor {
material::ImageRef decode_image(std::string_view bytes) {
    using Microsoft::WRL::ComPtr;
    const auto require=[](bool valid,const char* message){if(!valid) throw std::runtime_error(message);};
    require(bytes.size()<=128*1024*1024,"Texture source exceeds 128 MiB.");
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    require(SUCCEEDED(initialized) || initialized==RPC_E_CHANGED_MODE,"Cannot initialize texture decoder.");
    struct Apartment {bool owns;~Apartment(){if(owns) CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"Cannot create texture decoder.");
    require(SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),static_cast<DWORD>(bytes.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)) &&
        SUCCEEDED(decoder->GetFrame(0,&frame)),"Cannot decode texture.");
    UINT width{},height{};require(SUCCEEDED(frame->GetSize(&width,&height)) && width && height && width<=8192 && height<=8192,
        "Texture exceeds the 8192-pixel dimension limit.");
    require(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),
        "Cannot convert texture to RGBA.");
    std::vector<std::uint8_t> pixels(std::size_t(width)*height*4);
    require(SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data())),"Cannot read texture pixels.");
    return std::make_shared<const material::Image>(width,height,std::move(pixels));
}
}
