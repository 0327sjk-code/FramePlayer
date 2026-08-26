#pragma once

#include "Core/PlayerTypes.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace zt::sequence {

class PlayerEngine final {
public:
    PlayerEngine();
    ~PlayerEngine();

    PlayerEngine(const PlayerEngine&) = delete;
    PlayerEngine& operator=(const PlayerEngine&) = delete;
    PlayerEngine(PlayerEngine&&) = delete;
    PlayerEngine& operator=(PlayerEngine&&) = delete;

    [[nodiscard]] bool LoadFolder(const std::filesystem::path& folder);
    [[nodiscard]] bool LoadSource(const std::filesystem::path& sourcePath);
    [[nodiscard]] bool ReloadFolder();
    [[nodiscard]] std::optional<SequenceExportSnapshot> CaptureExportSnapshot() const;

    void Tick(double elapsedSeconds);
    void SetExternalClockEnabled(bool enabled);
    void RequestFrame(
        FrameIndex frame,
        FrameRequestKind requestKind,
        int direction = 1);
    void TogglePlayback();
    void SetPlaying(bool playing);
    void StepFrame(int delta);
    void BeginScrub();
    ScrubUpdateResult UpdateScrub(FrameIndex frame);
    void EndScrub();
    void Seek(FrameIndex frame);
    void SeekNormalized(double normalizedPosition);
    void SetPlaybackRange(FrameIndex startFrame, FrameIndex endFrame);
    void SetLoopPlayback(bool enabled);
    void SetFramesPerSecond(double framesPerSecond);
    void SetDecodePercent(std::uint32_t percent);
    void SetMemoryLimitBytes(std::uint64_t bytes);
    void SetResourceBudget(const EngineResourceBudget& budget);
    void SetBackgroundResourceMode(bool enabled);

    [[nodiscard]] PlayerSnapshot Snapshot() const;
    void Shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence
