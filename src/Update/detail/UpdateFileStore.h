#pragma once

#include "Update/Sha256.h"
#include "Update/detail/WinHttpTransport.h"

#include <cstdint>
#include <functional>

namespace zt::sequence::updating::detail {

using HttpTransferOperation =
    std::function<HttpTransferResult(const HttpChunkWriter& writer)>;

// Owns the transactional .part -> validated x64 PE -> final-file workflow.
class UpdateFileStore final {
public:
    explicit UpdateFileStore(std::uint64_t maximumBytes) noexcept;

    [[nodiscard]] ExecutableDownloadResult Acquire(
        const SemanticVersion& version,
        const Sha256Digest& expectedChecksum,
        const HttpTransferOperation& transfer) const noexcept;

private:
    std::uint64_t maximumBytes_{};
};

}  // namespace zt::sequence::updating::detail
