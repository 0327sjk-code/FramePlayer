#pragma once

#include <algorithm>
#include <cmath>

namespace zt::sequence::ui_detail {

inline constexpr float kMinimumViewportZoom = 0.25F;
inline constexpr float kMaximumViewportZoom = 16.0F;
inline constexpr float kViewportZoomWheelStep = 1.2F;

struct ViewportPoint final {
    float x = 0.0F;
    float y = 0.0F;
};

struct ViewportSize final {
    float width = 0.0F;
    float height = 0.0F;
};

struct ViewportTransform final {
    float zoom = 1.0F;
    float centerU = 0.5F;
    float centerV = 0.5F;
};

struct ViewportImageRect final {
    float minimumX = 0.0F;
    float minimumY = 0.0F;
    float maximumX = 0.0F;
    float maximumY = 0.0F;
};

[[nodiscard]] inline constexpr float ClampViewportZoom(
    const float zoom) noexcept {
    if (zoom != zoom) {
        return 1.0F;
    }
    return zoom < kMinimumViewportZoom
        ? kMinimumViewportZoom
        : (zoom > kMaximumViewportZoom ? kMaximumViewportZoom : zoom);
}

[[nodiscard]] inline constexpr float AbsoluteFloat(
    const float value) noexcept {
    return value < 0.0F ? -value : value;
}

[[nodiscard]] inline constexpr bool IsValidViewportSize(
    const ViewportSize size) noexcept {
    return size.width > 0.0F && size.height > 0.0F;
}

[[nodiscard]] inline constexpr float ClampViewportCenterAxis(
    const float center,
    const float scaledImageExtent,
    const float viewportExtent) noexcept {
    if (center != center ||
        scaledImageExtent != scaledImageExtent ||
        viewportExtent != viewportExtent ||
        scaledImageExtent <= 0.0F || viewportExtent <= 0.0F) {
        return 0.5F;
    }
    const float firstBoundary = viewportExtent / (2.0F * scaledImageExtent);
    const float secondBoundary = 1.0F - firstBoundary;
    const float minimumCenter = std::min(firstBoundary, secondBoundary);
    const float maximumCenter = std::max(firstBoundary, secondBoundary);
    return std::clamp(center, minimumCenter, maximumCenter);
}

[[nodiscard]] inline constexpr ViewportTransform ClampViewportCenter(
    ViewportTransform transform,
    const ViewportSize fittedImage,
    const ViewportSize viewport) noexcept {
    transform.zoom = ClampViewportZoom(transform.zoom);
    if (!IsValidViewportSize(fittedImage) ||
        !IsValidViewportSize(viewport)) {
        transform.centerU = 0.5F;
        transform.centerV = 0.5F;
        return transform;
    }

    const float scaledWidth = fittedImage.width * transform.zoom;
    const float scaledHeight = fittedImage.height * transform.zoom;
    transform.centerU = ClampViewportCenterAxis(
        transform.centerU,
        scaledWidth,
        viewport.width);
    transform.centerV = ClampViewportCenterAxis(
        transform.centerV,
        scaledHeight,
        viewport.height);
    return transform;
}

[[nodiscard]] inline constexpr ViewportTransform PanViewport(
    ViewportTransform transform,
    const ViewportPoint delta,
    const ViewportSize fittedImage,
    const ViewportSize viewport) noexcept {
    transform = ClampViewportCenter(transform, fittedImage, viewport);
    const float scaledWidth = fittedImage.width * transform.zoom;
    const float scaledHeight = fittedImage.height * transform.zoom;
    if (scaledWidth > 0.0F) {
        transform.centerU -= delta.x / scaledWidth;
    }
    if (scaledHeight > 0.0F) {
        transform.centerV -= delta.y / scaledHeight;
    }
    return ClampViewportCenter(transform, fittedImage, viewport);
}

[[nodiscard]] inline constexpr ViewportTransform ZoomViewportAtPoint(
    ViewportTransform transform,
    const float requestedZoom,
    const ViewportPoint anchor,
    const ViewportSize fittedImage,
    const ViewportSize viewport) noexcept {
    transform = ClampViewportCenter(transform, fittedImage, viewport);
    const float previousZoom = transform.zoom;
    const float nextZoom = ClampViewportZoom(requestedZoom);
    if (!IsValidViewportSize(viewport) || previousZoom == nextZoom) {
        transform.zoom = nextZoom;
        return ClampViewportCenter(transform, fittedImage, viewport);
    }

    const float previousWidth = fittedImage.width * previousZoom;
    const float previousHeight = fittedImage.height * previousZoom;
    const float nextWidth = fittedImage.width * nextZoom;
    const float nextHeight = fittedImage.height * nextZoom;
    const float viewportCenterX = viewport.width * 0.5F;
    const float viewportCenterY = viewport.height * 0.5F;
    if (previousWidth > 0.0F && nextWidth > 0.0F) {
        const float anchorU = transform.centerU +
            ((anchor.x - viewportCenterX) / previousWidth);
        transform.centerU = anchorU -
            ((anchor.x - viewportCenterX) / nextWidth);
    }
    if (previousHeight > 0.0F && nextHeight > 0.0F) {
        const float anchorV = transform.centerV +
            ((anchor.y - viewportCenterY) / previousHeight);
        transform.centerV = anchorV -
            ((anchor.y - viewportCenterY) / nextHeight);
    }
    transform.zoom = nextZoom;
    return ClampViewportCenter(transform, fittedImage, viewport);
}

[[nodiscard]] inline ViewportTransform ZoomViewportByWheel(
    const ViewportTransform transform,
    const float wheelDelta,
    const ViewportPoint anchor,
    const ViewportSize fittedImage,
    const ViewportSize viewport) noexcept {
    if (!std::isfinite(wheelDelta) || wheelDelta == 0.0F) {
        return ClampViewportCenter(transform, fittedImage, viewport);
    }
    const float requestedZoom = transform.zoom * static_cast<float>(
        std::pow(
            static_cast<double>(kViewportZoomWheelStep),
            static_cast<double>(wheelDelta)));
    return ZoomViewportAtPoint(
        transform,
        requestedZoom,
        anchor,
        fittedImage,
        viewport);
}

[[nodiscard]] inline constexpr ViewportImageRect CalculateViewportImageRect(
    const ViewportTransform transform,
    const ViewportPoint viewportMinimum,
    const ViewportSize fittedImage,
    const ViewportSize viewport) noexcept {
    const ViewportTransform clamped =
        ClampViewportCenter(transform, fittedImage, viewport);
    const float width = fittedImage.width * clamped.zoom;
    const float height = fittedImage.height * clamped.zoom;
    const float viewportCenterX =
        viewportMinimum.x + (viewport.width * 0.5F);
    const float viewportCenterY =
        viewportMinimum.y + (viewport.height * 0.5F);
    const float minimumX = viewportCenterX - (clamped.centerU * width);
    const float minimumY = viewportCenterY - (clamped.centerV * height);
    return {
        minimumX,
        minimumY,
        minimumX + width,
        minimumY + height};
}

[[nodiscard]] inline constexpr bool IsDefaultViewportTransform(
    const ViewportTransform transform,
    const float tolerance = 0.0001F) noexcept {
    return AbsoluteFloat(transform.zoom - 1.0F) <= tolerance &&
        AbsoluteFloat(transform.centerU - 0.5F) <= tolerance &&
        AbsoluteFloat(transform.centerV - 0.5F) <= tolerance;
}

}  // namespace zt::sequence::ui_detail
