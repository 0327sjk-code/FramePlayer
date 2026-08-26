#pragma once

#include "Core/PlayerTypes.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace zt::sequence {

enum class DecodePriority : std::uint8_t {
    Current = 0,      // P0: exact requested frame
    Forward = 1,      // P1: playback direction deadline window
    Neighborhood = 2, // P2: short reverse/scrub neighborhood
    Background = 3,   // P3: slow full-sequence RAM fill
};

struct DecodeTask {
    FrameFile file;
    FrameIndex index = 0;
    Generation generation = 0;
    std::uint32_t decodePercent = kDefaultDecodePercent;
    DecodePriority priority = DecodePriority::Background;
    std::uint64_t sortRank = 0;
};

struct ScheduledDecodeResult {
    DecodeTask task;
    std::shared_ptr<DecodedFrame> frame;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return frame != nullptr && errorUtf8.empty();
    }
};

class DecodeScheduler final {
public:
    using Completion = std::function<void(ScheduledDecodeResult)>;
    using DecodeFunction = std::function<std::shared_ptr<DecodedFrame>(
        const DecodeTask&,
        std::string& errorUtf8,
        const std::atomic_bool& cancelled)>;

    DecodeScheduler(
        std::size_t workerCount,
        Completion completion,
        DecodeFunction decodeFunction = {});
    ~DecodeScheduler();

    DecodeScheduler(const DecodeScheduler&) = delete;
    DecodeScheduler& operator=(const DecodeScheduler&) = delete;

    // Returns true when a new task was queued or an existing queued task was
    // promoted. The same generation/index/percent is decoded at most once.
    [[nodiscard]] bool Submit(DecodeTask task);

    // Enables a bounded latest-wins lane for interactive seeking. While active,
    // Current tasks for this generation are routed through a single pending
    // slot and non-Current tasks for the generation are rejected.
    void BeginLatestWins(Generation generation);
    // Sequence scrubbing keeps already-running background work alive and
    // pauses queued background work in place. ResumeBackground must be called
    // explicitly after the post-scrub hot window is ready.
    void BeginLatestWinsPreservingBackground(Generation generation);
    [[nodiscard]] bool SubmitLatest(DecodeTask task);
    // Clears obsolete sequence scrub targets without leaving the preserving
    // latest-wins mode or resuming its suspended background queue.
    void ClearLatestTargetPreservingBackground(Generation generation);
    void EndLatestWins(Generation generation);
    void ResumeBackground(Generation generation);

    void CancelGeneration(Generation generation);
    // Cancels a stateful decode generation and immediately releases its task
    // keys so a latest-wins replacement can be queued while an old task exits.
    void CancelGenerationForReplacement(Generation generation);
    void CancelAllExcept(Generation generationToKeep);
    void CancelCurrent(Generation generation);
    void CancelInteractive(Generation generation);
    void CancelBackground(Generation generation);
    void SetBackgroundConcurrency(std::size_t maximumConcurrentTasks);

    [[nodiscard]] std::size_t PendingTaskCount() const;
    [[nodiscard]] std::size_t PendingTaskCount(DecodePriority priority) const;
    void Shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence
