#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace zt::sequence::exporting {

using FfmpegProgressCallback =
    std::function<void(std::string_view key, std::string_view value)>;

struct FfmpegProcessResult final {
    bool started = false;
    bool cancelled = false;
    std::uint32_t exitCode = 0;
    std::string diagnosticUtf8;
};

[[nodiscard]] std::optional<std::filesystem::path> FindFfmpegExecutable(
    std::string& errorUtf8) noexcept;

class FfmpegProcess final {
public:
    FfmpegProcess();
    ~FfmpegProcess();

    FfmpegProcess(const FfmpegProcess&) = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;

    [[nodiscard]] FfmpegProcessResult Run(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        std::stop_token stopToken,
        const FfmpegProgressCallback& progressCallback) noexcept;

    void RequestTerminate() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence::exporting
