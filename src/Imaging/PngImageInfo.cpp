#include "Imaging/PngImageInfo.h"

#include "Platform/Utf8.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace zt::sequence {
namespace {

inline constexpr std::array<unsigned char, 8> kPngSignature{
    0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
};

[[nodiscard]] std::uint32_t ReadBigEndian32(
    const unsigned char* const bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] std::string PathError(
    const char* const prefix,
    const std::filesystem::path& path) {
    const std::string pathUtf8 = WideToUtf8(path.wstring());
    return std::string(prefix) + (pathUtf8.empty() ? "<path>" : pathUtf8);
}

}  // namespace

std::optional<PngImageInfo> ReadPngImageInfo(
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

        const PngImageInfo info{
            ReadBigEndian32(header.data() + 16U),
            ReadBigEndian32(header.data() + 20U),
        };
        if (info.width == 0U || info.height == 0U) {
            errorUtf8 = PathError("PNG 尺寸无效：", pngPath);
            return std::nullopt;
        }
        return info;
    } catch (...) {
        errorUtf8 = PathError("检查 PNG 失败：", pngPath);
        return std::nullopt;
    }
}

}  // namespace zt::sequence
