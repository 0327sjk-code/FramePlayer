#include "Cache/MemoryFrameCache.h"

#include <algorithm>
#include <list>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zt::sequence {
namespace {

struct CacheKey {
    Generation generation = 0;
    FrameIndex index = 0;

    [[nodiscard]] bool operator==(const CacheKey&) const noexcept = default;
};

struct CacheKeyHash {
    [[nodiscard]] std::size_t operator()(const CacheKey& key) const noexcept {
        const std::size_t generationHash = std::hash<Generation>{}(key.generation);
        const std::size_t indexHash = std::hash<FrameIndex>{}(key.index);
        return generationHash ^ (indexHash + 0x9e3779b9U + (generationHash << 6U)
            + (generationHash >> 2U));
    }
};

}  // namespace

class MemoryFrameCache::Impl final {
public:
    explicit Impl(const std::uint64_t capacityBytes)
        : capacityBytes_(capacityBytes) {}

    void SetCapacityBytes(const std::uint64_t capacityBytes) {
        std::scoped_lock lock(mutex_);
        capacityBytes_ = capacityBytes;
        EvictToBudget();
    }

    void SetCapacityBytesRetainingNeighborhood(
        const std::uint64_t capacityBytes,
        const Generation generation,
        const FrameIndex anchorIndex,
        const int preferredDirection,
        const PlaybackRange retentionRange,
        const bool loopPlayback) {
        std::scoped_lock lock(mutex_);
        if (capacityBytes >= capacityBytes_) {
            capacityBytes_ = capacityBytes;
            return;
        }

        capacityBytes_ = capacityBytes;
        if (entries_.empty() || retentionRange.endFrame < retentionRange.startFrame ||
            anchorIndex < retentionRange.startFrame ||
            anchorIndex > retentionRange.endFrame) {
            EvictToBudget();
            return;
        }

        constexpr std::uint64_t kOppositeDirectionWeight = 4ULL;
        constexpr std::uint64_t kUnavailableDistance =
            std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t rangeFrames =
            static_cast<std::uint64_t>(retentionRange.endFrame) -
            static_cast<std::uint64_t>(retentionRange.startFrame) + 1ULL;

        const auto distanceAlong = [retentionRange,
                                    anchorIndex,
                                    loopPlayback,
                                    rangeFrames](
                                       const FrameIndex index,
                                       const int direction) noexcept {
            if (index < retentionRange.startFrame ||
                index > retentionRange.endFrame) {
                return kUnavailableDistance;
            }

            if (direction >= 0) {
                if (index >= anchorIndex) {
                    return static_cast<std::uint64_t>(index) - anchorIndex;
                }
                if (loopPlayback) {
                    return rangeFrames -
                        (static_cast<std::uint64_t>(anchorIndex) - index);
                }
                return kUnavailableDistance;
            }

            if (index <= anchorIndex) {
                return static_cast<std::uint64_t>(anchorIndex) - index;
            }
            if (loopPlayback) {
                return rangeFrames -
                    (static_cast<std::uint64_t>(index) - anchorIndex);
            }
            return kUnavailableDistance;
        };

        const auto weightedOppositeDistance = [](const std::uint64_t distance) noexcept {
            if (distance == kUnavailableDistance ||
                distance > kUnavailableDistance / kOppositeDirectionWeight) {
                return kUnavailableDistance;
            }
            return distance * kOppositeDirectionWeight;
        };

        struct Candidate final {
            CacheKey key;
            std::uint64_t rank = kUnavailableDistance;
            std::uint64_t distance = kUnavailableDistance;
            std::uint64_t byteSize = 0;
            bool preferred = false;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(entries_.size());
        const int normalizedDirection = preferredDirection < 0 ? -1 : 1;
        for (const auto& [key, entry] : entries_) {
            if (key.generation != generation) {
                continue;
            }
            const std::uint64_t preferredDistance = distanceAlong(
                key.index,
                normalizedDirection);
            const std::uint64_t oppositeDistance = distanceAlong(
                key.index,
                -normalizedDirection);
            const std::uint64_t oppositeRank =
                weightedOppositeDistance(oppositeDistance);
            const bool retainAsPreferred = preferredDistance <= oppositeRank;
            const std::uint64_t rank = retainAsPreferred
                ? preferredDistance
                : oppositeRank;
            if (rank == kUnavailableDistance) {
                continue;
            }
            candidates.push_back(Candidate{
                key,
                rank,
                retainAsPreferred ? preferredDistance : oppositeDistance,
                entry.byteSize,
                retainAsPreferred});
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& left, const Candidate& right) noexcept {
                if (left.rank != right.rank) {
                    return left.rank < right.rank;
                }
                if (left.preferred != right.preferred) {
                    return left.preferred;
                }
                if (left.distance != right.distance) {
                    return left.distance < right.distance;
                }
                return left.key.index < right.key.index;
            });

        std::unordered_set<CacheKey, CacheKeyHash> retained;
        retained.reserve(candidates.size());
        std::uint64_t retainedBytes = 0;
        for (const Candidate& candidate : candidates) {
            if (candidate.byteSize > capacityBytes_ - retainedBytes) {
                continue;
            }
            retained.insert(candidate.key);
            retainedBytes += candidate.byteSize;
        }

        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (retained.contains(iterator->first)) {
                ++iterator;
                continue;
            }
            iterator = EraseEntry(iterator);
        }
        EvictToBudget();
    }

    [[nodiscard]] std::uint64_t CapacityBytes() const noexcept {
        std::scoped_lock lock(mutex_);
        return capacityBytes_;
    }

    [[nodiscard]] std::uint64_t SizeBytes() const noexcept {
        std::scoped_lock lock(mutex_);
        return sizeBytes_;
    }

    [[nodiscard]] std::size_t Count() const noexcept {
        std::scoped_lock lock(mutex_);
        return entries_.size();
    }

    [[nodiscard]] std::size_t Count(const Generation generation) const noexcept {
        std::scoped_lock lock(mutex_);
        const auto found = generationCounts_.find(generation);
        return found == generationCounts_.end() ? 0U : found->second;
    }

    [[nodiscard]] std::shared_ptr<const DecodedFrame> Get(
        const Generation generation,
        const FrameIndex index) {
        std::scoped_lock lock(mutex_);
        const auto found = entries_.find(CacheKey{generation, index});
        if (found == entries_.end()) {
            return {};
        }

        lru_.splice(lru_.begin(), lru_, found->second.lruPosition);
        found->second.lruPosition = lru_.begin();
        return found->second.frame;
    }

    [[nodiscard]] bool Contains(
        const Generation generation,
        const FrameIndex index) const {
        std::scoped_lock lock(mutex_);
        return entries_.contains(CacheKey{generation, index});
    }

    [[nodiscard]] bool Put(std::shared_ptr<const DecodedFrame> frame) {
        if (!frame) {
            return false;
        }

        const std::uint64_t frameBytes = static_cast<std::uint64_t>(frame->ByteSize());
        const CacheKey key{frame->generation, frame->index};
        std::scoped_lock lock(mutex_);

        const auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            EraseEntry(existing);
        }

        if (capacityBytes_ == 0 || frameBytes > capacityBytes_) {
            return false;
        }

        lru_.push_front(key);
        Entry entry;
        entry.frame = std::move(frame);
        entry.byteSize = frameBytes;
        entry.lruPosition = lru_.begin();
        entries_.emplace(key, std::move(entry));
        sizeBytes_ += frameBytes;
        ++generationCounts_[key.generation];
        EvictToBudget();
        return entries_.contains(key);
    }

    [[nodiscard]] std::size_t CountContiguous(
        const Generation generation,
        const FrameIndex startIndex,
        const int direction,
        const std::size_t totalFrames,
        const bool loopPlayback,
        const std::size_t maxFrames) const {
        if (totalFrames == 0 ||
            totalFrames - 1U > std::numeric_limits<FrameIndex>::max()) {
            return 0;
        }

        return CountContiguousInRange(
            generation,
            startIndex,
            direction,
            PlaybackRange{
                0U,
                static_cast<FrameIndex>(totalFrames - 1U)},
            loopPlayback,
            maxFrames);
    }

    [[nodiscard]] std::size_t CountContiguousInRange(
        const Generation generation,
        const FrameIndex startIndex,
        const int direction,
        const PlaybackRange range,
        const bool loopPlayback,
        const std::size_t maxFrames) const {
        if (range.endFrame < range.startFrame ||
            startIndex < range.startFrame || startIndex > range.endFrame ||
            maxFrames == 0) {
            return 0;
        }

        const std::uint64_t rangeFrameCount =
            static_cast<std::uint64_t>(range.endFrame) -
            static_cast<std::uint64_t>(range.startFrame) + 1ULL;
        const std::size_t boundedMaximum = static_cast<std::size_t>(
            std::min<std::uint64_t>(maxFrames, rangeFrameCount));
        std::int64_t current = static_cast<std::int64_t>(startIndex);
        const std::int64_t minimum =
            static_cast<std::int64_t>(range.startFrame);
        const std::int64_t maximum =
            static_cast<std::int64_t>(range.endFrame);
        const std::int64_t step = direction < 0 ? -1 : 1;
        std::size_t count = 0;

        std::scoped_lock lock(mutex_);
        while (count < boundedMaximum) {
            if (!entries_.contains(CacheKey{generation, static_cast<FrameIndex>(current)})) {
                break;
            }
            ++count;
            current += step;
            if (current < minimum || current > maximum) {
                if (!loopPlayback) {
                    break;
                }
                current = current < minimum ? maximum : minimum;
            }
        }
        return count;
    }

    void RemoveGeneration(const Generation generation) {
        std::scoped_lock lock(mutex_);
        for (auto iterator = entries_.begin(); iterator != entries_.end();) {
            if (iterator->first.generation != generation) {
                ++iterator;
                continue;
            }
            iterator = EraseEntry(iterator);
        }
    }

    void Clear() {
        std::scoped_lock lock(mutex_);
        entries_.clear();
        lru_.clear();
        generationCounts_.clear();
        sizeBytes_ = 0;
    }

private:
    struct Entry {
        std::shared_ptr<const DecodedFrame> frame;
        std::uint64_t byteSize = 0;
        std::list<CacheKey>::iterator lruPosition;
    };

    using EntryMap = std::unordered_map<CacheKey, Entry, CacheKeyHash>;

    EntryMap::iterator EraseEntry(
        const EntryMap::iterator iterator) noexcept {
        sizeBytes_ -= iterator->second.byteSize;
        lru_.erase(iterator->second.lruPosition);

        const auto generationCount = generationCounts_.find(
            iterator->first.generation);
        if (generationCount != generationCounts_.end()) {
            if (generationCount->second <= 1U) {
                generationCounts_.erase(generationCount);
            } else {
                --generationCount->second;
            }
        }

        return entries_.erase(iterator);
    }

    void EvictToBudget() {
        while (sizeBytes_ > capacityBytes_ && !lru_.empty()) {
            const CacheKey key = lru_.back();
            const auto found = entries_.find(key);
            if (found != entries_.end()) {
                EraseEntry(found);
            } else {
                lru_.pop_back();
            }
        }
    }

    mutable std::mutex mutex_;
    std::uint64_t capacityBytes_ = 0;
    std::uint64_t sizeBytes_ = 0;
    std::list<CacheKey> lru_;
    EntryMap entries_;
    std::unordered_map<Generation, std::size_t> generationCounts_;
};

MemoryFrameCache::MemoryFrameCache(const std::uint64_t capacityBytes)
    : impl_(std::make_unique<Impl>(capacityBytes)) {}

MemoryFrameCache::~MemoryFrameCache() = default;

void MemoryFrameCache::SetCapacityBytes(const std::uint64_t capacityBytes) {
    impl_->SetCapacityBytes(capacityBytes);
}

void MemoryFrameCache::SetCapacityBytesRetainingNeighborhood(
    const std::uint64_t capacityBytes,
    const Generation generation,
    const FrameIndex anchorIndex,
    const int preferredDirection,
    const PlaybackRange retentionRange,
    const bool loopPlayback) {
    impl_->SetCapacityBytesRetainingNeighborhood(
        capacityBytes,
        generation,
        anchorIndex,
        preferredDirection,
        retentionRange,
        loopPlayback);
}

std::uint64_t MemoryFrameCache::CapacityBytes() const noexcept {
    return impl_->CapacityBytes();
}

std::uint64_t MemoryFrameCache::SizeBytes() const noexcept {
    return impl_->SizeBytes();
}

std::size_t MemoryFrameCache::Count() const noexcept {
    return impl_->Count();
}

std::size_t MemoryFrameCache::Count(const Generation generation) const noexcept {
    return impl_->Count(generation);
}

std::shared_ptr<const DecodedFrame> MemoryFrameCache::Get(
    const Generation generation,
    const FrameIndex index) {
    return impl_->Get(generation, index);
}

bool MemoryFrameCache::Contains(
    const Generation generation,
    const FrameIndex index) const {
    return impl_->Contains(generation, index);
}

bool MemoryFrameCache::Put(std::shared_ptr<const DecodedFrame> frame) {
    return impl_->Put(std::move(frame));
}

std::size_t MemoryFrameCache::CountContiguous(
    const Generation generation,
    const FrameIndex startIndex,
    const int direction,
    const std::size_t totalFrames,
    const bool loopPlayback,
    const std::size_t maxFrames) const {
    return impl_->CountContiguous(
        generation,
        startIndex,
        direction,
        totalFrames,
        loopPlayback,
        maxFrames);
}

std::size_t MemoryFrameCache::CountContiguousInRange(
    const Generation generation,
    const FrameIndex startIndex,
    const int direction,
    const PlaybackRange range,
    const bool loopPlayback,
    const std::size_t maxFrames) const {
    return impl_->CountContiguousInRange(
        generation,
        startIndex,
        direction,
        range,
        loopPlayback,
        maxFrames);
}

void MemoryFrameCache::RemoveGeneration(const Generation generation) {
    impl_->RemoveGeneration(generation);
}

void MemoryFrameCache::Clear() {
    impl_->Clear();
}

}  // namespace zt::sequence
