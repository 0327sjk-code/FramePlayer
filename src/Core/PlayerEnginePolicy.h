#pragma once

#include "Core/PlayerTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>

namespace zt::sequence::detail {

inline constexpr double kNormalPlaybackSpeedScale = 1.0;
inline constexpr double kMinimumPlaybackSpeedScale = 0.05;
inline constexpr double kMaximumPlaybackSpeedScale = 4.0;

inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTransientMemoryReserveBytes = 512ULL * kMiB;
inline constexpr std::uint64_t kMinimumAcceptedMemoryLimitBytes = 256ULL * kMiB;
inline constexpr double kMinimumFramesPerSecond = 1.0;
inline constexpr double kMaximumFramesPerSecond = 240.0;
inline constexpr double kForwardPrefetchSeconds = 4.0;
inline constexpr double kPostScrubHotFillSeconds = 2.0;
inline constexpr double kNeighborhoodSeconds = 1.0;
inline constexpr double kFpsSampleSeconds = 0.5;
inline constexpr std::size_t kBackgroundBatchFrames = 4;
inline constexpr std::size_t kMaximumPendingDecodeTasks = 512;
inline constexpr std::size_t kPlayingBackgroundConcurrency = 1;
inline constexpr std::size_t kPausedBackgroundConcurrency = 4;
inline constexpr std::uint64_t kHotWindowPercent = 70;
inline constexpr std::uint64_t kBackwardWindowPercent = 15;
inline constexpr std::uint64_t kBackgroundFillPercentWhenSequenceExceedsRam = 85;
inline constexpr std::uint64_t kBackgroundPrivateUsageLimitPercent = 98;
inline constexpr std::size_t kMaximumPendingBackgroundTasks = 8;
inline constexpr auto kSnapshotTelemetryInterval =
    std::chrono::milliseconds{100};

[[nodiscard]] inline bool ShouldRefreshSnapshotTelemetry(
    const bool initialized,
    const Generation sampledGeneration,
    const Generation currentGeneration,
    const PlaybackRange sampledRange,
    const PlaybackRange currentRange,
    const bool sampledLoopPlayback,
    const bool currentLoopPlayback,
    const std::chrono::steady_clock::duration elapsed) noexcept {
    return !initialized ||
        sampledGeneration != currentGeneration ||
        sampledRange != currentRange ||
        sampledLoopPlayback != currentLoopPlayback ||
        elapsed >= kSnapshotTelemetryInterval;
}

// During single-sequence scrubbing, several lossless PNG targets may already
// be decoding when the mouse moves again. A completed in-flight target is safe
// to present only when it advances the actual picture monotonically toward the
// newest requested frame. Comparison playback keeps its stricter atomic-pair
// commit path by using an external clock, and therefore never enters here.
[[nodiscard]] inline constexpr bool ShouldPresentScrubIntermediateFrame(
    const bool scrubbing,
    const bool externalClockEnabled,
    const SourceKind sourceKind,
    const FrameIndex currentFrame,
    const FrameIndex requestedFrame,
    const FrameIndex completedFrame) noexcept {
    if (!scrubbing || externalClockEnabled ||
        sourceKind != SourceKind::PngSequence ||
        currentFrame == requestedFrame || completedFrame == currentFrame) {
        return false;
    }

    if (currentFrame < requestedFrame) {
        return completedFrame > currentFrame &&
            completedFrame < requestedFrame;
    }
    return completedFrame < currentFrame &&
        completedFrame > requestedFrame;
}

[[nodiscard]] inline std::uint64_t CacheCapacityForTotalLimit(
    const std::uint64_t totalBytes) noexcept {
    if (totalBytes == 0) {
        return 0;
    }
    const std::uint64_t reserve = std::min(kTransientMemoryReserveBytes, totalBytes / 4ULL);
    return totalBytes > reserve ? totalBytes - reserve : totalBytes;
}

[[nodiscard]] inline bool AllowsBackgroundDecode(
    const std::uint64_t privateBytes,
    const std::uint64_t memoryLimitBytes) noexcept {
    if (privateBytes == 0 || memoryLimitBytes == 0) {
        return true;
    }
    const std::uint64_t headroomPercent = 100ULL - kBackgroundPrivateUsageLimitPercent;
    const std::uint64_t threshold = memoryLimitBytes
        - (memoryLimitBytes / 100ULL) * headroomPercent;
    return privateBytes < threshold;
}

[[nodiscard]] inline constexpr std::size_t PercentageOfSize(
    const std::size_t value,
    const std::uint64_t percent) noexcept {
    const std::uint64_t boundedPercent = std::min<std::uint64_t>(percent, 100ULL);
    const std::size_t whole =
        (value / 100U) * static_cast<std::size_t>(boundedPercent);
    const std::size_t remainder =
        ((value % 100U) * static_cast<std::size_t>(boundedPercent)) / 100U;
    return whole + remainder;
}

[[nodiscard]] inline std::size_t RecommendedDecodeWorkerCount() noexcept {
    constexpr std::size_t kMinimumWorkers = 2;
    constexpr std::size_t kMaximumWorkers = 8;
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads <= kMinimumWorkers) {
        return kMinimumWorkers;
    }
    return std::clamp<std::size_t>(
        static_cast<std::size_t>(hardwareThreads - 2U),
        kMinimumWorkers,
        kMaximumWorkers);
}

[[nodiscard]] inline bool IsSupportedDecodePercent(const std::uint32_t percent) noexcept {
    constexpr std::array<std::uint32_t, 4> kSupportedPercents{25U, 50U, 75U, 100U};
    return std::find(kSupportedPercents.begin(), kSupportedPercents.end(), percent)
        != kSupportedPercents.end();
}

[[nodiscard]] inline std::optional<FrameIndex> OffsetFrame(
    const FrameIndex start,
    const std::int64_t offset,
    const std::size_t totalFrames,
    const bool loopPlayback) noexcept {
    if (totalFrames == 0 || totalFrames > std::numeric_limits<FrameIndex>::max()) {
        return std::nullopt;
    }

    const std::int64_t total = static_cast<std::int64_t>(totalFrames);
    std::int64_t target = static_cast<std::int64_t>(start) + offset;
    if (loopPlayback) {
        target %= total;
        if (target < 0) {
            target += total;
        }
        return static_cast<FrameIndex>(target);
    }
    if (target < 0 || target >= total) {
        return std::nullopt;
    }
    return static_cast<FrameIndex>(target);
}

[[nodiscard]] inline FrameIndex ClampFrame(
    const std::int64_t frame,
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0) {
        return 0;
    }
    const std::int64_t maximum = static_cast<std::int64_t>(totalFrames - 1);
    return static_cast<FrameIndex>(std::clamp<std::int64_t>(frame, 0, maximum));
}

[[nodiscard]] inline constexpr PlaybackRange FullPlaybackRange(
    const std::size_t totalFrames) noexcept {
    if (totalFrames == 0) {
        return {};
    }
    const std::size_t maximumIndex = std::min<std::size_t>(
        totalFrames - 1U,
        std::numeric_limits<FrameIndex>::max());
    return {0U, static_cast<FrameIndex>(maximumIndex)};
}

[[nodiscard]] inline constexpr PlaybackRange NormalizePlaybackRange(
    PlaybackRange range,
    const std::size_t totalFrames) noexcept {
    const PlaybackRange fullRange = FullPlaybackRange(totalFrames);
    if (totalFrames == 0) {
        return fullRange;
    }

    range.startFrame = std::min(range.startFrame, fullRange.endFrame);
    range.endFrame = std::min(range.endFrame, fullRange.endFrame);
    if (range.startFrame > range.endFrame) {
        std::swap(range.startFrame, range.endFrame);
    }
    return range;
}

[[nodiscard]] inline constexpr PlaybackRange PlaybackRangeForLoadedSequence(
    const PlaybackRange previousRange,
    const std::size_t totalFrames,
    const bool preservePreviousRange,
    const bool previousRangeCustomized) noexcept {
    return preservePreviousRange && previousRangeCustomized
        ? NormalizePlaybackRange(previousRange, totalFrames)
        : FullPlaybackRange(totalFrames);
}

[[nodiscard]] inline constexpr std::uint64_t PlaybackRangeFrameCount(
    const PlaybackRange range) noexcept {
    return range.endFrame >= range.startFrame
        ? static_cast<std::uint64_t>(range.endFrame) -
            static_cast<std::uint64_t>(range.startFrame) + 1ULL
        : 0ULL;
}

[[nodiscard]] inline constexpr bool PlaybackRangeContains(
    const PlaybackRange range,
    const FrameIndex frame) noexcept {
    return range.endFrame >= range.startFrame &&
        frame >= range.startFrame && frame <= range.endFrame;
}

[[nodiscard]] inline constexpr FrameIndex ClampFrameToPlaybackRange(
    const std::int64_t frame,
    const PlaybackRange range) noexcept {
    if (range.endFrame < range.startFrame) {
        return 0U;
    }
    return static_cast<FrameIndex>(std::clamp<std::int64_t>(
        frame,
        static_cast<std::int64_t>(range.startFrame),
        static_cast<std::int64_t>(range.endFrame)));
}

[[nodiscard]] inline constexpr FrameIndex PlaybackStartForPlay(
    const FrameIndex requestedFrame,
    const PlaybackRange range) noexcept {
    return PlaybackRangeContains(range, requestedFrame)
        ? requestedFrame
        : range.startFrame;
}

[[nodiscard]] inline constexpr std::optional<FrameIndex>
OffsetFrameInPlaybackRange(
    const FrameIndex start,
    const std::int64_t offset,
    const PlaybackRange range,
    const bool loopPlayback) noexcept {
    const std::uint64_t unsignedFrameCount = PlaybackRangeFrameCount(range);
    if (unsignedFrameCount == 0ULL ||
        unsignedFrameCount >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !PlaybackRangeContains(range, start)) {
        return std::nullopt;
    }

    const std::int64_t frameCount = static_cast<std::int64_t>(unsignedFrameCount);
    const std::int64_t relativeStart =
        static_cast<std::int64_t>(start) -
        static_cast<std::int64_t>(range.startFrame);
    std::int64_t relativeTarget = relativeStart;
    if (loopPlayback) {
        relativeTarget += offset % frameCount;
        relativeTarget %= frameCount;
        if (relativeTarget < 0) {
            relativeTarget += frameCount;
        }
    } else {
        if ((offset < 0 && offset < -relativeStart) ||
            (offset >= 0 && offset >= frameCount - relativeStart)) {
            return std::nullopt;
        }
        relativeTarget += offset;
    }

    return static_cast<FrameIndex>(
        static_cast<std::int64_t>(range.startFrame) + relativeTarget);
}

[[nodiscard]] inline bool MultiplicationFits(
    const std::uint64_t left,
    const std::uint64_t right,
    const std::uint64_t limit) noexcept {
    return left == 0 || right <= limit / left;
}

[[nodiscard]] inline constexpr bool CanRetainDecodedFrame(
    const std::uint64_t cacheCapacityBytes,
    const std::uint64_t frameBytes) noexcept {
    return frameBytes == 0U || cacheCapacityBytes >= frameBytes;
}

}  // namespace zt::sequence::detail
