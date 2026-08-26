#pragma once

#include "Update/Sha256.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zt::sequence {

namespace update_command_line {

inline constexpr wchar_t ApplyUpdateArgument[] = L"--apply-update";
inline constexpr wchar_t TargetExecutablePrefix[] = L"--target-exe=";
inline constexpr wchar_t ParentProcessIdPrefix[] = L"--parent-pid=";
inline constexpr wchar_t ExpectedChecksumPrefix[] = L"--expected-sha256=";
inline constexpr wchar_t CleanupUpdatePrefix[] = L"--cleanup-update=";
inline constexpr wchar_t UpdateBootstrapProcessIdPrefix[] =
    L"--update-bootstrap-pid=";
inline constexpr wchar_t UpdateHealthEventPrefix[] =
    L"--update-health-event=";
inline constexpr wchar_t UpdateHealthEventNamePrefix[] =
    L"Local\\FramePlayer.UpdateHealth.";

}  // namespace update_command_line

struct UpdateCommandLineOptions final {
    bool valid{true};
    bool applyUpdate{};
    std::filesystem::path targetExecutable;
    std::uint32_t parentProcessId{};
    std::optional<updating::Sha256Digest> expectedChecksum;
    std::filesystem::path cleanupUpdateExecutable;
    std::uint32_t updateBootstrapProcessId{};
    std::wstring updateHealthEventName;
    std::wstring errorMessage;

    [[nodiscard]] bool HasCleanupRequest() const noexcept {
        return !cleanupUpdateExecutable.empty() &&
            updateBootstrapProcessId != 0U &&
            !updateHealthEventName.empty();
    }
};

[[nodiscard]] UpdateCommandLineOptions ParseUpdateCommandLine() noexcept;

}  // namespace zt::sequence
