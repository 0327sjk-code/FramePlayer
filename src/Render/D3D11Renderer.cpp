#include "Render/D3D11Renderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <utility>

namespace zt::sequence {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::array<D3D_FEATURE_LEVEL, 4> kFeatureLevels{
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

[[nodiscard]] HRESULT CreateDevice(
    const D3D_DRIVER_TYPE driverType,
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_10_0;

    HRESULT result = D3D11CreateDevice(
        nullptr,
        driverType,
        nullptr,
        flags,
        kFeatureLevels.data(),
        static_cast<UINT>(kFeatureLevels.size()),
        D3D11_SDK_VERSION,
        device.ReleaseAndGetAddressOf(),
        &selectedFeatureLevel,
        context.ReleaseAndGetAddressOf());

    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            flags,
            kFeatureLevels.data() + 1,
            static_cast<UINT>(kFeatureLevels.size() - 1),
            D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(),
            &selectedFeatureLevel,
            context.ReleaseAndGetAddressOf());
    }

    return result;
}

[[nodiscard]] RenderResult ResultFromHResult(
    const HRESULT result,
    const RenderStatus fallbackStatus) noexcept {
    if (result == DXGI_ERROR_DEVICE_REMOVED) {
        return {RenderStatus::DeviceRemoved, static_cast<std::int32_t>(result)};
    }
    if (result == DXGI_ERROR_DEVICE_RESET) {
        return {RenderStatus::DeviceReset, static_cast<std::int32_t>(result)};
    }
    return {fallbackStatus, static_cast<std::int32_t>(result)};
}

}  // namespace

class D3D11Renderer::Impl final {
public:
    [[nodiscard]] bool Initialize(HWND windowHandle) {
        if (windowHandle == nullptr) {
            return false;
        }

        HRESULT result = CreateDevice(D3D_DRIVER_TYPE_HARDWARE, device_, context_);
        if (FAILED(result)) {
            result = CreateDevice(D3D_DRIVER_TYPE_WARP, device_, context_);
        }
        if (FAILED(result)) {
            Shutdown();
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        result = device_.As(&dxgiDevice);
        if (SUCCEEDED(result)) {
            result = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
        }
        if (FAILED(result)) {
            Shutdown();
            return false;
        }

        RECT clientRect{};
        if (::GetClientRect(windowHandle, &clientRect) == FALSE) {
            Shutdown();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
        swapChainDescription.Width = static_cast<UINT>(clientRect.right - clientRect.left);
        swapChainDescription.Height = static_cast<UINT>(clientRect.bottom - clientRect.top);
        swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDescription.SampleDesc.Count = 1;
        swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDescription.BufferCount = 2;
        swapChainDescription.Scaling = DXGI_SCALING_STRETCH;
        swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        result = factory->CreateSwapChainForHwnd(
            device_.Get(),
            windowHandle,
            &swapChainDescription,
            nullptr,
            nullptr,
            swapChain_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            Shutdown();
            return false;
        }

        static_cast<void>(factory->MakeWindowAssociation(windowHandle, DXGI_MWA_NO_ALT_ENTER));
        if (FAILED(CreateRenderTarget())) {
            Shutdown();
            return false;
        }

        windowHandle_ = windowHandle;
        return true;
    }

    [[nodiscard]] RenderResult Resize(
        const std::uint32_t width,
        const std::uint32_t height) {
        if (width == 0 || height == 0) {
            return {};
        }
        if (swapChain_ == nullptr || context_ == nullptr) {
            return {RenderStatus::ResizeFailed, static_cast<std::int32_t>(E_POINTER)};
        }

        context_->OMSetRenderTargets(0, nullptr, nullptr);
        renderTargetView_.Reset();

        const HRESULT result = swapChain_->ResizeBuffers(
            0,
            width,
            height,
            DXGI_FORMAT_UNKNOWN,
            0);
        if (FAILED(result)) {
            // ResizeBuffers can fail transiently while the old swap-chain
            // buffers remain valid. Recreate their RTV so a diagnostic can
            // still be rendered on the next frame when possible.
            const HRESULT restoreResult = CreateRenderTarget();
            if (FAILED(restoreResult)) {
                return ResultFromHResult(
                    restoreResult,
                    RenderStatus::RenderTargetFailed);
            }
            return ResultFromHResult(result, RenderStatus::ResizeFailed);
        }
        const HRESULT renderTargetResult = CreateRenderTarget();
        if (FAILED(renderTargetResult)) {
            return ResultFromHResult(
                renderTargetResult,
                RenderStatus::RenderTargetFailed);
        }
        return {};
    }

    [[nodiscard]] RenderResult BeginFrame() {
        if (context_ == nullptr || renderTargetView_ == nullptr) {
            return {
                RenderStatus::RenderTargetFailed,
                static_cast<std::int32_t>(E_POINTER)};
        }

        constexpr float clearColor[4]{0.0168F, 0.0168F, 0.0168F, 1.0F};
        ID3D11RenderTargetView* renderTargets[]{renderTargetView_.Get()};
        context_->OMSetRenderTargets(1, renderTargets, nullptr);
        context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);
        return {};
    }

    [[nodiscard]] RenderResult EndFrame(const bool enableVsync) {
        if (swapChain_ == nullptr) {
            return {
                RenderStatus::PresentFailed,
                static_cast<std::int32_t>(E_POINTER)};
        }

        const HRESULT result = swapChain_->Present(enableVsync ? 1U : 0U, 0);
        occluded_ = result == DXGI_STATUS_OCCLUDED;
        if (occluded_) {
            return {RenderStatus::Occluded, static_cast<std::int32_t>(result)};
        }
        if (FAILED(result)) {
            return ResultFromHResult(result, RenderStatus::PresentFailed);
        }
        return {};
    }

    [[nodiscard]] bool TestOcclusion() {
        if (!occluded_ || swapChain_ == nullptr) {
            return !occluded_;
        }

        const HRESULT result = swapChain_->Present(0, DXGI_PRESENT_TEST);
        if (result != DXGI_STATUS_OCCLUDED) {
            occluded_ = false;
        }
        return !occluded_;
    }

    void Shutdown() {
        if (context_ != nullptr) {
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->ClearState();
            context_->Flush();
        }

        renderTargetView_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();
        windowHandle_ = nullptr;
        occluded_ = false;
    }

    [[nodiscard]] bool IsOccluded() const noexcept {
        return occluded_;
    }

    [[nodiscard]] ID3D11Device* Device() const noexcept {
        return device_.Get();
    }

    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept {
        return context_.Get();
    }

private:
    [[nodiscard]] HRESULT CreateRenderTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT result = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        result = device_->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            renderTargetView_.ReleaseAndGetAddressOf());
        return result;
    }

    HWND windowHandle_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTargetView_;
    bool occluded_ = false;
};

D3D11Renderer::D3D11Renderer() : impl_(new Impl()) {}

D3D11Renderer::~D3D11Renderer() {
    Shutdown();
    delete std::exchange(impl_, nullptr);
}

bool D3D11Renderer::Initialize(const HWND windowHandle) {
    return impl_ != nullptr && impl_->Initialize(windowHandle);
}

RenderResult D3D11Renderer::Resize(
    const std::uint32_t width,
    const std::uint32_t height) {
    return impl_ != nullptr
        ? impl_->Resize(width, height)
        : RenderResult{
            RenderStatus::ResizeFailed,
            static_cast<std::int32_t>(E_POINTER)};
}

RenderResult D3D11Renderer::BeginFrame() {
    return impl_ != nullptr
        ? impl_->BeginFrame()
        : RenderResult{
            RenderStatus::RenderTargetFailed,
            static_cast<std::int32_t>(E_POINTER)};
}

RenderResult D3D11Renderer::EndFrame(const bool enableVsync) {
    return impl_ != nullptr
        ? impl_->EndFrame(enableVsync)
        : RenderResult{
            RenderStatus::PresentFailed,
            static_cast<std::int32_t>(E_POINTER)};
}

void D3D11Renderer::Shutdown() {
    if (impl_ != nullptr) {
        impl_->Shutdown();
    }
}

bool D3D11Renderer::IsOccluded() const noexcept {
    return impl_ != nullptr && impl_->IsOccluded();
}

bool D3D11Renderer::TestOcclusion() {
    return impl_ == nullptr || impl_->TestOcclusion();
}

ID3D11Device* D3D11Renderer::Device() const noexcept {
    return impl_ != nullptr ? impl_->Device() : nullptr;
}

ID3D11DeviceContext* D3D11Renderer::Context() const noexcept {
    return impl_ != nullptr ? impl_->Context() : nullptr;
}

}  // namespace zt::sequence
