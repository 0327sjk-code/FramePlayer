#include "Platform/DefaultFileLauncher.h"

#include "Platform/Utf8.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace zt::sequence::platform {
namespace {

[[nodiscard]] std::string DescribeWindowsError(const DWORD errorCode) {
    std::array<wchar_t, 512> message{};
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        message.data(),
        static_cast<DWORD>(message.size()),
        nullptr);
    if (length == 0U) {
        return "Windows 错误代码 " +
            std::to_string(static_cast<std::uint32_t>(errorCode));
    }

    std::size_t trimmedLength = static_cast<std::size_t>(length);
    while (trimmedLength > 0U) {
        const wchar_t character = message[trimmedLength - 1U];
        if (character != L'\r' && character != L'\n' &&
            character != L' ' && character != L'\t') {
            break;
        }
        --trimmedLength;
    }
    const std::string utf8 = WideToUtf8(
        std::wstring_view(message.data(), trimmedLength));
    return utf8.empty()
        ? "Windows 错误代码 " +
            std::to_string(static_cast<std::uint32_t>(errorCode))
        : utf8;
}

}  // namespace

FileLaunchResult OpenFileWithDefaultApplication(
    const std::filesystem::path& filePath) {
    if (filePath.empty()) {
        return {false, "没有可打开的视频路径"};
    }

    std::error_code fileError;
    const std::filesystem::file_status status =
        std::filesystem::status(filePath, fileError);
    if (fileError) {
        return {
            false,
            "无法检查视频文件（错误代码 " +
                std::to_string(fileError.value()) + "）"};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {false, "最近导出的视频已被移动或删除"};
    }

    const std::wstring nativePath = filePath.wstring();
    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    executeInfo.lpVerb = L"open";
    executeInfo.lpFile = nativePath.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    if (::ShellExecuteExW(&executeInfo) == FALSE) {
        const DWORD errorCode = ::GetLastError();
        return {
            false,
            "系统默认播放器未能打开视频：" +
                DescribeWindowsError(errorCode)};
    }
    return {true, {}};
}

}  // namespace zt::sequence::platform
