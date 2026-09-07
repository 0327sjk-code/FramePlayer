#pragma once

#include "Core/PlayerEnginePolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace zt::sequence::comparison_detail {

inline constexpr std::uint64_t kBackgroundMemoryLimitBytes =
    8ULL * kBytesPerGiB;

[[nodiscard]] inline constexpr std::uint64_t EffectiveMemoryLimitBytes(
    const std::uint64_t configuredLimitBytes,
    const bool backgroundResourceMode) noexcept {
    return backgroundResourceMode
        ? std::min(configuredLimitBytes, kBackgroundMemoryLimitBytes)
        : configuredLimitBytes;
}

struct MemorySplit final {
    std::uint64_t globalLimitBytes = kDefaultMemoryLimitBytes;
    std::uint64_t usableCacheBytes = 0U;
    std::uint64_t primaryCacheBytes = 0U;
    std::uint64_t secondaryCacheBytes = 0U;
};

[[nodiscard]] inline MemorySplit CalculateMemorySplit(
    const std::uint64_t globalLimitBytes,
    const bool comparisonEnabled) noexcept {
    const std::uint64_t usable = detail::CacheCapacityForTotalLimit(
        globalLimitBytes);
    if (!comparisonEnabled) {
        return MemorySplit{
            globalLimitBytes,
            usable,
            usable,
            0U};
    }

    const std::uint64_t primary = usable / 2U;
    return MemorySplit{
        globalLimitBytes,
        usable,
        primary,
        usable - primary};
}

[[nodiscard]] inline std::size_t CommonTotalFrames(
    const std::size_t primaryFrames,
    const std::size_t secondaryFrames,
    const bool comparisonActive) noexcept {
    return comparisonActive
        ? std::max(primaryFrames, secondaryFrames)
        : primaryFrames;
}

struct SequenceFrameOffsetDomain final {
    bool available = false;
    bool onPrimary = false;
    FrameIndex maximum = 0U;
};

[[nodiscard]] inline constexpr SequenceFrameOffsetDomain
ResolveSequenceFrameOffsetDomain(
    const SourceKind primaryKind,
    const std::size_t primaryFrames,
    const SourceKind secondaryKind,
    const std::size_t secondaryFrames,
    const bool comparisonActive) noexcept {
    const bool primarySequence = primaryKind == SourceKind::PngSequence;
    const bool secondarySequence = secondaryKind == SourceKind::PngSequence;
    if (!comparisonActive || primarySequence == secondarySequence) {
        return {};
    }

    const bool onPrimary = primarySequence;
    const std::size_t sequenceFrames = onPrimary
        ? primaryFrames
        : secondaryFrames;
    if (sequenceFrames == 0U) {
        return {};
    }

    const std::size_t maximum = std::min<std::size_t>(
        sequenceFrames - 1U,
        static_cast<std::size_t>(
            std::numeric_limits<FrameIndex>::max()));
    return SequenceFrameOffsetDomain{
        true,
        onPrimary,
        static_cast<FrameIndex>(maximum)};
}

[[nodiscard]] inline constexpr bool LaneUsesSequenceFrameOffset(
    const SourceKind laneKind,
    const bool primaryLane,
    const SequenceFrameOffsetDomain domain) noexcept {
    return domain.available && laneKind == SourceKind::PngSequence &&
        domain.onPrimary == primaryLane;
}

struct LaneFrameMapping final {
    bool exists = false;
    FrameIndex sourceFrame = 0U;
};

[[nodiscard]] inline constexpr LaneFrameMapping MapSharedFrameToLane(
    const FrameIndex sharedFrame,
    const std::size_t laneTotalFrames,
    const bool applySequenceOffset,
    const FrameIndex sequenceFrameOffset) noexcept {
    const std::uint64_t mappedFrame =
        static_cast<std::uint64_t>(sharedFrame) +
        (applySequenceOffset
            ? static_cast<std::uint64_t>(sequenceFrameOffset)
            : 0ULL);
    if (mappedFrame >= static_cast<std::uint64_t>(laneTotalFrames) ||
        mappedFrame > static_cast<std::uint64_t>(
            std::numeric_limits<FrameIndex>::max())) {
        return {};
    }
    return LaneFrameMapping{
        true,
        static_cast<FrameIndex>(mappedFrame)};
}

[[nodiscard]] inline constexpr std::size_t LaneSharedFrameCount(
    const std::size_t laneTotalFrames,
    const bool applySequenceOffset,
    const FrameIndex sequenceFrameOffset) noexcept {
    if (!applySequenceOffset) {
        return laneTotalFrames;
    }
    const std::size_t offset = static_cast<std::size_t>(sequenceFrameOffset);
    return laneTotalFrames > offset ? laneTotalFrames - offset : 0U;
}

[[nodiscard]] inline constexpr std::size_t
CommonTotalFramesWithSequenceOffset(
    const std::size_t primaryFrames,
    const SourceKind primaryKind,
    const std::size_t secondaryFrames,
    const SourceKind secondaryKind,
    const bool comparisonActive,
    const FrameIndex sequenceFrameOffset) noexcept {
    const SequenceFrameOffsetDomain domain =
        ResolveSequenceFrameOffsetDomain(
            primaryKind,
            primaryFrames,
            secondaryKind,
            secondaryFrames,
            comparisonActive);
    if (!domain.available) {
        return CommonTotalFrames(
            primaryFrames,
            secondaryFrames,
            comparisonActive);
    }

    const std::size_t primarySharedFrames = LaneSharedFrameCount(
        primaryFrames,
        LaneUsesSequenceFrameOffset(primaryKind, true, domain),
        sequenceFrameOffset);
    const std::size_t secondarySharedFrames = LaneSharedFrameCount(
        secondaryFrames,
        LaneUsesSequenceFrameOffset(secondaryKind, false, domain),
        sequenceFrameOffset);
    return std::max(primarySharedFrames, secondarySharedFrames);
}

[[nodiscard]] inline constexpr PlaybackRange MapSharedPlaybackRangeToLane(
    const PlaybackRange sharedRange,
    const std::size_t laneTotalFrames,
    const bool applySequenceOffset,
    const FrameIndex sequenceFrameOffset) noexcept {
    if (laneTotalFrames == 0U) {
        return {};
    }

    const std::uint64_t offset = applySequenceOffset
        ? static_cast<std::uint64_t>(sequenceFrameOffset)
        : 0ULL;
    const std::uint64_t maximumFrame = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(laneTotalFrames - 1U),
        static_cast<std::uint64_t>(
            std::numeric_limits<FrameIndex>::max()));
    const std::uint64_t mappedStart = std::min(
        static_cast<std::uint64_t>(sharedRange.startFrame) + offset,
        maximumFrame);
    const std::uint64_t mappedEnd = std::min(
        static_cast<std::uint64_t>(sharedRange.endFrame) + offset,
        maximumFrame);
    return PlaybackRange{
        static_cast<FrameIndex>(mappedStart),
        static_cast<FrameIndex>(std::max(mappedStart, mappedEnd))};
}

[[nodiscard]] inline PlaybackRange EffectivePlaybackRange(
    const PlaybackRange rangeIntent,
    const std::size_t commonTotalFrames) noexcept {
    return detail::NormalizePlaybackRange(rangeIntent, commonTotalFrames);
}

[[nodiscard]] inline constexpr bool IsFullSharedPlaybackRange(
    const PlaybackRange effectiveRange,
    const std::size_t commonTotalFrames) noexcept {
    return effectiveRange == detail::FullPlaybackRange(commonTotalFrames);
}

struct AdvanceResult final {
    FrameIndex target = 0U;
    bool stoppedAtBoundary = false;
};

[[nodiscard]] inline AdvanceResult AdvanceFrame(
    const FrameIndex current,
    const std::int64_t signedSteps,
    const PlaybackRange range,
    const bool loopPlayback) noexcept {
    const std::optional<FrameIndex> target =
        detail::OffsetFrameInPlaybackRange(
            current,
            signedSteps,
            range,
            loopPlayback);
    if (target.has_value()) {
        return AdvanceResult{*target, false};
    }

    const std::int64_t unclamped =
        static_cast<std::int64_t>(current) + signedSteps;
    return AdvanceResult{
        detail::ClampFrameToPlaybackRange(unclamped, range),
        true};
}

}  // namespace zt::sequence::comparison_detail
