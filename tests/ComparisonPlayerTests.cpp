#include "Core/ComparisonPlayer.h"
#include "Core/ComparisonPlayerPolicy.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using zt::sequence::ComparisonPlayer;
using zt::sequence::ComparisonPlayerSnapshot;
using zt::sequence::FrameIndex;
using zt::sequence::Generation;
using zt::sequence::SourceKind;
using zt::sequence::kDefaultMemoryLimitBytes;

inline constexpr std::array<std::uint8_t, 70> kOnePixelPng{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
    0x54, 0x78, 0xDA, 0x63, 0x64, 0xF8, 0xCF, 0xF0,
    0x1F, 0x00, 0x05, 0x02, 0x02, 0x00, 0x0A, 0x3D,
    0x02, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

class TemporaryComparisonSequences final {
public:
    TemporaryComparisonSequences() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        root_ = std::filesystem::temp_directory_path() /
            ("ZTComparisonPlayer_" + std::to_string(nonce));
        primary_ = root_ / L"primary";
        secondary_ = root_ / L"secondary";
        std::error_code error;
        std::filesystem::create_directories(primary_, error);
        if (!error) {
            std::filesystem::create_directories(secondary_, error);
        }
        ready_ = !error;
    }

    ~TemporaryComparisonSequences() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] bool WritePrimaryFrames(const std::size_t count) const {
        return WriteFrames(primary_, count);
    }

    [[nodiscard]] bool WriteSecondaryFrames(const std::size_t count) const {
        return WriteFrames(secondary_, count);
    }

    [[nodiscard]] const std::filesystem::path& Primary() const noexcept {
        return primary_;
    }

    [[nodiscard]] const std::filesystem::path& Secondary() const noexcept {
        return secondary_;
    }

    [[nodiscard]] bool CorruptSecondaryFrame(
        const std::size_t oneBasedIndex) const {
        if (!ready_ || oneBasedIndex == 0U) {
            return false;
        }
        const std::filesystem::path path = secondary_ /
            (L"Frame." + std::to_wstring(oneBasedIndex) + L".png");
        const std::array<std::uint8_t, 8U> invalidBytes{
            0x89U, 0x50U, 0x4EU, 0x47U, 0x00U, 0x00U, 0x00U, 0x00U};
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(invalidBytes.data()),
            static_cast<std::streamsize>(invalidBytes.size()));
        return output.good();
    }

private:
    [[nodiscard]] bool WriteFrames(
        const std::filesystem::path& folder,
        const std::size_t count) const {
        if (!ready_) {
            return false;
        }
        for (std::size_t index = 0U; index < count; ++index) {
            const std::filesystem::path path = folder /
                (L"Frame." + std::to_wstring(index + 1U) + L".png");
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                return false;
            }
            output.write(
                reinterpret_cast<const char*>(kOnePixelPng.data()),
                static_cast<std::streamsize>(kOnePixelPng.size()));
            if (!output.good()) {
                return false;
            }
        }
        return true;
    }

    std::filesystem::path root_;
    std::filesystem::path primary_;
    std::filesystem::path secondary_;
    bool ready_ = false;
};

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

template <typename Predicate>
[[nodiscard]] bool WaitFor(
    ComparisonPlayer& player,
    ComparisonPlayerSnapshot& snapshot,
    Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        player.Tick(0.001);
        snapshot = player.Snapshot();
        if (predicate(snapshot)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    snapshot = player.Snapshot();
    return false;
}

[[nodiscard]] bool LoadActiveComparison(
    ComparisonPlayer& player,
    const TemporaryComparisonSequences& sequences,
    ComparisonPlayerSnapshot& snapshot) {
    if (!player.LoadFolder(sequences.Primary()) ||
        !WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.primary.hasSource && !value.primary.loading;
            }) ||
        !player.SetComparisonEnabled(true) ||
        !player.LoadSecondarySource(sequences.Secondary())) {
        return false;
    }
    return WaitFor(
        player,
        snapshot,
        [](const ComparisonPlayerSnapshot& value) {
            return value.active && value.pairReady &&
                value.requestedFrame == 0U;
        });
}

[[nodiscard]] bool TestScrubTransactionAndRecovery() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(
        sequences.WritePrimaryFrames(5U),
        "write scrub primary frames");
    passed &= Expect(
        sequences.WriteSecondaryFrames(3U),
        "write scrub secondary frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        LoadActiveComparison(player, sequences, snapshot),
        "load active comparison for scrub transaction");
    if (!passed) {
        player.Shutdown();
        return false;
    }

    player.SetPlaying(true);
    player.BeginScrub();
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.scrubbing && snapshot.primary.scrubbing &&
            snapshot.secondary.scrubbing && !snapshot.playing,
        "begin scrub pauses both comparison lanes");

    for (std::size_t update = 0U; update < 2000U; ++update) {
        player.UpdateScrub(static_cast<FrameIndex>(update % 5U));
    }
    player.UpdateScrub(4U);
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.scrubbing && snapshot.requestedFrame == 4U,
        "scrub keeps the latest shared target");
    player.EndScrub();
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return !value.scrubbing && value.pairReady &&
                    value.requestedFrame == 4U;
            }),
        "scrub commits the final shared target after release");
    passed &= Expect(
        snapshot.primaryFrameAvailable && !snapshot.secondaryFrameAvailable,
        "scrub preserves short-side black-frame semantics");

    player.BeginScrub();
    player.UpdateScrub(2U);
    player.EndScrub();
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.pairReady && value.requestedFrame == 2U &&
                    value.primaryFrameAvailable &&
                    value.secondaryFrameAvailable;
            }),
        "scrub back into range restores both lanes");

    // A normal transport command must recover even if the UI loses MouseUp.
    player.BeginScrub();
    player.UpdateScrub(3U);
    player.StepFrame(-1);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return !value.scrubbing && value.requestedFrame == 2U &&
                    value.pairReady;
            }),
        "single-frame command closes a stale scrub transaction");
    player.SetPlaying(true);
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.playing && !snapshot.scrubbing,
        "playback resumes after scrub and frame stepping");

    player.Shutdown();
    return passed;
}

[[nodiscard]] bool TestPendingPlaybackIntent() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(sequences.WritePrimaryFrames(3U),
        "write pending-intent primary frames");
    passed &= Expect(sequences.WriteSecondaryFrames(3U),
        "write pending-intent secondary frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        LoadActiveComparison(player, sequences, snapshot),
        "prepare pending-intent comparison");
    if (!passed) {
        player.Shutdown();
        return false;
    }

    player.SetPlaying(true);
    const auto previousSecondaryGeneration = snapshot.secondary.generation;
    passed &= Expect(
        player.LoadSecondarySource(sequences.Secondary()),
        "start pending secondary reload");
    player.BeginScrub();
    snapshot = player.Snapshot();
    passed &= Expect(
        !snapshot.scrubbing && !snapshot.primary.scrubbing &&
            !snapshot.secondary.scrubbing,
        "scrub is rejected while a comparison source load is pending");
    player.SetPlaying(false);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [previousSecondaryGeneration](const ComparisonPlayerSnapshot& value) {
                return value.secondary.generation !=
                        previousSecondaryGeneration &&
                    !value.primary.loading && !value.secondary.loading &&
                    value.pairReady;
            }),
        "finish secondary reload after explicit pause");
    passed &= Expect(
        !snapshot.playing,
        "explicit pause during source load must cancel automatic resume");

    player.Seek(0U);
    player.SetPlaying(true);
    player.SetDecodePercent(50U);
    player.StepFrame(1);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.decodePercent == 50U &&
                    value.primary.decodePercent == 50U &&
                    value.secondary.decodePercent == 50U &&
                    !value.primary.loading && !value.secondary.loading &&
                    value.pairReady && value.requestedFrame == 1U;
            }),
        "finish transactional decode switch after pending step");
    passed &= Expect(
        !snapshot.playing,
        "step during decode load must cancel automatic resume");
    passed &= Expect(
        snapshot.currentFrame == 1U,
        "step target during decode load must remain authoritative");

    player.SetPlaying(true);
    passed &= Expect(
        player.LoadSecondarySource(sequences.Secondary()),
        "start pending reload before comparison exit");
    player.SetPlaying(false);
    player.SetPlaying(true);
    passed &= Expect(
        player.SetComparisonEnabled(false),
        "exit comparison during pending reload");
    snapshot = player.Snapshot();
    passed &= Expect(
        !snapshot.enabled && snapshot.playing,
        "comparison exit must restore the final pending playback intent");

    passed &= Expect(
        player.SetComparisonEnabled(true),
        "re-enable comparison for primary pending exit");
    passed &= Expect(
        player.LoadSecondarySource(sequences.Secondary()),
        "reload comparison lane for primary pending exit");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.active && value.pairReady &&
                    !value.primary.loading && !value.secondary.loading;
            }),
        "prepare primary pending exit");
    player.SetPlaying(true);
    player.SetDecodePercent(100U);
    player.SetPlaying(false);
    player.SetPlaying(true);
    passed &= Expect(
        player.SetComparisonEnabled(false),
        "exit comparison during primary decode load");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return !value.enabled && !value.primary.loading &&
                    value.primary.decodePercent == 100U && value.playing;
            }),
        "restore final playback intent after primary pending commit");

    passed &= Expect(
        player.SetComparisonEnabled(true),
        "re-enable comparison for final pause exit");
    passed &= Expect(
        player.LoadSecondarySource(sequences.Secondary()),
        "reload comparison lane for final pause exit");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.active && value.pairReady &&
                    !value.primary.loading && !value.secondary.loading;
            }),
        "prepare final pause exit");
    player.SetPlaying(true);
    player.SetDecodePercent(50U);
    player.SetPlaying(false);
    passed &= Expect(
        player.SetComparisonEnabled(false),
        "exit comparison after final pending pause");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return !value.enabled && !value.primary.loading &&
                    value.primary.decodePercent == 50U && !value.playing;
            }),
        "keep final pause intent after primary pending commit");

    player.Shutdown();
    return passed;
}

[[nodiscard]] bool TestDecodePercentRollback() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(sequences.WritePrimaryFrames(3U),
        "write rollback primary frames");
    passed &= Expect(sequences.WriteSecondaryFrames(3U),
        "write rollback secondary frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        LoadActiveComparison(player, sequences, snapshot),
        "prepare decode rollback comparison");
    if (!passed) {
        player.Shutdown();
        return false;
    }

    const Generation primaryGeneration = snapshot.primary.generation;
    passed &= Expect(
        sequences.CorruptSecondaryFrame(1U),
        "corrupt one lane first frame before decode switch");
    player.SetPlaying(true);
    player.SetDecodePercent(50U);
    passed &= Expect(
        !player.LoadFolder(sequences.Primary()),
        "reject primary folder load during decode transaction");
    passed &= Expect(
        !player.LoadSource(sequences.Primary()),
        "reject primary source load during decode transaction");
    passed &= Expect(
        !player.ReloadFolder(),
        "reject primary rescan during decode transaction");
    passed &= Expect(
        !player.LoadSecondarySource(sequences.Secondary()),
        "reject secondary load during decode transaction");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [primaryGeneration](const ComparisonPlayerSnapshot& value) {
                return value.primary.generation >= primaryGeneration + 2U &&
                    value.decodePercent == 100U &&
                    value.primary.decodePercent == 100U &&
                    value.secondary.decodePercent == 100U &&
                    !value.primary.loading && !value.secondary.loading &&
                    value.playing &&
                    value.errorUtf8.find("已恢复原比例") !=
                        std::string::npos;
            }),
        "rollback both lanes before restoring playback");
    passed &= Expect(
        snapshot.primary.decodePercent == snapshot.secondary.decodePercent,
        "failed decode transaction must not leave mixed lane percentages");
    passed &= Expect(
        snapshot.decodePercent == 100U,
        "failed decode transaction must keep the shared percentage unchanged");

    player.Shutdown();
    return passed;
}

[[nodiscard]] bool TestRejectComparisonEnableDuringPrimaryPending() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(sequences.WritePrimaryFrames(3U),
        "write mode-boundary primary frames");
    passed &= Expect(sequences.WriteSecondaryFrames(2U),
        "write mode-boundary alternate frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer decodePlayer;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        decodePlayer.LoadFolder(sequences.Primary()),
        "load primary before pending decode boundary");
    passed &= Expect(
        WaitFor(
            decodePlayer,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.primary.hasSource && !value.primary.loading;
            }),
        "commit primary before pending decode boundary");

    bool observedDecodePending = false;
    std::uint32_t pendingDecodePercent = 50U;
    for (std::size_t attempt = 0U;
         attempt < 16U && !observedDecodePending;
         ++attempt) {
        pendingDecodePercent = attempt % 2U == 0U ? 50U : 100U;
        decodePlayer.SetDecodePercent(pendingDecodePercent);
        snapshot = decodePlayer.Snapshot();
        observedDecodePending = snapshot.primary.loading;
        if (!observedDecodePending) {
            passed &= Expect(
                WaitFor(
                    decodePlayer,
                    snapshot,
                    [pendingDecodePercent](
                        const ComparisonPlayerSnapshot& value) {
                        return !value.primary.loading &&
                            value.primary.decodePercent ==
                                pendingDecodePercent;
                    }),
                "settle fast decode candidate before retry");
        }
    }
    passed &= Expect(
        observedDecodePending,
        "observe single-view decode candidate pending");
    if (observedDecodePending) {
        passed &= Expect(
            !decodePlayer.SetComparisonEnabled(true),
            "reject comparison while primary decode candidate is pending");
        snapshot = decodePlayer.Snapshot();
        passed &= Expect(
            !snapshot.enabled,
            "rejected decode-boundary enable must not change mode");
        passed &= Expect(
            WaitFor(
                decodePlayer,
                snapshot,
                [pendingDecodePercent](
                    const ComparisonPlayerSnapshot& value) {
                    return !value.primary.loading &&
                        value.primary.decodePercent == pendingDecodePercent;
                }),
            "commit single-view decode candidate after rejected enable");
        passed &= Expect(
            decodePlayer.SetComparisonEnabled(true),
            "enable comparison after decode candidate settles");
        snapshot = decodePlayer.Snapshot();
        passed &= Expect(
            snapshot.enabled &&
                snapshot.decodePercent == pendingDecodePercent &&
                snapshot.primary.decodePercent == pendingDecodePercent &&
                snapshot.secondary.decodePercent == pendingDecodePercent,
            "settled decode percentage must seed both comparison lanes");
    }
    decodePlayer.Shutdown();

    ComparisonPlayer sourcePlayer;
    passed &= Expect(
        sourcePlayer.LoadFolder(sequences.Primary()),
        "load primary before pending source boundary");
    passed &= Expect(
        WaitFor(
            sourcePlayer,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.primary.hasSource && !value.primary.loading;
            }),
        "commit primary before pending source boundary");

    bool observedSourcePending = false;
    std::size_t expectedTotalFrames = 0U;
    Generation generationBeforePending = 0U;
    for (std::size_t attempt = 0U;
         attempt < 16U && !observedSourcePending;
         ++attempt) {
        const bool loadAlternate = attempt % 2U == 0U;
        const std::filesystem::path& folder = loadAlternate
            ? sequences.Secondary()
            : sequences.Primary();
        expectedTotalFrames = loadAlternate ? 2U : 3U;
        generationBeforePending = sourcePlayer.Snapshot().primary.generation;
        passed &= Expect(
            sourcePlayer.LoadFolder(folder),
            "start single-view source candidate");
        snapshot = sourcePlayer.Snapshot();
        observedSourcePending = snapshot.primary.loading;
        if (!observedSourcePending) {
            passed &= Expect(
                WaitFor(
                    sourcePlayer,
                    snapshot,
                    [generationBeforePending](
                        const ComparisonPlayerSnapshot& value) {
                        return !value.primary.loading &&
                            value.primary.generation !=
                                generationBeforePending;
                    }),
                "settle fast source candidate before retry");
        }
    }
    passed &= Expect(
        observedSourcePending,
        "observe single-view source candidate pending");
    if (observedSourcePending) {
        passed &= Expect(
            !sourcePlayer.SetComparisonEnabled(true),
            "reject comparison while primary source candidate is pending");
        snapshot = sourcePlayer.Snapshot();
        passed &= Expect(
            !snapshot.enabled,
            "rejected source-boundary enable must not change mode");
        passed &= Expect(
            WaitFor(
                sourcePlayer,
                snapshot,
                [generationBeforePending, expectedTotalFrames](
                    const ComparisonPlayerSnapshot& value) {
                    return !value.primary.loading &&
                        value.primary.generation != generationBeforePending &&
                        value.primary.totalFrames == expectedTotalFrames;
                }),
            "commit source candidate after rejected enable");
        passed &= Expect(
            sourcePlayer.SetComparisonEnabled(true),
            "enable comparison after source candidate settles");
        snapshot = sourcePlayer.Snapshot();
        passed &= Expect(
            snapshot.enabled &&
                snapshot.totalFrames == expectedTotalFrames &&
                snapshot.primary.totalFrames == expectedTotalFrames,
            "settled source domain must seed comparison shared domain");
    }
    sourcePlayer.Shutdown();
    return passed;
}

[[nodiscard]] bool TestBackgroundResourceMode() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(
        sequences.WritePrimaryFrames(8U),
        "write background-mode primary frames");
    passed &= Expect(
        sequences.WriteSecondaryFrames(8U),
        "write background-mode secondary frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        LoadActiveComparison(player, sequences, snapshot),
        "load active comparison for background resource mode");
    if (!passed) {
        player.Shutdown();
        return false;
    }

    const Generation primaryGeneration = snapshot.primary.generation;
    const Generation secondaryGeneration = snapshot.secondary.generation;
    player.SetPlaying(true);
    passed &= Expect(
        player.Snapshot().playing,
        "comparison plays before entering background resource mode");

    player.SetBackgroundResourceMode(true);
    snapshot = player.Snapshot();
    const auto backgroundSplit =
        zt::sequence::comparison_detail::CalculateMemorySplit(
            zt::sequence::comparison_detail::kBackgroundMemoryLimitBytes,
            true);
    passed &= Expect(
        snapshot.backgroundResourceMode && !snapshot.playing,
        "background resource mode pauses both lanes");
    passed &= Expect(
        snapshot.memoryLimitBytes == kDefaultMemoryLimitBytes &&
            snapshot.primary.memoryLimitBytes == kDefaultMemoryLimitBytes &&
            snapshot.secondary.memoryLimitBytes == kDefaultMemoryLimitBytes,
        "background resource mode preserves the configured memory setting");
    passed &= Expect(
        snapshot.primary.cacheCapacityBytes ==
            backgroundSplit.primaryCacheBytes &&
            snapshot.secondary.cacheCapacityBytes ==
                backgroundSplit.secondaryCacheBytes,
        "background resource mode splits one 8 GiB effective budget");
    passed &= Expect(
        snapshot.primary.generation == primaryGeneration &&
            snapshot.secondary.generation == secondaryGeneration,
        "background resource mode does not reload either source");

    player.SetPlaying(true);
    passed &= Expect(
        !player.Snapshot().playing,
        "background resource mode cannot resume playback implicitly");

    player.SetBackgroundResourceMode(false);
    snapshot = player.Snapshot();
    const auto foregroundSplit =
        zt::sequence::comparison_detail::CalculateMemorySplit(
            kDefaultMemoryLimitBytes,
            true);
    passed &= Expect(
        !snapshot.backgroundResourceMode && !snapshot.playing,
        "foreground restoration keeps playback paused");
    passed &= Expect(
        snapshot.primary.cacheCapacityBytes == foregroundSplit.primaryCacheBytes &&
            snapshot.secondary.cacheCapacityBytes ==
                foregroundSplit.secondaryCacheBytes,
        "foreground restoration restores the configured cache budget");
    player.SetPlaying(true);
    passed &= Expect(
        player.Snapshot().playing,
        "manual playback works after foreground restoration");

    constexpr std::uint64_t kSixGiB = 6ULL * 1024ULL * 1024ULL * 1024ULL;
    player.SetPlaying(false);
    player.SetMemoryLimitBytes(kSixGiB);
    player.SetBackgroundResourceMode(true);
    snapshot = player.Snapshot();
    const auto smallerSplit =
        zt::sequence::comparison_detail::CalculateMemorySplit(
            kSixGiB,
            true);
    passed &= Expect(
        snapshot.memoryLimitBytes == kSixGiB &&
            snapshot.primary.cacheCapacityBytes == smallerSplit.primaryCacheBytes &&
            snapshot.secondary.cacheCapacityBytes == smallerSplit.secondaryCacheBytes,
        "background resource mode never raises a smaller configured limit");

    player.Shutdown();
    return passed;
}

[[nodiscard]] bool TestSequenceFrameOffsetNoOpForTwoSequences() {
    bool passed = true;
    TemporaryComparisonSequences sequences;
    passed &= Expect(
        sequences.WritePrimaryFrames(4U),
        "write offset-noop primary frames");
    passed &= Expect(
        sequences.WriteSecondaryFrames(4U),
        "write offset-noop secondary frames");
    if (!passed) {
        return false;
    }

    ComparisonPlayer player;
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        LoadActiveComparison(player, sequences, snapshot),
        "load two-sequence comparison for offset no-op");
    if (!passed) {
        player.Shutdown();
        return false;
    }

    const Generation primaryGeneration = snapshot.primary.generation;
    const Generation secondaryGeneration = snapshot.secondary.generation;
    passed &= Expect(
        snapshot.primary.sourceKind == SourceKind::PngSequence &&
            snapshot.secondary.sourceKind == SourceKind::PngSequence &&
            !snapshot.sequenceFrameOffsetAvailable,
        "two-sequence comparison does not expose an ambiguous sequence offset");

    player.SetComparisonSequenceFrameOffset(2U);
    snapshot = player.Snapshot();
    passed &= Expect(
        !snapshot.sequenceFrameOffsetAvailable &&
            snapshot.totalFrames == 4U && snapshot.requestedFrame == 0U &&
            snapshot.primary.generation == primaryGeneration &&
            snapshot.secondary.generation == secondaryGeneration,
        "sequence offset command is a no-op when both lanes are sequences");

    player.Seek(2U);
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.pairReady && value.currentFrame == 2U &&
                    value.requestedFrame == 2U &&
                    value.primaryDisplayFrame != nullptr &&
                    value.secondaryDisplayFrame != nullptr &&
                    value.primaryDisplayFrame->index == 2U &&
                    value.secondaryDisplayFrame->index == 2U;
            }),
        "two-sequence comparison retains the original shared frame mapping");

    player.Shutdown();
    return passed;
}

}  // namespace

int main() {
    bool passed = true;

    const auto split =
        zt::sequence::comparison_detail::CalculateMemorySplit(
            kDefaultMemoryLimitBytes,
            true);
    passed &= Expect(
        split.primaryCacheBytes + split.secondaryCacheBytes ==
            split.usableCacheBytes,
        "comparison cache shares must sum to one global usable budget");
    passed &= Expect(
        split.primaryCacheBytes == split.secondaryCacheBytes,
        "default 25 GB comparison budget must split fifty-fifty");
    passed &= Expect(
        zt::sequence::comparison_detail::CommonTotalFrames(1200U, 600U, true) ==
            1200U,
        "comparison timeline must use the longer source");
    const auto looped = zt::sequence::comparison_detail::AdvanceFrame(
        2U,
        1,
        zt::sequence::PlaybackRange{0U, 2U},
        true);
    passed &= Expect(
        looped.target == 0U && !looped.stoppedAtBoundary,
        "shared loop must wrap at the longest source boundary");
    const auto stopped = zt::sequence::comparison_detail::AdvanceFrame(
        2U,
        1,
        zt::sequence::PlaybackRange{0U, 2U},
        false);
    passed &= Expect(
        stopped.target == 2U && stopped.stoppedAtBoundary,
        "shared non-loop playback must stop at the longest source boundary");

    TemporaryComparisonSequences sequences;
    passed &= Expect(sequences.WritePrimaryFrames(3U), "write primary frames");
    passed &= Expect(sequences.WriteSecondaryFrames(2U), "write secondary frames");
    if (!passed) {
        return 1;
    }

    ComparisonPlayer player;
    passed &= Expect(player.LoadFolder(sequences.Primary()), "load primary sequence");
    ComparisonPlayerSnapshot snapshot;
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.primary.hasSource && !value.primary.loading;
            }),
        "commit primary sequence");
    const auto primaryGeneration = snapshot.primary.generation;

    passed &= Expect(
        player.SetComparisonEnabled(true),
        "enable comparison mode");
    passed &= Expect(
        player.LoadSecondarySource(sequences.Secondary()),
        "load secondary sequence");
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.active && value.pairReady &&
                    value.requestedFrame == 0U;
            }),
        "commit frame zero as the first comparison pair");
    passed &= Expect(snapshot.totalFrames == 3U, "use maximum frame count");
    passed &= Expect(
        snapshot.primary.cacheCapacityBytes +
            snapshot.secondary.cacheCapacityBytes == split.usableCacheBytes,
        "two lane capacities must share one 25 GB budget");
    passed &= Expect(
        snapshot.primaryFrameAvailable && snapshot.secondaryFrameAvailable,
        "both frame-zero images must be available");

    const std::uint64_t firstPairRevision = snapshot.pairRevision;
    player.Seek(static_cast<FrameIndex>(2U));
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [firstPairRevision](const ComparisonPlayerSnapshot& value) {
                return value.pairReady && value.requestedFrame == 2U &&
                    value.pairRevision > firstPairRevision;
            }),
        "commit long-source frame with short source exhausted");
    passed &= Expect(snapshot.primaryFrameAvailable, "long source remains visible");
    passed &= Expect(
        !snapshot.secondaryFrameAvailable &&
            snapshot.secondaryDisplayFrame == nullptr,
        "short source must become a committed black frame");

    player.Seek(static_cast<FrameIndex>(1U));
    passed &= Expect(
        WaitFor(
            player,
            snapshot,
            [](const ComparisonPlayerSnapshot& value) {
                return value.pairReady && value.requestedFrame == 1U &&
                    value.secondaryFrameAvailable &&
                    value.secondaryDisplayFrame != nullptr;
            }),
        "short source must recover after seeking back into its valid range");

    player.SetPlaybackRange(0U, 2U);
    player.SetLoopPlayback(false);
    player.SetFramesPerSecond(120.0);
    player.SetPlaying(true);
    player.Tick(0.02);
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.requestedFrame == 2U && !snapshot.playing,
        "non-loop shared clock must stop on the longest source last frame");
    passed &= Expect(
        snapshot.pairReady && !snapshot.secondaryFrameAvailable,
        "clock-driven long-source tail must commit a black short-side frame");

    player.SetLoopPlayback(true);
    player.SetPlaying(true);
    player.Tick(0.01);
    snapshot = player.Snapshot();
    passed &= Expect(
        snapshot.requestedFrame == 0U && snapshot.playing,
        "looping shared clock must wrap both lanes to frame zero");
    passed &= Expect(
        snapshot.pairReady && snapshot.primaryFrameAvailable &&
            snapshot.secondaryFrameAvailable,
        "both lanes must recover after the shared clock wraps into valid frames");

    passed &= Expect(
        player.SetComparisonEnabled(false),
        "disable comparison mode");
    snapshot = player.Snapshot();
    passed &= Expect(!snapshot.enabled, "comparison mode disabled");
    passed &= Expect(
        snapshot.primary.generation == primaryGeneration,
        "disabling comparison must not reload the primary source");
    passed &= Expect(
        snapshot.primary.cacheCapacityBytes == split.usableCacheBytes,
        "primary cache must recover the full global budget");

    player.Shutdown();
    passed &= TestScrubTransactionAndRecovery();
    passed &= TestPendingPlaybackIntent();
    passed &= TestDecodePercentRollback();
    passed &= TestRejectComparisonEnableDuringPrimaryPending();
    passed &= TestBackgroundResourceMode();
    passed &= TestSequenceFrameOffsetNoOpForTwoSequences();
    return passed ? 0 : 1;
}
