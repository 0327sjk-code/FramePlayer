#include "UI/PlayerUIInternal.h"

#include "UI/PlayerUILogic.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace zt::sequence::ui_internal {

const ImVec4 kColorBackground{0.03939F, 0.03939F, 0.03939F, 1.0F};
const ImVec4 kColorSurface{0.08171F, 0.08171F, 0.08171F, 1.0F};
const ImVec4 kColorSurfaceRaised{0.11762F, 0.11762F, 0.11762F, 1.0F};
const ImVec4 kColorSurfaceHover{0.16469F, 0.16469F, 0.16469F, 1.0F};
const ImVec4 kColorSurfaceActive{0.20500F, 0.20500F, 0.20500F, 1.0F};
const ImVec4 kColorBorder{0.16469F, 0.16469F, 0.16469F, 1.0F};
const ImVec4 kColorInk{1.0F, 1.0F, 1.0F, 1.0F};
const ImVec4 kColorMuted{0.77255F, 0.78431F, 0.77647F, 1.0F};
const ImVec4 kColorPrimary{0.18431F, 0.47843F, 0.33333F, 1.0F};
const ImVec4 kColorPrimaryHover{0.22745F, 0.50980F, 0.36863F, 1.0F};
const ImVec4 kColorPrimaryActive{0.15686F, 0.41569F, 0.28627F, 1.0F};
const ImVec4 kColorExportProgress{0.29020F, 0.87059F, 0.50196F, 1.0F};
const ImVec4 kColorTimelinePlayback{0.95294F, 0.97255F, 0.96078F, 1.0F};
const ImVec4 kColorTimelineHotCache{0.52549F, 0.78824F, 0.64314F, 1.0F};
const ImVec4 kColorTimelineColdCache{0.29020F, 0.56863F, 0.41569F, 1.0F};
const ImVec4 kColorOnPrimary{1.0F, 1.0F, 1.0F, 1.0F};
const ImVec4 kColorAccent{0.37310F, 0.73922F, 0.93936F, 1.0F};
const ImVec4 kColorWarning{0.87371F, 0.68952F, 0.26012F, 1.0F};
const ImVec4 kColorDanger{0.91906F, 0.33978F, 0.29206F, 1.0F};
const ImVec4 kColorViewportBackground{0.01680F, 0.01680F, 0.01680F, 1.0F};
const ImVec4 kColorViewportBadge{0.03939F, 0.03939F, 0.03939F, 0.90F};
const ImVec4 kColorViewportBadgeText{1.0F, 1.0F, 1.0F, 1.0F};
const ImVec4 kColorMask{0.01569F, 0.01569F, 0.01569F, 1.0F};
const ImVec4 kColorMaskBorder{1.0F, 1.0F, 1.0F, 0.90F};

namespace {

[[nodiscard]] double BytesToGiB(const std::uint64_t bytes) noexcept {
    return static_cast<double>(bytes) /
        static_cast<double>(ui_detail::kBytesPerGiB);
}

[[nodiscard]] const char* RenderedTextEnd(const char* label) noexcept {
    if (label == nullptr) {
        return nullptr;
    }
    const char* current = label;
    while (*current != '\0') {
        if (current[0] == '#' && current[1] == '#') {
            return current;
        }
        ++current;
    }
    return current;
}

[[nodiscard]] ImVec4 ButtonBaseColor(const AnimatedButtonStyle style) noexcept {
    switch (style) {
    case AnimatedButtonStyle::Primary:
    case AnimatedButtonStyle::Selected:
        return kColorPrimary;
    case AnimatedButtonStyle::Ghost:
        return WithAlpha(kColorSurfaceRaised, 0.0F);
    case AnimatedButtonStyle::Neutral:
    default:
        return kColorSurfaceRaised;
    }
}

[[nodiscard]] ImVec4 ButtonHoverColor(const AnimatedButtonStyle style) noexcept {
    switch (style) {
    case AnimatedButtonStyle::Primary:
    case AnimatedButtonStyle::Selected:
        return kColorPrimaryHover;
    case AnimatedButtonStyle::Ghost:
        return kColorSurfaceRaised;
    case AnimatedButtonStyle::Neutral:
    default:
        return kColorSurfaceHover;
    }
}

[[nodiscard]] ImVec4 ButtonActiveColor(const AnimatedButtonStyle style) noexcept {
    switch (style) {
    case AnimatedButtonStyle::Primary:
    case AnimatedButtonStyle::Selected:
        return kColorPrimaryActive;
    case AnimatedButtonStyle::Ghost:
        return kColorSurfaceHover;
    case AnimatedButtonStyle::Neutral:
    default:
        return kColorSurfaceActive;
    }
}

[[nodiscard]] ImVec4 ButtonTextColor(const AnimatedButtonStyle style) noexcept {
    return style == AnimatedButtonStyle::Primary ||
            style == AnimatedButtonStyle::Selected
        ? kColorOnPrimary
        : kColorInk;
}

}  // namespace

int RoundedMemoryGiB(const std::uint64_t bytes) noexcept {
    const auto rounded =
        (bytes + (ui_detail::kBytesPerGiB / 2ULL)) / ui_detail::kBytesPerGiB;
    return ui_detail::ClampMemoryGiB(static_cast<int>(rounded));
}

std::string CacheSummary(const PlayerSnapshot& snapshot) {
    const std::uint64_t processBytes = snapshot.processPrivateBytes > 0
        ? snapshot.processPrivateBytes
        : snapshot.processWorkingSetBytes;
    std::ostringstream stream;
    stream << "缓存 " << snapshot.cachedFrames << " / " << snapshot.totalFrames
           << "  ·  前方 " << std::fixed << std::setprecision(1)
           << snapshot.readyAheadSeconds << " 秒"
           << "  ·  图片缓存 " << BytesToGiB(snapshot.cacheBytes) << " GB"
           << " / 进程 " << BytesToGiB(processBytes) << " GB"
           << " / 目标 " << BytesToGiB(snapshot.memoryLimitBytes) << " GB";
    return stream.str();
}

std::string CompactCacheSummary(const PlayerSnapshot& snapshot) {
    const std::uint64_t processBytes = snapshot.processPrivateBytes > 0
        ? snapshot.processPrivateBytes
        : snapshot.processWorkingSetBytes;
    std::ostringstream stream;
    stream << snapshot.cachedFrames << "/" << snapshot.totalFrames
           << " · " << std::fixed << std::setprecision(1)
           << BytesToGiB(processBytes) << "/"
           << BytesToGiB(snapshot.memoryLimitBytes) << " GB";
    return stream.str();
}

std::string PerformanceSummary(const PlayerSnapshot& snapshot) {
    std::ostringstream stream;
    stream << "目标 " << std::fixed << std::setprecision(0)
           << snapshot.targetFramesPerSecond << " FPS"
           << "  ·  实际 " << std::setprecision(1)
           << snapshot.actualFramesPerSecond << " FPS"
           << "  ·  掉帧 " << snapshot.droppedFrames;
    return stream.str();
}

std::string DecodeDescription(const std::uint32_t percent) {
    std::string description = std::to_string(percent) + "%";
    description += percent == 100U ? " · 原始像素" : " · 缩放预览";
    return description;
}

void TooltipForLastItem(const char* text) {
    if (text == nullptr || text[0] == '\0' ||
        !ImGui::IsItemHovered(
            ImGuiHoveredFlags_DelayNormal |
            ImGuiHoveredFlags_AllowWhenDisabled)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0F);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void CenteredText(const char* text, const float availableWidth) {
    const float textWidth = ImGui::CalcTextSize(text).x;
    const float offset = std::max(0.0F, (availableWidth - textWidth) * 0.5F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    ImGui::TextUnformatted(text);
}

void EllipsizedText(
    const char* text,
    const float availableWidth,
    const ImVec4 color) {
    const char* safeText = text != nullptr ? text : "";
    const float width = std::max(1.0F, availableWidth);
    const float height = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(width, height));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(
        ImGui::GetWindowDrawList(),
        minimum,
        maximum,
        maximum.x,
        safeText,
        nullptr,
        nullptr);
    ImGui::PopStyleColor();
}

bool AnimatedButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size,
    const AnimatedButtonStyle style) {
    if (label == nullptr || size.x <= 0.0F || size.y <= 0.0F) {
        return false;
    }

    const ImGuiID id = ImGui::GetID(label);
    const bool clicked = ImGui::InvisibleButton(
        label,
        size,
        ImGuiButtonFlags_MouseButtonLeft);
    const ui::InteractionAnimation animation = animator.ObserveLastItem(id);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    ImVec4 fill = LerpColor(
        ButtonBaseColor(style),
        ButtonHoverColor(style),
        animation.hover);
    fill = LerpColor(fill, ButtonActiveColor(style), animation.press);
    const float rounding = ImGui::GetStyle().FrameRounding;
    drawList->AddRectFilled(
        minimum,
        maximum,
        ImGui::GetColorU32(fill),
        rounding);

    if (style == AnimatedButtonStyle::Neutral) {
        drawList->AddRect(
            minimum,
            maximum,
            ImGui::GetColorU32(kColorBorder),
            rounding,
            0,
            1.0F);
    }
    if (animation.focus > 0.001F) {
        drawList->AddRect(
            ImVec2(minimum.x - 1.0F, minimum.y - 1.0F),
            ImVec2(maximum.x + 1.0F, maximum.y + 1.0F),
            ImGui::GetColorU32(WithAlpha(kColorAccent, animation.focus)),
            rounding + 1.0F,
            0,
            1.5F);
    }

    const char* textEnd = RenderedTextEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, textEnd);
    const float pressOffset = animation.press;
    const ImVec2 textPosition{
        minimum.x + ((size.x - textSize.x) * 0.5F),
        minimum.y + ((size.y - textSize.y) * 0.5F) + pressOffset};
    drawList->AddText(
        textPosition,
        ImGui::GetColorU32(ButtonTextColor(style)),
        label,
        textEnd);
    return clicked;
}

bool AnimatedCheckbox(
    ui::InteractionAnimator& animator,
    const char* label,
    bool* value,
    const float height) {
    if (label == nullptr || value == nullptr || height <= 0.0F) {
        return false;
    }

    const float boxSize = std::min(height, ImGui::GetFontSize() + 7.0F);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const char* textEnd = RenderedTextEnd(label);
    const ImVec2 textSize = ImGui::CalcTextSize(label, textEnd);
    const ImVec2 size{boxSize + spacing + textSize.x, height};
    const ImGuiID id = ImGui::GetID(label);
    const bool clicked = ImGui::InvisibleButton(
        label,
        size,
        ImGuiButtonFlags_MouseButtonLeft);
    if (clicked) {
        *value = !*value;
    }

    const ui::InteractionAnimation animation = animator.ObserveLastItem(id);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const float boxY = minimum.y + ((height - boxSize) * 0.5F);
    const ImVec2 boxMinimum{minimum.x, boxY};
    const ImVec2 boxMaximum{minimum.x + boxSize, boxY + boxSize};
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImVec4 unchecked = LerpColor(
        kColorSurfaceRaised,
        kColorSurfaceHover,
        animation.hover);
    ImVec4 fill = *value ? kColorPrimary : unchecked;
    fill = LerpColor(fill, *value ? kColorPrimaryActive : kColorSurfaceActive,
                     animation.press);
    const float rounding = std::min(ImGui::GetStyle().FrameRounding, boxSize * 0.28F);
    drawList->AddRectFilled(
        boxMinimum,
        boxMaximum,
        ImGui::GetColorU32(fill),
        rounding);
    drawList->AddRect(
        boxMinimum,
        boxMaximum,
        ImGui::GetColorU32(*value ? kColorPrimary : kColorBorder),
        rounding,
        0,
        1.0F);

    if (*value) {
        const float unit = boxSize / 10.0F;
        const ImU32 checkColor = ImGui::GetColorU32(kColorOnPrimary);
        drawList->AddLine(
            ImVec2(boxMinimum.x + 2.4F * unit, boxMinimum.y + 5.2F * unit),
            ImVec2(boxMinimum.x + 4.3F * unit, boxMinimum.y + 7.0F * unit),
            checkColor,
            std::max(1.5F, 1.35F * unit));
        drawList->AddLine(
            ImVec2(boxMinimum.x + 4.3F * unit, boxMinimum.y + 7.0F * unit),
            ImVec2(boxMinimum.x + 7.8F * unit, boxMinimum.y + 3.2F * unit),
            checkColor,
            std::max(1.5F, 1.35F * unit));
    }

    drawList->AddText(
        ImVec2(
            boxMaximum.x + spacing,
            minimum.y + ((height - textSize.y) * 0.5F)),
        ImGui::GetColorU32(kColorInk),
        label,
        textEnd);
    if (animation.focus > 0.001F) {
        drawList->AddRect(
            ImVec2(minimum.x - 1.0F, minimum.y - 1.0F),
            ImVec2(minimum.x + size.x + 1.0F, minimum.y + height + 1.0F),
            ImGui::GetColorU32(WithAlpha(kColorAccent, animation.focus)),
            rounding + 1.0F,
            0,
            1.5F);
    }
    return clicked;
}

bool PrimaryButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size) {
    return AnimatedButton(
        animator,
        label,
        size,
        AnimatedButtonStyle::Primary);
}

bool PlaybackButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size,
    const bool selected) {
    return AnimatedButton(
        animator,
        label,
        size,
        selected
            ? AnimatedButtonStyle::Selected
            : AnimatedButtonStyle::Neutral);
}

ImVec4 WithAlpha(ImVec4 color, const float alpha) noexcept {
    color.w = alpha;
    return color;
}

ImVec4 LerpColor(
    const ImVec4& first,
    const ImVec4& second,
    const float amount) noexcept {
    const float clamped = std::clamp(amount, 0.0F, 1.0F);
    return {
        first.x + ((second.x - first.x) * clamped),
        first.y + ((second.y - first.y) * clamped),
        first.z + ((second.z - first.z) * clamped),
        first.w + ((second.w - first.w) * clamped),
    };
}

}  // namespace zt::sequence::ui_internal
