#pragma once

#include <windows.h>

#include <filesystem>

namespace zt::sequence::updating::detail {

[[nodiscard]] std::filesystem::path CurrentExecutablePath(DWORD* error);

[[nodiscard]] bool ValidateExecutablePath(
    const std::filesystem::path& executable,
    bool requireProductFileName,
    DWORD* error);

[[nodiscard]] bool PathsEqualInsensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept;

[[nodiscard]] std::filesystem::path BackupPathFor(
    const std::filesystem::path& targetExecutable);

[[nodiscard]] bool VerifyInstalledCopy(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    DWORD* error) noexcept;

[[nodiscard]] bool NormalizeInstalledExecutableAttributes(
    const std::filesystem::path& executable,
    DWORD* error) noexcept;

[[nodiscard]] bool IsOwnedTemporaryUpdateExecutable(
    const std::filesystem::path& executable,
    DWORD* error);

void RemoveFileBestEffort(const std::filesystem::path& path) noexcept;

}  // namespace zt::sequence::updating::detail
