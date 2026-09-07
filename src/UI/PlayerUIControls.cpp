#include "UI/PlayerUIInternal.h"

#include "Core/ComparisonPlayer.h"
#include "UI/PlayerUILogic.h"

#include <array>
#include <string>

namespace zt::sequence {

using ui_internal::AnimatedButton;
using ui_internal::AnimatedButtonStyle;
using ui_internal::DecodeDescription;
using ui_internal::kColorMuted;
using ui_internal::kControlHeight;
using ui_internal::TooltipForLastItem;

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
    RenderDecodePercent(player, snapshot);
    ImGui::SameLine(0.0F, Scale(12.0F));
    ImGui::TextColored(kColorMuted, "遮罩");
    ImGui::SameLine();
    RenderMaskPreset();
    ImGui::PopStyleVar();
}

void PlayerUI::Impl::RenderDecodePercent(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot) {
    std::string preview = DecodeDescription(decodePercent_);
    if (decodeChangePending_) {
        preview = std::to_string(decodePercent_) + "% → 当前" +
            std::to_string(snapshot.decodePercent) + "%";
    }

    ImGui::SetNextItemWidth(Scale(168.0F));
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

void PlayerUI::Impl::RenderMaskPreset() {
    const std::string_view preview = ui::MaskPresetLabel(maskPreset_);
    ImGui::SetNextItemWidth(Scale(152.0F));
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
