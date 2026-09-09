#pragma once

#include <cstdint>

namespace zt::sequence::overlay {

inline constexpr std::uint32_t kMaskOverlayWidthPixels = 1080U;
inline constexpr std::uint32_t kMaskOverlayHeightPixels = 1920U;
inline constexpr std::uint32_t kVideoCodecDimensionAlignmentPixels = 16U;

[[nodiscard]] inline constexpr std::uint32_t AlignMaskOverlayDimension(
    const std::uint32_t value) noexcept {
    return ((value + kVideoCodecDimensionAlignmentPixels - 1U) /
        kVideoCodecDimensionAlignmentPixels) *
        kVideoCodecDimensionAlignmentPixels;
}

[[nodiscard]] inline constexpr bool IsRequiredMaskOverlaySize(
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    return width == kMaskOverlayWidthPixels &&
        height == kMaskOverlayHeightPixels;
}

// Media Foundation can expose a codec-aligned coded width (1088) for a file
// whose visible FFmpeg frame is 1080 pixels wide.
[[nodiscard]] inline constexpr bool IsCodecAlignedMaskOverlaySize(
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    return width >= kMaskOverlayWidthPixels &&
        width <= AlignMaskOverlayDimension(kMaskOverlayWidthPixels) &&
        height >= kMaskOverlayHeightPixels &&
        height <= AlignMaskOverlayDimension(kMaskOverlayHeightPixels);
}

}  // namespace zt::sequence::overlay
