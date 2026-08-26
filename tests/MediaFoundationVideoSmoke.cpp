#include "Imaging/MediaFoundationVideoDecoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] std::uint32_t ScaledDimension(
    const std::uint32_t sourceDimension,
    const std::uint32_t percent) noexcept {
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1ULL,
        (static_cast<std::uint64_t>(sourceDimension) * percent + 50ULL) /
            100ULL));
}

[[nodiscard]] bool ValidateFrame(
    const zt::sequence::DecodedFrame& frame,
    const zt::sequence::VideoMetadata& metadata,
    const zt::sequence::FrameIndex expectedIndex,
    const std::uint32_t percent) {
    const std::uint32_t expectedWidth =
        ScaledDimension(metadata.width, percent);
    const std::uint32_t expectedHeight =
        ScaledDimension(metadata.height, percent);
    if (frame.index != expectedIndex ||
        frame.sourceWidth != metadata.width ||
        frame.sourceHeight != metadata.height ||
        frame.width != expectedWidth ||
        frame.height != expectedHeight ||
        frame.strideBytes != expectedWidth * 4U ||
        frame.decodePercent != percent ||
        frame.bgraPixels.size() !=
            static_cast<std::size_t>(expectedWidth) * expectedHeight * 4U) {
        return false;
    }

    std::uint64_t colorSum = 0ULL;
    for (std::size_t offset = 0U;
         offset + 3U < frame.bgraPixels.size();
         offset += 4096U) {
        const std::size_t pixelOffset = offset - (offset % 4U);
        colorSum += frame.bgraPixels[pixelOffset];
        colorSum += frame.bgraPixels[pixelOffset + 1U];
        colorSum += frame.bgraPixels[pixelOffset + 2U];
        if (frame.bgraPixels[pixelOffset + 3U] != 0xFFU) {
            return false;
        }
    }
    return colorSum > 0ULL;
}

[[nodiscard]] bool WritePpm(
    const std::filesystem::path& outputPath,
    const zt::sequence::DecodedFrame& frame) {
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
    for (std::size_t offset = 0U;
         offset + 3U < frame.bgraPixels.size();
         offset += 4U) {
        const std::array<char, 3U> rgb{
            static_cast<char>(frame.bgraPixels[offset + 2U]),
            static_cast<char>(frame.bgraPixels[offset + 1U]),
            static_cast<char>(frame.bgraPixels[offset]),
        };
        output.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }
    return output.good();
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    using namespace zt::sequence;
    if ((argumentCount != 2 && argumentCount != 3) ||
        arguments == nullptr || arguments[1] == nullptr) {
        std::cerr << "usage: ZTMediaFoundationVideoSmoke <video> [frame0.ppm]\n";
        return 2;
    }

    const std::filesystem::path videoFile(arguments[1]);
    const VideoProbeResult probe = ProbeVideoFile(videoFile);
    if (!probe) {
        std::cerr << "probe failed: " << probe.errorUtf8 << '\n';
        return 3;
    }

    std::cout << "metadata=" << probe.metadata.width << 'x'
              << probe.metadata.height << " fps="
              << probe.metadata.framesPerSecond << " frames="
              << probe.metadata.frameCount << " duration100ns="
              << probe.metadata.durationHundredNanoseconds << '\n';

    MediaFoundationVideoDecoder decoder;
    constexpr std::array<std::uint32_t, 4U> percentages{25U, 50U, 75U, 100U};
    const std::array<FrameIndex, 4U> requestedFrames{
        0U,
        1U,
        static_cast<FrameIndex>(probe.metadata.frameCount / 2U),
        static_cast<FrameIndex>(probe.metadata.frameCount - 1U),
    };

    const auto started = std::chrono::steady_clock::now();
    Generation generation = 1U;
    for (const std::uint32_t percent : percentages) {
        for (const FrameIndex frameIndex : requestedFrames) {
            VideoDecodeResult result = decoder.Decode(
                videoFile,
                frameIndex,
                generation,
                percent);
            if (!result) {
                std::cerr << "decode failed percent=" << percent
                          << " frame=" << frameIndex << ": "
                          << result.errorUtf8 << '\n';
                return 4;
            }
            if (!ValidateFrame(
                    *result.frame,
                    probe.metadata,
                    frameIndex,
                    percent)) {
                std::cerr << "invalid frame percent=" << percent
                          << " frame=" << frameIndex << '\n';
                return 5;
            }
        }
        ++generation;
    }

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "decoded=16 elapsed=" << seconds << "s\n";

    constexpr std::size_t kSequentialFrames = 120U;
    const auto sequentialStarted = std::chrono::steady_clock::now();
    std::shared_ptr<DecodedFrame> firstSequentialFrame;
    for (std::size_t index = 0U; index < kSequentialFrames; ++index) {
        const FrameIndex frameIndex = static_cast<FrameIndex>(index);
        VideoDecodeResult result = decoder.Decode(
            videoFile,
            frameIndex,
            10U,
            100U);
        if (!result || !ValidateFrame(
                *result.frame,
                probe.metadata,
                frameIndex,
                100U)) {
            std::cerr << "sequential decode failed frame=" << frameIndex
                      << ": " << result.errorUtf8 << '\n';
            return 6;
        }
        if (index == 0U) {
            firstSequentialFrame = std::move(result.frame);
        }
    }
    const double sequentialSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sequentialStarted).count();
    const double decodedFramesPerSecond = sequentialSeconds > 0.0
        ? static_cast<double>(kSequentialFrames) / sequentialSeconds
        : 0.0;
    std::cout << "sequential=" << kSequentialFrames
              << " elapsed=" << sequentialSeconds << "s fps="
              << decodedFramesPerSecond << '\n';
    if (decodedFramesPerSecond < 60.0) {
        std::cerr << "sequential decode throughput below 60 fps\n";
        return 7;
    }
    if (argumentCount == 3 && arguments[2] != nullptr &&
        (!firstSequentialFrame ||
            !WritePpm(arguments[2], *firstSequentialFrame))) {
        std::cerr << "failed to write reference frame\n";
        return 8;
    }
    return 0;
}
