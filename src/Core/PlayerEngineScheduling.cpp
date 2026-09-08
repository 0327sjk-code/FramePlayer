#include "Core/PlayerEngineInternal.h"

#include "Core/PlayerEnginePolicy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zt::sequence::detail {

ProcessMemoryUsage QueryProcessMemoryUsage() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return {};
    }

    ProcessMemoryUsage usage;
    usage.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    usage.privateBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
    return usage;
}

}  // namespace zt::sequence::detail

namespace zt::sequence {

void PlayerEngine::Impl::ScheduleScrubTarget() {
    std::scoped_lock lock(mutex_);
    ScheduleScrubTargetLocked();
}

void PlayerEngine::Impl::ScheduleScrubTargetLocked() {
    if (shutdown_ || !scrubbing_ || !activeSession_ ||
        activeSession_->generation != scrubGeneration_ ||
        activeSession_->TotalFrames() == 0U) {
        return;
    }

    const std::shared_ptr<const SourceSession> session = activeSession_;
    const FrameIndex index = requestedFrame_;
    const bool targetResolved = index < session->TotalFrames()
        && (failedFrames_.contains(index)
            || cache_.Contains(session->generation, index)
            || (displayFrame_
                && displayFrame_->generation == session->generation
                && displayFrame_->index == index));
    if (index >= session->TotalFrames() || targetResolved) {
        if (targetResolved && session->kind == SourceKind::PngSequence) {
            scheduler_.ClearLatestTargetPreservingBackground(
                session->generation);
        }
        return;
    }

    DecodeTask current;
    if (session->kind == SourceKind::Video) {
        current.file.path = session->sourcePath;
        current.file.relativePath = session->sourcePath.filename().wstring();
    } else {
        current.file = session->frames[index];
    }
    current.index = index;
    current.generation = session->generation;
    current.decodePercent = session->decodePercent;
    current.priority = DecodePriority::Current;
    current.sortRank = 0U;

    // Keep target capture and scheduler submission in one controller critical
    // section. A completion callback may request another scheduling pass, but
    // it never holds the scheduler lock while acquiring this mutex, so the
    // established controller -> scheduler lock order remains acyclic.
    if (session->kind == SourceKind::Video) {
        (void)videoScheduler_.SubmitLatest(std::move(current));
    } else {
        (void)scheduler_.SubmitLatest(std::move(current));
    }
}

bool PlayerEngine::Impl::IsPostScrubHotFillSatisfiedLocked() const {
    if (!postScrubHotFillActive_ || !activeSession_
        || activeSession_->kind != SourceKind::PngSequence
        || activeSession_->generation != postScrubHotFillGeneration_
        || activeSession_->TotalFrames() == 0U) {
        return true;
    }

    const std::uint64_t rangeFrameCount =
        detail::PlaybackRangeFrameCount(playbackRange_);
    if (rangeFrameCount == 0U
        || !detail::PlaybackRangeContains(
            playbackRange_,
            postScrubHotFillStartFrame_)) {
        return true;
    }

    const std::size_t desiredFrames = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::ceil(
            settings_.framesPerSecond * detail::kPostScrubHotFillSeconds)) + 1U);
    const std::size_t availableFrames = settings_.loopPlayback
        ? static_cast<std::size_t>(rangeFrameCount)
        : static_cast<std::size_t>(playbackRange_.endFrame)
            - static_cast<std::size_t>(postScrubHotFillStartFrame_) + 1U;
    const std::uint64_t frameBytes = displayFrame_
        ? static_cast<std::uint64_t>(displayFrame_->ByteSize())
        : 0U;
    if (!detail::CanRetainDecodedFrame(cache_.CapacityBytes(), frameBytes)) {
        return true;
    }
    const std::size_t capacityFrames = frameBytes > 0U
        ? static_cast<std::size_t>(cache_.CapacityBytes() / frameBytes)
        : desiredFrames;
    const std::size_t requiredFrames = std::min({
        desiredFrames,
        availableFrames,
        capacityFrames});
    if (requiredFrames == 0U) {
        return true;
    }

    const std::size_t readyFrames = cache_.CountContiguousInRange(
        activeSession_->generation,
        postScrubHotFillStartFrame_,
        1,
        playbackRange_,
        settings_.loopPlayback,
        requiredFrames);
    if (readyFrames >= requiredFrames) {
        return true;
    }

    // Decode failures must not suspend the cold queue forever. This bounded
    // fallback runs only when the normal contiguous cache query encountered a
    // gap and the session has known failed frames.
    if (failedFrames_.empty()) {
        return false;
    }
    for (std::size_t distance = 0U; distance < requiredFrames; ++distance) {
        const std::optional<FrameIndex> index =
            detail::OffsetFrameInPlaybackRange(
                postScrubHotFillStartFrame_,
                static_cast<std::int64_t>(distance),
                playbackRange_,
                settings_.loopPlayback);
        if (!index || (!failedFrames_.contains(*index)
            && !cache_.Contains(activeSession_->generation, *index))) {
            return false;
        }
    }
    return true;
}

void PlayerEngine::Impl::ScheduleWork() {
    bool scrubActive = false;
    {
        std::scoped_lock lock(mutex_);
        if (shutdown_) {
            return;
        }
        scrubActive = scrubbing_;
    }
    if (scrubActive) {
        // The scrub planner submits only the current target. It owns the
        // controller lock across target capture and scheduler submission so
        // an older completion pass cannot overtake a newer UI request.
        ScheduleScrubTarget();
        return;
    }

    const detail::ProcessMemoryUsage processMemory = detail::QueryProcessMemoryUsage();
    const std::size_t pendingTaskCount = scheduler_.PendingTaskCount() +
        videoScheduler_.PendingTaskCount();
    const std::size_t pendingBackgroundTaskCount =
        scheduler_.PendingTaskCount(DecodePriority::Background) +
        videoScheduler_.PendingTaskCount(DecodePriority::Background);
    std::vector<DecodeTask> tasks;
    SourceKind sourceKind = SourceKind::None;

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || scrubbing_ || !activeSession_ ||
            activeSession_->TotalFrames() == 0U) {
            return;
        }

        const std::shared_ptr<const SourceSession> session = activeSession_;
        sourceKind = session->kind;
        const std::size_t totalFrames = session->TotalFrames();
        bool postScrubHotFill = postScrubHotFillActive_
            && postScrubHotFillGeneration_ == session->generation
            && session->kind == SourceKind::PngSequence;
        if (postScrubHotFill) {
            if (IsPostScrubHotFillSatisfiedLocked()) {
                scheduler_.ResumeBackground(session->generation);
                postScrubHotFillActive_ = false;
                postScrubHotFillGeneration_ = 0U;
                postScrubHotFill = false;
            }
        }
        const PlaybackRange hotRange = playing_
            ? ActiveTransportRangeLocked()
            : (postScrubHotFill
                ? playbackRange_
                : detail::FullPlaybackRange(totalFrames));
        const bool hotRangeLoops = playing_
            ? ActiveTransportLoopLocked()
            : settings_.loopPlayback;
        const FrameIndex hotStartFrame = postScrubHotFill
            ? postScrubHotFillStartFrame_
            : requestedFrame_;
        const std::size_t hotRangeFrames = static_cast<std::size_t>(
            detail::PlaybackRangeFrameCount(hotRange));
        std::unordered_set<FrameIndex> planned;
        planned.reserve(320);

        const auto addTask = [this, &tasks, &planned, &session](
                                 const FrameIndex index,
                                 const DecodePriority priority,
                                 const std::uint64_t sortRank) {
            if (index >= session->TotalFrames()
                || failedFrames_.contains(index)
                || cache_.Contains(session->generation, index)
                || !planned.insert(index).second) {
                return;
            }
            if (priority == DecodePriority::Current
                && displayFrame_
                && displayFrame_->generation == session->generation
                && displayFrame_->index == index) {
                return;
            }

            DecodeTask task;
            if (session->kind == SourceKind::Video) {
                task.file.path = session->sourcePath;
                task.file.relativePath =
                    session->sourcePath.filename().wstring();
            } else {
                task.file = session->frames[index];
            }
            task.index = index;
            task.generation = session->generation;
            task.decodePercent = session->decodePercent;
            task.priority = priority;
            task.sortRank = sortRank;
            tasks.emplace_back(std::move(task));
        };

        addTask(requestedFrame_, DecodePriority::Current, 0);
        if (!playing_) {
            const FrameIndex playbackEntry = detail::PlaybackStartForPlay(
                requestedFrame_,
                playbackRange_);
            addTask(playbackEntry, DecodePriority::Forward, 0);
            const std::optional<FrameIndex> nextPlaybackFrame =
                detail::OffsetFrameInPlaybackRange(
                    playbackEntry,
                    1,
                    playbackRange_,
                    settings_.loopPlayback);
            if (nextPlaybackFrame) {
                addTask(*nextPlaybackFrame, DecodePriority::Forward, 1);
            }
        }

        const std::uint64_t frameBytes = displayFrame_
            ? static_cast<std::uint64_t>(displayFrame_->ByteSize())
            : 0;
        const std::size_t estimatedCapacityFrames = frameBytes > 0
            ? static_cast<std::size_t>(cache_.CapacityBytes() / frameBytes)
            : 0;
        const std::size_t timeBasedAhead = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(
                std::ceil(settings_.framesPerSecond * (
                    postScrubHotFill
                        ? detail::kPostScrubHotFillSeconds
                        : detail::kForwardPrefetchSeconds))));
        const std::size_t hotWindowCapacity = estimatedCapacityFrames > 0
            ? estimatedCapacityFrames - 1
            : 0;
        const std::size_t capacityBasedAhead = postScrubHotFill
            ? hotWindowCapacity
            : estimatedCapacityFrames > 0
                ? hotWindowCapacity > 0
                ? std::max<std::size_t>(
                    1,
                    detail::PercentageOfSize(
                        hotWindowCapacity,
                        detail::kHotWindowPercent))
                : 0
            : timeBasedAhead;
        const std::size_t aheadFrames = std::min({
            timeBasedAhead,
            capacityBasedAhead,
            hotRangeFrames > 0 ? hotRangeFrames - 1 : 0});
        const int prefetchDirection = postScrubHotFill
            ? 1
            : session->kind == SourceKind::Video
            ? 1
            : direction_;

        for (std::size_t distance = 1; distance <= aheadFrames; ++distance) {
            const std::optional<FrameIndex> index =
                detail::OffsetFrameInPlaybackRange(
                hotStartFrame,
                prefetchDirection < 0
                    ? -static_cast<std::int64_t>(distance)
                    : static_cast<std::int64_t>(distance),
                hotRange,
                hotRangeLoops);
            if (!index || *index == hotStartFrame) {
                break;
            }
            addTask(*index, DecodePriority::Forward, distance);
        }

        const std::size_t timeBasedNeighborhood = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(
                std::ceil(settings_.framesPerSecond * detail::kNeighborhoodSeconds)));
        const std::size_t capacityBasedNeighborhood = estimatedCapacityFrames > 0
            ? detail::PercentageOfSize(
                hotWindowCapacity,
                detail::kBackwardWindowPercent)
            : timeBasedNeighborhood;
        const std::size_t neighborhoodFrames =
            session->kind == SourceKind::Video || postScrubHotFill
            ? 0U
            : std::min({
            hotRangeFrames > 0 ? hotRangeFrames - 1 : 0,
            timeBasedNeighborhood,
            capacityBasedNeighborhood});
        for (std::size_t distance = 1; distance <= neighborhoodFrames; ++distance) {
            const std::optional<FrameIndex> index =
                detail::OffsetFrameInPlaybackRange(
                requestedFrame_,
                direction_ < 0
                    ? static_cast<std::int64_t>(distance)
                    : -static_cast<std::int64_t>(distance),
                hotRange,
                hotRangeLoops);
            if (!index || *index == requestedFrame_) {
                break;
            }
            addTask(*index, DecodePriority::Neighborhood, distance);
        }

        const std::uint64_t cacheCapacity = cache_.CapacityBytes();
        const std::uint64_t cacheBytes = cache_.SizeBytes();
        const bool sequenceFits = frameBytes > 0
            && detail::MultiplicationFits(frameBytes, totalFrames, cacheCapacity);
        const bool belowBackgroundFillTarget = sequenceFits
            ? cache_.Count(session->generation) < totalFrames
            : cacheCapacity > 0
                && cacheBytes <= (cacheCapacity / 100ULL)
                    * detail::kBackgroundFillPercentWhenSequenceExceedsRam;
        const bool processMemoryAllowsBackground = detail::AllowsBackgroundDecode(
            processMemory.privateBytes,
            settings_.memoryLimitBytes);
        if (!backgroundResourceMode_
            && !postScrubHotFill
            && pendingTaskCount < detail::kMaximumPendingDecodeTasks
            && pendingBackgroundTaskCount < detail::kMaximumPendingBackgroundTasks
            && belowBackgroundFillTarget
            && processMemoryAllowsBackground
            && cache_.Count(session->generation) < totalFrames) {
            std::size_t inspected = 0;
            std::size_t added = 0;
            while (inspected < totalFrames && added < detail::kBackgroundBatchFrames) {
                const FrameIndex index = static_cast<FrameIndex>(backgroundCursor_ % totalFrames);
                backgroundCursor_ = (backgroundCursor_ + 1) % totalFrames;
                ++inspected;
                const std::size_t before = tasks.size();
                addTask(index, DecodePriority::Background, backgroundSequenceRank_++);
                if (tasks.size() != before) {
                    ++added;
                }
            }
        }
    }

    {
        std::scoped_lock lock(mutex_);
        if (shutdown_ || scrubbing_ || !activeSession_ ||
            activeSession_->kind != sourceKind) {
            return;
        }
        for (DecodeTask& task : tasks) {
            if (task.generation != activeSession_->generation) {
                continue;
            }
            if (backgroundResourceMode_ &&
                task.priority == DecodePriority::Background) {
                continue;
            }
            if (sourceKind == SourceKind::Video) {
                (void)videoScheduler_.Submit(std::move(task));
            } else {
                (void)scheduler_.Submit(std::move(task));
            }
        }
    }
}

}  // namespace zt::sequence
