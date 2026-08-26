#include "Core/ComparisonPlayer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using zt::sequence::ComparisonPlayer;
using zt::sequence::ComparisonPlayerSnapshot;
using zt::sequence::FrameIndex;
using zt::sequence::SourceKind;
using zt::sequence::kBytesPerGiB;

constexpr std::size_t kRapidScrubUpdates = 1500U;
constexpr std::uint64_t kSmokeMemoryLimitBytes = 1ULL * kBytesPerGiB;
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

[[nodiscard]] bool LaneMatchesRequested(
    const zt::sequence::PlayerSnapshot& lane,
    const bool available,
    const std::shared_ptr<const zt::sequence::DecodedFrame>& displayed,
    const FrameIndex requestedFrame) noexcept {
    const bool frameExists =
        static_cast<std::size_t>(requestedFrame) < lane.totalFrames;
    if (!frameExists) {
        return !available && displayed == nullptr;
    }
    return available && displayed != nullptr &&
        displayed->generation == lane.generation &&
        displayed->index == requestedFrame;
}

[[nodiscard]] bool PairMatchesRequested(
    const ComparisonPlayerSnapshot& snapshot,
    const FrameIndex requestedFrame) noexcept {
    return snapshot.active && snapshot.pairReady &&
        snapshot.currentFrame == requestedFrame &&
        snapshot.requestedFrame == requestedFrame &&
        LaneMatchesRequested(
            snapshot.primary,
            snapshot.primaryFrameAvailable,
            snapshot.primaryDisplayFrame,
            requestedFrame) &&
        LaneMatchesRequested(
            snapshot.secondary,
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
    const std::size_t primaryFrames = snapshot.primary.totalFrames;
    const std::size_t secondaryFrames = snapshot.secondary.totalFrames;
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
        passed &= VerifyRapidScrub(player, snapshot, timings);
        passed &= VerifyPlaybackAndFrameSteps(player, snapshot);
        passed &= VerifyShortSideBlackFrame(player, snapshot);
        passed &= Expect(
            snapshot.cacheBytes <= snapshot.memoryLimitBytes,
            "combined lane caches stay within the configured memory limit");
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
