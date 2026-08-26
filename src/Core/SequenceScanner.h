#pragma once

#include "Core/PlayerTypes.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zt::sequence {

struct SequenceScanResult {
    std::vector<FrameFile> frames;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return errorUtf8.empty() && !frames.empty();
    }
};

// Scans only the selected directory. Nested folders intentionally form separate
// sequences so a folder load has a stable and predictable frame set.
[[nodiscard]] SequenceScanResult ScanPngFolder(const std::filesystem::path& folder);

// Numeric runs are compared by numeric magnitude without converting them to a
// fixed-width integer, so arbitrarily long frame numbers remain deterministic.
[[nodiscard]] bool NaturalPathLess(std::wstring_view left, std::wstring_view right) noexcept;

}  // namespace zt::sequence
