#include "UI/PlayerUIInternal.h"

#include "Core/ComparisonPlayer.h"
#include "Export/FfmpegExportController.h"
#include "Platform/UserSettings.h"
#include "Platform/Utf8.h"
#include "UI/ComparisonCanvasLayout.h"
#include "UI/PlayerUILogic.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace zt::sequence {

using ui_internal::CacheSummary;
using ui_internal::CompactCacheSummary;
using ui_internal::EllipsizedText;
using ui_internal::AnimatedButton;
using ui_internal::AnimatedButtonStyle;
using ui_internal::AnimatedCheckbox;
using ui_internal::kColorDanger;
using ui_internal::kColorExportProgress;
using ui_internal::kColorMuted;
using ui_internal::kColorPrimary;
using ui_internal::kColorPrimaryHover;
using ui_internal::kColorSurface;
using ui_internal::kColorSurfaceActive;
using ui_internal::kColorTimelineColdCache;
using ui_internal::kColorTimelineHotCache;
using ui_internal::kColorTimelinePlayback;
using ui_internal::kColorWarning;
using ui_internal::kControlHeight;
using ui_internal::PerformanceSummary;
using ui_internal::PlaybackButton;
using ui_internal::PrimaryButton;
using ui_internal::TooltipForLastItem;
using ui_internal::WithAlpha;

namespace {

[[nodiscard]] bool IsExportBusy(
    const exporting::ExportState state) noexcept {
    return state == exporting::ExportState::Preparing ||
        state == exporting::ExportState::Running ||
        state == exporting::ExportState::Retrying ||
        state == exporting::ExportState::Cancelling;
}

[[nodiscard]] bool IsExportableSourceKind(
    const SourceKind kind) noexcept {
    return kind == SourceKind::PngSequence || kind == SourceKind::Video;
}

[[nodiscard]] std::filesystem::path DefaultExportFolder(
    const ExportSourceSnapshot& source) {
    return std::visit(
        [](const auto& value) {
            using Snapshot = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Snapshot, SequenceExportSnapshot>) {
                return value.sourceFolder;
            } else {
                return value.sourceFile.parent_path();
            }
        },
        source);
}

[[nodiscard]] const char* ExportProgressLabel(
    const exporting::ExportProgressSnapshot& progress) noexcept {
    if (!progress.statusUtf8.empty()) {
        return progress.statusUtf8.c_str();
    }
    switch (progress.state) {
    case exporting::ExportState::Preparing:
        return "正在准备导出 MP4";
    case exporting::ExportState::Running:
        return "正在导出 MP4";
    case exporting::ExportState::Retrying:
        return "正在重试导出 MP4";
    case exporting::ExportState::Cancelling:
        return "正在取消导出";
    case exporting::ExportState::Idle:
    case exporting::ExportState::Completed:
    case exporting::ExportState::Cancelled:
    case exporting::ExportState::Failed:
        return "导出";
    }
    return "导出";
}

[[nodiscard]] const char* OnlineUpdateButtonLabel(
    const OnlineUpdatePhase phase) noexcept {
    switch (phase) {
    case OnlineUpdatePhase::Checking:
        return "检查中";
    case OnlineUpdatePhase::Downloading:
        return "下载中";
    case OnlineUpdatePhase::ReadyToInstall:
        return "重启并更新";
    case OnlineUpdatePhase::Failed:
        return "重试";
    case OnlineUpdatePhase::Unavailable:
    case OnlineUpdatePhase::Idle:
    case OnlineUpdatePhase::UpToDate:
        return "检查更新";
    }
    return "检查更新";
}

[[nodiscard]] bool IsOnlineUpdateBusy(
    const OnlineUpdatePhase phase) noexcept {
    return phase == OnlineUpdatePhase::Checking ||
        phase == OnlineUpdatePhase::Downloading;
}

[[nodiscard]] bool HasVisibleOnlineUpdateStatus(
    const OnlineUpdatePhase phase) noexcept {
    return phase == OnlineUpdatePhase::Checking ||
        phase == OnlineUpdatePhase::Downloading ||
        phase == OnlineUpdatePhase::UpToDate ||
        phase == OnlineUpdatePhase::ReadyToInstall ||
        phase == OnlineUpdatePhase::Failed;
}

[[nodiscard]] std::string DisplayVersion(const std::string& version) {
    if (version.empty() || version.front() == 'v' || version.front() == 'V') {
        return version;
    }
    return "v" + version;
}

[[nodiscard]] std::string FormatDownloadBytes(const std::uint64_t bytes) {
    constexpr double kBytesPerKibibyte = 1024.0;
    constexpr double kBytesPerMebibyte =
        kBytesPerKibibyte * kBytesPerKibibyte;
    constexpr double kBytesPerGibibyte =
        kBytesPerMebibyte * kBytesPerKibibyte;

    std::ostringstream result;
    if (bytes >= static_cast<std::uint64_t>(kBytesPerGibibyte)) {
        result << std::fixed << std::setprecision(1)
               << static_cast<double>(bytes) / kBytesPerGibibyte << " GB";
    } else if (bytes >= static_cast<std::uint64_t>(kBytesPerMebibyte)) {
        result << std::fixed << std::setprecision(1)
               << static_cast<double>(bytes) / kBytesPerMebibyte << " MB";
    } else if (bytes >= static_cast<std::uint64_t>(kBytesPerKibibyte)) {
        result << std::fixed << std::setprecision(0)
               << static_cast<double>(bytes) / kBytesPerKibibyte << " KB";
    } else {
        result << bytes << " B";
    }
    return result.str();
}

[[nodiscard]] std::string PathDisplayName(
    const std::filesystem::path& path) {
    const std::filesystem::path filename = path.filename();
    const std::wstring value = filename.empty()
        ? path.wstring()
        : filename.wstring();
    const std::string utf8 = WideToUtf8(value);
    return utf8.empty() ? "自定义位置" : utf8;
}

struct FrameNumberInputResult final {
    bool commit = false;
    bool active = false;
};

[[nodiscard]] FrameNumberInputResult RenderFrameNumberInput(
    const char* const label,
    std::int64_t& value,
    const float width,
    bool& committedWhileActive) {
    ImGui::SetNextItemWidth(width);
    constexpr ImGuiInputTextFlags inputFlags =
        ImGuiInputTextFlags_CharsDecimal |
        ImGuiInputTextFlags_EnterReturnsTrue;
    const bool submitted = ImGui::InputScalar(
        label,
        ImGuiDataType_S64,
        &value,
        nullptr,
        nullptr,
        "%lld",
        inputFlags);
    const bool activated = ImGui::IsItemActivated();
    const bool edited = ImGui::IsItemEdited();
    const bool deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
    if (activated || edited) {
        committedWhileActive = false;
    }
    const bool commit = submitted ||
        (deactivatedAfterEdit && !committedWhileActive);
    if (submitted) {
        committedWhileActive = true;
    }
    if (ImGui::IsItemDeactivated()) {
        committedWhileActive = false;
    }
    return {
        commit,
        ImGui::IsItemActive()};
}

[[nodiscard]] constexpr std::int64_t OneBasedFrameNumber(
    const FrameIndex frame,
    const std::size_t totalFrames) noexcept {
    return totalFrames == 0U
        ? 0
        : static_cast<std::int64_t>(frame) + 1;
}

}  // namespace

void PlayerUI::Impl::RenderBottomBar(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const bool comparisonEnabled,
    exporting::FfmpegExportController& exporter,
    const exporting::ExportProgressSnapshot& exportProgress,
    const OnlineUpdateView& onlineUpdateView,
    const UiActions& actions,
    const ErrorView& error,
    const float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(Scale(16.0F), 0.0F));
    ImGui::BeginChild(
        "##BottomBar",
        ImVec2(0.0F, height),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float logicalWidth =
        ImGui::GetWindowSize().x / std::max(uiScale_, 0.5F);
    const bool compact = logicalWidth < ui_internal::kCompactBottomBarThreshold;

    ImGui::SetCursorPosY(Scale(8.0F));
    RenderTimeline(player, snapshot);
    ImGui::SetCursorPosY(Scale(42.0F));
    RenderPlaybackRange(player, snapshot);
    ImGui::SetCursorPosY(Scale(80.0F));
    RenderPlaybackControls(
        player,
        snapshot,
        comparisonEnabled,
        exporter,
        exportProgress,
        onlineUpdateView,
        actions,
        compact);
    ImGui::SetCursorPosY(Scale(compact ? 244.0F : 164.0F));
    RenderStatusLine(snapshot, exportProgress, onlineUpdateView, error);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void PlayerUI::Impl::RenderTimeline(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot) {
    const float hitHeight = Scale(32.0F);
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    ImGui::BeginDisabled(!snapshot.hasSource || snapshot.loading);
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (ImGui::BeginTable(
            "##TimelineLayout",
            2,
            tableFlags,
            ImVec2(-1.0F, hitHeight))) {
        ImGui::TableSetupColumn(
            "##TimelineTrack",
            ImGuiTableColumnFlags_WidthStretch,
            1.0F);
        ImGui::TableSetupColumn(
            "##TimelineValue",
            ImGuiTableColumnFlags_WidthFixed,
            Scale(248.0F));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, hitHeight);
        ImGui::TableSetColumnIndex(0);

        const float trackWidth = std::max(
            Scale(80.0F),
            ImGui::GetContentRegionAvail().x);
        const ImGuiID timelineId = ImGui::GetID("##Timeline");
        static_cast<void>(ImGui::InvisibleButton(
            "##Timeline",
            ImVec2(trackWidth, hitHeight),
            ImGuiButtonFlags_MouseButtonLeft));
        const ui::InteractionAnimation timelineAnimation =
            interactionAnimator_.ObserveLastItem(timelineId);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const float centerY = (minimum.y + maximum.y) * 0.5F;
        const float knobRadius = Scale(
            6.0F + timelineAnimation.hover + timelineAnimation.press);
        const float trackEndpointInset = Scale(8.0F);
        const float trackMinimumX = minimum.x + trackEndpointInset;
        const float trackMaximumX = maximum.x - trackEndpointInset;
        const float usableTrackWidth =
            std::max(1.0F, trackMaximumX - trackMinimumX);
        const float trackHalfHeight =
            Scale(4.0F + timelineAnimation.hover * 0.5F);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            ImVec2(trackMinimumX, centerY - trackHalfHeight),
            ImVec2(trackMaximumX, centerY + trackHalfHeight),
            ImGui::GetColorU32(kColorSurfaceActive),
            Scale(4.0F));

        const double cacheProgress =
            std::clamp(snapshot.cacheProgress, 0.0, 1.0);
        const float cacheX = trackMinimumX +
            usableTrackWidth * static_cast<float>(cacheProgress);
        drawList->AddRectFilled(
            ImVec2(trackMinimumX, centerY - trackHalfHeight),
            ImVec2(cacheX, centerY + trackHalfHeight),
            ImGui::GetColorU32(kColorTimelineColdCache),
            Scale(4.0F));

        const double currentProgress = snapshot.totalFrames > 1U
            ? static_cast<double>(snapshot.currentFrame) /
                static_cast<double>(snapshot.totalFrames - 1U)
            : 0.0;
        const float currentX = trackMinimumX +
            usableTrackWidth * static_cast<float>(currentProgress);
        const long long currentFrameNumber = static_cast<long long>(
            OneBasedFrameNumber(
                snapshot.currentFrame,
                snapshot.totalFrames));

        const double readyAheadSeconds =
            snapshot.readyAheadSeconds > 0.0 &&
                std::isfinite(snapshot.readyAheadSeconds)
            ? snapshot.readyAheadSeconds
            : 0.0;
        std::uint64_t readyStepCount = 0U;
        if (readyAheadSeconds > 0.0 &&
            snapshot.targetFramesPerSecond > 0.0 &&
            std::isfinite(snapshot.targetFramesPerSecond)) {
            const double readySteps = readyAheadSeconds *
                snapshot.targetFramesPerSecond;
            const double maximumSteps = static_cast<double>(
                std::numeric_limits<std::uint64_t>::max());
            readyStepCount = readySteps >= maximumSteps
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(readySteps + 0.5);
        }
        const ui_detail::TimelineReadyCacheSegments readySegments =
            ui_detail::CalculateTimelineReadyCacheSegments(
                snapshot.totalFrames,
                snapshot.requestedFrame,
                snapshot.playbackStartFrame,
                snapshot.playbackEndFrame,
                readyStepCount,
                snapshot.loopPlayback);
        const auto drawReadySegment = [&](
                                          const ui_detail::NormalizedTimelineSegment
                                              segment) {
            if (!segment.visible) {
                return;
            }
            float segmentMinimumX = trackMinimumX + usableTrackWidth *
                static_cast<float>(segment.minimum);
            float segmentMaximumX = trackMinimumX + usableTrackWidth *
                static_cast<float>(segment.maximum);
            const float minimumSegmentWidth = Scale(2.0F);
            if (segmentMaximumX - segmentMinimumX < minimumSegmentWidth) {
                const float segmentCenter =
                    (segmentMinimumX + segmentMaximumX) * 0.5F;
                segmentMinimumX = std::max(
                    trackMinimumX,
                    segmentCenter - minimumSegmentWidth * 0.5F);
                segmentMaximumX = std::min(
                    trackMaximumX,
                    segmentMinimumX + minimumSegmentWidth);
                segmentMinimumX = std::max(
                    trackMinimumX,
                    segmentMaximumX - minimumSegmentWidth);
            }
            drawList->AddRectFilled(
                ImVec2(segmentMinimumX, centerY - Scale(2.0F)),
                ImVec2(segmentMaximumX, centerY + Scale(2.0F)),
                ImGui::GetColorU32(kColorTimelineHotCache),
                Scale(2.0F));
        };
        drawReadySegment(readySegments.first);
        drawReadySegment(readySegments.wrapped);

        drawList->AddCircleFilled(
            ImVec2(currentX, centerY),
            knobRadius,
            ImGui::GetColorU32(kColorTimelineHotCache));
        drawList->AddCircleFilled(
            ImVec2(currentX, centerY),
            std::max(Scale(2.0F), knobRadius - Scale(2.0F)),
            ImGui::GetColorU32(kColorTimelinePlayback));
        if (timelineAnimation.focus > 0.001F) {
            drawList->AddRect(
                minimum,
                maximum,
                ImGui::GetColorU32(WithAlpha(
                    ui_internal::kColorAccent,
                    timelineAnimation.focus)),
                Scale(6.0F),
                0,
                Scale(1.5F));
        }

        const auto frameAtMouseX = [&](const float mouseX) noexcept {
            const double normalized = usableTrackWidth > 0.0F
                ? static_cast<double>(
                    (mouseX - trackMinimumX) / usableTrackWidth)
                : 0.0;
            return ui_detail::FrameFromNormalizedPosition(
                normalized,
                snapshot.totalFrames);
        };

        if (ImGui::IsItemActivated()) {
            timelineScrubbing_ = true;
            timelineScrubLastMouseX_ = ImGui::GetIO().MousePos.x;
            lastTimelineRequest_ = std::numeric_limits<FrameIndex>::max();
            player.BeginScrub();
        }
        if (timelineScrubbing_ &&
            ImGui::IsItemActive() &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mousePosition = ImGui::GetIO().MousePos;
            if (ImGui::IsMousePosValid(&mousePosition)) {
                timelineScrubLastMouseX_ = mousePosition.x;
            }
            const FrameIndex requested = frameAtMouseX(
                timelineScrubLastMouseX_);
            if (requested != lastTimelineRequest_) {
                lastTimelineRequest_ = requested;
                player.UpdateScrub(requested);
            }
        }
        const bool shouldFinishTimelineScrub = timelineScrubbing_ && (
            ImGui::IsItemDeactivated() ||
            !ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
            ImGui::GetIO().AppFocusLost);
        if (shouldFinishTimelineScrub) {
            const ImVec2 mousePosition = ImGui::GetIO().MousePos;
            if (ImGui::IsMousePosValid(&mousePosition)) {
                timelineScrubLastMouseX_ = mousePosition.x;
            }
            const FrameIndex finalRequest = frameAtMouseX(
                timelineScrubLastMouseX_);
            if (finalRequest != lastTimelineRequest_) {
                lastTimelineRequest_ = finalRequest;
                player.UpdateScrub(finalRequest);
            }
            player.EndScrub();
            timelineScrubbing_ = false;
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::BeginTooltip();
            ImGui::TextColored(
                kColorTimelinePlayback,
                "播放位置：第 %lld / %zu 帧",
                currentFrameNumber,
                snapshot.totalFrames);
            ImGui::TextColored(
                kColorTimelineHotCache,
                "即时缓存：当前前方 %.1f 秒",
                readyAheadSeconds);
            ImGui::TextColored(
                kColorTimelineColdCache,
                "后台缓存：%zu / %zu 帧",
                snapshot.cachedFrames,
                snapshot.totalFrames);
            ImGui::EndTooltip();
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(
                0.0F,
                (hitHeight - ImGui::GetTextLineHeight()) * 0.5F));
        if (snapshot.hasSource) {
            const double timestampSeconds = ui_detail::FrameTimestampSeconds(
                snapshot.currentFrame,
                snapshot.targetFramesPerSecond);
            ImGui::Text(
                "第 %lld / %zu 帧  ·  %.2f 秒",
                currentFrameNumber,
                snapshot.totalFrames,
                timestampSeconds);
        } else {
            ImGui::TextColored(kColorMuted, "第 0 / 0 帧  ·  0.00 秒");
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
}

void PlayerUI::Impl::RenderPlaybackRange(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot) {
    const float rowHeight = Scale(kControlHeight);
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;

    ImGui::BeginDisabled(!snapshot.hasSource || snapshot.loading);
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (ImGui::BeginTable(
            "##PlaybackRangeLayout",
            3,
            tableFlags,
            ImVec2(-1.0F, rowHeight))) {
        ImGui::TableSetupColumn(
            "##PlaybackRangeLabel",
            ImGuiTableColumnFlags_WidthFixed,
            Scale(78.0F));
        ImGui::TableSetupColumn(
            "##PlaybackRangeTrack",
            ImGuiTableColumnFlags_WidthStretch,
            1.0F);
        ImGui::TableSetupColumn(
            "##PlaybackRangeValues",
            ImGuiTableColumnFlags_WidthFixed,
            Scale(252.0F));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);

        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(
                0.0F,
                (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
        ImGui::TextColored(kColorMuted, "播放范围");

        FrameIndex startFrame = snapshot.playbackStartFrame;
        FrameIndex endFrame = snapshot.playbackEndFrame;
        ImGui::TableSetColumnIndex(1);
        if (ui::FrameRangeSlider(
                interactionAnimator_,
                "##PlaybackRangeSlider",
                startFrame,
                endFrame,
                snapshot.totalFrames,
                ImVec2(ImGui::GetContentRegionAvail().x, rowHeight),
                uiScale_,
                playbackRangeSliderState_)) {
            player.SetPlaybackRange(startFrame, endFrame);
        }
        TooltipForLastItem(
            "仅限制连续播放；时间轴、画面拖动和方向键仍可查看完整来源");

        ImGui::TableSetColumnIndex(2);
        if (!playbackStartFrameInputEditing_) {
            playbackStartFrameInput_ = OneBasedFrameNumber(
                startFrame,
                snapshot.totalFrames);
        }
        if (!playbackEndFrameInputEditing_) {
            playbackEndFrameInput_ = OneBasedFrameNumber(
                endFrame,
                snapshot.totalFrames);
        }

        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(Scale(10.0F), Scale(10.0F)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColorMuted, "起始");
        ImGui::SameLine(0.0F, Scale(4.0F));
        const FrameNumberInputResult startInput = RenderFrameNumberInput(
            "##PlaybackStartFrameInput",
            playbackStartFrameInput_,
            Scale(76.0F),
            playbackStartFrameCommittedWhileActive_);
        TooltipForLastItem("输入起始帧；按 Enter 或移开焦点后跳转");
        playbackStartFrameInputEditing_ = startInput.active;
        if (startInput.commit && snapshot.totalFrames > 0U) {
            startFrame = ui_detail::ClampPlaybackStartInputOneBased(
                playbackStartFrameInput_,
                endFrame,
                snapshot.totalFrames);
            playbackStartFrameInput_ = OneBasedFrameNumber(
                startFrame,
                snapshot.totalFrames);
            if (!playbackEndFrameInputEditing_) {
                playbackEndFrameInput_ = OneBasedFrameNumber(
                    endFrame,
                    snapshot.totalFrames);
            }
            player.SetPlaying(false);
            player.SetPlaybackRange(startFrame, endFrame);
            player.Seek(startFrame);
        }

        ImGui::SameLine(0.0F, Scale(10.0F));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColorMuted, "结束");
        ImGui::SameLine(0.0F, Scale(4.0F));
        const FrameNumberInputResult endInput = RenderFrameNumberInput(
            "##PlaybackEndFrameInput",
            playbackEndFrameInput_,
            Scale(76.0F),
            playbackEndFrameCommittedWhileActive_);
        TooltipForLastItem("输入结束帧；按 Enter 或移开焦点后跳转");
        playbackEndFrameInputEditing_ = endInput.active;
        if (endInput.commit && snapshot.totalFrames > 0U) {
            endFrame = ui_detail::ClampPlaybackEndInputOneBased(
                playbackEndFrameInput_,
                startFrame,
                snapshot.totalFrames);
            playbackEndFrameInput_ = OneBasedFrameNumber(
                endFrame,
                snapshot.totalFrames);
            if (!playbackStartFrameInputEditing_) {
                playbackStartFrameInput_ = OneBasedFrameNumber(
                    startFrame,
                    snapshot.totalFrames);
            }
            player.SetPlaying(false);
            player.SetPlaybackRange(startFrame, endFrame);
            player.Seek(endFrame);
        }
        ImGui::PopStyleVar();
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
}

void PlayerUI::Impl::RenderPlaybackControls(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const bool comparisonEnabled,
    exporting::FfmpegExportController& exporter,
    const exporting::ExportProgressSnapshot& exportProgress,
    const OnlineUpdateView& onlineUpdateView,
    const UiActions& actions,
    const bool compact) {
    constexpr float kQuickActionsWidth = 460.0F;
    constexpr float kResourceSettingsWidth = 648.0F;
    constexpr float kResourceSettingsColumnWidth = 660.0F;
    constexpr float kPlaybackSettingsWidth = 206.0F;
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    const auto centerGroup = [this](const float logicalWidth) {
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX() +
            std::max(
                0.0F,
                (availableWidth - Scale(logicalWidth)) * 0.5F));
    };

    if (compact) {
        RenderTransportControls(player, snapshot);

        ImGui::SetCursorPosY(Scale(120.0F));
        centerGroup(kQuickActionsWidth);
        RenderQuickActions(player, snapshot, comparisonEnabled, actions);

        ImGui::SetCursorPosY(Scale(160.0F));
        centerGroup(kResourceSettingsWidth);
        RenderResourceSettings(player, snapshot);

        ImGui::SetCursorPosY(Scale(200.0F));
        ImGui::PushStyleVar(
            ImGuiStyleVar_CellPadding,
            ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
        if (ImGui::BeginTable(
                "##CompactPlaybackAndExport",
                2,
                tableFlags,
                ImVec2(-1.0F, Scale(kControlHeight)))) {
            ImGui::TableSetupColumn(
                "##CompactPlaybackSettings",
                ImGuiTableColumnFlags_WidthFixed,
                Scale(kPlaybackSettingsWidth));
            ImGui::TableSetupColumn(
                "##CompactExportControls",
                ImGuiTableColumnFlags_WidthStretch,
                1.0F);
            ImGui::TableNextRow(
                ImGuiTableRowFlags_None,
                Scale(kControlHeight));
            ImGui::TableSetColumnIndex(0);
            RenderPlaybackSettings(
                player,
                snapshot,
                IsExportBusy(exportProgress.state));
            ImGui::TableSetColumnIndex(1);
            RenderExportControls(
                player,
                snapshot,
                exporter,
                exportProgress,
                onlineUpdateView,
                actions);
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        return;
    }

    const ImVec2 primaryRowOrigin = ImGui::GetCursorPos();
    const float primaryRowWidth = ImGui::GetContentRegionAvail().x;
    RenderQuickActions(player, snapshot, comparisonEnabled, actions);

    ImGui::SetCursorPos(primaryRowOrigin);
    RenderTransportControls(player, snapshot);

    ImGui::SetCursorPos(ImVec2(
        primaryRowOrigin.x + std::max(
            0.0F,
            primaryRowWidth - Scale(kPlaybackSettingsWidth)),
        primaryRowOrigin.y));
    RenderPlaybackSettings(
        player,
        snapshot,
        IsExportBusy(exportProgress.state));

    ImGui::SetCursorPosY(Scale(120.0F));
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (ImGui::BeginTable(
            "##SecondaryControlRow",
            2,
            tableFlags,
            ImVec2(-1.0F, Scale(kControlHeight)))) {
        ImGui::TableSetupColumn(
            "##ResourceSettings",
            ImGuiTableColumnFlags_WidthFixed,
            Scale(kResourceSettingsColumnWidth));
        ImGui::TableSetupColumn(
            "##ExportControls",
            ImGuiTableColumnFlags_WidthStretch,
            1.0F);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, Scale(kControlHeight));

        ImGui::TableSetColumnIndex(0);
        centerGroup(kResourceSettingsWidth);
        RenderResourceSettings(player, snapshot);

        ImGui::TableSetColumnIndex(1);
        RenderExportControls(
            player,
            snapshot,
            exporter,
            exportProgress,
            onlineUpdateView,
            actions);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void PlayerUI::Impl::RenderTransportControls(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot) {
    constexpr float kTransportGroupWidth = 360.0F;
    const float contentMinimumX = ImGui::GetWindowContentRegionMin().x;
    const float contentMaximumX = ImGui::GetWindowContentRegionMax().x;
    ImGui::SetCursorPosX(
        contentMinimumX +
        std::max(
            0.0F,
            ((contentMaximumX - contentMinimumX) -
                Scale(kTransportGroupWidth)) * 0.5F));

    ImGui::BeginDisabled(!snapshot.hasSource || snapshot.loading);
    if (AnimatedButton(
            interactionAnimator_,
            "首帧",
            ImVec2(Scale(64.0F), Scale(kControlHeight)))) {
        player.SetPlaying(false);
        player.Seek(snapshot.playbackStartFrame);
    }
    TooltipForLastItem("跳到当前播放范围的起始帧");
    ImGui::SameLine();
    if (AnimatedButton(
            interactionAnimator_,
            "上一帧",
            ImVec2(Scale(68.0F), Scale(kControlHeight)))) {
        player.SetPlaying(false);
        player.StepFrame(-1);
    }
    ImGui::SameLine();
    if (PlaybackButton(
            interactionAnimator_,
            snapshot.playing ? "暂停###PlayToggle" : "播放###PlayToggle",
            ImVec2(Scale(64.0F), Scale(kControlHeight)),
            snapshot.playing)) {
        player.TogglePlayback();
    }
    ImGui::SameLine();
    if (AnimatedButton(
            interactionAnimator_,
            "下一帧",
            ImVec2(Scale(68.0F), Scale(kControlHeight)))) {
        player.SetPlaying(false);
        player.StepFrame(1);
    }
    ImGui::SameLine();
    if (AnimatedButton(
            interactionAnimator_,
            "末帧",
            ImVec2(Scale(64.0F), Scale(kControlHeight)))) {
        player.SetPlaying(false);
        player.Seek(snapshot.playbackEndFrame);
    }
    TooltipForLastItem("跳到当前播放范围的结束帧");
    ImGui::EndDisabled();
}

void PlayerUI::Impl::RenderPlaybackSettings(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const bool exportBusy) {
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(
            0.0F,
            (Scale(kControlHeight) - ImGui::GetFrameHeight()) * 0.5F));
    ImGui::BeginDisabled(exportBusy);
    ImGui::SetNextItemWidth(Scale(112.0F));
    if (ImGui::DragFloat(
            "##TargetFps",
            &framesPerSecond_,
            1.0F,
            1.0F,
            120.0F,
            "%.0f FPS",
            ImGuiSliderFlags_AlwaysClamp)) {
        player.SetFramesPerSecond(static_cast<double>(framesPerSecond_));
    }
    TooltipForLastItem("目标播放帧率，范围 1–120 FPS");
    ImGui::EndDisabled();
    ImGui::SameLine(0.0F, Scale(14.0F));
    bool loopPlayback = snapshot.loopPlayback;
    if (AnimatedCheckbox(
            interactionAnimator_,
            "循环",
            &loopPlayback,
            Scale(kControlHeight))) {
        player.SetLoopPlayback(loopPlayback);
    }
}

void PlayerUI::Impl::RenderExportControls(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    exporting::FfmpegExportController& exporter,
    const exporting::ExportProgressSnapshot& exportProgress,
    const OnlineUpdateView& onlineUpdateView,
    const UiActions& actions) {
    constexpr float kExportGroupWidth = 588.0F;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float groupWidth = std::min(availableWidth, Scale(kExportGroupWidth));
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        std::max(0.0F, availableWidth - groupWidth));

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (!ImGui::BeginTable(
            "##ExportControlGroup",
            5,
            tableFlags,
            ImVec2(groupWidth, Scale(kControlHeight)))) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn(
        "##ExportFolderButton",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(96.0F));
    ImGui::TableSetupColumn(
        "##ExportFolderValue",
        ImGuiTableColumnFlags_WidthStretch,
        1.0F);
    ImGui::TableSetupColumn(
        "##ExportStartButton",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(104.0F));
    ImGui::TableSetupColumn(
        "##OpenExportButton",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(104.0F));
    ImGui::TableSetupColumn(
        "##OnlineUpdateButton",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(120.0F));
    ImGui::TableNextRow(ImGuiTableRowFlags_None, Scale(kControlHeight));

    const bool exportBusy = IsExportBusy(exportProgress.state);
    const bool canExportMedia = IsExportableSourceKind(snapshot.sourceKind);
    const bool exportingVideo = snapshot.sourceKind == SourceKind::Video;
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginDisabled(exportBusy || !actions.chooseExportFolder);
    if (AnimatedButton(
            interactionAnimator_,
            "导出位置",
            ImVec2(Scale(88.0F), Scale(kControlHeight)))) {
        ChooseExportFolder(actions);
    }
    ImGui::EndDisabled();
    const std::string folderTooltip = !canExportMedia
        ? "请先打开 PNG 序列或视频"
        : exportFolder_.has_value()
        ? WideToUtf8(exportFolder_->wstring())
        : exportingVideo
        ? "未设置自定义位置；导出时使用当前视频所在文件夹"
        : "未设置自定义位置；导出时使用当前 PNG 文件夹";
    TooltipForLastItem(folderTooltip.c_str());

    ImGui::TableSetColumnIndex(1);
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(
            0.0F,
            (Scale(kControlHeight) - ImGui::GetTextLineHeight()) * 0.5F));
    const std::string folderLabel = !canExportMedia
        ? "等待来源"
        : exportFolder_.has_value()
        ? PathDisplayName(*exportFolder_)
        : exportingVideo
        ? "视频同目录"
        : "PNG 同目录";
    EllipsizedText(
        folderLabel.c_str(),
        ImGui::GetContentRegionAvail().x,
        kColorMuted);
    TooltipForLastItem(folderTooltip.c_str());

    ImGui::TableSetColumnIndex(2);
    ImGui::BeginDisabled(
        exportBusy || !canExportMedia || snapshot.loading);
    if (PrimaryButton(
            interactionAnimator_,
            "导出 MP4",
            ImVec2(Scale(96.0F), Scale(kControlHeight)))) {
        StartExport(player, exporter);
    }
    ImGui::EndDisabled();
    TooltipForLastItem(!canExportMedia
        ? "请先打开 PNG 序列或视频"
        : exportingVideo
        ? "裁剪原始视频，并继承当前遮罩、播放范围和 FPS；预览解码比例不影响导出"
        : "使用原始 PNG，并继承当前遮罩、播放范围和 FPS；预览解码比例不影响导出");

    const bool hasSuccessfulExport = !lastSuccessfulExportPath_.empty();
    const bool exportedFileExists = IsLastExportedVideoAvailable();
    const bool canOpenVideo = ui_detail::CanOpenLastExportedVideo(
        static_cast<bool>(actions.openFile),
        hasSuccessfulExport,
        exportedFileExists);
    const std::string videoPathUtf8 = hasSuccessfulExport
        ? WideToUtf8(lastSuccessfulExportPath_.wstring())
        : std::string{};
    std::string openTooltip;
    if (!hasSuccessfulExport) {
        openTooltip = "完成一次 MP4 导出后可直接打开视频";
    } else if (!exportedFileExists) {
        openTooltip = "最近导出的视频已被移动或删除 · " + videoPathUtf8;
    } else if (!actions.openFile) {
        openTooltip = "当前应用无法调用 Windows 默认播放器";
    } else if (exportBusy) {
        openTooltip = "新视频导出中；打开上一次成功导出的视频 · " +
            videoPathUtf8;
    } else {
        openTooltip = "使用 Windows 默认播放器打开 · " + videoPathUtf8;
    }

    ImGui::TableSetColumnIndex(3);
    ImGui::BeginDisabled(!canOpenVideo);
    if (AnimatedButton(
            interactionAnimator_,
            "打开视频",
            ImVec2(Scale(96.0F), Scale(kControlHeight)))) {
        OpenLastExportedVideo(actions);
    }
    ImGui::EndDisabled();
    TooltipForLastItem(openTooltip.c_str());

    const OnlineUpdatePhase updatePhase = onlineUpdateView.phase;
    const bool installReady =
        updatePhase == OnlineUpdatePhase::ReadyToInstall;
    const bool hasUpdateAction = installReady
        ? static_cast<bool>(actions.installOnlineUpdate)
        : static_cast<bool>(actions.checkForUpdates);
    const bool updateDisabled =
        updatePhase == OnlineUpdatePhase::Unavailable ||
        IsOnlineUpdateBusy(updatePhase) ||
        !hasUpdateAction ||
        (installReady && exportBusy);

    std::string updateTooltip;
    if (updatePhase == OnlineUpdatePhase::Unavailable) {
        updateTooltip = "当前版本未启用在线更新";
    } else if (updatePhase == OnlineUpdatePhase::Checking) {
        updateTooltip = "正在检查最新版本";
    } else if (updatePhase == OnlineUpdatePhase::Downloading) {
        updateTooltip = "正在下载并校验更新";
    } else if (installReady && exportBusy) {
        updateTooltip = "导出进行中；导出完成后可重启并更新";
    } else if (installReady) {
        updateTooltip = "关闭播放器、安装已下载的新版本并自动重启";
    } else if (!hasUpdateAction) {
        updateTooltip = "当前应用没有提供在线更新操作";
    } else if (updatePhase == OnlineUpdatePhase::Failed &&
        !onlineUpdateView.statusUtf8.empty()) {
        updateTooltip = onlineUpdateView.statusUtf8;
    } else {
        const std::string currentVersion =
            DisplayVersion(onlineUpdateView.currentVersion);
        updateTooltip = currentVersion.empty()
            ? "检查最新版本"
            : "当前版本 " + currentVersion + "；检查最新版本";
    }

    ImGui::TableSetColumnIndex(4);
    ImGui::BeginDisabled(updateDisabled);
    const char* const updateButtonLabel =
        OnlineUpdateButtonLabel(updatePhase);
    const bool updatePressed = installReady
        ? PrimaryButton(
            interactionAnimator_,
            updateButtonLabel,
            ImVec2(Scale(112.0F), Scale(kControlHeight)))
        : AnimatedButton(
            interactionAnimator_,
            updateButtonLabel,
            ImVec2(Scale(112.0F), Scale(kControlHeight)));
    if (updatePressed) {
        if (installReady) {
            actions.installOnlineUpdate();
        } else {
            actions.checkForUpdates();
        }
    }
    ImGui::EndDisabled();
    TooltipForLastItem(updateTooltip.c_str());

    ImGui::EndTable();
    ImGui::PopStyleVar();
}

bool PlayerUI::Impl::IsLastExportedVideoAvailable() const {
    return !lastSuccessfulExportPath_.empty() &&
        lastSuccessfulExportAvailable_;
}

void PlayerUI::Impl::OpenLastExportedVideo(const UiActions& actions) {
    if (!actions.openFile || lastSuccessfulExportPath_.empty()) {
        SetLocalError(
            "无法打开视频",
            "当前没有可交给系统默认播放器的视频文件。",
            LocalErrorKind::ExternalFile);
        return;
    }

    const platform::FileLaunchResult result =
        actions.openFile(lastSuccessfulExportPath_);
    if (!result.succeeded) {
        std::error_code fileError;
        lastSuccessfulExportAvailable_ =
            std::filesystem::is_regular_file(
                lastSuccessfulExportPath_,
                fileError) &&
            !fileError;
        SetLocalError(
            "无法打开视频",
            result.errorUtf8.empty()
                ? "Windows 默认播放器没有接受该视频文件。"
                : result.errorUtf8,
            LocalErrorKind::ExternalFile);
        return;
    }
    lastSuccessfulExportAvailable_ = true;
    if (localErrorKind_ == LocalErrorKind::ExternalFile) {
        ClearLocalError();
    }
}

void PlayerUI::Impl::ChooseExportFolder(const UiActions& actions) {
    if (!actions.chooseExportFolder) {
        exportUiError_ = "当前应用没有提供导出文件夹选择器";
        return;
    }
    const std::optional<std::filesystem::path> selected =
        actions.chooseExportFolder();
    if (!selected.has_value()) {
        return;
    }

    exportFolder_ = *selected;
    exportUiError_.clear();
    if (!user_settings::SaveExportFolder(*selected)) {
        exportUiError_ = "导出位置可用于本次运行，但无法保存到用户设置";
    }
}

void PlayerUI::Impl::StartExport(
    ComparisonPlayer& player,
    exporting::FfmpegExportController& exporter) {
    exportUiError_.clear();
    std::optional<ExportSourceSnapshot> source =
        player.CaptureExportSnapshot();
    if (!source.has_value()) {
        exportUiError_ = "当前媒体尚未准备好，无法开始导出";
        return;
    }

    exporting::FfmpegExportRequest request;
    request.outputFolder = exportFolder_.value_or(
        DefaultExportFolder(*source));
    request.crop = CurrentExportCrop(player.Snapshot());
    request.source = std::move(*source);
    if (!exporter.Start(std::move(request))) {
        const exporting::ExportProgressSnapshot progress = exporter.Snapshot();
        exportUiError_ = !progress.errorUtf8.empty()
            ? progress.errorUtf8
            : "导出任务未能启动";
        return;
    }
    if (localErrorKind_ == LocalErrorKind::ExternalFile) {
        ClearLocalError();
    }
}

exporting::NormalizedCrop PlayerUI::Impl::CurrentExportCrop(
    const ComparisonPlayerSnapshot& snapshot) const noexcept {
    const ui::NormalizedMaskOpening opening =
        ui::MaskOpeningForPreset(maskPreset_);
    if (snapshot.enabled) {
        const ui_detail::ComparisonCanvasLayout layout =
            ui_detail::CalculateComparisonCanvasLayout(
                snapshot.primary.sourceWidth,
                snapshot.primary.sourceHeight,
                opening);
        if (layout.hasVisibleContent) {
            return {
                static_cast<double>(layout.sourceUv.minimumX),
                static_cast<double>(layout.sourceUv.minimumY),
                static_cast<double>(layout.sourceUv.maximumX),
                static_cast<double>(layout.sourceUv.maximumY),
            };
        }
    }
    return {
        static_cast<double>(opening.minimumX),
        static_cast<double>(opening.minimumY),
        static_cast<double>(opening.maximumX),
        static_cast<double>(opening.maximumY),
    };
}

void PlayerUI::Impl::RenderExportProgress(
    const exporting::ExportProgressSnapshot& exportProgress) {
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    const float rowHeight = Scale(20.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (!ImGui::BeginTable(
            "##ExportProgressLayout",
            3,
            tableFlags,
            ImVec2(-1.0F, rowHeight))) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn(
        "##ExportProgressStatus",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(176.0F));
    ImGui::TableSetupColumn(
        "##ExportProgressBar",
        ImGuiTableColumnFlags_WidthStretch,
        1.0F);
    ImGui::TableSetupColumn(
        "##ExportProgressValue",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(164.0F));
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);

    ImGui::TableSetColumnIndex(0);
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(0.0F, (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
    EllipsizedText(
        ExportProgressLabel(exportProgress),
        ImGui::GetContentRegionAvail().x,
        kColorMuted);

    const double normalizedProgress = exportProgress.totalFrames > 0U
        ? std::clamp(
            static_cast<double>(exportProgress.completedFrames) /
                static_cast<double>(exportProgress.totalFrames),
            0.0,
            1.0)
        : 0.0;
    ImGui::TableSetColumnIndex(1);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, rowHeight));
    const ImVec2 barItemMinimum = ImGui::GetItemRectMin();
    const ImVec2 barItemMaximum = ImGui::GetItemRectMax();
    const float barCenterY = (barItemMinimum.y + barItemMaximum.y) * 0.5F;
    const ImVec2 barMinimum{barItemMinimum.x, barCenterY - Scale(4.0F)};
    const ImVec2 barMaximum{barItemMaximum.x, barCenterY + Scale(4.0F)};
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        barMinimum,
        barMaximum,
        ImGui::GetColorU32(kColorSurfaceActive),
        Scale(4.0F));
    drawList->AddRectFilled(
        barMinimum,
        ImVec2(
            barMinimum.x +
                ((barMaximum.x - barMinimum.x) *
                    static_cast<float>(normalizedProgress)),
            barMaximum.y),
        ImGui::GetColorU32(kColorExportProgress),
        Scale(4.0F));

    ImGui::TableSetColumnIndex(2);
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(0.0F, (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
    std::ostringstream value;
    if (exportProgress.totalFrames > 0U) {
        value << exportProgress.completedFrames << " / "
              << exportProgress.totalFrames << " 帧 · "
              << std::fixed << std::setprecision(0)
              << normalizedProgress * 100.0 << '%';
    } else {
        value << "准备中";
    }
    EllipsizedText(
        value.str().c_str(),
        ImGui::GetContentRegionAvail().x,
        kColorMuted);
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void PlayerUI::Impl::RenderOnlineUpdateStatus(
    const OnlineUpdateView& onlineUpdateView) {
    if (onlineUpdateView.phase != OnlineUpdatePhase::Downloading) {
        std::string status;
        ImVec4 color = kColorMuted;
        switch (onlineUpdateView.phase) {
        case OnlineUpdatePhase::Checking:
            status = onlineUpdateView.statusUtf8.empty()
                ? "正在检查更新"
                : onlineUpdateView.statusUtf8;
            break;
        case OnlineUpdatePhase::UpToDate: {
            const std::string version = DisplayVersion(
                onlineUpdateView.latestVersion.empty()
                    ? onlineUpdateView.currentVersion
                    : onlineUpdateView.latestVersion);
            status = onlineUpdateView.statusUtf8.empty()
                ? (version.empty()
                    ? "已是最新版本"
                    : "已是最新版本 · " + version)
                : onlineUpdateView.statusUtf8;
            color = kColorPrimaryHover;
            break;
        }
        case OnlineUpdatePhase::ReadyToInstall: {
            const std::string version =
                DisplayVersion(onlineUpdateView.latestVersion);
            status = onlineUpdateView.statusUtf8.empty()
                ? (version.empty()
                    ? "更新已下载 · 可重启并更新"
                    : "更新已下载 · " + version + " · 可重启并更新")
                : onlineUpdateView.statusUtf8;
            color = kColorPrimaryHover;
            break;
        }
        case OnlineUpdatePhase::Failed:
            status = onlineUpdateView.statusUtf8.empty()
                ? "更新失败 · 请重试"
                : "更新失败 · " + onlineUpdateView.statusUtf8;
            color = kColorDanger;
            break;
        case OnlineUpdatePhase::Unavailable:
        case OnlineUpdatePhase::Idle:
        case OnlineUpdatePhase::Downloading:
            return;
        }
        EllipsizedText(
            status.c_str(),
            ImGui::GetContentRegionAvail().x,
            color);
        TooltipForLastItem(status.c_str());
        return;
    }

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    const float rowHeight = Scale(20.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (!ImGui::BeginTable(
            "##OnlineUpdateProgressLayout",
            3,
            tableFlags,
            ImVec2(-1.0F, rowHeight))) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::TableSetupColumn(
        "##OnlineUpdateProgressStatus",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(176.0F));
    ImGui::TableSetupColumn(
        "##OnlineUpdateProgressBar",
        ImGuiTableColumnFlags_WidthStretch,
        1.0F);
    ImGui::TableSetupColumn(
        "##OnlineUpdateProgressValue",
        ImGuiTableColumnFlags_WidthFixed,
        Scale(200.0F));
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);

    const std::string status = onlineUpdateView.statusUtf8.empty()
        ? "正在下载更新"
        : onlineUpdateView.statusUtf8;
    ImGui::TableSetColumnIndex(0);
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(0.0F, (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
    EllipsizedText(
        status.c_str(),
        ImGui::GetContentRegionAvail().x,
        kColorMuted);
    TooltipForLastItem(status.c_str());

    const double normalizedProgress = onlineUpdateView.totalBytes > 0U
        ? std::clamp(
            static_cast<double>(onlineUpdateView.downloadedBytes) /
                static_cast<double>(onlineUpdateView.totalBytes),
            0.0,
            1.0)
        : 0.0;
    ImGui::TableSetColumnIndex(1);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, rowHeight));
    const ImVec2 barItemMinimum = ImGui::GetItemRectMin();
    const ImVec2 barItemMaximum = ImGui::GetItemRectMax();
    const float barCenterY = (barItemMinimum.y + barItemMaximum.y) * 0.5F;
    const ImVec2 barMinimum{barItemMinimum.x, barCenterY - Scale(4.0F)};
    const ImVec2 barMaximum{barItemMaximum.x, barCenterY + Scale(4.0F)};
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        barMinimum,
        barMaximum,
        ImGui::GetColorU32(kColorSurfaceActive),
        Scale(4.0F));
    drawList->AddRectFilled(
        barMinimum,
        ImVec2(
            barMinimum.x +
                ((barMaximum.x - barMinimum.x) *
                    static_cast<float>(normalizedProgress)),
            barMaximum.y),
        ImGui::GetColorU32(kColorExportProgress),
        Scale(4.0F));

    ImGui::TableSetColumnIndex(2);
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(0.0F, (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
    std::string progressValue = FormatDownloadBytes(
        onlineUpdateView.downloadedBytes);
    if (onlineUpdateView.totalBytes > 0U) {
        std::ostringstream value;
        value << progressValue << " / "
              << FormatDownloadBytes(onlineUpdateView.totalBytes) << " · "
              << std::fixed << std::setprecision(0)
              << normalizedProgress * 100.0 << '%';
        progressValue = value.str();
    }
    EllipsizedText(
        progressValue.c_str(),
        ImGui::GetContentRegionAvail().x,
        kColorMuted);
    TooltipForLastItem(progressValue.c_str());
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

void PlayerUI::Impl::RenderStatusLine(
    const PlayerSnapshot& snapshot,
    const exporting::ExportProgressSnapshot& exportProgress,
    const OnlineUpdateView& onlineUpdateView,
    const ErrorView& error) {
    if (error.visible) {
        ImGui::TextColored(kColorDanger, "错误");
        ImGui::SameLine();
        ImGui::TextUnformatted(error.headline.c_str());
        TooltipForLastItem(error.detail.c_str());
        ImGui::SameLine(0.0F, Scale(12.0F));
        if (AnimatedButton(
                interactionAnimator_,
                errorDetailsExpanded_
                    ? "收起详情###ErrorDetails"
                    : "查看详情###ErrorDetails",
                ImVec2(Scale(80.0F), Scale(28.0F)),
                AnimatedButtonStyle::Ghost)) {
            errorDetailsExpanded_ = !errorDetailsExpanded_;
        }
        ImGui::SameLine();
        if (AnimatedButton(
                interactionAnimator_,
                "清除##ClearError",
                ImVec2(Scale(48.0F), Scale(28.0F)),
                AnimatedButtonStyle::Ghost)) {
            if (error.fromEngine) {
                dismissedEngineError_ = error.detail;
                decodeChangeFailed_ = false;
            }
            ClearLocalError();
            errorDetailsExpanded_ = false;
        }
        if (errorDetailsExpanded_) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            ImGui::TextUnformatted(error.detail.c_str());
            ImGui::PopTextWrapPos();
        }
        return;
    }

    errorDetailsExpanded_ = false;
    if (IsExportBusy(exportProgress.state)) {
        RenderExportProgress(exportProgress);
        return;
    }

    if (exportProgress.state == exporting::ExportState::Failed) {
        const std::string detail = !exportProgress.errorUtf8.empty()
            ? exportProgress.errorUtf8
            : (!exportProgress.statusUtf8.empty()
                ? exportProgress.statusUtf8
                : "未知导出错误");
        const std::string failed = "导出失败 · " + detail;
        EllipsizedText(
            failed.c_str(),
            ImGui::GetContentRegionAvail().x,
            kColorDanger);
        TooltipForLastItem(failed.c_str());
        return;
    }

    if (!exportUiError_.empty()) {
        const std::string failed = "导出设置 · " + exportUiError_;
        EllipsizedText(
            failed.c_str(),
            ImGui::GetContentRegionAvail().x,
            kColorDanger);
        TooltipForLastItem(failed.c_str());
        return;
    }

    if (HasVisibleOnlineUpdateStatus(onlineUpdateView.phase)) {
        RenderOnlineUpdateStatus(onlineUpdateView);
        return;
    }

    if (exportProgress.state == exporting::ExportState::Completed) {
        const std::string outputPath =
            WideToUtf8(exportProgress.finalOutputPath.wstring());
        const std::string completed = outputPath.empty()
            ? "导出完成"
            : "导出完成 · " + outputPath;
        EllipsizedText(
            completed.c_str(),
            ImGui::GetContentRegionAvail().x,
            kColorPrimaryHover);
        TooltipForLastItem(completed.c_str());
        return;
    }

    if (exportProgress.state == exporting::ExportState::Cancelled) {
        EllipsizedText(
            "导出已取消",
            ImGui::GetContentRegionAvail().x,
            kColorMuted);
        return;
    }

    const std::string performance = PerformanceSummary(snapshot);
    const char* status = !snapshot.statusUtf8.empty()
        ? snapshot.statusUtf8.c_str()
        : (snapshot.hasSource ? snapshot.currentFileUtf8.c_str() : "就绪");
    const std::string cache = CacheSummary(snapshot);
    const float logicalWidth =
        ImGui::GetContentRegionAvail().x / std::max(uiScale_, 0.5F);
    const bool compact = logicalWidth < 1100.0F;
    const std::string visibleCache = compact
        ? CompactCacheSummary(snapshot)
        : cache;
    const char* cacheState = snapshot.buffering
        ? "缓冲中"
        : (snapshot.hasSource && snapshot.cacheProgress >= 0.999
            ? "缓存完成"
            : (snapshot.hasSource ? "缓存中" : "等待来源"));
    const std::string cacheLine =
        std::string(cacheState) + " · " + visibleCache;
    const ImVec4 cacheColor = snapshot.buffering
        ? kColorWarning
        : (snapshot.hasSource && snapshot.cacheProgress >= 0.999
            ? kColorPrimaryHover
            : kColorMuted);
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_NoPadOuterX;
    const float rowHeight = Scale(20.0F);
    const float performanceWidth = std::max(
        Scale(compact ? 220.0F : 230.0F),
        ImGui::CalcTextSize(performance.c_str()).x + Scale(8.0F));
    const float cacheWidth = Scale(compact ? 270.0F : 430.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, 0.0F));
    if (ImGui::BeginTable(
            "##StatusLineLayout",
            3,
            tableFlags,
            ImVec2(-1.0F, rowHeight))) {
        ImGui::TableSetupColumn(
            "##StatusText",
            ImGuiTableColumnFlags_WidthStretch,
            1.0F);
        ImGui::TableSetupColumn(
            "##CacheText",
            ImGuiTableColumnFlags_WidthFixed,
            cacheWidth);
        ImGui::TableSetupColumn(
            "##PerformanceText",
            ImGuiTableColumnFlags_WidthFixed,
            performanceWidth);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
        ImGui::TableSetColumnIndex(0);
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(
                0.0F,
                (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
        EllipsizedText(
            status,
            ImGui::GetContentRegionAvail().x,
            kColorMuted);
        TooltipForLastItem(status);

        ImGui::TableSetColumnIndex(1);
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(
                0.0F,
                (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
        EllipsizedText(
            cacheLine.c_str(),
            ImGui::GetContentRegionAvail().x,
            cacheColor);
        TooltipForLastItem(cache.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::SetCursorPosY(
            ImGui::GetCursorPosY() +
            std::max(
                0.0F,
                (rowHeight - ImGui::GetTextLineHeight()) * 0.5F));
        ImGui::TextColored(kColorMuted, "%s", performance.c_str());
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

}  // namespace zt::sequence
