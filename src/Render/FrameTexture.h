#pragma once

#include "Core/PlayerTypes.h"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace zt::sequence {

enum class FrameTextureUploadDomain : std::uint8_t {
    PlayerEngine = 0,
    ComparisonPair,
    MaskOverlay,
};

struct FrameTextureUploadKey final {
    FrameTextureUploadDomain domain = FrameTextureUploadDomain::PlayerEngine;
    Generation generation = 0U;
    FrameIndex frame = 0U;
    std::uint64_t revision = 0U;

    [[nodiscard]] bool operator==(
        const FrameTextureUploadKey&) const noexcept = default;
};

[[nodiscard]] inline constexpr FrameTextureUploadKey MakeFrameTextureUploadKey(
    const DecodedFrame& frame,
    const std::uint64_t revision,
    const FrameTextureUploadDomain domain) noexcept {
    return FrameTextureUploadKey{
        domain,
        frame.generation,
        frame.index,
        revision};
}

class FrameTexture final {
public:
    FrameTexture() = default;
    ~FrameTexture();

    FrameTexture(const FrameTexture&) = delete;
    FrameTexture& operator=(const FrameTexture&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    [[nodiscard]] bool Upload(const DecodedFrame& frame, std::uint64_t displayRevision);
    [[nodiscard]] bool Upload(
        const DecodedFrame& frame,
        FrameTextureUploadKey uploadKey);
    void ClearFrame();
    void Reset();

    [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept;
    [[nodiscard]] std::uint32_t Width() const noexcept;
    [[nodiscard]] std::uint32_t Height() const noexcept;
    [[nodiscard]] std::uint64_t UploadedRevision() const noexcept;
    [[nodiscard]] bool MatchesUploadKey(
        FrameTextureUploadKey uploadKey) const noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace zt::sequence
