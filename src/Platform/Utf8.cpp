#include "Platform/Utf8.h"

#include <windows.h>

#include <limits>

namespace zt::sequence {
namespace {

template <typename Character>
[[nodiscard]] bool FitsWindowsStringLength(const std::basic_string_view<Character> value) noexcept {
    return value.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

}  // namespace

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (!FitsWindowsStringLength(value)) {
        return {};
    }

    const int sourceLength = static_cast<int>(value.size());
    const int requiredLength = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (requiredLength <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(requiredLength), '\0');
    const int convertedLength = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        result.data(),
        requiredLength,
        nullptr,
        nullptr);
    return convertedLength == requiredLength ? result : std::string{};
}

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (!FitsWindowsStringLength(value)) {
        return {};
    }

    const int sourceLength = static_cast<int>(value.size());
    const int requiredLength = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        nullptr,
        0);
    if (requiredLength <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(requiredLength), L'\0');
    const int convertedLength = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        sourceLength,
        result.data(),
        requiredLength);
    return convertedLength == requiredLength ? result : std::wstring{};
}

}  // namespace zt::sequence
