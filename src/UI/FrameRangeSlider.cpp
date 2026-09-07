#include "UI/FrameRangeSlider.h"

#include "UI/PlayerUIInternal.h"
#include "UI/PlayerUILogic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zt::sequence::ui {
namespace {

[[nodiscard]] float FramePositionX(
    const FrameIndex frame,
    const std::size_t totalFrames,
    const float trackMinimumX,
    const float trackWidth) noexcept {
    if (totalFrames <= 1U) {
        return trackMinimumX;
    }
    const double normalized = static_cast<double>(frame) /
        static_cast<double>(totalFrames - 1U);
    return trackMinimumX +
        trackWidth * static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

[[nodiscard]] FrameRangeHandle NearestHandle(
    const float mouseX,
    const float startX,
    const float endX,
    const FrameRangeHandle lastHandle) noexcept {
    const float startDistance = std::abs(mouseX - startX);
    const float endDistance = std::abs(mouseX - endX);
    constexpr float kTieTolerancePixels = 0.25F;
    if (std::abs(startDistance - endDistance) <= kTieTolerancePixels) {
        if (startX == endX) {
            if (mouseX < startX - kTieTolerancePixels) {
                return FrameRangeHandle::Start;
            }
            if (mouseX > startX + kTieTolerancePixels) {
                return FrameRangeHandle::End;
            }
            if (lastHandle != FrameRangeHandle::None) {
                return lastHandle;
            }
        }
        return mouseX < ((startX + endX) * 0.5F)
            ? FrameRangeHandle::Start
            : FrameRangeHandle::End;
    }
    return startDistance < endDistance
        ? FrameRangeHandle::Start
        : FrameRangeHandle::End;
}

}  // namespace

bool FrameRangeSlider(
    InteractionAnimator& animator,
    const char* label,
    FrameIndex& startFrame,
    FrameIndex& endFrame,
    const std::size_t totalFrames,
    ImVec2 size,
    const float uiScale,
    FrameRangeSliderState& state) {
    if (label == nullptr || totalFrames == 0U || size.x <= 0.0F ||
        size.y <= 0.0F) {
        return false;
    }

    const float scale = std::max(uiScale, 0.5F);
    const FrameIndex lastFrame = static_cast<FrameIndex>(std::min<std::size_t>(
        totalFrames - 1U,
        static_cast<std::size_t>(std::numeric_limits<FrameIndex>::max())));
    startFrame = std::min(startFrame, lastFrame);
    endFrame = std::clamp(endFrame, startFrame, lastFrame);

    const ImGuiID id = ImGui::GetID(label);
    static_cast<void>(ImGui::InvisibleButton(
        label,
        size,
        ImGuiButtonFlags_MouseButtonLeft));
    const InteractionAnimation animation = animator.ObserveLastItem(id);

    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float centerY = (minimum.y + maximum.y) * 0.5F;
    const float endpointInset = 10.0F * scale;
    const float trackMinimumX = minimum.x + endpointInset;
    const float trackMaximumX = std::max(
        trackMinimumX + 1.0F,
        maximum.x - endpointInset);
    const float trackWidth = trackMaximumX - trackMinimumX;

    float startX = FramePositionX(
        startFrame,
        totalFrames,
        trackMinimumX,
        trackWidth);
    float endX = FramePositionX(
        endFrame,
        totalFrames,
        trackMinimumX,
        trackWidth);

    FrameRangeHandle hoverHandle = FrameRangeHandle::None;
    if (ImGui::IsItemHovered()) {
        const float mouseX = ImGui::GetIO().MousePos.x;
        hoverHandle = NearestHandle(
            mouseX,
            startX,
            endX,
            state.lastHandle);
        const float handleHitHalfWidth = 12.0F * scale;
        const bool nearStart = std::abs(mouseX - startX) <= handleHitHalfWidth;
        const bool nearEnd = std::abs(mouseX - endX) <= handleHitHalfWidth;
        ImGui::SetMouseCursor(
            nearStart || nearEnd
                ? ImGuiMouseCursor_ResizeEW
                : ImGuiMouseCursor_Hand);
    } else if (ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (ImGui::IsItemActivated()) {
        state.activeHandle = hoverHandle != FrameRangeHandle::None
            ? hoverHandle
            : NearestHandle(
                ImGui::GetIO().MousePos.x,
                startX,
                endX,
                state.lastHandle);
        state.lastHandle = state.activeHandle;
    }

    bool changed = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double normalized = trackWidth > 0.0F
            ? static_cast<double>(
                (ImGui::GetIO().MousePos.x - trackMinimumX) / trackWidth)
            : 0.0;
        const FrameIndex requested = ui_detail::FrameFromNormalizedPosition(
            normalized,
            totalFrames);
        if (state.activeHandle == FrameRangeHandle::Start) {
            const FrameIndex nextStart = ui_detail::ClampPlaybackStartHandle(
                requested,
                endFrame,
                totalFrames);
            changed = nextStart != startFrame;
            startFrame = nextStart;
        } else if (state.activeHandle == FrameRangeHandle::End) {
            const FrameIndex nextEnd = ui_detail::ClampPlaybackEndHandle(
                requested,
                startFrame,
                totalFrames);
            changed = nextEnd != endFrame;
            endFrame = nextEnd;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        state.activeHandle = FrameRangeHandle::None;
    }

    startX = FramePositionX(
        startFrame,
        totalFrames,
        trackMinimumX,
        trackWidth);
    endX = FramePositionX(
        endFrame,
        totalFrames,
        trackMinimumX,
        trackWidth);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float baseHalfHeight = (3.0F + animation.hover * 0.45F) * scale;
    drawList->AddRectFilled(
        ImVec2(trackMinimumX, centerY - baseHalfHeight),
        ImVec2(trackMaximumX, centerY + baseHalfHeight),
        ImGui::GetColorU32(ui_internal::kColorSurfaceActive),
        3.0F * scale);
    drawList->AddRectFilled(
        ImVec2(startX, centerY - 3.5F * scale),
        ImVec2(endX, centerY + 3.5F * scale),
        ImGui::GetColorU32(ui_internal::WithAlpha(
            ui_internal::kColorPrimary,
            0.72F)),
        3.5F * scale);

    const auto drawHandle = [
                                drawList,
                                centerY,
                                scale,
                                &state,
                                hoverHandle,
                                &animation](
                                const float x,
                                const FrameRangeHandle handle) {
        const bool active = state.activeHandle == handle;
        const bool highlighted = hoverHandle == handle;
        const float halfWidth =
            (active ? 6.0F : (highlighted ? 5.0F + animation.hover : 5.0F)) *
            scale;
        const float halfHeight = 11.0F * scale;
        const ImVec2 handleMinimum{x - halfWidth, centerY - halfHeight};
        const ImVec2 handleMaximum{x + halfWidth, centerY + halfHeight};
        drawList->AddRectFilled(
            handleMinimum,
            handleMaximum,
            ImGui::GetColorU32(
                active
                    ? ui_internal::kColorPrimary
                    : ui_internal::kColorSurface),
            5.0F * scale);
        drawList->AddRect(
            handleMinimum,
            handleMaximum,
            ImGui::GetColorU32(ui_internal::kColorPrimaryHover),
            5.0F * scale,
            0,
            (highlighted || active ? 2.5F : 2.0F) * scale);
    };
    const FrameRangeHandle foregroundHandle =
        state.activeHandle != FrameRangeHandle::None
            ? state.activeHandle
            : hoverHandle;
    if (foregroundHandle == FrameRangeHandle::Start) {
        drawHandle(endX, FrameRangeHandle::End);
        drawHandle(startX, FrameRangeHandle::Start);
    } else {
        drawHandle(startX, FrameRangeHandle::Start);
        drawHandle(endX, FrameRangeHandle::End);
    }
    if (startX == endX && foregroundHandle == FrameRangeHandle::None) {
        drawList->AddLine(
            ImVec2(startX, centerY - 8.0F * scale),
            ImVec2(startX, centerY + 8.0F * scale),
            ImGui::GetColorU32(ui_internal::kColorPrimary),
            1.0F * scale);
    }

    if (animation.focus > 0.001F) {
        drawList->AddRect(
            minimum,
            maximum,
            ImGui::GetColorU32(ui_internal::WithAlpha(
                ui_internal::kColorAccent,
                animation.focus)),
            6.0F * scale,
            0,
            1.5F * scale);
    }
    return changed;
}

bool FramePositionSlider(
    InteractionAnimator& animator,
    const char* const label,
    FrameIndex& frame,
    const FrameIndex maximumFrame,
    ImVec2 size,
    const float uiScale) {
    if (label == nullptr || size.x <= 0.0F || size.y <= 0.0F) {
        return false;
    }

    const float scale = std::max(uiScale, 0.5F);
    frame = std::min(frame, maximumFrame);

    const ImGuiID id = ImGui::GetID(label);
    static_cast<void>(ImGui::InvisibleButton(
        label,
        size,
        ImGuiButtonFlags_MouseButtonLeft));
    const InteractionAnimation animation = animator.ObserveLastItem(id);

    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float centerY = (minimum.y + maximum.y) * 0.5F;
    const float endpointInset = 10.0F * scale;
    const float trackMinimumX = minimum.x + endpointInset;
    const float trackMaximumX = std::max(
        trackMinimumX + 1.0F,
        maximum.x - endpointInset);
    const float trackWidth = trackMaximumX - trackMinimumX;

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    bool changed = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double normalized = trackWidth > 0.0F
            ? static_cast<double>(
                (ImGui::GetIO().MousePos.x - trackMinimumX) / trackWidth)
            : 0.0;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        const FrameIndex requested = static_cast<FrameIndex>(std::llround(
            clamped * static_cast<double>(maximumFrame)));
        changed = requested != frame;
        frame = requested;
    }

    const float frameX = maximumFrame > 0U
        ? trackMinimumX + trackWidth * static_cast<float>(
            static_cast<double>(frame) / static_cast<double>(maximumFrame))
        : trackMinimumX;

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    const float baseHalfHeight = (3.0F + animation.hover * 0.45F) * scale;
    drawList->AddRectFilled(
        ImVec2(trackMinimumX, centerY - baseHalfHeight),
        ImVec2(trackMaximumX, centerY + baseHalfHeight),
        ImGui::GetColorU32(ui_internal::kColorSurfaceActive),
        3.0F * scale);
    if (frameX > trackMinimumX) {
        drawList->AddRectFilled(
            ImVec2(trackMinimumX, centerY - 3.5F * scale),
            ImVec2(frameX, centerY + 3.5F * scale),
            ImGui::GetColorU32(ui_internal::WithAlpha(
                ui_internal::kColorPrimary,
                0.72F)),
            3.5F * scale);
    }

    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const float halfWidth =
        (active ? 6.0F : (hovered ? 5.0F + animation.hover : 5.0F)) * scale;
    const float halfHeight = 11.0F * scale;
    const ImVec2 handleMinimum{frameX - halfWidth, centerY - halfHeight};
    const ImVec2 handleMaximum{frameX + halfWidth, centerY + halfHeight};
    drawList->AddRectFilled(
        handleMinimum,
        handleMaximum,
        ImGui::GetColorU32(
            active
                ? ui_internal::kColorPrimary
                : ui_internal::kColorSurface),
        5.0F * scale);
    drawList->AddRect(
        handleMinimum,
        handleMaximum,
        ImGui::GetColorU32(ui_internal::kColorPrimaryHover),
        5.0F * scale,
        0,
        (hovered || active ? 2.5F : 2.0F) * scale);

    if (animation.focus > 0.001F) {
        drawList->AddRect(
            minimum,
            maximum,
            ImGui::GetColorU32(ui_internal::WithAlpha(
                ui_internal::kColorAccent,
                animation.focus)),
            6.0F * scale,
            0,
            1.5F * scale);
    }
    return changed;
}

}  // namespace zt::sequence::ui
