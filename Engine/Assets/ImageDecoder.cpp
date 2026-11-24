#include "ImageDecoder.h"

#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace Aether
{
    namespace
    {
        void Check(HRESULT result, const char* operation)
        {
            if (FAILED(result))
                throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
        }
    }

    DecodedImage DecodeImage(std::span<const std::byte> encoded)
    {
        if (encoded.empty() || encoded.size() > std::numeric_limits<DWORD>::max())
            throw std::runtime_error("Invalid encoded image size");

        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (initialized != RPC_E_CHANGED_MODE)
            Check(initialized, "COM initialization");
        struct ComScope
        {
            bool initialized;
            ~ComScope() { if (initialized) CoUninitialize(); }
        } com{SUCCEEDED(initialized)};

        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())), "WIC factory creation");
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(stream.GetAddressOf()), "WIC stream creation");
        Check(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(encoded.data())),
            static_cast<DWORD>(encoded.size())), "WIC stream initialization");
        ComPtr<IWICBitmapDecoder> decoder;
        Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
            decoder.GetAddressOf()), "Image decoder creation");
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(0, frame.GetAddressOf()), "Image frame extraction");
        DecodedImage result;
        Check(frame->GetSize(&result.width, &result.height), "Image dimensions");
        if (!result.width || !result.height ||
            static_cast<uint64_t>(result.width) * result.height > std::numeric_limits<UINT>::max() / 4)
            throw std::runtime_error("Decoded image is too large");

        ComPtr<IWICFormatConverter> converter;
        Check(factory->CreateFormatConverter(converter.GetAddressOf()), "RGBA converter creation");
        Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom), "RGBA conversion");
        result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
        Check(converter->CopyPixels(nullptr, result.width * 4, static_cast<UINT>(result.rgba.size()),
            reinterpret_cast<BYTE*>(result.rgba.data())), "Image decode");
        return result;
    }
}
