#include "Core/DecodeScheduler.h"

#include "Imaging/WicImageDecoder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zt::sequence {
namespace {

struct TaskKey {
    Generation generation = 0;
    FrameIndex index = 0;
    std::uint32_t decodePercent = 0;

    [[nodiscard]] bool operator==(const TaskKey&) const noexcept = default;
};

struct TaskKeyHash {
    [[nodiscard]] std::size_t operator()(const TaskKey& key) const noexcept {
        std::size_t hash = std::hash<Generation>{}(key.generation);
        const auto combine = [&hash](const std::size_t value) {
            hash ^= value + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
        };
        combine(std::hash<FrameIndex>{}(key.index));
        combine(std::hash<std::uint32_t>{}(key.decodePercent));
        return hash;
    }
};

[[nodiscard]] TaskKey MakeTaskKey(const DecodeTask& task) noexcept {
    return TaskKey{task.generation, task.index, task.decodePercent};
}

}  // namespace

class DecodeScheduler::Impl final {
public:
    Impl(
        std::size_t workerCount,
        Completion completion,
        DecodeFunction decodeFunction)
        : completion_(std::move(completion)),
          decodeFunction_(std::move(decodeFunction)) {
        workerCount = workerCount == 0 ? 1 : workerCount;
        workers_.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~Impl() {
        Shutdown();
    }

    [[nodiscard]] bool Submit(DecodeTask task) {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            return false;
        }

        if (latestWinsGeneration_
            && task.generation == *latestWinsGeneration_) {
            if (task.priority != DecodePriority::Current) {
                return false;
            }
            return SubmitLatestLocked(std::move(task));
        }

        return SubmitNormalLocked(std::move(task));
    }

    void BeginLatestWins(const Generation generation) {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || (latestWinsGeneration_
                && *latestWinsGeneration_ == generation
                && (!preservingBackgroundLatestGeneration_
                    || *preservingBackgroundLatestGeneration_ != generation))) {
                return;
            }

            // A scheduler has one stateful latest lane. A new transaction
            // supersedes any unfinished transaction that previously owned it.
            CancelLatestSlotsLocked();
            CancelMatchingLocked(
                [generation](const DecodeTask& task) {
                    return task.generation == generation;
                },
                true,
                true,
                true);
            if (suspendedBackgroundGeneration_
                && *suspendedBackgroundGeneration_ == generation) {
                suspendedBackgroundGeneration_.reset();
            }
            preservingBackgroundLatestGeneration_.reset();
            latestWinsGeneration_ = generation;
        }
        condition_.notify_all();
    }

    void BeginLatestWinsPreservingBackground(const Generation generation) {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || (latestWinsGeneration_
                && *latestWinsGeneration_ == generation
                && suspendedBackgroundGeneration_
                && *suspendedBackgroundGeneration_ == generation)) {
                return;
            }

            // Started PNG targets are allowed to finish and populate the cache.
            // Only the single not-yet-running target remains latest-wins.
            DetachLatestRunningPreservingCompletionLocked();
            ClearLatestPendingForReplacementLocked();
            latestWinsGeneration_.reset();
            CancelMatchingLocked(
                [generation](const DecodeTask& task) {
                    return task.generation == generation
                        && task.priority != DecodePriority::Background;
                },
                false,
                false,
                true);
            suspendedBackgroundGeneration_ = generation;
            preservingBackgroundLatestGeneration_ = generation;
            latestWinsGeneration_ = generation;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool SubmitLatest(DecodeTask task) {
        std::scoped_lock lock(mutex_);
        if (stopping_ || !latestWinsGeneration_
            || task.generation != *latestWinsGeneration_
            || task.priority != DecodePriority::Current) {
            return false;
        }
        return SubmitLatestLocked(std::move(task));
    }

    void ClearLatestTargetPreservingBackground(const Generation generation) {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || !latestWinsGeneration_
                || *latestWinsGeneration_ != generation
                || !preservingBackgroundLatestGeneration_
                || *preservingBackgroundLatestGeneration_ != generation) {
                return;
            }

            ClearLatestPendingForReplacementLocked();
            DetachLatestRunningPreservingCompletionLocked();
            CompactQueuesLocked();
        }
        condition_.notify_all();
    }

    void EndLatestWins(const Generation generation) {
        {
            std::scoped_lock lock(mutex_);
            if (!latestWinsGeneration_
                || *latestWinsGeneration_ != generation) {
                return;
            }
            // The final running/pending target remains owned by the latest
            // lane until it completes. Only subsequent normal scheduling is
            // re-enabled here.
            latestWinsGeneration_.reset();
            if (preservingBackgroundLatestGeneration_
                && *preservingBackgroundLatestGeneration_ == generation) {
                preservingBackgroundLatestGeneration_.reset();
            }
        }
        condition_.notify_all();
    }

    void ResumeBackground(const Generation generation) {
        {
            std::scoped_lock lock(mutex_);
            if (!suspendedBackgroundGeneration_
                || *suspendedBackgroundGeneration_ != generation) {
                return;
            }
            suspendedBackgroundGeneration_.reset();
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool SubmitNormalLocked(DecodeTask task) {

        const TaskKey key = MakeTaskKey(task);
        const auto existing = states_.find(key);
        if (existing != states_.end()) {
            const std::shared_ptr<TaskState>& state = existing->second;
            if (state->cancelled.load(std::memory_order_acquire)
                || state->running
                || state->latest) {
                return false;
            }

            const bool higherPriority = task.priority < state->task.priority;
            const bool betterRank = task.priority == state->task.priority
                && task.sortRank < state->task.sortRank;
            if (!higherPriority && !betterRank) {
                return false;
            }

            state->task.priority = task.priority;
            state->task.sortRank = task.sortRank;
            ++state->queueVersion;
            QueueState(state);
            MaybeCompactQueuesLocked();
            condition_.notify_one();
            return true;
        }

        auto state = std::make_shared<TaskState>();
        state->task = std::move(task);
        state->queueVersion = 1;
        states_.emplace(key, state);
        QueueState(state);
        MaybeCompactQueuesLocked();
        condition_.notify_one();
        return true;
    }

    [[nodiscard]] bool SubmitLatestLocked(DecodeTask task) {
        const TaskKey key = MakeTaskKey(task);
        const bool preservingBackground =
            preservingBackgroundLatestGeneration_
            && *preservingBackgroundLatestGeneration_ == task.generation;
        if (latestPending_
            && !latestPending_->cancelled.load(std::memory_order_acquire)
            && MakeTaskKey(latestPending_->task) == key) {
            return false;
        }
        if (latestRunning_
            && !latestRunning_->cancelled.load(std::memory_order_acquire)
            && MakeTaskKey(latestRunning_->task) == key) {
            return false;
        }

        // PNG sequence scrubbing allows started Current tasks to complete in
        // parallel and enter the cache. Normal/video latest-wins remains an
        // exclusive single-running lane and suppresses its superseded result.
        if (latestRunning_) {
            if (preservingBackground) {
                DetachLatestRunningPreservingCompletionLocked();
            } else if (!latestRunning_->promotedFromBackground) {
                latestRunning_->cancelled.store(true, std::memory_order_release);
                EraseStateIfOwnedLocked(latestRunning_);
            }
        }
        if (latestPending_) {
            ClearLatestPendingForReplacementLocked();
        }

        const auto existing = states_.find(key);
        if (existing != states_.end()) {
            // A cold PNG decode that is already running is the cheapest path
            // to the requested frame. Keep its callback alive instead of
            // launching a duplicate interactive decode.
            if (existing->second->running
                && existing->second->task.priority == DecodePriority::Background
                && !existing->second->cancelled.load(std::memory_order_acquire)) {
                return false;
            }
            if (preservingBackground
                && existing->second->running
                && existing->second->task.priority == DecodePriority::Current
                && !existing->second->cancelled.load(std::memory_order_acquire)) {
                return false;
            }
            if (preservingBackground
                && !existing->second->running
                && existing->second->task.priority == DecodePriority::Background
                && !existing->second->cancelled.load(std::memory_order_acquire)) {
                const std::shared_ptr<TaskState>& state = existing->second;
                state->promotedFromBackground = true;
                state->backgroundSortRank = state->task.sortRank;
                state->task.priority = DecodePriority::Current;
                state->task.sortRank = task.sortRank;
                state->latest = true;
                ++state->queueVersion;
                latestPending_ = state;
                MaybeCompactQueuesLocked();
                condition_.notify_one();
                return true;
            }
            existing->second->cancelled.store(true, std::memory_order_release);
            states_.erase(existing);
        }

        auto state = std::make_shared<TaskState>();
        state->task = std::move(task);
        state->latest = true;
        states_.emplace(key, state);
        latestPending_ = std::move(state);
        condition_.notify_one();
        return true;
    }

    void CancelGeneration(const Generation generation) {
        CancelMatching([generation](const DecodeTask& task) {
            return task.generation == generation;
        }, true, false, true, true, true);
    }

    void CancelGenerationForReplacement(const Generation generation) {
        CancelMatching([generation](const DecodeTask& task) {
            return task.generation == generation;
        }, true, true, true, true, true);
    }

    void CancelAllExcept(const Generation generationToKeep) {
        CancelMatching([generationToKeep](const DecodeTask& task) {
            return task.generation != generationToKeep;
        }, true, false, true, true, true);
    }

    void CancelCurrent(const Generation generation) {
        CancelMatching([generation](const DecodeTask& task) {
            return task.generation == generation
                && task.priority == DecodePriority::Current;
        }, false);
    }

    void CancelInteractive(const Generation generation) {
        CancelMatching([generation](const DecodeTask& task) {
            return task.generation == generation
                && task.priority <= DecodePriority::Neighborhood;
        }, false);
    }

    void CancelBackground(const Generation generation) {
        CancelMatching(
            [generation](const DecodeTask& task) {
                return task.generation == generation
                    && task.priority == DecodePriority::Background;
            },
            true,
            // A fast return to the foreground immediately replans the same
            // frame as Current/Forward work. Detach a cancelled running cold
            // task from its key so that replacement cannot be rejected while
            // the old decoder cooperatively exits. The worker still owns the
            // old TaskState and suppresses its completion callback.
            true,
            false,
            true,
            true);
    }

    [[nodiscard]] std::size_t PendingTaskCount() const {
        std::scoped_lock lock(mutex_);
        return states_.size();
    }

    [[nodiscard]] std::size_t PendingTaskCount(const DecodePriority priority) const {
        std::scoped_lock lock(mutex_);
        std::size_t count = 0;
        for (const auto& [key, state] : states_) {
            (void)key;
            if (!state->cancelled.load(std::memory_order_acquire)
                && state->task.priority == priority) {
                ++count;
            }
        }
        return count;
    }

    void SetBackgroundConcurrency(const std::size_t maximumConcurrentTasks) {
        {
            std::scoped_lock lock(mutex_);
            backgroundConcurrencyLimit_ = std::clamp<std::size_t>(
                maximumConcurrentTasks,
                1,
                workers_.size() > 1 ? workers_.size() - 1 : 1);
        }
        condition_.notify_all();
    }

    void Shutdown() {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            for (auto& [key, state] : states_) {
                (void)key;
                state->cancelled.store(true, std::memory_order_release);
            }
            if (latestRunning_) {
                latestRunning_->cancelled.store(true, std::memory_order_release);
            }
            if (latestPending_) {
                latestPending_->cancelled.store(true, std::memory_order_release);
            }
            states_.clear();
            latestWinsGeneration_.reset();
            preservingBackgroundLatestGeneration_.reset();
            suspendedBackgroundGeneration_.reset();
            latestPending_.reset();
            latestRunning_.reset();
            interactiveQueue_ = {};
            backgroundQueue_ = {};
        }
        condition_.notify_all();

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    struct TaskState {
        DecodeTask task;
        std::atomic_bool cancelled{false};
        bool running = false;
        bool latest = false;
        bool promotedFromBackground = false;
        std::uint64_t backgroundSortRank = 0;
        std::uint64_t queueVersion = 0;
    };

    struct QueueEntry {
        std::shared_ptr<TaskState> state;
        DecodePriority priority = DecodePriority::Background;
        std::uint64_t sortRank = 0;
        std::uint64_t serial = 0;
        std::uint64_t queueVersion = 0;
    };

    struct QueueEntryLater {
        [[nodiscard]] bool operator()(const QueueEntry& left, const QueueEntry& right) const noexcept {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            if (left.sortRank != right.sortRank) {
                return left.sortRank > right.sortRank;
            }
            return left.serial > right.serial;
        }
    };

    using TaskQueue =
        std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryLater>;

    template <typename Predicate>
    void CancelMatching(
        Predicate&& predicate,
        const bool cancelRunning,
        const bool releaseRunningKey = false,
        const bool clearMatchingLatestMode = false,
        const bool compactImmediately = false,
        const bool clearMatchingBackgroundSuspension = false) {
        {
            std::scoped_lock lock(mutex_);
            CancelMatchingLocked(
                predicate,
                cancelRunning,
                releaseRunningKey,
                compactImmediately);
            if (clearMatchingLatestMode && latestWinsGeneration_) {
                DecodeTask modeMarker;
                modeMarker.generation = *latestWinsGeneration_;
                if (predicate(modeMarker)) {
                    latestWinsGeneration_.reset();
                }
            }
            if (clearMatchingLatestMode
                && preservingBackgroundLatestGeneration_) {
                DecodeTask modeMarker;
                modeMarker.generation =
                    *preservingBackgroundLatestGeneration_;
                if (predicate(modeMarker)) {
                    preservingBackgroundLatestGeneration_.reset();
                }
            }
            if (clearMatchingBackgroundSuspension
                && suspendedBackgroundGeneration_) {
                DecodeTask modeMarker;
                modeMarker.generation = *suspendedBackgroundGeneration_;
                modeMarker.priority = DecodePriority::Background;
                if (predicate(modeMarker)) {
                    suspendedBackgroundGeneration_.reset();
                }
            }
        }
        condition_.notify_all();
    }

    template <typename Predicate>
    void CancelMatchingLocked(
        Predicate&& predicate,
        const bool cancelRunning,
        const bool releaseRunningKey = false,
        const bool compactImmediately = false,
        const bool preservePromotedRunning = false) {
        for (auto iterator = states_.begin(); iterator != states_.end();) {
            const std::shared_ptr<TaskState>& state = iterator->second;
            if (!predicate(state->task)) {
                ++iterator;
                continue;
            }

            if (state->running) {
                if (preservePromotedRunning
                    && state->promotedFromBackground) {
                    ++iterator;
                    continue;
                }
                if (cancelRunning) {
                    state->cancelled.store(true, std::memory_order_release);
                    if (releaseRunningKey) {
                        iterator = states_.erase(iterator);
                        continue;
                    }
                }
                ++iterator;
                continue;
            }

            state->cancelled.store(true, std::memory_order_release);
            iterator = states_.erase(iterator);
        }

        if (latestPending_ && predicate(latestPending_->task)) {
            latestPending_->cancelled.store(true, std::memory_order_release);
            EraseStateIfOwnedLocked(latestPending_);
            latestPending_.reset();
        }
        if (latestRunning_ && predicate(latestRunning_->task) && cancelRunning
            && !(preservePromotedRunning
                && latestRunning_->promotedFromBackground)) {
            latestRunning_->cancelled.store(true, std::memory_order_release);
            if (releaseRunningKey) {
                EraseStateIfOwnedLocked(latestRunning_);
            }
        }
        if (compactImmediately) {
            CompactQueuesLocked();
        } else {
            MaybeCompactQueuesLocked();
        }
    }

    void RestorePromotedBackgroundLocked(
        const std::shared_ptr<TaskState>& state) {
        if (!state || state->running || !state->promotedFromBackground) {
            return;
        }
        state->task.priority = DecodePriority::Background;
        state->task.sortRank = state->backgroundSortRank;
        state->latest = false;
        state->promotedFromBackground = false;
        ++state->queueVersion;
        QueueState(state);
        MaybeCompactQueuesLocked();
    }

    void ClearLatestPendingForReplacementLocked() {
        if (!latestPending_) {
            return;
        }
        if (latestPending_->promotedFromBackground) {
            RestorePromotedBackgroundLocked(latestPending_);
        } else {
            latestPending_->cancelled.store(true, std::memory_order_release);
            EraseStateIfOwnedLocked(latestPending_);
        }
        latestPending_.reset();
    }

    void DetachLatestRunningPreservingCompletionLocked() {
        if (!latestRunning_) {
            return;
        }
        latestRunning_->latest = false;
        latestRunning_.reset();
    }

    void CancelLatestSlotsLocked() {
        if (latestPending_) {
            ClearLatestPendingForReplacementLocked();
        }
        if (latestRunning_) {
            if (!latestRunning_->promotedFromBackground) {
                latestRunning_->cancelled.store(true, std::memory_order_release);
                EraseStateIfOwnedLocked(latestRunning_);
            }
        }
        latestWinsGeneration_.reset();
    }

    void EraseStateIfOwnedLocked(const std::shared_ptr<TaskState>& state) {
        if (!state) {
            return;
        }
        const auto found = states_.find(MakeTaskKey(state->task));
        if (found != states_.end() && found->second == state) {
            states_.erase(found);
        }
    }

    void QueueState(const std::shared_ptr<TaskState>& state) {
        QueueEntry entry;
        entry.state = state;
        entry.priority = state->task.priority;
        entry.sortRank = state->task.sortRank;
        entry.serial = nextSerial_++;
        entry.queueVersion = state->queueVersion;
        TaskQueue& targetQueue = state->task.priority == DecodePriority::Background
            ? backgroundQueue_
            : interactiveQueue_;
        targetQueue.push(std::move(entry));
    }

    void RemoveInvalidQueueEntries(TaskQueue& queue) {
        while (!queue.empty()) {
            const QueueEntry& entry = queue.top();
            const std::shared_ptr<TaskState>& state = entry.state;
            if (state
                && !state->cancelled.load(std::memory_order_acquire)
                && !state->running
                && !state->latest
                && state->queueVersion == entry.queueVersion) {
                return;
            }
            queue.pop();
        }
    }

    [[nodiscard]] static bool IsValidQueueEntry(const QueueEntry& entry) {
        const std::shared_ptr<TaskState>& state = entry.state;
        return state
            && !state->cancelled.load(std::memory_order_acquire)
            && !state->running
            && !state->latest
            && state->queueVersion == entry.queueVersion;
    }

    void CompactQueueLocked(TaskQueue& queue) {
        TaskQueue compacted;
        while (!queue.empty()) {
            QueueEntry entry = queue.top();
            queue.pop();
            if (IsValidQueueEntry(entry)) {
                compacted.push(std::move(entry));
            }
        }
        queue = std::move(compacted);
    }

    void CompactQueuesLocked() {
        CompactQueueLocked(interactiveQueue_);
        CompactQueueLocked(backgroundQueue_);
    }

    void MaybeCompactQueuesLocked() {
        constexpr std::size_t kQueueTombstoneAllowance = 64;
        const std::size_t physicalQueueSize =
            interactiveQueue_.size() + backgroundQueue_.size();
        const std::size_t logicalStateCount = states_.size();
        if (physicalQueueSize
            > (logicalStateCount * 2U) + kQueueTombstoneAllowance) {
            CompactQueuesLocked();
        }
    }

    [[nodiscard]] std::shared_ptr<TaskState> TakeLatestTask() {
        if (!latestPending_ || latestRunning_) {
            return {};
        }
        std::shared_ptr<TaskState> state = std::move(latestPending_);
        latestPending_.reset();
        state->running = true;
        latestRunning_ = state;
        return state;
    }

    [[nodiscard]] std::shared_ptr<TaskState> TakeFromQueue(TaskQueue& queue) {
        RemoveInvalidQueueEntries(queue);
        if (!queue.empty()) {
            QueueEntry entry = queue.top();
            const std::shared_ptr<TaskState>& state = entry.state;
            queue.pop();
            state->running = true;
            if (state->task.priority == DecodePriority::Background) {
                ++runningBackgroundTasks_;
            }
            return state;
        }
        return {};
    }

    [[nodiscard]] std::shared_ptr<TaskState> TakeNextTask() {
        if (std::shared_ptr<TaskState> latest = TakeLatestTask()) {
            return latest;
        }
        RemoveInvalidQueueEntries(interactiveQueue_);
        if (!interactiveQueue_.empty()
            && interactiveQueue_.top().priority == DecodePriority::Current) {
            return TakeFromQueue(interactiveQueue_);
        }
        // Stateful video decode uses a single worker. Forward/neighborhood
        // requests must drain before background fill so SourceReader can stay
        // sequential and playback prefetch cannot be starved by RAM filling.
        if (workers_.size() == 1U && !interactiveQueue_.empty()) {
            return TakeFromQueue(interactiveQueue_);
        }
        const bool backgroundSuspended = suspendedBackgroundGeneration_
            && !backgroundQueue_.empty()
            && backgroundQueue_.top().state
            && backgroundQueue_.top().state->task.generation
                == *suspendedBackgroundGeneration_;
        if (!backgroundSuspended
            && runningBackgroundTasks_ < backgroundConcurrencyLimit_) {
            if (std::shared_ptr<TaskState> background = TakeFromQueue(backgroundQueue_)) {
                return background;
            }
        }
        return TakeFromQueue(interactiveQueue_);
    }

    void WorkerLoop() {
        std::unique_ptr<WicImageDecoder> decoder;
        if (!decodeFunction_) {
            decoder = std::make_unique<WicImageDecoder>();
        }
        while (true) {
            std::shared_ptr<TaskState> state;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || HasRunnableTask(); });
                if (stopping_) {
                    return;
                }
                state = TakeNextTask();
                if (!state) {
                    continue;
                }
            }

            ScheduledDecodeResult scheduledResult;
            scheduledResult.task = state->task;
            try {
                if (decodeFunction_) {
                    scheduledResult.frame = decodeFunction_(
                        state->task,
                        scheduledResult.errorUtf8,
                        state->cancelled);
                } else {
                    ImageDecodeResult decodeResult = decoder->Decode(
                        state->task.file.path,
                        state->task.index,
                        state->task.generation,
                        state->task.decodePercent);
                    scheduledResult.frame = std::move(decodeResult.frame);
                    scheduledResult.errorUtf8 = std::move(decodeResult.errorUtf8);
                }
            } catch (const std::exception& error) {
                scheduledResult.errorUtf8 = std::string("解码线程异常: ") + error.what();
            } catch (...) {
                scheduledResult.errorUtf8 = "解码线程发生未知异常";
            }

            const bool cancelled = state->cancelled.load(std::memory_order_acquire);
            if (!cancelled && completion_) {
                try {
                    completion_(std::move(scheduledResult));
                } catch (...) {
                    // Completion belongs to the controller. A controller bug must
                    // not silently terminate the decode worker pool.
                }
            }

            {
                std::scoped_lock lock(mutex_);
                if (state->task.priority == DecodePriority::Background
                    && runningBackgroundTasks_ > 0) {
                    --runningBackgroundTasks_;
                }
                const TaskKey key = MakeTaskKey(state->task);
                const auto found = states_.find(key);
                if (found != states_.end() && found->second == state) {
                    states_.erase(found);
                }
                if (state->latest && latestRunning_ == state) {
                    latestRunning_.reset();
                }
            }
            condition_.notify_all();
        }
    }

    [[nodiscard]] bool HasRunnableTask() {
        if (latestPending_ && !latestRunning_) {
            return true;
        }
        RemoveInvalidQueueEntries(interactiveQueue_);
        RemoveInvalidQueueEntries(backgroundQueue_);
        if (!interactiveQueue_.empty()
            && interactiveQueue_.top().priority == DecodePriority::Current) {
            return true;
        }
        const bool backgroundSuspended = suspendedBackgroundGeneration_
            && !backgroundQueue_.empty()
            && backgroundQueue_.top().state
            && backgroundQueue_.top().state->task.generation
                == *suspendedBackgroundGeneration_;
        return (!backgroundSuspended
                && !backgroundQueue_.empty()
                && runningBackgroundTasks_ < backgroundConcurrencyLimit_)
            || !interactiveQueue_.empty();
    }

    Completion completion_;
    DecodeFunction decodeFunction_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    std::uint64_t nextSerial_ = 0;
    std::size_t runningBackgroundTasks_ = 0;
    std::size_t backgroundConcurrencyLimit_ = 1;
    std::optional<Generation> latestWinsGeneration_;
    std::optional<Generation> preservingBackgroundLatestGeneration_;
    std::optional<Generation> suspendedBackgroundGeneration_;
    std::shared_ptr<TaskState> latestPending_;
    std::shared_ptr<TaskState> latestRunning_;
    TaskQueue interactiveQueue_;
    TaskQueue backgroundQueue_;
    std::unordered_map<TaskKey, std::shared_ptr<TaskState>, TaskKeyHash> states_;
    std::vector<std::thread> workers_;
};

DecodeScheduler::DecodeScheduler(
    const std::size_t workerCount,
    Completion completion,
    DecodeFunction decodeFunction)
    : impl_(std::make_unique<Impl>(
          workerCount,
          std::move(completion),
          std::move(decodeFunction))) {}

DecodeScheduler::~DecodeScheduler() = default;

bool DecodeScheduler::Submit(DecodeTask task) {
    return impl_->Submit(std::move(task));
}

void DecodeScheduler::BeginLatestWins(const Generation generation) {
    impl_->BeginLatestWins(generation);
}

void DecodeScheduler::BeginLatestWinsPreservingBackground(
    const Generation generation) {
    impl_->BeginLatestWinsPreservingBackground(generation);
}

bool DecodeScheduler::SubmitLatest(DecodeTask task) {
    return impl_->SubmitLatest(std::move(task));
}

void DecodeScheduler::ClearLatestTargetPreservingBackground(
    const Generation generation) {
    impl_->ClearLatestTargetPreservingBackground(generation);
}

void DecodeScheduler::EndLatestWins(const Generation generation) {
    impl_->EndLatestWins(generation);
}

void DecodeScheduler::ResumeBackground(const Generation generation) {
    impl_->ResumeBackground(generation);
}

void DecodeScheduler::CancelGeneration(const Generation generation) {
    impl_->CancelGeneration(generation);
}

void DecodeScheduler::CancelGenerationForReplacement(
    const Generation generation) {
    impl_->CancelGenerationForReplacement(generation);
}

void DecodeScheduler::CancelAllExcept(const Generation generationToKeep) {
    impl_->CancelAllExcept(generationToKeep);
}

void DecodeScheduler::CancelCurrent(const Generation generation) {
    impl_->CancelCurrent(generation);
}

void DecodeScheduler::CancelInteractive(const Generation generation) {
    impl_->CancelInteractive(generation);
}

void DecodeScheduler::CancelBackground(const Generation generation) {
    impl_->CancelBackground(generation);
}

void DecodeScheduler::SetBackgroundConcurrency(const std::size_t maximumConcurrentTasks) {
    impl_->SetBackgroundConcurrency(maximumConcurrentTasks);
}

std::size_t DecodeScheduler::PendingTaskCount() const {
    return impl_->PendingTaskCount();
}

std::size_t DecodeScheduler::PendingTaskCount(const DecodePriority priority) const {
    return impl_->PendingTaskCount(priority);
}

void DecodeScheduler::Shutdown() {
    impl_->Shutdown();
}

}  // namespace zt::sequence
