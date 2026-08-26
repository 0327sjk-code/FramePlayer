#include "Render/FrameTexture.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace zt::sequence {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] bool ValidateFrame(const DecodedFrame& frame) noexcept {
    if (frame.width == 0 || frame.height == 0) {
        return false;
    }

    constexpr std::uint64_t bytesPerPixel = 4;
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(frame.width) * bytesPerPixel;
    if (rowBytes > std::numeric_limits<std::uint32_t>::max() || frame.strideBytes < rowBytes) {
        return false;
    }

    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(frame.strideBytes) * static_cast<std::uint64_t>(frame.height);
    return requiredBytes <= frame.bgraPixels.size();
}

void CopyFrameRows(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    const DecodedFrame& frame,
    D3D11_MAPPED_SUBRESOURCE& mapped) {
    const std::size_t sourceStride = frame.strideBytes;
    const std::size_t destinationStride = mapped.RowPitch;
    const std::size_t copyBytes = static_cast<std::size_t>(frame.width) * 4U;
    const auto* source = frame.bgraPixels.data();
    auto* destination = static_cast<std::uint8_t*>(mapped.pData);

    for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(destination, source, copyBytes);
        source += sourceStride;
        destination += destinationStride;
    }

    context->Unmap(texture, 0);
}

}  // namespace

class FrameTexture::Impl final {
public:
    [[nodiscard]] bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (device == nullptr || context == nullptr) {
            return false;
        }

        Reset();
        device_ = device;
        context_ = context;
        return true;
    }

    [[nodiscard]] bool Upload(
        const DecodedFrame& frame,
        const FrameTextureUploadKey uploadKey) {
        if (device_ == nullptr || context_ == nullptr || !ValidateFrame(frame)) {
            return false;
        }
        if (shaderResourceView_ != nullptr && uploadedKey_.has_value() &&
            *uploadedKey_ == uploadKey) {
            return true;
        }

        if (texture_ == nullptr || width_ != frame.width || height_ != frame.height) {
            if (!CreateAndFillTexture(frame)) {
                return false;
            }
        } else {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT result = context_->Map(
                texture_.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
            if (FAILED(result)) {
                return false;
            }
            CopyFrameRows(context_.Get(), texture_.Get(), frame, mapped);
        }

        uploadedKey_ = uploadKey;
        return true;
    }

    void ClearFrame() {
        shaderResourceView_.Reset();
        texture_.Reset();
        width_ = 0;
        height_ = 0;
        uploadedKey_.reset();
    }

    void Reset() {
        ClearFrame();
        context_.Reset();
        device_.Reset();
    }

    [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept {
        return shaderResourceView_.Get();
    }

    [[nodiscard]] std::uint32_t Width() const noexcept {
        return width_;
    }

    [[nodiscard]] std::uint32_t Height() const noexcept {
        return height_;
    }

    [[nodiscard]] std::uint64_t UploadedRevision() const noexcept {
        return uploadedKey_.has_value() ? uploadedKey_->revision : 0U;
    }

    [[nodiscard]] bool MatchesUploadKey(
        const FrameTextureUploadKey uploadKey) const noexcept {
        return shaderResourceView_ != nullptr && uploadedKey_.has_value() &&
            *uploadedKey_ == uploadKey;
    }

private:
    [[nodiscard]] bool CreateAndFillTexture(const DecodedFrame& frame) {
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = frame.width;
        textureDescription.Height = frame.height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DYNAMIC;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textureDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ComPtr<ID3D11Texture2D> newTexture;
        HRESULT result = device_->CreateTexture2D(
            &textureDescription,
            nullptr,
            newTexture.GetAddressOf());
        if (FAILED(result)) {
            return false;
        }

        ComPtr<ID3D11ShaderResourceView> newShaderResourceView;
        result = device_->CreateShaderResourceView(
            newTexture.Get(),
            nullptr,
            newShaderResourceView.GetAddressOf());
        if (FAILED(result)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        result = context_->Map(
            newTexture.Get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(result)) {
            return false;
        }
        CopyFrameRows(context_.Get(), newTexture.Get(), frame, mapped);

        texture_ = std::move(newTexture);
        shaderResourceView_ = std::move(newShaderResourceView);
        width_ = frame.width;
        height_ = frame.height;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> shaderResourceView_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::optional<FrameTextureUploadKey> uploadedKey_;
};

FrameTexture::~FrameTexture() {
    Reset();
    delete std::exchange(impl_, nullptr);
}

bool FrameTexture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    return impl_->Initialize(device, context);
}

bool FrameTexture::Upload(const DecodedFrame& frame, const std::uint64_t displayRevision) {
    return Upload(
        frame,
        MakeFrameTextureUploadKey(
            frame,
            displayRevision,
            FrameTextureUploadDomain::PlayerEngine));
}

bool FrameTexture::Upload(
    const DecodedFrame& frame,
    const FrameTextureUploadKey uploadKey) {
    return impl_ != nullptr && impl_->Upload(frame, uploadKey);
}

void FrameTexture::ClearFrame() {
    if (impl_ != nullptr) {
        impl_->ClearFrame();
    }
}

void FrameTexture::Reset() {
    if (impl_ != nullptr) {
        impl_->Reset();
    }
}

ID3D11ShaderResourceView* FrameTexture::ShaderResourceView() const noexcept {
    return impl_ != nullptr ? impl_->ShaderResourceView() : nullptr;
}

std::uint32_t FrameTexture::Width() const noexcept {
    return impl_ != nullptr ? impl_->Width() : 0;
}

std::uint32_t FrameTexture::Height() const noexcept {
    return impl_ != nullptr ? impl_->Height() : 0;
}

std::uint64_t FrameTexture::UploadedRevision() const noexcept {
    return impl_ != nullptr ? impl_->UploadedRevision() : 0;
}

bool FrameTexture::MatchesUploadKey(
    const FrameTextureUploadKey uploadKey) const noexcept {
    return impl_ != nullptr && impl_->MatchesUploadKey(uploadKey);
}

}  // namespace zt::sequence
