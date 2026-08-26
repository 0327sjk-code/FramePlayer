#pragma once

#include <filesystem>
#include <optional>

struct HWND__;
using HWND = HWND__*;

namespace zt::sequence {

[[nodiscard]] std::optional<std::filesystem::path> ShowFolderPicker(HWND owner);
[[nodiscard]] std::optional<std::filesystem::path> ShowExportFolderPicker(HWND owner);

}  // namespace zt::sequence
