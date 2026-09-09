#pragma once

#include "Core/PlayerTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace zt::sequence::exporting {

struct NormalizedCrop final {
    double minimumX = 0.0;
    double minimumY = 0.0;
    double maximumX = 1.0;
    double maximumY = 1.0;

    [[nodiscard]] bool operator==(const NormalizedCrop&) const noexcept = default;
};

struct FfmpegExportRequest final {
    ExportSourceSnapshot source;
    NormalizedCrop crop;
    std::filesystem::path outputFolder;
    std::optional<std::filesystem::path> overlayImagePath;
};

enum class ExportState : std::uint8_t {
    Idle,
    Preparing,
    Running,
    Retrying,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct ExportProgressSnapshot final {
    std::uint64_t jobId = 0;
    ExportState state = ExportState::Idle;
    std::size_t completedFrames = 0;
    std::size_t totalFrames = 0;
    std::uint32_t attempt = 0;
    std::filesystem::path finalOutputPath;
    std::uint64_t outputBytes = 0;
    std::string statusUtf8;
    std::string errorUtf8;
};

}  // namespace zt::sequence::exporting
