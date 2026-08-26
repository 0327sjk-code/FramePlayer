#include "Core/PlayerEngineInternal.h"

#include "Core/PlayerEnginePolicy.h"
#include "Core/SequenceScanner.h"
#include "Imaging/MediaFoundationVideoDecoder.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace zt::sequence {
namespace {

[[nodiscard]] bool RefersToSameFolder(
    const std::filesystem::path& first,
    const std::filesystem::path& second) noexcept {
    if (first.empty() || second.empty()) {
        return false;
    }

    std::error_code error;
    if (std::filesystem::equivalent(first, second, error)) {
        return true;
    }

    std::filesystem::path normalizedFirst = first.lexically_normal();
    std::filesystem::path normalizedSecond = second.lexically_normal();
    normalizedFirst.make_preferred();
    normalizedSecond.make_preferred();
    return ::_wcsicmp(
        normalizedFirst.c_str(),
        normalizedSecond.c_str()) == 0;
}

[[nodiscard]] std::wstring LowercaseExtension(
    const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return extension;
}

}  // namespace

PlayerEngine::Impl::Impl()
    : cache_(detail::CacheCapacityForTotalLimit(settings_.memoryLimitBytes)),
      scheduler_(
          detail::RecommendedDecodeWorkerCount(),
          [this](ScheduledDecodeResult result) { OnDecodeCompleted(std::move(result)); }),
      videoScheduler_(
          1U,
          [this](ScheduledDecodeResult result) { OnDecodeCompleted(std::move(result)); },
          [this](const DecodeTask& task,
             std::string& errorUtf8,
             const std::atomic_bool& cancelled) {
              thread_local MediaFoundationVideoDecoder decoder;
              activeVideoDecoder_.store(&decoder, std::memory_order_release);
              if (task.file.path.empty()) {
                  decoder.PrepareForProcessShutdown();
                  {
                      std::scoped_lock lock(videoDecoderControlMutex_);
                      videoDecoderCloseCompleted_ = true;
                  }
                  videoDecoderControlCondition_.notify_all();
                  return std::shared_ptr<DecodedFrame>{};
              }
              VideoDecodeResult result = decoder.Decode(
                  task.file.path,
                  task.index,
                  task.generation,
                  task.decodePercent,
                  &cancelled);
              errorUtf8 = std::move(result.errorUtf8);
              return std::move(result.frame);
           }) {
    cacheCapacityBytes_ = cache_.CapacityBytes();
    SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
}

PlayerEngine::Impl::~Impl() {
    Shutdown();
}

void PlayerEngine::Impl::CancelGeneration(const Generation generation) {
    scheduler_.CancelGeneration(generation);
    videoScheduler_.CancelGeneration(generation);
}

void PlayerEngine::Impl::CancelAllExcept(const Generation generation) {
    scheduler_.CancelAllExcept(generation);
    videoScheduler_.CancelAllExcept(generation);
}

void PlayerEngine::Impl::CancelInteractive(const Generation generation) {
    scheduler_.CancelInteractive(generation);
    videoScheduler_.CancelInteractive(generation);
}

void PlayerEngine::Impl::CancelBackground(const Generation generation) {
    scheduler_.CancelBackground(generation);
    videoScheduler_.CancelBackground(generation);
}

void PlayerEngine::Impl::CancelForImmediateTarget(
    const Generation generation) {
    scheduler_.CancelInteractive(generation);
    videoScheduler_.CancelGenerationForReplacement(generation);
}

void PlayerEngine::Impl::SetBackgroundConcurrency(
    const std::size_t maximumConcurrentTasks) {
    scheduler_.SetBackgroundConcurrency(maximumConcurrentTasks);
    videoScheduler_.SetBackgroundConcurrency(1U);
}

bool PlayerEngine::Impl::LoadFolder(const std::filesystem::path& folder) {
    EndScrub();
    std::filesystem::path currentFolder;
    std::optional<PreservedPlaybackRange> preservedPlaybackRange;
    {
        std::scoped_lock lock(mutex_);
        if (activeSession_ &&
            activeSession_->kind == SourceKind::PngSequence) {
            currentFolder = activeSession_->sourcePath;
            preservedPlaybackRange = PreservedPlaybackRange{
                playbackRange_,
                playbackRangeCustomized_,
                activeSession_->generation};
        }
    }
    if (!RefersToSameFolder(currentFolder, folder)) {
        preservedPlaybackRange.reset();
    }
    return BeginLoad(
        folder,
        std::nullopt,
        std::move(preservedPlaybackRange));
}

bool PlayerEngine::Impl::LoadSource(
    const std::filesystem::path& sourcePath) {
    EndScrub();
    if (sourcePath.empty()) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = "来源路径为空";
        statusUtf8_ = "来源加载失败，当前内容保持不变";
        return false;
    }

    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::status(sourcePath, error);
    if (error) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = "来源不存在或无法访问";
        statusUtf8_ = "来源加载失败，当前内容保持不变";
        return false;
    }
    if (std::filesystem::is_directory(status)) {
        return LoadFolder(sourcePath);
    }
    if (!std::filesystem::is_regular_file(status)) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = "只支持 PNG 序列文件夹、PNG 文件或视频文件";
        statusUtf8_ = "来源加载失败，当前内容保持不变";
        return false;
    }
    if (LowercaseExtension(sourcePath) == L".png") {
        return LoadFolder(sourcePath.parent_path());
    }

    std::optional<PreservedPlaybackRange> preservedPlaybackRange;
    FrameIndex preferredFrame = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (activeSession_ && activeSession_->kind == SourceKind::Video &&
            RefersToSameFolder(activeSession_->sourcePath, sourcePath)) {
            preferredFrame = currentFrame_;
            preservedPlaybackRange = PreservedPlaybackRange{
                playbackRange_,
                playbackRangeCustomized_,
                activeSession_->generation};
        }
    }
    return BeginVideoLoad(
        sourcePath,
        preferredFrame,
        std::move(preservedPlaybackRange));
}

bool PlayerEngine::Impl::ReloadFolder() {
    EndScrub();
    std::filesystem::path folder;
    std::optional<std::wstring> preferredFile;
    std::optional<PreservedPlaybackRange> preservedPlaybackRange;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return false;
        }
        if (activeSession_ &&
            activeSession_->kind == SourceKind::PngSequence) {
            folder = activeSession_->sourcePath;
            preservedPlaybackRange = PreservedPlaybackRange{
                playbackRange_,
                playbackRangeCustomized_,
                activeSession_->generation};
            if (currentFrame_ < activeSession_->frames.size()) {
                preferredFile = activeSession_->frames[currentFrame_].relativePath;
            }
        } else if (pendingLoad_ &&
            pendingLoad_->session->kind == SourceKind::PngSequence) {
            folder = pendingLoad_->session->sourcePath;
            preservedPlaybackRange =
                pendingLoad_->preservedPlaybackRange;
        } else {
            errorUtf8_ = activeSession_ && activeSession_->kind == SourceKind::Video
                ? "视频来源不支持重扫"
                : "当前没有可重新扫描的文件夹";
            return false;
        }
    }
    return BeginLoad(
        folder,
        std::move(preferredFile),
        std::move(preservedPlaybackRange));
}

std::optional<ExportSourceSnapshot>
PlayerEngine::Impl::CaptureExportSnapshot() const {
    std::shared_ptr<const SourceSession> session;
    PlaybackRange inclusiveRange;
    double framesPerSecond = kDefaultFramesPerSecond;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || pendingLoad_ || !activeSession_ ||
            activeSession_->kind == SourceKind::None ||
            activeSession_->TotalFrames() == 0U ||
            (activeSession_->kind == SourceKind::PngSequence &&
             activeSession_->frames.empty())) {
            return std::nullopt;
        }

        session = activeSession_;
        inclusiveRange = detail::NormalizePlaybackRange(
            playbackRange_,
            session->TotalFrames());
        framesPerSecond = settings_.framesPerSecond;
    }

    try {
        if (session->kind == SourceKind::PngSequence) {
            SequenceExportSnapshot snapshot;
            snapshot.sourceGeneration = session->generation;
            snapshot.sourceFolder = session->sourcePath;
            snapshot.orderedPngFrames = session->frames;
            snapshot.inclusiveRange = inclusiveRange;
            snapshot.framesPerSecond = framesPerSecond;
            return ExportSourceSnapshot{std::move(snapshot)};
        }
        if (session->kind == SourceKind::Video) {
            VideoExportSnapshot snapshot;
            snapshot.sourceGeneration = session->generation;
            snapshot.sourceFile = session->sourcePath;
            snapshot.inclusiveRange = inclusiveRange;
            snapshot.sourceFramesPerSecond =
                session->videoMetadata.framesPerSecond;
            snapshot.framesPerSecond = framesPerSecond;
            snapshot.totalFrames = session->videoMetadata.frameCount;
            snapshot.sourceWidth = session->videoMetadata.width;
            snapshot.sourceHeight = session->videoMetadata.height;
            return ExportSourceSnapshot{std::move(snapshot)};
        }
        return std::nullopt;
    } catch (...) {
        // Copying thousands of filesystem paths can fail under severe memory
        // pressure. Treat that as an unavailable snapshot and leave engine
        // state untouched.
        return std::nullopt;
    }
}

void PlayerEngine::Impl::Shutdown() {
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        playing_ = false;
        scrubbing_ = false;
        scrubGeneration_ = 0U;
        postScrubHotFillActive_ = false;
        postScrubHotFillGeneration_ = 0U;
        pendingLoad_.reset();
        activeSession_.reset();
    }
    scheduler_.Shutdown();
    videoScheduler_.CancelAllExcept(0U);
    if (activeVideoDecoder_.load(std::memory_order_acquire) != nullptr) {
        {
            std::scoped_lock lock(videoDecoderControlMutex_);
            videoDecoderCloseCompleted_ = false;
        }
        DecodeTask closeTask;
        closeTask.generation = 0U;
        closeTask.decodePercent = kDefaultDecodePercent;
        closeTask.priority = DecodePriority::Current;
        closeTask.sortRank = 0U;
        if (videoScheduler_.Submit(std::move(closeTask))) {
            std::unique_lock lock(videoDecoderControlMutex_);
            static_cast<void>(videoDecoderControlCondition_.wait_for(
                lock,
                std::chrono::seconds(5),
                [this] { return videoDecoderCloseCompleted_; }));
        }
    }
    videoScheduler_.Shutdown();
    activeVideoDecoder_.store(nullptr, std::memory_order_release);
    cache_.Clear();
    {
        std::scoped_lock lock(mutex_);
        displayFrame_.reset();
        buffering_ = false;
        statusUtf8_ = "播放器已停止";
    }
}

bool PlayerEngine::Impl::BeginLoad(
    const std::filesystem::path& requestedFolder,
    std::optional<std::wstring> preferredFile,
    std::optional<PreservedPlaybackRange> preservedPlaybackRange) {
    SequenceScanResult scan = ScanPngFolder(requestedFolder);
    if (!scan) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = std::move(scan.errorUtf8);
        statusUtf8_ = "序列加载失败，当前内容保持不变";
        return false;
    }
    if (scan.frames.size() > std::numeric_limits<FrameIndex>::max()) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = "PNG 数量超过播放器索引上限";
        statusUtf8_ = "序列加载失败，当前内容保持不变";
        return false;
    }

    FrameIndex initialFrame = 0;
    if (preferredFile) {
        const auto found = std::find_if(
            scan.frames.begin(),
            scan.frames.end(),
            [&preferredFile](const FrameFile& frame) {
                return frame.relativePath == *preferredFile;
            });
        if (found != scan.frames.end()) {
            initialFrame = static_cast<FrameIndex>(
                std::distance(scan.frames.begin(), found));
        }
    }

    auto session = std::make_shared<SourceSession>();
    Generation previousPendingGeneration = 0;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return false;
        }
        session->generation = nextGeneration_++;
        session->kind = SourceKind::PngSequence;
        session->sourcePath = requestedFolder.lexically_normal();
        session->frames = std::move(scan.frames);
        session->decodePercent = settings_.decodePercent;
        if (pendingLoad_) {
            previousPendingGeneration = pendingLoad_->session->generation;
        }
        const PlaybackRange nextPlaybackRange =
            detail::PlaybackRangeForLoadedSequence(
                preservedPlaybackRange
                    ? preservedPlaybackRange->range
                    : PlaybackRange{},
                session->frames.size(),
                preservedPlaybackRange.has_value(),
                preservedPlaybackRange
                    ? preservedPlaybackRange->customized
                    : false);
        pendingLoad_ = PendingLoad{
            session,
            initialFrame,
            nextPlaybackRange,
            preservedPlaybackRange
                ? preservedPlaybackRange->customized
                : false,
            std::move(preservedPlaybackRange)};
        errorUtf8_.clear();
        statusUtf8_ = "正在解码候选序列首帧";
    }

    if (previousPendingGeneration != 0) {
        CancelGeneration(previousPendingGeneration);
    }

    DecodeTask firstFrameTask;
    firstFrameTask.file = session->frames[initialFrame];
    firstFrameTask.index = initialFrame;
    firstFrameTask.generation = session->generation;
    firstFrameTask.decodePercent = session->decodePercent;
    firstFrameTask.priority = DecodePriority::Current;
    firstFrameTask.sortRank = 0;
    if (!scheduler_.Submit(std::move(firstFrameTask))) {
        std::scoped_lock lock(mutex_);
        if (pendingLoad_ && pendingLoad_->session->generation == session->generation) {
            pendingLoad_.reset();
            errorUtf8_ = "首帧解码任务无法提交";
            statusUtf8_ = "序列加载失败，当前内容保持不变";
        }
        return false;
    }
    return true;
}

bool PlayerEngine::Impl::BeginVideoLoad(
    const std::filesystem::path& videoFile,
    const FrameIndex preferredFrame,
    std::optional<PreservedPlaybackRange> preservedPlaybackRange) {
    VideoProbeResult probe = ProbeVideoFile(videoFile);
    if (!probe) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = probe.errorUtf8.empty()
            ? "无法读取视频信息或当前编码不受系统支持"
            : std::move(probe.errorUtf8);
        statusUtf8_ = "视频加载失败，当前内容保持不变";
        return false;
    }
    if (probe.metadata.frameCount >
        static_cast<std::size_t>(std::numeric_limits<FrameIndex>::max())) {
        std::scoped_lock lock(mutex_);
        errorUtf8_ = "视频帧数超过播放器索引上限";
        statusUtf8_ = "视频加载失败，当前内容保持不变";
        return false;
    }

    auto session = std::make_shared<SourceSession>();
    Generation previousPendingGeneration = 0U;
    FrameIndex initialFrame = 0U;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return false;
        }
        session->generation = nextGeneration_++;
        session->kind = SourceKind::Video;
        session->sourcePath = videoFile.lexically_normal();
        session->videoMetadata = probe.metadata;
        session->decodePercent = settings_.decodePercent;
        initialFrame = std::min<FrameIndex>(
            preferredFrame,
            static_cast<FrameIndex>(session->TotalFrames() - 1U));
        if (pendingLoad_) {
            previousPendingGeneration = pendingLoad_->session->generation;
        }
        const PlaybackRange nextPlaybackRange =
            detail::PlaybackRangeForLoadedSequence(
                preservedPlaybackRange
                    ? preservedPlaybackRange->range
                    : PlaybackRange{},
                session->TotalFrames(),
                preservedPlaybackRange.has_value(),
                preservedPlaybackRange
                    ? preservedPlaybackRange->customized
                    : false);
        pendingLoad_ = PendingLoad{
            session,
            initialFrame,
            nextPlaybackRange,
            preservedPlaybackRange
                ? preservedPlaybackRange->customized
                : false,
            std::move(preservedPlaybackRange)};
        errorUtf8_.clear();
        statusUtf8_ = "正在解码候选视频首帧";
    }

    if (previousPendingGeneration != 0U) {
        CancelGeneration(previousPendingGeneration);
    }

    DecodeTask firstFrameTask;
    firstFrameTask.file.path = session->sourcePath;
    firstFrameTask.file.relativePath = session->sourcePath.filename().wstring();
    firstFrameTask.index = initialFrame;
    firstFrameTask.generation = session->generation;
    firstFrameTask.decodePercent = session->decodePercent;
    firstFrameTask.priority = DecodePriority::Current;
    firstFrameTask.sortRank = 0U;
    if (!videoScheduler_.Submit(std::move(firstFrameTask))) {
        std::scoped_lock lock(mutex_);
        if (pendingLoad_ &&
            pendingLoad_->session->generation == session->generation) {
            pendingLoad_.reset();
            errorUtf8_ = "视频首帧解码任务无法提交";
            statusUtf8_ = "视频加载失败，当前内容保持不变";
        }
        return false;
    }
    return true;
}

void PlayerEngine::Impl::OnDecodeCompleted(ScheduledDecodeResult result) {
    bool shouldSchedule = false;
    bool continueScrub = false;
    Generation committedGeneration = 0;

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }

        if (pendingLoad_
            && result.task.generation == pendingLoad_->session->generation
            && result.task.index == pendingLoad_->initialFrame) {
            if (!result) {
                const bool videoLoad =
                    pendingLoad_->session->kind == SourceKind::Video;
                errorUtf8_ = result.errorUtf8.empty()
                    ? (videoLoad
                        ? "候选视频首帧解码失败"
                        : "候选序列首帧解码失败")
                    : std::move(result.errorUtf8);
                statusUtf8_ = videoLoad
                    ? "视频加载失败，当前内容保持不变"
                    : "序列加载失败，当前内容保持不变";
                pendingLoad_.reset();
                return;
            }

            activeSession_ = pendingLoad_->session;
            committedGeneration = activeSession_->generation;
            if (activeSession_->kind == SourceKind::Video &&
                !externalClockEnabled_) {
                settings_.framesPerSecond = std::clamp(
                    activeSession_->videoMetadata.framesPerSecond,
                    detail::kMinimumFramesPerSecond,
                    detail::kMaximumFramesPerSecond);
            } else {
                settings_.framesPerSecond = sequenceFramesPerSecond_;
            }
            currentFrame_ = pendingLoad_->initialFrame;
            requestedFrame_ = pendingLoad_->initialFrame;
            playbackRange_ = pendingLoad_->playbackRange;
            playbackRangeCustomized_ =
                pendingLoad_->playbackRangeCustomized;
            pendingLoad_.reset();
            playing_ = false;
            scrubbing_ = false;
            scrubGeneration_ = 0U;
            postScrubHotFillActive_ = false;
            postScrubHotFillGeneration_ = 0U;
            buffering_ = false;
            playbackFrameAccumulator_ = 0.0;
            direction_ = 1;
            failedFrames_.clear();
            backgroundCursor_ = 0;
            backgroundSequenceRank_ = 0;
            cache_.Clear();
            cache_.SetCapacityBytes(cacheCapacityBytes_);
            (void)cache_.Put(result.frame);
            displayFrame_ = std::move(result.frame);
            sourceWidth_ = displayFrame_->sourceWidth;
            sourceHeight_ = displayFrame_->sourceHeight;
            decodedWidth_ = displayFrame_->width;
            decodedHeight_ = displayFrame_->height;
            ++displayRevision_;
            ++presentedFramesSinceSample_;
            errorUtf8_.clear();
            statusUtf8_ = activeSession_->kind == SourceKind::Video
                ? "视频已加载"
                : "序列已加载";
            // Keep cancellation in the same controller critical section as the
            // commit so a newer pending generation cannot be cancelled here.
            CancelAllExcept(committedGeneration);
            shouldSchedule = true;
        } else if (activeSession_
            && result.task.generation == activeSession_->generation
            && result.task.decodePercent == activeSession_->decodePercent) {
            if (backgroundResourceMode_ &&
                result.task.priority == DecodePriority::Background) {
                shouldSchedule = true;
            } else if (!result) {
                failedFrames_.insert(result.task.index);
                if (result.task.index == requestedFrame_) {
                    buffering_ = false;
                    errorUtf8_ = result.errorUtf8.empty()
                        ? "目标帧解码失败"
                        : std::move(result.errorUtf8);
                    statusUtf8_ = "目标帧不可用";
                }
                shouldSchedule = true;
            } else {
                (void)cache_.Put(result.frame);
                const bool requestedFrameCompleted =
                    result.task.index == requestedFrame_;
                const bool intermediateScrubFrameCompleted =
                    result.task.priority == DecodePriority::Current &&
                    displayFrame_ &&
                    !failedFrames_.contains(requestedFrame_) &&
                    detail::ShouldPresentScrubIntermediateFrame(
                        scrubbing_,
                        externalClockEnabled_,
                        activeSession_->kind,
                        currentFrame_,
                        requestedFrame_,
                        result.task.index);
                if (requestedFrameCompleted ||
                    intermediateScrubFrameCompleted) {
                    displayFrame_ = std::move(result.frame);
                    currentFrame_ = result.task.index;
                    sourceWidth_ = displayFrame_->sourceWidth;
                    sourceHeight_ = displayFrame_->sourceHeight;
                    decodedWidth_ = displayFrame_->width;
                    decodedHeight_ = displayFrame_->height;
                    if (requestedFrameCompleted) {
                        buffering_ = false;
                    } else {
                        buffering_ = true;
                    }
                    ++displayRevision_;
                    ++presentedFramesSinceSample_;
                }
                shouldSchedule = true;
            }
        }
        continueScrub = shouldSchedule && scrubbing_ && activeSession_ &&
            activeSession_->generation == scrubGeneration_;
    }

    if (shouldSchedule) {
        if (committedGeneration != 0) {
            SetBackgroundConcurrency(detail::kPausedBackgroundConcurrency);
        }
        if (continueScrub) {
            ScheduleScrubTarget();
        } else {
            ScheduleWork();
        }
    }
}

}  // namespace zt::sequence
