#pragma once

#include <filesystem>
#include <optional>

namespace zt::sequence::user_settings {

// The application persists only lightweight user preferences. Decoded frames
// and image caches remain memory-only.
[[nodiscard]] std::optional<std::filesystem::path> LoadExportFolder() noexcept;

[[nodiscard]] bool SaveExportFolder(
    const std::filesystem::path& exportFolder) noexcept;

[[nodiscard]] std::optional<std::filesystem::path>
LoadLastSequenceFolder() noexcept;

[[nodiscard]] bool SaveLastSequenceFolder(
    const std::filesystem::path& sequenceFolder) noexcept;

[[nodiscard]] std::optional<int>
LoadKeyboardShuttleSpeedPercent() noexcept;

[[nodiscard]] bool SaveKeyboardShuttleSpeedPercent(int percent) noexcept;

}  // namespace zt::sequence::user_settings
