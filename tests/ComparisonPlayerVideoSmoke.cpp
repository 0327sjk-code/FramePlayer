#include "Core/ComparisonPlayer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <variant>

namespace {

using namespace std::chrono_literals;
using zt::sequence::ComparisonPlayer;
using zt::sequence::ComparisonPlayerSnapshot;
using zt::sequence::ExportSourceSnapshot;
using zt::sequence::FrameIndex;
using zt::sequence::Generation;
using zt::sequence::PlaybackRange;
using zt::sequence::SequenceExportSnapshot;
using zt::sequence::SourceKind;
using zt::sequence::VideoExportSnapshot;
using zt::sequence::kBytesPerGiB;

constexpr std::size_t kRapidScrubUpdates = 1500U;
constexpr std::uint64_t kSmokeMemoryLimitBytes = 1ULL * kBytesPerGiB;
constexpr FrameIndex kRequestedSequenceFrameOffset = 200U;
constexpr auto kLoadTimeout = 30s;
constexpr auto kScrubSettleTimeout = 30s;
constexpr auto kTransportTimeout = 15s;

struct ScenarioTimings final {
    std::chrono::steady_clock::duration load{};
    std::chrono::steady_clock::duration scrubSubmission{};
    std::chrono::steady_clock::duration scrubSettlement{};
    std::chrono::steady_clock::duration shutdown{};
};

[[nodiscard]] double Milliseconds(
    const std::chrono::steady_clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] bool Expect(
    const bool condition,
    const std::string_view label) {
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
    }
    return condition;
}

template <typename Predicate>
[[nodiscard]] bool WaitFor(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot,
    Predicate predicate,
    const std::chrono::steady_clock::duration timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        player.Tick(1.0 / 120.0);
        snapshot = player.Snapshot();
        if (predicate(snapshot)) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    snapshot = player.Snapshot();
    return false;
}

[[nodiscard]] bool LaneUsesSequenceOffset(
    const ComparisonPlayerSnapshot& snapshot,
    const bool primaryLane) noexcept {
    return snapshot.sequenceFrameOffsetAvailable &&
        snapshot.sequenceFrameOffsetOnPrimary == primaryLane;
}

[[nodiscard]] std::optional<FrameIndex> ExpectedLaneSourceFrame(
    const ComparisonPlayerSnapshot& snapshot,
    const zt::sequence::PlayerSnapshot& lane,
    const bool primaryLane,
    const FrameIndex sharedFrame) noexcept {
    const std::uint64_t offset = LaneUsesSequenceOffset(snapshot, primaryLane)
        ? snapshot.sequenceFrameOffset
        : 0U;
    const std::uint64_t sourceFrame =
        static_cast<std::uint64_t>(sharedFrame) + offset;
    if (sourceFrame >= lane.totalFrames ||
        sourceFrame > std::numeric_limits<FrameIndex>::max()) {
        return std::nullopt;
    }
    return static_cast<FrameIndex>(sourceFrame);
}

[[nodiscard]] std::size_t LaneSharedFrameCount(
    const ComparisonPlayerSnapshot& snapshot,
    const zt::sequence::PlayerSnapshot& lane,
    const bool primaryLane) noexcept {
    if (!LaneUsesSequenceOffset(snapshot, primaryLane)) {
        return lane.totalFrames;
    }
    return snapshot.sequenceFrameOffset < lane.totalFrames
        ? lane.totalFrames - snapshot.sequenceFrameOffset
        : 0U;
}

[[nodiscard]] bool LaneMatchesRequested(
    const ComparisonPlayerSnapshot& snapshot,
    const zt::sequence::PlayerSnapshot& lane,
    const bool primaryLane,
    const bool available,
    const std::shared_ptr<const zt::sequence::DecodedFrame>& displayed,
    const FrameIndex sharedFrame) noexcept {
    const std::optional<FrameIndex> expected = ExpectedLaneSourceFrame(
        snapshot,
        lane,
        primaryLane,
        sharedFrame);
    if (!expected.has_value()) {
        return !available && displayed == nullptr;
    }
    return available && displayed != nullptr &&
        displayed->generation == lane.generation &&
        displayed->index == *expected;
}

[[nodiscard]] bool PairMatchesRequested(
    const ComparisonPlayerSnapshot& snapshot,
    const FrameIndex requestedFrame) noexcept {
    return snapshot.active && snapshot.pairReady &&
        snapshot.currentFrame == requestedFrame &&
        snapshot.requestedFrame == requestedFrame &&
        LaneMatchesRequested(
            snapshot,
            snapshot.primary,
            true,
            snapshot.primaryFrameAvailable,
            snapshot.primaryDisplayFrame,
            requestedFrame) &&
        LaneMatchesRequested(
            snapshot,
            snapshot.secondary,
            false,
            snapshot.secondaryFrameAvailable,
            snapshot.secondaryDisplayFrame,
            requestedFrame);
}

[[nodiscard]] FrameIndex ScrubTarget(
    const std::size_t update,
    const std::size_t totalFrames) noexcept {
    constexpr std::size_t kScrubStride = 397U;
    return static_cast<FrameIndex>((update * kScrubStride) % totalFrames);
}

[[nodiscard]] bool LoadActiveComparison(
    ComparisonPlayer& player,
    const std::filesystem::path& primarySource,
    const std::filesystem::path& secondarySource,
    ComparisonPlayerSnapshot& snapshot,
    ScenarioTimings& timings) {
    const auto started = std::chrono::steady_clock::now();
    bool passed = true;
    player.SetMemoryLimitBytes(kSmokeMemoryLimitBytes);
    passed &= Expect(
        player.LoadSource(primarySource),
        "submit primary source load");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.primary.hasSource && !value.primary.loading &&
                    value.primary.displayFrame != nullptr;
            },
            kLoadTimeout),
        "commit primary source");
    if (!passed) {
        timings.load = std::chrono::steady_clock::now() - started;
        return false;
    }

    passed &= Expect(
        player.SetComparisonEnabled(true),
        "enable comparison mode");
    passed &= Expect(
        player.LoadSecondarySource(secondarySource),
        "submit secondary source load");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, 0U);
            },
            kLoadTimeout),
        "commit initial frame pair");
    timings.load = std::chrono::steady_clock::now() - started;
    return passed;
}

[[nodiscard]] bool VerifyRapidScrub(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot,
    ScenarioTimings& timings) {
    if (!Expect(snapshot.totalFrames > 2U, "comparison frame count")) {
        return false;
    }

    const FrameIndex finalTarget = static_cast<FrameIndex>(
        (snapshot.totalFrames * 2U) / 3U);
    player.BeginScrub();
    snapshot = player.Snapshot();
    bool passed = Expect(
        snapshot.scrubbing && snapshot.primary.scrubbing &&
            snapshot.secondary.scrubbing && !snapshot.playing,
        "begin scrub pauses both lanes");

    const auto submissionStarted = std::chrono::steady_clock::now();
    for (std::size_t update = 0U; update < kRapidScrubUpdates; ++update) {
        player.UpdateScrub(ScrubTarget(update, snapshot.totalFrames));
    }
    player.UpdateScrub(finalTarget);
    timings.scrubSubmission =
        std::chrono::steady_clock::now() - submissionStarted;
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.scrubbing && snapshot.requestedFrame == finalTarget,
        "rapid scrub retains latest shared target");
    passed &= Expect(
        timings.scrubSubmission < 2s,
        "rapid scrub submission remains non-blocking");

    const auto settlementStarted = std::chrono::steady_clock::now();
    player.EndScrub();
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [finalTarget](const ComparisonPlayerSnapshot& value) {
                return !value.scrubbing &&
                    PairMatchesRequested(value, finalTarget);
            },
            kScrubSettleTimeout),
        "rapid scrub settles atomically on final pair");
    timings.scrubSettlement =
        std::chrono::steady_clock::now() - settlementStarted;
    return passed;
}

[[nodiscard]] bool VerifyPlaybackAndFrameSteps(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    bool passed = true;
    const FrameIndex frameBeforePlayback = snapshot.currentFrame;
    const std::uint64_t revisionBeforePlayback = snapshot.pairRevision;
    player.SetPlaying(true);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [frameBeforePlayback, revisionBeforePlayback](
                const ComparisonPlayerSnapshot& value) {
                return value.playing && value.pairReady &&
                    value.pairRevision > revisionBeforePlayback &&
                    value.currentFrame != frameBeforePlayback &&
                    value.currentFrame == value.requestedFrame;
            },
            kTransportTimeout),
        "playback resumes with synchronized pairs after scrub");
    player.SetPlaying(false);

    const FrameIndex stepBase = static_cast<FrameIndex>(
        std::min<std::size_t>(
            snapshot.totalFrames - 2U,
            snapshot.totalFrames / 3U));
    player.Seek(stepBase);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [stepBase](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, stepBase);
            },
            kTransportTimeout),
        "prepare synchronized frame-step base");

    const FrameIndex forwardTarget = stepBase + 1U;
    player.StepFrame(1);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [forwardTarget](const ComparisonPlayerSnapshot& value) {
                return !value.playing &&
                    PairMatchesRequested(value, forwardTarget);
            },
            kTransportTimeout),
        "step forward exactly one synchronized frame");

    player.StepFrame(-1);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [stepBase](const ComparisonPlayerSnapshot& value) {
                return !value.playing &&
                    PairMatchesRequested(value, stepBase);
            },
            kTransportTimeout),
        "step backward exactly one synchronized frame");
    return passed;
}

[[nodiscard]] bool VerifyShortSideBlackFrame(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    const std::size_t primaryFrames = LaneSharedFrameCount(
        snapshot,
        snapshot.primary,
        true);
    const std::size_t secondaryFrames = LaneSharedFrameCount(
        snapshot,
        snapshot.secondary,
        false);
    if (primaryFrames == secondaryFrames) {
        std::cout << "short-side check skipped: lane durations are equal\n";
        return true;
    }

    const std::size_t shorterFrames = std::min(primaryFrames, secondaryFrames);
    const std::size_t longerFrames = std::max(primaryFrames, secondaryFrames);
    if (!Expect(
            shorterFrames < longerFrames &&
                shorterFrames <=
                    static_cast<std::size_t>(
                        std::numeric_limits<FrameIndex>::max()),
            "different lane durations provide a representable black target")) {
        return false;
    }

    const FrameIndex blackTarget = static_cast<FrameIndex>(shorterFrames);
    player.Seek(blackTarget);
    bool passed = Expect(
        WaitFor(
            player,
            snapshot,
            [blackTarget](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, blackTarget);
            },
            kTransportTimeout),
        "commit first out-of-range frame as synchronized black pair");
    if (primaryFrames < secondaryFrames) {
        passed &= Expect(
            !snapshot.primaryFrameAvailable &&
                snapshot.primaryDisplayFrame == nullptr &&
                snapshot.secondaryFrameAvailable,
            "short primary lane is black while secondary remains visible");
    } else {
        passed &= Expect(
            snapshot.primaryFrameAvailable &&
                !snapshot.secondaryFrameAvailable &&
                snapshot.secondaryDisplayFrame == nullptr,
            "short secondary lane is black while primary remains visible");
    }

    if (shorterFrames > 0U) {
        const FrameIndex recoveryTarget = static_cast<FrameIndex>(
            shorterFrames - 1U);
        player.Seek(recoveryTarget);
        passed &= Expect(
            WaitFor(
                player,
                snapshot,
                [recoveryTarget](const ComparisonPlayerSnapshot& value) {
                    return PairMatchesRequested(value, recoveryTarget) &&
                        value.primaryFrameAvailable &&
                        value.secondaryFrameAvailable;
                },
                kTransportTimeout),
            "seeking back before the mapped boundary restores both lanes");
    }
    return passed;
}

[[nodiscard]] bool ConfigureSequenceFrameOffset(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot,
    const SourceKind expectedPrimaryKind,
    const SourceKind expectedSecondaryKind) {
    const bool sequenceOnPrimary =
        expectedPrimaryKind == SourceKind::PngSequence &&
        expectedSecondaryKind == SourceKind::Video;
    const bool sequenceOnSecondary =
        expectedPrimaryKind == SourceKind::Video &&
        expectedSecondaryKind == SourceKind::PngSequence;
    const bool mixedSources = sequenceOnPrimary || sequenceOnSecondary;
    const std::size_t previousTotalFrames = snapshot.totalFrames;

    if (!mixedSources) {
        bool passed = Expect(
            !snapshot.sequenceFrameOffsetAvailable,
            "sequence offset is unavailable without one sequence and one video");
        player.SetComparisonSequenceFrameOffset(
            kRequestedSequenceFrameOffset);
        snapshot = player.Snapshot();
        passed &= Expect(
            !snapshot.sequenceFrameOffsetAvailable &&
                snapshot.totalFrames == previousTotalFrames &&
                PairMatchesRequested(snapshot, snapshot.requestedFrame),
            "unavailable sequence offset command leaves video comparison unchanged");
        return passed;
    }

    const zt::sequence::PlayerSnapshot& sequenceLane = sequenceOnPrimary
        ? snapshot.primary
        : snapshot.secondary;
    const FrameIndex expectedMaximum = sequenceLane.totalFrames > 0U
        ? static_cast<FrameIndex>(sequenceLane.totalFrames - 1U)
        : 0U;
    const FrameIndex expectedOffset = std::min(
        kRequestedSequenceFrameOffset,
        expectedMaximum);
    bool passed = Expect(
        snapshot.sequenceFrameOffsetAvailable &&
            snapshot.sequenceFrameOffsetOnPrimary == sequenceOnPrimary &&
            snapshot.maximumSequenceFrameOffset == expectedMaximum,
        "mixed comparison exposes the sequence lane offset domain");

    const Generation primaryGeneration = snapshot.primary.generation;
    const Generation secondaryGeneration = snapshot.secondary.generation;
    const FrameIndex sharedFrame = snapshot.requestedFrame;
    const std::uint64_t revisionBeforeOffset = snapshot.pairRevision;
    player.SetComparisonSequenceFrameOffset(expectedOffset);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [expectedOffset, sharedFrame, revisionBeforeOffset](
                const ComparisonPlayerSnapshot& value) {
                return value.sequenceFrameOffset == expectedOffset &&
                    value.requestedFrame == sharedFrame &&
                    value.pairRevision > revisionBeforeOffset &&
                    PairMatchesRequested(value, sharedFrame);
            },
            kTransportTimeout),
        "sequence offset remaps the current pair without moving shared time");

    const std::size_t expectedPrimaryFrames = LaneSharedFrameCount(
        snapshot,
        snapshot.primary,
        true);
    const std::size_t expectedSecondaryFrames = LaneSharedFrameCount(
        snapshot,
        snapshot.secondary,
        false);
    passed &= Expect(
        snapshot.totalFrames ==
            std::max(expectedPrimaryFrames, expectedSecondaryFrames),
        "shared duration uses the sequence frames remaining after its offset");
    passed &= Expect(
        snapshot.primary.generation == primaryGeneration &&
            snapshot.secondary.generation == secondaryGeneration,
        "changing sequence offset does not reload either source");
    return passed;
}

[[nodiscard]] bool VerifyOffsetChangePreservesVideoFrame(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    if (!snapshot.sequenceFrameOffsetAvailable) {
        return true;
    }

    const bool sequenceOnPrimary = snapshot.sequenceFrameOffsetOnPrimary;
    const zt::sequence::PlayerSnapshot& videoLane = sequenceOnPrimary
        ? snapshot.secondary
        : snapshot.primary;
    if (videoLane.totalFrames == 0U) {
        return Expect(false, "mixed comparison video lane has frames");
    }
    const FrameIndex sharedTarget = static_cast<FrameIndex>(
        std::min<std::size_t>(100U, videoLane.totalFrames - 1U));
    player.Seek(sharedTarget);
    bool passed = Expect(
        WaitFor(
            player,
            snapshot,
            [sharedTarget](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, sharedTarget);
            },
            kTransportTimeout),
        "prepare mapped pair before changing sequence offset");
    if (!passed) {
        return false;
    }

    const FrameIndex originalOffset = snapshot.sequenceFrameOffset;
    const FrameIndex changedOffset = originalOffset <
            snapshot.maximumSequenceFrameOffset
        ? static_cast<FrameIndex>(std::min<std::uint64_t>(
              snapshot.maximumSequenceFrameOffset,
              static_cast<std::uint64_t>(originalOffset) + 17U))
        : (originalOffset > 0U ? originalOffset - 1U : originalOffset);
    if (changedOffset == originalOffset) {
        std::cout << "offset-change check skipped: sequence has one frame\n";
        return true;
    }

    const Generation primaryGeneration = snapshot.primary.generation;
    const Generation secondaryGeneration = snapshot.secondary.generation;
    const std::uint64_t revisionBeforeChange = snapshot.pairRevision;
    player.SetComparisonSequenceFrameOffset(changedOffset);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [sharedTarget, changedOffset, revisionBeforeChange](
                const ComparisonPlayerSnapshot& value) {
                return value.sequenceFrameOffset == changedOffset &&
                    value.requestedFrame == sharedTarget &&
                    value.pairRevision > revisionBeforeChange &&
                    PairMatchesRequested(value, sharedTarget);
            },
            kTransportTimeout),
        "offset change commits a new sequence frame at the same shared frame");
    const std::shared_ptr<const zt::sequence::DecodedFrame> videoDisplay =
        sequenceOnPrimary
        ? snapshot.secondaryDisplayFrame
        : snapshot.primaryDisplayFrame;
    passed &= Expect(
        videoDisplay != nullptr && videoDisplay->index == sharedTarget,
        "changing sequence offset leaves the video on the shared frame");
    passed &= Expect(
        snapshot.primary.generation == primaryGeneration &&
            snapshot.secondary.generation == secondaryGeneration,
        "offset remapping keeps both source generations stable");

    player.SetComparisonSequenceFrameOffset(originalOffset);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [sharedTarget, originalOffset](
                const ComparisonPlayerSnapshot& value) {
                return value.sequenceFrameOffset == originalOffset &&
                    PairMatchesRequested(value, sharedTarget);
            },
            kTransportTimeout),
        "restore requested sequence offset after remap check");
    return passed;
}

[[nodiscard]] bool VerifyPlaybackRangeMapping(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    if (snapshot.totalFrames < 4U) {
        std::cout << "playback-range check skipped: comparison is too short\n";
        return true;
    }

    const double originalFramesPerSecond = snapshot.targetFramesPerSecond;
    const FrameIndex rangeStart = static_cast<FrameIndex>(
        std::min<std::size_t>(100U, snapshot.totalFrames - 2U));
    const FrameIndex rangeEnd = static_cast<FrameIndex>(
        std::min<std::size_t>(
            static_cast<std::size_t>(rangeStart) + 200U,
            snapshot.totalFrames - 1U));
    player.SetPlaybackRange(rangeStart, rangeEnd);
    snapshot = player.Snapshot();

    const auto expectedRangeForLane = [&snapshot, rangeStart, rangeEnd](
                                          const zt::sequence::PlayerSnapshot& lane,
                                          const bool primaryLane) {
        const std::uint64_t offset =
            LaneUsesSequenceOffset(snapshot, primaryLane)
            ? snapshot.sequenceFrameOffset
            : 0U;
        const FrameIndex maximum = lane.totalFrames > 0U
            ? static_cast<FrameIndex>(lane.totalFrames - 1U)
            : 0U;
        return PlaybackRange{
            static_cast<FrameIndex>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(rangeStart) + offset,
                maximum)),
            static_cast<FrameIndex>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(rangeEnd) + offset,
                maximum))};
    };
    const PlaybackRange expectedPrimaryRange = expectedRangeForLane(
        snapshot.primary,
        true);
    const PlaybackRange expectedSecondaryRange = expectedRangeForLane(
        snapshot.secondary,
        false);
    bool passed = Expect(
        snapshot.primary.playbackStartFrame == expectedPrimaryRange.startFrame &&
            snapshot.primary.playbackEndFrame == expectedPrimaryRange.endFrame &&
            snapshot.secondary.playbackStartFrame ==
                expectedSecondaryRange.startFrame &&
            snapshot.secondary.playbackEndFrame == expectedSecondaryRange.endFrame,
        "shared playback range maps only the sequence lane through its offset");

    player.SetLoopPlayback(false);
    player.SetFramesPerSecond(120.0);
    player.Seek(rangeEnd - 1U);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [rangeEnd](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, rangeEnd - 1U);
            },
            kTransportTimeout),
        "prepare mapped non-loop playback boundary");
    player.SetPlaying(true);
    player.Tick(0.02);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [rangeEnd](const ComparisonPlayerSnapshot& value) {
                return !value.playing &&
                    PairMatchesRequested(value, rangeEnd);
            },
            kTransportTimeout),
        "non-loop playback stops on the shared range end with mapped lanes");

    player.SetLoopPlayback(true);
    player.SetPlaying(true);
    player.Tick(0.01);
    player.SetPlaying(false);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [rangeStart](const ComparisonPlayerSnapshot& value) {
                return !value.playing &&
                    PairMatchesRequested(value, rangeStart);
            },
            kTransportTimeout),
        "loop playback wraps to the shared start with mapped lanes");

    player.SetPlaying(false);
    player.SetFramesPerSecond(originalFramesPerSecond);
    player.SetPlaybackRange(
        0U,
        static_cast<FrameIndex>(snapshot.totalFrames - 1U));
    return passed;
}

[[nodiscard]] bool VerifyDecodePercentPreservesOffset(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    if (!snapshot.sequenceFrameOffsetAvailable) {
        return true;
    }

    const FrameIndex offset = snapshot.sequenceFrameOffset;
    const FrameIndex sharedFrame = snapshot.requestedFrame;
    const std::uint32_t originalPercent = snapshot.decodePercent;
    const std::uint32_t alternatePercent = originalPercent == 50U ? 100U : 50U;
    player.SetDecodePercent(alternatePercent);
    bool passed = Expect(
        WaitFor(
            player,
            snapshot,
            [alternatePercent, offset, sharedFrame](
                const ComparisonPlayerSnapshot& value) {
                return !value.primary.loading && !value.secondary.loading &&
                    value.decodePercent == alternatePercent &&
                    value.primary.decodePercent == alternatePercent &&
                    value.secondary.decodePercent == alternatePercent &&
                    value.sequenceFrameOffset == offset &&
                    PairMatchesRequested(value, sharedFrame);
            },
            kLoadTimeout),
        "decode percentage transaction preserves sequence offset mapping");

    player.SetDecodePercent(originalPercent);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [originalPercent, offset, sharedFrame](
                const ComparisonPlayerSnapshot& value) {
                return !value.primary.loading && !value.secondary.loading &&
                    value.decodePercent == originalPercent &&
                    value.primary.decodePercent == originalPercent &&
                    value.secondary.decodePercent == originalPercent &&
                    value.sequenceFrameOffset == offset &&
                    PairMatchesRequested(value, sharedFrame);
            },
            kLoadTimeout),
        "restoring decode percentage keeps the same mapped comparison pair");
    return passed;
}

[[nodiscard]] bool VerifyExportRangeIgnoresSequenceOffset(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot,
    const SourceKind expectedPrimaryKind) {
    if (snapshot.totalFrames < 31U) {
        std::cout << "export-range check skipped: comparison is too short\n";
        return true;
    }

    constexpr PlaybackRange kExportRange{10U, 30U};
    player.SetPlaybackRange(
        kExportRange.startFrame,
        kExportRange.endFrame);
    const std::optional<ExportSourceSnapshot> captured =
        player.CaptureExportSnapshot();
    bool passed = Expect(
        captured.has_value(),
        "capture primary export snapshot while comparison offset is active");
    if (captured.has_value()) {
        const PlaybackRange capturedRange = std::visit(
            [](const auto& value) { return value.inclusiveRange; },
            *captured);
        passed &= Expect(
            capturedRange == kExportRange,
            "sequence comparison offset does not shift the export range");
        passed &= Expect(
            (expectedPrimaryKind == SourceKind::PngSequence &&
             std::holds_alternative<SequenceExportSnapshot>(*captured)) ||
                (expectedPrimaryKind == SourceKind::Video &&
                 std::holds_alternative<VideoExportSnapshot>(*captured)),
            "comparison export snapshot keeps the primary source type");
    }
    player.SetPlaybackRange(
        0U,
        static_cast<FrameIndex>(snapshot.totalFrames - 1U));
    snapshot = player.Snapshot();
    return passed;
}

[[nodiscard]] bool VerifyComparisonExitReturnsToSharedFrame(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot) {
    if (snapshot.totalFrames == 0U) {
        return Expect(false, "comparison has a frame before exit");
    }

    const FrameIndex sharedFrame = static_cast<FrameIndex>(
        std::min<std::size_t>(100U, snapshot.totalFrames - 1U));
    player.Seek(sharedFrame);
    bool passed = Expect(
        WaitFor(
            player,
            snapshot,
            [sharedFrame](const ComparisonPlayerSnapshot& value) {
                return PairMatchesRequested(value, sharedFrame);
            },
            kTransportTimeout),
        "prepare mapped pair before leaving comparison mode");
    const Generation primaryGeneration = snapshot.primary.generation;
    passed &= Expect(
        player.SetComparisonEnabled(false),
        "leave comparison mode after mapped playback");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [sharedFrame](const ComparisonPlayerSnapshot& value) {
                return !value.enabled && !value.primary.loading &&
                    value.primary.displayFrame != nullptr &&
                    value.primary.currentFrame == sharedFrame &&
                    value.primary.requestedFrame == sharedFrame &&
                    value.primary.displayFrame->index == sharedFrame;
            },
            kTransportTimeout),
        "comparison exit removes the comparison-only offset and returns to the shared frame");
    passed &= Expect(
        snapshot.primary.generation == primaryGeneration,
        "comparison exit does not reload the primary source");
    return passed;
}

[[nodiscard]] bool RunScenario(
    const std::string_view label,
    const std::filesystem::path& primarySource,
    const std::filesystem::path& secondarySource,
    const SourceKind expectedPrimaryKind,
    const SourceKind expectedSecondaryKind) {
    std::cout << "scenario " << label << " started\n";
    const auto scenarioStarted = std::chrono::steady_clock::now();
    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    ScenarioTimings timings;

    bool passed = LoadActiveComparison(
        player,
        primarySource,
        secondarySource,
        snapshot,
        timings);
    if (passed) {
        passed &= Expect(
            snapshot.primary.sourceKind == expectedPrimaryKind &&
                snapshot.secondary.sourceKind == expectedSecondaryKind,
            "comparison lane source kinds");
        passed &= Expect(
            snapshot.memoryLimitBytes == kSmokeMemoryLimitBytes,
            "comparison uses bounded one-GiB smoke budget");
        passed &= ConfigureSequenceFrameOffset(
            player,
            snapshot,
            expectedPrimaryKind,
            expectedSecondaryKind);
        passed &= VerifyOffsetChangePreservesVideoFrame(player, snapshot);
        passed &= VerifyRapidScrub(player, snapshot, timings);
        passed &= VerifyPlaybackAndFrameSteps(player, snapshot);
        passed &= VerifyPlaybackRangeMapping(player, snapshot);
        passed &= VerifyDecodePercentPreservesOffset(player, snapshot);
        passed &= VerifyShortSideBlackFrame(player, snapshot);
        passed &= VerifyExportRangeIgnoresSequenceOffset(
            player,
            snapshot,
            expectedPrimaryKind);
        passed &= Expect(
            snapshot.cacheBytes <= snapshot.memoryLimitBytes,
            "combined lane caches stay within the configured memory limit");
        passed &= VerifyComparisonExitReturnsToSharedFrame(
            player,
            snapshot);
    }

    const auto shutdownStarted = std::chrono::steady_clock::now();
    player.Shutdown();
    timings.shutdown = std::chrono::steady_clock::now() - shutdownStarted;
    const auto scenarioElapsed =
        std::chrono::steady_clock::now() - scenarioStarted;
    std::cout << std::fixed << std::setprecision(1)
              << "scenario " << label
              << (passed ? " passed" : " failed")
              << " primaryFrames=" << snapshot.primary.totalFrames
              << " secondaryFrames=" << snapshot.secondary.totalFrames
              << " loadMs=" << Milliseconds(timings.load)
              << " scrubSubmitMs=" << Milliseconds(timings.scrubSubmission)
              << " scrubSettleMs=" << Milliseconds(timings.scrubSettlement)
              << " shutdownMs=" << Milliseconds(timings.shutdown)
              << " totalMs=" << Milliseconds(scenarioElapsed) << '\n';
    return passed;
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2 || arguments == nullptr || arguments[1] == nullptr) {
        std::cerr <<
            "usage: ZTComparisonPlayerVideoSmoke <video> [png-folder]\n";
        return 2;
    }

    const std::filesystem::path videoFile(arguments[1]);
    bool passed = RunScenario(
        "video-video",
        videoFile,
        videoFile,
        SourceKind::Video,
        SourceKind::Video);

    if (argumentCount >= 3 && arguments[2] != nullptr) {
        const std::filesystem::path pngFolder(arguments[2]);
        passed &= RunScenario(
            "sequence-video",
            pngFolder,
            videoFile,
            SourceKind::PngSequence,
            SourceKind::Video);
        passed &= RunScenario(
            "video-sequence",
            videoFile,
            pngFolder,
            SourceKind::Video,
            SourceKind::PngSequence);
    }
    return passed ? 0 : 1;
}
