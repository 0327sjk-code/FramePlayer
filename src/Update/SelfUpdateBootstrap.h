#pragma once

#include "Update/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace zt::sequence::updating {

enum class BootstrapStage : std::uint8_t {
    None,
    ValidateRequest,
    WaitForPreviousProcess,
    PreservePreviousExecutable,
    InstallExecutable,
    LaunchBootstrapExecutable,
    LaunchInstalledExecutable,
    WaitForInstalledExecutableHealth,
    SignalInstalledExecutableReady,
    RestartPreviousExecutable,
    Rollback,
};

struct ApplyUpdateRequest final {
    std::filesystem::path targetExecutable;
    std::uint32_t parentProcessId{};
    Sha256Digest expectedChecksum{};
};

struct BootstrapResult final {
    bool success{};
    BootstrapStage stage{BootstrapStage::None};
    std::uint32_t nativeError{};
    std::uint32_t rollbackError{};
    std::wstring message;
};

// Runs inside the downloaded executable. The caller must invoke this before
// creating the application's single-instance mutex.
[[nodiscard]] BootstrapResult ApplyUpdate(
    const ApplyUpdateRequest& request) noexcept;

// Runs in the normal application after a verified download is ready. It starts
// that executable in bootstrap mode; the caller can then close the old app.
[[nodiscard]] BootstrapResult LaunchApplyUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    std::uint32_t parentProcessId,
    const Sha256Digest& expectedChecksum) noexcept;

// Signals only after the installed process completed initialization and
// presented its first frame successfully.
[[nodiscard]] BootstrapResult SignalUpdateReady(
    std::wstring_view healthEventName) noexcept;

// Runs inside the newly installed executable before normal startup. Cleanup is
// deliberately best-effort and never prevents the player from launching.
void CleanupUpdateArtifacts(
    const std::filesystem::path& bootstrapExecutable,
    std::uint32_t bootstrapProcessId) noexcept;

}  // namespace zt::sequence::updating
