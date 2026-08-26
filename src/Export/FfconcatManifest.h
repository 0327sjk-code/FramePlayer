#pragma once

#include "Core/PlayerTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zt::sequence::exporting {

struct PngDimensions final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] std::optional<PngDimensions> ReadPngDimensions(
    const std::filesystem::path& pngPath,
    std::string& errorUtf8) noexcept;

[[nodiscard]] bool WriteFfconcatManifest(
    const std::filesystem::path& manifestPath,
    const SequenceExportSnapshot& sequence,
    std::string& errorUtf8) noexcept;

}  // namespace zt::sequence::exporting
