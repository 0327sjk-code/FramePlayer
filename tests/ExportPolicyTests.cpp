#include "Export/ExportPolicy.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zt::sequence::FrameFile;
using zt::sequence::SequenceExportSnapshot;
using zt::sequence::exporting::NormalizedCrop;
using zt::sequence::exporting::PixelCrop;

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

[[nodiscard]] NormalizedCrop CenteredCrop(
    const std::uint32_t width,
    const std::uint32_t height) {
    constexpr double source = 1920.0;
    const double horizontalMargin =
        (source - static_cast<double>(width)) / (2.0 * source);
    const double verticalMargin =
        (source - static_cast<double>(height)) / (2.0 * source);
    return {
        horizontalMargin,
        verticalMargin,
        1.0 - horizontalMargin,
        1.0 - verticalMargin,
    };
}

[[nodiscard]] SequenceExportSnapshot MakeSequence() {
    SequenceExportSnapshot sequence;
    sequence.sourceFolder = L"D:\\渲染\\O'Brien";
    sequence.framesPerSecond = 60.0;
    sequence.inclusiveRange = {1U, 3U};
    sequence.orderedPngFrames = {
        FrameFile{L"D:\\渲染\\O'Brien\\Frame.0.png"},
        FrameFile{L"D:\\渲染\\O'Brien\\Frame.1.png"},
        FrameFile{L"D:\\渲染\\O'Brien\\Frame.2.png"},
        FrameFile{L"D:\\渲染\\O'Brien\\Frame.3.png"},
        FrameFile{L"D:\\渲染\\O'Brien\\Frame.4.png"},
    };
    return sequence;
}

[[nodiscard]] std::size_t CountOccurrences(
    const std::string_view text,
    const std::string_view needle) {
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] std::optional<std::wstring_view> ArgumentValue(
    const std::vector<std::wstring>& arguments,
    const std::wstring_view option) {
    for (std::size_t index = 0U; index + 1U < arguments.size(); ++index) {
        if (arguments[index] == option) {
            return arguments[index + 1U];
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool TestCrop(
    const NormalizedCrop& normalized,
    const PixelCrop& expected,
    const char* message) {
    const auto actual = zt::sequence::exporting::ResolvePixelCrop(
        normalized,
        1920U,
        1920U);
    return Expect(actual.has_value() && *actual == expected, message);
}

}  // namespace

int main() {
    using namespace zt::sequence::exporting;

    bool passed = true;
    const SequenceExportSnapshot sequence = MakeSequence();

    const auto frameCount = CountExportFrames(sequence);
    passed &= Expect(
        frameCount.has_value() && *frameCount == 3U,
        "inclusive range exports end-start+1 frames");

    SequenceExportSnapshot invalidRange = sequence;
    invalidRange.inclusiveRange = {4U, 1U};
    passed &= Expect(
        !CountExportFrames(invalidRange).has_value(),
        "crossed inclusive range is rejected");
    invalidRange.inclusiveRange = {0U, 5U};
    passed &= Expect(
        !CountExportFrames(invalidRange).has_value(),
        "range beyond ordered PNG list is rejected");

    const auto sixtyFpsBitRate = CalculateInitialVideoBitRate(3000U, 60.0);
    passed &= Expect(
        sixtyFpsBitRate.has_value() && *sixtyFpsBitRate == 13'920'000ULL,
        "60 FPS duration drives exact 87 MB target bitrate");
    passed &= Expect(
        CalculateInitialVideoBitRate(1U, 60.0) == kMaximumVideoBitRate,
        "very short export clamps without integer overflow");
    passed &= Expect(
        CalculateInitialVideoBitRate(
            std::numeric_limits<std::size_t>::max(),
            1.0) == kMinimumVideoBitRate,
        "very long export clamps without integer overflow");
    passed &= Expect(
        !CalculateInitialVideoBitRate(0U, 60.0).has_value() &&
            !CalculateInitialVideoBitRate(1U, 0.0).has_value(),
        "invalid duration inputs are rejected");

    const auto retryRate = CalculateRetryVideoBitRate(
        20'000'000ULL,
        100'000'000ULL);
    passed &= Expect(
        retryRate.has_value() && *retryRate == 17'400'000ULL,
        "oversized output lowers bitrate in proportion to actual bytes");
    passed &= Expect(
        !CalculateRetryVideoBitRate(
            20'000'000ULL,
            kMaximumOutputBytes).has_value(),
        "output at hard cap does not retry");

    passed &= TestCrop(
        {0.0, 0.0, 1.0, 1.0},
        {0U, 0U, 1920U, 1920U},
        "full frame crop");
    passed &= TestCrop(
        CenteredCrop(1080U, 1080U),
        {420U, 420U, 1080U, 1080U},
        "1080x1080 crop is centered");
    passed &= TestCrop(
        CenteredCrop(1920U, 1080U),
        {0U, 420U, 1920U, 1080U},
        "1920x1080 crop is centered");
    passed &= TestCrop(
        CenteredCrop(1080U, 1920U),
        {420U, 0U, 1080U, 1920U},
        "1080x1920 crop is centered");
    passed &= TestCrop(
        CenteredCrop(864U, 1080U),
        {528U, 420U, 864U, 1080U},
        "864x1080 crop is centered");

    const auto oddSourceCrop = ResolvePixelCrop(
        {0.0, 0.0, 1.0, 1.0},
        1921U,
        1081U);
    passed &= Expect(
        oddSourceCrop.has_value() &&
            oddSourceCrop->x % 2U == 0U && oddSourceCrop->y % 2U == 0U &&
            oddSourceCrop->width == 1920U &&
            oddSourceCrop->height == 1080U,
        "odd source is safely aligned for yuv420p");
    passed &= Expect(
        !ResolvePixelCrop(
            {0.5, 0.0, 0.5, 1.0},
            1920U,
            1920U).has_value() &&
            !ResolvePixelCrop(
                {std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, 1.0},
                1920U,
                1920U).has_value(),
        "invalid normalized crop is rejected");

    const auto manifest = BuildFfconcatManifestText(sequence);
    passed &= Expect(manifest.has_value(), "build ffconcat manifest");
    if (manifest) {
        passed &= Expect(
            manifest->starts_with("ffconcat version 1.0\n"),
            "ffconcat header has no BOM");
        passed &= Expect(
            CountOccurrences(*manifest, "file ") == 4U &&
                CountOccurrences(*manifest, "duration ") == 3U,
            "manifest writes selected frames and repeats only the final file");
        passed &= Expect(
            manifest->find("Frame.0.png") == std::string::npos &&
                manifest->find("Frame.4.png") == std::string::npos,
            "manifest excludes frames outside inclusive range");
        passed &= Expect(
            manifest->find("D:/渲染/O'\\''Brien/Frame.1.png") !=
                std::string::npos,
            "ffconcat path normalizes backslashes and escapes apostrophes");
        const std::size_t lastFrameFirst = manifest->find("Frame.3.png");
        passed &= Expect(
            lastFrameFirst != std::string::npos &&
                manifest->find("Frame.3.png", lastFrameFirst + 1U) !=
                    std::string::npos,
            "ffconcat repeats last file so its duration is applied");
    }

    const PixelCrop exactCrop{420U, 420U, 1080U, 1080U};
    const FfmpegInputSpec image2Input{
        FfmpegInputKind::Image2Sequence,
        L"F:\\Render\\众人兜鱼\\MainSeq.%04d.png",
        517,
    };
    const std::vector<std::wstring> arguments = BuildFfmpegArguments(
        image2Input,
        L"D:\\导出\\序列.part.mp4",
        3U,
        60.0,
        13'920'000ULL,
        exactCrop,
        1920U,
        1920U);
    passed &= Expect(
        ArgumentValue(arguments, L"-frames:v") == L"3",
        "FFmpeg receives exact inclusive frame count");
    passed &= Expect(
        ArgumentValue(arguments, L"-threads") == L"0" &&
            ArgumentValue(arguments, L"-f") == L"image2" &&
            ArgumentValue(arguments, L"-pattern_type") == L"sequence" &&
            ArgumentValue(arguments, L"-framerate") == L"60" &&
            ArgumentValue(arguments, L"-start_number") == L"517" &&
            ArgumentValue(arguments, L"-start_number_range") == L"1" &&
            ArgumentValue(arguments, L"-i") == image2Input.path.wstring() &&
            !ArgumentValue(arguments, L"-safe").has_value(),
        "continuous input uses the frame-threaded image2 contract");
    passed &= Expect(
        ArgumentValue(arguments, L"-c:v") == L"hevc_nvenc" &&
            ArgumentValue(arguments, L"-preset") == L"p5" &&
            ArgumentValue(arguments, L"-tune") == L"hq" &&
            ArgumentValue(arguments, L"-profile:v") == L"main" &&
            ArgumentValue(arguments, L"-tag:v") == L"hvc1" &&
            ArgumentValue(arguments, L"-pix_fmt") == L"yuv420p",
        "FFmpeg uses NVENC HEVC Main p5 hvc1 yuv420p");
    passed &= Expect(
        ArgumentValue(arguments, L"-b:v") == L"13920000" &&
            ArgumentValue(arguments, L"-minrate") == L"13920000" &&
            ArgumentValue(arguments, L"-maxrate") == L"13920000" &&
            ArgumentValue(arguments, L"-bufsize") == L"27840000",
        "FFmpeg uses explicit CBR rate controls");
    const auto filter = ArgumentValue(arguments, L"-vf");
    passed &= Expect(
        filter.has_value() &&
            *filter ==
                L"crop=1080:1080:420:420,"
                L"scale=in_range=pc:out_range=tv:out_color_matrix=bt709,"
                L"format=nv12,setparams=range=limited:color_primaries=bt709:"
                L"color_trc=bt709:colorspace=bt709",
        "filter crops, explicitly converts full-range RGB to limited BT.709, and tags output");
    passed &= Expect(
        ArgumentValue(arguments, L"-colorspace") == L"bt709" &&
            ArgumentValue(arguments, L"-color_primaries") == L"bt709" &&
            ArgumentValue(arguments, L"-color_trc") == L"bt709" &&
            ArgumentValue(arguments, L"-color_range") == L"tv",
        "FFmpeg output carries complete BT.709 metadata");

    const FfmpegInputSpec concatInput{
        FfmpegInputKind::FfconcatManifest,
        L"manifest.ffconcat",
        0,
    };
    const std::vector<std::wstring> fullFrameArguments = BuildFfmpegArguments(
        concatInput,
        L"output.mp4",
        1U,
        60.0,
        10'000'000ULL,
        {0U, 0U, 1920U, 1920U},
        1920U,
        1920U);
    const auto fullFrameFilter = ArgumentValue(fullFrameArguments, L"-vf");
    passed &= Expect(
        fullFrameFilter.has_value() &&
            *fullFrameFilter ==
                L"scale=in_range=pc:out_range=tv:out_color_matrix=bt709,"
                L"format=nv12,setparams=range=limited:color_primaries=bt709:"
                L"color_trc=bt709:colorspace=bt709",
        "full even source begins with explicit RGB to limited BT.709 conversion");
    passed &= Expect(
        ArgumentValue(fullFrameArguments, L"-f") == L"concat" &&
            ArgumentValue(fullFrameArguments, L"-safe") == L"0" &&
            ArgumentValue(fullFrameArguments, L"-i") ==
                concatInput.path.wstring() &&
            !ArgumentValue(fullFrameArguments, L"-pattern_type").has_value() &&
            !ArgumentValue(fullFrameArguments, L"-threads").has_value(),
        "non-contiguous input safely falls back to the concat contract");

    return passed ? 0 : 1;
}
