#include "UI/PlayerUIInternal.h"

#include "Core/ComparisonPlayer.h"
#include "UI/PlayerUILogic.h"

#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace zt::sequence {

using ui_internal::AnimatedButton;
using ui_internal::AnimatedButtonStyle;
using ui_internal::DecodeDescription;
using ui_internal::kColorMuted;
using ui_internal::kColorPrimary;
using ui_internal::kColorSurfaceActive;
using ui_internal::kControlHeight;
using ui_internal::TooltipForLastItem;

namespace {

inline constexpr float kKeyboardSpeedPreferredControlWidth = 220.0F;
inline constexpr float kKeyboardSpeedEndpointInset = 11.0F;
inline constexpr float kKeyboardSpeedTrackCenterY = 11.5F;
inline constexpr float kKeyboardSpeedTrackHalfHeight = 6.5F;
inline constexpr float kKeyboardSpeedThumbRadius = 9.0F;
inline constexpr float kResourceSettingsPreferredWidth = 876.0F;
inline constexpr float kDecodeComboWidth = 168.0F;
inline constexpr float kConstrainedDecodeComboWidth = 136.0F;
inline constexpr float kMaskComboWidth = 152.0F;
inline constexpr float kConstrainedMaskComboWidth = 116.0F;

struct CapsuleSliderResult final {
    bool changed = false;
    bool released = false;
};

[[nodiscard]] CapsuleSliderResult RenderCapsulePercentageSlider(
    ui::InteractionAnimator& animator,
    const char* const idLabel,
    const char* const displayLabel,
    int& value,
    const ImVec2 size,
    const float uiScale) {
    CapsuleSliderResult result;
    if (idLabel == nullptr || displayLabel == nullptr || size.x <= 0.0F ||
        size.y <= 0.0F) {
        return result;
    }

    const float scale = std::max(uiScale, 0.5F);
    value = ui_detail::ClampKeyboardShuttleSpeedPercent(value);
    const ImGuiID id = ImGui::GetID(idLabel);
    static_cast<void>(ImGui::InvisibleButton(
        idLabel,
        size,
        ImGuiButtonFlags_MouseButtonLeft));
    const ui::InteractionAnimation animation = animator.ObserveLastItem(id);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const float centerY = std::min(
        maximum.y,
        minimum.y + kKeyboardSpeedTrackCenterY * scale);
    const float endpointInset = std::min(
        kKeyboardSpeedEndpointInset * scale,
        std::max(0.0F, (size.x - 1.0F) * 0.5F));
    const float trackMinimumX = minimum.x + endpointInset;
    const float trackMaximumX = std::max(
        trackMinimumX + 1.0F,
        maximum.x - endpointInset);
    const float trackWidth = trackMaximumX - trackMinimumX;

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double normalized = std::clamp(
            static_cast<double>(
                (ImGui::GetIO().MousePos.x - trackMinimumX) / trackWidth),
            0.0,
            1.0);
        const double speedSteps = static_cast<double>(
            ui_detail::kMaximumKeyboardShuttleSpeedPercent -
            ui_detail::kMinimumKeyboardShuttleSpeedPercent) /
            static_cast<double>(ui_detail::kKeyboardShuttleSpeedPercentStep);
        const int stepIndex = static_cast<int>(
            std::floor(normalized * speedSteps + 0.5));
        const int requested =
            ui_detail::kMinimumKeyboardShuttleSpeedPercent +
            stepIndex * ui_detail::kKeyboardShuttleSpeedPercentStep;
        const int snapped =
            ui_detail::ClampKeyboardShuttleSpeedPercent(requested);
        result.changed = snapped != value;
        value = snapped;
        if (result.changed) {
            ImGui::MarkItemEdited(id);
        }
    }
    result.released = ImGui::IsItemDeactivatedAfterEdit();

    const double normalizedValue = static_cast<double>(
        value - ui_detail::kMinimumKeyboardShuttleSpeedPercent) /
        static_cast<double>(
            ui_detail::kMaximumKeyboardShuttleSpeedPercent -
            ui_detail::kMinimumKeyboardShuttleSpeedPercent);
    const float thumbX = trackMinimumX + trackWidth *
        static_cast<float>(std::clamp(normalizedValue, 0.0, 1.0));
    const float trackHalfHeight =
        (kKeyboardSpeedTrackHalfHeight + animation.hover * 0.35F) * scale;

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        ImVec2(trackMinimumX, centerY - trackHalfHeight),
        ImVec2(trackMaximumX, centerY + trackHalfHeight),
        ImGui::GetColorU32(kColorSurfaceActive),
        trackHalfHeight);
    if (thumbX > trackMinimumX) {
        drawList->AddRectFilled(
            ImVec2(trackMinimumX, centerY - trackHalfHeight),
            ImVec2(thumbX, centerY + trackHalfHeight),
            ImGui::GetColorU32(kColorPrimary),
            trackHalfHeight);
    }

    const float thumbRadius =
        (kKeyboardSpeedThumbRadius + animation.hover * 0.8F +
            animation.press * 0.9F) * scale;
    drawList->AddCircleFilled(
        ImVec2(thumbX, centerY),
        thumbRadius,
        ImGui::GetColorU32(ui_internal::kColorTimelineHotCache),
        24);
    const ImVec2 labelSize = ImGui::CalcTextSize(displayLabel);
    drawList->AddText(
        ImVec2(
            minimum.x + std::max(0.0F, (size.x - labelSize.x) * 0.5F),
            maximum.y - labelSize.y),
        ImGui::GetColorU32(kColorMuted),
        displayLabel);
    return result;
}

}  // namespace

void PlayerUI::Impl::RenderQuickActions(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const bool comparisonEnabled,
    const UiActions& actions) {
    const bool canChooseFolder = static_cast<bool>(actions.chooseFolder);
    ImGui::BeginDisabled(snapshot.loading || !canChooseFolder);
    if (AnimatedButton(
            interactionAnimator_,
            "打开文件夹",
            ImVec2(Scale(92.0F), Scale(kControlHeight)))) {
        OpenFolder(player, actions);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool currentSequenceRecorded =
        actions.hasCurrentSequence && actions.hasCurrentSequence();
    const bool canOpenCurrentSequence =
        static_cast<bool>(actions.openCurrentSequence) &&
        currentSequenceRecorded;
    ImGui::BeginDisabled(snapshot.loading || !canOpenCurrentSequence);
    if (AnimatedButton(
            interactionAnimator_,
            "打开上次序列",
            ImVec2(Scale(112.0F), Scale(kControlHeight)))) {
        actions.openCurrentSequence();
    }
    TooltipForLastItem(currentSequenceRecorded
        ? "打开上次成功拖入的 PNG 序列文件夹"
        : "尚未记录拖入的 PNG 序列文件夹");
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool canRescan = snapshot.sourceKind == SourceKind::PngSequence;
    ImGui::BeginDisabled(snapshot.loading || !canRescan);
    if (AnimatedButton(
            interactionAnimator_,
            "重新加载",
            ImVec2(Scale(80.0F), Scale(kControlHeight)))) {
        ReloadFolder(player);
    }
    TooltipForLastItem(canRescan
        ? "重新加载当前序列；失败时保留现有序列"
        : "仅 PNG 序列支持重新加载");
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (AnimatedButton(
            interactionAnimator_,
            comparisonEnabled
                ? "退出对比###ComparisonToggle"
                : "对比画面###ComparisonToggle",
            ImVec2(Scale(88.0F), Scale(kControlHeight)),
            comparisonEnabled
                ? AnimatedButtonStyle::Selected
                : AnimatedButtonStyle::Neutral)) {
        const bool modeChanged =
            player.SetComparisonEnabled(!comparisonEnabled);
        if (modeChanged) {
            if (!comparisonEnabled) {
                activeViewportPane_ = ViewportPane::Secondary;
            } else {
                activeViewportPane_ = ViewportPane::Primary;
                ResetViewportView(ViewportPane::Secondary);
            }
        } else if (!comparisonEnabled) {
            SetLocalError(
                "暂时无法开启对比画面",
                "主画面仍在加载候选来源或解码比例，请完成后重试。");
        }
    }
    TooltipForLastItem(comparisonEnabled
        ? "退出双画面对比；保留主画面来源、缓存和视图"
        : "将画布分成左右两侧，然后把来源拖入右侧");

    ImGui::SameLine();
    const ViewportPaneState& activePane = PaneState(activeViewportPane_);
    ImGui::BeginDisabled(
        ui_detail::IsDefaultViewportTransform(activePane.transform));
    if (AnimatedButton(
            interactionAnimator_,
            "重置画面",
            ImVec2(Scale(80.0F), Scale(kControlHeight)))) {
        ResetViewportView(activeViewportPane_);
    }
    TooltipForLastItem(activeViewportPane_ == ViewportPane::Primary
        ? "只恢复主画面的适应窗口与居中"
        : "只恢复对比画面的适应窗口与居中");
    ImGui::EndDisabled();
}

void PlayerUI::Impl::RenderResourceSettings(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot) {
    const bool constrained = ImGui::GetContentRegionAvail().x <
        Scale(kResourceSettingsPreferredWidth);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(Scale(10.0F), Scale(10.0F)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kColorMuted, "内存目标");
    ImGui::SameLine();
    bool memoryChanged = false;
    if (AnimatedButton(
            interactionAnimator_,
            "−##MemoryMinus",
            ImVec2(Scale(30.0F), Scale(kControlHeight)),
            AnimatedButtonStyle::Neutral)) {
        --memoryGiB_;
        memoryChanged = true;
    }
    TooltipForLastItem("减少 1 GB 内存目标");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scale(48.0F));
    const bool memorySubmitted = ImGui::InputInt(
        "##MemoryGiB",
        &memoryGiB_,
        0,
        0,
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool memoryFinished = ImGui::IsItemDeactivatedAfterEdit();
    TooltipForLastItem("进程内存目标，范围 4–48 GB");
    ImGui::SameLine();
    if (AnimatedButton(
            interactionAnimator_,
            "+##MemoryPlus",
            ImVec2(Scale(30.0F), Scale(kControlHeight)),
            AnimatedButtonStyle::Neutral)) {
        ++memoryGiB_;
        memoryChanged = true;
    }
    TooltipForLastItem("增加 1 GB 内存目标");
    if (memoryChanged || memorySubmitted || memoryFinished) {
        memoryGiB_ = ui_detail::ClampMemoryGiB(memoryGiB_);
        player.SetMemoryLimitBytes(
            ui_detail::MemoryBytesFromGiB(memoryGiB_));
    }
    ImGui::SameLine();
    ImGui::TextColored(kColorMuted, "GB");
    ImGui::SameLine(0.0F, Scale(12.0F));
    ImGui::TextColored(kColorMuted, "解码");
    ImGui::SameLine();
    RenderDecodePercent(player, snapshot, constrained);
    ImGui::SameLine(0.0F, Scale(12.0F));
    ImGui::TextColored(kColorMuted, "遮罩");
    ImGui::SameLine();
    RenderMaskPreset(constrained);
    ImGui::SameLine(0.0F, Scale(12.0F));
    RenderKeyboardShuttleSpeed();
    ImGui::PopStyleVar();
}

void PlayerUI::Impl::RenderKeyboardShuttleSpeed() {
    keyboardShuttleSpeedPercent_ =
        ui_detail::ClampKeyboardShuttleSpeedPercent(
            keyboardShuttleSpeedPercent_);
    std::array<char, 48> speedLabel{};
    static_cast<void>(std::snprintf(
        speedLabel.data(),
        speedLabel.size(),
        "长按速度 %d%%",
        keyboardShuttleSpeedPercent_));

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float trackWidth = std::min(
        Scale(kKeyboardSpeedPreferredControlWidth),
        std::max(1.0F, availableWidth));
    const CapsuleSliderResult result = RenderCapsulePercentageSlider(
        interactionAnimator_,
        "##KeyboardShuttleSpeed",
        speedLabel.data(),
        keyboardShuttleSpeedPercent_,
        ImVec2(trackWidth, Scale(kControlHeight)),
        uiScale_);
    if (result.released) {
        keyboardShuttleSpeedPersistPending_ = true;
    }

    const double approximateFramesPerSecond =
        static_cast<double>(framesPerSecond_) *
        ui_detail::KeyboardShuttlePlaybackRateFromPercent(
            keyboardShuttleSpeedPercent_);
    std::array<char, 128> tooltip{};
    static_cast<void>(std::snprintf(
        tooltip.data(),
        tooltip.size(),
        "方向键长按速度：%d%% · 当前约%.1f FPS",
        keyboardShuttleSpeedPercent_,
        approximateFramesPerSecond));
    TooltipForLastItem(tooltip.data());
}

void PlayerUI::Impl::RenderDecodePercent(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const bool constrained) {
    std::string preview = DecodeDescription(decodePercent_);
    if (decodeChangePending_) {
        preview = std::to_string(decodePercent_) + "% → 当前" +
            std::to_string(snapshot.decodePercent) + "%";
    }

    ImGui::SetNextItemWidth(Scale(
        constrained ? kConstrainedDecodeComboWidth : kDecodeComboWidth));
    if (!ImGui::BeginCombo("##DecodePercent", preview.c_str())) {
        return;
    }

    constexpr std::array<std::uint32_t, 4> percentages{
        25U,
        50U,
        75U,
        100U};
    for (const std::uint32_t percent : percentages) {
        const std::string label = DecodeDescription(percent);
        const bool selected = decodePercent_ == percent;
        if (ImGui::Selectable(label.c_str(), selected)) {
            decodePercent_ = percent;
            requestedDecodePercent_ = percent;
            decodeChangePending_ =
                snapshot.hasSource && percent != snapshot.decodePercent;
            decodeChangeFailed_ = false;
            player.SetDecodePercent(percent);
            dismissedEngineError_.clear();
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void PlayerUI::Impl::RenderMaskPreset(const bool constrained) {
    const std::string_view preview = ui::MaskPresetLabel(maskPreset_);
    ImGui::SetNextItemWidth(Scale(
        constrained ? kConstrainedMaskComboWidth : kMaskComboWidth));
    if (!ImGui::BeginCombo("##MaskPreset", preview.data())) {
        TooltipForLastItem("居中遮罩仅影响预览，不修改源文件");
        return;
    }

    for (const ui::MaskPreset preset : ui::kMaskPresets) {
        const std::string_view label = ui::MaskPresetLabel(preset);
        const bool selected = maskPreset_ == preset;
        if (ImGui::Selectable(label.data(), selected)) {
            maskPreset_ = preset;
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
    TooltipForLastItem("居中遮罩仅影响预览，不修改源文件");
}

}  // namespace zt::sequence
