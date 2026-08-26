#pragma once

#include <filesystem>
#include <optional>

namespace zt::sequence::user_settings {

// The application persists only lightweight path preferences. Decoded frames
// and image caches remain memory-only.
[[nodiscard]] std::optional<std::filesystem::path> LoadExportFolder() noexcept;

[[nodiscard]] bool SaveExportFolder(
    const std::filesystem::path& exportFolder) noexcept;

[[nodiscard]] std::optional<std::filesystem::path>
LoadLastSequenceFolder() noexcept;

[[nodiscard]] bool SaveLastSequenceFolder(
    const std::filesystem::path& sequenceFolder) noexcept;

}  // namespace zt::sequence::user_settings
