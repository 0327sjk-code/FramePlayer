#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace zt::sequence::ui_detail {

inline constexpr int kMinimumMemoryGiB = 4;
inline constexpr int kMaximumMemoryGiB = 48;
inline constexpr std::uint64_t kBytesPerGiB = 1024ULL * 1024ULL * 1024ULL;
inline constexpr float kViewportScrubFramesPerPixel = 0.5F;
inline constexpr float kBottomBarControlRowStride = 40.0F;

struct BottomBarLogicalLayout final {
    float timelineY = 0.0F;
    float playbackRangeY = 0.0F;
    float sequenceFrameOffsetY = 0.0F;
    float controlsY = 0.0F;
    float statusY = 0.0F;
    float height = 0.0F;
};

// Keeps all conditional bottom-bar coordinates in one pure calculation. A
// sequence offset row is inserted above the controls without changing their
// internal spacing in either the standard or compact layout.
[[nodiscard]] inline constexpr BottomBarLogicalLayout
CalculateBottomBarLogicalLayout(
    const bool compact,
    const bool showSequenceFrameOffset) noexcept {
    constexpr float kTimelineY = 8.0F;
    constexpr float kPlaybackRangeY = 42.0F;
    constexpr float kSequenceFrameOffsetY = 80.0F;
    constexpr float kControlsY = 80.0F;
    constexpr float kStatusGapAfterControlRows = 4.0F;
    constexpr float kBottomPaddingAfterStatus = 28.0F;
    const float insertedHeight = showSequenceFrameOffset
        ? kBottomBarControlRowStride
        : 0.0F;
    const float controlsY = kControlsY + insertedHeight;
    const float controlRows = compact ? 4.0F : 2.0F;
    const float statusY = controlsY +
        kBottomBarControlRowStride * controlRows +
        kStatusGapAfterControlRows;
    return {
        kTimelineY,
        kPlaybackRangeY,
        kSequenceFrameOffsetY,
        controlsY,
        statusY,
        statusY + kBottomPaddingAfterStatus};
}

[[nodiscard]] inline constexpr bool
ShouldShowComparisonSequenceFrameOffset(
    const bool comparisonActive,
    const bool sequenceFrameOffsetAvailable) noexcept {
    return comparisonActive && sequenceFrameOffsetAvailable;
}

[[nodiscard]] inline constexpr std::uint32_t
ClampComparisonSequenceFrameOffsetInput(
    const std::int64_t requestedOffset,
    const std::uint32_t maximumOffset) noexcept {
    if (requestedOffset <= 0) {
        return 0U;
    }
    const std::uint64_t positiveOffset =
        static_cast<std::uint64_t>(requestedOffset);
    return positiveOffset > static_cast<std::uint64_t>(maximumOffset)
        ? maximumOffset
        : static_cast<std::uint32_t>(positiveOffset);
}

[[nodiscard]] inline constexpr bool ClientPointInsideRect(
    const std::int32_t clientX,
    const std::int32_t clientY,
    const float minimumX,
    const float minimumY,
    const float maximumX,
    const float maximumY) noexcept {
    const float x = static_cast<float>(clientX);
    const float y = static_cast<float>(clientY);
    return x >= minimumX && x <= maximumX &&
        y >= minimumY && y <= maximumY;
}

[[nodiscard]] inline constexpr double FrameTimestampSeconds(
    const std::uint32_t zeroBasedFrame,
    const double framesPerSecond) noexcept {
    return framesPerSecond > 0.0
        ? static_cast<double>(zeroBasedFrame) / framesPerSecond
        : 0.0;
}

struct KeyboardRoutingState final {
    bool hasSource = false;
    bool loading = false;
    bool popupOpen = false;
    bool wantsTextInput = false;
    bool anyItemActive = false;
};

struct PlayerHotkeyPressState final {
    bool space = false;
    bool left = false;
    bool right = false;
    bool home = false;
    bool end = false;
    bool loop = false;
};

enum class PlayerHotkeyCommand : std::uint8_t {
    None,
    TogglePlayback,
    StepBackward,
    StepForward,
    SeekPlaybackStart,
    SeekPlaybackEnd,
    ToggleLoop,
};

// Space is a global playback command. A focused button, slider, timeline, or
// viewport scrub region must not suppress it after frame stepping.
[[nodiscard]] inline constexpr bool ShouldHandlePlaybackHotkey(
    const KeyboardRoutingState& state) noexcept {
    return state.hasSource && !state.loading && !state.popupOpen &&
        !state.wantsTextInput;
}

// Navigation remains local to an idle, non-editing UI so arrow keys cannot
// alter the player while a numeric field or another control consumes them.
[[nodiscard]] inline constexpr bool ShouldHandleNavigationHotkeys(
    const KeyboardRoutingState& state) noexcept {
    return ShouldHandlePlaybackHotkey(state) && !state.anyItemActive;
}

// Resolve exactly one command per UI frame. Space has priority so an arrow-key
// repeat delivered in the same frame cannot immediately pause playback again.
[[nodiscard]] inline constexpr PlayerHotkeyCommand ResolvePlayerHotkeyCommand(
    const KeyboardRoutingState& routing,
    const PlayerHotkeyPressState& pressed) noexcept {
    if (ShouldHandlePlaybackHotkey(routing) && pressed.space) {
        return PlayerHotkeyCommand::TogglePlayback;
    }
    if (!ShouldHandleNavigationHotkeys(routing)) {
        return PlayerHotkeyCommand::None;
    }
    if (pressed.left) {
        return PlayerHotkeyCommand::StepBackward;
    }
    if (pressed.right) {
        return PlayerHotkeyCommand::StepForward;
    }
    if (pressed.home) {
        return PlayerHotkeyCommand::SeekPlaybackStart;
    }
    if (pressed.end) {
        return PlayerHotkeyCommand::SeekPlaybackEnd;
    }
    if (pressed.loop) {
        return PlayerHotkeyCommand::ToggleLoop;
    }
    return PlayerHotkeyCommand::None;
}

[[nodiscard]] inline constexpr bool CanOpenLastExportedVideo(
    const bool openActionAvailable,
    const bool hasSuccessfulExportPath,
    const bool exportedFileExists) noexcept {
    return openActionAvailable && hasSuccessfulExportPath &&
        exportedFileExists;
}

[[nodiscard]] inline constexpr int ClampMemoryGiB(const int value) noexcept {
    return value < kMinimumMemoryGiB
        ? kMinimumMemoryGiB
        : (value > kMaximumMemoryGiB ? kMaximumMemoryGiB : value);
}

[[nodiscard]] inline constexpr std::uint64_t MemoryBytesFromGiB(const int value) noexcept {
    return static_cast<std::uint64_t>(ClampMemoryGiB(value)) * kBytesPerGiB;
}

[[nodiscard]] inline constexpr std::uint32_t NormalizeDecodePercent(
    const std::uint32_t value) noexcept {
    if (value <= 37U) {
        return 25U;
    }
    if (value <= 62U) {
        return 50U;
    }
    if (value <= 87U) {
        return 75U;
    }
    return 100U;
}

// Comparison mode exposes only the last fully committed shared value. A lane
// may already have decoded the candidate percentage while its peer is still
// applying or rolling back, so the primary lane is not authoritative there.
[[nodiscard]] inline constexpr std::uint32_t ResolveActiveDecodePercent(
    const bool comparisonEnabled,
    const std::uint32_t sharedDecodePercent,
    const std::uint32_t primaryDecodePercent) noexcept {
    return comparisonEnabled
        ? sharedDecodePercent
        : primaryDecodePercent;
}

[[nodiscard]] inline constexpr bool IsDecodeLoadPending(
    const bool comparisonEnabled,
    const bool primaryLoading,
    const bool secondaryLoading) noexcept {
    return primaryLoading || (comparisonEnabled && secondaryLoading);
}

[[nodiscard]] inline constexpr std::uint32_t FrameFromNormalizedPosition(
    const double normalizedPosition,
    const std::size_t totalFrames) noexcept {
    if (totalFrames <= 1U) {
        return 0U;
    }

    const double clamped = normalizedPosition < 0.0
        ? 0.0
        : (normalizedPosition > 1.0 ? 1.0 : normalizedPosition);
    const double scaled = clamped * static_cast<double>(totalFrames - 1U);
    return static_cast<std::uint32_t>(scaled + 0.5);
}

struct NormalizedTimelineSegment final {
    double minimum = 0.0;
    double maximum = 0.0;
    bool visible = false;
};

struct TimelineReadyCacheSegments final {
    NormalizedTimelineSegment first;
    NormalizedTimelineSegment wrapped;
};

[[nodiscard]] inline constexpr double NormalizeTimelineFrame(
    const std::uint32_t frame,
    const std::uint32_t lastFrame) noexcept {
    return lastFrame > 0U
        ? static_cast<double>(frame) / static_cast<double>(lastFrame)
        : 0.0;
}

// Converts the existing contiguous ready-ahead count into one or two visual
// timeline segments. The cache itself remains authoritative; this helper only
// describes the current forward hot window and never changes scheduling.
[[nodiscard]] inline constexpr TimelineReadyCacheSegments
CalculateTimelineReadyCacheSegments(
    const std::size_t totalFrames,
    const std::uint32_t requestedFrame,
    const std::uint32_t playbackStartFrame,
    const std::uint32_t playbackEndFrame,
    const std::uint64_t readyStepCount,
    const bool loopPlayback) noexcept {
    if (totalFrames <= 1U || readyStepCount == 0U) {
        return {};
    }

    const std::uint32_t lastFrame = static_cast<std::uint32_t>(
        totalFrames - 1U >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : totalFrames - 1U);
    std::uint32_t rangeStart = playbackStartFrame > lastFrame
        ? lastFrame
        : playbackStartFrame;
    std::uint32_t rangeEnd = playbackEndFrame > lastFrame
        ? lastFrame
        : playbackEndFrame;
    if (rangeStart > rangeEnd) {
        const std::uint32_t previousStart = rangeStart;
        rangeStart = rangeEnd;
        rangeEnd = previousStart;
    }

    const std::uint32_t readyStart =
        requestedFrame >= rangeStart && requestedFrame <= rangeEnd
        ? requestedFrame
        : rangeStart;
    const std::uint64_t rangeFrameCount =
        static_cast<std::uint64_t>(rangeEnd) - rangeStart + 1ULL;
    const std::uint64_t boundedSteps = readyStepCount < rangeFrameCount
        ? readyStepCount
        : rangeFrameCount - 1ULL;
    if (boundedSteps == 0U) {
        return {};
    }

    const std::uint64_t stepsToRangeEnd =
        static_cast<std::uint64_t>(rangeEnd) - readyStart;
    const std::uint64_t visibleSteps = loopPlayback
        ? boundedSteps
        : (boundedSteps < stepsToRangeEnd
            ? boundedSteps
            : stepsToRangeEnd);
    if (visibleSteps == 0U) {
        return {};
    }

    TimelineReadyCacheSegments result;
    if (visibleSteps <= stepsToRangeEnd) {
        const std::uint32_t readyEnd = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(readyStart) + visibleSteps);
        result.first = {
            NormalizeTimelineFrame(readyStart, lastFrame),
            NormalizeTimelineFrame(readyEnd, lastFrame),
            true};
        return result;
    }

    result.first = {
        NormalizeTimelineFrame(readyStart, lastFrame),
        NormalizeTimelineFrame(rangeEnd, lastFrame),
        true};
    const std::uint64_t wrappedFrameCount =
        visibleSteps - stepsToRangeEnd;
    const std::uint32_t wrappedEnd = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(rangeStart) + wrappedFrameCount - 1ULL);
    result.wrapped = {
        NormalizeTimelineFrame(rangeStart, lastFrame),
        NormalizeTimelineFrame(wrappedEnd, lastFrame),
        true};
    return result;
}

[[nodiscard]] inline constexpr std::uint32_t ClampPlaybackStartHandle(
    const std::uint32_t requestedFrame,
    const std::uint32_t endFrame,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0U) {
        return 0U;
    }
    const std::uint32_t lastFrame = static_cast<std::uint32_t>(
        totalFrames - 1U >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : totalFrames - 1U);
    const std::uint32_t boundedEnd = endFrame > lastFrame
        ? lastFrame
        : endFrame;
    return requestedFrame > boundedEnd ? boundedEnd : requestedFrame;
}

[[nodiscard]] inline constexpr std::uint32_t ClampPlaybackEndHandle(
    const std::uint32_t requestedFrame,
    const std::uint32_t startFrame,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0U) {
        return 0U;
    }
    const std::uint32_t lastFrame = static_cast<std::uint32_t>(
        totalFrames - 1U >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : totalFrames - 1U);
    const std::uint32_t boundedStart = startFrame > lastFrame
        ? lastFrame
        : startFrame;
    if (requestedFrame < boundedStart) {
        return boundedStart;
    }
    return requestedFrame > lastFrame ? lastFrame : requestedFrame;
}

// Converts a user-entered 1-based start frame into the engine's 0-based
// frame index. The start endpoint cannot move past the current end endpoint.
[[nodiscard]] inline constexpr std::uint32_t
ClampPlaybackStartInputOneBased(
    const std::int64_t requestedFrame,
    const std::uint32_t endFrame,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0U) {
        return 0U;
    }

    const std::uint32_t lastFrame = static_cast<std::uint32_t>(
        totalFrames - 1U >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : totalFrames - 1U);
    const std::uint32_t boundedEnd = endFrame > lastFrame
        ? lastFrame
        : endFrame;
    const std::int64_t maximumOneBased =
        static_cast<std::int64_t>(boundedEnd) + 1;
    const std::int64_t bounded = requestedFrame < 1
        ? 1
        : (requestedFrame > maximumOneBased
                ? maximumOneBased
                : requestedFrame);
    return static_cast<std::uint32_t>(bounded - 1);
}

// Converts a user-entered 1-based end frame into the engine's 0-based frame
// index. The end endpoint cannot move before the current start endpoint.
[[nodiscard]] inline constexpr std::uint32_t
ClampPlaybackEndInputOneBased(
    const std::int64_t requestedFrame,
    const std::uint32_t startFrame,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0U) {
        return 0U;
    }

    const std::uint32_t lastFrame = static_cast<std::uint32_t>(
        totalFrames - 1U >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : totalFrames - 1U);
    const std::uint32_t boundedStart = startFrame > lastFrame
        ? lastFrame
        : startFrame;
    const std::int64_t minimumOneBased =
        static_cast<std::int64_t>(boundedStart) + 1;
    const std::int64_t maximumOneBased =
        static_cast<std::int64_t>(lastFrame) + 1;
    const std::int64_t bounded = requestedFrame < minimumOneBased
        ? minimumOneBased
        : (requestedFrame > maximumOneBased
                ? maximumOneBased
                : requestedFrame);
    return static_cast<std::uint32_t>(bounded - 1);
}

[[nodiscard]] inline constexpr std::uint32_t ScrubTargetFrame(
    const std::uint32_t originFrame,
    const float horizontalDeltaPixels,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0U) {
        return 0U;
    }

    const auto deltaFrames = static_cast<std::int64_t>(
        horizontalDeltaPixels * kViewportScrubFramesPerPixel);
    const auto maximumFrame = static_cast<std::int64_t>(totalFrames - 1U);
    const auto requested = static_cast<std::int64_t>(originFrame) + deltaFrames;
    const auto clamped = requested < 0
        ? 0
        : (requested > maximumFrame ? maximumFrame : requested);
    return static_cast<std::uint32_t>(clamped);
}

struct FittedSize final {
    float width = 0.0F;
    float height = 0.0F;
};

[[nodiscard]] inline constexpr FittedSize FitInside(
    const float sourceWidth,
    const float sourceHeight,
    const float availableWidth,
    const float availableHeight) noexcept {
    if (sourceWidth <= 0.0F || sourceHeight <= 0.0F ||
        availableWidth <= 0.0F || availableHeight <= 0.0F) {
        return {};
    }

    const float widthScale = availableWidth / sourceWidth;
    const float heightScale = availableHeight / sourceHeight;
    const float scale = widthScale < heightScale ? widthScale : heightScale;
    return {sourceWidth * scale, sourceHeight * scale};
}

}  // namespace zt::sequence::ui_detail
