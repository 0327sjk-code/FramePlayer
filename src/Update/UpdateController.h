#pragma once

#include "Update/SelfUpdateBootstrap.h"
#include "Update/UpdateTypes.h"

#include <memory>
#include <optional>
#include <string>

namespace zt::sequence::updating {

class UpdateCoordinator;

// UI-thread facade around the worker coordinator and self-update bootstrap.
// It intentionally has no dependency on playback, decoding, rendering, or UI.
class UpdateController final {
public:
    UpdateController();
    ~UpdateController();

    UpdateController(const UpdateController&) = delete;
    UpdateController& operator=(const UpdateController&) = delete;

    [[nodiscard]] bool Initialize() noexcept;
    void Tick() noexcept;
    [[nodiscard]] bool CheckForUpdates() noexcept;
    [[nodiscard]] BootstrapResult BeginInstall() noexcept;
    [[nodiscard]] UpdateSnapshot Snapshot() const noexcept;
    [[nodiscard]] bool InstallWasLaunched() const noexcept;
    void Shutdown() noexcept;

private:
    void PublishLocalFailure(
        UpdateErrorCode code,
        std::wstring message,
        std::uint32_t nativeCode = 0U) noexcept;
    void RemoveDownloadedFileBestEffort(
        const std::filesystem::path& file) const noexcept;

    std::unique_ptr<UpdateCoordinator> coordinator_;
    std::optional<UpdateSnapshot> localSnapshot_;
    bool downloadRequested_{};
    bool installLaunched_{};
    bool shuttingDown_{};
};

}  // namespace zt::sequence::updating
