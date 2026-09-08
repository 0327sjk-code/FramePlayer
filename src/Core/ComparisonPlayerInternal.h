#pragma once

#include "Core/ComparisonPlayer.h"
#include "Core/PlayerEngine.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace zt::sequence {

class ComparisonPlayer::Impl final {
public:
    Impl();
    ~Impl();

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
    void BeginShuttlePlayback(int direction, double speedScale);
    void EndShuttlePlayback();
    void StepFrame(int delta);
    void BeginScrub();
    void UpdateScrub(FrameIndex frame);
    void EndScrub();
    void Seek(FrameIndex frame);
    void SeekNormalized(double normalizedPosition);
    void SetPlaybackRange(FrameIndex startFrame, FrameIndex endFrame);
    void SetComparisonSequenceFrameOffset(FrameIndex offset);
    void SetLoopPlayback(bool enabled);
    void SetFramesPerSecond(double framesPerSecond);
    void SetDecodePercent(std::uint32_t percent);
    void SetMemoryLimitBytes(std::uint64_t bytes);
    void SetBackgroundResourceMode(bool enabled);
    [[nodiscard]] ComparisonPlayerSnapshot Snapshot() const;
    void Shutdown();

private:
    enum class PendingLoadPurpose : std::uint8_t {
        Source = 0,
        DecodeApply,
        DecodeRollback,
    };

    struct PendingLoadState final {
        bool pending = false;
        Generation previousGeneration = 0U;
        SourceKind previousSourceKind = SourceKind::None;
        bool adoptRangeOnSuccess = false;
        bool resetSharedFrameOnSuccess = false;
        bool resetSequenceFrameOffsetOnSuccess = false;
        PendingLoadPurpose purpose = PendingLoadPurpose::Source;
        std::uint32_t expectedDecodePercent = kDefaultDecodePercent;
    };

    struct DecodeLaneTransition final {
        bool required = false;
        bool completed = true;
        bool succeeded = true;
    };

    struct DecodePercentTransaction final {
        enum class Phase : std::uint8_t {
            Idle = 0,
            Applying,
            RollingBack,
        };

        Phase phase = Phase::Idle;
        std::uint32_t previousPercent = kDefaultDecodePercent;
        std::uint32_t targetPercent = kDefaultDecodePercent;
        DecodeLaneTransition primary;
        DecodeLaneTransition secondary;
    };

    struct PresentedPairState final {
        bool committed = false;
        bool primaryAvailable = false;
        bool secondaryAvailable = false;
        FrameIndex frame = 0U;
        Generation primaryGeneration = 0U;
        Generation secondaryGeneration = 0U;
        std::shared_ptr<const DecodedFrame> primaryFrame;
        std::shared_ptr<const DecodedFrame> secondaryFrame;
        std::uint64_t revision = 0U;
    };

    [[nodiscard]] bool BeginPrimaryLoad(bool accepted);
    void PauseForPendingOperation();
    void RestorePendingPlaybackAfterRejectedOperation();
    void SetEffectivePlaying(bool playing);
    void ResetShuttlePlayback() noexcept;
    [[nodiscard]] PlaybackRange ActiveTransportRange() const noexcept;
    [[nodiscard]] bool ActiveTransportLoop() const noexcept;
    void MarkUserNavigationDuringPending();
    [[nodiscard]] bool HasTrackedPendingOperation() const noexcept;
    [[nodiscard]] bool DecodeTransactionActive() const noexcept;
    void StartDecodePercentTransaction(
        std::uint32_t percent,
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary);
    void BeginDecodePercentRollback(
        PlayerSnapshot& primary,
        PlayerSnapshot& secondary);
    void ProcessCompletedDecodeTransaction(
        PlayerSnapshot& primary,
        PlayerSnapshot& secondary,
        bool& contextChanged);
    void BeginDecodeLaneTransition(
        PlayerEngine& engine,
        PendingLoadState& pendingLoad,
        DecodeLaneTransition& transition,
        const PlayerSnapshot& before,
        std::uint32_t percent,
        PendingLoadPurpose purpose);
    void ProcessPendingLoads(
        PlayerSnapshot& primary,
        PlayerSnapshot& secondary);
    void SynchronizeFromSinglePlayer(const PlayerSnapshot& primary);
    void ApplyResourceBudgets();
    void ApplySharedContext(
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary);
    void UpdateSharedDomain(
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary);
    void BroadcastFrameRequest(
        FrameRequestKind requestKind,
        int direction,
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary,
        bool force = false);
    [[nodiscard]] bool TargetReady(
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary) const noexcept;
    [[nodiscard]] bool CommitPresentedPair(
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary);
    [[nodiscard]] bool HasRequestedFrameFailure(
        const PlayerSnapshot& primary,
        const PlayerSnapshot& secondary,
        std::string& detail) const;
    void PauseForDecodeFailure(std::string detail);
    void ClearComparisonError();
    void UpdateActualFramesPerSecond(double elapsedSeconds);

    std::unique_ptr<PlayerEngine> primary_;
    std::unique_ptr<PlayerEngine> secondary_;

    bool shutdown_ = false;
    bool comparisonEnabled_ = false;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool loopPlayback_ = true;
    bool pairReady_ = false;
    bool backgroundResourceMode_ = false;
    bool shuttlePlayback_ = false;
    int direction_ = 1;

    FrameIndex requestedFrame_ = 0U;
    FrameIndex sequenceFrameOffset_ = 0U;
    PlaybackRange rangeIntent_{};
    PlaybackRange effectiveRange_{};
    std::size_t commonTotalFrames_ = 0U;
    double framesPerSecond_ = kDefaultFramesPerSecond;
    double playbackFrameAccumulator_ = 0.0;
    double playbackSpeedScale_ = 1.0;
    double fpsSampleElapsed_ = 0.0;
    double actualFramesPerSecond_ = 0.0;
    std::uint64_t presentedPairsSinceSample_ = 0U;
    std::uint64_t memoryLimitBytes_ = kDefaultMemoryLimitBytes;
    std::uint32_t decodePercent_ = kDefaultDecodePercent;

    PendingLoadState primaryLoad_;
    PendingLoadState secondaryLoad_;
    DecodePercentTransaction decodeTransaction_;
    std::optional<bool> pendingPlaybackIntent_;
    std::optional<bool> primaryPlaybackIntentAfterComparisonExit_;
    std::optional<std::uint32_t> queuedDecodePercent_;
    PresentedPairState presentedPair_;
    Generation lastPrimaryRequestGeneration_ = 0U;
    Generation lastSecondaryRequestGeneration_ = 0U;
    FrameIndex lastPrimaryRequestedFrame_ = 0U;
    FrameIndex lastSecondaryRequestedFrame_ = 0U;

    std::string statusUtf8_ = "等待打开 PNG 序列或视频";
    std::string errorUtf8_;
};

}  // namespace zt::sequence
