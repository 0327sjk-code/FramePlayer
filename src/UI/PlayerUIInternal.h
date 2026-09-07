#pragma once

#include "Core/ComparisonPlayer.h"
#include "Core/PlayerTypes.h"
#include "Export/ExportTypes.h"
#include "UI/FrameRangeSlider.h"
#include "UI/InteractionAnimator.h"
#include "UI/MaskPreset.h"
#include "UI/PlayerUI.h"
#include "UI/ViewportTransform.h"

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace zt::sequence {

class ComparisonPlayer;
struct ComparisonPlayerSnapshot;
class FrameTexture;
enum class FrameTextureUploadDomain : std::uint8_t;
namespace exporting {
class FfmpegExportController;
}

namespace ui_internal {

inline constexpr float kTopBarHeight = 44.0F;
inline constexpr float kCompactBottomBarThreshold = 1340.0F;
inline constexpr float kExpandedErrorHeight = 42.0F;
inline constexpr float kControlHeight = 36.0F;
inline constexpr float kViewportScrubThresholdPixels = 2.0F;

extern const ImVec4 kColorBackground;
extern const ImVec4 kColorSurface;
extern const ImVec4 kColorSurfaceRaised;
extern const ImVec4 kColorSurfaceHover;
extern const ImVec4 kColorSurfaceActive;
extern const ImVec4 kColorBorder;
extern const ImVec4 kColorInk;
extern const ImVec4 kColorMuted;
extern const ImVec4 kColorPrimary;
extern const ImVec4 kColorPrimaryHover;
extern const ImVec4 kColorPrimaryActive;
extern const ImVec4 kColorExportProgress;
extern const ImVec4 kColorTimelinePlayback;
extern const ImVec4 kColorTimelineHotCache;
extern const ImVec4 kColorTimelineColdCache;
extern const ImVec4 kColorOnPrimary;
extern const ImVec4 kColorAccent;
extern const ImVec4 kColorWarning;
extern const ImVec4 kColorDanger;
extern const ImVec4 kColorViewportBackground;
extern const ImVec4 kColorViewportBadge;
extern const ImVec4 kColorViewportBadgeText;
extern const ImVec4 kColorMask;
extern const ImVec4 kColorMaskBorder;

enum class AnimatedButtonStyle : std::uint8_t {
    Neutral,
    Primary,
    Selected,
    Ghost,
};

[[nodiscard]] int RoundedMemoryGiB(std::uint64_t bytes) noexcept;
[[nodiscard]] std::string CacheSummary(const PlayerSnapshot& snapshot);
[[nodiscard]] std::string CompactCacheSummary(const PlayerSnapshot& snapshot);
[[nodiscard]] std::string PerformanceSummary(const PlayerSnapshot& snapshot);
[[nodiscard]] std::string DecodeDescription(std::uint32_t percent);
void TooltipForLastItem(const char* text);
void CenteredText(const char* text, float availableWidth);
void EllipsizedText(const char* text, float availableWidth, ImVec4 color);
[[nodiscard]] bool AnimatedButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size,
    AnimatedButtonStyle style = AnimatedButtonStyle::Neutral);
[[nodiscard]] bool AnimatedCheckbox(
    ui::InteractionAnimator& animator,
    const char* label,
    bool* value,
    float height);
[[nodiscard]] bool PrimaryButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size);
[[nodiscard]] bool PlaybackButton(
    ui::InteractionAnimator& animator,
    const char* label,
    const ImVec2& size,
    bool selected);
[[nodiscard]] ImVec4 WithAlpha(ImVec4 color, float alpha) noexcept;
[[nodiscard]] ImVec4 LerpColor(
    const ImVec4& first,
    const ImVec4& second,
    float amount) noexcept;

}  // namespace ui_internal

class PlayerUI::Impl final {
public:
    void SetUiScale(float scale) noexcept;
    void ReportRendererError(std::string_view detail);
    void Render(
        ComparisonPlayer& player,
        FrameTexture& primaryFrameTexture,
        FrameTexture& secondaryFrameTexture,
        exporting::FfmpegExportController& exporter,
        const UiActions& actions);
    [[nodiscard]] bool IsSecondaryViewportAtClientPoint(
        std::int32_t clientX,
        std::int32_t clientY) const noexcept;

private:
    enum class ViewportPane : std::uint8_t {
        Primary = 0,
        Secondary = 1,
    };

    struct ViewportPaneState final {
        ui_detail::ViewportTransform transform;
        ui_detail::ViewportTransform panOriginTransform;
        ui_detail::ViewportPoint panOriginMouse;
        bool panning = false;
        SourceKind observedSourceKind = SourceKind::None;
        std::string observedSourcePath;
        bool textureWasVisible = false;
    };

    struct ScreenRect final {
        float minimumX = 0.0F;
        float minimumY = 0.0F;
        float maximumX = 0.0F;
        float maximumY = 0.0F;
        bool valid = false;
    };

    enum class LocalErrorKind : std::uint8_t {
        None,
        General,
        FrameUpload,
        Renderer,
        ExternalFile,
    };

    struct ErrorView final {
        bool visible = false;
        std::string headline;
        std::string detail;
        bool fromEngine = false;
    };

    [[nodiscard]] float Scale(float value) const noexcept;
    [[nodiscard]] float TopBarHeightForWidth(float physicalWidth) const noexcept;
    [[nodiscard]] float BottomBarHeightForWidth(
        float physicalWidth,
        bool showSequenceFrameOffset) const noexcept;
    void SynchronizeControls(const ComparisonPlayerSnapshot& snapshot);
    void SynchronizeExportResult(
        const exporting::ExportProgressSnapshot& exportProgress);
    void UploadDisplayFrame(
        const PlayerSnapshot& snapshot,
        FrameTexture& frameTexture,
        ViewportPane pane,
        FrameTextureUploadDomain uploadDomain);

    void RenderTopBar(const PlayerSnapshot& snapshot);
    void RenderSequenceIdentity(
        const PlayerSnapshot& snapshot,
        float availableWidth);
    void RenderQuickActions(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        bool comparisonEnabled,
        const UiActions& actions);
    void RenderResourceSettings(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void RenderDecodePercent(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void RenderMaskPreset();
    void RenderViewport(
        ComparisonPlayer& player,
        const ComparisonPlayerSnapshot& snapshot,
        FrameTexture& primaryFrameTexture,
        FrameTexture& secondaryFrameTexture,
        float height);
    void RenderEmptyOrLoading(
        const PlayerSnapshot& snapshot,
        ImVec2 available,
        ViewportPane pane,
        bool comparisonLayout);
    void RenderSequenceViewport(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        FrameTexture& frameTexture,
        ImVec2 available,
        ViewportPane pane,
        bool comparisonLayout,
        std::size_t transportTotalFrames,
        bool frameAvailable,
        FrameTextureUploadDomain uploadDomain);
    void HandleViewportScrub(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        ViewportPane pane,
        std::size_t transportTotalFrames);
    void HandleViewportNavigation(
        ViewportPaneState& paneState,
        ImVec2 viewportMin,
        ui_detail::ViewportSize fittedImage,
        ui_detail::ViewportSize viewportSize);
    void ResetViewportView(ViewportPane pane) noexcept;
    [[nodiscard]] ViewportPaneState& PaneState(ViewportPane pane) noexcept;
    [[nodiscard]] const ViewportPaneState& PaneState(
        ViewportPane pane) const noexcept;
    void RenderMaskOverlay(
        ImVec2 imageMin,
        ImVec2 imageMax,
        ImDrawList* drawList) const;
    void RenderViewportBadges(
        const PlayerSnapshot& snapshot,
        const ViewportPaneState& paneState,
        ViewportPane pane,
        bool comparisonLayout,
        ImVec2 viewportMin,
        ImVec2 viewportMax,
        ImDrawList* drawList) const;

    void RenderBottomBar(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        const ComparisonPlayerSnapshot& comparisonSnapshot,
        exporting::FfmpegExportController& exporter,
        const exporting::ExportProgressSnapshot& exportProgress,
        const OnlineUpdateView& onlineUpdateView,
        const UiActions& actions,
        const ErrorView& error,
        float height);
    void RenderTimeline(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void RenderPlaybackRange(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void RenderComparisonSequenceFrameOffset(
        ComparisonPlayer& player,
        const ComparisonPlayerSnapshot& snapshot);
    void RenderPlaybackControls(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        bool comparisonEnabled,
        exporting::FfmpegExportController& exporter,
        const exporting::ExportProgressSnapshot& exportProgress,
        const OnlineUpdateView& onlineUpdateView,
        const UiActions& actions,
        bool compact);
    void RenderTransportControls(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void RenderPlaybackSettings(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        bool exportBusy);
    void RenderExportControls(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot,
        exporting::FfmpegExportController& exporter,
        const exporting::ExportProgressSnapshot& exportProgress,
        const OnlineUpdateView& onlineUpdateView,
        const UiActions& actions);
    void RenderExportProgress(
        const exporting::ExportProgressSnapshot& exportProgress);
    void RenderOnlineUpdateStatus(const OnlineUpdateView& onlineUpdateView);
    void RenderStatusLine(
        const PlayerSnapshot& snapshot,
        const exporting::ExportProgressSnapshot& exportProgress,
        const OnlineUpdateView& onlineUpdateView,
        const ErrorView& error);
    void ChooseExportFolder(const UiActions& actions);
    void OpenLastExportedVideo(const UiActions& actions);
    [[nodiscard]] bool IsLastExportedVideoAvailable() const;
    void StartExport(
        ComparisonPlayer& player,
        exporting::FfmpegExportController& exporter);
    [[nodiscard]] exporting::NormalizedCrop CurrentExportCrop(
        const ComparisonPlayerSnapshot& snapshot) const noexcept;

    void HandleKeyboard(
        ComparisonPlayer& player,
        const PlayerSnapshot& snapshot);
    void OpenFolder(ComparisonPlayer& player, const UiActions& actions);
    void ReloadFolder(ComparisonPlayer& player);
    [[nodiscard]] ErrorView CurrentError(const PlayerSnapshot& snapshot) const;
    void SetLocalError(
        std::string headline,
        std::string detail,
        LocalErrorKind kind = LocalErrorKind::General);
    void ClearLocalError();
    void ClearFrameUploadError();

    bool controlsInitialized_ = false;
    float uiScale_ = 1.0F;
    ui::InteractionAnimator interactionAnimator_;
    int memoryGiB_ = 25;
    std::uint32_t decodePercent_ = 100U;
    std::uint32_t requestedDecodePercent_ = 100U;
    std::uint32_t activeDecodePercentAfterFailure_ = 100U;
    bool decodeChangePending_ = false;
    bool decodeChangeFailed_ = false;
    ui::MaskPreset maskPreset_ = ui::MaskPreset::None;
    float framesPerSecond_ = 60.0F;
    Generation observedGeneration_ = std::numeric_limits<Generation>::max();
    bool exportSettingsLoaded_ = false;
    std::optional<std::filesystem::path> exportFolder_;
    std::filesystem::path lastSuccessfulExportPath_;
    std::uint64_t lastSuccessfulExportJobId_ = 0U;
    bool lastSuccessfulExportAvailable_ = false;
    std::string exportUiError_;

    bool viewportScrubbing_ = false;
    bool viewportScrubArmed_ = false;
    FrameIndex viewportScrubOriginFrame_ = 0U;
    float viewportScrubOriginX_ = 0.0F;
    float viewportScrubLastMouseX_ = 0.0F;
    FrameIndex lastViewportRequest_ = std::numeric_limits<FrameIndex>::max();
    std::array<ViewportPaneState, 2> viewportPanes_;
    ViewportPane activeViewportPane_ = ViewportPane::Primary;
    ScreenRect primaryViewportRect_;
    ScreenRect secondaryViewportRect_;

    bool timelineScrubbing_ = false;
    float timelineScrubLastMouseX_ = 0.0F;
    FrameIndex lastTimelineRequest_ = std::numeric_limits<FrameIndex>::max();
    ui::FrameRangeSliderState playbackRangeSliderState_;
    std::int64_t playbackStartFrameInput_ = 0;
    std::int64_t playbackEndFrameInput_ = 0;
    bool playbackStartFrameInputEditing_ = false;
    bool playbackEndFrameInputEditing_ = false;
    bool playbackStartFrameCommittedWhileActive_ = false;
    bool playbackEndFrameCommittedWhileActive_ = false;
    FrameIndex sequenceFrameOffsetCandidate_ = 0U;
    std::int64_t sequenceFrameOffsetInput_ = 0;
    bool sequenceFrameOffsetInputEditing_ = false;
    bool sequenceFrameOffsetCommittedWhileActive_ = false;
    bool sequenceFrameOffsetSliderEditing_ = false;

    bool errorDetailsExpanded_ = false;
    LocalErrorKind localErrorKind_ = LocalErrorKind::None;
    std::string localErrorHeadline_;
    std::string localErrorDetail_;
    std::string dismissedEngineError_;
};

}  // namespace zt::sequence
