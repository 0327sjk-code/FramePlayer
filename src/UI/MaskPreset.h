#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zt::sequence::ui {

inline constexpr std::uint32_t kMaskReferenceWidthPixels = 1920U;
inline constexpr std::uint32_t kMaskReferenceHeightPixels = 1920U;

inline constexpr std::uint32_t kMask1080x1080WidthPixels = 1080U;
inline constexpr std::uint32_t kMask1080x1080HeightPixels = 1080U;
inline constexpr std::uint32_t kMask1920x1080WidthPixels = 1920U;
inline constexpr std::uint32_t kMask1920x1080HeightPixels = 1080U;
inline constexpr std::uint32_t kMask1080x1920WidthPixels = 1080U;
inline constexpr std::uint32_t kMask1080x1920HeightPixels = 1920U;
inline constexpr std::uint32_t kMask864x1080WidthPixels = 864U;
inline constexpr std::uint32_t kMask864x1080HeightPixels = 1080U;

inline constexpr float kNormalizedOpeningMinimum = 0.0F;
inline constexpr float kNormalizedOpeningMaximum = 1.0F;
inline constexpr float kCenteredOpeningMarginScale = 0.5F;

enum class MaskPreset : std::uint8_t {
    None = 0U,
    Opening1080x1080,
    Opening1920x1080,
    Opening1080x1920,
    Opening864x1080,
};

inline constexpr std::size_t kMaskPresetCount = 5U;
inline constexpr std::array<MaskPreset, kMaskPresetCount> kMaskPresets{
    MaskPreset::None,
    MaskPreset::Opening1080x1080,
    MaskPreset::Opening1920x1080,
    MaskPreset::Opening1080x1920,
    MaskPreset::Opening864x1080,
};

struct MaskPixelSize final {
    std::uint32_t width = kMaskReferenceWidthPixels;
    std::uint32_t height = kMaskReferenceHeightPixels;
};

struct NormalizedMaskOpening final {
    float minimumX = kNormalizedOpeningMinimum;
    float minimumY = kNormalizedOpeningMinimum;
    float maximumX = kNormalizedOpeningMaximum;
    float maximumY = kNormalizedOpeningMaximum;
};

struct MaskDisplayRect final {
    float minimumX = 0.0F;
    float minimumY = 0.0F;
    float maximumX = 0.0F;
    float maximumY = 0.0F;
};

[[nodiscard]] inline constexpr NormalizedMaskOpening FullNormalizedMaskOpening() noexcept {
    return {};
}

[[nodiscard]] inline constexpr bool HasMask(const MaskPreset preset) noexcept {
    switch (preset) {
    case MaskPreset::Opening1080x1080:
    case MaskPreset::Opening1920x1080:
    case MaskPreset::Opening1080x1920:
    case MaskPreset::Opening864x1080:
        return true;
    case MaskPreset::None:
    default:
        return false;
    }
}

[[nodiscard]] inline constexpr std::string_view MaskPresetLabel(
    const MaskPreset preset) noexcept {
    switch (preset) {
    case MaskPreset::Opening1080x1080:
        return "1080 × 1080";
    case MaskPreset::Opening1920x1080:
        return "1920 × 1080";
    case MaskPreset::Opening1080x1920:
        return "1080 × 1920";
    case MaskPreset::Opening864x1080:
        return "864 × 1080";
    case MaskPreset::None:
    default:
        return "无遮罩";
    }
}

[[nodiscard]] inline constexpr MaskPixelSize MaskOpeningPixelSize(
    const MaskPreset preset) noexcept {
    switch (preset) {
    case MaskPreset::Opening1080x1080:
        return {kMask1080x1080WidthPixels, kMask1080x1080HeightPixels};
    case MaskPreset::Opening1920x1080:
        return {kMask1920x1080WidthPixels, kMask1920x1080HeightPixels};
    case MaskPreset::Opening1080x1920:
        return {kMask1080x1920WidthPixels, kMask1080x1920HeightPixels};
    case MaskPreset::Opening864x1080:
        return {kMask864x1080WidthPixels, kMask864x1080HeightPixels};
    case MaskPreset::None:
    default:
        return {kMaskReferenceWidthPixels, kMaskReferenceHeightPixels};
    }
}

[[nodiscard]] inline constexpr bool IsValidMaskPixelSize(
    const MaskPixelSize size) noexcept {
    return size.width > 0U && size.height > 0U &&
        size.width <= kMaskReferenceWidthPixels &&
        size.height <= kMaskReferenceHeightPixels;
}

[[nodiscard]] inline constexpr NormalizedMaskOpening CenteredNormalizedMaskOpening(
    const MaskPixelSize size) noexcept {
    if (!IsValidMaskPixelSize(size)) {
        return FullNormalizedMaskOpening();
    }

    const float normalizedWidth = static_cast<float>(size.width) /
        static_cast<float>(kMaskReferenceWidthPixels);
    const float normalizedHeight = static_cast<float>(size.height) /
        static_cast<float>(kMaskReferenceHeightPixels);
    const float marginX = (kNormalizedOpeningMaximum - normalizedWidth) *
        kCenteredOpeningMarginScale;
    const float marginY = (kNormalizedOpeningMaximum - normalizedHeight) *
        kCenteredOpeningMarginScale;

    return {
        marginX,
        marginY,
        marginX + normalizedWidth,
        marginY + normalizedHeight,
    };
}

[[nodiscard]] inline constexpr NormalizedMaskOpening MaskOpeningForPreset(
    const MaskPreset preset) noexcept {
    if (!HasMask(preset)) {
        return FullNormalizedMaskOpening();
    }
    return CenteredNormalizedMaskOpening(MaskOpeningPixelSize(preset));
}

[[nodiscard]] inline constexpr bool IsValidNormalizedMaskOpening(
    const NormalizedMaskOpening opening) noexcept {
    return opening.minimumX >= kNormalizedOpeningMinimum &&
        opening.minimumY >= kNormalizedOpeningMinimum &&
        opening.maximumX <= kNormalizedOpeningMaximum &&
        opening.maximumY <= kNormalizedOpeningMaximum &&
        opening.minimumX <= opening.maximumX &&
        opening.minimumY <= opening.maximumY;
}

[[nodiscard]] inline constexpr float MapNormalizedCoordinate(
    const float minimum,
    const float maximum,
    const float normalizedCoordinate) noexcept {
    if (normalizedCoordinate <= kNormalizedOpeningMinimum) {
        return minimum;
    }
    if (normalizedCoordinate >= kNormalizedOpeningMaximum) {
        return maximum;
    }
    return minimum + ((maximum - minimum) * normalizedCoordinate);
}

[[nodiscard]] inline constexpr MaskDisplayRect MapMaskOpeningToDisplay(
    const NormalizedMaskOpening opening,
    const MaskDisplayRect displayRect) noexcept {
    if (!IsValidNormalizedMaskOpening(opening)) {
        return displayRect;
    }

    return {
        MapNormalizedCoordinate(displayRect.minimumX, displayRect.maximumX, opening.minimumX),
        MapNormalizedCoordinate(displayRect.minimumY, displayRect.maximumY, opening.minimumY),
        MapNormalizedCoordinate(displayRect.minimumX, displayRect.maximumX, opening.maximumX),
        MapNormalizedCoordinate(displayRect.minimumY, displayRect.maximumY, opening.maximumY),
    };
}

[[nodiscard]] inline constexpr MaskDisplayRect MaskOpeningForPresetInDisplay(
    const MaskPreset preset,
    const MaskDisplayRect displayRect) noexcept {
    return MapMaskOpeningToDisplay(MaskOpeningForPreset(preset), displayRect);
}

}  // namespace zt::sequence::ui
