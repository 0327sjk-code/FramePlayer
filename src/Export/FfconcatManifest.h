#pragma once

#include "Core/PlayerTypes.h"

#include <filesystem>
#include <string>

namespace zt::sequence::exporting {

[[nodiscard]] bool WriteFfconcatManifest(
    const std::filesystem::path& manifestPath,
    const SequenceExportSnapshot& sequence,
    std::string& errorUtf8) noexcept;

}  // namespace zt::sequence::exporting
