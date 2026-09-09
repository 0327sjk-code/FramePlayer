#include "UI/PlayerUIInternal.h"

#include "Core/ComparisonPlayer.h"
#include "Export/FfmpegExportController.h"
#include "Overlay/MaskOverlayTexture.h"
#include "Platform/UserSettings.h"
#include "Render/FrameTexture.h"
#include "UI/PlayerUILogic.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace zt::sequence {

using ui_internal::DecodeDescription;
using ui_internal::kCompactBottomBarThreshold;
using ui_internal::kExpandedErrorHeight;
using ui_internal::kTopBarHeight;
using ui_internal::RoundedMemoryGiB;

void PlayerUI::Impl::SetUiScale(const float scale) noexcept {
    uiScale_ = std::clamp(scale, 0.5F, 4.0F);
}

void PlayerUI::Impl::ReportRendererError(const std::string_view detail) {
    SetLocalError(
        "Direct3D 渲染失败",
        detail.empty() ? "Direct3D 运行时发生未知错误。" : std::string(detail),
        LocalErrorKind::Renderer);
}

void PlayerUI::Impl::Render(
    ComparisonPlayer& player,
    FrameTexture& primaryFrameTexture,
    FrameTexture& secondaryFrameTexture,
    overlay::MaskOverlayTexture& maskOverlayTexture,
    exporting::FfmpegExportController& exporter,
    const UiActions& actions) {
    interactionAnimator_.BeginFrame(ImGui::GetIO().DeltaTime);
    ComparisonPlayerSnapshot comparisonSnapshot = player.Snapshot();
    PlayerSnapshot snapshot = comparisonSnapshot.primary;
    if (comparisonSnapshot.enabled) {
        snapshot.decodePercent = comparisonSnapshot.decodePercent;
    }
    if (comparisonSnapshot.active) {
        snapshot.playing = comparisonSnapshot.playing;
        snapshot.buffering = comparisonSnapshot.buffering;
        snapshot.loopPlayback = comparisonSnapshot.loopPlayback;
        snapshot.currentFrame = comparisonSnapshot.currentFrame;
        snapshot.requestedFrame = comparisonSnapshot.requestedFrame;
        snapshot.playbackStartFrame = comparisonSnapshot.playbackStartFrame;
        snapshot.playbackEndFrame = comparisonSnapshot.playbackEndFrame;
        snapshot.totalFrames = comparisonSnapshot.totalFrames;
        snapshot.targetFramesPerSecond =
            comparisonSnapshot.targetFramesPerSecond;
        snapshot.actualFramesPerSecond =
            comparisonSnapshot.actualFramesPerSecond;
        snapshot.memoryLimitBytes = comparisonSnapshot.memoryLimitBytes;
        snapshot.cacheBytes = comparisonSnapshot.cacheBytes;
        snapshot.statusUtf8 = comparisonSnapshot.statusUtf8;
        snapshot.errorUtf8 = comparisonSnapshot.errorUtf8;
    }
    SynchronizeControls(comparisonSnapshot);
    SynchronizeMaskOverlaySettings(maskOverlayTexture);

    PlayerSnapshot primaryViewportSnapshot = comparisonSnapshot.primary;
    PlayerSnapshot secondaryViewportSnapshot = comparisonSnapshot.secondary;
    if (comparisonSnapshot.enabled) {
        primaryViewportSnapshot.decodePercent = comparisonSnapshot.decodePercent;
        secondaryViewportSnapshot.decodePercent = comparisonSnapshot.decodePercent;
    }
    const FrameTextureUploadDomain uploadDomain = comparisonSnapshot.active
        ? FrameTextureUploadDomain::ComparisonPair
        : FrameTextureUploadDomain::PlayerEngine;
    if (comparisonSnapshot.active) {
        primaryViewportSnapshot.currentFrame = comparisonSnapshot.currentFrame;
        primaryViewportSnapshot.requestedFrame = comparisonSnapshot.requestedFrame;
        primaryViewportSnapshot.playing = comparisonSnapshot.playing;
        primaryViewportSnapshot.buffering = comparisonSnapshot.buffering;
        primaryViewportSnapshot.displayRevision = comparisonSnapshot.pairRevision;
        primaryViewportSnapshot.displayFrame =
            comparisonSnapshot.primaryDisplayFrame;

        secondaryViewportSnapshot.currentFrame = comparisonSnapshot.currentFrame;
        secondaryViewportSnapshot.requestedFrame = comparisonSnapshot.requestedFrame;
        secondaryViewportSnapshot.playing = comparisonSnapshot.playing;
        secondaryViewportSnapshot.buffering = comparisonSnapshot.buffering;
        secondaryViewportSnapshot.displayRevision = comparisonSnapshot.pairRevision;
        secondaryViewportSnapshot.displayFrame =
            comparisonSnapshot.secondaryDisplayFrame;
    }
    UploadDisplayFrame(
        primaryViewportSnapshot,
        primaryFrameTexture,
        ViewportPane::Primary,
        uploadDomain);
    if (comparisonSnapshot.enabled) {
        UploadDisplayFrame(
            secondaryViewportSnapshot,
            secondaryFrameTexture,
            ViewportPane::Secondary,
            uploadDomain);
        if (comparisonSnapshot.active && comparisonSnapshot.pairReady &&
            !comparisonSnapshot.secondaryFrameAvailable) {
            secondaryFrameTexture.ClearFrame();
            PaneState(ViewportPane::Secondary).textureWasVisible = false;
        }
        if (comparisonSnapshot.active && comparisonSnapshot.pairReady &&
            !comparisonSnapshot.primaryFrameAvailable) {
            primaryFrameTexture.ClearFrame();
            PaneState(ViewportPane::Primary).textureWasVisible = false;
        }
    } else {
        secondaryFrameTexture.ClearFrame();
        PaneState(ViewportPane::Secondary).textureWasVisible = false;
    }
    const exporting::ExportProgressSnapshot exportProgress = exporter.Snapshot();
    SynchronizeExportResult(exportProgress);
    const OnlineUpdateView onlineUpdateView = actions.getOnlineUpdateView
        ? actions.getOnlineUpdateView()
        : OnlineUpdateView{};

    const ErrorView error = CurrentError(snapshot);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool showSequenceFrameOffset =
        ui_detail::ShouldShowComparisonSequenceFrameOffset(
            comparisonSnapshot.active,
            comparisonSnapshot.sequenceFrameOffsetAvailable);
    const float bottomHeight = BottomBarHeightForWidth(
        viewport->WorkSize.x,
        showSequenceFrameOffset) +
        ((error.visible && errorDetailsExpanded_)
            ? Scale(kExpandedErrorHeight)
            : 0.0F);

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    if (ImGui::Begin("##NativeSequencePlayer", nullptr, rootFlags)) {
        ImGui::SetCursorPos(ImVec2(0.0F, 0.0F));
        RenderTopBar(snapshot);

        const float topBarHeight = TopBarHeightForWidth(ImGui::GetWindowSize().x);
        const float viewportHeight = std::max(
            Scale(120.0F),
            ImGui::GetWindowSize().y - topBarHeight - bottomHeight);
        ImGui::SetCursorPos(ImVec2(0.0F, topBarHeight));
        RenderViewport(
            player,
            comparisonSnapshot,
            primaryFrameTexture,
            secondaryFrameTexture,
            maskOverlayTexture,
            viewportHeight);
        ImGui::SetCursorPos(ImVec2(0.0F, topBarHeight + viewportHeight));
        RenderBottomBar(
            player,
            snapshot,
            comparisonSnapshot,
            exporter,
            exportProgress,
            onlineUpdateView,
            actions,
            error,
            maskOverlayTexture,
            bottomHeight);
        HandleKeyboard(player, snapshot, comparisonSnapshot);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    if (keyboardShuttleSpeedPersistPending_) {
        keyboardShuttleSpeedPercent_ =
            ui_detail::ClampKeyboardShuttleSpeedPercent(
                keyboardShuttleSpeedPercent_);
        keyboardShuttleSpeedPersistPending_ = false;
        static_cast<void>(user_settings::SaveKeyboardShuttleSpeedPercent(
            keyboardShuttleSpeedPercent_));
    }
}

void PlayerUI::Impl::ResetViewportView(const ViewportPane pane) noexcept {
    ViewportPaneState& state = PaneState(pane);
    state.transform = {};
    state.panOriginTransform = {};
    state.panOriginMouse = {};
    state.panning = false;
    viewportScrubbing_ = false;
    viewportScrubArmed_ = false;
}

PlayerUI::Impl::ViewportPaneState& PlayerUI::Impl::PaneState(
    const ViewportPane pane) noexcept {
    return viewportPanes_[static_cast<std::size_t>(pane)];
}

const PlayerUI::Impl::ViewportPaneState& PlayerUI::Impl::PaneState(
    const ViewportPane pane) const noexcept {
    return viewportPanes_[static_cast<std::size_t>(pane)];
}

bool PlayerUI::Impl::IsSecondaryViewportAtClientPoint(
    const std::int32_t clientX,
    const std::int32_t clientY) const noexcept {
    if (!secondaryViewportRect_.valid) {
        return false;
    }
    return ui_detail::ClientPointInsideRect(
        clientX,
        clientY,
        secondaryViewportRect_.minimumX,
        secondaryViewportRect_.minimumY,
        secondaryViewportRect_.maximumX,
        secondaryViewportRect_.maximumY);
}

void PlayerUI::Impl::SynchronizeExportResult(
    const exporting::ExportProgressSnapshot& exportProgress) {
    if (exportProgress.state != exporting::ExportState::Completed ||
        exportProgress.finalOutputPath.empty() ||
        (exportProgress.jobId == lastSuccessfulExportJobId_ &&
            exportProgress.finalOutputPath == lastSuccessfulExportPath_)) {
        return;
    }

    lastSuccessfulExportJobId_ = exportProgress.jobId;
    lastSuccessfulExportPath_ = exportProgress.finalOutputPath;
    lastSuccessfulExportAvailable_ = true;
    if (localErrorKind_ == LocalErrorKind::ExternalFile) {
        ClearLocalError();
    }
}

float PlayerUI::Impl::Scale(const float value) const noexcept {
    return value * uiScale_;
}

float PlayerUI::Impl::TopBarHeightForWidth(
    const float /*physicalWidth*/) const noexcept {
    return Scale(kTopBarHeight);
}

float PlayerUI::Impl::BottomBarHeightForWidth(
    const float physicalWidth,
    const bool showSequenceFrameOffset) const noexcept {
    const float safeScale = std::max(uiScale_, 0.5F);
    const float logicalWidth = physicalWidth / safeScale;
    const bool compact = logicalWidth < kCompactBottomBarThreshold;
    return Scale(ui_detail::CalculateBottomBarLogicalLayout(
        compact,
        showSequenceFrameOffset).height);
}

void PlayerUI::Impl::SynchronizeControls(
    const ComparisonPlayerSnapshot& comparisonSnapshot) {
    const PlayerSnapshot& snapshot = comparisonSnapshot.primary;
    const std::uint32_t activeDecodePercent =
        ui_detail::ResolveActiveDecodePercent(
            comparisonSnapshot.enabled,
            comparisonSnapshot.decodePercent,
            snapshot.decodePercent);
    const bool decodeLoadPending = ui_detail::IsDecodeLoadPending(
        comparisonSnapshot.enabled,
        snapshot.loading,
        comparisonSnapshot.secondary.loading);
    if (!exportSettingsLoaded_) {
        exportSettingsLoaded_ = true;
        exportFolder_ = user_settings::LoadExportFolder();
    }
    if (!keyboardShuttleSpeedSettingsLoaded_) {
        keyboardShuttleSpeedSettingsLoaded_ = true;
        keyboardShuttleSpeedPercent_ =
            ui_detail::ClampKeyboardShuttleSpeedPercent(
                user_settings::LoadKeyboardShuttleSpeedPercent().value_or(
                    ui_detail::kDefaultKeyboardShuttleSpeedPercent));
    }
    if (!controlsInitialized_) {
        controlsInitialized_ = true;
        memoryGiB_ = RoundedMemoryGiB(snapshot.memoryLimitBytes);
        decodePercent_ = ui_detail::NormalizeDecodePercent(activeDecodePercent);
        requestedDecodePercent_ = decodePercent_;
        framesPerSecond_ = static_cast<float>(snapshot.targetFramesPerSecond);
    }

    const auto synchronizePaneSource = [this](
                                          const ViewportPane pane,
                                          const PlayerSnapshot& paneSnapshot) {
        ViewportPaneState& state = PaneState(pane);
        if (state.observedSourceKind == paneSnapshot.sourceKind &&
            state.observedSourcePath == paneSnapshot.sourcePathUtf8) {
            return;
        }
        state.observedSourceKind = paneSnapshot.sourceKind;
        state.observedSourcePath = paneSnapshot.sourcePathUtf8;
        ResetViewportView(pane);
    };
    synchronizePaneSource(ViewportPane::Primary, snapshot);
    synchronizePaneSource(
        ViewportPane::Secondary,
        comparisonSnapshot.enabled
            ? comparisonSnapshot.secondary
            : PlayerSnapshot{});

    if (observedGeneration_ != snapshot.generation) {
        observedGeneration_ = snapshot.generation;
        memoryGiB_ = RoundedMemoryGiB(snapshot.memoryLimitBytes);
        if (!comparisonSnapshot.enabled || !decodeChangePending_ ||
            activeDecodePercent == requestedDecodePercent_) {
            decodePercent_ =
                ui_detail::NormalizeDecodePercent(activeDecodePercent);
            requestedDecodePercent_ = decodePercent_;
            decodeChangePending_ = false;
            decodeChangeFailed_ = false;
        }
        framesPerSecond_ = static_cast<float>(snapshot.targetFramesPerSecond);
        viewportScrubbing_ = false;
        viewportScrubArmed_ = false;
        PaneState(ViewportPane::Primary).panning = false;
        PaneState(ViewportPane::Secondary).panning = false;
        timelineScrubbing_ = false;
        lastTimelineRequest_ = std::numeric_limits<FrameIndex>::max();
        playbackRangeSliderState_ = {};
        playbackStartFrameInputEditing_ = false;
        playbackEndFrameInputEditing_ = false;
        playbackStartFrameCommittedWhileActive_ = false;
        playbackEndFrameCommittedWhileActive_ = false;
        PaneState(ViewportPane::Primary).textureWasVisible = false;
        dismissedEngineError_.clear();
        if (snapshot.hasSource && snapshot.errorUtf8.empty()) {
            ClearLocalError();
        }
    }

    if (decodeChangePending_ && !decodeLoadPending) {
        const bool requestedValueBecameActive =
            activeDecodePercent == requestedDecodePercent_;
        decodePercent_ = ui_detail::NormalizeDecodePercent(activeDecodePercent);
        decodeChangePending_ = false;
        decodeChangeFailed_ = !requestedValueBecameActive;
        activeDecodePercentAfterFailure_ = activeDecodePercent;
    }

    if (!snapshot.hasSource &&
        PaneState(ViewportPane::Primary).textureWasVisible) {
        PaneState(ViewportPane::Primary).textureWasVisible = false;
    }
}

void PlayerUI::Impl::SynchronizeMaskOverlaySettings(
    overlay::MaskOverlayTexture& maskOverlayTexture) {
    if (maskOverlaySettingsLoaded_) {
        return;
    }

    maskOverlaySettingsLoaded_ = true;
    maskOverlayImagePath_ = user_settings::LoadMaskOverlayImagePath();
    if (!maskOverlayImagePath_.has_value()) {
        return;
    }

    const overlay::MaskOverlayLoadResult loaded =
        maskOverlayTexture.Load(*maskOverlayImagePath_);
    maskOverlayTextureReady_ = static_cast<bool>(loaded);
    if (loaded) {
        maskOverlayImagePath_ = maskOverlayTexture.LoadedPath();
        maskOverlayUiError_.clear();
    } else {
        maskOverlayUiError_ = loaded.errorUtf8.empty()
            ? "已保存的 PNG 蒙版无法加载"
            : loaded.errorUtf8;
    }
}

void PlayerUI::Impl::UploadDisplayFrame(
    const PlayerSnapshot& snapshot,
    FrameTexture& frameTexture,
    const ViewportPane pane,
    const FrameTextureUploadDomain uploadDomain) {
    ViewportPaneState& paneState = PaneState(pane);
    if (!snapshot.displayFrame) {
        return;
    }
    const FrameTextureUploadKey uploadKey = MakeFrameTextureUploadKey(
        *snapshot.displayFrame,
        snapshot.displayRevision,
        uploadDomain);
    if (frameTexture.MatchesUploadKey(uploadKey)) {
        paneState.textureWasVisible = true;
        ClearFrameUploadError();
        return;
    }
    if (!frameTexture.Upload(*snapshot.displayFrame, uploadKey)) {
        frameTexture.ClearFrame();
        paneState.textureWasVisible = false;
        SetLocalError(
            pane == ViewportPane::Primary
                ? "主画面上传失败"
                : "对比画面上传失败",
            "D3D11 纹理上传失败。当前帧不会使用错误纹理冒充，请重试或重新打开来源。",
            LocalErrorKind::FrameUpload);
        return;
    }
    ClearFrameUploadError();
    paneState.textureWasVisible = true;
}

void PlayerUI::Impl::HandleKeyboard(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const ComparisonPlayerSnapshot& comparisonSnapshot) {
    constexpr ImGuiPopupFlags anyPopupFlags =
        ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel;

    const ImGuiIO& io = ImGui::GetIO();
    const bool sourceContextChanged = !keyboardSourceContextInitialized_ ||
        keyboardComparisonEnabled_ != comparisonSnapshot.enabled ||
        keyboardPrimaryGeneration_ != comparisonSnapshot.primary.generation ||
        keyboardSecondaryGeneration_ != comparisonSnapshot.secondary.generation;
    keyboardSourceContextInitialized_ = true;
    keyboardComparisonEnabled_ = comparisonSnapshot.enabled;
    keyboardPrimaryGeneration_ = comparisonSnapshot.primary.generation;
    keyboardSecondaryGeneration_ = comparisonSnapshot.secondary.generation;

    const bool eitherSourceLoading = comparisonSnapshot.primary.loading ||
        comparisonSnapshot.secondary.loading;
    const ui_detail::KeyboardRoutingState routingState{
        snapshot.hasSource,
        eitherSourceLoading,
        ImGui::IsPopupOpen(nullptr, anyPopupFlags),
        io.WantTextInput,
        ImGui::IsAnyItemActive()};

    const ui_detail::PlayerHotkeyPressState pressed{
        ImGui::IsKeyPressed(ImGuiKey_Space, false),
        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false),
        ImGui::IsKeyPressed(ImGuiKey_RightArrow, false),
        ImGui::IsKeyPressed(ImGuiKey_Home, false),
        ImGui::IsKeyPressed(ImGuiKey_End, false)};

    ui_detail::PlayerHotkeyPressState routedPresses = pressed;
    if (pressed.home || pressed.end) {
        routedPresses.left = false;
        routedPresses.right = false;
    }
    const ui_detail::PlayerHotkeyCommand command =
        ui_detail::ResolvePlayerHotkeyCommand(routingState, routedPresses);
    const bool nonDirectionalCommandPressed = pressed.space || pressed.home ||
        pressed.end;
    const ui_detail::KeyboardShuttleInput shuttleInput{
        ui_detail::ShouldHandleNavigationHotkeys(routingState),
        io.AppFocusLost || sourceContextChanged ||
            nonDirectionalCommandPressed ||
            (keyboardShuttleController_.IsActive() && !snapshot.playing),
        pressed.left,
        pressed.right,
        ImGui::IsKeyDown(ImGuiKey_LeftArrow),
        ImGui::IsKeyDown(ImGuiKey_RightArrow),
        static_cast<double>(io.DeltaTime)};
    const ui_detail::KeyboardShuttleAction shuttleAction =
        keyboardShuttleController_.Update(shuttleInput);
    if (shuttleAction.endShuttle) {
        player.EndShuttlePlayback();
    }

    switch (command) {
    case ui_detail::PlayerHotkeyCommand::TogglePlayback:
        // Use the frame-start snapshot as the single source of truth. If the
        // transport button and Space arrive in the same UI frame, both resolve
        // to the same target state instead of toggling twice.
        player.SetPlaying(!snapshot.playing);
        return;
    case ui_detail::PlayerHotkeyCommand::StepBackward:
    case ui_detail::PlayerHotkeyCommand::StepForward:
        break;
    case ui_detail::PlayerHotkeyCommand::SeekPlaybackStart:
        player.SetPlaying(false);
        player.Seek(snapshot.playbackStartFrame);
        return;
    case ui_detail::PlayerHotkeyCommand::SeekPlaybackEnd:
        player.SetPlaying(false);
        player.Seek(snapshot.playbackEndFrame);
        return;
    case ui_detail::PlayerHotkeyCommand::None:
        break;
    }

    if (shuttleAction.stepDirection != 0) {
        player.SetPlaying(false);
        player.StepFrame(shuttleAction.stepDirection);
        return;
    }
    if (shuttleAction.beginDirection != 0) {
        player.BeginShuttlePlayback(
            shuttleAction.beginDirection,
            ui_detail::KeyboardShuttlePlaybackRateFromPercent(
                keyboardShuttleSpeedPercent_));
    }
}

void PlayerUI::Impl::OpenFolder(
    ComparisonPlayer& player,
    const UiActions& actions) {
    if (!actions.chooseFolder) {
        SetLocalError(
            "无法打开文件夹",
            "当前应用没有提供系统文件夹选择器。");
        return;
    }

    const std::optional<std::filesystem::path> selected = actions.chooseFolder();
    if (!selected.has_value()) {
        return;
    }
    if (!player.LoadFolder(*selected)) {
        SetLocalError(
            "文件夹加载失败，已保留当前序列",
            "无法扫描文件夹或解码首帧。请查看路径、PNG 文件和内存设置后重试。");
        return;
    }
    ClearLocalError();
    dismissedEngineError_.clear();
}

void PlayerUI::Impl::ReloadFolder(ComparisonPlayer& player) {
    if (!player.ReloadFolder()) {
        SetLocalError(
            "重新加载失败，已保留当前序列",
            "当前序列无法重新加载，或首帧无法解码。播放器没有替换现有可用序列。");
        return;
    }
    ClearLocalError();
    dismissedEngineError_.clear();
}

PlayerUI::Impl::ErrorView PlayerUI::Impl::CurrentError(
    const PlayerSnapshot& snapshot) const {
    if (!localErrorDetail_.empty()) {
        return {true, localErrorHeadline_, localErrorDetail_, false};
    }
    if (!snapshot.errorUtf8.empty() &&
        snapshot.errorUtf8 != dismissedEngineError_) {
        if (decodeChangeFailed_) {
            return {
                true,
                "解码比例切换失败，当前仍为 " +
                    DecodeDescription(activeDecodePercentAfterFailure_),
                snapshot.errorUtf8,
                true};
        }
        return {
            true,
            "操作未完成，已保留当前可用状态",
            snapshot.errorUtf8,
            true};
    }
    return {};
}

void PlayerUI::Impl::SetLocalError(
    std::string headline,
    std::string detail,
    const LocalErrorKind kind) {
    localErrorHeadline_ = std::move(headline);
    localErrorDetail_ = std::move(detail);
    localErrorKind_ = kind;
    errorDetailsExpanded_ = false;
}

void PlayerUI::Impl::ClearLocalError() {
    localErrorHeadline_.clear();
    localErrorDetail_.clear();
    localErrorKind_ = LocalErrorKind::None;
}

void PlayerUI::Impl::ClearFrameUploadError() {
    if (localErrorKind_ == LocalErrorKind::FrameUpload) {
        ClearLocalError();
    }
}

}  // namespace zt::sequence
