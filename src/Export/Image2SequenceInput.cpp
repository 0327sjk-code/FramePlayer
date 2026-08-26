#include "Export/Image2SequenceInput.h"

#include <limits>
#include <string>
#include <string_view>

namespace zt::sequence::exporting {
namespace {

[[nodiscard]] bool IsAsciiDigit(const wchar_t character) noexcept {
    return character >= L'0' && character <= L'9';
}

[[nodiscard]] bool IsPngExtension(
    const std::filesystem::path& path) noexcept {
    const std::wstring extension = path.extension().wstring();
    return extension.size() == 4U && extension[0] == L'.' &&
        (extension[1] == L'p' || extension[1] == L'P') &&
        (extension[2] == L'n' || extension[2] == L'N') &&
        (extension[3] == L'g' || extension[3] == L'G');
}

[[nodiscard]] std::optional<int> ParseStartNumber(
    const std::wstring_view digits) noexcept {
    if (digits.empty()) {
        return std::nullopt;
    }

    constexpr int maximum = std::numeric_limits<int>::max();
    int value = 0;
    for (const wchar_t character : digits) {
        if (!IsAsciiDigit(character)) {
            return std::nullopt;
        }
        const int digit = static_cast<int>(character - L'0');
        if (value > (maximum - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

[[nodiscard]] std::optional<std::wstring> FixedWidthNumber(
    const int value,
    const std::size_t width) {
    if (value < 0 || width == 0U) {
        return std::nullopt;
    }

    std::wstring result = std::to_wstring(value);
    if (result.size() > width) {
        return std::nullopt;
    }
    result.insert(result.begin(), width - result.size(), L'0');
    return result;
}

[[nodiscard]] std::wstring EscapeImage2LiteralPercents(
    const std::wstring_view text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    for (const wchar_t character : text) {
        escaped.push_back(character);
        if (character == L'%') {
            escaped.push_back(L'%');
        }
    }
    return escaped;
}

[[nodiscard]] std::filesystem::path BuildPatternPath(
    const std::filesystem::path& directory,
    const std::wstring_view prefix,
    const std::wstring_view suffix,
    const std::size_t digitWidth) {
    std::wstring patternFileName = EscapeImage2LiteralPercents(prefix);
    patternFileName.append(L"%0");
    patternFileName.append(std::to_wstring(digitWidth));
    patternFileName.push_back(L'd');
    patternFileName.append(EscapeImage2LiteralPercents(suffix));

    const std::filesystem::path escapedDirectory(
        EscapeImage2LiteralPercents(directory.wstring()));
    return escapedDirectory / std::filesystem::path(
        std::move(patternFileName));
}

[[nodiscard]] std::optional<Image2SequenceInput> TryDigitRun(
    const SequenceExportSnapshot& sequence,
    const std::size_t selectedStart,
    const std::size_t selectedEnd,
    const std::filesystem::path& directory,
    const std::wstring_view firstFileName,
    const std::size_t runStart,
    const std::size_t runEnd) {
    const std::size_t digitWidth = runEnd - runStart;
    if (digitWidth == 0U ||
        digitWidth > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    const auto startNumber = ParseStartNumber(
        firstFileName.substr(runStart, digitWidth));
    if (!startNumber) {
        return std::nullopt;
    }

    const std::wstring_view prefix = firstFileName.substr(0U, runStart);
    const std::wstring_view suffix = firstFileName.substr(runEnd);
    constexpr std::size_t maximumNumber = static_cast<std::size_t>(
        std::numeric_limits<int>::max());

    for (std::size_t index = selectedStart; index <= selectedEnd; ++index) {
        const FrameFile& frame = sequence.orderedPngFrames[index];
        if (!IsPngExtension(frame.path) ||
            frame.path.parent_path().lexically_normal() != directory) {
            return std::nullopt;
        }

        const std::size_t offset = index - selectedStart;
        const std::size_t startAsSize = static_cast<std::size_t>(*startNumber);
        if (offset > maximumNumber - startAsSize) {
            return std::nullopt;
        }
        const int expectedNumber = static_cast<int>(startAsSize + offset);
        const auto digits = FixedWidthNumber(expectedNumber, digitWidth);
        if (!digits) {
            return std::nullopt;
        }

        std::wstring expectedFileName(prefix);
        expectedFileName.append(*digits);
        expectedFileName.append(suffix);
        if (frame.path.filename().wstring() != expectedFileName) {
            return std::nullopt;
        }
    }

    return Image2SequenceInput{
        BuildPatternPath(directory, prefix, suffix, digitWidth),
        *startNumber,
        static_cast<std::uint32_t>(digitWidth),
    };
}

}  // namespace

std::optional<Image2SequenceInput> DetectImage2SequenceInput(
    const SequenceExportSnapshot& sequence) {
    if (sequence.orderedPngFrames.empty()) {
        return std::nullopt;
    }

    const std::size_t selectedStart = sequence.inclusiveRange.startFrame;
    const std::size_t selectedEnd = sequence.inclusiveRange.endFrame;
    if (selectedStart > selectedEnd ||
        selectedEnd >= sequence.orderedPngFrames.size()) {
        return std::nullopt;
    }

    const std::filesystem::path& firstPath =
        sequence.orderedPngFrames[selectedStart].path;
    if (!IsPngExtension(firstPath)) {
        return std::nullopt;
    }

    const std::filesystem::path directory =
        firstPath.parent_path().lexically_normal();
    const std::wstring firstFileName = firstPath.filename().wstring();
    std::size_t searchEnd = firstFileName.size();
    while (searchEnd > 0U) {
        while (searchEnd > 0U &&
               !IsAsciiDigit(firstFileName[searchEnd - 1U])) {
            --searchEnd;
        }
        if (searchEnd == 0U) {
            break;
        }

        const std::size_t runEnd = searchEnd;
        while (searchEnd > 0U &&
               IsAsciiDigit(firstFileName[searchEnd - 1U])) {
            --searchEnd;
        }
        const std::size_t runStart = searchEnd;
        if (auto detected = TryDigitRun(
                sequence,
                selectedStart,
                selectedEnd,
                directory,
                firstFileName,
                runStart,
                runEnd)) {
            return detected;
        }
    }
    return std::nullopt;
}

}  // namespace zt::sequence::exporting
