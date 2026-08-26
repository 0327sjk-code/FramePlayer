#pragma once

#include "Update/UpdateTypes.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace zt::sequence::updating {

struct HttpTimeouts final {
    std::chrono::milliseconds resolve{5'000};
    std::chrono::milliseconds connect{10'000};
    std::chrono::milliseconds send{10'000};
    std::chrono::milliseconds receive{30'000};
};

struct GitHubUpdateClientOptions final {
    std::wstring versionUrl;
    std::wstring releaseDownloadBaseUrl;
    std::wstring userAgent{L"FramePlayer-Updater/1.0"};
    HttpTimeouts timeouts;
    std::uint64_t maxVersionBytes{256};
    std::uint64_t maxChecksumBytes{256};
    std::uint64_t maxExecutableBytes{512ULL * 1024ULL * 1024ULL};
    std::uint32_t maxRedirects{10};
    std::chrono::milliseconds progressInterval{100};
};

using DownloadProgressCallback = std::function<void(
    std::uint64_t downloadedBytes,
    std::optional<std::uint64_t> totalBytes)>;

// Performs blocking WinHTTP operations intended to run on a background thread.
// HTTPS is mandatory for both configured and final redirected URLs.
class GitHubUpdateClient final {
public:
    explicit GitHubUpdateClient(GitHubUpdateClientOptions options);
    ~GitHubUpdateClient();

    GitHubUpdateClient(const GitHubUpdateClient&) = delete;
    GitHubUpdateClient& operator=(const GitHubUpdateClient&) = delete;
    GitHubUpdateClient(GitHubUpdateClient&&) = delete;
    GitHubUpdateClient& operator=(GitHubUpdateClient&&) = delete;

    [[nodiscard]] VersionFetchResult FetchLatestVersion(
        std::stop_token stopToken = {}) noexcept;

    // Downloads through a .part file and publishes only a complete PE file to:
    // %TEMP%\FramePlayer-Update\<version>\FramePlayer.exe
    [[nodiscard]] ExecutableDownloadResult DownloadExecutable(
        const SemanticVersion& version,
        DownloadProgressCallback progress = {},
        std::stop_token stopToken = {}) noexcept;

    // Non-blocking. Closing the current request handle interrupts a blocking
    // WinHTTP send/receive operation; the client can be reused afterward.
    void Cancel() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence::updating
