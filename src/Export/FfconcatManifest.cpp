#include "Export/FfconcatManifest.h"

#include "Export/ExportPolicy.h"
#include "Platform/Utf8.h"

#include <fstream>
#include <system_error>

namespace zt::sequence::exporting {
namespace {

[[nodiscard]] std::string PathError(
    const char* prefix,
    const std::filesystem::path& path) {
    const std::string pathUtf8 = WideToUtf8(path.wstring());
    return std::string(prefix) + (pathUtf8.empty() ? "<path>" : pathUtf8);
}

}  // namespace

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
