#pragma once

#include "Core/PlayerTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace zt::sequence {

// Thread-safe byte-accounted LRU. References held by the renderer may outlive an
// eviction, while the cache budget accounts only entries retained by this cache.
class MemoryFrameCache final {
public:
    explicit MemoryFrameCache(std::uint64_t capacityBytes = 0);
    ~MemoryFrameCache();

    MemoryFrameCache(const MemoryFrameCache&) = delete;
    MemoryFrameCache& operator=(const MemoryFrameCache&) = delete;

    void SetCapacityBytes(std::uint64_t capacityBytes);
    void SetCapacityBytesRetainingNeighborhood(
        std::uint64_t capacityBytes,
        Generation generation,
        FrameIndex anchorIndex,
        int preferredDirection,
        PlaybackRange retentionRange,
        bool loopPlayback);
    [[nodiscard]] std::uint64_t CapacityBytes() const noexcept;
    [[nodiscard]] std::uint64_t SizeBytes() const noexcept;
    [[nodiscard]] std::size_t Count() const noexcept;
    [[nodiscard]] std::size_t Count(Generation generation) const noexcept;

    [[nodiscard]] std::shared_ptr<const DecodedFrame> Get(
        Generation generation,
        FrameIndex index);
    [[nodiscard]] bool Contains(Generation generation, FrameIndex index) const;
    [[nodiscard]] bool Put(std::shared_ptr<const DecodedFrame> frame);

    // Includes startIndex and stops at the first cache miss. maxFrames is also
    // capped to totalFrames, preventing an endless loop for looping sequences.
    [[nodiscard]] std::size_t CountContiguous(
        Generation generation,
        FrameIndex startIndex,
        int direction,
        std::size_t totalFrames,
        bool loopPlayback,
        std::size_t maxFrames) const;
    [[nodiscard]] std::size_t CountContiguousInRange(
        Generation generation,
        FrameIndex startIndex,
        int direction,
        PlaybackRange range,
        bool loopPlayback,
        std::size_t maxFrames) const;

    void RemoveGeneration(Generation generation);
    void Clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence
