#pragma once

#include "Cache/MemoryFrameCache.h"
#include "Core/DecodeScheduler.h"
#include "Core/PlayerEngine.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace zt::sequence {

class MediaFoundationVideoDecoder;

namespace detail {

struct ProcessMemoryUsage {
    std::uint64_t workingSetBytes = 0;
    std::uint64_t privateBytes = 0;
};

[[nodiscard]] ProcessMemoryUsage QueryProcessMemoryUsage() noexcept;

}  // namespace detail

class PlayerEngine::Impl final {
public:
    Impl();
    ~Impl();

    [[nodiscard]] bool LoadFolder(const std::filesystem::path& folder);
    [[nodiscard]] bool LoadSource(const std::filesystem::path& sourcePath);
    [[nodiscard]] bool ReloadFolder();
    [[nodiscard]] std::optional<ExportSourceSnapshot> CaptureExportSnapshot() const;
    void Tick(double elapsedSeconds);
    void SetExternalClockEnabled(bool enabled);
    void RequestFrame(
        FrameIndex frame,
        FrameRequestKind requestKind,
        int direction);
    void TogglePlayback();
    void SetPlaying(bool playing);
    void BeginShuttlePlayback(int direction, double speedScale);
    void EndShuttlePlayback();
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
    struct SourceSession {
        Generation generation = 0;
        SourceKind kind = SourceKind::None;
        std::filesystem::path sourcePath;
        std::vector<FrameFile> frames;
        VideoMetadata videoMetadata;
        std::uint32_t decodePercent = kDefaultDecodePercent;

        [[nodiscard]] std::size_t TotalFrames() const noexcept {
            return kind == SourceKind::Video
                ? videoMetadata.frameCount
                : frames.size();
        }
    };

    struct PreservedPlaybackRange {
        PlaybackRange range;
        bool customized = false;
        Generation sourceGeneration = 0;
    };

    struct PendingLoad {
        std::shared_ptr<const SourceSession> session;
        FrameIndex initialFrame = 0;
        PlaybackRange playbackRange;
        bool playbackRangeCustomized = false;
        std::optional<PreservedPlaybackRange> preservedPlaybackRange;
    };

    struct SnapshotTelemetry final {
        bool initialized = false;
        Generation generation = 0U;
        PlaybackRange playbackRange;
        bool loopPlayback = false;
        std::chrono::steady_clock::time_point sampledAt{};
        std::size_t cachedFrames = 0U;
        std::size_t readyFrames = 0U;
        std::uint64_t cacheCapacityBytes = 0U;
        std::uint64_t cacheBytes = 0U;
        detail::ProcessMemoryUsage processMemory;
    };

    [[nodiscard]] bool BeginLoad(
        const std::filesystem::path& requestedFolder,
        std::optional<std::wstring> preferredFile,
        std::optional<PreservedPlaybackRange> preservedPlaybackRange);
    [[nodiscard]] bool BeginVideoLoad(
        const std::filesystem::path& videoFile,
        FrameIndex preferredFrame,
        std::optional<PreservedPlaybackRange> preservedPlaybackRange);
    void OnDecodeCompleted(ScheduledDecodeResult result);
    void CancelGeneration(Generation generation);
    void CancelAllExcept(Generation generation);
    void CancelInteractive(Generation generation);
    void CancelBackground(Generation generation);
    void CancelForImmediateTarget(Generation generation);
    void SetBackgroundConcurrency(std::size_t maximumConcurrentTasks);
    void ApplyCacheCapacityLocked(std::uint64_t capacityBytes);
    void CancelPostScrubHotFillLocked();
    void ResetShuttlePlaybackLocked() noexcept;
    [[nodiscard]] PlaybackRange ActiveTransportRangeLocked() const noexcept;
    [[nodiscard]] bool ActiveTransportLoopLocked() const noexcept;
    void PresentRequestedFromCacheLocked();
    [[nodiscard]] bool IsPostScrubHotFillSatisfiedLocked() const;
    void ScheduleScrubTarget();
    void ScheduleScrubTargetLocked();
    void ScheduleWork();

    mutable std::mutex mutex_;
    PlayerSettings settings_;
    MemoryFrameCache cache_;
    DecodeScheduler scheduler_;
    DecodeScheduler videoScheduler_;
    std::atomic<MediaFoundationVideoDecoder*> activeVideoDecoder_{nullptr};
    std::mutex videoDecoderControlMutex_;
    std::condition_variable videoDecoderControlCondition_;
    bool videoDecoderCloseCompleted_ = false;

    bool shutdown_ = false;
    bool externalClockEnabled_ = false;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool buffering_ = false;
    bool backgroundResourceMode_ = false;
    bool shuttlePlayback_ = false;
    int direction_ = 1;
    Generation nextGeneration_ = 1;
    Generation scrubGeneration_ = 0;
    bool postScrubHotFillActive_ = false;
    Generation postScrubHotFillGeneration_ = 0;
    FrameIndex postScrubHotFillStartFrame_ = 0;
    std::shared_ptr<const SourceSession> activeSession_;
    std::optional<PendingLoad> pendingLoad_;
    std::unordered_set<FrameIndex> failedFrames_;

    FrameIndex currentFrame_ = 0;
    FrameIndex requestedFrame_ = 0;
    PlaybackRange playbackRange_;
    bool playbackRangeCustomized_ = false;
    std::shared_ptr<const DecodedFrame> displayFrame_;
    std::uint64_t displayRevision_ = 0;
    std::uint64_t droppedFrames_ = 0;
    std::uint32_t sourceWidth_ = 0;
    std::uint32_t sourceHeight_ = 0;
    std::uint32_t decodedWidth_ = 0;
    std::uint32_t decodedHeight_ = 0;

    double playbackFrameAccumulator_ = 0.0;
    double playbackSpeedScale_ = 1.0;
    double sequenceFramesPerSecond_ = kDefaultFramesPerSecond;
    double fpsSampleElapsed_ = 0.0;
    double actualFramesPerSecond_ = 0.0;
    std::uint64_t presentedFramesSinceSample_ = 0;
    std::uint64_t cacheCapacityBytes_ = 0;

    std::size_t backgroundCursor_ = 0;
    std::uint64_t backgroundSequenceRank_ = 0;
    mutable SnapshotTelemetry snapshotTelemetry_;
    std::string statusUtf8_ = "等待打开 PNG 序列或视频";
    std::string errorUtf8_;
};

}  // namespace zt::sequence
