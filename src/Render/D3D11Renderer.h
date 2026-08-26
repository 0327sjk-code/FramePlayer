#pragma once

#include <cstdint>

struct HWND__;
using HWND = HWND__*;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace zt::sequence {

enum class RenderStatus : std::uint8_t {
    Success,
    Occluded,
    ResizeFailed,
    RenderTargetFailed,
    DeviceRemoved,
    DeviceReset,
    PresentFailed,
};

struct RenderResult final {
    RenderStatus status = RenderStatus::Success;
    std::int32_t nativeCode = 0;

    [[nodiscard]] constexpr bool Succeeded() const noexcept {
        return status == RenderStatus::Success;
    }
};

class D3D11Renderer final {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    [[nodiscard]] bool Initialize(HWND windowHandle);
    [[nodiscard]] RenderResult Resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] RenderResult BeginFrame();
    [[nodiscard]] RenderResult EndFrame(bool enableVsync = true);
    void Shutdown();

    [[nodiscard]] bool IsOccluded() const noexcept;
    [[nodiscard]] bool TestOcclusion();

    [[nodiscard]] ID3D11Device* Device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace zt::sequence
