#include "Core/ComparisonPlayerInternal.h"

#include "Core/ComparisonPlayerPolicy.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace zt::sequence {

ComparisonPlayer::ComparisonPlayer()
    : impl_(std::make_unique<Impl>()) {}

ComparisonPlayer::~ComparisonPlayer() = default;

bool ComparisonPlayer::LoadFolder(const std::filesystem::path& folder) {
    return impl_->LoadFolder(folder);
}

bool ComparisonPlayer::LoadSource(const std::filesystem::path& sourcePath) {
    return impl_->LoadSource(sourcePath);
}

bool ComparisonPlayer::ReloadFolder() {
    return impl_->ReloadFolder();
}

std::optional<SequenceExportSnapshot>
ComparisonPlayer::CaptureExportSnapshot() const {
    return impl_->CaptureExportSnapshot();
}

bool ComparisonPlayer::SetComparisonEnabled(const bool enabled) {
    return impl_->SetComparisonEnabled(enabled);
}

bool ComparisonPlayer::LoadSecondarySource(
    const std::filesystem::path& sourcePath) {
    return impl_->LoadSecondarySource(sourcePath);
}

void ComparisonPlayer::Tick(const double elapsedSeconds) {
    impl_->Tick(elapsedSeconds);
}

void ComparisonPlayer::TogglePlayback() {
    impl_->TogglePlayback();
}

void ComparisonPlayer::SetPlaying(const bool playing) {
    impl_->SetPlaying(playing);
}

void ComparisonPlayer::StepFrame(const int delta) {
    impl_->StepFrame(delta);
}

void ComparisonPlayer::BeginScrub() {
    impl_->BeginScrub();
}

void ComparisonPlayer::UpdateScrub(const FrameIndex frame) {
    impl_->UpdateScrub(frame);
}

void ComparisonPlayer::EndScrub() {
    impl_->EndScrub();
}

void ComparisonPlayer::Seek(const FrameIndex frame) {
    impl_->Seek(frame);
}

void ComparisonPlayer::SeekNormalized(const double normalizedPosition) {
    impl_->SeekNormalized(normalizedPosition);
}

void ComparisonPlayer::SetPlaybackRange(
    const FrameIndex startFrame,
    const FrameIndex endFrame) {
    impl_->SetPlaybackRange(startFrame, endFrame);
}

void ComparisonPlayer::SetLoopPlayback(const bool enabled) {
    impl_->SetLoopPlayback(enabled);
}

void ComparisonPlayer::SetFramesPerSecond(const double framesPerSecond) {
    impl_->SetFramesPerSecond(framesPerSecond);
}

void ComparisonPlayer::SetDecodePercent(const std::uint32_t percent) {
    impl_->SetDecodePercent(percent);
}

void ComparisonPlayer::SetMemoryLimitBytes(const std::uint64_t bytes) {
    impl_->SetMemoryLimitBytes(bytes);
}

void ComparisonPlayer::SetBackgroundResourceMode(const bool enabled) {
    impl_->SetBackgroundResourceMode(enabled);
}

ComparisonPlayerSnapshot ComparisonPlayer::Snapshot() const {
    return impl_->Snapshot();
}

void ComparisonPlayer::Shutdown() {
    impl_->Shutdown();
}

ComparisonPlayer::Impl::Impl()
    : primary_(std::make_unique<PlayerEngine>()) {
    const PlayerSnapshot initial = primary_->Snapshot();
    SynchronizeFromSinglePlayer(initial);
}

ComparisonPlayer::Impl::~Impl() {
    Shutdown();
}

bool ComparisonPlayer::Impl::HasTrackedPendingOperation() const noexcept {
    return primaryLoad_.pending || secondaryLoad_.pending ||
        DecodeTransactionActive();
}

bool ComparisonPlayer::Impl::DecodeTransactionActive() const noexcept {
    return decodeTransaction_.phase !=
        DecodePercentTransaction::Phase::Idle;
}

void ComparisonPlayer::Impl::SetEffectivePlaying(const bool playing) {
    playing_ = playing;
    playbackFrameAccumulator_ = 0.0;
    primary_->SetPlaying(playing);
    if (secondary_) {
        secondary_->SetPlaying(playing);
    }
}

void ComparisonPlayer::Impl::PauseForPendingOperation() {
    if (!pendingPlaybackIntent_) {
        pendingPlaybackIntent_ = playing_;
    }
    SetEffectivePlaying(false);
}

void ComparisonPlayer::Impl::RestorePendingPlaybackAfterRejectedOperation() {
    if (!pendingPlaybackIntent_ || HasTrackedPendingOperation()) {
        return;
    }
    const bool desiredPlaying = *pendingPlaybackIntent_;
    pendingPlaybackIntent_.reset();
    SetPlaying(desiredPlaying);
}

void ComparisonPlayer::Impl::MarkUserNavigationDuringPending() {
    if (pendingPlaybackIntent_) {
        *pendingPlaybackIntent_ = false;
    }
    primaryLoad_.resetSharedFrameOnSuccess = false;
    secondaryLoad_.resetSharedFrameOnSuccess = false;
}

bool ComparisonPlayer::Impl::LoadFolder(
    const std::filesystem::path& folder) {
    if (shutdown_) {
        return false;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        return primary_->LoadFolder(folder);
    }
    if (DecodeTransactionActive()) {
        errorUtf8_ = "解码比例切换尚未完成，请稍后再加载来源";
        return false;
    }

    const PlayerSnapshot before = primary_->Snapshot();
    PauseForPendingOperation();
    primaryLoad_.previousGeneration = before.generation;
    primaryLoad_.adoptRangeOnSuccess = true;
    primaryLoad_.purpose = PendingLoadPurpose::Source;
    return BeginPrimaryLoad(primary_->LoadFolder(folder));
}

bool ComparisonPlayer::Impl::LoadSource(
    const std::filesystem::path& sourcePath) {
    if (shutdown_) {
        return false;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        return primary_->LoadSource(sourcePath);
    }
    if (DecodeTransactionActive()) {
        errorUtf8_ = "解码比例切换尚未完成，请稍后再加载来源";
        return false;
    }

    const PlayerSnapshot before = primary_->Snapshot();
    PauseForPendingOperation();
    primaryLoad_.previousGeneration = before.generation;
    primaryLoad_.adoptRangeOnSuccess = true;
    primaryLoad_.purpose = PendingLoadPurpose::Source;
    return BeginPrimaryLoad(primary_->LoadSource(sourcePath));
}

bool ComparisonPlayer::Impl::ReloadFolder() {
    if (shutdown_) {
        return false;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        return primary_->ReloadFolder();
    }
    if (DecodeTransactionActive()) {
        errorUtf8_ = "解码比例切换尚未完成，请稍后再重新扫描";
        return false;
    }

    const PlayerSnapshot before = primary_->Snapshot();
    PauseForPendingOperation();
    primaryLoad_.previousGeneration = before.generation;
    primaryLoad_.adoptRangeOnSuccess = false;
    primaryLoad_.purpose = PendingLoadPurpose::Source;
    return BeginPrimaryLoad(primary_->ReloadFolder());
}

bool ComparisonPlayer::Impl::BeginPrimaryLoad(const bool accepted) {
    primaryLoad_.pending = accepted;
    if (!accepted) {
        primaryLoad_ = {};
        RestorePendingPlaybackAfterRejectedOperation();
    }
    return accepted;
}

std::optional<SequenceExportSnapshot>
ComparisonPlayer::Impl::CaptureExportSnapshot() const {
    return shutdown_ ? std::nullopt : primary_->CaptureExportSnapshot();
}

bool ComparisonPlayer::Impl::SetComparisonEnabled(const bool enabled) {
    if (shutdown_) {
        return false;
    }
    EndScrub();
    if (comparisonEnabled_ == enabled) {
        return true;
    }

    if (enabled) {
        const PlayerSnapshot primary = primary_->Snapshot();
        if (primary.loading) {
            errorUtf8_ = "主画面仍在加载，请完成后再开启对比画面";
            statusUtf8_ = "暂时无法开启对比画面";
            return false;
        }
        SynchronizeFromSinglePlayer(primary);
        try {
            secondary_ = std::make_unique<PlayerEngine>();
        } catch (...) {
            errorUtf8_ = "无法创建右侧对比解码器";
            return false;
        }

        comparisonEnabled_ = true;
        primary_->SetExternalClockEnabled(true);
        secondary_->SetExternalClockEnabled(true);
        primary_->SetFramesPerSecond(framesPerSecond_);
        secondary_->SetFramesPerSecond(framesPerSecond_);
        secondary_->SetDecodePercent(decodePercent_);
        secondary_->SetBackgroundResourceMode(backgroundResourceMode_);
        primary_->SetLoopPlayback(loopPlayback_);
        secondary_->SetLoopPlayback(loopPlayback_);
        ApplyResourceBudgets();
        effectiveRange_ = comparison_detail::EffectivePlaybackRange(
            rangeIntent_,
            primary.totalFrames);
        primary_->SetPlaybackRange(
            effectiveRange_.startFrame,
            effectiveRange_.endFrame);
        primary_->SetPlaying(playing_);
        requestedFrame_ = primary.requestedFrame;
        playbackFrameAccumulator_ = 0.0;
        pairReady_ = false;
        const std::uint64_t previousRevision = presentedPair_.revision;
        presentedPair_ = {};
        presentedPair_.revision = previousRevision;
        ClearComparisonError();
        statusUtf8_ = "对比画面已开启，请向右侧拖入来源";
        return true;
    }

    const PlayerSnapshot primaryBefore = primary_->Snapshot();
    const bool resumePlaying = pendingPlaybackIntent_.value_or(playing_);
    const FrameIndex preservedFrame = presentedPair_.primaryFrame
        ? presentedPair_.frame
        : primaryBefore.currentFrame;

    primary_->SetPlaying(false);
    std::unique_ptr<PlayerEngine> secondaryToShutdown =
        std::move(secondary_);
    std::optional<std::jthread> secondaryShutdownWorker;
    if (secondaryToShutdown) {
        try {
            secondaryShutdownWorker.emplace(
                [engine = secondaryToShutdown.get()] {
                    engine->Shutdown();
                });
        } catch (const std::system_error&) {
            secondaryToShutdown->Shutdown();
        }
    }
    comparisonEnabled_ = false;
    primaryLoad_ = {};
    secondaryLoad_ = {};
    decodeTransaction_ = {};
    pendingPlaybackIntent_.reset();
    queuedDecodePercent_.reset();
    primaryPlaybackIntentAfterComparisonExit_ = primaryBefore.loading
        ? std::optional<bool>{resumePlaying}
        : std::nullopt;
    pairReady_ = false;
    ApplyResourceBudgets();

    const PlayerSnapshot currentPrimary = primary_->Snapshot();
    commonTotalFrames_ = currentPrimary.totalFrames;
    effectiveRange_ = comparison_detail::EffectivePlaybackRange(
        rangeIntent_,
        commonTotalFrames_);
    primary_->SetPlaybackRange(
        effectiveRange_.startFrame,
        effectiveRange_.endFrame);
    primary_->RequestFrame(
        preservedFrame,
        FrameRequestKind::InteractiveSeek,
        direction_);
    primary_->SetExternalClockEnabled(false);
    primary_->SetFramesPerSecond(framesPerSecond_);
    primary_->SetLoopPlayback(loopPlayback_);
    primary_->SetPlaying(resumePlaying);
    playing_ = resumePlaying;
    requestedFrame_ = std::min<FrameIndex>(
        preservedFrame,
        commonTotalFrames_ > 0U
            ? static_cast<FrameIndex>(commonTotalFrames_ - 1U)
            : 0U);
    playbackFrameAccumulator_ = 0.0;
    ClearComparisonError();
    statusUtf8_ = "已退出对比画面";
    if (secondaryShutdownWorker && secondaryShutdownWorker->joinable()) {
        secondaryShutdownWorker->join();
    }
    secondaryToShutdown.reset();
    return true;
}

bool ComparisonPlayer::Impl::LoadSecondarySource(
    const std::filesystem::path& sourcePath) {
    if (shutdown_) {
        return false;
    }
    EndScrub();
    if (!comparisonEnabled_ && !SetComparisonEnabled(true)) {
        return false;
    }
    if (!secondary_) {
        return false;
    }
    if (DecodeTransactionActive()) {
        errorUtf8_ = "解码比例切换尚未完成，请稍后再加载右侧来源";
        return false;
    }

    const PlayerSnapshot before = secondary_->Snapshot();
    PauseForPendingOperation();
    secondaryLoad_.previousGeneration = before.generation;
    secondaryLoad_.adoptRangeOnSuccess = false;
    secondaryLoad_.resetSharedFrameOnSuccess = true;
    secondaryLoad_.purpose = PendingLoadPurpose::Source;
    const bool accepted = secondary_->LoadSource(sourcePath);
    secondaryLoad_.pending = accepted;
    if (!accepted) {
        secondaryLoad_ = {};
        errorUtf8_ = "右侧来源加载请求未被接受";
        RestorePendingPlaybackAfterRejectedOperation();
    } else {
        statusUtf8_ = "正在加载右侧对比来源";
    }
    return accepted;
}

void ComparisonPlayer::Impl::ApplyResourceBudgets() {
    const std::uint64_t effectiveLimitBytes =
        comparison_detail::EffectiveMemoryLimitBytes(
            memoryLimitBytes_,
            backgroundResourceMode_);
    const comparison_detail::MemorySplit split =
        comparison_detail::CalculateMemorySplit(
            effectiveLimitBytes,
            comparisonEnabled_);
    primary_->SetResourceBudget(EngineResourceBudget{
        memoryLimitBytes_,
        split.primaryCacheBytes});
    if (secondary_) {
        secondary_->SetResourceBudget(EngineResourceBudget{
            memoryLimitBytes_,
            split.secondaryCacheBytes});
    }
}

void ComparisonPlayer::Impl::SetDecodePercent(
    const std::uint32_t percent) {
    if (shutdown_ || !detail::IsSupportedDecodePercent(percent)) {
        return;
    }
    EndScrub();
    if (!comparisonEnabled_) {
        decodePercent_ = percent;
        primary_->SetDecodePercent(percent);
        return;
    }

    const PlayerSnapshot primary = primary_->Snapshot();
    const PlayerSnapshot secondary = secondary_
        ? secondary_->Snapshot()
        : PlayerSnapshot{};
    if (HasTrackedPendingOperation() || primary.loading || secondary.loading) {
        queuedDecodePercent_ = percent;
        statusUtf8_ = "等待当前加载完成后切换解码比例";
        return;
    }
    StartDecodePercentTransaction(percent, primary, secondary);
}

void ComparisonPlayer::Impl::SetMemoryLimitBytes(const std::uint64_t bytes) {
    if (shutdown_ || bytes < detail::kMinimumAcceptedMemoryLimitBytes) {
        return;
    }
    memoryLimitBytes_ = bytes;
    ApplyResourceBudgets();
}

void ComparisonPlayer::Impl::SetBackgroundResourceMode(const bool enabled) {
    if (shutdown_ || backgroundResourceMode_ == enabled) {
        return;
    }

    if (enabled) {
        EndScrub();
        SetPlaying(false);
        backgroundResourceMode_ = true;
        primary_->SetBackgroundResourceMode(true);
        if (secondary_) {
            secondary_->SetBackgroundResourceMode(true);
        }
        ApplyResourceBudgets();
        return;
    }

    backgroundResourceMode_ = false;
    ApplyResourceBudgets();
    primary_->SetBackgroundResourceMode(false);
    if (secondary_) {
        secondary_->SetBackgroundResourceMode(false);
    }
}

void ComparisonPlayer::Impl::Shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    playing_ = false;
    scrubbing_ = false;
    std::unique_ptr<PlayerEngine> secondaryToShutdown =
        std::move(secondary_);
    std::optional<std::jthread> secondaryShutdownWorker;
    if (secondaryToShutdown) {
        try {
            secondaryShutdownWorker.emplace(
                [engine = secondaryToShutdown.get()] {
                    engine->Shutdown();
                });
        } catch (const std::system_error&) {
            secondaryToShutdown->Shutdown();
        }
    }
    if (primary_) {
        primary_->Shutdown();
    }
    if (secondaryShutdownWorker && secondaryShutdownWorker->joinable()) {
        secondaryShutdownWorker->join();
    }
    secondaryToShutdown.reset();
    presentedPair_.primaryFrame.reset();
    presentedPair_.secondaryFrame.reset();
    pairReady_ = false;
    statusUtf8_ = "播放器已停止";
}

}  // namespace zt::sequence
