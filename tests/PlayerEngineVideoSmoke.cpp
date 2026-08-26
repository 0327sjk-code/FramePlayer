#include "Core/PlayerEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using zt::sequence::PlayerEngine;
using zt::sequence::PlayerSnapshot;
using zt::sequence::VideoExportSnapshot;

[[nodiscard]] bool WaitFor(
    PlayerEngine& engine,
    const std::function<bool(const PlayerSnapshot&)>& predicate,
    PlayerSnapshot& snapshot,
    const std::chrono::seconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        engine.Tick(1.0 / 120.0);
        snapshot = engine.Snapshot();
        if (predicate(snapshot)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    snapshot = engine.Snapshot();
    return false;
}

[[nodiscard]] bool Expect(const bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAILED: " << label << '\n';
    }
    return condition;
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    using namespace zt::sequence;
    if (argumentCount < 2 || arguments == nullptr || arguments[1] == nullptr) {
        std::cerr << "usage: ZTPlayerEngineVideoSmoke <video> [png-folder]\n";
        return 2;
    }

    const std::filesystem::path videoFile(arguments[1]);
    PlayerEngine engine;
    PlayerSnapshot snapshot;
    bool passed = true;

    passed &= Expect(engine.LoadSource(videoFile), "submit video load");
    passed &= Expect(
        WaitFor(
            engine,
            [](const PlayerSnapshot& value) {
                return value.hasSource && !value.loading &&
                    value.sourceKind == SourceKind::Video &&
                    value.displayFrame != nullptr;
            },
            snapshot),
        "commit video source");
    const Generation videoGeneration = snapshot.generation;
    passed &= Expect(snapshot.totalFrames >= 32U, "video frame count");
    passed &= Expect(
        snapshot.sourceWidth > 0U && snapshot.sourceHeight > 0U,
        "video source dimensions");
    passed &= Expect(
        snapshot.targetFramesPerSecond > 59.99 &&
            snapshot.targetFramesPerSecond < 60.01,
        "video native fps");
    const auto initialExportSnapshot = engine.CaptureExportSnapshot();
    const VideoExportSnapshot* const initialVideoExport = initialExportSnapshot
        ? std::get_if<VideoExportSnapshot>(&*initialExportSnapshot)
        : nullptr;
    passed &= Expect(
        initialVideoExport != nullptr,
        "video export snapshot available");
    if (initialVideoExport != nullptr) {
        passed &= Expect(
            initialVideoExport->sourceFile == videoFile.lexically_normal() &&
                initialVideoExport->inclusiveRange.startFrame == 0U &&
                initialVideoExport->inclusiveRange.endFrame + 1U ==
                    initialVideoExport->totalFrames,
            "video export snapshot captures source and full range");
        passed &= Expect(
            initialVideoExport->sourceWidth == snapshot.sourceWidth &&
                initialVideoExport->sourceHeight == snapshot.sourceHeight &&
                initialVideoExport->sourceFramesPerSecond > 59.99 &&
                initialVideoExport->sourceFramesPerSecond < 60.01,
            "video export snapshot captures native metadata");
    }
    if (!passed || snapshot.totalFrames < 32U) {
        engine.Shutdown();
        return 1;
    }
    engine.SetMemoryLimitBytes(1ULL * kBytesPerGiB);

    engine.StepFrame(1);
    passed &= Expect(
        WaitFor(
            engine,
            [](const PlayerSnapshot& value) {
                return value.currentFrame == 1U && !value.buffering;
            },
            snapshot),
        "step forward exactly one frame");
    engine.StepFrame(-1);
    passed &= Expect(
        WaitFor(
            engine,
            [](const PlayerSnapshot& value) {
                return value.currentFrame == 0U && !value.buffering;
            },
            snapshot),
        "step backward exactly one frame");

    const FrameIndex randomTarget = static_cast<FrameIndex>(
        (snapshot.totalFrames * 3U) / 5U);
    engine.Seek(randomTarget);
    passed &= Expect(
        WaitFor(
            engine,
            [randomTarget](const PlayerSnapshot& value) {
                return value.currentFrame == randomTarget && !value.buffering;
            },
            snapshot),
        "random seek target");

    const std::size_t totalFrames = snapshot.totalFrames;
    const FrameIndex scrubFinalTarget = static_cast<FrameIndex>(
        (totalFrames * 2U) / 3U);
    engine.BeginScrub();
    const auto scrubSubmitStarted = std::chrono::steady_clock::now();
    for (std::size_t update = 0U; update < 2000U; ++update) {
        const FrameIndex target = static_cast<FrameIndex>(
            (update * 37U) % totalFrames);
        engine.UpdateScrub(target);
    }
    engine.UpdateScrub(scrubFinalTarget);
    const auto scrubSubmitElapsed = std::chrono::steady_clock::now() -
        scrubSubmitStarted;
    const double scrubSubmitMilliseconds =
        std::chrono::duration<double, std::milli>(scrubSubmitElapsed).count();
    snapshot = engine.Snapshot();
    passed &= Expect(
        snapshot.scrubbing && snapshot.requestedFrame == scrubFinalTarget,
        "rapid scrub keeps latest requested frame");
    passed &= Expect(
        scrubSubmitElapsed < std::chrono::seconds(2),
        "rapid scrub submission stays non-blocking");
    const auto scrubSettleStarted = std::chrono::steady_clock::now();
    engine.EndScrub();
    const bool scrubSettled = WaitFor(
        engine,
        [scrubFinalTarget](const PlayerSnapshot& value) {
            return !value.scrubbing &&
                value.currentFrame == scrubFinalTarget &&
                !value.buffering;
        },
        snapshot,
        std::chrono::seconds(10));
    const double scrubSettleMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - scrubSettleStarted).count();
    passed &= Expect(
        scrubSettled,
        "rapid scrub settles on latest frame");

    engine.SetPlaying(true);
    passed &= Expect(
        WaitFor(
            engine,
            [scrubFinalTarget](const PlayerSnapshot& value) {
                return value.playing && value.currentFrame != scrubFinalTarget;
            },
            snapshot),
        "playback resumes after rapid scrub");
    engine.SetPlaying(false);

    const std::uint32_t fullDecodedWidth = snapshot.decodedWidth;
    const std::uint32_t fullDecodedHeight = snapshot.decodedHeight;
    engine.SetDecodePercent(25U);
    passed &= Expect(
        WaitFor(
            engine,
            [videoGeneration, fullDecodedWidth, fullDecodedHeight](
                const PlayerSnapshot& value) {
                return value.generation != videoGeneration &&
                    !value.loading && value.decodePercent == 25U &&
                    value.decodedWidth > 0U && value.decodedHeight > 0U &&
                    value.decodedWidth <= fullDecodedWidth &&
                    value.decodedHeight <= fullDecodedHeight;
            },
            snapshot,
            std::chrono::seconds(10)),
        "reload video at 25 percent");

    const FrameIndex rangeStart = static_cast<FrameIndex>(
        std::min<std::size_t>(snapshot.totalFrames - 2U, snapshot.totalFrames / 8U));
    const FrameIndex rangeEnd = static_cast<FrameIndex>(
        std::min<std::size_t>(
            snapshot.totalFrames - 1U,
            static_cast<std::size_t>(rangeStart) + 180U));
    engine.SetPlaybackRange(rangeStart, rangeEnd);
    engine.Seek(rangeStart);
    passed &= Expect(
        WaitFor(
            engine,
            [rangeStart](const PlayerSnapshot& value) {
                return value.currentFrame == rangeStart && !value.buffering;
            },
            snapshot),
        "seek playback range start");
    engine.SetPlaying(true);
    const auto playbackDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < playbackDeadline) {
        engine.Tick(1.0 / 60.0);
        snapshot = engine.Snapshot();
        if (snapshot.currentFrame >= rangeStart + 10U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    engine.SetPlaying(false);
    passed &= Expect(
        snapshot.currentFrame >= rangeStart + 10U,
        "video playback advances");
    passed &= Expect(
        snapshot.currentFrame >= rangeStart && snapshot.currentFrame <= rangeEnd,
        "video playback stays in range");
    passed &= Expect(
        snapshot.cacheBytes <= snapshot.memoryLimitBytes,
        "video cache respects memory target");
    passed &= Expect(
        snapshot.memoryLimitBytes == 1ULL * kBytesPerGiB,
        "video memory target is configurable");

    const auto rangedExportSnapshot = engine.CaptureExportSnapshot();
    const VideoExportSnapshot* const rangedVideoExport = rangedExportSnapshot
        ? std::get_if<VideoExportSnapshot>(&*rangedExportSnapshot)
        : nullptr;
    passed &= Expect(
        rangedVideoExport != nullptr &&
            rangedVideoExport->inclusiveRange.startFrame == rangeStart &&
            rangedVideoExport->inclusiveRange.endFrame == rangeEnd,
        "video export snapshot preserves custom inclusive range");

    const Generation preservedGeneration = snapshot.generation;
    passed &= Expect(
        !engine.LoadSource(videoFile.parent_path() / L"missing-video.mp4"),
        "reject missing video");
    snapshot = engine.Snapshot();
    passed &= Expect(
        snapshot.generation == preservedGeneration && snapshot.hasSource,
        "failed video load preserves source");
    const std::size_t videoCachedFrames = snapshot.cachedFrames;
    const std::uint64_t videoCacheBytes = snapshot.cacheBytes;

    if (argumentCount >= 3 && arguments[2] != nullptr) {
        const std::filesystem::path pngFolder(arguments[2]);
        passed &= Expect(engine.LoadFolder(pngFolder), "submit png switch");
        passed &= Expect(
            WaitFor(
                engine,
                [](const PlayerSnapshot& value) {
                    return value.hasSource && !value.loading &&
                        value.sourceKind == SourceKind::PngSequence &&
                        value.displayFrame != nullptr;
                },
                snapshot,
                std::chrono::seconds(10)),
            "switch video to png");
        passed &= Expect(
            snapshot.targetFramesPerSecond > 59.99 &&
                snapshot.targetFramesPerSecond < 60.01,
            "restore sequence fps");

        passed &= Expect(engine.LoadSource(videoFile), "submit video reload");
        passed &= Expect(
            WaitFor(
                engine,
                [](const PlayerSnapshot& value) {
                    return value.hasSource && !value.loading &&
                        value.sourceKind == SourceKind::Video;
                },
                snapshot),
            "switch png to video");
    }

    engine.Shutdown();
    if (!passed) {
        return 1;
    }
    std::cout << "player-engine-video-smoke passed frames="
              << totalFrames << " scrub_submit_ms="
              << scrubSubmitMilliseconds << " scrub_settle_ms="
              << scrubSettleMilliseconds << " video_cached="
              << videoCachedFrames << " video_cache_bytes="
              << videoCacheBytes << " final_generation="
              << snapshot.generation << '\n';
    return 0;
}
