#include "Render/FrameTexture.h"
#include "UI/ComparisonCanvasLayout.h"
#include "UI/MaskPreset.h"
#include "UI/PlayerUILogic.h"
#include "UI/ViewportTransform.h"

#include <cstdint>
#include <limits>

using zt::sequence::ui_detail::ClampMemoryGiB;
using zt::sequence::ui_detail::CanOpenLastExportedVideo;
using zt::sequence::ui_detail::ClientPointInsideRect;
using zt::sequence::ui_detail::ClampPlaybackEndInputOneBased;
using zt::sequence::ui_detail::ClampPlaybackEndHandle;
using zt::sequence::ui_detail::ClampPlaybackStartInputOneBased;
using zt::sequence::ui_detail::ClampPlaybackStartHandle;
using zt::sequence::ui_detail::ClampComparisonSequenceFrameOffsetInput;
using zt::sequence::ui_detail::CalculateBottomBarLogicalLayout;
using zt::sequence::ui_detail::CalculateTimelineReadyCacheSegments;
using zt::sequence::ui_detail::FrameFromNormalizedPosition;
using zt::sequence::ui_detail::FrameTimestampSeconds;
using zt::sequence::ui_detail::KeyboardRoutingState;
using zt::sequence::ui_detail::MemoryBytesFromGiB;
using zt::sequence::ui_detail::NormalizeTimelineFrame;
using zt::sequence::ui_detail::NormalizeDecodePercent;
using zt::sequence::ui_detail::ResolveActiveDecodePercent;
using zt::sequence::ui_detail::IsDecodeLoadPending;
using zt::sequence::ui_detail::PlayerHotkeyCommand;
using zt::sequence::ui_detail::PlayerHotkeyPressState;
using zt::sequence::ui_detail::ResolvePlayerHotkeyCommand;
using zt::sequence::ui_detail::ScrubTargetFrame;
using zt::sequence::ui_detail::ShouldHandleNavigationHotkeys;
using zt::sequence::ui_detail::ShouldHandlePlaybackHotkey;
using zt::sequence::ui_detail::ShouldShowComparisonSequenceFrameOffset;
using zt::sequence::ui_detail::CalculateViewportImageRect;
using zt::sequence::ui_detail::CalculateComparisonCanvasLayout;
using zt::sequence::ui_detail::ClampViewportCenter;
using zt::sequence::ui_detail::ClampViewportCenterAxis;
using zt::sequence::ui_detail::ClampViewportZoom;
using zt::sequence::ui_detail::FitComparisonCanvasToDisplay;
using zt::sequence::ui_detail::IsDefaultViewportTransform;
using zt::sequence::ui_detail::MapComparisonContentToDisplay;
using zt::sequence::ui_detail::MapComparisonOpeningToDisplay;
using zt::sequence::ui_detail::PanViewport;
using zt::sequence::ui_detail::ViewportPoint;
using zt::sequence::ui_detail::ViewportSize;
using zt::sequence::ui_detail::ViewportTransform;

inline constexpr zt::sequence::FrameTextureUploadKey kPlayerTextureKey{
    zt::sequence::FrameTextureUploadDomain::PlayerEngine,
    7U,
    12U,
    3U};
inline constexpr zt::sequence::FrameTextureUploadKey kComparisonTextureKey{
    zt::sequence::FrameTextureUploadDomain::ComparisonPair,
    7U,
    12U,
    3U};
inline constexpr zt::sequence::FrameTextureUploadKey kOtherGenerationKey{
    zt::sequence::FrameTextureUploadDomain::PlayerEngine,
    8U,
    12U,
    3U};
inline constexpr zt::sequence::FrameTextureUploadKey kOtherFrameKey{
    zt::sequence::FrameTextureUploadDomain::PlayerEngine,
    7U,
    13U,
    3U};
static_assert(kPlayerTextureKey != kComparisonTextureKey);
static_assert(kPlayerTextureKey != kOtherGenerationKey);
static_assert(kPlayerTextureKey != kOtherFrameKey);
static_assert(ClientPointInsideRect(728, 44, 728.0F, 44.0F, 1448.0F, 739.0F));
static_assert(ClientPointInsideRect(1448, 739, 728.0F, 44.0F, 1448.0F, 739.0F));
static_assert(!ClientPointInsideRect(727, 320, 728.0F, 44.0F, 1448.0F, 739.0F));
static_assert(!ClientPointInsideRect(1516, 320, 728.0F, 44.0F, 1448.0F, 739.0F));
using zt::sequence::ui_detail::ZoomViewportAtPoint;
using zt::sequence::ui_detail::ZoomViewportByWheel;

using zt::sequence::ui::CenteredNormalizedMaskOpening;
using zt::sequence::ui::HasMask;
using zt::sequence::ui::IsValidNormalizedMaskOpening;
using zt::sequence::ui::MapMaskOpeningToDisplay;
using zt::sequence::ui::MaskDisplayRect;
using zt::sequence::ui::MaskOpeningForPreset;
using zt::sequence::ui::MaskOpeningForPresetInDisplay;
using zt::sequence::ui::MaskPixelSize;
using zt::sequence::ui::MaskPreset;

[[nodiscard]] constexpr bool NearlyEqual(
    const float first,
    const float second,
    const float tolerance = 0.000001F) noexcept {
    const float difference = first - second;
    return difference >= -tolerance && difference <= tolerance;
}

[[nodiscard]] constexpr bool RectEquals(
    const zt::sequence::ui::NormalizedMaskOpening rectangle,
    const float minimumX,
    const float minimumY,
    const float maximumX,
    const float maximumY) noexcept {
    return NearlyEqual(rectangle.minimumX, minimumX) &&
        NearlyEqual(rectangle.minimumY, minimumY) &&
        NearlyEqual(rectangle.maximumX, maximumX) &&
        NearlyEqual(rectangle.maximumY, maximumY);
}

[[nodiscard]] constexpr bool DisplayRectEquals(
    const MaskDisplayRect rectangle,
    const float minimumX,
    const float minimumY,
    const float maximumX,
    const float maximumY) noexcept {
    return NearlyEqual(rectangle.minimumX, minimumX) &&
        NearlyEqual(rectangle.minimumY, minimumY) &&
        NearlyEqual(rectangle.maximumX, maximumX) &&
        NearlyEqual(rectangle.maximumY, maximumY);
}

[[nodiscard]] constexpr float DisplayRectWidth(
    const MaskDisplayRect rectangle) noexcept {
    return rectangle.maximumX - rectangle.minimumX;
}

[[nodiscard]] constexpr float DisplayRectHeight(
    const MaskDisplayRect rectangle) noexcept {
    return rectangle.maximumY - rectangle.minimumY;
}

[[nodiscard]] constexpr bool DisplayRectSizeEquals(
    const MaskDisplayRect first,
    const MaskDisplayRect second) noexcept {
    return NearlyEqual(DisplayRectWidth(first), DisplayRectWidth(second)) &&
        NearlyEqual(DisplayRectHeight(first), DisplayRectHeight(second));
}

[[nodiscard]] constexpr bool CanvasRectEquals(
    const zt::sequence::ui_detail::ComparisonCanvasRect rectangle,
    const float minimumX,
    const float minimumY,
    const float maximumX,
    const float maximumY) noexcept {
    return NearlyEqual(rectangle.minimumX, minimumX) &&
        NearlyEqual(rectangle.minimumY, minimumY) &&
        NearlyEqual(rectangle.maximumX, maximumX) &&
        NearlyEqual(rectangle.maximumY, maximumY);
}

[[nodiscard]] constexpr zt::sequence::ui_detail::ComparisonCanvasLayout
PreviewLayoutIgnoringDecodedSize(
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const std::uint32_t /*decodedWidth*/,
    const std::uint32_t /*decodedHeight*/,
    const MaskPreset preset) noexcept {
    return CalculateComparisonCanvasLayout(
        sourceWidth,
        sourceHeight,
        MaskOpeningForPreset(preset));
}

static_assert(ClampMemoryGiB(3) == 4);
static_assert(ClampMemoryGiB(25) == 25);
static_assert(ClampMemoryGiB(64) == 48);
static_assert(MemoryBytesFromGiB(25) == 25ULL * 1024ULL * 1024ULL * 1024ULL);

inline constexpr auto kStandardBottomBar =
    CalculateBottomBarLogicalLayout(false, false);
inline constexpr auto kStandardBottomBarWithSequenceOffset =
    CalculateBottomBarLogicalLayout(false, true);
inline constexpr auto kCompactBottomBar =
    CalculateBottomBarLogicalLayout(true, false);
inline constexpr auto kCompactBottomBarWithSequenceOffset =
    CalculateBottomBarLogicalLayout(true, true);
static_assert(kStandardBottomBar.controlsY == 80.0F);
static_assert(kStandardBottomBar.statusY == 164.0F);
static_assert(kStandardBottomBar.height == 192.0F);
static_assert(kStandardBottomBarWithSequenceOffset.sequenceFrameOffsetY == 80.0F);
static_assert(kStandardBottomBarWithSequenceOffset.controlsY == 120.0F);
static_assert(kStandardBottomBarWithSequenceOffset.statusY == 204.0F);
static_assert(kStandardBottomBarWithSequenceOffset.height == 232.0F);
static_assert(kCompactBottomBar.controlsY == 80.0F);
static_assert(kCompactBottomBar.statusY == 244.0F);
static_assert(kCompactBottomBar.height == 272.0F);
static_assert(kCompactBottomBarWithSequenceOffset.controlsY == 120.0F);
static_assert(kCompactBottomBarWithSequenceOffset.statusY == 284.0F);
static_assert(kCompactBottomBarWithSequenceOffset.height == 312.0F);

static_assert(!ShouldShowComparisonSequenceFrameOffset(false, false));
static_assert(!ShouldShowComparisonSequenceFrameOffset(false, true));
static_assert(!ShouldShowComparisonSequenceFrameOffset(true, false));
static_assert(ShouldShowComparisonSequenceFrameOffset(true, true));
static_assert(ClampComparisonSequenceFrameOffsetInput(-1, 999U) == 0U);
static_assert(ClampComparisonSequenceFrameOffsetInput(0, 999U) == 0U);
static_assert(ClampComparisonSequenceFrameOffsetInput(200, 999U) == 200U);
static_assert(ClampComparisonSequenceFrameOffsetInput(1000, 999U) == 999U);
static_assert(ClampComparisonSequenceFrameOffsetInput(
    std::numeric_limits<std::int64_t>::max(),
    std::numeric_limits<std::uint32_t>::max()) ==
    std::numeric_limits<std::uint32_t>::max());

static_assert(CanOpenLastExportedVideo(true, true, true));
static_assert(!CanOpenLastExportedVideo(false, true, true));
static_assert(!CanOpenLastExportedVideo(true, false, true));
static_assert(!CanOpenLastExportedVideo(true, true, false));

// Space remains global after frame stepping even when a non-text control keeps
// ImGui's Active ID. Arrow/Home/End/L routing stays suppressed in that state.
inline constexpr KeyboardRoutingState kIdleKeyboardState{
    true, false, false, false, false};
inline constexpr KeyboardRoutingState kActiveControlKeyboardState{
    true, false, false, false, true};
static_assert(ShouldHandlePlaybackHotkey(kIdleKeyboardState));
static_assert(ShouldHandleNavigationHotkeys(kIdleKeyboardState));
static_assert(ShouldHandlePlaybackHotkey(kActiveControlKeyboardState));
static_assert(!ShouldHandleNavigationHotkeys(kActiveControlKeyboardState));

static_assert(!ShouldHandlePlaybackHotkey(
    KeyboardRoutingState{false, false, false, false, false}));
static_assert(!ShouldHandlePlaybackHotkey(
    KeyboardRoutingState{true, true, false, false, false}));
static_assert(!ShouldHandlePlaybackHotkey(
    KeyboardRoutingState{true, false, true, false, false}));
static_assert(!ShouldHandlePlaybackHotkey(
    KeyboardRoutingState{true, false, false, true, true}));

// A simultaneous arrow repeat must never overwrite Space's playback command.
static_assert(ResolvePlayerHotkeyCommand(
    kIdleKeyboardState,
    PlayerHotkeyPressState{true, false, true, false, false, false}) ==
    PlayerHotkeyCommand::TogglePlayback);
static_assert(ResolvePlayerHotkeyCommand(
    kActiveControlKeyboardState,
    PlayerHotkeyPressState{true, true, false, false, false, false}) ==
    PlayerHotkeyCommand::TogglePlayback);
static_assert(ResolvePlayerHotkeyCommand(
    kIdleKeyboardState,
    PlayerHotkeyPressState{false, true, false, false, false, false}) ==
    PlayerHotkeyCommand::StepBackward);
static_assert(ResolvePlayerHotkeyCommand(
    kIdleKeyboardState,
    PlayerHotkeyPressState{false, false, true, false, false, false}) ==
    PlayerHotkeyCommand::StepForward);
static_assert(ResolvePlayerHotkeyCommand(
    KeyboardRoutingState{true, false, false, true, true},
    PlayerHotkeyPressState{true, false, true, false, false, false}) ==
    PlayerHotkeyCommand::None);

static_assert(NormalizeDecodePercent(25U) == 25U);
static_assert(NormalizeDecodePercent(49U) == 50U);
static_assert(NormalizeDecodePercent(75U) == 75U);
static_assert(NormalizeDecodePercent(100U) == 100U);
static_assert(ResolveActiveDecodePercent(true, 100U, 50U) == 100U);
static_assert(ResolveActiveDecodePercent(true, 50U, 50U) == 50U);
static_assert(ResolveActiveDecodePercent(false, 100U, 50U) == 50U);
static_assert(IsDecodeLoadPending(true, false, true));
static_assert(!IsDecodeLoadPending(false, false, true));

static_assert(FrameFromNormalizedPosition(0.0, 4000U) == 0U);
static_assert(FrameFromNormalizedPosition(1.0, 4000U) == 3999U);
static_assert(FrameFromNormalizedPosition(-1.0, 4000U) == 0U);
static_assert(FrameFromNormalizedPosition(2.0, 4000U) == 3999U);
static_assert(FrameFromNormalizedPosition(0.5, 5U) == 2U);

inline constexpr auto kReadyCacheNonLooping =
    CalculateTimelineReadyCacheSegments(100U, 20U, 0U, 99U, 10U, false);
static_assert(kReadyCacheNonLooping.first.visible);
static_assert(!kReadyCacheNonLooping.wrapped.visible);
static_assert(
    kReadyCacheNonLooping.first.minimum == NormalizeTimelineFrame(20U, 99U));
static_assert(
    kReadyCacheNonLooping.first.maximum == NormalizeTimelineFrame(30U, 99U));

// A non-looping hot-cache segment may never extend beyond the custom end.
inline constexpr auto kReadyCacheClampedToPlaybackEnd =
    CalculateTimelineReadyCacheSegments(100U, 90U, 10U, 95U, 50U, false);
static_assert(kReadyCacheClampedToPlaybackEnd.first.visible);
static_assert(!kReadyCacheClampedToPlaybackEnd.wrapped.visible);
static_assert(
    kReadyCacheClampedToPlaybackEnd.first.minimum ==
        NormalizeTimelineFrame(90U, 99U));
static_assert(
    kReadyCacheClampedToPlaybackEnd.first.maximum ==
        NormalizeTimelineFrame(95U, 99U));

inline constexpr auto kReadyCacheWrapping =
    CalculateTimelineReadyCacheSegments(100U, 90U, 10U, 95U, 10U, true);
static_assert(kReadyCacheWrapping.first.visible);
static_assert(kReadyCacheWrapping.wrapped.visible);
static_assert(
    kReadyCacheWrapping.first.minimum == NormalizeTimelineFrame(90U, 99U));
static_assert(
    kReadyCacheWrapping.first.maximum == NormalizeTimelineFrame(95U, 99U));
static_assert(
    kReadyCacheWrapping.wrapped.minimum == NormalizeTimelineFrame(10U, 99U));
static_assert(
    kReadyCacheWrapping.wrapped.maximum == NormalizeTimelineFrame(14U, 99U));

inline constexpr auto kReadyCacheStartsAtRangeWhenOutside =
    CalculateTimelineReadyCacheSegments(100U, 5U, 10U, 50U, 4U, false);
static_assert(kReadyCacheStartsAtRangeWhenOutside.first.visible);
static_assert(
    kReadyCacheStartsAtRangeWhenOutside.first.minimum ==
        NormalizeTimelineFrame(10U, 99U));
static_assert(
    kReadyCacheStartsAtRangeWhenOutside.first.maximum ==
        NormalizeTimelineFrame(14U, 99U));

inline constexpr auto kReadyCacheZeroSteps =
    CalculateTimelineReadyCacheSegments(100U, 20U, 0U, 99U, 0U, true);
static_assert(!kReadyCacheZeroSteps.first.visible);
static_assert(!kReadyCacheZeroSteps.wrapped.visible);

inline constexpr auto kReadyCacheSingleFrameRange =
    CalculateTimelineReadyCacheSegments(100U, 40U, 40U, 40U, 10U, true);
static_assert(!kReadyCacheSingleFrameRange.first.visible);
static_assert(!kReadyCacheSingleFrameRange.wrapped.visible);

inline constexpr auto kReadyCacheNonLoopingAtEnd =
    CalculateTimelineReadyCacheSegments(100U, 80U, 10U, 80U, 10U, false);
static_assert(!kReadyCacheNonLoopingAtEnd.first.visible);
static_assert(!kReadyCacheNonLoopingAtEnd.wrapped.visible);

// Frame 1 represents 0.00 seconds; timestamps use the active target FPS.
static_assert(FrameTimestampSeconds(0U, 60.0) == 0.0);
static_assert(FrameTimestampSeconds(60U, 60.0) == 1.0);
static_assert(FrameTimestampSeconds(120U, 24.0) == 5.0);
static_assert(FrameTimestampSeconds(100U, 0.0) == 0.0);

static_assert(ClampPlaybackStartHandle(7U, 5U, 10U) == 5U);
static_assert(ClampPlaybackStartHandle(3U, 5U, 10U) == 3U);
static_assert(ClampPlaybackEndHandle(2U, 4U, 10U) == 4U);
static_assert(ClampPlaybackEndHandle(99U, 4U, 10U) == 9U);

// 手动输入使用 1-based 帧号，提交时夹取到合法范围并转成 0-based。
static_assert(ClampPlaybackStartInputOneBased(-20, 5U, 10U) == 0U);
static_assert(ClampPlaybackStartInputOneBased(4, 5U, 10U) == 3U);
static_assert(ClampPlaybackStartInputOneBased(99, 5U, 10U) == 5U);
static_assert(ClampPlaybackStartInputOneBased(1, 99U, 10U) == 0U);
static_assert(ClampPlaybackStartInputOneBased(1, 0U, 0U) == 0U);
static_assert(ClampPlaybackEndInputOneBased(-20, 4U, 10U) == 4U);
static_assert(ClampPlaybackEndInputOneBased(7, 4U, 10U) == 6U);
static_assert(ClampPlaybackEndInputOneBased(99, 4U, 10U) == 9U);
static_assert(ClampPlaybackEndInputOneBased(99, 99U, 10U) == 9U);
static_assert(ClampPlaybackEndInputOneBased(1, 0U, 0U) == 0U);

// 画布擦帧固定为 0.5 frame/px：累计 2 px 才移动 1 帧。
static_assert(ScrubTargetFrame(100U, 1.0F, 4000U) == 100U);
static_assert(ScrubTargetFrame(100U, 2.0F, 4000U) == 101U);
static_assert(ScrubTargetFrame(100U, -2.0F, 4000U) == 99U);
static_assert(ScrubTargetFrame(0U, -200.0F, 4000U) == 0U);
static_assert(ScrubTargetFrame(3999U, 200.0F, 4000U) == 3999U);

// Viewport zoom is relative to the fitted image. Zooming around a point keeps
// the same image coordinate under that point until a safe edge clamp is hit.
inline constexpr ViewportSize kViewportSize{800.0F, 800.0F};
inline constexpr ViewportSize kFittedImageSize{800.0F, 800.0F};
inline constexpr ViewportTransform kZoomedAtQuarter = ZoomViewportAtPoint(
    ViewportTransform{},
    2.0F,
    ViewportPoint{200.0F, 200.0F},
    kFittedImageSize,
    kViewportSize);
static_assert(kZoomedAtQuarter.zoom == 2.0F);
static_assert(kZoomedAtQuarter.centerU == 0.375F);
static_assert(kZoomedAtQuarter.centerV == 0.375F);
inline constexpr auto kZoomedRect = CalculateViewportImageRect(
    kZoomedAtQuarter,
    ViewportPoint{},
    kFittedImageSize,
    kViewportSize);
static_assert(kZoomedRect.minimumX == -200.0F);
static_assert(kZoomedRect.minimumY == -200.0F);
static_assert(kZoomedRect.maximumX == 1400.0F);
static_assert(kZoomedRect.maximumY == 1400.0F);

inline constexpr ViewportTransform kZoomedOutAtQuarter = ZoomViewportAtPoint(
    ViewportTransform{},
    0.5F,
    ViewportPoint{200.0F, 200.0F},
    kFittedImageSize,
    kViewportSize);
static_assert(kZoomedOutAtQuarter.zoom == 0.5F);
static_assert(kZoomedOutAtQuarter.centerU == 0.75F);
static_assert(kZoomedOutAtQuarter.centerV == 0.75F);
inline constexpr auto kZoomedOutRect = CalculateViewportImageRect(
    kZoomedOutAtQuarter,
    ViewportPoint{},
    kFittedImageSize,
    kViewportSize);
static_assert(kZoomedOutRect.minimumX == 100.0F);
static_assert(kZoomedOutRect.minimumY == 100.0F);
static_assert(kZoomedOutRect.maximumX == 500.0F);
static_assert(kZoomedOutRect.maximumY == 500.0F);

inline constexpr ViewportTransform kClampedPan = PanViewport(
    ViewportTransform{2.0F, 0.5F, 0.5F},
    ViewportPoint{1000.0F, -1000.0F},
    kFittedImageSize,
    kViewportSize);
static_assert(kClampedPan.centerU == 0.25F);
static_assert(kClampedPan.centerV == 0.75F);
static_assert(ClampViewportZoom(0.01F) == 0.25F);
static_assert(ClampViewportZoom(20.0F) == 16.0F);
static_assert(ClampViewportZoom(
    std::numeric_limits<float>::quiet_NaN()) == 1.0F);
static_assert(ClampViewportCenterAxis(
    std::numeric_limits<float>::quiet_NaN(),
    800.0F,
    800.0F) == 0.5F);
static_assert(IsDefaultViewportTransform(ViewportTransform{}));
static_assert(!IsDefaultViewportTransform(kZoomedAtQuarter));

// 对比模式始终按完整 1920 x 1920 虚拟画布缩放。遮罩只映射为同一
// 画布缩放下的子矩形，最终可见内容的内侧边缘统一贴中线。
inline constexpr auto kSquareNoMask = CalculateComparisonCanvasLayout(
    1920U,
    1920U,
    MaskOpeningForPreset(MaskPreset::None));
inline constexpr auto kSquareHorizontalMask = CalculateComparisonCanvasLayout(
    1920U,
    1920U,
    MaskOpeningForPreset(MaskPreset::Opening1920x1080));
inline constexpr auto kSquareSquareMask = CalculateComparisonCanvasLayout(
    1920U,
    1920U,
    MaskOpeningForPreset(MaskPreset::Opening1080x1080));
inline constexpr auto kSquarePortraitMask = CalculateComparisonCanvasLayout(
    1920U,
    1920U,
    MaskOpeningForPreset(MaskPreset::Opening1080x1920));
inline constexpr auto kSquareNarrowMask = CalculateComparisonCanvasLayout(
    1920U,
    1920U,
    MaskOpeningForPreset(MaskPreset::Opening864x1080));

inline constexpr auto kNoMaskCanvasRight = FitComparisonCanvasToDisplay(
    kSquareNoMask, 1000.0F, 800.0F, false);
inline constexpr auto kHorizontalCanvasRight = FitComparisonCanvasToDisplay(
    kSquareHorizontalMask, 1000.0F, 800.0F, false);
inline constexpr auto kSquareMaskCanvasRight = FitComparisonCanvasToDisplay(
    kSquareSquareMask, 1000.0F, 800.0F, false);
inline constexpr auto kPortraitCanvasRight = FitComparisonCanvasToDisplay(
    kSquarePortraitMask, 1000.0F, 800.0F, false);
inline constexpr auto kNarrowCanvasRight = FitComparisonCanvasToDisplay(
    kSquareNarrowMask, 1000.0F, 800.0F, false);

// None / 1920x1080 / 1080x1080 / 1080x1920 / 864x1080 的基础
// canvas 显示尺寸完全相同，均为 800 x 800；仅水平平移量不同。
static_assert(DisplayRectSizeEquals(kNoMaskCanvasRight, kHorizontalCanvasRight));
static_assert(DisplayRectSizeEquals(kNoMaskCanvasRight, kSquareMaskCanvasRight));
static_assert(DisplayRectSizeEquals(kNoMaskCanvasRight, kPortraitCanvasRight));
static_assert(DisplayRectSizeEquals(kNoMaskCanvasRight, kNarrowCanvasRight));
static_assert(NearlyEqual(DisplayRectWidth(kNoMaskCanvasRight), 800.0F));
static_assert(NearlyEqual(DisplayRectHeight(kNoMaskCanvasRight), 800.0F));

// 遮罩开口只是 800 x 800 方形画布中的同比例子矩形，不得放大到 Pane。
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(kSquareNoMask, kNoMaskCanvasRight),
    0.0F, 0.0F, 800.0F, 800.0F));
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(
        kSquareHorizontalMask, kHorizontalCanvasRight),
    0.0F, 175.0F, 800.0F, 625.0F));
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(
        kSquareSquareMask, kSquareMaskCanvasRight),
    0.0F, 175.0F, 450.0F, 625.0F));
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(
        kSquarePortraitMask, kPortraitCanvasRight),
    0.0F, 0.0F, 450.0F, 800.0F));
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(
        kSquareNarrowMask, kNarrowCanvasRight),
    0.0F, 175.0F, 360.0F, 625.0F));

// 方形来源填满开口，因此右侧实际可见内容 minimumX 全部为 0。
static_assert(MapComparisonContentToDisplay(
    kSquareNoMask, kNoMaskCanvasRight).minimumX == 0.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareHorizontalMask, kHorizontalCanvasRight).minimumX == 0.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareSquareMask, kSquareMaskCanvasRight).minimumX == 0.0F);
static_assert(MapComparisonContentToDisplay(
    kSquarePortraitMask, kPortraitCanvasRight).minimumX == 0.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareNarrowMask, kNarrowCanvasRight).minimumX == 0.0F);

inline constexpr auto kNoMaskCanvasLeft = FitComparisonCanvasToDisplay(
    kSquareNoMask, 1000.0F, 800.0F, true);
inline constexpr auto kHorizontalCanvasLeft = FitComparisonCanvasToDisplay(
    kSquareHorizontalMask, 1000.0F, 800.0F, true);
inline constexpr auto kSquareMaskCanvasLeft = FitComparisonCanvasToDisplay(
    kSquareSquareMask, 1000.0F, 800.0F, true);
inline constexpr auto kPortraitCanvasLeft = FitComparisonCanvasToDisplay(
    kSquarePortraitMask, 1000.0F, 800.0F, true);
inline constexpr auto kNarrowCanvasLeft = FitComparisonCanvasToDisplay(
    kSquareNarrowMask, 1000.0F, 800.0F, true);
static_assert(MapComparisonContentToDisplay(
    kSquareNoMask, kNoMaskCanvasLeft).maximumX == 1000.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareHorizontalMask, kHorizontalCanvasLeft).maximumX == 1000.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareSquareMask, kSquareMaskCanvasLeft).maximumX == 1000.0F);
static_assert(MapComparisonContentToDisplay(
    kSquarePortraitMask, kPortraitCanvasLeft).maximumX == 1000.0F);
static_assert(MapComparisonContentToDisplay(
    kSquareNarrowMask, kNarrowCanvasLeft).maximumX == 1000.0F);

// 已裁好的匹配尺寸来源使用完整 UV；1920 方图则只裁开口覆盖区域。
inline constexpr auto kPortraitInMatchingOpening =
    CalculateComparisonCanvasLayout(
        1080U,
        1920U,
        MaskOpeningForPreset(MaskPreset::Opening1080x1920));
static_assert(CanvasRectEquals(
    kPortraitInMatchingOpening.sourceInCanvas,
    420.0F, 0.0F, 1500.0F, 1920.0F));
static_assert(RectEquals(
    kPortraitInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));
static_assert(RectEquals(
    kSquarePortraitMask.sourceUv,
    0.21875F, 0.0F, 0.78125F, 1.0F));

inline constexpr auto kLandscapeInMatchingOpening =
    CalculateComparisonCanvasLayout(
        1920U,
        1080U,
        MaskOpeningForPreset(MaskPreset::Opening1920x1080));
inline constexpr auto kSquareInMatchingOpening =
    CalculateComparisonCanvasLayout(
        1080U,
        1080U,
        MaskOpeningForPreset(MaskPreset::Opening1080x1080));
inline constexpr auto kNarrowInMatchingOpening =
    CalculateComparisonCanvasLayout(
        864U,
        1080U,
        MaskOpeningForPreset(MaskPreset::Opening864x1080));
static_assert(RectEquals(
    kLandscapeInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));
static_assert(RectEquals(
    kSquareInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));
static_assert(RectEquals(
    kNarrowInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));

// 竖屏来源配横版遮罩时，横版开口仍保持方画布比例，交集右侧贴中线。
inline constexpr auto kPortraitInHorizontalOpening =
    CalculateComparisonCanvasLayout(
        1080U,
        1920U,
        MaskOpeningForPreset(MaskPreset::Opening1920x1080));
inline constexpr auto kPortraitHorizontalCanvasRight =
    FitComparisonCanvasToDisplay(
        kPortraitInHorizontalOpening,
        1000.0F,
        800.0F,
        false);
static_assert(DisplayRectEquals(
    kPortraitHorizontalCanvasRight,
    -175.0F, 0.0F, 625.0F, 800.0F));
static_assert(DisplayRectEquals(
    MapComparisonOpeningToDisplay(
        kPortraitInHorizontalOpening,
        kPortraitHorizontalCanvasRight),
    -175.0F, 175.0F, 625.0F, 625.0F));
static_assert(DisplayRectEquals(
    MapComparisonContentToDisplay(
        kPortraitInHorizontalOpening,
        kPortraitHorizontalCanvasRight),
    0.0F, 175.0F, 450.0F, 625.0F));
static_assert(RectEquals(
    kPortraitInHorizontalOpening.sourceUv,
    0.0F, 0.21875F, 1.0F, 0.78125F));

// 720 x 1280 小来源保持原始逻辑尺寸，不放大到 1080 x 1920 开口。
inline constexpr auto kSmallPortraitInLargerOpening =
    CalculateComparisonCanvasLayout(
        720U,
        1280U,
        MaskOpeningForPreset(MaskPreset::Opening1080x1920));
static_assert(CanvasRectEquals(
    kSmallPortraitInLargerOpening.sourceInCanvas,
    600.0F, 320.0F, 1320.0F, 1600.0F));
inline constexpr auto kSmallPortraitCanvasRight =
    FitComparisonCanvasToDisplay(
        kSmallPortraitInLargerOpening,
        1000.0F,
        800.0F,
        false);
static_assert(DisplayRectEquals(
    kSmallPortraitCanvasRight,
    -250.0F, 0.0F, 550.0F, 800.0F));
inline constexpr auto kSmallPortraitContentRight =
    MapComparisonContentToDisplay(
        kSmallPortraitInLargerOpening,
        kSmallPortraitCanvasRight);
static_assert(NearlyEqual(
    DisplayRectWidth(kSmallPortraitContentRight), 300.0F));
static_assert(NearlyEqual(
    DisplayRectHeight(kSmallPortraitContentRight),
    800.0F * (2.0F / 3.0F),
    0.001F));
static_assert(NearlyEqual(kSmallPortraitContentRight.minimumX, 0.0F));

// 超大来源只等比缩小到虚拟画布；匹配开口仍使用完整源 UV。
inline constexpr auto kOversizedPortraitInMatchingOpening =
    CalculateComparisonCanvasLayout(
        2160U,
        3840U,
        MaskOpeningForPreset(MaskPreset::Opening1080x1920));
inline constexpr auto kOversizedLandscapeInMatchingOpening =
    CalculateComparisonCanvasLayout(
        3840U,
        2160U,
        MaskOpeningForPreset(MaskPreset::Opening1920x1080));
static_assert(CanvasRectEquals(
    kOversizedPortraitInMatchingOpening.sourceInCanvas,
    420.0F, 0.0F, 1500.0F, 1920.0F));
static_assert(CanvasRectEquals(
    kOversizedLandscapeInMatchingOpening.sourceInCanvas,
    0.0F, 420.0F, 1920.0F, 1500.0F));
static_assert(RectEquals(
    kOversizedPortraitInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));
static_assert(RectEquals(
    kOversizedLandscapeInMatchingOpening.sourceUv,
    0.0F, 0.0F, 1.0F, 1.0F));

// 解码后的 270 x 480（25%）或 1080 x 1920（100%）纹理尺寸不进入布局。
inline constexpr auto kSourceLayoutWithDecoded25 =
    PreviewLayoutIgnoringDecodedSize(
        1080U, 1920U, 270U, 480U, MaskPreset::Opening1080x1920);
inline constexpr auto kSourceLayoutWithDecoded100 =
    PreviewLayoutIgnoringDecodedSize(
        1080U, 1920U, 1080U, 1920U, MaskPreset::Opening1080x1920);
static_assert(CanvasRectEquals(
    kSourceLayoutWithDecoded25.sourceInCanvas,
    kSourceLayoutWithDecoded100.sourceInCanvas.minimumX,
    kSourceLayoutWithDecoded100.sourceInCanvas.minimumY,
    kSourceLayoutWithDecoded100.sourceInCanvas.maximumX,
    kSourceLayoutWithDecoded100.sourceInCanvas.maximumY));
static_assert(RectEquals(
    kSourceLayoutWithDecoded25.sourceUv,
    kSourceLayoutWithDecoded100.sourceUv.minimumX,
    kSourceLayoutWithDecoded100.sourceUv.minimumY,
    kSourceLayoutWithDecoded100.sourceUv.maximumX,
    kSourceLayoutWithDecoded100.sourceUv.maximumY));

inline constexpr auto kInvalidComparisonSource =
    CalculateComparisonCanvasLayout(
        0U,
        1920U,
        MaskOpeningForPreset(MaskPreset::Opening1080x1920));
static_assert(!kInvalidComparisonSource.hasVisibleContent);

// 四档遮罩均以 1920 x 1920 为基准，开口严格居中。
static_assert(RectEquals(
    MaskOpeningForPreset(MaskPreset::Opening1080x1080),
    0.21875F,
    0.21875F,
    0.78125F,
    0.78125F));
static_assert(RectEquals(
    MaskOpeningForPreset(MaskPreset::Opening1920x1080),
    0.0F,
    0.21875F,
    1.0F,
    0.78125F));
static_assert(RectEquals(
    MaskOpeningForPreset(MaskPreset::Opening1080x1920),
    0.21875F,
    0.0F,
    0.78125F,
    1.0F));
static_assert(RectEquals(
    MaskOpeningForPreset(MaskPreset::Opening864x1080),
    0.275F,
    0.21875F,
    0.725F,
    0.78125F));

// 无遮罩、非法枚举、非法自定义尺寸或非法归一化矩形均回退完整画面。
static_assert(!HasMask(MaskPreset::None));
static_assert(RectEquals(MaskOpeningForPreset(MaskPreset::None), 0.0F, 0.0F, 1.0F, 1.0F));
static_assert(RectEquals(
    MaskOpeningForPreset(static_cast<MaskPreset>(255U)),
    0.0F,
    0.0F,
    1.0F,
    1.0F));
static_assert(RectEquals(
    CenteredNormalizedMaskOpening(MaskPixelSize{0U, 1080U}),
    0.0F,
    0.0F,
    1.0F,
    1.0F));
static_assert(!IsValidNormalizedMaskOpening({0.8F, 0.2F, 0.1F, 0.9F}));

// 映射不依赖实际视口大小。960 x 480 的显示区域应保持同一归一化开口。
inline constexpr MaskDisplayRect kTestDisplayRect{10.0F, 20.0F, 970.0F, 500.0F};
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(MaskPreset::Opening1080x1080, kTestDisplayRect),
    220.0F,
    125.0F,
    760.0F,
    395.0F));
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(MaskPreset::Opening1920x1080, kTestDisplayRect),
    10.0F,
    125.0F,
    970.0F,
    395.0F));
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(MaskPreset::Opening1080x1920, kTestDisplayRect),
    220.0F,
    20.0F,
    760.0F,
    500.0F));
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(MaskPreset::Opening864x1080, kTestDisplayRect),
    274.0F,
    125.0F,
    706.0F,
    395.0F));
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(MaskPreset::None, kTestDisplayRect),
    10.0F,
    20.0F,
    970.0F,
    500.0F));
static_assert(DisplayRectEquals(
    MaskOpeningForPresetInDisplay(static_cast<MaskPreset>(255U), kTestDisplayRect),
    10.0F,
    20.0F,
    970.0F,
    500.0F));
static_assert(DisplayRectEquals(
    MapMaskOpeningToDisplay({0.8F, 0.2F, 0.1F, 0.9F}, kTestDisplayRect),
    10.0F,
    20.0F,
    970.0F,
    500.0F));

int main() {
    const ViewportTransform wheelZoom = ZoomViewportByWheel(
        ViewportTransform{},
        1.0F,
        ViewportPoint{400.0F, 400.0F},
        kFittedImageSize,
        kViewportSize);
    const ViewportTransform wheelZoomOut = ZoomViewportByWheel(
        ViewportTransform{},
        -1.0F,
        ViewportPoint{400.0F, 400.0F},
        kFittedImageSize,
        kViewportSize);
    return NearlyEqual(wheelZoom.zoom, 1.2F) &&
            NearlyEqual(wheelZoom.centerU, 0.5F) &&
            NearlyEqual(wheelZoom.centerV, 0.5F) &&
            NearlyEqual(wheelZoomOut.zoom, 1.0F / 1.2F) &&
            NearlyEqual(wheelZoomOut.centerU, 0.5F) &&
            NearlyEqual(wheelZoomOut.centerV, 0.5F)
        ? 0
        : 1;
}
