#include "Imaging/WicImageDecoder.h"
#include "Overlay/MaskOverlayTexture.h"
#include "Render/FrameTexture.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using Microsoft::WRL::ComPtr;

void PrintHResult(const wchar_t* operation, const HRESULT result) {
    std::wcerr << operation << L" HRESULT=0x" << std::hex
               << static_cast<unsigned long>(result) << std::dec << L'\n';
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2 || argumentCount > 3) {
        std::wcerr
            << L"用法：ZTFrameUploadSmoke <PNG路径> "
               L"[25|50|75|100|overlay]\n";
        return 2;
    }

    const bool testMaskOverlay = argumentCount == 3 &&
        std::wstring_view(arguments[2]) == L"overlay";
    const std::uint32_t decodePercent = argumentCount == 3 &&
        !testMaskOverlay
        ? static_cast<std::uint32_t>(std::wcstoul(arguments[2], nullptr, 10))
        : 100U;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT deviceResult = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        device.GetAddressOf(),
        &featureLevel,
        context.GetAddressOf());
    if (FAILED(deviceResult)) {
        PrintHResult(L"D3D11CreateDevice", deviceResult);
        return 3;
    }

    if (testMaskOverlay) {
        zt::sequence::overlay::MaskOverlayTexture overlayTexture;
        if (!overlayTexture.Initialize(device.Get(), context.Get())) {
            std::wcerr << L"MaskOverlayTexture::Initialize failed\n";
            return 4;
        }
        const zt::sequence::overlay::MaskOverlayLoadResult loaded =
            overlayTexture.Load(std::filesystem::path(arguments[1]));
        if (!loaded || !overlayTexture.IsLoaded() ||
            overlayTexture.ShaderResourceView() == nullptr) {
            std::cerr << (loaded.errorUtf8.empty()
                ? "MaskOverlayTexture::Load failed"
                : loaded.errorUtf8) << '\n';
            return 5;
        }
        std::wcout << L"MaskOverlayTexture upload OK: "
                   << overlayTexture.LoadedPath().c_str() << L'\n';
        return 0;
    }

    zt::sequence::WicImageDecoder decoder;
    const zt::sequence::ImageDecodeResult decoded = decoder.Decode(
        arguments[1],
        0,
        1,
        decodePercent);
    if (!decoded) {
        std::cerr << decoded.errorUtf8 << '\n';
        return 4;
    }

    const auto& frame = *decoded.frame;
    std::wcout << L"frame=" << frame.width << L'x' << frame.height
               << L" stride=" << frame.strideBytes
               << L" bytes=" << frame.ByteSize() << L'\n';

    D3D11_TEXTURE2D_DESC description{};
    description.Width = frame.width;
    description.Height = frame.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DYNAMIC;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT textureResult = device->CreateTexture2D(
        &description,
        nullptr,
        texture.GetAddressOf());
    if (FAILED(textureResult)) {
        PrintHResult(L"CreateTexture2D", textureResult);
        return 5;
    }

    ComPtr<ID3D11ShaderResourceView> view;
    const HRESULT viewResult = device->CreateShaderResourceView(
        texture.Get(),
        nullptr,
        view.GetAddressOf());
    if (FAILED(viewResult)) {
        PrintHResult(L"CreateShaderResourceView", viewResult);
        return 6;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = context->Map(
        texture.Get(),
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(mapResult)) {
        PrintHResult(L"Map", mapResult);
        return 7;
    }
    context->Unmap(texture.Get(), 0);

    zt::sequence::FrameTexture frameTexture;
    if (!frameTexture.Initialize(device.Get(), context.Get())) {
        std::wcerr << L"FrameTexture::Initialize failed\n";
        return 8;
    }
    const zt::sequence::FrameTextureUploadKey playerKey{
        zt::sequence::FrameTextureUploadDomain::PlayerEngine,
        frame.generation,
        frame.index,
        1U};
    if (!frameTexture.Upload(frame, playerKey)) {
        std::wcerr << L"FrameTexture::Upload failed\n";
        return 9;
    }
    const zt::sequence::FrameTextureUploadKey comparisonKey{
        zt::sequence::FrameTextureUploadDomain::ComparisonPair,
        frame.generation,
        frame.index,
        1U};
    if (!frameTexture.Upload(frame, comparisonKey) ||
        !frameTexture.MatchesUploadKey(comparisonKey) ||
        frameTexture.MatchesUploadKey(playerKey)) {
        std::wcerr << L"FrameTexture upload key domain isolation failed\n";
        return 10;
    }

    std::wcout << L"FrameTexture upload OK\n";
    return 0;
}
