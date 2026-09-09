#pragma once

#include "Export/ExportTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zt::sequence::exporting {

inline constexpr std::uint64_t kTargetOutputBytes = 87'000'000ULL;
inline constexpr std::uint64_t kMaximumOutputBytes = 90'000'000ULL;
inline constexpr std::uint64_t kMinimumVideoBitRate = 100'000ULL;
inline constexpr std::uint64_t kMaximumVideoBitRate = 200'000'000ULL;
inline constexpr std::uint32_t kMaximumEncodingAttempts = 3U;

struct PixelCrop final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool operator==(const PixelCrop&) const noexcept = default;
};

enum class FfmpegInputKind : std::uint8_t {
    FfconcatManifest,
    Image2Sequence,
    VideoFile,
};

// Immutable input contract for every encoding attempt. Image2Sequence uses
// path as an escaped image2 pattern and startNumber as the first selected file
// number. FfconcatManifest uses path as the manifest. VideoFile uses path as
// the original media, startFrame as the first selected source frame, and
// sourceFramesPerSecond for frame-accurate input seeking.
struct FfmpegInputSpec final {
    FfmpegInputKind kind = FfmpegInputKind::FfconcatManifest;
    std::filesystem::path path;
    int startNumber = 0;
    FrameIndex startFrame = 0;
    double sourceFramesPerSecond = 0.0;
};

[[nodiscard]] std::optional<std::size_t> CountExportFrames(
    const SequenceExportSnapshot& sequence) noexcept;

[[nodiscard]] std::optional<std::size_t> CountExportFrames(
    const VideoExportSnapshot& video) noexcept;

[[nodiscard]] std::optional<std::size_t> CountExportFrames(
    const ExportSourceSnapshot& source) noexcept;

[[nodiscard]] double ExportFramesPerSecond(
    const ExportSourceSnapshot& source) noexcept;

[[nodiscard]] std::optional<std::uint64_t> CalculateInitialVideoBitRate(
    std::size_t frameCount,
    double framesPerSecond) noexcept;

[[nodiscard]] std::optional<std::uint64_t> CalculateRetryVideoBitRate(
    std::uint64_t currentBitRate,
    std::uint64_t actualOutputBytes) noexcept;

[[nodiscard]] std::optional<PixelCrop> ResolvePixelCrop(
    const NormalizedCrop& crop,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight) noexcept;

[[nodiscard]] bool IsFullFrameCrop(
    const PixelCrop& crop,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight) noexcept;

// Produces an ffconcat 1.0 document in UTF-8 without a BOM. Windows path
// separators are normalized to '/', and apostrophes use ffconcat token
// quoting. The final selected file is repeated so its duration is honored.
[[nodiscard]] std::optional<std::string> BuildFfconcatManifestText(
    const SequenceExportSnapshot& sequence);

// Builds arguments only; process creation and Windows quoting are kept in the
// process layer. This seam makes the exact encoder contract testable without
// requiring FFmpeg or an NVIDIA GPU on the test machine.
[[nodiscard]] std::vector<std::wstring> BuildFfmpegArguments(
    const FfmpegInputSpec& input,
    const std::filesystem::path& temporaryOutputPath,
    std::size_t frameCount,
    double framesPerSecond,
    std::uint64_t videoBitRate,
    const PixelCrop& crop,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    const std::optional<std::filesystem::path>& overlayImagePath =
        std::nullopt);

}  // namespace zt::sequence::exporting
