#pragma once

#include "Core/PlayerTypes.h"

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace zt::sequence {

struct VideoProbeResult final {
    VideoMetadata metadata;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return errorUtf8.empty() && metadata.width > 0U &&
            metadata.height > 0U && metadata.frameCount > 0U &&
            metadata.framesPerSecond > 0.0;
    }
};

struct VideoDecodeResult final {
    std::shared_ptr<DecodedFrame> frame;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return frame != nullptr && errorUtf8.empty();
    }
};

// Reads video frames through the Windows Media Foundation Source Reader.
// One instance must stay on one worker thread; it reuses its decoder for
// sequential playback and seeks only when the requested frame is not next.
class MediaFoundationVideoDecoder final {
public:
    MediaFoundationVideoDecoder();
    ~MediaFoundationVideoDecoder();

    MediaFoundationVideoDecoder(const MediaFoundationVideoDecoder&) = delete;
    MediaFoundationVideoDecoder& operator=(const MediaFoundationVideoDecoder&) = delete;
    MediaFoundationVideoDecoder(MediaFoundationVideoDecoder&&) = delete;
    MediaFoundationVideoDecoder& operator=(MediaFoundationVideoDecoder&&) = delete;

    [[nodiscard]] VideoDecodeResult Decode(
        const std::filesystem::path& videoFile,
        FrameIndex frameIndex,
        Generation generation,
        std::uint32_t decodePercent,
        const std::atomic_bool* cancelled = nullptr);

    // Called on the decoder worker immediately before the application exits.
    // Windows reclaims the final Source Reader with the process; detaching it
    // avoids a Media Foundation driver teardown stall after cancelled seeks.
    void PrepareForProcessShutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] VideoProbeResult ProbeVideoFile(
    const std::filesystem::path& videoFile) noexcept;

}  // namespace zt::sequence
