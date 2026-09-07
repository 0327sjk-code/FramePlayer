#include "Cache/MemoryFrameCache.h"
#include "App/ApplicationActivityPolicy.h"
#include "Core/ComparisonPlayerPolicy.h"
#include "Core/PlayerEnginePolicy.h"
#include "Core/SequenceScanner.h"
#include "Export/ExportNaming.h"
#include "Platform/DroppedSource.h"
#include "Platform/Win32Window.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using zt::sequence::DecodedFrame;
using zt::sequence::FrameIndex;
using zt::sequence::Generation;
using zt::sequence::MemoryFrameCache;

[[nodiscard]] std::shared_ptr<const DecodedFrame> MakeFrame(
    const Generation generation,
    const FrameIndex index,
    const std::size_t bytes) {
    auto frame = std::make_shared<DecodedFrame>();
    frame->generation = generation;
    frame->index = index;
    frame->width = static_cast<std::uint32_t>(bytes / 4U);
    frame->height = 1;
    frame->strideBytes = frame->width * 4U;
    frame->bgraPixels.resize(bytes);
    return frame;
}

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    using zt::sequence::DroppedSourceKind;
    using zt::sequence::NaturalPathLess;
    using zt::sequence::WindowClientPoint;
    using zt::sequence::WindowDropEvent;
    using zt::sequence::dropped_source::ClassifyExistingPath;
    using zt::sequence::dropped_source::EntryKind;
    using zt::sequence::detail::AllowsBackgroundDecode;
    using zt::sequence::detail::CacheCapacityForTotalLimit;
    using zt::sequence::detail::CanRetainDecodedFrame;
    using zt::sequence::detail::ClampFrameToPlaybackRange;
    using zt::sequence::detail::FullPlaybackRange;
    using zt::sequence::detail::NormalizePlaybackRange;
    using zt::sequence::detail::OffsetFrameInPlaybackRange;
    using zt::sequence::detail::PlaybackRangeForLoadedSequence;
    using zt::sequence::detail::PlaybackStartForPlay;
    using zt::sequence::detail::PercentageOfSize;
    using zt::sequence::detail::ShouldPresentScrubIntermediateFrame;
    using zt::sequence::detail::ShouldRefreshSnapshotTelemetry;
    using zt::sequence::app_detail::ApplicationActivityPolicy;
    using zt::sequence::app_detail::BackgroundModeTransition;
    using zt::sequence::exporting::BuildMp4ExportCandidate;
    using zt::sequence::exporting::FindAvailableMp4ExportPath;

    bool passed = true;

    const std::filesystem::path droppedSequenceDirectory =
        std::filesystem::path(L"F:\\Render\\Sequence");
    const auto directorySource = ClassifyExistingPath(
        droppedSequenceDirectory,
        EntryKind::Directory);
    passed &= Expect(
        directorySource.has_value() &&
            directorySource->kind == DroppedSourceKind::PngSequence &&
            directorySource->path == droppedSequenceDirectory,
        "dropped directory resolves to a PNG sequence source");

    const std::filesystem::path droppedPng =
        droppedSequenceDirectory / L"MainSeq.0516.PNG";
    const auto pngSource = ClassifyExistingPath(droppedPng, EntryKind::RegularFile);
    passed &= Expect(
        pngSource.has_value() &&
            pngSource->kind == DroppedSourceKind::PngSequence &&
            pngSource->path == droppedSequenceDirectory,
        "dropped PNG resolves to its containing sequence directory");

    const std::vector<std::wstring> supportedVideoExtensions{
        L".mp4",
        L".MOV",
        L".m4v",
        L".WMV",
        L".avi",
        L".MKV",
        L".webm",
    };
    for (const std::wstring& extension : supportedVideoExtensions) {
        const std::filesystem::path droppedVideo =
            droppedSequenceDirectory / (std::wstring(L"preview") + extension);
        const auto videoSource = ClassifyExistingPath(
            droppedVideo,
            EntryKind::RegularFile);
        passed &= Expect(
            videoSource.has_value() &&
                videoSource->kind == DroppedSourceKind::Video &&
                videoSource->path == droppedVideo,
            "supported dropped video keeps its original file path");
    }

    const auto unsupportedSource = ClassifyExistingPath(
        droppedSequenceDirectory / L"notes.txt",
        EntryKind::RegularFile);
    passed &= Expect(
        !unsupportedSource.has_value(),
        "unsupported dropped file type is ignored");

    const auto videoNamedDirectory = ClassifyExistingPath(
        droppedSequenceDirectory / L"folder.mp4",
        EntryKind::Directory);
    passed &= Expect(
        videoNamedDirectory.has_value() &&
            videoNamedDirectory->kind == DroppedSourceKind::PngSequence,
        "filesystem entry kind takes priority over a directory extension");

    const WindowDropEvent positionedDrop{
        zt::sequence::DroppedSource{
            droppedSequenceDirectory,
            DroppedSourceKind::PngSequence},
        WindowClientPoint{384, 216}};
    passed &= Expect(
        positionedDrop.source.path == droppedSequenceDirectory &&
            positionedDrop.source.kind == DroppedSourceKind::PngSequence &&
            positionedDrop.clientPoint.x == 384 &&
            positionedDrop.clientPoint.y == 216,
        "window drop event retains its classified source and client point");

    std::vector<std::wstring> names{
        L"MainSeq.10.png",
        L"MainSeq.2.png",
        L"MainSeq.1.png",
        L"MainSeq.00000000000000000000000020.png",
        L"MainSeq.00000000000000000000000003.png",
    };
    std::stable_sort(names.begin(), names.end(), NaturalPathLess);
    passed &= Expect(names[0] == L"MainSeq.1.png", "natural sort: frame 1");
    passed &= Expect(names[1] == L"MainSeq.2.png", "natural sort: frame 2");
    passed &= Expect(
        names[2] == L"MainSeq.00000000000000000000000003.png",
        "natural sort: long frame 3");
    passed &= Expect(names[3] == L"MainSeq.10.png", "natural sort: frame 10");
    passed &= Expect(
        names[4] == L"MainSeq.00000000000000000000000020.png",
        "natural sort: long frame 20");

    MemoryFrameCache cache(16);
    passed &= Expect(cache.Put(MakeFrame(1, 0, 8)), "cache put frame 0");
    passed &= Expect(cache.Put(MakeFrame(1, 1, 8)), "cache put frame 1");
    passed &= Expect(cache.Get(1, 0) != nullptr, "cache get frame 0");
    passed &= Expect(cache.Put(MakeFrame(1, 2, 8)), "cache put frame 2");
    passed &= Expect(cache.Contains(1, 0), "LRU retained recently used frame");
    passed &= Expect(!cache.Contains(1, 1), "LRU evicted least recently used frame");
    passed &= Expect(cache.Contains(1, 2), "LRU retained inserted frame");
    passed &= Expect(cache.SizeBytes() <= cache.CapacityBytes(), "cache byte budget");

    cache.Clear();
    passed &= Expect(cache.Put(MakeFrame(1, 0, 8)), "generation 1 put");
    passed &= Expect(cache.Put(MakeFrame(2, 0, 8)), "generation 2 put");
    cache.RemoveGeneration(1);
    passed &= Expect(!cache.Contains(1, 0), "generation 1 removed");
    passed &= Expect(cache.Contains(2, 0), "generation 2 retained");

    MemoryFrameCache generationCountCache(24U);
    passed &= Expect(
        generationCountCache.Count() == 0U &&
            generationCountCache.Count(100U) == 0U,
        "empty cache has zero total and generation counts");
    passed &= Expect(
        generationCountCache.Put(MakeFrame(100U, 0U, 8U)) &&
            generationCountCache.Put(MakeFrame(100U, 1U, 8U)) &&
            generationCountCache.Put(MakeFrame(200U, 0U, 8U)),
        "populate multiple cache generations");
    passed &= Expect(
        generationCountCache.Count() == 3U &&
            generationCountCache.Count(100U) == 2U &&
            generationCountCache.Count(200U) == 1U,
        "per-generation counts track successful inserts");
    passed &= Expect(
        generationCountCache.Put(MakeFrame(100U, 1U, 8U)) &&
            generationCountCache.Count() == 3U &&
            generationCountCache.Count(100U) == 2U,
        "replacing a cache key preserves its generation count");
    passed &= Expect(
        generationCountCache.Put(MakeFrame(300U, 0U, 8U)) &&
            generationCountCache.Count() == 3U &&
            generationCountCache.Count(100U) == 1U &&
            generationCountCache.Count(200U) == 1U &&
            generationCountCache.Count(300U) == 1U,
        "LRU eviction decrements the evicted generation count");
    generationCountCache.SetCapacityBytes(8U);
    passed &= Expect(
        generationCountCache.Count() == 1U &&
            generationCountCache.Count(100U) == 0U &&
            generationCountCache.Count(200U) == 0U &&
            generationCountCache.Count(300U) == 1U,
        "capacity eviction keeps generation counts synchronized");
    passed &= Expect(
        !generationCountCache.Put(MakeFrame(300U, 0U, 16U)) &&
            generationCountCache.Count() == 0U &&
            generationCountCache.Count(300U) == 0U,
        "failed oversized replacement removes its previous generation count");
    passed &= Expect(
        generationCountCache.Put(MakeFrame(400U, 0U, 8U)),
        "generation count cache accepts a frame after failed replacement");
    generationCountCache.RemoveGeneration(400U);
    passed &= Expect(
        generationCountCache.Count() == 0U &&
            generationCountCache.Count(400U) == 0U,
        "generation removal clears its cached count");
    passed &= Expect(
        generationCountCache.Put(MakeFrame(500U, 0U, 8U)),
        "generation count cache repopulates before clear");
    generationCountCache.Clear();
    passed &= Expect(
        generationCountCache.Count() == 0U &&
            generationCountCache.Count(500U) == 0U,
        "cache clear resets all generation counts");

    MemoryFrameCache neighborhoodCache(88U);
    std::shared_ptr<const DecodedFrame> externallyHeldFrame;
    passed &= Expect(
        neighborhoodCache.Put(MakeFrame(8U, 0U, 8U)),
        "populate unrelated cache generation before neighborhood trim");
    for (FrameIndex index = 0U; index < 10U; ++index) {
        passed &= Expect(
            neighborhoodCache.Put(MakeFrame(9U, index, 8U)),
            "populate neighborhood cache");
    }
    externallyHeldFrame = neighborhoodCache.Get(9U, 0U);
    neighborhoodCache.SetCapacityBytesRetainingNeighborhood(
        32U,
        9U,
        5U,
        1,
        {0U, 9U},
        false);
    passed &= Expect(
        neighborhoodCache.Count(9U) == 4U &&
            neighborhoodCache.Count(8U) == 0U &&
            neighborhoodCache.Count() == 4U &&
            neighborhoodCache.Contains(9U, 5U) &&
            neighborhoodCache.Contains(9U, 6U) &&
            neighborhoodCache.Contains(9U, 7U) &&
            neighborhoodCache.Contains(9U, 8U),
        "neighborhood trim synchronizes counts and retains preferred frames");
    passed &= Expect(
        !neighborhoodCache.Contains(9U, 0U) &&
            externallyHeldFrame != nullptr && externallyHeldFrame->index == 0U,
        "cache eviction preserves an externally held display frame");

    MemoryFrameCache reverseNeighborhoodCache(80U);
    for (FrameIndex index = 0U; index < 10U; ++index) {
        passed &= Expect(
            reverseNeighborhoodCache.Put(MakeFrame(10U, index, 8U)),
            "populate reverse neighborhood cache");
    }
    reverseNeighborhoodCache.SetCapacityBytesRetainingNeighborhood(
        32U,
        10U,
        5U,
        -1,
        {0U, 9U},
        false);
    passed &= Expect(
        reverseNeighborhoodCache.Contains(10U, 5U) &&
            reverseNeighborhoodCache.Contains(10U, 4U) &&
            reverseNeighborhoodCache.Contains(10U, 3U) &&
            reverseNeighborhoodCache.Contains(10U, 2U),
        "reverse neighborhood trim follows the playback direction");

    MemoryFrameCache loopingNeighborhoodCache(80U);
    for (FrameIndex index = 0U; index < 10U; ++index) {
        passed &= Expect(
            loopingNeighborhoodCache.Put(MakeFrame(11U, index, 8U)),
            "populate looping neighborhood cache");
    }
    loopingNeighborhoodCache.SetCapacityBytesRetainingNeighborhood(
        32U,
        11U,
        9U,
        1,
        {0U, 9U},
        true);
    passed &= Expect(
        loopingNeighborhoodCache.Contains(11U, 9U) &&
            loopingNeighborhoodCache.Contains(11U, 0U) &&
            loopingNeighborhoodCache.Contains(11U, 1U) &&
            loopingNeighborhoodCache.Contains(11U, 2U),
        "looping neighborhood trim wraps around the playback range");

    constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
    constexpr std::uint64_t kMemoryLimit = 25ULL * kGiB;
    constexpr std::uint64_t kExpectedCacheCapacity = kMemoryLimit - 512ULL * kMiB;
    constexpr std::uint64_t kPrivateUsageThreshold =
        kMemoryLimit - (kMemoryLimit / 100ULL) * 2ULL;
    passed &= Expect(
        CacheCapacityForTotalLimit(kMemoryLimit) == kExpectedCacheCapacity,
        "25 GiB total limit reserves 512 MiB for transient memory");
    passed &= Expect(
        AllowsBackgroundDecode(kPrivateUsageThreshold - 1ULL, kMemoryLimit),
        "background allowed immediately below private memory threshold");
    passed &= Expect(
        !AllowsBackgroundDecode(kPrivateUsageThreshold, kMemoryLimit),
        "background rejected at private memory threshold");
    passed &= Expect(
        AllowsBackgroundDecode(0, kMemoryLimit),
        "missing process memory telemetry safely allows background");
    passed &= Expect(
        CanRetainDecodedFrame(16U, 16U)
            && !CanRetainDecodedFrame(15U, 16U)
            && CanRetainDecodedFrame(0U, 0U),
        "post-scrub hot fill requires capacity for one decoded frame");

    constexpr zt::sequence::PlaybackRange kTelemetryRange{10U, 20U};
    constexpr zt::sequence::PlaybackRange kChangedTelemetryRange{10U, 21U};
    passed &= Expect(
        ShouldRefreshSnapshotTelemetry(
            false,
            7U,
            7U,
            kTelemetryRange,
            kTelemetryRange,
            true,
            true,
            std::chrono::milliseconds{0}),
        "snapshot telemetry samples immediately before initialization");
    passed &= Expect(
        !ShouldRefreshSnapshotTelemetry(
            true,
            7U,
            7U,
            kTelemetryRange,
            kTelemetryRange,
            true,
            true,
            std::chrono::milliseconds{99}),
        "snapshot telemetry reuses cache statistics inside 100 ms");
    passed &= Expect(
        ShouldRefreshSnapshotTelemetry(
            true,
            7U,
            7U,
            kTelemetryRange,
            kTelemetryRange,
            true,
            true,
            std::chrono::milliseconds{100}),
        "snapshot telemetry refreshes cache statistics at 10 Hz");
    passed &= Expect(
        ShouldRefreshSnapshotTelemetry(
            true,
            7U,
            8U,
            kTelemetryRange,
            kTelemetryRange,
            true,
            true,
            std::chrono::milliseconds{1}),
        "snapshot telemetry refreshes immediately for a new generation");
    passed &= Expect(
        ShouldRefreshSnapshotTelemetry(
            true,
            7U,
            7U,
            kTelemetryRange,
            kChangedTelemetryRange,
            true,
            true,
            std::chrono::milliseconds{1}),
        "snapshot telemetry refreshes immediately for a new playback range");
    passed &= Expect(
        ShouldRefreshSnapshotTelemetry(
            true,
            7U,
            7U,
            kTelemetryRange,
            kTelemetryRange,
            true,
            false,
            std::chrono::milliseconds{1}),
        "snapshot telemetry refreshes immediately when looping changes");
    passed &= Expect(
        ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            100U,
            120U,
            108U),
        "forward PNG scrub presents a completed frame toward the latest target");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            108U,
            120U,
            104U),
        "forward PNG scrub rejects a completed frame behind the displayed frame");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            108U,
            120U,
            120U),
        "exact PNG scrub target remains on the exact-frame commit path");
    passed &= Expect(
        ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            120U,
            100U,
            112U),
        "reverse PNG scrub presents a completed frame toward the latest target");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            112U,
            100U,
            116U),
        "reverse PNG scrub rejects a completed frame behind the displayed frame");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::PngSequence,
            112U,
            100U,
            96U),
        "reverse PNG scrub rejects a completed frame beyond the requested target");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            true,
            zt::sequence::SourceKind::PngSequence,
            100U,
            120U,
            108U),
        "comparison external clock keeps atomic exact-frame presentation");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            true,
            false,
            zt::sequence::SourceKind::Video,
            100U,
            120U,
            108U),
        "video scrubbing never uses PNG intermediate presentation");
    passed &= Expect(
        !ShouldPresentScrubIntermediateFrame(
            false,
            false,
            zt::sequence::SourceKind::PngSequence,
            100U,
            120U,
            108U),
        "normal PNG playback never presents an out-of-date intermediate frame");
    passed &= Expect(
        zt::sequence::comparison_detail::EffectiveMemoryLimitBytes(
            kMemoryLimit,
            true) == 8ULL * kGiB,
        "background mode caps a larger configured limit at 8 GiB");
    passed &= Expect(
        zt::sequence::comparison_detail::EffectiveMemoryLimitBytes(
            6ULL * kGiB,
            true) == 6ULL * kGiB,
        "background mode never raises a smaller configured limit");
    passed &= Expect(
        zt::sequence::comparison_detail::EffectiveMemoryLimitBytes(
            kMemoryLimit,
            false) == kMemoryLimit,
        "foreground mode preserves the configured memory limit");

    const auto primarySequenceOffsetDomain =
        zt::sequence::comparison_detail::ResolveSequenceFrameOffsetDomain(
            zt::sequence::SourceKind::PngSequence,
            1000U,
            zt::sequence::SourceKind::Video,
            800U,
            true);
    passed &= Expect(
        primarySequenceOffsetDomain.available &&
            primarySequenceOffsetDomain.onPrimary &&
            primarySequenceOffsetDomain.maximum == 999U,
        "comparison offset resolves the primary sequence lane and maximum");
    const auto secondarySequenceOffsetDomain =
        zt::sequence::comparison_detail::ResolveSequenceFrameOffsetDomain(
            zt::sequence::SourceKind::Video,
            800U,
            zt::sequence::SourceKind::PngSequence,
            1000U,
            true);
    passed &= Expect(
        secondarySequenceOffsetDomain.available &&
            !secondarySequenceOffsetDomain.onPrimary &&
            secondarySequenceOffsetDomain.maximum == 999U,
        "comparison offset resolves the secondary sequence lane and maximum");
    passed &= Expect(
        !zt::sequence::comparison_detail::ResolveSequenceFrameOffsetDomain(
             zt::sequence::SourceKind::Video,
             800U,
             zt::sequence::SourceKind::Video,
             800U,
             true).available &&
            !zt::sequence::comparison_detail::ResolveSequenceFrameOffsetDomain(
                 zt::sequence::SourceKind::PngSequence,
                 800U,
                 zt::sequence::SourceKind::PngSequence,
                 800U,
                 true).available &&
            !zt::sequence::comparison_detail::ResolveSequenceFrameOffsetDomain(
                 zt::sequence::SourceKind::PngSequence,
                 800U,
                 zt::sequence::SourceKind::Video,
                 800U,
                 false).available,
        "comparison offset is unavailable without exactly one active sequence lane");
    passed &= Expect(
        zt::sequence::comparison_detail::LaneUsesSequenceFrameOffset(
            zt::sequence::SourceKind::PngSequence,
            true,
            primarySequenceOffsetDomain) &&
            !zt::sequence::comparison_detail::LaneUsesSequenceFrameOffset(
                zt::sequence::SourceKind::Video,
                false,
                primarySequenceOffsetDomain) &&
            zt::sequence::comparison_detail::LaneUsesSequenceFrameOffset(
                zt::sequence::SourceKind::PngSequence,
                false,
                secondarySequenceOffsetDomain),
        "only the resolved sequence lane consumes the comparison offset");

    const auto sequenceFirstMapped =
        zt::sequence::comparison_detail::MapSharedFrameToLane(
            0U,
            1000U,
            true,
            200U);
    const auto sequenceLastMapped =
        zt::sequence::comparison_detail::MapSharedFrameToLane(
            799U,
            1000U,
            true,
            200U);
    const auto sequencePastEndMapped =
        zt::sequence::comparison_detail::MapSharedFrameToLane(
            800U,
            1000U,
            true,
            200U);
    const auto videoMapped =
        zt::sequence::comparison_detail::MapSharedFrameToLane(
            799U,
            800U,
            false,
            std::numeric_limits<FrameIndex>::max());
    passed &= Expect(
        sequenceFirstMapped.exists &&
            sequenceFirstMapped.sourceFrame == 200U &&
            sequenceLastMapped.exists &&
            sequenceLastMapped.sourceFrame == 999U &&
            !sequencePastEndMapped.exists &&
            videoMapped.exists && videoMapped.sourceFrame == 799U,
        "shared frames map through the sequence in-point while video stays unchanged");
    const auto overflowMapped =
        zt::sequence::comparison_detail::MapSharedFrameToLane(
            std::numeric_limits<FrameIndex>::max(),
            std::numeric_limits<std::size_t>::max(),
            true,
            1U);
    passed &= Expect(
        !overflowMapped.exists,
        "sequence frame offset addition rejects FrameIndex overflow");

    passed &= Expect(
        zt::sequence::comparison_detail::LaneSharedFrameCount(
            1000U,
            true,
            200U) == 800U &&
            zt::sequence::comparison_detail::LaneSharedFrameCount(
                1000U,
                false,
                200U) == 1000U &&
            zt::sequence::comparison_detail::LaneSharedFrameCount(
                100U,
                true,
                100U) == 0U,
        "sequence in-point shortens only the sequence shared duration");
    passed &= Expect(
        zt::sequence::comparison_detail::CommonTotalFramesWithSequenceOffset(
            1000U,
            zt::sequence::SourceKind::PngSequence,
            800U,
            zt::sequence::SourceKind::Video,
            true,
            200U) == 800U &&
            zt::sequence::comparison_detail::CommonTotalFramesWithSequenceOffset(
                1000U,
                zt::sequence::SourceKind::PngSequence,
                900U,
                zt::sequence::SourceKind::Video,
                true,
                200U) == 900U &&
            zt::sequence::comparison_detail::CommonTotalFramesWithSequenceOffset(
                900U,
                zt::sequence::SourceKind::Video,
                1000U,
                zt::sequence::SourceKind::PngSequence,
                true,
                200U) == 900U,
        "comparison duration uses the sequence duration remaining after its in-point");
    passed &= Expect(
        zt::sequence::comparison_detail::CommonTotalFramesWithSequenceOffset(
            1000U,
            zt::sequence::SourceKind::PngSequence,
            800U,
            zt::sequence::SourceKind::Video,
            true,
            0U) ==
            zt::sequence::comparison_detail::CommonTotalFrames(
                1000U,
                800U,
                true),
        "zero sequence offset preserves the original comparison duration");
    const zt::sequence::PlaybackRange mappedSequenceRange =
        zt::sequence::comparison_detail::MapSharedPlaybackRangeToLane(
            {100U, 300U},
            1000U,
            true,
            200U);
    const zt::sequence::PlaybackRange mappedVideoRange =
        zt::sequence::comparison_detail::MapSharedPlaybackRangeToLane(
            {100U, 300U},
            800U,
            false,
            200U);
    passed &= Expect(
        mappedSequenceRange == zt::sequence::PlaybackRange{300U, 500U} &&
            mappedVideoRange == zt::sequence::PlaybackRange{100U, 300U},
        "shared playback range shifts only inside the sequence engine");
    passed &= Expect(
        zt::sequence::comparison_detail::IsFullSharedPlaybackRange(
            {0U, 799U},
            800U) &&
            !zt::sequence::comparison_detail::IsFullSharedPlaybackRange(
                {100U, 799U},
                800U),
        "offset-shortened full range remains identifiable when a lane is replaced");

    const std::size_t maximumSize = std::numeric_limits<std::size_t>::max();
    const std::size_t seventyPercentMaximum = PercentageOfSize(maximumSize, 70U);
    passed &= Expect(
        seventyPercentMaximum <= maximumSize &&
            seventyPercentMaximum > maximumSize / 2U,
        "hot-window percentage calculation does not overflow");

    ApplicationActivityPolicy activityPolicy;
    passed &= Expect(
        activityPolicy.Update(false, true, 0.0) ==
            BackgroundModeTransition::None,
        "active application remains in foreground mode");
    passed &= Expect(
        activityPolicy.Update(false, false, 19.9) ==
            BackgroundModeTransition::None &&
            !activityPolicy.BackgroundMode(),
        "focus loss shorter than 20 seconds preserves foreground mode");
    passed &= Expect(
        activityPolicy.Update(false, false, 0.1) ==
            BackgroundModeTransition::Entered &&
            activityPolicy.BackgroundMode(),
        "20 seconds of focus loss enters background mode");
    passed &= Expect(
        activityPolicy.Update(false, true, 0.0) ==
            BackgroundModeTransition::Exited &&
            !activityPolicy.BackgroundMode(),
        "foreground activation immediately exits background mode");
    passed &= Expect(
        activityPolicy.Update(true, false, 0.0) ==
            BackgroundModeTransition::Entered,
        "minimizing enters background mode immediately");
    passed &= Expect(
        activityPolicy.Update(false, false, 1.0) ==
            BackgroundModeTransition::None &&
            activityPolicy.BackgroundMode(),
        "restoring behind another application stays in background mode");
    passed &= Expect(
        activityPolicy.Update(false, true, 0.0) ==
            BackgroundModeTransition::Exited,
        "restoring to the foreground exits background mode without delay");

    const zt::sequence::PlaybackRange fullTen = FullPlaybackRange(10U);
    passed &= Expect(
        fullTen.startFrame == 0U && fullTen.endFrame == 9U,
        "new sequence defaults to its complete playback range");
    const zt::sequence::PlaybackRange normalized = NormalizePlaybackRange(
        {18U, 3U},
        12U);
    passed &= Expect(
        normalized.startFrame == 3U && normalized.endFrame == 11U,
        "playback range clamps to the sequence and cannot cross");

    const zt::sequence::PlaybackRange customRange{10U, 20U};
    const auto loopForward = OffsetFrameInPlaybackRange(
        20U,
        1,
        customRange,
        true);
    const auto loopBackward = OffsetFrameInPlaybackRange(
        10U,
        -1,
        customRange,
        true);
    const auto multiStepForward = OffsetFrameInPlaybackRange(
        18U,
        5,
        customRange,
        true);
    passed &= Expect(
        loopForward.has_value() && *loopForward == 10U,
        "looping playback wraps custom end to custom start");
    passed &= Expect(
        loopBackward.has_value() && *loopBackward == 20U,
        "reverse range offset wraps custom start to custom end");
    passed &= Expect(
        multiStepForward.has_value() && *multiStepForward == 12U,
        "multi-frame tick remains inside the custom playback range");
    passed &= Expect(
        !OffsetFrameInPlaybackRange(20U, 1, customRange, false).has_value(),
        "non-looping playback stops after the inclusive custom end");
    passed &= Expect(
        ClampFrameToPlaybackRange(999, customRange) == 20U,
        "non-looping stop clamps to the inclusive custom end");
    passed &= Expect(
        PlaybackStartForPlay(30U, customRange) == 10U,
        "play invoked outside a custom range enters at its start");

    const zt::sequence::PlaybackRange preservedCustom =
        PlaybackRangeForLoadedSequence({2U, 5U}, 12U, true, true);
    const zt::sequence::PlaybackRange preservedFullMode =
        PlaybackRangeForLoadedSequence({0U, 9U}, 12U, true, false);
    const zt::sequence::PlaybackRange resetForNewFolder =
        PlaybackRangeForLoadedSequence({2U, 5U}, 4U, false, true);
    const zt::sequence::PlaybackRange clampedAfterShrink =
        PlaybackRangeForLoadedSequence({7U, 9U}, 5U, true, true);
    passed &= Expect(
        preservedCustom.startFrame == 2U && preservedCustom.endFrame == 5U,
        "same-folder reload preserves a custom playback range");
    passed &= Expect(
        preservedFullMode.startFrame == 0U && preservedFullMode.endFrame == 11U,
        "full-range mode expands when a growing folder is rescanned");
    passed &= Expect(
        resetForNewFolder.startFrame == 0U && resetForNewFolder.endFrame == 3U,
        "a genuinely new folder resets to its complete playback range");
    passed &= Expect(
        clampedAfterShrink.startFrame == 4U && clampedAfterShrink.endFrame == 4U,
        "a preserved custom range clamps safely after a folder shrinks");

    MemoryFrameCache rangeCache(64U);
    passed &= Expect(rangeCache.Put(MakeFrame(7U, 10U, 8U)), "range cache frame 10");
    passed &= Expect(rangeCache.Put(MakeFrame(7U, 11U, 8U)), "range cache frame 11");
    passed &= Expect(rangeCache.Put(MakeFrame(7U, 12U, 8U)), "range cache frame 12");
    passed &= Expect(
        rangeCache.CountContiguousInRange(
            7U,
            12U,
            1,
            {10U, 12U},
            true,
            32U) == 3U,
        "ready-ahead cache count wraps only inside the playback range");
    passed &= Expect(
        rangeCache.CountContiguousInRange(
            7U,
            12U,
            1,
            {10U, 12U},
            false,
            32U) == 1U,
        "non-looping ready-ahead cache count stops at the playback end");

    const std::filesystem::path chineseExportDirectory =
        std::filesystem::path(L"D:\\导出\\众人兜鱼");
    passed &= Expect(
        BuildMp4ExportCandidate(chineseExportDirectory, 0U) ==
            chineseExportDirectory / L"众人兜鱼.mp4",
        "Chinese export directory produces a Unicode MP4 filename");
    passed &= Expect(
        BuildMp4ExportCandidate(chineseExportDirectory, 1U) ==
            chineseExportDirectory / L"众人兜鱼01.mp4",
        "first collision uses a two-digit 01 suffix");
    passed &= Expect(
        BuildMp4ExportCandidate(chineseExportDirectory, 100U) ==
            chineseExportDirectory / L"众人兜鱼100.mp4",
        "collision suffix grows beyond two digits without truncation");

    const std::vector<std::filesystem::path> occupiedExportPaths{
        chineseExportDirectory / L"众人兜鱼.mp4",
        chineseExportDirectory / L"众人兜鱼01.mp4",
        chineseExportDirectory / L"众人兜鱼02.mp4.partial",
    };
    const std::filesystem::path nextExportPath = FindAvailableMp4ExportPath(
        chineseExportDirectory,
        [&occupiedExportPaths](const std::filesystem::path& candidate) {
            return std::find(
                       occupiedExportPaths.begin(),
                       occupiedExportPaths.end(),
                       candidate) != occupiedExportPaths.end();
        });
    passed &= Expect(
        nextExportPath == chineseExportDirectory / L"众人兜鱼02.mp4",
        "temporary files do not consume a final MP4 sequence number");

    const std::filesystem::path trailingSeparatorDirectory =
        std::filesystem::path(L"D:\\导出\\众人兜鱼\\");
    passed &= Expect(
        BuildMp4ExportCandidate(trailingSeparatorDirectory, 0U) ==
            trailingSeparatorDirectory / L"众人兜鱼.mp4",
        "trailing directory separator preserves the final folder name");

    return passed ? 0 : 1;
}
