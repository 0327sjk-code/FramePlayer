#include "App/UpdateCommandLine.h"

#include <windows.h>
#include <shellapi.h>

#include <cerrno>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string_view>

namespace zt::sequence {
namespace {

[[nodiscard]] bool EqualsInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
        ::CompareStringOrdinal(
            left.data(),
            static_cast<int>(left.size()),
            right.data(),
            static_cast<int>(right.size()),
            TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool StartsWithInsensitive(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        ::CompareStringOrdinal(
            value.data(),
            static_cast<int>(prefix.size()),
            prefix.data(),
            static_cast<int>(prefix.size()),
            TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool ParseProcessId(
    const std::wstring_view value,
    std::uint32_t* const processId) noexcept {
    if (processId == nullptr || value.empty()) {
        return false;
    }
    try {
        const std::wstring text(value);
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::wcstoul(text.c_str(), &end, 10);
        if (errno == ERANGE || end == nullptr || end == text.c_str() ||
            *end != L'\0' || parsed == 0UL ||
            parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        *processId = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

void SetError(
    UpdateCommandLineOptions* const options,
    const std::wstring_view message) noexcept {
    if (options == nullptr || !options->valid) {
        return;
    }
    options->valid = false;
    try {
        options->errorMessage.assign(message);
    } catch (...) {
        options->errorMessage.clear();
    }
}

[[nodiscard]] bool AssignPath(
    const std::wstring_view value,
    bool* const seen,
    std::filesystem::path* const destination,
    UpdateCommandLineOptions* const options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"同一个路径参数不能重复。");
        return false;
    }
    if (value.empty()) {
        SetError(options, L"路径参数不能为空。");
        return false;
    }
    try {
        *destination = std::filesystem::path(std::wstring(value));
        *seen = true;
        return true;
    } catch (...) {
        SetError(options, L"路径参数无效。");
        return false;
    }
}

[[nodiscard]] bool AssignProcessId(
    const std::wstring_view value,
    bool* const seen,
    std::uint32_t* const destination,
    UpdateCommandLineOptions* const options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"同一个进程 ID 参数不能重复。");
        return false;
    }
    if (!ParseProcessId(value, destination)) {
        SetError(options, L"进程 ID 参数必须是有效的非零整数。");
        return false;
    }
    *seen = true;
    return true;
}

[[nodiscard]] bool IsValidHealthEventName(
    const std::wstring_view value) noexcept {
    constexpr std::wstring_view prefix(
        update_command_line::UpdateHealthEventNamePrefix,
        std::size(update_command_line::UpdateHealthEventNamePrefix) - 1U);
    constexpr std::size_t maximumCharacters = 240U;
    if (value.size() <= prefix.size() || value.size() > maximumCharacters ||
        value.substr(0U, prefix.size()) != prefix) {
        return false;
    }
    const std::wstring_view uniquePart = value.substr(prefix.size());
    if (uniquePart.front() == L'.' || uniquePart.back() == L'.') {
        return false;
    }
    for (const wchar_t character : uniquePart) {
        const bool alphaNumeric =
            (character >= L'0' && character <= L'9') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'a' && character <= L'z');
        if (!alphaNumeric && character != L'.' && character != L'_' &&
            character != L'-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AssignHealthEventName(
    const std::wstring_view value,
    bool* const seen,
    std::wstring* const destination,
    UpdateCommandLineOptions* const options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"--update-health-event 参数不能重复。");
        return false;
    }
    if (!IsValidHealthEventName(value)) {
        SetError(options, L"更新健康事件名称无效。");
        return false;
    }
    try {
        destination->assign(value);
        *seen = true;
        return true;
    } catch (...) {
        SetError(options, L"无法保存更新健康事件名称。");
        return false;
    }
}

[[nodiscard]] bool AssignExpectedChecksum(
    const std::wstring_view value,
    bool* const seen,
    std::optional<updating::Sha256Digest>* const destination,
    UpdateCommandLineOptions* const options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"--expected-sha256 参数不能重复。");
        return false;
    }
    if (value.size() != 64U) {
        SetError(options, L"更新 SHA-256 参数必须是 64 位十六进制字符。");
        return false;
    }
    std::string narrow;
    try {
        narrow.reserve(value.size());
        for (const wchar_t character : value) {
            if (character > 0x7FU) {
                SetError(options, L"更新 SHA-256 参数包含无效字符。");
                return false;
            }
            narrow.push_back(static_cast<char>(character));
        }
    } catch (...) {
        SetError(options, L"无法保存更新 SHA-256 参数。");
        return false;
    }
    const std::optional<updating::Sha256Digest> parsed =
        updating::ParseSha256(narrow);
    if (!parsed.has_value()) {
        SetError(options, L"更新 SHA-256 参数必须是 64 位十六进制字符。");
        return false;
    }
    *destination = *parsed;
    *seen = true;
    return true;
}

}  // namespace

UpdateCommandLineOptions ParseUpdateCommandLine() noexcept {
    UpdateCommandLineOptions options;
    int argumentCount = 0;
    LPWSTR* const arguments =
        ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        SetError(&options, L"无法解析启动参数。");
        return options;
    }

    bool targetSeen = false;
    bool parentProcessSeen = false;
    bool expectedChecksumSeen = false;
    bool cleanupExecutableSeen = false;
    bool bootstrapProcessSeen = false;
    bool healthEventSeen = false;

    for (int index = 1; index < argumentCount && options.valid; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (EqualsInsensitive(
                argument,
                update_command_line::ApplyUpdateArgument)) {
            if (options.applyUpdate) {
                SetError(&options, L"--apply-update 参数不能重复。");
            } else {
                options.applyUpdate = true;
            }
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::TargetExecutablePrefix)) {
            static_cast<void>(AssignPath(
                argument.substr(
                    std::size(update_command_line::TargetExecutablePrefix) - 1U),
                &targetSeen,
                &options.targetExecutable,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::ParentProcessIdPrefix)) {
            static_cast<void>(AssignProcessId(
                argument.substr(
                    std::size(update_command_line::ParentProcessIdPrefix) - 1U),
                &parentProcessSeen,
                &options.parentProcessId,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::ExpectedChecksumPrefix)) {
            static_cast<void>(AssignExpectedChecksum(
                argument.substr(
                    std::size(update_command_line::ExpectedChecksumPrefix) - 1U),
                &expectedChecksumSeen,
                &options.expectedChecksum,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::CleanupUpdatePrefix)) {
            static_cast<void>(AssignPath(
                argument.substr(
                    std::size(update_command_line::CleanupUpdatePrefix) - 1U),
                &cleanupExecutableSeen,
                &options.cleanupUpdateExecutable,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::UpdateBootstrapProcessIdPrefix)) {
            static_cast<void>(AssignProcessId(
                argument.substr(
                    std::size(
                        update_command_line::UpdateBootstrapProcessIdPrefix) - 1U),
                &bootstrapProcessSeen,
                &options.updateBootstrapProcessId,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                update_command_line::UpdateHealthEventPrefix)) {
            static_cast<void>(AssignHealthEventName(
                argument.substr(
                    std::size(update_command_line::UpdateHealthEventPrefix) - 1U),
                &healthEventSeen,
                &options.updateHealthEventName,
                &options));
            continue;
        }
        if (EqualsInsensitive(argument, L"--update-health-event")) {
            SetError(&options, L"--update-health-event 缺少事件名称。");
        }
    }
    ::LocalFree(arguments);

    if (!options.valid) {
        return options;
    }
    if (options.applyUpdate) {
        if (!targetSeen || !parentProcessSeen || !expectedChecksumSeen) {
            SetError(
                &options,
                L"更新模式必须同时提供目标路径、父进程 ID 和 SHA-256。");
        } else if (cleanupExecutableSeen || bootstrapProcessSeen ||
                   healthEventSeen) {
            SetError(&options, L"更新安装参数与更新清理参数不能同时使用。");
        }
        return options;
    }

    if (targetSeen || parentProcessSeen || expectedChecksumSeen) {
        SetError(&options, L"更新安装参数只能用于更新模式。");
    } else if (cleanupExecutableSeen != bootstrapProcessSeen ||
               cleanupExecutableSeen != healthEventSeen) {
        SetError(
            &options,
            L"更新清理必须同时提供 --cleanup-update、--update-bootstrap-pid 和 --update-health-event。");
    }
    return options;
}

}  // namespace zt::sequence
