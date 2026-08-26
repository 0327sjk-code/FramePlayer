#include "Core/SequenceScanner.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace zt::sequence {
namespace {

[[nodiscard]] bool IsAsciiDigit(const wchar_t value) noexcept {
    return value >= L'0' && value <= L'9';
}

[[nodiscard]] wchar_t FoldCase(const wchar_t value) noexcept {
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(value)));
}

[[nodiscard]] int CompareDigitRuns(
    const std::wstring_view left,
    std::size_t& leftOffset,
    const std::wstring_view right,
    std::size_t& rightOffset) noexcept {
    const std::size_t leftRunStart = leftOffset;
    const std::size_t rightRunStart = rightOffset;

    while (leftOffset < left.size() && IsAsciiDigit(left[leftOffset])) {
        ++leftOffset;
    }
    while (rightOffset < right.size() && IsAsciiDigit(right[rightOffset])) {
        ++rightOffset;
    }

    std::size_t leftSignificant = leftRunStart;
    std::size_t rightSignificant = rightRunStart;
    while (leftSignificant < leftOffset && left[leftSignificant] == L'0') {
        ++leftSignificant;
    }
    while (rightSignificant < rightOffset && right[rightSignificant] == L'0') {
        ++rightSignificant;
    }

    const std::size_t leftDigits = leftOffset - leftSignificant;
    const std::size_t rightDigits = rightOffset - rightSignificant;
    if (leftDigits != rightDigits) {
        return leftDigits < rightDigits ? -1 : 1;
    }

    for (std::size_t index = 0; index < leftDigits; ++index) {
        const wchar_t leftDigit = left[leftSignificant + index];
        const wchar_t rightDigit = right[rightSignificant + index];
        if (leftDigit != rightDigit) {
            return leftDigit < rightDigit ? -1 : 1;
        }
    }

    const std::size_t leftRunLength = leftOffset - leftRunStart;
    const std::size_t rightRunLength = rightOffset - rightRunStart;
    if (leftRunLength != rightRunLength) {
        return leftRunLength < rightRunLength ? -1 : 1;
    }
    return 0;
}

[[nodiscard]] bool IsPngExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), FoldCase);
    return extension == L".png";
}

}  // namespace

bool NaturalPathLess(const std::wstring_view left, const std::wstring_view right) noexcept {
    std::size_t leftOffset = 0;
    std::size_t rightOffset = 0;

    while (leftOffset < left.size() && rightOffset < right.size()) {
        if (IsAsciiDigit(left[leftOffset]) && IsAsciiDigit(right[rightOffset])) {
            const int digitComparison = CompareDigitRuns(left, leftOffset, right, rightOffset);
            if (digitComparison != 0) {
                return digitComparison < 0;
            }
            continue;
        }

        const wchar_t foldedLeft = FoldCase(left[leftOffset]);
        const wchar_t foldedRight = FoldCase(right[rightOffset]);
        if (foldedLeft != foldedRight) {
            return foldedLeft < foldedRight;
        }
        ++leftOffset;
        ++rightOffset;
    }

    if (left.size() != right.size()) {
        return left.size() < right.size();
    }
    // Preserve a strict deterministic ordering for names that differ only by case.
    return left < right;
}

SequenceScanResult ScanPngFolder(const std::filesystem::path& folder) {
    SequenceScanResult result;
    std::error_code error;

    if (!std::filesystem::exists(folder, error) || error) {
        result.errorUtf8 = "文件夹不存在或无法访问";
        return result;
    }
    if (!std::filesystem::is_directory(folder, error) || error) {
        result.errorUtf8 = "选择的路径不是文件夹";
        return result;
    }

    std::filesystem::directory_iterator iterator(
        folder,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::directory_iterator end;
    if (error) {
        result.errorUtf8 = "无法读取文件夹内容: " + error.message();
        return result;
    }

    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            result.errorUtf8 = "扫描文件夹时发生错误: " + error.message();
            return result;
        }

        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError || !IsPngExtension(entry.path())) {
            continue;
        }

        FrameFile frame;
        frame.path = entry.path();
        frame.relativePath = entry.path().filename().wstring();
        frame.fileSizeBytes = entry.file_size(entryError);
        if (entryError) {
            frame.fileSizeBytes = 0;
            entryError.clear();
        }
        frame.lastWriteTime = entry.last_write_time(entryError);
        if (entryError) {
            frame.lastWriteTime = {};
        }
        result.frames.emplace_back(std::move(frame));
    }

    if (error) {
        result.frames.clear();
        result.errorUtf8 = "扫描文件夹时发生错误: " + error.message();
        return result;
    }

    if (result.frames.empty()) {
        result.errorUtf8 = "当前文件夹中没有 PNG 图片";
        return result;
    }

    std::stable_sort(
        result.frames.begin(),
        result.frames.end(),
        [](const FrameFile& left, const FrameFile& right) {
            return NaturalPathLess(left.relativePath, right.relativePath);
        });
    return result;
}

}  // namespace zt::sequence
