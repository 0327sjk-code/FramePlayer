#include "Core/ComparisonPlayerInternal.h"

#include "Core/ComparisonPlayerPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace zt::sequence {

namespace {

[[nodiscard]] bool FrameMatchesTarget(
    const PlayerSnapshot& snapshot,
    const comparison_detail::LaneFrameMapping mapping) noexcept {
    return mapping.exists && snapshot.hasSource &&
        snapshot.displayFrame != nullptr &&
        snapshot.displayFrame->generation == snapshot.generation &&
        snapshot.displayFrame->index == mapping.sourceFrame;
}

[[nodiscard]] bool FrameExistsInSource(
    const PlayerSnapshot& snapshot,
    const comparison_detail::LaneFrameMapping mapping) noexcept {
    return snapshot.hasSource && mapping.exists;
}

[[nodiscard]] bool FrameReadyOrBlack(
    const PlayerSnapshot& snapshot,
    const comparison_detail::LaneFrameMapping mapping) noexcept {
    return snapshot.hasSource &&
        (!mapping.exists || FrameMatchesTarget(snapshot, mapping));
}

[[nodiscard]] comparison_detail::SequenceFrameOffsetDomain OffsetDomain(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary,
    const bool comparisonEnabled) noexcept {
    const bool active = comparisonEnabled &&
        primary.hasSource && secondary.hasSource;
    return comparison_detail::ResolveSequenceFrameOffsetDomain(
        primary.sourceKind,
        primary.totalFrames,
        secondary.sourceKind,
        secondary.totalFrames,
        active);
}

[[nodiscard]] comparison_detail::LaneFrameMapping LaneFrame(
    const PlayerSnapshot& snapshot,
    const FrameIndex sharedFrame,
    const bool primaryLane,
    const comparison_detail::SequenceFrameOffsetDomain domain,
    const FrameIndex sequenceFrameOffset) noexcept {
    return comparison_detail::MapSharedFrameToLane(
        sharedFrame,
        snapshot.totalFrames,
        comparison_detail::LaneUsesSequenceFrameOffset(
            snapshot.sourceKind,
            primaryLane,
            domain),
        sequenceFrameOffset);
}

[[nodiscard]] PlaybackRange LaneRange(
    const PlaybackRange sharedRange,
    const PlayerSnapshot& snapshot,
    const bool primaryLane,
    const comparison_detail::SequenceFrameOffsetDomain domain,
    const FrameIndex sequenceFrameOffset) noexcept {
    return comparison_detail::MapSharedPlaybackRangeToLane(
        sharedRange,
        snapshot.totalFrames,
        comparison_detail::LaneUsesSequenceFrameOffset(
            snapshot.sourceKind,
            primaryLane,
            domain),
        sequenceFrameOffset);
}

[[nodiscard]] bool SameRange(
    const PlayerSnapshot& snapshot,
    const PlaybackRange range) noexcept {
    return snapshot.playbackStartFrame == range.startFrame &&
        snapshot.playbackEndFrame == range.endFrame;
}

}  // namespace

void ComparisonPlayer::Impl::SynchronizeFromSinglePlayer(
    const PlayerSnapshot& primary) {
    playing_ = primary.playing;
    scrubbing_ = primary.scrubbing;
    loopPlayback_ = primary.loopPlayback;
    requestedFrame_ = primary.requestedFrame;
    rangeIntent_ = PlaybackRange{
        primary.playbackStartFrame,
        primary.playbackEndFrame};
    effectiveRange_ = rangeIntent_;
    commonTotalFrames_ = primary.totalFrames;
    framesPerSecond_ = primary.targetFramesPerSecond;
    memoryLimitBytes_ = primary.memoryLimitBytes;
    decodePercent_ = primary.decodePercent;
    actualFramesPerSecond_ = primary.actualFramesPerSecond;
    statusUtf8_ = primary.statusUtf8;
    errorUtf8_ = primary.errorUtf8;
}

void ComparisonPlayer::Impl::BeginDecodeLaneTransition(
    PlayerEngine& engine,
    PendingLoadState& pendingLoad,
    DecodeLaneTransition& transition,
    const PlayerSnapshot& before,
    const std::uint32_t percent,
    const PendingLoadPurpose purpose) {
    transition = {};
    if (before.decodePercent == percent && !before.loading) {
        engine.SetDecodePercent(percent);
        return;
    }

    transition.required = true;
    transition.completed = false;
    transition.succeeded = false;
    pendingLoad = {};
    pendingLoad.pending = true;
    pendingLoad.previousGeneration = before.generation;
    pendingLoad.purpose = purpose;
    pendingLoad.expectedDecodePercent = percent;

    engine.SetDecodePercent(percent);
    const PlayerSnapshot after = engine.Snapshot();
    if (after.loading) {
        return;
    }

    transition.completed = true;
    transition.succeeded = after.decodePercent == percent &&
        (!before.hasSource || after.generation != before.generation);
    pendingLoad = {};
}

void ComparisonPlayer::Impl::StartDecodePercentTransaction(
    const std::uint32_t percent,
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary) {
    if (primary.decodePercent == percent &&
        (!secondary_ || secondary.decodePercent == percent)) {
        decodePercent_ = percent;
        primary_->SetDecodePercent(percent);
        if (secondary_) {
            secondary_->SetDecodePercent(percent);
        }
        statusUtf8_ = "左右解码比例无需切换";
        return;
    }

    if (!primary.hasSource && !secondary.hasSource) {
        primary_->SetDecodePercent(percent);
        if (secondary_) {
            secondary_->SetDecodePercent(percent);
        }
        decodePercent_ = percent;
        statusUtf8_ = "左右解码比例已同步设置";
        return;
    }

    PauseForPendingOperation();
    decodeTransaction_ = {};
    decodeTransaction_.phase = DecodePercentTransaction::Phase::Applying;
    decodeTransaction_.previousPercent = decodePercent_;
    decodeTransaction_.targetPercent = percent;

    BeginDecodeLaneTransition(
        *primary_,
        primaryLoad_,
        decodeTransaction_.primary,
        primary,
        percent,
        PendingLoadPurpose::DecodeApply);
    if (secondary_) {
        BeginDecodeLaneTransition(
            *secondary_,
            secondaryLoad_,
            decodeTransaction_.secondary,
            secondary,
            percent,
            PendingLoadPurpose::DecodeApply);
    }
    ClearComparisonError();
    statusUtf8_ = "正在事务化切换左右解码比例";
}

void ComparisonPlayer::Impl::BeginDecodePercentRollback(
    PlayerSnapshot& primary,
    PlayerSnapshot& secondary) {
    const std::uint32_t rollbackPercent =
        decodeTransaction_.previousPercent;
    decodeTransaction_.phase = DecodePercentTransaction::Phase::RollingBack;
    decodeTransaction_.primary = {};
    decodeTransaction_.secondary = {};

    primary = primary_->Snapshot();
    secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    BeginDecodeLaneTransition(
        *primary_,
        primaryLoad_,
        decodeTransaction_.primary,
        primary,
        rollbackPercent,
        PendingLoadPurpose::DecodeRollback);
    if (secondary_) {
        BeginDecodeLaneTransition(
            *secondary_,
            secondaryLoad_,
            decodeTransaction_.secondary,
            secondary,
            rollbackPercent,
            PendingLoadPurpose::DecodeRollback);
    }
    statusUtf8_ = "解码比例切换失败，正在同步回滚左右画面";
    errorUtf8_ = "左右解码比例切换失败，正在恢复原比例";
}

void ComparisonPlayer::Impl::ProcessCompletedDecodeTransaction(
    PlayerSnapshot& primary,
    PlayerSnapshot& secondary,
    bool& contextChanged) {
    if (!DecodeTransactionActive() ||
        !decodeTransaction_.primary.completed ||
        !decodeTransaction_.secondary.completed) {
        return;
    }

    const bool succeeded = decodeTransaction_.primary.succeeded &&
        decodeTransaction_.secondary.succeeded;
    if (decodeTransaction_.phase ==
        DecodePercentTransaction::Phase::Applying) {
        if (!succeeded) {
            BeginDecodePercentRollback(primary, secondary);
            contextChanged = true;
            return;
        }

        decodePercent_ = decodeTransaction_.targetPercent;
        decodeTransaction_ = {};
        ClearComparisonError();
        statusUtf8_ = "左右解码比例已同步切换";
        contextChanged = true;
        return;
    }

    const std::uint32_t restoredPercent =
        decodeTransaction_.previousPercent;
    decodeTransaction_ = {};
    contextChanged = true;
    if (succeeded) {
        decodePercent_ = restoredPercent;
        errorUtf8_ = "左右解码比例切换失败，已恢复原比例";
        statusUtf8_ = "左右解码比例已完成回滚";
        return;
    }

    SetEffectivePlaying(false);
    pendingPlaybackIntent_.reset();
    queuedDecodePercent_.reset();
    errorUtf8_ = "左右解码比例回滚失败，播放保持暂停";
    statusUtf8_ = "解码比例回滚未完成";
}

void ComparisonPlayer::Impl::ProcessPendingLoads(
    PlayerSnapshot& primary,
    PlayerSnapshot& secondary) {
    bool contextChanged = false;

    if (primaryLoad_.pending && !primary.loading) {
        if (primaryLoad_.purpose == PendingLoadPurpose::Source) {
            const bool succeeded = primary.hasSource &&
                primary.generation != primaryLoad_.previousGeneration;
            if (succeeded) {
                if (primaryLoad_.resetSequenceFrameOffsetOnSuccess &&
                    (primaryLoad_.previousSourceKind ==
                            SourceKind::PngSequence ||
                        primary.sourceKind == SourceKind::PngSequence)) {
                    sequenceFrameOffset_ = 0U;
                }
                if (primaryLoad_.adoptRangeOnSuccess ||
                    primaryLoad_.previousGeneration == 0U) {
                    rangeIntent_ = PlaybackRange{
                        primary.playbackStartFrame,
                        primary.playbackEndFrame};
                    if (secondary.hasSource && rangeIntent_ ==
                        detail::FullPlaybackRange(primary.totalFrames)) {
                        rangeIntent_ = detail::FullPlaybackRange(std::max(
                            primary.totalFrames,
                            secondary.totalFrames));
                    }
                }
                statusUtf8_ = "左侧来源已加载";
                contextChanged = true;
            } else {
                errorUtf8_ = "左侧来源加载失败";
                if (!primary.errorUtf8.empty()) {
                    errorUtf8_ += "：" + primary.errorUtf8;
                }
            }
        } else {
            decodeTransaction_.primary.completed = true;
            decodeTransaction_.primary.succeeded = primary.hasSource &&
                primary.generation != primaryLoad_.previousGeneration &&
                primary.decodePercent == primaryLoad_.expectedDecodePercent;
        }
        primaryLoad_ = {};
    }

    if (secondaryLoad_.pending && !secondary.loading) {
        if (secondaryLoad_.purpose == PendingLoadPurpose::Source) {
            const bool succeeded = secondary.hasSource &&
                secondary.generation != secondaryLoad_.previousGeneration;
            if (succeeded) {
                if (secondaryLoad_.resetSequenceFrameOffsetOnSuccess &&
                    (secondaryLoad_.previousSourceKind ==
                            SourceKind::PngSequence ||
                        secondary.sourceKind == SourceKind::PngSequence)) {
                    sequenceFrameOffset_ = 0U;
                }
                if (secondaryLoad_.resetSharedFrameOnSuccess) {
                    // rangeIntent_ intentionally retains the unshifted source
                    // range while a sequence offset is active. Compare the
                    // effective shared range here so replacing the video with
                    // a longer source still expands a previously-full
                    // transport range.
                    const bool rangeFollowedPrimaryFull =
                        comparison_detail::IsFullSharedPlaybackRange(
                            effectiveRange_,
                            commonTotalFrames_ > 0U
                                ? commonTotalFrames_
                                : primary.totalFrames);
                    const std::size_t expandedTotal = std::max(
                        primary.totalFrames,
                        secondary.totalFrames);
                    if (rangeFollowedPrimaryFull) {
                        rangeIntent_ = detail::FullPlaybackRange(expandedTotal);
                    }
                    requestedFrame_ = 0U;
                    direction_ = 1;
                    playbackFrameAccumulator_ = 0.0;
                }
                statusUtf8_ = "右侧对比来源已加载";
                contextChanged = true;
            } else {
                errorUtf8_ = "右侧来源加载失败";
                if (!secondary.errorUtf8.empty()) {
                    errorUtf8_ += "：" + secondary.errorUtf8;
                }
            }
        } else {
            decodeTransaction_.secondary.completed = true;
            decodeTransaction_.secondary.succeeded = secondary.hasSource &&
                secondary.generation != secondaryLoad_.previousGeneration &&
                secondary.decodePercent == secondaryLoad_.expectedDecodePercent;
        }
        secondaryLoad_ = {};
    }

    ProcessCompletedDecodeTransaction(primary, secondary, contextChanged);

    if (contextChanged) {
        if (scrubbing_) {
            EndScrub();
        }
        UpdateSharedDomain(primary, secondary);
        ApplySharedContext(primary, secondary);
        primary = primary_->Snapshot();
        secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    }

    if (!HasTrackedPendingOperation() && !primary.loading &&
        !secondary.loading && queuedDecodePercent_) {
        const std::uint32_t queuedPercent = *queuedDecodePercent_;
        queuedDecodePercent_.reset();
        StartDecodePercentTransaction(queuedPercent, primary, secondary);
        primary = primary_->Snapshot();
        secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    }
}

void ComparisonPlayer::Impl::ApplySharedContext(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary) {
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    primary_->SetExternalClockEnabled(true);
    primary_->SetFramesPerSecond(framesPerSecond_);
    primary_->SetLoopPlayback(loopPlayback_);
    if (primary.hasSource) {
        const PlaybackRange primaryRange = LaneRange(
            effectiveRange_,
            primary,
            true,
            offsetDomain,
            sequenceFrameOffset_);
        primary_->SetPlaybackRange(
            primaryRange.startFrame,
            primaryRange.endFrame);
    }
    primary_->SetPlaying(playing_);

    if (secondary_) {
        secondary_->SetExternalClockEnabled(true);
        secondary_->SetFramesPerSecond(framesPerSecond_);
        secondary_->SetLoopPlayback(loopPlayback_);
        if (secondary.hasSource) {
            const PlaybackRange secondaryRange = LaneRange(
                effectiveRange_,
                secondary,
                false,
                offsetDomain,
                sequenceFrameOffset_);
            secondary_->SetPlaybackRange(
                secondaryRange.startFrame,
                secondaryRange.endFrame);
        }
        secondary_->SetPlaying(playing_);
    }

    BroadcastFrameRequest(
        playing_
            ? FrameRequestKind::PlaybackAdvance
            : FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary,
        true);
}

void ComparisonPlayer::Impl::UpdateSharedDomain(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary) {
    const bool active = comparisonEnabled_ &&
        primary.hasSource && secondary.hasSource;
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    if (offsetDomain.available) {
        sequenceFrameOffset_ = std::min(
            sequenceFrameOffset_,
            offsetDomain.maximum);
    }
    commonTotalFrames_ =
        comparison_detail::CommonTotalFramesWithSequenceOffset(
        primary.totalFrames,
        primary.sourceKind,
        secondary.totalFrames,
        secondary.sourceKind,
        active,
        sequenceFrameOffset_);
    effectiveRange_ = comparison_detail::EffectivePlaybackRange(
        rangeIntent_,
        commonTotalFrames_);

    if (commonTotalFrames_ == 0U) {
        requestedFrame_ = 0U;
        return;
    }

    requestedFrame_ = std::min<FrameIndex>(
        requestedFrame_,
        static_cast<FrameIndex>(commonTotalFrames_ - 1U));
    if (playing_ && !detail::PlaybackRangeContains(
            effectiveRange_,
            requestedFrame_)) {
        requestedFrame_ = effectiveRange_.startFrame;
    }

    const PlaybackRange primaryRange = LaneRange(
        effectiveRange_,
        primary,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    if (primary.hasSource && !SameRange(primary, primaryRange)) {
        primary_->SetPlaybackRange(
            primaryRange.startFrame,
            primaryRange.endFrame);
    }
    const PlaybackRange secondaryRange = LaneRange(
        effectiveRange_,
        secondary,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    if (secondary.hasSource && secondary_ &&
        !SameRange(secondary, secondaryRange)) {
        secondary_->SetPlaybackRange(
            secondaryRange.startFrame,
            secondaryRange.endFrame);
    }
}

void ComparisonPlayer::Impl::BroadcastFrameRequest(
    const FrameRequestKind requestKind,
    const int direction,
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary,
    const bool force) {
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    const comparison_detail::LaneFrameMapping primaryFrame = LaneFrame(
        primary,
        requestedFrame_,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    const comparison_detail::LaneFrameMapping secondaryFrame = LaneFrame(
        secondary,
        requestedFrame_,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    if (FrameExistsInSource(primary, primaryFrame) &&
        (force || lastPrimaryRequestGeneration_ != primary.generation ||
            lastPrimaryRequestedFrame_ != primaryFrame.sourceFrame)) {
        if (scrubbing_ && requestKind == FrameRequestKind::InteractiveSeek) {
            primary_->UpdateScrub(primaryFrame.sourceFrame);
        } else {
            primary_->RequestFrame(
                primaryFrame.sourceFrame,
                requestKind,
                direction);
        }
        lastPrimaryRequestGeneration_ = primary.generation;
        lastPrimaryRequestedFrame_ = primaryFrame.sourceFrame;
    }
    if (secondary_ && FrameExistsInSource(secondary, secondaryFrame) &&
        (force || lastSecondaryRequestGeneration_ != secondary.generation ||
            lastSecondaryRequestedFrame_ != secondaryFrame.sourceFrame)) {
        if (scrubbing_ && requestKind == FrameRequestKind::InteractiveSeek) {
            secondary_->UpdateScrub(secondaryFrame.sourceFrame);
        } else {
            secondary_->RequestFrame(
                secondaryFrame.sourceFrame,
                requestKind,
                direction);
        }
        lastSecondaryRequestGeneration_ = secondary.generation;
        lastSecondaryRequestedFrame_ = secondaryFrame.sourceFrame;
    }
}

bool ComparisonPlayer::Impl::TargetReady(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary) const noexcept {
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    const comparison_detail::LaneFrameMapping primaryFrame = LaneFrame(
        primary,
        requestedFrame_,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    if (!FrameReadyOrBlack(primary, primaryFrame)) {
        return false;
    }
    const bool active = comparisonEnabled_ &&
        primary.hasSource && secondary.hasSource;
    const comparison_detail::LaneFrameMapping secondaryFrame = LaneFrame(
        secondary,
        requestedFrame_,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    return !active || FrameReadyOrBlack(secondary, secondaryFrame);
}

bool ComparisonPlayer::Impl::CommitPresentedPair(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary) {
    if (DecodeTransactionActive()) {
        pairReady_ = false;
        return false;
    }
    const bool active = comparisonEnabled_ &&
        primary.hasSource && secondary.hasSource;
    if (!active || !TargetReady(primary, secondary)) {
        pairReady_ = false;
        return false;
    }

    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    const comparison_detail::LaneFrameMapping primaryTarget = LaneFrame(
        primary,
        requestedFrame_,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    const comparison_detail::LaneFrameMapping secondaryTarget = LaneFrame(
        secondary,
        requestedFrame_,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    const bool primaryAvailable = FrameExistsInSource(
        primary,
        primaryTarget);
    const bool secondaryAvailable = FrameExistsInSource(
        secondary,
        secondaryTarget);
    const std::shared_ptr<const DecodedFrame> primaryFrame = primaryAvailable
        ? primary.displayFrame
        : std::shared_ptr<const DecodedFrame>{};
    const std::shared_ptr<const DecodedFrame> secondaryFrame = secondaryAvailable
        ? secondary.displayFrame
        : std::shared_ptr<const DecodedFrame>{};
    const bool changed = !presentedPair_.committed ||
        presentedPair_.frame != requestedFrame_ ||
        presentedPair_.primaryGeneration != primary.generation ||
        presentedPair_.secondaryGeneration != secondary.generation ||
        presentedPair_.primaryAvailable != primaryAvailable ||
        presentedPair_.secondaryAvailable != secondaryAvailable ||
        presentedPair_.primaryFrame.get() != primaryFrame.get() ||
        presentedPair_.secondaryFrame.get() != secondaryFrame.get();
    if (changed) {
        presentedPair_.committed = true;
        presentedPair_.primaryAvailable = primaryAvailable;
        presentedPair_.secondaryAvailable = secondaryAvailable;
        presentedPair_.frame = requestedFrame_;
        presentedPair_.primaryGeneration = primary.generation;
        presentedPair_.secondaryGeneration = secondary.generation;
        presentedPair_.primaryFrame = primaryFrame;
        presentedPair_.secondaryFrame = secondaryFrame;
        ++presentedPair_.revision;
        ++presentedPairsSinceSample_;
    }
    pairReady_ = true;
    return true;
}

bool ComparisonPlayer::Impl::HasRequestedFrameFailure(
    const PlayerSnapshot& primary,
    const PlayerSnapshot& secondary,
    std::string& detail) const {
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    const comparison_detail::LaneFrameMapping primaryFrame = LaneFrame(
        primary,
        requestedFrame_,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    const comparison_detail::LaneFrameMapping secondaryFrame = LaneFrame(
        secondary,
        requestedFrame_,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    if (FrameExistsInSource(primary, primaryFrame) &&
        primary.requestedFrame == primaryFrame.sourceFrame &&
        primary.requestedFrameFailed) {
        detail = "左侧第 " + std::to_string(
            static_cast<std::uint64_t>(primaryFrame.sourceFrame) + 1U) +
            " 帧解码失败";
        if (!primary.errorUtf8.empty()) {
            detail += "：" + primary.errorUtf8;
        }
        return true;
    }
    const bool active = comparisonEnabled_ &&
        primary.hasSource && secondary.hasSource;
    if (active && FrameExistsInSource(secondary, secondaryFrame) &&
        secondary.requestedFrame == secondaryFrame.sourceFrame &&
        secondary.requestedFrameFailed) {
        detail = "右侧第 " + std::to_string(
            static_cast<std::uint64_t>(secondaryFrame.sourceFrame) + 1U) +
            " 帧解码失败";
        if (!secondary.errorUtf8.empty()) {
            detail += "：" + secondary.errorUtf8;
        }
        return true;
    }
    return false;
}

void ComparisonPlayer::Impl::PauseForDecodeFailure(std::string detail) {
    playing_ = false;
    if (pendingPlaybackIntent_) {
        *pendingPlaybackIntent_ = false;
    }
    playbackFrameAccumulator_ = 0.0;
    errorUtf8_ = std::move(detail);
    statusUtf8_ = "对比播放已暂停";
    primary_->SetPlaying(false);
    if (secondary_) {
        secondary_->SetPlaying(false);
    }
}

void ComparisonPlayer::Impl::ClearComparisonError() {
    errorUtf8_.clear();
}

void ComparisonPlayer::Impl::UpdateActualFramesPerSecond(
    const double elapsedSeconds) {
    fpsSampleElapsed_ += elapsedSeconds;
    if (fpsSampleElapsed_ < detail::kFpsSampleSeconds) {
        return;
    }
    actualFramesPerSecond_ = fpsSampleElapsed_ > 0.0
        ? static_cast<double>(presentedPairsSinceSample_) /
            fpsSampleElapsed_
        : 0.0;
    fpsSampleElapsed_ = 0.0;
    presentedPairsSinceSample_ = 0U;
}

void ComparisonPlayer::Impl::Tick(const double elapsedSeconds) {
    if (shutdown_ || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0) {
        return;
    }

    if (!comparisonEnabled_) {
        primary_->Tick(elapsedSeconds);
        PlayerSnapshot primary = primary_->Snapshot();
        if (primaryPlaybackIntentAfterComparisonExit_ && !primary.loading) {
            const bool desiredPlaying =
                *primaryPlaybackIntentAfterComparisonExit_;
            primaryPlaybackIntentAfterComparisonExit_.reset();
            primary_->SetPlaying(desiredPlaying);
            primary = primary_->Snapshot();
        }
        SynchronizeFromSinglePlayer(primary);
        return;
    }

    primary_->Tick(elapsedSeconds);
    if (secondary_) {
        secondary_->Tick(elapsedSeconds);
    }

    PlayerSnapshot primary = primary_->Snapshot();
    PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    ProcessPendingLoads(primary, secondary);
    UpdateSharedDomain(primary, secondary);

    if (std::abs(primary.targetFramesPerSecond - framesPerSecond_) > 0.0001) {
        primary_->SetFramesPerSecond(framesPerSecond_);
    }
    if (secondary_ &&
        std::abs(secondary.targetFramesPerSecond - framesPerSecond_) > 0.0001) {
        secondary_->SetFramesPerSecond(framesPerSecond_);
    }

    BroadcastFrameRequest(
        playing_
            ? FrameRequestKind::PlaybackAdvance
            : FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary);
    primary = primary_->Snapshot();
    secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};

    static_cast<void>(CommitPresentedPair(primary, secondary));
    std::string failure;
    if (HasRequestedFrameFailure(primary, secondary, failure)) {
        PauseForDecodeFailure(std::move(failure));
        UpdateActualFramesPerSecond(elapsedSeconds);
        return;
    }

    const bool pendingLoad = HasTrackedPendingOperation() ||
        primary.loading || secondary.loading;
    bool targetReady = TargetReady(primary, secondary);
    if (pendingPlaybackIntent_ && !pendingLoad && targetReady) {
        const bool desiredPlaying = *pendingPlaybackIntent_;
        pendingPlaybackIntent_.reset();
        if (desiredPlaying) {
            SetPlaying(true);
        } else {
            SetEffectivePlaying(false);
        }
        primary = primary_->Snapshot();
        secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
        targetReady = TargetReady(primary, secondary);
        static_cast<void>(CommitPresentedPair(primary, secondary));
    }

    if (!playing_ || pendingLoad || !targetReady ||
        commonTotalFrames_ == 0U) {
        if (playing_ && !targetReady) {
            playbackFrameAccumulator_ = 0.0;
            statusUtf8_ = "正在等待左右目标帧";
        }
        UpdateActualFramesPerSecond(elapsedSeconds);
        return;
    }

    playbackFrameAccumulator_ += elapsedSeconds * framesPerSecond_;
    const double wholeSteps = std::floor(playbackFrameAccumulator_);
    if (wholeSteps < 1.0) {
        UpdateActualFramesPerSecond(elapsedSeconds);
        return;
    }
    playbackFrameAccumulator_ -= wholeSteps;
    const double maximumSafeSteps = static_cast<double>(
        std::numeric_limits<std::int64_t>::max() / 2);
    const std::int64_t steps = static_cast<std::int64_t>(
        std::min(wholeSteps, maximumSafeSteps));
    const std::int64_t signedSteps = direction_ < 0 ? -steps : steps;
    const comparison_detail::AdvanceResult advance =
        comparison_detail::AdvanceFrame(
            requestedFrame_,
            signedSteps,
            effectiveRange_,
            loopPlayback_);
    requestedFrame_ = advance.target;
    if (advance.stoppedAtBoundary) {
        playing_ = false;
        playbackFrameAccumulator_ = 0.0;
        primary_->SetPlaying(false);
        if (secondary_) {
            secondary_->SetPlaying(false);
        }
        statusUtf8_ = direction_ < 0
            ? "已到共享播放起始帧"
            : "已到共享播放结束帧";
    }

    BroadcastFrameRequest(
        FrameRequestKind::PlaybackAdvance,
        direction_,
        primary,
        secondary,
        true);
    primary = primary_->Snapshot();
    secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    static_cast<void>(CommitPresentedPair(primary, secondary));
    if (playing_) {
        statusUtf8_ = pairReady_ ? "正在同步播放" : "正在同步缓冲";
    }
    UpdateActualFramesPerSecond(elapsedSeconds);
}

void ComparisonPlayer::Impl::TogglePlayback() {
    if (backgroundResourceMode_) {
        return;
    }
    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            const bool desiredPlaying =
                !*primaryPlaybackIntentAfterComparisonExit_;
            *primaryPlaybackIntentAfterComparisonExit_ = desiredPlaying;
            primary_->SetPlaying(desiredPlaying);
            SynchronizeFromSinglePlayer(primary_->Snapshot());
            return;
        }
        primary_->TogglePlayback();
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }
    SetPlaying(pendingPlaybackIntent_
        ? !*pendingPlaybackIntent_
        : !playing_);
}

void ComparisonPlayer::Impl::SetPlaying(const bool playing) {
    if (shutdown_) {
        return;
    }
    if (backgroundResourceMode_ && playing) {
        return;
    }
    if (playing) {
        EndScrub();
    }
    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            *primaryPlaybackIntentAfterComparisonExit_ = playing;
        }
        primary_->SetPlaying(playing);
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }

    if (pendingPlaybackIntent_) {
        *pendingPlaybackIntent_ = playing;
        SetEffectivePlaying(false);
        statusUtf8_ = playing
            ? "加载完成后继续播放"
            : "已暂停，加载完成后保持暂停";
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    UpdateSharedDomain(primary, secondary);
    if (playing && (!primary.hasSource || commonTotalFrames_ == 0U)) {
        return;
    }

    playing_ = playing;
    direction_ = 1;
    playbackFrameAccumulator_ = 0.0;
    if (playing_ && !detail::PlaybackRangeContains(
            effectiveRange_, requestedFrame_)) {
        requestedFrame_ = effectiveRange_.startFrame;
    }
    primary_->SetPlaying(playing_);
    if (secondary_) {
        secondary_->SetPlaying(playing_);
    }
    BroadcastFrameRequest(
        playing_
            ? FrameRequestKind::PlaybackAdvance
            : FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary,
        true);
    statusUtf8_ = playing_ ? "正在同步播放" : "已暂停";
}

void ComparisonPlayer::Impl::StepFrame(const int delta) {
    if (shutdown_ || delta == 0) {
        return;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            *primaryPlaybackIntentAfterComparisonExit_ = false;
        }
        primary_->StepFrame(delta);
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    UpdateSharedDomain(primary, secondary);
    if (commonTotalFrames_ == 0U) {
        return;
    }
    MarkUserNavigationDuringPending();
    SetPlaying(false);
    direction_ = delta < 0 ? -1 : 1;
    const std::int64_t target = static_cast<std::int64_t>(requestedFrame_) +
        static_cast<std::int64_t>(delta);
    requestedFrame_ = detail::ClampFrame(target, commonTotalFrames_);
    ClearComparisonError();
    BroadcastFrameRequest(
        FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary,
        true);
    statusUtf8_ = "已同步逐帧定位";
}

void ComparisonPlayer::Impl::BeginScrub() {
    if (shutdown_ || scrubbing_) {
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    if (HasTrackedPendingOperation() || primary.loading || secondary.loading) {
        return;
    }

    MarkUserNavigationDuringPending();
    playing_ = false;
    scrubbing_ = true;
    playbackFrameAccumulator_ = 0.0;
    primary_->BeginScrub();
    if (secondary_) {
        secondary_->BeginScrub();
    }

    if (!comparisonEnabled_) {
        SynchronizeFromSinglePlayer(primary_->Snapshot());
    } else {
        statusUtf8_ = "正在同步快速定位";
    }
}

void ComparisonPlayer::Impl::UpdateScrub(const FrameIndex frame) {
    if (shutdown_) {
        return;
    }
    if (!scrubbing_) {
        BeginScrub();
    }
    if (!scrubbing_) {
        return;
    }

    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            *primaryPlaybackIntentAfterComparisonExit_ = false;
        }
        const ScrubUpdateResult result = primary_->UpdateScrub(frame);
        if (!result.accepted) {
            return;
        }
        playing_ = false;
        scrubbing_ = result.scrubbing;
        requestedFrame_ = result.requestedFrame;
        playbackFrameAccumulator_ = 0.0;
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    UpdateSharedDomain(primary, secondary);
    if (commonTotalFrames_ == 0U) {
        return;
    }

    MarkUserNavigationDuringPending();
    const FrameIndex target = std::min<FrameIndex>(
        frame,
        static_cast<FrameIndex>(commonTotalFrames_ - 1U));
    if (target < requestedFrame_) {
        direction_ = -1;
    } else if (target > requestedFrame_) {
        direction_ = 1;
    }
    requestedFrame_ = target;
    playbackFrameAccumulator_ = 0.0;
    ClearComparisonError();

    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    const comparison_detail::LaneFrameMapping primaryFrame = LaneFrame(
        primary,
        requestedFrame_,
        true,
        offsetDomain,
        sequenceFrameOffset_);
    const comparison_detail::LaneFrameMapping secondaryFrame = LaneFrame(
        secondary,
        requestedFrame_,
        false,
        offsetDomain,
        sequenceFrameOffset_);
    if (FrameExistsInSource(primary, primaryFrame)) {
        primary_->UpdateScrub(primaryFrame.sourceFrame);
        lastPrimaryRequestGeneration_ = primary.generation;
        lastPrimaryRequestedFrame_ = primaryFrame.sourceFrame;
    }
    if (secondary_ && FrameExistsInSource(secondary, secondaryFrame)) {
        secondary_->UpdateScrub(secondaryFrame.sourceFrame);
        lastSecondaryRequestGeneration_ = secondary.generation;
        lastSecondaryRequestedFrame_ = secondaryFrame.sourceFrame;
    }
    statusUtf8_ = "正在同步快速定位目标帧";
}

void ComparisonPlayer::Impl::EndScrub() {
    if (!scrubbing_) {
        return;
    }

    // Clear the shared flag before normal lane scheduling resumes so the next
    // comparison tick cannot route a normal paused request back into the
    // latest-wins transaction.
    scrubbing_ = false;
    primary_->EndScrub();
    if (secondary_) {
        secondary_->EndScrub();
    }
    playbackFrameAccumulator_ = 0.0;

    if (!comparisonEnabled_) {
        SynchronizeFromSinglePlayer(primary_->Snapshot());
    } else {
        statusUtf8_ = "已同步定位目标帧";
    }
}

void ComparisonPlayer::Impl::Seek(const FrameIndex frame) {
    if (shutdown_) {
        return;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            *primaryPlaybackIntentAfterComparisonExit_ = false;
        }
        primary_->Seek(frame);
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    UpdateSharedDomain(primary, secondary);
    if (commonTotalFrames_ == 0U) {
        return;
    }
    MarkUserNavigationDuringPending();
    SetPlaying(false);
    direction_ = frame < requestedFrame_ ? -1 : 1;
    requestedFrame_ = std::min<FrameIndex>(
        frame,
        static_cast<FrameIndex>(commonTotalFrames_ - 1U));
    ClearComparisonError();
    BroadcastFrameRequest(
        FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary,
        true);
    statusUtf8_ = "正在同步定位目标帧";
}

void ComparisonPlayer::Impl::SeekNormalized(
    const double normalizedPosition) {
    if (!std::isfinite(normalizedPosition)) {
        return;
    }
    if (!comparisonEnabled_) {
        if (primaryPlaybackIntentAfterComparisonExit_) {
            *primaryPlaybackIntentAfterComparisonExit_ = false;
        }
        primary_->SeekNormalized(normalizedPosition);
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }
    if (commonTotalFrames_ == 0U) {
        return;
    }
    const double normalized = std::clamp(normalizedPosition, 0.0, 1.0);
    const double scaled = normalized *
        static_cast<double>(commonTotalFrames_ - 1U);
    Seek(static_cast<FrameIndex>(std::llround(scaled)));
}

void ComparisonPlayer::Impl::SetPlaybackRange(
    const FrameIndex startFrame,
    const FrameIndex endFrame) {
    if (shutdown_) {
        return;
    }
    if (!comparisonEnabled_) {
        primary_->SetPlaybackRange(startFrame, endFrame);
        SynchronizeFromSinglePlayer(primary_->Snapshot());
        return;
    }

    rangeIntent_ = PlaybackRange{startFrame, endFrame};
    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    UpdateSharedDomain(primary, secondary);
    if (playing_ && !detail::PlaybackRangeContains(
            effectiveRange_, requestedFrame_)) {
        requestedFrame_ = effectiveRange_.startFrame;
        BroadcastFrameRequest(
            FrameRequestKind::PlaybackAdvance,
            direction_,
            primary,
            secondary,
            true);
    }
    playbackFrameAccumulator_ = 0.0;
    statusUtf8_ = "共享播放范围已更新";
}

void ComparisonPlayer::Impl::SetComparisonSequenceFrameOffset(
    const FrameIndex offset) {
    if (shutdown_ || !comparisonEnabled_) {
        return;
    }
    EndScrub();

    PlayerSnapshot primary = primary_->Snapshot();
    PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    if (HasTrackedPendingOperation() || primary.loading || secondary.loading) {
        return;
    }

    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(primary, secondary, comparisonEnabled_);
    if (!offsetDomain.available) {
        return;
    }

    const FrameIndex clampedOffset = std::min(offset, offsetDomain.maximum);
    if (sequenceFrameOffset_ == clampedOffset) {
        return;
    }

    sequenceFrameOffset_ = clampedOffset;
    playbackFrameAccumulator_ = 0.0;
    pairReady_ = false;
    ClearComparisonError();
    UpdateSharedDomain(primary, secondary);
    primary = primary_->Snapshot();
    secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    BroadcastFrameRequest(
        playing_
            ? FrameRequestKind::PlaybackAdvance
            : FrameRequestKind::InteractiveSeek,
        direction_,
        primary,
        secondary,
        true);
    statusUtf8_ = "序列对齐偏移已更新";
}

void ComparisonPlayer::Impl::SetLoopPlayback(const bool enabled) {
    if (shutdown_) {
        return;
    }
    loopPlayback_ = enabled;
    playbackFrameAccumulator_ = 0.0;
    primary_->SetLoopPlayback(enabled);
    if (secondary_) {
        secondary_->SetLoopPlayback(enabled);
    }
    if (!comparisonEnabled_) {
        SynchronizeFromSinglePlayer(primary_->Snapshot());
    }
}

void ComparisonPlayer::Impl::SetFramesPerSecond(
    const double framesPerSecond) {
    if (shutdown_ || !std::isfinite(framesPerSecond) ||
        framesPerSecond <= 0.0) {
        return;
    }
    framesPerSecond_ = std::clamp(
        framesPerSecond,
        detail::kMinimumFramesPerSecond,
        detail::kMaximumFramesPerSecond);
    playbackFrameAccumulator_ = 0.0;
    primary_->SetFramesPerSecond(framesPerSecond_);
    if (secondary_) {
        secondary_->SetFramesPerSecond(framesPerSecond_);
    }
    if (!comparisonEnabled_) {
        SynchronizeFromSinglePlayer(primary_->Snapshot());
    }
}

ComparisonPlayerSnapshot ComparisonPlayer::Impl::Snapshot() const {
    ComparisonPlayerSnapshot snapshot;
    snapshot.enabled = comparisonEnabled_;
    snapshot.primary = primary_ ? primary_->Snapshot() : PlayerSnapshot{};
    snapshot.secondary = secondary_ ? secondary_->Snapshot() : PlayerSnapshot{};
    snapshot.active = comparisonEnabled_ &&
        snapshot.primary.hasSource && snapshot.secondary.hasSource;
    const comparison_detail::SequenceFrameOffsetDomain offsetDomain =
        OffsetDomain(snapshot.primary, snapshot.secondary, comparisonEnabled_);
    snapshot.sequenceFrameOffsetAvailable = offsetDomain.available;
    snapshot.sequenceFrameOffsetOnPrimary = offsetDomain.available &&
        offsetDomain.onPrimary;
    snapshot.sequenceFrameOffset = offsetDomain.available
        ? std::min(sequenceFrameOffset_, offsetDomain.maximum)
        : 0U;
    snapshot.maximumSequenceFrameOffset = offsetDomain.available
        ? offsetDomain.maximum
        : 0U;

    if (!comparisonEnabled_) {
        snapshot.playing = snapshot.primary.playing;
        snapshot.scrubbing = snapshot.primary.scrubbing;
        snapshot.buffering = snapshot.primary.buffering;
        snapshot.loopPlayback = snapshot.primary.loopPlayback;
        snapshot.backgroundResourceMode = backgroundResourceMode_;
        snapshot.currentFrame = snapshot.primary.currentFrame;
        snapshot.requestedFrame = snapshot.primary.requestedFrame;
        snapshot.playbackStartFrame = snapshot.primary.playbackStartFrame;
        snapshot.playbackEndFrame = snapshot.primary.playbackEndFrame;
        snapshot.totalFrames = snapshot.primary.totalFrames;
        snapshot.targetFramesPerSecond = snapshot.primary.targetFramesPerSecond;
        snapshot.actualFramesPerSecond = snapshot.primary.actualFramesPerSecond;
        snapshot.decodePercent = snapshot.primary.decodePercent;
        snapshot.memoryLimitBytes = snapshot.primary.memoryLimitBytes;
        snapshot.cacheBytes = snapshot.primary.cacheBytes;
        snapshot.primaryGeneration = snapshot.primary.generation;
        snapshot.primaryDisplayFrame = snapshot.primary.displayFrame;
        snapshot.primaryFrameAvailable =
            snapshot.primary.displayFrame != nullptr;
        snapshot.statusUtf8 = snapshot.primary.statusUtf8;
        snapshot.errorUtf8 = snapshot.primary.errorUtf8;
        return snapshot;
    }

    snapshot.pairReady = pairReady_ && snapshot.active &&
        presentedPair_.committed &&
        presentedPair_.primaryGeneration == snapshot.primary.generation &&
        presentedPair_.secondaryGeneration == snapshot.secondary.generation &&
        presentedPair_.frame == requestedFrame_;
    snapshot.playing = playing_;
    snapshot.scrubbing = scrubbing_;
    snapshot.buffering = snapshot.primary.loading || snapshot.secondary.loading ||
        snapshot.primary.buffering || snapshot.secondary.buffering ||
        (snapshot.active && !snapshot.pairReady);
    snapshot.loopPlayback = loopPlayback_;
    snapshot.backgroundResourceMode = backgroundResourceMode_;
    snapshot.currentFrame = snapshot.active && presentedPair_.committed
        ? presentedPair_.frame
        : snapshot.primary.currentFrame;
    snapshot.requestedFrame = requestedFrame_;
    snapshot.playbackStartFrame = effectiveRange_.startFrame;
    snapshot.playbackEndFrame = effectiveRange_.endFrame;
    snapshot.totalFrames = commonTotalFrames_;
    snapshot.targetFramesPerSecond = framesPerSecond_;
    snapshot.actualFramesPerSecond = snapshot.active
        ? actualFramesPerSecond_
        : snapshot.primary.actualFramesPerSecond;
    snapshot.decodePercent = decodePercent_;
    snapshot.memoryLimitBytes = memoryLimitBytes_;
    snapshot.cacheBytes = snapshot.primary.cacheBytes +
        snapshot.secondary.cacheBytes;
    snapshot.pairRevision = presentedPair_.revision;
    snapshot.primaryGeneration = snapshot.primary.generation;
    snapshot.secondaryGeneration = snapshot.secondary.generation;
    snapshot.primaryFrameAvailable = snapshot.active
        ? presentedPair_.primaryAvailable
        : snapshot.primary.displayFrame != nullptr;
    snapshot.secondaryFrameAvailable = snapshot.active
        ? presentedPair_.secondaryAvailable
        : false;
    snapshot.primaryDisplayFrame = snapshot.active
        ? presentedPair_.primaryFrame
        : snapshot.primary.displayFrame;
    snapshot.secondaryDisplayFrame = snapshot.active
        ? presentedPair_.secondaryFrame
        : std::shared_ptr<const DecodedFrame>{};
    snapshot.statusUtf8 = statusUtf8_;
    snapshot.errorUtf8 = errorUtf8_;
    return snapshot;
}

}  // namespace zt::sequence
