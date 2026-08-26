#pragma once

#include "Export/ExportTypes.h"

#include <memory>

namespace zt::sequence::exporting {

class FfmpegExportController final {
public:
    FfmpegExportController();
    ~FfmpegExportController();

    FfmpegExportController(const FfmpegExportController&) = delete;
    FfmpegExportController& operator=(const FfmpegExportController&) = delete;
    FfmpegExportController(FfmpegExportController&&) = delete;
    FfmpegExportController& operator=(FfmpegExportController&&) = delete;

    [[nodiscard]] bool Start(FfmpegExportRequest request);
    void RequestCancel() noexcept;
    [[nodiscard]] ExportProgressSnapshot Snapshot() const;
    void Shutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence::exporting
