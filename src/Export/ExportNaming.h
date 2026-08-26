#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace zt::sequence::exporting {

using ExportPathExists =
    std::function<bool(const std::filesystem::path& candidate)>;

// collisionIndex 0 produces "name.mp4". Positive values use a minimum
// two-digit suffix: "name01.mp4", "name02.mp4", ... "name100.mp4".
[[nodiscard]] std::filesystem::path BuildMp4ExportCandidate(
    const std::filesystem::path& exportDirectory,
    std::uint64_t collisionIndex);

// The predicate is queried only with final .mp4 candidates. Temporary files
// such as name.mp4.partial therefore never consume a final sequence number.
[[nodiscard]] std::filesystem::path FindAvailableMp4ExportPath(
    const std::filesystem::path& exportDirectory,
    const ExportPathExists& pathExists);

// Filesystem-backed convenience overload for production code.
[[nodiscard]] std::filesystem::path FindAvailableMp4ExportPath(
    const std::filesystem::path& exportDirectory);

}  // namespace zt::sequence::exporting
