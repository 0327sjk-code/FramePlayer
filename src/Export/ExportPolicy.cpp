#include "Export/ExportPolicy.h"

#include "Overlay/MaskOverlaySpec.h"
#include "Platform/Utf8.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace zt::sequence::exporting {
namespace {

[[nodiscard]] double ClampUnit(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] std::uint32_t LargestEvenAtMost(
    const std::uint32_t value) noexcept {
    return value & ~std::uint32_t{1U};
}

[[nodiscard]] std::uint32_t NearestEven(
    const long double value,
    const std::uint32_t maximumEven) noexcept {
    const long double roundedHalf = std::round(value / 2.0L) * 2.0L;
    if (roundedHalf <= 0.0L) {
        return 0U;
    }
    if (roundedHalf >= static_cast<long double>(maximumEven)) {
        return maximumEven;
    }
    return static_cast<std::uint32_t>(roundedHalf);
}

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
ResolveAxis(
    const double normalizedMinimum,
    const double normalizedMaximum,
    const std::uint32_t sourceExtent) noexcept {
    if (!std::isfinite(normalizedMinimum) ||
        !std::isfinite(normalizedMaximum) ||
        sourceExtent < 2U) {
        return std::nullopt;
    }

    const double minimum = ClampUnit(normalizedMinimum);
    const double maximum = ClampUnit(normalizedMaximum);
    if (!(minimum < maximum)) {
        return std::nullopt;
    }

    const std::uint32_t largestEvenExtent = LargestEvenAtMost(sourceExtent);
    if (largestEvenExtent < 2U) {
        return std::nullopt;
    }

    const long double source = static_cast<long double>(sourceExtent);
    const long double idealExtent =
        (static_cast<long double>(maximum) -
         static_cast<long double>(minimum)) * source;
    std::uint32_t extent = NearestEven(idealExtent, largestEvenExtent);
    extent = std::clamp(extent, 2U, largestEvenExtent);

    const long double idealCenter =
        (static_cast<long double>(minimum) +
         static_cast<long double>(maximum)) * source / 2.0L;
    const long double idealOrigin =
        idealCenter - static_cast<long double>(extent) / 2.0L;
    const std::uint32_t maximumOrigin = LargestEvenAtMost(
        sourceExtent - extent);
    const std::uint32_t origin = NearestEven(idealOrigin, maximumOrigin);
    return std::pair{origin, extent};
}

[[nodiscard]] std::string FormatPositiveDouble(const double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    return stream.str();
}

[[nodiscard]] std::optional<std::string> QuoteFfconcatPath(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return std::nullopt;
    }

    std::wstring normalized = path.lexically_normal().wstring();
    for (wchar_t& character : normalized) {
        if (character == L'\\') {
            character = L'/';
        }
        if (character == L'\r' || character == L'\n' || character == L'\0') {
            return std::nullopt;
        }
    }

    const std::string utf8 = WideToUtf8(normalized);
    if (utf8.empty() && !normalized.empty()) {
        return std::nullopt;
    }

    std::string result;
    result.reserve(utf8.size() + 2U);
    result.push_back('\'');
    for (const char character : utf8) {
        if (character == '\'') {
            result.append("'\\''");
        } else {
            result.push_back(character);
        }
    }
    result.push_back('\'');
    return result;
}

[[nodiscard]] std::uint64_t SaturatingDouble(
    const std::uint64_t value) noexcept {
    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    return value > maximum / 2U ? maximum : value * 2U;
}

[[nodiscard]] std::wstring ToWideNumber(const std::uint64_t value) {
    return std::to_wstring(value);
}

[[nodiscard]] std::wstring ToWideSize(const std::size_t value) {
    return std::to_wstring(static_cast<unsigned long long>(value));
}

[[nodiscard]] std::wstring ToWideNumber(const double value) {
    return Utf8ToWide(FormatPositiveDouble(value));
}

}  // namespace

std::optional<std::size_t> CountExportFrames(
    const SequenceExportSnapshot& sequence) noexcept {
    if (sequence.orderedPngFrames.empty()) {
        return std::nullopt;
    }

    const std::size_t start = sequence.inclusiveRange.startFrame;
    const std::size_t end = sequence.inclusiveRange.endFrame;
    if (start > end || end >= sequence.orderedPngFrames.size()) {
        return std::nullopt;
    }
    return (end - start) + 1U;
}

std::optional<std::size_t> CountExportFrames(
    const VideoExportSnapshot& video) noexcept {
    if (video.sourceFile.empty() || video.totalFrames == 0U ||
        video.sourceWidth < 2U || video.sourceHeight < 2U ||
        !std::isfinite(video.sourceFramesPerSecond) ||
        video.sourceFramesPerSecond <= 0.0 ||
        !std::isfinite(video.framesPerSecond) ||
        video.framesPerSecond <= 0.0) {
        return std::nullopt;
    }

    const std::size_t start = video.inclusiveRange.startFrame;
    const std::size_t end = video.inclusiveRange.endFrame;
    if (start > end || end >= video.totalFrames) {
        return std::nullopt;
    }
    return (end - start) + 1U;
}

std::optional<std::size_t> CountExportFrames(
    const ExportSourceSnapshot& source) noexcept {
    return std::visit(
        [](const auto& value) { return CountExportFrames(value); },
        source);
}

double ExportFramesPerSecond(
    const ExportSourceSnapshot& source) noexcept {
    return std::visit(
        [](const auto& value) { return value.framesPerSecond; },
        source);
}

std::optional<std::uint64_t> CalculateInitialVideoBitRate(
    const std::size_t frameCount,
    const double framesPerSecond) noexcept {
    if (frameCount == 0U || !std::isfinite(framesPerSecond) ||
        framesPerSecond <= 0.0) {
        return std::nullopt;
    }

    const long double durationSeconds =
        static_cast<long double>(frameCount) /
        static_cast<long double>(framesPerSecond);
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0L) {
        return std::nullopt;
    }

    constexpr long double targetBits =
        static_cast<long double>(kTargetOutputBytes) * 8.0L;
    const long double calculated = std::floor(targetBits / durationSeconds);
    if (!std::isfinite(calculated) || calculated < 0.0L) {
        return std::nullopt;
    }

    const long double clamped = std::clamp(
        calculated,
        static_cast<long double>(kMinimumVideoBitRate),
        static_cast<long double>(kMaximumVideoBitRate));
    return static_cast<std::uint64_t>(clamped);
}

std::optional<std::uint64_t> CalculateRetryVideoBitRate(
    const std::uint64_t currentBitRate,
    const std::uint64_t actualOutputBytes) noexcept {
    if (currentBitRate <= kMinimumVideoBitRate ||
        actualOutputBytes <= kMaximumOutputBytes) {
        return std::nullopt;
    }

    const long double scaled = std::floor(
        static_cast<long double>(currentBitRate) *
        static_cast<long double>(kTargetOutputBytes) /
        static_cast<long double>(actualOutputBytes));
    if (!std::isfinite(scaled) || scaled <= 0.0L) {
        return std::nullopt;
    }

    std::uint64_t next = static_cast<std::uint64_t>(std::clamp(
        scaled,
        static_cast<long double>(kMinimumVideoBitRate),
        static_cast<long double>(kMaximumVideoBitRate)));
    if (next >= currentBitRate) {
        next = currentBitRate - 1U;
    }
    if (next < kMinimumVideoBitRate) {
        return std::nullopt;
    }
    return next;
}

std::optional<PixelCrop> ResolvePixelCrop(
    const NormalizedCrop& crop,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight) noexcept {
    const auto horizontal = ResolveAxis(
        crop.minimumX,
        crop.maximumX,
        sourceWidth);
    const auto vertical = ResolveAxis(
        crop.minimumY,
        crop.maximumY,
        sourceHeight);
    if (!horizontal || !vertical) {
        return std::nullopt;
    }

    return PixelCrop{
        horizontal->first,
        vertical->first,
        horizontal->second,
        vertical->second,
    };
}

bool IsFullFrameCrop(
    const PixelCrop& crop,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight) noexcept {
    return crop.x == 0U && crop.y == 0U &&
        crop.width == sourceWidth && crop.height == sourceHeight;
}

std::optional<std::string> BuildFfconcatManifestText(
    const SequenceExportSnapshot& sequence) {
    const auto frameCount = CountExportFrames(sequence);
    if (!frameCount || !std::isfinite(sequence.framesPerSecond) ||
        sequence.framesPerSecond <= 0.0) {
        return std::nullopt;
    }

    const double frameDuration = 1.0 / sequence.framesPerSecond;
    if (!std::isfinite(frameDuration) || frameDuration <= 0.0) {
        return std::nullopt;
    }
    const std::string durationText = FormatPositiveDouble(frameDuration);

    std::string manifest = "ffconcat version 1.0\n";
    const std::size_t start = sequence.inclusiveRange.startFrame;
    const std::size_t end = sequence.inclusiveRange.endFrame;
    std::optional<std::string> lastQuotedPath;
    for (std::size_t index = start; index <= end; ++index) {
        const auto quotedPath = QuoteFfconcatPath(
            sequence.orderedPngFrames[index].path);
        if (!quotedPath) {
            return std::nullopt;
        }
        manifest.append("file ");
        manifest.append(*quotedPath);
        manifest.push_back('\n');
        manifest.append("duration ");
        manifest.append(durationText);
        manifest.push_back('\n');
        lastQuotedPath = quotedPath;
    }

    if (!lastQuotedPath) {
        return std::nullopt;
    }
    manifest.append("file ");
    manifest.append(*lastQuotedPath);
    manifest.push_back('\n');
    return manifest;
}

std::vector<std::wstring> BuildFfmpegArguments(
    const FfmpegInputSpec& input,
    const std::filesystem::path& temporaryOutputPath,
    const std::size_t frameCount,
    const double framesPerSecond,
    const std::uint64_t videoBitRate,
    const PixelCrop& crop,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const std::optional<std::filesystem::path>& overlayImagePath) {
    const bool hasOverlayImage =
        overlayImagePath.has_value() && !overlayImagePath->empty();
    std::wstring filter;
    if (input.kind == FfmpegInputKind::VideoFile) {
        filter.append(L"trim=end_frame=");
        filter.append(ToWideSize(frameCount));
        filter.append(L",setpts=N/(");
        filter.append(ToWideNumber(framesPerSecond));
        filter.append(L"*TB),");
    }
    if (!IsFullFrameCrop(crop, sourceWidth, sourceHeight)) {
        filter.append(L"crop=");
        filter.append(std::to_wstring(crop.width));
        filter.push_back(L':');
        filter.append(std::to_wstring(crop.height));
        filter.push_back(L':');
        filter.append(std::to_wstring(crop.x));
        filter.push_back(L':');
        filter.append(std::to_wstring(crop.y));
        filter.push_back(L',');
    }
    filter.append(L"scale=");
    if (hasOverlayImage) {
        filter.append(std::to_wstring(overlay::kMaskOverlayWidthPixels));
        filter.push_back(L':');
        filter.append(std::to_wstring(overlay::kMaskOverlayHeightPixels));
        filter.push_back(L':');
    }
    filter.append(input.kind == FfmpegInputKind::VideoFile
        ? L"in_range=auto:out_range=tv:out_color_matrix=bt709,"
          L"format=nv12,setparams=range=limited:color_primaries=bt709:"
          L"color_trc=bt709:colorspace=bt709"
        : L"in_range=pc:out_range=tv:out_color_matrix=bt709,"
          L"format=nv12,setparams=range=limited:color_primaries=bt709:"
          L"color_trc=bt709:colorspace=bt709");

    std::vector<std::wstring> arguments{
        L"-hide_banner",
        L"-loglevel", L"error",
        L"-nostdin",
    };
    if (input.kind == FfmpegInputKind::Image2Sequence) {
        arguments.insert(
            arguments.end(),
            {
                L"-threads", L"0",
                L"-f", L"image2",
                L"-pattern_type", L"sequence",
                L"-framerate", ToWideNumber(framesPerSecond),
                L"-start_number", std::to_wstring(input.startNumber),
                L"-start_number_range", L"1",
                L"-i", input.path.wstring(),
            });
    } else if (input.kind == FfmpegInputKind::FfconcatManifest) {
        arguments.insert(
            arguments.end(),
            {
                L"-f", L"concat",
                L"-safe", L"0",
                L"-i", input.path.wstring(),
            });
    } else {
        arguments.insert(arguments.end(), {L"-threads", L"0"});
        if (input.startFrame > 0U) {
            const double seekSeconds =
                static_cast<double>(input.startFrame) /
                input.sourceFramesPerSecond;
            arguments.insert(
                arguments.end(),
                {L"-ss", ToWideNumber(seekSeconds)});
        }
        arguments.insert(arguments.end(), {L"-i", input.path.wstring()});
        if (!hasOverlayImage) {
            arguments.insert(arguments.end(), {L"-map", L"0:v:0"});
        }
    }

    if (hasOverlayImage) {
        arguments.insert(
            arguments.end(),
            {L"-i", overlayImagePath->wstring()});
    }

    arguments.push_back(L"-an");
    if (hasOverlayImage) {

        std::wstring complexFilter = L"[0:v]";
        complexFilter.append(filter);
        complexFilter.append(
            L"[base];[1:v]"
            L"scale=in_range=pc:out_range=tv:out_color_matrix=bt709,"
            L"format=yuva420p,loop=loop=-1:size=1:start=0,setpts=N/(");
        complexFilter.append(ToWideNumber(framesPerSecond));
        complexFilter.append(
            L"*TB)[overlay];[base][overlay]"
            L"overlay=0:0:shortest=1:format=yuv420,format=nv12,"
            L"setparams=range=limited:color_primaries=bt709:"
            L"color_trc=bt709:colorspace=bt709[outv]");
        arguments.insert(
            arguments.end(),
            {
                L"-filter_complex", std::move(complexFilter),
                L"-map", L"[outv]",
            });
    } else {
        arguments.insert(
            arguments.end(),
            {L"-vf", std::move(filter)});
    }
    arguments.insert(
        arguments.end(),
        {
        L"-c:v", L"hevc_nvenc",
        L"-preset", L"p5",
        L"-tune", L"hq",
        });
    arguments.insert(
        arguments.end(),
        {
        L"-profile:v", L"main",
        L"-tag:v", L"hvc1",
        L"-rc", L"cbr",
        L"-b:v", ToWideNumber(videoBitRate),
        L"-minrate", ToWideNumber(videoBitRate),
        L"-maxrate", ToWideNumber(videoBitRate),
        L"-bufsize", ToWideNumber(SaturatingDouble(videoBitRate)),
        L"-g", L"120",
        L"-bf", L"2",
        L"-pix_fmt", L"yuv420p",
        L"-colorspace", L"bt709",
        L"-color_primaries", L"bt709",
        L"-color_trc", L"bt709",
        L"-color_range", L"tv",
        L"-r", ToWideNumber(framesPerSecond),
        L"-fps_mode", L"cfr",
        L"-frames:v", ToWideSize(frameCount),
        L"-movflags", L"+faststart",
        L"-progress", L"pipe:1",
        L"-nostats",
        L"-y",
        temporaryOutputPath.wstring(),
        });
    return arguments;
}

}  // namespace zt::sequence::exporting
