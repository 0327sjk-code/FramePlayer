#include "Core/PlayerEngineInternal.h"

#include "Core/PlayerEnginePolicy.h"
#include "Platform/Utf8.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace zt::sequence {

void PlayerEngine::Impl::ResetShuttlePlaybackLocked() noexcept {
    shuttlePlayback_ = false;
    playbackSpeedScale_ = detail::kNormalPlaybackSpeedScale;
}

PlaybackRange PlayerEngine::Impl::ActiveTransportRangeLocked() const noexcept {
    if (!activeSession_) {
        return {};
    }
    return shuttlePlayback_
        ? detail::FullPlaybackRange(activeSession_->TotalFrames())
        : playbackRange_;
}

bool PlayerEngine::Impl::ActiveTransportLoopLocked() const noexcept {
    return !shuttlePlayback_ && settings_.loopPlayback;
}

void PlayerEngine::Impl::Tick(const double elapsedSeconds) {
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0) {
        return;
    }

    bool targetChanged = false;
    bool stoppedPlayback = false;
    Generation targetGeneration = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }

        fpsSampleElapsed_ += elapsedSeconds;
        if (fpsSampleElapsed_ >= detail::kFpsSampleSeconds) {
            actualFramesPerSecond_ = fpsSampleElapsed_ > 0.0
                ? static_cast<double>(presentedFramesSinceSample_) / fpsSampleElapsed_
                : 0.0;
            fpsSampleElapsed_ = 0.0;
            presentedFramesSinceSample_ = 0;
        }

        if (externalClockEnabled_) {
            return;
        }

        if (!playing_ || !activeSession_ || activeSession_->TotalFrames() == 0U) {
            return;
        }

        if (buffering_ && !cache_.Contains(
                activeSession_->generation,
                requestedFrame_)) {
            playbackFrameAccumulator_ = 0.0;
            return;
        }

        playbackFrameAccumulator_ += elapsedSeconds *
            settings_.framesPerSecond * playbackSpeedScale_;
        const double wholeSteps = std::floor(playbackFrameAccumulator_);
        if (wholeSteps < 1.0) {
            return;
        }
        playbackFrameAccumulator_ -= wholeSteps;

        const auto maximumSafeSteps = static_cast<double>(
            std::numeric_limits<std::int64_t>::max() / 2);
        const std::int64_t stepCount = static_cast<std::int64_t>(
            std::min(wholeSteps, maximumSafeSteps));
        if (requestedFrame_ != currentFrame_) {
            ++droppedFrames_;
        }
        if (stepCount > 1) {
            droppedFrames_ += static_cast<std::uint64_t>(stepCount - 1);
        }

        const std::int64_t signedSteps = direction_ < 0 ? -stepCount : stepCount;
        const PlaybackRange transportRange = ActiveTransportRangeLocked();
        const bool transportLoop = ActiveTransportLoopLocked();
        const std::optional<FrameIndex> target =
            detail::OffsetFrameInPlaybackRange(
            requestedFrame_,
            signedSteps,
            transportRange,
            transportLoop);
        if (target) {
            requestedFrame_ = *target;
        } else {
            const std::int64_t unclamped = static_cast<std::int64_t>(requestedFrame_)
                + signedSteps;
            requestedFrame_ = detail::ClampFrameToPlaybackRange(
                unclamped,
                transportRange);
            playing_ = false;
            stoppedPlayback = true;
            playbackFrameAccumulator_ = 0.0;
            const bool endedShuttle = shuttlePlayback_;
            ResetShuttlePlaybackLocked();
            statusUtf8_ = direction_ < 0
                ? (endedShuttle ? "已到素材起始帧" : "已到播放起始帧")
                : (endedShuttle ? "已到素材结束帧" : "已到播放结束帧");
        }

        PresentRequestedFromCacheLocked();
        targetGeneration = activeSession_->generation;
        targetChanged = true;
    }

    if (targetChanged) {
        if (stoppedPlayback) {
            SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
        }
        CancelInteractive(targetGeneration);
        ScheduleWork();
    }
}

void PlayerEngine::Impl::SetExternalClockEnabled(const bool enabled) {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || externalClockEnabled_ == enabled) {
        return;
    }
    externalClockEnabled_ = enabled;
    playbackFrameAccumulator_ = 0.0;
}

void PlayerEngine::Impl::CancelPostScrubHotFillLocked() {
    if (postScrubHotFillActive_ && postScrubHotFillGeneration_ != 0U) {
        scheduler_.ResumeBackground(postScrubHotFillGeneration_);
    }
    postScrubHotFillActive_ = false;
    postScrubHotFillGeneration_ = 0U;
    postScrubHotFillStartFrame_ = 0U;
}

void PlayerEngine::Impl::RequestFrame(
    const FrameIndex frame,
    const FrameRequestKind requestKind,
    const int direction) {
    Generation generation = 0U;
    bool requestChanged = false;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !activeSession_ || activeSession_->TotalFrames() == 0U) {
            return;
        }
        if (requestKind == FrameRequestKind::InteractiveSeek) {
            CancelPostScrubHotFillLocked();
            ResetShuttlePlaybackLocked();
        }

        const FrameIndex target = std::min<FrameIndex>(
            frame,
            static_cast<FrameIndex>(activeSession_->TotalFrames() - 1U));
        requestChanged = target != requestedFrame_;
        requestedFrame_ = target;
        direction_ = direction < 0 ? -1 : 1;
        playbackFrameAccumulator_ = 0.0;
        if (requestKind == FrameRequestKind::InteractiveSeek) {
            playing_ = false;
            statusUtf8_ = "正在同步定位目标帧";
        }
        generation = activeSession_->generation;
        PresentRequestedFromCacheLocked();
    }

    if (requestKind == FrameRequestKind::InteractiveSeek) {
        SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
        CancelForImmediateTarget(generation);
    } else if (requestChanged) {
        CancelInteractive(generation);
    }
    ScheduleWork();
}

void PlayerEngine::Impl::TogglePlayback() {
    EndScrub();
    bool nowPlaying = false;
    bool targetChanged = false;
    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !activeSession_) {
            return;
        }
        ResetShuttlePlaybackLocked();
        playing_ = !playing_;
        if (playing_) {
            direction_ = 1;
            targetChanged = true;
            generation = activeSession_->generation;
            const FrameIndex playbackStart = detail::PlaybackStartForPlay(
                requestedFrame_,
                playbackRange_);
            if (playbackStart != requestedFrame_) {
                requestedFrame_ = playbackStart;
                PresentRequestedFromCacheLocked();
            }
        }
        playbackFrameAccumulator_ = 0.0;
        statusUtf8_ = playing_ ? "正在播放" : "已暂停";
        nowPlaying = playing_;
    }
    if (targetChanged) {
        CancelInteractive(generation);
    }
    SetBackgroundConcurrency(
        nowPlaying ? detail::kPlayingBackgroundConcurrency : detail::kPausedBackgroundConcurrency);
    ScheduleWork();
}

void PlayerEngine::Impl::SetPlaying(const bool playing) {
    if (playing) {
        EndScrub();
    }
    bool targetChanged = false;
    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || (playing && !activeSession_)) {
            return;
        }
        ResetShuttlePlaybackLocked();
        playing_ = playing;
        if (playing_ && activeSession_) {
            direction_ = 1;
            targetChanged = true;
            generation = activeSession_->generation;
            const FrameIndex playbackStart = detail::PlaybackStartForPlay(
                requestedFrame_,
                playbackRange_);
            if (playbackStart != requestedFrame_) {
                requestedFrame_ = playbackStart;
                PresentRequestedFromCacheLocked();
            }
        }
        playbackFrameAccumulator_ = 0.0;
        statusUtf8_ = playing_ ? "正在播放" : "已暂停";
    }
    if (targetChanged) {
        CancelInteractive(generation);
    }
    SetBackgroundConcurrency(
        playing ? detail::kPlayingBackgroundConcurrency : detail::kPausedBackgroundConcurrency);
    ScheduleWork();
}

void PlayerEngine::Impl::BeginShuttlePlayback(
    const int direction,
    const double speedScale) {
    if (direction == 0 || !std::isfinite(speedScale) || speedScale <= 0.0) {
        return;
    }

    EndScrub();
    bool started = false;
    Generation generation = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || backgroundResourceMode_ || !activeSession_ ||
            activeSession_->TotalFrames() == 0U) {
            return;
        }

        CancelPostScrubHotFillLocked();
        shuttlePlayback_ = true;
        playing_ = true;
        direction_ = direction < 0 ? -1 : 1;
        playbackSpeedScale_ = std::clamp(
            speedScale,
            detail::kMinimumPlaybackSpeedScale,
            detail::kMaximumPlaybackSpeedScale);
        playbackFrameAccumulator_ = 0.0;
        requestedFrame_ = detail::ClampFrameToPlaybackRange(
            static_cast<std::int64_t>(requestedFrame_),
            detail::FullPlaybackRange(activeSession_->TotalFrames()));
        generation = activeSession_->generation;
        PresentRequestedFromCacheLocked();
        snapshotTelemetry_.initialized = false;
        statusUtf8_ = direction_ < 0
            ? "方向键反向快览"
            : "方向键正向快览";
        started = true;
    }

    if (started) {
        CancelInteractive(generation);
        SetBackgroundConcurrency(detail::kPlayingBackgroundConcurrency);
        ScheduleWork();
    }
}

void PlayerEngine::Impl::EndShuttlePlayback() {
    bool ended = false;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !shuttlePlayback_) {
            return;
        }
        playing_ = false;
        playbackFrameAccumulator_ = 0.0;
        ResetShuttlePlaybackLocked();
        snapshotTelemetry_.initialized = false;
        statusUtf8_ = "方向键快览已暂停";
        ended = true;
    }
    if (ended) {
        SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
        ScheduleWork();
    }
}

void PlayerEngine::Impl::StepFrame(const int delta) {
    if (delta == 0) {
        return;
    }

    EndScrub();

    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !activeSession_ || activeSession_->TotalFrames() == 0U) {
            return;
        }
        CancelPostScrubHotFillLocked();
        ResetShuttlePlaybackLocked();
        playing_ = false;
        playbackFrameAccumulator_ = 0.0;
        direction_ = delta < 0 ? -1 : 1;
        const std::int64_t target = static_cast<std::int64_t>(requestedFrame_)
            + static_cast<std::int64_t>(delta);
        requestedFrame_ = detail::ClampFrame(target, activeSession_->TotalFrames());
        generation = activeSession_->generation;
        PresentRequestedFromCacheLocked();
        statusUtf8_ = "已逐帧定位";
    }
    SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
    CancelForImmediateTarget(generation);
    ScheduleWork();
}

void PlayerEngine::Impl::BeginScrub() {
    Generation generation = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || pendingLoad_ || !activeSession_ ||
            activeSession_->TotalFrames() == 0U) {
            return;
        }

        generation = activeSession_->generation;
        const SourceKind sourceKind = activeSession_->kind;
        const bool entered = !scrubbing_ || scrubGeneration_ != generation;
        scrubbing_ = true;
        scrubGeneration_ = generation;
        postScrubHotFillActive_ = false;
        postScrubHotFillGeneration_ = 0U;
        ResetShuttlePlaybackLocked();
        playing_ = false;
        playbackFrameAccumulator_ = 0.0;
        PresentRequestedFromCacheLocked();
        statusUtf8_ = "正在快速定位";
        if (entered) {
            if (sourceKind == SourceKind::Video) {
                videoScheduler_.BeginLatestWins(generation);
            } else {
                scheduler_.BeginLatestWinsPreservingBackground(generation);
            }
        }
        ScheduleScrubTargetLocked();
    }
    SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
}

ScrubUpdateResult PlayerEngine::Impl::UpdateScrub(const FrameIndex frame) {
    bool needsBegin = false;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || pendingLoad_ || !activeSession_ ||
            activeSession_->TotalFrames() == 0U) {
            return {};
        }
        needsBegin = !scrubbing_ ||
            scrubGeneration_ != activeSession_->generation;
    }
    if (needsBegin) {
        BeginScrub();
    }

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !scrubbing_ || !activeSession_ ||
            activeSession_->generation != scrubGeneration_ ||
            activeSession_->TotalFrames() == 0U) {
            return {};
        }

        const FrameIndex target = std::min<FrameIndex>(
            frame,
            static_cast<FrameIndex>(activeSession_->TotalFrames() - 1U));
        if (target < requestedFrame_) {
            direction_ = -1;
        } else if (target > requestedFrame_) {
            direction_ = 1;
        }
        requestedFrame_ = target;
        playbackFrameAccumulator_ = 0.0;
        const Generation generation = activeSession_->generation;
        PresentRequestedFromCacheLocked();
        statusUtf8_ = "正在快速定位目标帧";
        ScheduleScrubTargetLocked();
        return ScrubUpdateResult{true, true, requestedFrame_, generation};
    }
}

void PlayerEngine::Impl::EndScrub() {
    bool resumeScheduling = false;
    {
        std::scoped_lock lock(mutex_);
        if (!scrubbing_) {
            return;
        }

        const Generation generation = scrubGeneration_;
        const SourceKind sourceKind = activeSession_
            ? activeSession_->kind
            : SourceKind::None;
        // Capture and submit the final target before ending latest-wins, all
        // under one controller lock. UpdateScrub cannot split its target write
        // and submission across this transition.
        ScheduleScrubTargetLocked();
        if (sourceKind == SourceKind::PngSequence) {
            scheduler_.EndLatestWins(generation);
            if (!backgroundResourceMode_) {
                postScrubHotFillActive_ = true;
                postScrubHotFillGeneration_ = generation;
                postScrubHotFillStartFrame_ = detail::PlaybackStartForPlay(
                    requestedFrame_,
                    playbackRange_);
            } else {
                scheduler_.ResumeBackground(generation);
                postScrubHotFillActive_ = false;
                postScrubHotFillGeneration_ = 0U;
            }
        } else {
            videoScheduler_.EndLatestWins(generation);
            postScrubHotFillActive_ = false;
            postScrubHotFillGeneration_ = 0U;
        }
        scrubbing_ = false;
        scrubGeneration_ = 0U;
        playbackFrameAccumulator_ = 0.0;
        statusUtf8_ = "已定位目标帧";
        resumeScheduling = !shutdown_ && activeSession_ != nullptr;
    }
    if (resumeScheduling) {
        ScheduleWork();
    }
}

void PlayerEngine::Impl::Seek(const FrameIndex frame) {
    EndScrub();
    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !activeSession_ || activeSession_->TotalFrames() == 0U) {
            return;
        }
        CancelPostScrubHotFillLocked();
        ResetShuttlePlaybackLocked();
        playing_ = false;
        playbackFrameAccumulator_ = 0.0;
        requestedFrame_ = std::min<FrameIndex>(
            frame,
            static_cast<FrameIndex>(activeSession_->TotalFrames() - 1U));
        generation = activeSession_->generation;
        PresentRequestedFromCacheLocked();
        statusUtf8_ = "正在定位目标帧";
    }
    SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
    CancelForImmediateTarget(generation);
    ScheduleWork();
}

void PlayerEngine::Impl::SeekNormalized(const double normalizedPosition) {
    if (!std::isfinite(normalizedPosition)) {
        return;
    }

    std::size_t totalFrames = 0;
    {
        std::scoped_lock lock(mutex_);
        if (!activeSession_) {
            return;
        }
        totalFrames = activeSession_->TotalFrames();
    }
    if (totalFrames == 0) {
        return;
    }

    const double normalized = std::clamp(normalizedPosition, 0.0, 1.0);
    const double scaled = normalized * static_cast<double>(totalFrames - 1);
    Seek(static_cast<FrameIndex>(std::llround(scaled)));
}

void PlayerEngine::Impl::SetPlaybackRange(
    const FrameIndex startFrame,
    const FrameIndex endFrame) {
    bool targetChanged = false;
    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || !activeSession_ || activeSession_->TotalFrames() == 0U) {
            return;
        }

        const PlaybackRange normalized = detail::NormalizePlaybackRange(
            PlaybackRange{startFrame, endFrame},
            activeSession_->TotalFrames());
        const bool rangeChanged = normalized != playbackRange_;
        playbackRange_ = normalized;
        playbackRangeCustomized_ =
            normalized != detail::FullPlaybackRange(
                activeSession_->TotalFrames());
        if (pendingLoad_ && pendingLoad_->preservedPlaybackRange &&
            pendingLoad_->preservedPlaybackRange->sourceGeneration ==
                activeSession_->generation) {
            pendingLoad_->preservedPlaybackRange->range = playbackRange_;
            pendingLoad_->preservedPlaybackRange->customized =
                playbackRangeCustomized_;
            pendingLoad_->playbackRange =
                detail::PlaybackRangeForLoadedSequence(
                    playbackRange_,
                    pendingLoad_->session->TotalFrames(),
                    true,
                    playbackRangeCustomized_);
            pendingLoad_->playbackRangeCustomized =
                playbackRangeCustomized_;
        }
        if (!rangeChanged) {
            return;
        }
        targetChanged = true;
        generation = activeSession_->generation;
        if (playing_ && !shuttlePlayback_ &&
            !detail::PlaybackRangeContains(playbackRange_, requestedFrame_)) {
            requestedFrame_ = playbackRange_.startFrame;
            PresentRequestedFromCacheLocked();
        }
        playbackFrameAccumulator_ = 0.0;
        statusUtf8_ = playbackRangeCustomized_
            ? "播放范围已更新"
            : "已恢复完整播放范围";
    }

    if (targetChanged) {
        CancelInteractive(generation);
    }
    ScheduleWork();
}

void PlayerEngine::Impl::SetLoopPlayback(const bool enabled) {
    Generation generation = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || settings_.loopPlayback == enabled) {
            return;
        }
        settings_.loopPlayback = enabled;
        playbackFrameAccumulator_ = 0.0;
        if (activeSession_) {
            generation = activeSession_->generation;
        }
    }
    if (generation != 0) {
        CancelInteractive(generation);
        ScheduleWork();
    }
}

void PlayerEngine::Impl::SetFramesPerSecond(const double framesPerSecond) {
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0) {
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        settings_.framesPerSecond = std::clamp(
            framesPerSecond,
            detail::kMinimumFramesPerSecond,
            detail::kMaximumFramesPerSecond);
        if (externalClockEnabled_ || !activeSession_ ||
            activeSession_->kind == SourceKind::PngSequence) {
            sequenceFramesPerSecond_ = settings_.framesPerSecond;
        }
        playbackFrameAccumulator_ = 0.0;
    }
    ScheduleWork();
}

void PlayerEngine::Impl::SetDecodePercent(const std::uint32_t percent) {
    if (!detail::IsSupportedDecodePercent(percent)) {
        return;
    }

    EndScrub();

    std::filesystem::path sourcePath;
    std::optional<std::wstring> preferredFile;
    std::optional<PreservedPlaybackRange> preservedPlaybackRange;
    SourceKind sourceKind = SourceKind::None;
    FrameIndex preferredFrame = 0U;
    bool reloadRequired = false;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        settings_.decodePercent = percent;
        if (pendingLoad_) {
            if (pendingLoad_->session->decodePercent == percent) {
                return;
            }
            sourceKind = pendingLoad_->session->kind;
            sourcePath = pendingLoad_->session->sourcePath;
            preferredFrame = pendingLoad_->initialFrame;
            if (sourceKind == SourceKind::PngSequence) {
                preferredFile = pendingLoad_->session->frames[
                    pendingLoad_->initialFrame].relativePath;
            }
            preservedPlaybackRange =
                pendingLoad_->preservedPlaybackRange;
            reloadRequired = true;
        } else if (activeSession_ && activeSession_->decodePercent != percent) {
            sourceKind = activeSession_->kind;
            sourcePath = activeSession_->sourcePath;
            preferredFrame = currentFrame_;
            if (sourceKind == SourceKind::PngSequence) {
                preferredFile = activeSession_->frames[currentFrame_].relativePath;
            }
            preservedPlaybackRange = PreservedPlaybackRange{
                playbackRange_,
                playbackRangeCustomized_,
                activeSession_->generation};
            reloadRequired = true;
        }
    }

    if (reloadRequired) {
        if (sourceKind == SourceKind::Video) {
            (void)BeginVideoLoad(
                sourcePath,
                preferredFrame,
                std::move(preservedPlaybackRange));
        } else {
            (void)BeginLoad(
                sourcePath,
                std::move(preferredFile),
                std::move(preservedPlaybackRange));
        }
    }
}

void PlayerEngine::Impl::SetMemoryLimitBytes(const std::uint64_t bytes) {
    if (bytes < detail::kMinimumAcceptedMemoryLimitBytes) {
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        settings_.memoryLimitBytes = bytes;
        cacheCapacityBytes_ = detail::CacheCapacityForTotalLimit(bytes);
        ApplyCacheCapacityLocked(cacheCapacityBytes_);
        PresentRequestedFromCacheLocked();
    }
    ScheduleWork();
}

void PlayerEngine::Impl::SetResourceBudget(
    const EngineResourceBudget& budget) {
    if (budget.globalProcessLimitBytes <
        detail::kMinimumAcceptedMemoryLimitBytes) {
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        settings_.memoryLimitBytes = budget.globalProcessLimitBytes;
        cacheCapacityBytes_ = budget.laneCacheCapacityBytes;
        ApplyCacheCapacityLocked(cacheCapacityBytes_);
        PresentRequestedFromCacheLocked();
    }
    ScheduleWork();
}

void PlayerEngine::Impl::ApplyCacheCapacityLocked(
    const std::uint64_t capacityBytes) {
    if (!backgroundResourceMode_ || !activeSession_ ||
        activeSession_->TotalFrames() == 0U) {
        cache_.SetCapacityBytes(capacityBytes);
        snapshotTelemetry_.initialized = false;
        return;
    }

    const PlaybackRange fullRange = detail::FullPlaybackRange(
        activeSession_->TotalFrames());
    const PlaybackRange retentionRange = detail::PlaybackRangeContains(
        playbackRange_,
        requestedFrame_)
        ? playbackRange_
        : fullRange;
    cache_.SetCapacityBytesRetainingNeighborhood(
        capacityBytes,
        activeSession_->generation,
        requestedFrame_,
        direction_,
        retentionRange,
        settings_.loopPlayback);
    snapshotTelemetry_.initialized = false;
}

void PlayerEngine::Impl::SetBackgroundResourceMode(const bool enabled) {
    if (enabled) {
        EndScrub();
    }

    Generation generation = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || backgroundResourceMode_ == enabled) {
            return;
        }
        backgroundResourceMode_ = enabled;
        if (enabled) {
            ResetShuttlePlaybackLocked();
            playing_ = false;
            playbackFrameAccumulator_ = 0.0;
            CancelPostScrubHotFillLocked();
        }
        if (activeSession_) {
            generation = activeSession_->generation;
        }
    }

    if (enabled && generation != 0U) {
        CancelBackground(generation);
    } else if (!enabled && generation != 0U) {
        scheduler_.ResumeBackground(generation);
    }
    ScheduleWork();
}

PlayerSnapshot PlayerEngine::Impl::Snapshot() const {
    PlayerSnapshot snapshot;
    std::scoped_lock lock(mutex_);

    snapshot.hasSource = activeSession_ != nullptr;
    snapshot.hasSequence = snapshot.hasSource;
    snapshot.loading = pendingLoad_.has_value();
    snapshot.playing = playing_;
    snapshot.scrubbing = scrubbing_;
    snapshot.buffering = buffering_;
    snapshot.loopPlayback = settings_.loopPlayback;
    snapshot.currentFrame = currentFrame_;
    snapshot.requestedFrame = requestedFrame_;
    snapshot.playbackStartFrame = playbackRange_.startFrame;
    snapshot.playbackEndFrame = playbackRange_.endFrame;
    snapshot.sourceWidth = sourceWidth_;
    snapshot.sourceHeight = sourceHeight_;
    snapshot.decodedWidth = decodedWidth_;
    snapshot.decodedHeight = decodedHeight_;
    snapshot.decodePercent = activeSession_
        ? activeSession_->decodePercent
        : settings_.decodePercent;
    snapshot.targetFramesPerSecond = settings_.framesPerSecond;
    snapshot.actualFramesPerSecond = actualFramesPerSecond_;
    snapshot.memoryLimitBytes = settings_.memoryLimitBytes;
    snapshot.droppedFrames = droppedFrames_;
    snapshot.sourceKind = activeSession_
        ? activeSession_->kind
        : SourceKind::None;
    snapshot.requestedFrameFailed = activeSession_ != nullptr &&
        failedFrames_.contains(requestedFrame_);
    snapshot.displayRevision = displayRevision_;
    snapshot.displayFrame = displayFrame_;
    snapshot.statusUtf8 = statusUtf8_;
    snapshot.errorUtf8 = errorUtf8_;

    if (pendingLoad_) {
        try {
            snapshot.pendingSourcePathUtf8 = WideToUtf8(
                pendingLoad_->session->sourcePath.wstring());
        } catch (...) {
            snapshot.pendingSourcePathUtf8.clear();
        }
    }

    const Generation telemetryGeneration = activeSession_
        ? activeSession_->generation
        : 0U;
    const PlaybackRange telemetryRange = activeSession_
        ? ActiveTransportRangeLocked()
        : PlaybackRange{};
    const bool telemetryLoopPlayback = activeSession_
        ? ActiveTransportLoopLocked()
        : false;
    const auto telemetryNow = std::chrono::steady_clock::now();
    if (detail::ShouldRefreshSnapshotTelemetry(
            snapshotTelemetry_.initialized,
            snapshotTelemetry_.generation,
            telemetryGeneration,
            snapshotTelemetry_.playbackRange,
            telemetryRange,
            snapshotTelemetry_.loopPlayback,
            telemetryLoopPlayback,
            telemetryNow - snapshotTelemetry_.sampledAt)) {
        snapshotTelemetry_.initialized = true;
        snapshotTelemetry_.generation = telemetryGeneration;
        snapshotTelemetry_.playbackRange = telemetryRange;
        snapshotTelemetry_.loopPlayback = telemetryLoopPlayback;
        snapshotTelemetry_.sampledAt = telemetryNow;
        snapshotTelemetry_.cacheCapacityBytes = cache_.CapacityBytes();
        snapshotTelemetry_.cacheBytes = cache_.SizeBytes();
        snapshotTelemetry_.processMemory = detail::QueryProcessMemoryUsage();
        snapshotTelemetry_.cachedFrames = activeSession_
            ? cache_.Count(activeSession_->generation)
            : 0U;
        snapshotTelemetry_.readyFrames = 0U;
        if (activeSession_) {
            const FrameIndex readyStart = detail::PlaybackStartForPlay(
                requestedFrame_,
                telemetryRange);
            snapshotTelemetry_.readyFrames = cache_.CountContiguousInRange(
                activeSession_->generation,
                readyStart,
                1,
                telemetryRange,
                telemetryLoopPlayback,
                static_cast<std::size_t>(
                    detail::PlaybackRangeFrameCount(telemetryRange)));
        }
    }

    snapshot.cachedFrames = snapshotTelemetry_.cachedFrames;
    snapshot.cacheCapacityBytes = snapshotTelemetry_.cacheCapacityBytes;
    snapshot.cacheBytes = snapshotTelemetry_.cacheBytes;
    snapshot.processWorkingSetBytes =
        snapshotTelemetry_.processMemory.workingSetBytes;
    snapshot.processPrivateBytes = snapshotTelemetry_.processMemory.privateBytes;
    snapshot.readyAheadSeconds = settings_.framesPerSecond > 0.0 &&
            snapshotTelemetry_.readyFrames > 0U
        ? static_cast<double>(snapshotTelemetry_.readyFrames - 1U) /
            settings_.framesPerSecond
        : 0.0;

    if (!activeSession_) {
        return snapshot;
    }

    snapshot.totalFrames = activeSession_->TotalFrames();
    snapshot.generation = activeSession_->generation;
    const std::uint64_t frameBytes = displayFrame_
        ? static_cast<std::uint64_t>(displayFrame_->ByteSize())
        : 0;
    if (frameBytes > 0) {
        snapshot.estimatedCacheCapacityFrames = static_cast<std::size_t>(
            snapshot.cacheCapacityBytes / frameBytes);
    }
    if (snapshot.totalFrames > 0) {
        snapshot.cacheProgress = static_cast<double>(snapshot.cachedFrames)
            / static_cast<double>(snapshot.totalFrames);
    }

    try {
        snapshot.sourcePathUtf8 = WideToUtf8(
            activeSession_->sourcePath.wstring());
        snapshot.folderUtf8 = snapshot.sourcePathUtf8;
        if (activeSession_->kind == SourceKind::PngSequence &&
            currentFrame_ < activeSession_->frames.size()) {
            snapshot.currentFileUtf8 = WideToUtf8(
                activeSession_->frames[currentFrame_].relativePath);
        } else if (activeSession_->kind == SourceKind::Video) {
            snapshot.currentFileUtf8 = WideToUtf8(
                activeSession_->sourcePath.filename().wstring());
        }
    } catch (...) {
        snapshot.folderUtf8.clear();
        snapshot.sourcePathUtf8.clear();
        snapshot.currentFileUtf8.clear();
    }
    return snapshot;
}

void PlayerEngine::Impl::PresentRequestedFromCacheLocked() {
    if (!activeSession_) {
        buffering_ = false;
        return;
    }

    std::shared_ptr<const DecodedFrame> cached = cache_.Get(
        activeSession_->generation,
        requestedFrame_);
    if (!cached) {
        const bool alreadyDisplayed = displayFrame_
            && displayFrame_->generation == activeSession_->generation
            && displayFrame_->index == requestedFrame_;
        buffering_ = !alreadyDisplayed;
        return;
    }

    const bool changed = !displayFrame_
        || displayFrame_->generation != cached->generation
        || displayFrame_->index != cached->index
        || displayFrame_.get() != cached.get();
    displayFrame_ = std::move(cached);
    currentFrame_ = requestedFrame_;
    sourceWidth_ = displayFrame_->sourceWidth;
    sourceHeight_ = displayFrame_->sourceHeight;
    decodedWidth_ = displayFrame_->width;
    decodedHeight_ = displayFrame_->height;
    buffering_ = false;
    if (changed) {
        ++displayRevision_;
        ++presentedFramesSinceSample_;
    }
}

}  // namespace zt::sequence
