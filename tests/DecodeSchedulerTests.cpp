#include "Core/DecodeScheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using zt::sequence::DecodePriority;
using zt::sequence::DecodeScheduler;
using zt::sequence::DecodeTask;
using zt::sequence::DecodedFrame;
using zt::sequence::ScheduledDecodeResult;

[[nodiscard]] std::shared_ptr<DecodedFrame> MakeFrame(const DecodeTask& task) {
    auto frame = std::make_shared<DecodedFrame>();
    frame->generation = task.generation;
    frame->index = task.index;
    frame->width = 1;
    frame->height = 1;
    frame->strideBytes = 4;
    frame->decodePercent = task.decodePercent;
    frame->bgraPixels.resize(4);
    return frame;
}

[[nodiscard]] DecodeTask MakeTask(
    const std::uint64_t generation,
    const std::uint32_t index,
    const DecodePriority priority) {
    DecodeTask task;
    task.generation = generation;
    task.index = index;
    task.decodePercent = 100;
    task.priority = priority;
    task.sortRank = index;
    return task;
}

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

template <typename Predicate>
[[nodiscard]] bool WaitUntil(
    std::condition_variable& condition,
    std::mutex& mutex,
    Predicate&& predicate,
    const std::chrono::milliseconds timeout = 1500ms) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, timeout, std::forward<Predicate>(predicate));
}

[[nodiscard]] bool TestRunningInteractiveTaskIsNotDuplicated() {
    std::mutex mutex;
    std::condition_variable condition;
    int started = 0;
    int decoded = 0;
    int callbacks = 0;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                ++callbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            {
                std::scoped_lock lock(mutex);
                ++started;
                condition.notify_all();
            }
            std::this_thread::sleep_for(120ms);
            {
                std::scoped_lock lock(mutex);
                ++decoded;
                condition.notify_all();
            }
            return MakeFrame(task);
        });

    const DecodeTask task = MakeTask(1, 7, DecodePriority::Current);
    bool passed = Expect(scheduler.Submit(task), "submit current task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "current task started");

    scheduler.CancelCurrent(1);
    passed &= Expect(!scheduler.Submit(task), "running current task was not duplicated");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks == 1; }),
        "running current task still completed to callback");

    scheduler.Shutdown();
    passed &= Expect(decoded == 1, "current task decoded exactly once");
    return passed;
}

[[nodiscard]] bool TestCancelledGenerationDropsResultAndKeepsDeduplication() {
    std::mutex mutex;
    std::condition_variable condition;
    int started = 0;
    int decoded = 0;
    int callbacks = 0;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult) {
            std::scoped_lock lock(mutex);
            ++callbacks;
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            {
                std::scoped_lock lock(mutex);
                ++started;
                condition.notify_all();
            }
            std::this_thread::sleep_for(120ms);
            {
                std::scoped_lock lock(mutex);
                ++decoded;
                condition.notify_all();
            }
            return MakeFrame(task);
        });

    const DecodeTask task = MakeTask(2, 9, DecodePriority::Forward);
    bool passed = Expect(scheduler.Submit(task), "submit generation task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "generation task started");
    scheduler.CancelGeneration(2);
    passed &= Expect(!scheduler.Submit(task), "cancelled running generation stayed deduplicated");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return decoded == 1; }),
        "cancelled generation task finished cleanup");
    std::this_thread::sleep_for(30ms);
    scheduler.Shutdown();
    passed &= Expect(callbacks == 0, "cancelled generation result was discarded");
    return passed;
}

[[nodiscard]] bool TestCancelledGenerationCanQueueLatestReplacement() {
    std::mutex mutex;
    std::condition_variable condition;
    int started = 0;
    int callbacks = 0;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                ++callbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task,
            std::string&,
            const std::atomic_bool& cancelled) {
            int invocation = 0;
            {
                std::scoped_lock lock(mutex);
                invocation = ++started;
                condition.notify_all();
            }
            if (invocation == 1) {
                while (!cancelled.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            return MakeFrame(task);
        });

    const DecodeTask task = MakeTask(7, 12, DecodePriority::Current);
    bool passed = Expect(
        scheduler.Submit(task),
        "submit replaceable generation task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "replaceable generation task started");
    scheduler.CancelGenerationForReplacement(7);
    passed &= Expect(
        scheduler.Submit(task),
        "latest replacement queued while cancelled task exits");
    passed &= Expect(
        WaitUntil(
            condition,
            mutex,
            [&] { return started == 2 && callbacks == 1; }),
        "latest replacement completed exactly once");
    scheduler.Shutdown();
    return passed;
}

[[nodiscard]] bool TestBackgroundLeavesInteractiveWorkerAvailable() {
    std::mutex mutex;
    std::condition_variable condition;
    int backgroundActive = 0;
    int maximumBackgroundActive = 0;
    int backgroundStarted = 0;
    int currentCallbacks = 0;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult result) {
            if (result && result.task.priority == DecodePriority::Current) {
                std::scoped_lock lock(mutex);
                ++currentCallbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            if (task.priority == DecodePriority::Background) {
                {
                    std::scoped_lock lock(mutex);
                    ++backgroundActive;
                    ++backgroundStarted;
                    maximumBackgroundActive = std::max(
                        maximumBackgroundActive,
                        backgroundActive);
                    condition.notify_all();
                }
                std::this_thread::sleep_for(250ms);
                {
                    std::scoped_lock lock(mutex);
                    --backgroundActive;
                    condition.notify_all();
                }
            } else {
                std::this_thread::sleep_for(10ms);
            }
            return MakeFrame(task);
        });

    scheduler.SetBackgroundConcurrency(4);
    bool passed = Expect(
        scheduler.Submit(MakeTask(3, 0, DecodePriority::Background)),
        "submit first background task");
    passed &= Expect(
        scheduler.Submit(MakeTask(3, 1, DecodePriority::Background)),
        "submit second background task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return backgroundStarted >= 1; }),
        "background task started");

    passed &= Expect(
        scheduler.Submit(MakeTask(3, 2, DecodePriority::Current)),
        "submit current task while background is active");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return currentCallbacks == 1; }, 200ms),
        "reserved worker completed current task without waiting for background");

    scheduler.Shutdown();
    passed &= Expect(maximumBackgroundActive == 1, "two-worker pool reserved one interactive worker");
    return passed;
}

[[nodiscard]] bool TestBackgroundProgressesDuringContinuousInteractiveWork() {
    std::mutex mutex;
    std::condition_variable condition;
    int forwardStarted = 0;
    int backgroundCallbacks = 0;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult result) {
            if (result && result.task.priority == DecodePriority::Background) {
                std::scoped_lock lock(mutex);
                ++backgroundCallbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            if (task.priority == DecodePriority::Forward) {
                {
                    std::scoped_lock lock(mutex);
                    ++forwardStarted;
                    condition.notify_all();
                }
                std::this_thread::sleep_for(80ms);
            }
            return MakeFrame(task);
        });

    scheduler.SetBackgroundConcurrency(1);
    bool passed = true;
    for (std::uint32_t index = 0; index < 12; ++index) {
        passed &= Expect(
            scheduler.Submit(MakeTask(4, index, DecodePriority::Forward)),
            "submit forward backlog task");
    }
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return forwardStarted >= 2; }),
        "interactive workers started");
    passed &= Expect(
        scheduler.Submit(MakeTask(4, 100, DecodePriority::Background)),
        "submit background task behind interactive backlog");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return backgroundCallbacks == 1; }, 250ms),
        "background task made progress while interactive backlog remained");

    scheduler.Shutdown();
    return passed;
}

[[nodiscard]] bool TestFourToOneTransitionKeepsCurrentResponsive() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBackground = false;
    int backgroundActive = 0;
    int backgroundStarted = 0;
    int maximumBackgroundActive = 0;
    int currentCallbacks = 0;

    DecodeScheduler scheduler(
        5,
        [&](ScheduledDecodeResult result) {
            if (result && result.task.priority == DecodePriority::Current) {
                std::scoped_lock lock(mutex);
                ++currentCallbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            if (task.priority == DecodePriority::Background) {
                std::unique_lock lock(mutex);
                ++backgroundActive;
                ++backgroundStarted;
                maximumBackgroundActive = std::max(
                    maximumBackgroundActive,
                    backgroundActive);
                condition.notify_all();
                condition.wait(lock, [&] { return releaseBackground; });
                --backgroundActive;
                condition.notify_all();
            }
            return MakeFrame(task);
        });

    scheduler.SetBackgroundConcurrency(4);
    bool passed = true;
    for (std::uint32_t index = 0; index < 5; ++index) {
        passed &= Expect(
            scheduler.Submit(MakeTask(5, index, DecodePriority::Background)),
            "submit transition background task");
    }
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return backgroundStarted == 4; }),
        "four paused background tasks started");

    scheduler.SetBackgroundConcurrency(1);
    passed &= Expect(
        scheduler.Submit(MakeTask(5, 100, DecodePriority::Current)),
        "submit current task after four-to-one transition");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return currentCallbacks == 1; }, 200ms),
        "reserved worker served current task during four-to-one transition");
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            backgroundStarted == 4,
            "fifth background task stayed queued while active count exceeded one");
        releaseBackground = true;
    }
    condition.notify_all();

    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return backgroundStarted == 5; }),
        "queued background resumed after transition settled");
    scheduler.Shutdown();
    passed &= Expect(maximumBackgroundActive == 4, "paused background concurrency reached four");
    return passed;
}

[[nodiscard]] bool TestSingleWorkerPrefersInteractiveBeforeBackground() {
    std::mutex mutex;
    std::condition_variable condition;
    bool currentStarted = false;
    bool releaseCurrent = false;
    std::vector<DecodePriority> completionOrder;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                completionOrder.push_back(result.task.priority);
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            if (task.priority == DecodePriority::Current) {
                std::unique_lock lock(mutex);
                currentStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseCurrent; });
            }
            return MakeFrame(task);
        });

    bool passed = true;
    passed &= Expect(
        scheduler.Submit(MakeTask(6, 0, DecodePriority::Current)),
        "submit single-worker current task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return currentStarted; }),
        "single-worker current task started");
    passed &= Expect(
        scheduler.Submit(MakeTask(6, 1, DecodePriority::Background)),
        "submit single-worker background task");
    passed &= Expect(
        scheduler.Submit(MakeTask(6, 2, DecodePriority::Forward)),
        "submit single-worker forward task");
    {
        std::scoped_lock lock(mutex);
        releaseCurrent = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(
            condition,
            mutex,
            [&] { return completionOrder.size() == 3U; }),
        "single-worker backlog completed");
    scheduler.Shutdown();

    passed &= Expect(
        completionOrder.size() == 3U &&
            completionOrder[0] == DecodePriority::Current &&
            completionOrder[1] == DecodePriority::Forward &&
            completionOrder[2] == DecodePriority::Background,
        "single worker drains interactive work before background");
    return passed;
}

[[nodiscard]] bool TestLatestWinsKeepsOnlyNewestPendingTask() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseFirst = false;
    int activeTasks = 0;
    int maximumActiveTasks = 0;
    std::vector<std::uint32_t> startedIndices;
    std::vector<std::uint32_t> callbackIndices;

    DecodeScheduler scheduler(
        4,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                callbackIndices.push_back(result.task.index);
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            ++activeTasks;
            maximumActiveTasks = std::max(maximumActiveTasks, activeTasks);
            startedIndices.push_back(task.index);
            condition.notify_all();
            if (startedIndices.size() == 1U) {
                condition.wait(lock, [&] { return releaseFirst; });
            }
            --activeTasks;
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 20;
    constexpr std::uint32_t firstIndex = 10;
    constexpr std::uint32_t finalIndex = 4000;
    scheduler.BeginLatestWins(generation);
    bool passed = Expect(
        scheduler.SubmitLatest(
            MakeTask(generation, firstIndex, DecodePriority::Current)),
        "submit first latest-wins task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return startedIndices.size() == 1U; }),
        "first latest-wins task started");

    for (std::uint32_t index = firstIndex + 1; index <= finalIndex; ++index) {
        passed &= Expect(
            scheduler.SubmitLatest(
                MakeTask(generation, index, DecodePriority::Current)),
            "replace latest-wins pending task");
    }
    passed &= Expect(
        scheduler.PendingTaskCount() == 1U
            && scheduler.PendingTaskCount(DecodePriority::Current) == 1U,
        "latest-wins kept exactly one logical pending task");

    {
        std::scoped_lock lock(mutex);
        releaseFirst = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(
            condition,
            mutex,
            [&] { return callbackIndices.size() == 1U; }),
        "newest latest-wins task completed");
    scheduler.EndLatestWins(generation);
    scheduler.Shutdown();

    passed &= Expect(
        startedIndices.size() == 2U
            && startedIndices[0] == firstIndex
            && startedIndices[1] == finalIndex,
        "only running and newest pending latest-wins tasks decoded");
    passed &= Expect(
        maximumActiveTasks == 1,
        "latest-wins allowed at most one running task across workers");
    passed &= Expect(
        callbackIndices.size() == 1U && callbackIndices[0] == finalIndex,
        "superseded latest-wins result was discarded");
    return passed;
}

[[nodiscard]] bool TestLatestWinsRejectsPrefetchAndEndPreservesFinalTarget() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseFirst = false;
    std::vector<std::uint32_t> startedIndices;
    std::vector<std::uint32_t> callbackIndices;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                callbackIndices.push_back(result.task.index);
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            startedIndices.push_back(task.index);
            condition.notify_all();
            if (task.index == 1U) {
                condition.wait(lock, [&] { return releaseFirst; });
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 21;
    scheduler.BeginLatestWins(generation);
    bool passed = Expect(
        !scheduler.Submit(MakeTask(generation, 90, DecodePriority::Forward))
            && !scheduler.Submit(
                MakeTask(generation, 91, DecodePriority::Neighborhood))
            && !scheduler.Submit(
                MakeTask(generation, 92, DecodePriority::Background)),
        "active latest-wins mode rejected normal prefetch");
    passed &= Expect(
        scheduler.Submit(MakeTask(generation, 1, DecodePriority::Current)),
        "normal Current submit was routed into latest-wins lane");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return startedIndices.size() == 1U; }),
        "routed Current task started");
    passed &= Expect(
        scheduler.SubmitLatest(
            MakeTask(generation, 2, DecodePriority::Current)),
        "final latest-wins target queued");

    scheduler.EndLatestWins(generation);
    passed &= Expect(
        scheduler.Submit(MakeTask(generation, 3, DecodePriority::Forward)),
        "normal prefetch resumed after latest-wins ended");
    {
        std::scoped_lock lock(mutex);
        releaseFirst = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(
            condition,
            mutex,
            [&] { return callbackIndices.size() == 2U; }),
        "final latest target and resumed prefetch completed");
    scheduler.Shutdown();

    passed &= Expect(
        callbackIndices.size() == 2U
            && callbackIndices[0] == 2U
            && callbackIndices[1] == 3U,
        "final latest target completed before resumed prefetch");
    return passed;
}

[[nodiscard]] bool TestLatestWinsCancellationClearsPendingAndMode() {
    std::mutex mutex;
    std::condition_variable condition;
    int started = 0;
    int callbacks = 0;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                ++callbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task,
            std::string&,
            const std::atomic_bool& cancelled) {
            {
                std::scoped_lock lock(mutex);
                ++started;
                condition.notify_all();
            }
            if (task.index == 1U) {
                while (!cancelled.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 22;
    scheduler.BeginLatestWins(generation);
    bool passed = Expect(
        scheduler.SubmitLatest(
            MakeTask(generation, 1, DecodePriority::Current)),
        "submit cancellable latest-wins task");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "cancellable latest-wins task started");
    passed &= Expect(
        scheduler.SubmitLatest(
            MakeTask(generation, 2, DecodePriority::Current)),
        "queue latest-wins task before generation cancellation");

    scheduler.CancelGeneration(generation);
    passed &= Expect(
        WaitUntil(condition, mutex, [&] {
            return scheduler.PendingTaskCount() == 0U;
        }),
        "generation cancellation removed latest-wins running key and pending slot");
    passed &= Expect(
        scheduler.Submit(MakeTask(generation, 3, DecodePriority::Forward)),
        "generation cancellation also cleared active latest-wins mode");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks == 1; }),
        "normal task completed after latest-wins cancellation");
    scheduler.Shutdown();

    passed &= Expect(started == 2, "cancelled latest-wins pending task never started");
    passed &= Expect(callbacks == 1, "cancelled latest-wins result was discarded");
    return passed;
}

[[nodiscard]] bool TestLatestWinsCancelAllExceptAndShutdown() {
    std::mutex mutex;
    std::condition_variable condition;
    int started = 0;
    int callbacks = 0;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (result) {
                std::scoped_lock lock(mutex);
                ++callbacks;
                condition.notify_all();
            }
        },
        [&](const DecodeTask& task,
            std::string&,
            const std::atomic_bool& cancelled) {
            {
                std::scoped_lock lock(mutex);
                ++started;
                condition.notify_all();
            }
            (void)task;
            while (!cancelled.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
            return MakeFrame(task);
        });

    scheduler.BeginLatestWins(23);
    bool passed = Expect(
        scheduler.SubmitLatest(MakeTask(23, 1, DecodePriority::Current)),
        "submit latest task before CancelAllExcept");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "latest task before CancelAllExcept started");
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(23, 2, DecodePriority::Current)),
        "queue pending latest task before CancelAllExcept");
    scheduler.CancelAllExcept(24);
    passed &= Expect(
        WaitUntil(condition, mutex, [&] {
            return scheduler.PendingTaskCount() == 0U;
        }),
        "CancelAllExcept cleared non-kept latest lane");

    scheduler.BeginLatestWins(24);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(24, 3, DecodePriority::Current)),
        "submit kept-generation latest task before shutdown");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 2; }),
        "kept-generation latest task started before shutdown");
    scheduler.CancelAllExcept(24);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(24, 4, DecodePriority::Current)),
        "CancelAllExcept retained kept-generation latest mode");
    scheduler.Shutdown();
    passed &= Expect(started == 2, "shutdown did not run discarded pending task");
    passed &= Expect(callbacks == 0, "shutdown discarded running and pending latest results");
    return passed;
}

[[nodiscard]] bool TestSequenceLatestWinsPausesAndResumesBackground() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBackground = false;
    bool releaseFirstInteractive = false;
    int backgroundStarted = 0;
    std::vector<std::uint32_t> startedCurrent;
    std::vector<std::uint32_t> callbacks;

    DecodeScheduler scheduler(
        3,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.index);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            if (task.priority == DecodePriority::Background) {
                ++backgroundStarted;
                condition.notify_all();
                if (task.index < 2U) {
                    condition.wait(lock, [&] { return releaseBackground; });
                }
            } else if (task.priority == DecodePriority::Current) {
                startedCurrent.push_back(task.index);
                condition.notify_all();
                if (task.index == 100U) {
                    condition.wait(lock, [&] { return releaseFirstInteractive; });
                }
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 30U;
    scheduler.SetBackgroundConcurrency(2U);
    bool passed = true;
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        passed &= Expect(
            scheduler.Submit(MakeTask(
                generation,
                index,
                DecodePriority::Background)),
            "submit sequence background task");
    }
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return backgroundStarted == 2; }),
        "two sequence background tasks started before scrub");

    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            100U,
            DecodePriority::Current)),
        "submit first sequence latest target");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return startedCurrent.size() == 1U; }),
        "first sequence latest target started");
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            101U,
            DecodePriority::Current)),
        "submit final sequence latest target");

    {
        std::scoped_lock lock(mutex);
        releaseBackground = true;
        releaseFirstInteractive = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] {
            return callbacks.size() == 4U && startedCurrent.size() == 2U;
        }),
        "running background and both started sequence targets completed");
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            backgroundStarted == 2,
            "queued background stayed paused during sequence latest-wins");
        passed &= Expect(
            startedCurrent.size() == 2U
                && startedCurrent[0] == 100U
                && startedCurrent[1] == 101U,
            "sequence latest-wins decoded only running and newest targets");
        passed &= Expect(
            std::find(callbacks.begin(), callbacks.end(), 100U) != callbacks.end()
                && std::find(callbacks.begin(), callbacks.end(), 101U)
                    != callbacks.end(),
            "all started sequence targets entered the cache");
    }

    scheduler.EndLatestWins(generation);
    std::this_thread::sleep_for(30ms);
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            backgroundStarted == 2,
            "EndLatestWins did not resume cold queue before hot fill");
    }

    scheduler.ResumeBackground(generation);
    passed &= Expect(
        WaitUntil(condition, mutex, [&] {
            return backgroundStarted == 4 && callbacks.size() == 6U;
        }),
        "cold queue resumed from preserved entries");
    scheduler.Shutdown();
    return passed;
}

[[nodiscard]] bool TestSequenceLatestWinsReusesRunningBackgroundTarget() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBackground = false;
    int started = 0;
    std::vector<DecodePriority> callbacks;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            ++started;
            condition.notify_all();
            if (task.priority == DecodePriority::Background) {
                condition.wait(lock, [&] { return releaseBackground; });
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 31U;
    constexpr std::uint32_t target = 77U;
    bool passed = Expect(
        scheduler.Submit(MakeTask(
            generation,
            target,
            DecodePriority::Background)),
        "submit running background target");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "background target started before sequence scrub");

    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        !scheduler.SubmitLatest(MakeTask(
            generation,
            target,
            DecodePriority::Current)),
        "running background target was reused without duplicate submit");
    {
        std::scoped_lock lock(mutex);
        releaseBackground = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 1U; }),
        "running background target retained its completion callback");
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    scheduler.Shutdown();
    passed &= Expect(started == 1, "running background target decoded once");
    passed &= Expect(
        callbacks.size() == 1U
            && callbacks.front() == DecodePriority::Background,
        "running background target completed as preserved cold work");
    return passed;
}

[[nodiscard]] bool TestSequenceLatestWinsPromotesQueuedBackgroundTarget() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBlocker = false;
    bool blockerStarted = false;
    std::vector<DecodePriority> targetStarts;
    std::vector<DecodePriority> targetCallbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result || result.task.index != 88U) {
                return;
            }
            std::scoped_lock lock(mutex);
            targetCallbacks.push_back(result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            if (task.index == 1U) {
                blockerStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseBlocker; });
            } else if (task.index == 88U) {
                targetStarts.push_back(task.priority);
                condition.notify_all();
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 32U;
    bool passed = Expect(
        scheduler.Submit(MakeTask(99U, 1U, DecodePriority::Current)),
        "submit blocker before queued cold promotion");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return blockerStarted; }),
        "promotion blocker started");
    passed &= Expect(
        scheduler.Submit(MakeTask(
            generation,
            88U,
            DecodePriority::Background)),
        "queue cold target before sequence scrub");

    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            88U,
            DecodePriority::Current)),
        "promote queued cold target into latest lane");
    {
        std::scoped_lock lock(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return targetCallbacks.size() == 1U; }),
        "promoted cold target completed");
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    scheduler.Shutdown();
    passed &= Expect(
        targetStarts.size() == 1U
            && targetStarts.front() == DecodePriority::Current,
        "queued cold target decoded once at Current priority");
    passed &= Expect(
        targetCallbacks.size() == 1U
            && targetCallbacks.front() == DecodePriority::Current,
        "promoted cold target produced one Current callback");
    return passed;
}

[[nodiscard]] bool TestSupersededPendingPromotionReturnsToColdQueue() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBlocker = false;
    bool blockerStarted = false;
    std::vector<std::pair<std::uint32_t, DecodePriority>> starts;
    std::vector<std::pair<std::uint32_t, DecodePriority>> callbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result || result.task.index == 1U) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.emplace_back(result.task.index, result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            if (task.index == 1U) {
                blockerStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseBlocker; });
            } else {
                starts.emplace_back(task.index, task.priority);
                condition.notify_all();
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 33U;
    bool passed = Expect(
        scheduler.Submit(MakeTask(99U, 1U, DecodePriority::Current)),
        "submit blocker before pending promotion replacement");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return blockerStarted; }),
        "pending promotion blocker started");
    passed &= Expect(
        scheduler.Submit(MakeTask(
            generation,
            88U,
            DecodePriority::Background)),
        "queue background before pending promotion replacement");

    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            88U,
            DecodePriority::Current)),
        "promote queued background before replacement");
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            89U,
            DecodePriority::Current)),
        "replace pending promoted background with final target");
    {
        std::scoped_lock lock(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 1U; }),
        "final target completed while restored cold task stayed paused");
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            callbacks[0].first == 89U
                && callbacks[0].second == DecodePriority::Current,
            "replacement latest target completed first");
        passed &= Expect(
            std::none_of(starts.begin(), starts.end(), [](const auto& value) {
                return value.first == 88U;
            }),
            "superseded pending promotion did not run before cold resume");
    }

    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 2U; }),
        "restored promoted task resumed as cold work");
    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 2U
            && callbacks[1].first == 88U
            && callbacks[1].second == DecodePriority::Background,
        "superseded pending promotion retained original background identity");
    return passed;
}

[[nodiscard]] bool TestSupersededRunningPromotionCompletesBeforeFinalTarget() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBlocker = false;
    bool releasePromoted = false;
    bool blockerStarted = false;
    bool promotedStarted = false;
    std::vector<std::pair<std::uint32_t, DecodePriority>> callbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result || result.task.index == 1U) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.emplace_back(result.task.index, result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            if (task.index == 1U) {
                blockerStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseBlocker; });
            } else if (task.index == 88U) {
                promotedStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releasePromoted; });
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 34U;
    bool passed = Expect(
        scheduler.Submit(MakeTask(99U, 1U, DecodePriority::Current)),
        "submit blocker before running promotion replacement");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return blockerStarted; }),
        "running promotion blocker started");
    passed &= Expect(
        scheduler.Submit(MakeTask(
            generation,
            88U,
            DecodePriority::Background)),
        "queue background before running promotion replacement");
    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            88U,
            DecodePriority::Current)),
        "promote background into running latest lane");
    {
        std::scoped_lock lock(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return promotedStarted; }),
        "promoted background started as Current");
    scheduler.ClearLatestTargetPreservingBackground(generation);
    scheduler.EndLatestWins(generation);
    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            89U,
            DecodePriority::Current)),
        "queue final target behind running promotion");
    {
        std::scoped_lock lock(mutex);
        releasePromoted = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 2U; }),
        "running promotion and final target both completed");
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 2U
            && callbacks[0].first == 88U
            && callbacks[0].second == DecodePriority::Current
            && callbacks[1].first == 89U
            && callbacks[1].second == DecodePriority::Current,
        "running promoted result was retained before newest target");
    return passed;
}

[[nodiscard]] bool TestSequenceStartedTargetsRunInParallelWithOnePendingLatest() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseStarted = false;
    int active = 0;
    int maximumActive = 0;
    std::vector<std::uint32_t> starts;
    std::vector<std::uint32_t> callbacks;

    DecodeScheduler scheduler(
        3,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.index);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            ++active;
            maximumActive = std::max(maximumActive, active);
            starts.push_back(task.index);
            condition.notify_all();
            condition.wait(lock, [&] { return releaseStarted; });
            --active;
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 35U;
    scheduler.BeginLatestWinsPreservingBackground(generation);
    bool passed = true;
    for (const std::uint32_t target : {10U, 20U, 30U}) {
        passed &= Expect(
            scheduler.SubmitLatest(MakeTask(
                generation,
                target,
                DecodePriority::Current)),
            "submit parallel sequence target");
        passed &= Expect(
            WaitUntil(condition, mutex, [&] {
                return starts.size() == static_cast<std::size_t>(target / 10U);
            }),
            "started sequence target uses an available worker");
    }

    for (std::uint32_t target = 40U; target <= 1000U; target += 10U) {
        passed &= Expect(
            scheduler.SubmitLatest(MakeTask(
                generation,
                target,
                DecodePriority::Current)),
            "replace the single pending sequence target");
    }
    passed &= Expect(
        scheduler.PendingTaskCount(DecodePriority::Current) == 4U,
        "sequence mode keeps three running targets and one pending latest");

    {
        std::scoped_lock lock(mutex);
        releaseStarted = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 4U; }),
        "parallel sequence targets and final pending target completed");
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    scheduler.Shutdown();

    passed &= Expect(
        maximumActive == 3,
        "parallel sequence targets remain bounded by worker count");
    passed &= Expect(
        starts.size() == 4U
            && std::find(starts.begin(), starts.end(), 10U) != starts.end()
            && std::find(starts.begin(), starts.end(), 20U) != starts.end()
            && std::find(starts.begin(), starts.end(), 30U) != starts.end()
            && std::find(starts.begin(), starts.end(), 1000U) != starts.end(),
        "only started targets and newest pending sequence target decoded");
    passed &= Expect(
        std::find(callbacks.begin(), callbacks.end(), 1000U)
            != callbacks.end(),
        "final sequence target produced a completion callback");
    return passed;
}

[[nodiscard]] bool TestClearSequenceLatestTargetDetachesStartedWork() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseStarted = false;
    std::vector<std::uint32_t> starts;
    std::vector<std::uint32_t> callbacks;

    DecodeScheduler scheduler(
        2,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.index);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            starts.push_back(task.index);
            condition.notify_all();
            if (task.index != 4U) {
                condition.wait(lock, [&] { return releaseStarted; });
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 36U;
    scheduler.BeginLatestWinsPreservingBackground(generation);
    bool passed = Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            1U,
            DecodePriority::Current)),
        "submit first ordinary target before clear");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return starts.size() == 1U; }),
        "first ordinary target started before clear");
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            2U,
            DecodePriority::Current)),
        "submit second ordinary target before clear");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return starts.size() == 2U; }),
        "second ordinary target started before clear");
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            3U,
            DecodePriority::Current)),
        "queue pending ordinary target before clear");

    scheduler.ClearLatestTargetPreservingBackground(generation);
    passed &= Expect(
        scheduler.PendingTaskCount(DecodePriority::Current) == 2U,
        "clear retains two started targets and removes one pending target");
    passed &= Expect(
        !scheduler.SubmitLatest(MakeTask(
            generation,
            1U,
            DecodePriority::Current)),
        "request for detached running target reuses its in-flight decode");
    {
        std::scoped_lock lock(mutex);
        releaseStarted = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 2U; }),
        "uncached started targets complete after cached target clears latest");
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            std::find(starts.begin(), starts.end(), 3U) == starts.end(),
            "cleared pending ordinary target never started");
    }

    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            4U,
            DecodePriority::Current)),
        "clear preserves the sequence latest-wins mode");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 3U; }),
        "new target completed after clear");
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 3U
            && std::find(callbacks.begin(), callbacks.end(), 1U)
                != callbacks.end()
            && std::find(callbacks.begin(), callbacks.end(), 2U)
                != callbacks.end()
            && callbacks.back() == 4U,
        "started uncached targets and post-clear target all reached cache");
    return passed;
}

[[nodiscard]] bool TestClearRestoresPromotedPendingBackground() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseBlocker = false;
    bool blockerStarted = false;
    std::vector<std::pair<std::uint32_t, DecodePriority>> starts;
    std::vector<std::pair<std::uint32_t, DecodePriority>> callbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result || result.task.index == 1U) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.emplace_back(result.task.index, result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            if (task.index == 1U) {
                blockerStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseBlocker; });
            } else {
                starts.emplace_back(task.index, task.priority);
                condition.notify_all();
            }
            lock.unlock();
            return MakeFrame(task);
        });

    constexpr std::uint64_t generation = 37U;
    bool passed = Expect(
        scheduler.Submit(MakeTask(99U, 1U, DecodePriority::Current)),
        "submit blocker before clearing promoted pending target");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return blockerStarted; }),
        "clear promotion blocker started");
    passed &= Expect(
        scheduler.Submit(MakeTask(
            generation,
            88U,
            DecodePriority::Background)),
        "queue background before clear promotion");
    scheduler.BeginLatestWinsPreservingBackground(generation);
    passed &= Expect(
        scheduler.SubmitLatest(MakeTask(
            generation,
            88U,
            DecodePriority::Current)),
        "promote queued background before clear");
    scheduler.ClearLatestTargetPreservingBackground(generation);
    {
        std::scoped_lock lock(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    std::this_thread::sleep_for(30ms);
    {
        std::scoped_lock lock(mutex);
        passed &= Expect(
            starts.empty() && callbacks.empty(),
            "cleared promoted pending target returned to suspended cold queue");
    }
    scheduler.EndLatestWins(generation);
    scheduler.ResumeBackground(generation);
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 1U; }),
        "cleared promoted pending target resumed as background");
    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 1U
            && callbacks[0].first == 88U
            && callbacks[0].second == DecodePriority::Background,
        "clear retained promoted target background identity");
    return passed;
}

[[nodiscard]] bool TestCancelBackgroundPreservesInteractiveWork() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseCurrent = false;
    bool currentStarted = false;
    std::vector<std::uint32_t> callbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.index);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            if (task.index == 0U) {
                std::unique_lock lock(mutex);
                currentStarted = true;
                condition.notify_all();
                condition.wait(lock, [&] { return releaseCurrent; });
            }
            return MakeFrame(task);
        });

    bool passed = Expect(
        scheduler.Submit(MakeTask(31U, 0U, DecodePriority::Current)),
        "submit blocking current task before background cancellation");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return currentStarted; }),
        "blocking current task started");
    passed &= Expect(
        scheduler.Submit(MakeTask(31U, 1U, DecodePriority::Forward)),
        "queue forward task before background cancellation");
    passed &= Expect(
        scheduler.Submit(MakeTask(31U, 2U, DecodePriority::Background)),
        "queue first cold background task");
    passed &= Expect(
        scheduler.Submit(MakeTask(31U, 3U, DecodePriority::Background)),
        "queue second cold background task");

    scheduler.CancelBackground(31U);
    passed &= Expect(
        scheduler.PendingTaskCount(DecodePriority::Background) == 0U,
        "background cancellation removes only cold queued work");
    passed &= Expect(
        scheduler.PendingTaskCount(DecodePriority::Forward) == 1U,
        "background cancellation preserves forward hot work");

    {
        std::scoped_lock lock(mutex);
        releaseCurrent = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return callbacks.size() == 2U; }),
        "current and forward work complete after cold cancellation");

    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 2U && callbacks[0] == 0U && callbacks[1] == 1U,
        "cancelled background tasks never reach completion callbacks");
    return passed;
}

[[nodiscard]] bool TestCancelledRunningBackgroundCanBeRequeuedImmediately() {
    std::mutex mutex;
    std::condition_variable condition;
    bool releaseFirstDecode = false;
    int started = 0;
    std::vector<DecodePriority> callbacks;

    DecodeScheduler scheduler(
        1,
        [&](ScheduledDecodeResult result) {
            if (!result) {
                return;
            }
            std::scoped_lock lock(mutex);
            callbacks.push_back(result.task.priority);
            condition.notify_all();
        },
        [&](const DecodeTask& task, std::string&, const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            ++started;
            condition.notify_all();
            if (started == 1) {
                condition.wait(lock, [&] { return releaseFirstDecode; });
            }
            lock.unlock();
            return MakeFrame(task);
        });

    const DecodeTask background =
        MakeTask(32U, 7U, DecodePriority::Background);
    DecodeTask replacement = background;
    replacement.priority = DecodePriority::Current;
    replacement.sortRank = 0U;

    bool passed = Expect(
        scheduler.Submit(background),
        "submit running background task before fast foreground restore");
    passed &= Expect(
        WaitUntil(condition, mutex, [&] { return started == 1; }),
        "background task started before cancellation");

    scheduler.CancelBackground(32U);
    passed &= Expect(
        scheduler.Submit(replacement),
        "cancelled running background key accepts immediate current replacement");

    {
        std::scoped_lock lock(mutex);
        releaseFirstDecode = true;
    }
    condition.notify_all();
    passed &= Expect(
        WaitUntil(condition, mutex, [&] {
            return started == 2 && callbacks.size() == 1U;
        }),
        "replacement completes without waiting for another scheduling event");

    scheduler.Shutdown();
    passed &= Expect(
        callbacks.size() == 1U &&
            callbacks.front() == DecodePriority::Current,
        "cancelled cold result is suppressed and only replacement completes");
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= TestRunningInteractiveTaskIsNotDuplicated();
    passed &= TestCancelledGenerationDropsResultAndKeepsDeduplication();
    passed &= TestCancelledGenerationCanQueueLatestReplacement();
    passed &= TestBackgroundLeavesInteractiveWorkerAvailable();
    passed &= TestBackgroundProgressesDuringContinuousInteractiveWork();
    passed &= TestFourToOneTransitionKeepsCurrentResponsive();
    passed &= TestSingleWorkerPrefersInteractiveBeforeBackground();
    passed &= TestLatestWinsKeepsOnlyNewestPendingTask();
    passed &= TestLatestWinsRejectsPrefetchAndEndPreservesFinalTarget();
    passed &= TestLatestWinsCancellationClearsPendingAndMode();
    passed &= TestLatestWinsCancelAllExceptAndShutdown();
    passed &= TestSequenceLatestWinsPausesAndResumesBackground();
    passed &= TestSequenceLatestWinsReusesRunningBackgroundTarget();
    passed &= TestSequenceLatestWinsPromotesQueuedBackgroundTarget();
    passed &= TestSupersededPendingPromotionReturnsToColdQueue();
    passed &= TestSupersededRunningPromotionCompletesBeforeFinalTarget();
    passed &= TestSequenceStartedTargetsRunInParallelWithOnePendingLatest();
    passed &= TestClearSequenceLatestTargetDetachesStartedWork();
    passed &= TestClearRestoresPromotedPendingBackground();
    passed &= TestCancelBackgroundPreservesInteractiveWork();
    passed &= TestCancelledRunningBackgroundCanBeRequeuedImmediately();
    return passed ? 0 : 1;
}
