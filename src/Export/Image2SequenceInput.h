#pragma once

#include "Core/PlayerTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace zt::sequence::exporting {

// Describes a selected range that FFmpeg's image2 demuxer can read directly.
// patternPath contains an escaped image2 pattern, for example
// "F:\\Render\\Sequence\\MainSeq.%04d.png". Literal percent characters in
// directories and static filename text are represented as "%%".
struct Image2SequenceInput final {
    std::filesystem::path patternPath;
    int startNumber = 0;
    std::uint32_t digitWidth = 0;

    [[nodiscard]] bool operator==(
        const Image2SequenceInput&) const noexcept = default;
};

// Returns an image2 input only when every PNG in the selected inclusive range
// is in the same directory and follows one strictly consecutive, fixed-width
// decimal filename run. Candidate digit runs are tried from right to left.
[[nodiscard]] std::optional<Image2SequenceInput>
DetectImage2SequenceInput(const SequenceExportSnapshot& sequence);

}  // namespace zt::sequence::exporting
