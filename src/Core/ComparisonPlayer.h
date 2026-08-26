#pragma once

#include "Core/PlayerTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace zt::sequence {

struct ComparisonPlayerSnapshot final {
    bool enabled = false;
    bool active = false;
    bool pairReady = false;
    bool primaryFrameAvailable = false;
    bool secondaryFrameAvailable = false;
    bool playing = false;
    bool scrubbing = false;
    bool buffering = false;
    bool loopPlayback = true;
    bool backgroundResourceMode = false;

    FrameIndex currentFrame = 0U;
    FrameIndex requestedFrame = 0U;
    FrameIndex playbackStartFrame = 0U;
    FrameIndex playbackEndFrame = 0U;
    std::size_t totalFrames = 0U;

    double targetFramesPerSecond = kDefaultFramesPerSecond;
    double actualFramesPerSecond = 0.0;
    std::uint32_t decodePercent = kDefaultDecodePercent;

    std::uint64_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    std::uint64_t cacheBytes = 0U;
    std::uint64_t pairRevision = 0U;
    Generation primaryGeneration = 0U;
    Generation secondaryGeneration = 0U;

    std::shared_ptr<const DecodedFrame> primaryDisplayFrame;
    std::shared_ptr<const DecodedFrame> secondaryDisplayFrame;
    PlayerSnapshot primary;
    PlayerSnapshot secondary;

    std::string statusUtf8;
    std::string errorUtf8;
};

// Coordinates one primary source and an optional comparison source. The
// transport clock, playback range, FPS and memory target are shared. Decode,
// cache and source lifetime remain isolated inside each PlayerEngine.
class ComparisonPlayer final {
public:
    ComparisonPlayer();
    ~ComparisonPlayer();

    ComparisonPlayer(const ComparisonPlayer&) = delete;
    ComparisonPlayer& operator=(const ComparisonPlayer&) = delete;
    ComparisonPlayer(ComparisonPlayer&&) = delete;
    ComparisonPlayer& operator=(ComparisonPlayer&&) = delete;

    [[nodiscard]] bool LoadFolder(const std::filesystem::path& folder);
    [[nodiscard]] bool LoadSource(const std::filesystem::path& sourcePath);
    [[nodiscard]] bool ReloadFolder();
    [[nodiscard]] std::optional<ExportSourceSnapshot>
    CaptureExportSnapshot() const;

    [[nodiscard]] bool SetComparisonEnabled(bool enabled);
    [[nodiscard]] bool LoadSecondarySource(
        const std::filesystem::path& sourcePath);

    void Tick(double elapsedSeconds);
    void TogglePlayback();
    void SetPlaying(bool playing);
    void StepFrame(int delta);
    void BeginScrub();
    void UpdateScrub(FrameIndex frame);
    void EndScrub();
    void Seek(FrameIndex frame);
    void SeekNormalized(double normalizedPosition);
    void SetPlaybackRange(FrameIndex startFrame, FrameIndex endFrame);
    void SetLoopPlayback(bool enabled);
    void SetFramesPerSecond(double framesPerSecond);
    void SetDecodePercent(std::uint32_t percent);
    void SetMemoryLimitBytes(std::uint64_t bytes);
    void SetBackgroundResourceMode(bool enabled);

    [[nodiscard]] ComparisonPlayerSnapshot Snapshot() const;
    void Shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence
