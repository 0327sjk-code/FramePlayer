#include "Export/Image2SequenceInput.h"

#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using zt::sequence::FrameFile;
using zt::sequence::SequenceExportSnapshot;
using zt::sequence::exporting::DetectImage2SequenceInput;
using zt::sequence::exporting::Image2SequenceInput;

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

[[nodiscard]] SequenceExportSnapshot MakeSequence(
    const std::filesystem::path& folder,
    const std::vector<std::wstring>& fileNames,
    const std::uint32_t startFrame,
    const std::uint32_t endFrame) {
    SequenceExportSnapshot sequence;
    sequence.sourceFolder = folder;
    sequence.inclusiveRange = {startFrame, endFrame};
    for (const std::wstring& fileName : fileNames) {
        FrameFile frame;
        frame.path = folder / fileName;
        frame.relativePath = fileName;
        sequence.orderedPngFrames.emplace_back(std::move(frame));
    }
    return sequence;
}

[[nodiscard]] bool Matches(
    const std::optional<Image2SequenceInput>& actual,
    const std::filesystem::path& pattern,
    const int startNumber,
    const std::uint32_t digitWidth,
    const char* message) {
    return Expect(
        actual.has_value() && actual->patternPath == pattern &&
            actual->startNumber == startNumber &&
            actual->digitWidth == digitWidth,
        message);
}

}  // namespace

int main() {
    bool passed = true;

    const std::filesystem::path realFolder = L"F:\\Render\\众人兜鱼";
    const SequenceExportSnapshot realFormat = MakeSequence(
        realFolder,
        {
            L"MainSeq.0516.png",
            L"MainSeq.0517.png",
            L"MainSeq.0518.png",
            L"MainSeq.0519.png",
        },
        0U,
        3U);
    passed &= Matches(
        DetectImage2SequenceInput(realFormat),
        realFolder / L"MainSeq.%04d.png",
        516,
        4U,
        "real MainSeq format maps to an image2 pattern");

    SequenceExportSnapshot customRange = realFormat;
    customRange.inclusiveRange = {1U, 3U};
    passed &= Matches(
        DetectImage2SequenceInput(customRange),
        realFolder / L"MainSeq.%04d.png",
        517,
        4U,
        "custom inclusive range maps to its real first filename number");

    const SequenceExportSnapshot missingInside = MakeSequence(
        L"D:\\Frames",
        {L"Frame.0001.png", L"Frame.0003.png", L"Frame.0004.png"},
        0U,
        2U);
    passed &= Expect(
        !DetectImage2SequenceInput(missingInside).has_value(),
        "a gap inside the selected range falls back to concat");

    const SequenceExportSnapshot multipleRuns = MakeSequence(
        L"D:\\Frames",
        {
            L"Shot.0516.Take.01.png",
            L"Shot.0517.Take.01.png",
            L"Shot.0518.Take.01.png",
        },
        0U,
        2U);
    passed &= Matches(
        DetectImage2SequenceInput(multipleRuns),
        L"D:\\Frames\\Shot.%04d.Take.01.png",
        516,
        4U,
        "digit runs are tried right to left until the continuous run is found");

    const SequenceExportSnapshot percentLiterals = MakeSequence(
        L"D:\\100%\\Frames",
        {
            L"Shot%Pass.0007.png",
            L"Shot%Pass.0008.png",
        },
        0U,
        1U);
    passed &= Matches(
        DetectImage2SequenceInput(percentLiterals),
        L"D:\\100%%\\Frames\\Shot%%Pass.%04d.png",
        7,
        4U,
        "literal percent characters are escaped for image2");

    const SequenceExportSnapshot gapOutsideRange = MakeSequence(
        L"D:\\Frames",
        {
            L"Frame.0001.png",
            L"Frame.0003.png",
            L"Frame.0004.png",
            L"Frame.0005.png",
        },
        1U,
        3U);
    passed &= Matches(
        DetectImage2SequenceInput(gapOutsideRange),
        L"D:\\Frames\\Frame.%04d.png",
        3,
        4U,
        "gaps outside the selected range do not disable the fast path");

    const std::wstring maximum = std::to_wstring(
        std::numeric_limits<int>::max());
    const SequenceExportSnapshot overflow = MakeSequence(
        L"D:\\Frames",
        {
            L"Frame." + maximum + L".png",
            L"Frame.2147483648.png",
        },
        0U,
        1U);
    passed &= Expect(
        !DetectImage2SequenceInput(overflow).has_value(),
        "a selected range that would exceed FFmpeg INT_MAX falls back");

    const SequenceExportSnapshot differentDirectories = [] {
        SequenceExportSnapshot sequence;
        sequence.inclusiveRange = {0U, 1U};
        sequence.orderedPngFrames = {
            FrameFile{L"D:\\A\\Frame.0001.png"},
            FrameFile{L"D:\\B\\Frame.0002.png"},
        };
        return sequence;
    }();
    passed &= Expect(
        !DetectImage2SequenceInput(differentDirectories).has_value(),
        "all selected frames must be in the same directory");

    const SequenceExportSnapshot changedWidth = MakeSequence(
        L"D:\\Frames",
        {L"Frame.99.png", L"Frame.100.png"},
        0U,
        1U);
    passed &= Expect(
        !DetectImage2SequenceInput(changedWidth).has_value(),
        "the numeric run must retain a fixed width");

    return passed ? 0 : 1;
}
