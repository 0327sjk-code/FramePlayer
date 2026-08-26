#include "Export/FfconcatManifest.h"

#include "Export/ExportPolicy.h"
#include "Platform/Utf8.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

namespace zt::sequence::exporting {
namespace {

inline constexpr std::array<unsigned char, 8> kPngSignature{
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
};

[[nodiscard]] std::uint32_t ReadBigEndian32(
    const unsigned char* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::string PathError(
    const char* prefix,
    const std::filesystem::path& path) {
    const std::string pathUtf8 = WideToUtf8(path.wstring());
    return std::string(prefix) + (pathUtf8.empty() ? "<path>" : pathUtf8);
}

}  // namespace

std::optional<PngDimensions> ReadPngDimensions(
    const std::filesystem::path& pngPath,
    std::string& errorUtf8) noexcept {
    errorUtf8.clear();
    try {
        std::ifstream input(pngPath, std::ios::binary);
        if (!input) {
            errorUtf8 = PathError("无法读取 PNG：", pngPath);
            return std::nullopt;
        }

        std::array<unsigned char, 24> header{};
        input.read(
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
            !std::equal(
                kPngSignature.begin(),
                kPngSignature.end(),
                header.begin()) ||
            ReadBigEndian32(header.data() + 8U) != 13U ||
            header[12] != static_cast<unsigned char>('I') ||
            header[13] != static_cast<unsigned char>('H') ||
            header[14] != static_cast<unsigned char>('D') ||
            header[15] != static_cast<unsigned char>('R')) {
            errorUtf8 = PathError("PNG 头无效：", pngPath);
            return std::nullopt;
        }

        const PngDimensions dimensions{
            ReadBigEndian32(header.data() + 16U),
            ReadBigEndian32(header.data() + 20U),
        };
        if (dimensions.width == 0U || dimensions.height == 0U) {
            errorUtf8 = PathError("PNG 尺寸无效：", pngPath);
            return std::nullopt;
        }
        return dimensions;
    } catch (...) {
        errorUtf8 = PathError("检查 PNG 失败：", pngPath);
        return std::nullopt;
    }
}

bool WriteFfconcatManifest(
    const std::filesystem::path& manifestPath,
    const SequenceExportSnapshot& sequence,
    std::string& errorUtf8) noexcept {
    errorUtf8.clear();
    try {
        const auto text = BuildFfconcatManifestText(sequence);
        if (!text) {
            errorUtf8 = "无法生成 FFmpeg 序列清单";
            return false;
        }

        std::ofstream output(
            manifestPath,
            std::ios::binary | std::ios::trunc);
        if (!output) {
            errorUtf8 = PathError("无法创建导出清单：", manifestPath);
            return false;
        }
        output.write(text->data(), static_cast<std::streamsize>(text->size()));
        output.flush();
        if (!output.good()) {
            errorUtf8 = PathError("写入导出清单失败：", manifestPath);
            return false;
        }
        return true;
    } catch (...) {
        errorUtf8 = PathError("创建导出清单失败：", manifestPath);
        return false;
    }
}

}  // namespace zt::sequence::exporting
