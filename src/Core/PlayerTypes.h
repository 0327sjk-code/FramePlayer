#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace zt::sequence {

inline constexpr std::uint64_t kBytesPerGiB = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kDefaultMemoryLimitBytes = 25ULL * kBytesPerGiB;
inline constexpr std::uint32_t kDefaultDecodePercent = 100;
inline constexpr double kDefaultFramesPerSecond = 60.0;

using FrameIndex = std::uint32_t;
using Generation = std::uint64_t;

enum class SourceKind : std::uint8_t {
    None = 0,
    PngSequence,
    Video,
};

enum class FrameRequestKind : std::uint8_t {
    PlaybackAdvance = 0,
    InteractiveSeek,
};

struct ScrubUpdateResult final {
    bool accepted = false;
    bool scrubbing = false;
    FrameIndex requestedFrame = 0;
    Generation generation = 0;
};

struct EngineResourceBudget final {
    std::uint64_t globalProcessLimitBytes = kDefaultMemoryLimitBytes;
    std::uint64_t laneCacheCapacityBytes = 0;
};

struct VideoMetadata final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double framesPerSecond = 0.0;
    std::size_t frameCount = 0;
    std::int64_t durationHundredNanoseconds = 0;
};

struct PlaybackRange final {
    FrameIndex startFrame = 0;
    FrameIndex endFrame = 0;

    [[nodiscard]] bool operator==(const PlaybackRange&) const noexcept = default;
};

struct FrameFile {
    std::filesystem::path path;
    std::wstring relativePath;
    std::uint64_t fileSizeBytes = 0;
    std::filesystem::file_time_type lastWriteTime{};
};

// Immutable-by-value source description for an export job. The playback range
// is inclusive and indexes orderedPngFrames. Export always reads the original
// PNG files, so preview decode percentage intentionally is not part of this
// snapshot.
struct SequenceExportSnapshot final {
    Generation sourceGeneration = 0;
    std::filesystem::path sourceFolder;
    std::vector<FrameFile> orderedPngFrames;
    PlaybackRange inclusiveRange;
    double framesPerSecond = kDefaultFramesPerSecond;
};

// Immutable-by-value video description for an export job. The playback range
// is inclusive and indexes decoded source frames. sourceFramesPerSecond keeps
// seeking tied to the original media timeline, while framesPerSecond is the
// current player rate used for the exported clip.
struct VideoExportSnapshot final {
    Generation sourceGeneration = 0;
    std::filesystem::path sourceFile;
    PlaybackRange inclusiveRange;
    double sourceFramesPerSecond = 0.0;
    double framesPerSecond = kDefaultFramesPerSecond;
    std::size_t totalFrames = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
};

using ExportSourceSnapshot = std::variant<
    SequenceExportSnapshot,
    VideoExportSnapshot>;

struct DecodedFrame {
    FrameIndex index = 0;
    Generation generation = 0;
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t strideBytes = 0;
    std::uint32_t decodePercent = kDefaultDecodePercent;
    std::vector<std::uint8_t> bgraPixels;

    [[nodiscard]] std::size_t ByteSize() const noexcept {
        return bgraPixels.size();
    }
};

struct PlayerSettings {
    std::uint64_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    std::uint32_t decodePercent = kDefaultDecodePercent;
    double framesPerSecond = kDefaultFramesPerSecond;
    bool loopPlayback = true;
};

struct PlayerSnapshot {
    bool hasSource = false;
    bool hasSequence = false;
    bool loading = false;
    bool playing = false;
    bool scrubbing = false;
    bool buffering = false;
    bool loopPlayback = true;

    FrameIndex currentFrame = 0;
    FrameIndex requestedFrame = 0;
    FrameIndex playbackStartFrame = 0;
    FrameIndex playbackEndFrame = 0;
    std::size_t totalFrames = 0;
    std::size_t cachedFrames = 0;
    std::size_t estimatedCacheCapacityFrames = 0;

    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::uint32_t decodedWidth = 0;
    std::uint32_t decodedHeight = 0;
    std::uint32_t decodePercent = kDefaultDecodePercent;

    double targetFramesPerSecond = kDefaultFramesPerSecond;
    double actualFramesPerSecond = 0.0;
    double readyAheadSeconds = 0.0;
    double cacheProgress = 0.0;

    std::uint64_t memoryLimitBytes = kDefaultMemoryLimitBytes;
    std::uint64_t cacheCapacityBytes = 0;
    std::uint64_t cacheBytes = 0;
    std::uint64_t processWorkingSetBytes = 0;
    std::uint64_t processPrivateBytes = 0;
    std::uint64_t droppedFrames = 0;

    Generation generation = 0;
    SourceKind sourceKind = SourceKind::None;
    bool requestedFrameFailed = false;
    std::uint64_t displayRevision = 0;
    std::shared_ptr<const DecodedFrame> displayFrame;

    std::string folderUtf8;
    std::string sourcePathUtf8;
    std::string pendingSourcePathUtf8;
    std::string currentFileUtf8;
    std::string statusUtf8;
    std::string errorUtf8;
};

}  // namespace zt::sequence
