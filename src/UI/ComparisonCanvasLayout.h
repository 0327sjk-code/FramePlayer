#pragma once

#include "UI/MaskPreset.h"

#include <cstdint>

namespace zt::sequence::ui_detail {

inline constexpr float kComparisonCanvasWidthPixels =
    static_cast<float>(ui::kMaskReferenceWidthPixels);
inline constexpr float kComparisonCanvasHeightPixels =
    static_cast<float>(ui::kMaskReferenceHeightPixels);

struct ComparisonCanvasRect final {
    float minimumX = 0.0F;
    float minimumY = 0.0F;
    float maximumX = 0.0F;
    float maximumY = 0.0F;
};

struct ComparisonCanvasLayout final {
    ComparisonCanvasRect sourceInCanvas;
    ComparisonCanvasRect openingInCanvas;
    ComparisonCanvasRect visibleInCanvas;
    ui::NormalizedMaskOpening openingInCanvasNormalized;
    ui::NormalizedMaskOpening sourceUv;
    ui::NormalizedMaskOpening targetInCanvas;
    bool hasVisibleContent = false;
};

[[nodiscard]] inline constexpr float ComparisonRectWidth(
    const ComparisonCanvasRect rectangle) noexcept {
    return rectangle.maximumX - rectangle.minimumX;
}

[[nodiscard]] inline constexpr float ComparisonRectHeight(
    const ComparisonCanvasRect rectangle) noexcept {
    return rectangle.maximumY - rectangle.minimumY;
}

[[nodiscard]] inline constexpr bool HasComparisonRectArea(
    const ComparisonCanvasRect rectangle) noexcept {
    return ComparisonRectWidth(rectangle) > 0.0F &&
        ComparisonRectHeight(rectangle) > 0.0F;
}

[[nodiscard]] inline constexpr float ClampComparisonUnit(
    const float value) noexcept {
    return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
}

[[nodiscard]] inline constexpr ComparisonCanvasRect IntersectComparisonRects(
    const ComparisonCanvasRect first,
    const ComparisonCanvasRect second) noexcept {
    const float minimumX = first.minimumX > second.minimumX
        ? first.minimumX
        : second.minimumX;
    const float minimumY = first.minimumY > second.minimumY
        ? first.minimumY
        : second.minimumY;
    const float maximumX = first.maximumX < second.maximumX
        ? first.maximumX
        : second.maximumX;
    const float maximumY = first.maximumY < second.maximumY
        ? first.maximumY
        : second.maximumY;
    return maximumX > minimumX && maximumY > minimumY
        ? ComparisonCanvasRect{minimumX, minimumY, maximumX, maximumY}
        : ComparisonCanvasRect{};
}

[[nodiscard]] inline constexpr ui::NormalizedMaskOpening
NormalizeComparisonRectWithin(
    const ComparisonCanvasRect rectangle,
    const ComparisonCanvasRect bounds) noexcept {
    const float boundsWidth = ComparisonRectWidth(bounds);
    const float boundsHeight = ComparisonRectHeight(bounds);
    if (boundsWidth <= 0.0F || boundsHeight <= 0.0F) {
        return {};
    }

    return {
        ClampComparisonUnit((rectangle.minimumX - bounds.minimumX) /
            boundsWidth),
        ClampComparisonUnit((rectangle.minimumY - bounds.minimumY) /
            boundsHeight),
        ClampComparisonUnit((rectangle.maximumX - bounds.minimumX) /
            boundsWidth),
        ClampComparisonUnit((rectangle.maximumY - bounds.minimumY) /
            boundsHeight),
    };
}

[[nodiscard]] inline constexpr ComparisonCanvasRect
ComparisonOpeningInCanvas(
    const ui::NormalizedMaskOpening requestedOpening) noexcept {
    const ui::NormalizedMaskOpening opening =
        ui::IsValidNormalizedMaskOpening(requestedOpening)
        ? requestedOpening
        : ui::FullNormalizedMaskOpening();
    return {
        opening.minimumX * kComparisonCanvasWidthPixels,
        opening.minimumY * kComparisonCanvasHeightPixels,
        opening.maximumX * kComparisonCanvasWidthPixels,
        opening.maximumY * kComparisonCanvasHeightPixels,
    };
}

[[nodiscard]] inline constexpr ComparisonCanvasRect
FullComparisonCanvasRect() noexcept {
    return {
        0.0F,
        0.0F,
        kComparisonCanvasWidthPixels,
        kComparisonCanvasHeightPixels,
    };
}

// Comparison viewports always use a 1920 x 1920 virtual canvas. The original
// source is centered without upscaling; oversized sources are fitted uniformly.
// The returned intersection and UV let the display-layout step target only
// visible content, so a pre-cropped source is never cropped a second time
// merely because its own dimensions differ from the virtual canvas dimensions.
[[nodiscard]] inline constexpr ComparisonCanvasLayout
CalculateComparisonCanvasLayout(
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const ui::NormalizedMaskOpening requestedOpening) noexcept {
    ComparisonCanvasLayout result{};
    result.openingInCanvas = ComparisonOpeningInCanvas(requestedOpening);
    result.openingInCanvasNormalized = NormalizeComparisonRectWithin(
        result.openingInCanvas,
        FullComparisonCanvasRect());
    if (sourceWidth == 0U || sourceHeight == 0U ||
        !HasComparisonRectArea(result.openingInCanvas)) {
        return result;
    }

    const float width = static_cast<float>(sourceWidth);
    const float height = static_cast<float>(sourceHeight);
    const float widthScale = kComparisonCanvasWidthPixels / width;
    const float heightScale = kComparisonCanvasHeightPixels / height;
    float scale = 1.0F;
    if (widthScale < scale) {
        scale = widthScale;
    }
    if (heightScale < scale) {
        scale = heightScale;
    }

    const float placedWidth = width * scale;
    const float placedHeight = height * scale;
    const float sourceMinimumX =
        (kComparisonCanvasWidthPixels - placedWidth) * 0.5F;
    const float sourceMinimumY =
        (kComparisonCanvasHeightPixels - placedHeight) * 0.5F;
    result.sourceInCanvas = {
        sourceMinimumX,
        sourceMinimumY,
        sourceMinimumX + placedWidth,
        sourceMinimumY + placedHeight,
    };
    result.visibleInCanvas = IntersectComparisonRects(
        result.sourceInCanvas,
        result.openingInCanvas);
    if (!HasComparisonRectArea(result.visibleInCanvas)) {
        return result;
    }

    result.sourceUv = NormalizeComparisonRectWithin(
        result.visibleInCanvas,
        result.sourceInCanvas);
    result.targetInCanvas = NormalizeComparisonRectWithin(
        result.visibleInCanvas,
        FullComparisonCanvasRect());
    result.hasVisibleContent = true;
    return result;
}

// Fits the complete 1920 x 1920 virtual canvas into one comparison pane. Every
// mask preset therefore uses exactly the same scale as no-mask playback. The
// whole canvas is translated only far enough for the final visible source edge
// to reach the shared divider; masks never enlarge their opening.
[[nodiscard]] inline constexpr ui::MaskDisplayRect
FitComparisonCanvasToDisplay(
    const ComparisonCanvasLayout& layout,
    const float availableWidth,
    const float availableHeight,
    const bool alignToMaximumX) noexcept {
    if (!layout.hasVisibleContent ||
        availableWidth <= 0.0F ||
        availableHeight <= 0.0F) {
        return {};
    }

    const float widthScale =
        availableWidth / kComparisonCanvasWidthPixels;
    const float heightScale =
        availableHeight / kComparisonCanvasHeightPixels;
    const float scale = widthScale < heightScale
        ? widthScale
        : heightScale;
    const float fittedWidth = kComparisonCanvasWidthPixels * scale;
    const float fittedHeight = kComparisonCanvasHeightPixels * scale;
    const float dividerCoordinateInCanvas = alignToMaximumX
        ? layout.targetInCanvas.maximumX
        : layout.targetInCanvas.minimumX;
    const float minimumX = alignToMaximumX
        ? availableWidth - (dividerCoordinateInCanvas * fittedWidth)
        : -(dividerCoordinateInCanvas * fittedWidth);
    const float minimumY = (availableHeight - fittedHeight) * 0.5F;
    return {
        minimumX,
        minimumY,
        minimumX + fittedWidth,
        minimumY + fittedHeight,
    };
}

[[nodiscard]] inline constexpr ui::MaskDisplayRect
MapComparisonOpeningToDisplay(
    const ComparisonCanvasLayout& layout,
    const ui::MaskDisplayRect canvasDisplay) noexcept {
    return ui::MapMaskOpeningToDisplay(
        layout.openingInCanvasNormalized,
        canvasDisplay);
}

[[nodiscard]] inline constexpr ui::MaskDisplayRect
MapComparisonContentToDisplay(
    const ComparisonCanvasLayout& layout,
    const ui::MaskDisplayRect canvasDisplay) noexcept {
    return layout.hasVisibleContent
        ? ui::MapMaskOpeningToDisplay(
            layout.targetInCanvas,
            canvasDisplay)
        : ui::MaskDisplayRect{};
}

}  // namespace zt::sequence::ui_detail
