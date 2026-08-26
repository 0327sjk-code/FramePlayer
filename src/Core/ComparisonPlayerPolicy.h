#pragma once

#include "Core/PlayerEnginePolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] inline PlaybackRange EffectivePlaybackRange(
    const PlaybackRange rangeIntent,
    const std::size_t commonTotalFrames) noexcept {
    return detail::NormalizePlaybackRange(rangeIntent, commonTotalFrames);
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
