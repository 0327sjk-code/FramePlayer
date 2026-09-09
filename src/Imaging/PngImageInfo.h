#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zt::sequence {

struct PngImageInfo final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

// Reads only the PNG signature and IHDR dimensions. Pixel decoding remains in
// WicImageDecoder so preview and export validation share one lightweight,
// pixel-buffer-free file contract.
[[nodiscard]] std::optional<PngImageInfo> ReadPngImageInfo(
    const std::filesystem::path& pngPath,
    std::string& errorUtf8) noexcept;

}  // namespace zt::sequence
