#pragma once

#include "Core/PlayerTypes.h"
#include "Platform/DefaultFileLauncher.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

struct ImVec2;

namespace zt::sequence {

class ComparisonPlayer;
class FrameTexture;
namespace exporting {
class FfmpegExportController;
}
namespace overlay {
class MaskOverlayTexture;
}

enum class OnlineUpdatePhase : std::uint8_t {
    Unavailable,
    Idle,
    Checking,
    Downloading,
    UpToDate,
    ReadyToInstall,
    Failed,
};

struct OnlineUpdateView final {
    OnlineUpdatePhase phase = OnlineUpdatePhase::Unavailable;
    std::string currentVersion;
    std::string latestVersion;
    std::string statusUtf8;
    std::uint64_t downloadedBytes = 0U;
    std::uint64_t totalBytes = 0U;
};

struct UiActions {
    std::function<std::optional<std::filesystem::path>()> chooseFolder;
    std::function<std::optional<std::filesystem::path>()> chooseExportFolder;
    std::function<std::optional<std::filesystem::path>()>
        chooseMaskOverlayImage;
    std::function<platform::FileLaunchResult(
        const std::filesystem::path&)> openFile;
    std::function<void()> requestClose;
    std::function<void()> openCurrentSequence;
    std::function<bool()> hasCurrentSequence;
    std::function<OnlineUpdateView()> getOnlineUpdateView;
    std::function<void()> checkForUpdates;
    std::function<void()> installOnlineUpdate;
};

class PlayerUI final {
public:
    PlayerUI();
    ~PlayerUI();

    PlayerUI(const PlayerUI&) = delete;
    PlayerUI& operator=(const PlayerUI&) = delete;

    void ApplyCodexStyle();
    void SetUiScale(float scale);
    void ReportRendererError(std::string_view detail);
    void Render(
        ComparisonPlayer& player,
        FrameTexture& primaryFrameTexture,
        FrameTexture& secondaryFrameTexture,
        overlay::MaskOverlayTexture& maskOverlayTexture,
        exporting::FfmpegExportController& exporter,
        const UiActions& actions);
    [[nodiscard]] bool IsSecondaryViewportAtClientPoint(
        std::int32_t clientX,
        std::int32_t clientY) const noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace zt::sequence
