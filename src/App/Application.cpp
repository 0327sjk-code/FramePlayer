#include "App/Application.h"
#include "App/ApplicationActivityPolicy.h"
#include "App/ProductInfo.h"

#include "Core/ComparisonPlayer.h"
#include "Export/FfmpegExportController.h"
#include "Platform/DefaultFileLauncher.h"
#include "Platform/FolderDialog.h"
#include "Platform/UserSettings.h"
#include "Platform/Utf8.h"
#include "Platform/Win32Window.h"
#include "Overlay/MaskOverlayTexture.h"
#include "Render/D3D11Renderer.h"
#include "Render/FrameTexture.h"
#include "UI/PlayerUI.h"
#include "Update/SelfUpdateBootstrap.h"
#include "Update/UpdateController.h"

#include <windows.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace zt::sequence {
namespace {

constexpr std::uint32_t kDefaultWindowWidth = 1440;
constexpr std::uint32_t kDefaultWindowHeight = 900;
constexpr float kDefaultDpi = 96.0F;
constexpr float kUiFontSize = 16.0F;
constexpr std::chrono::milliseconds kOccludedSleepDuration{16};

struct PendingSequencePreference final {
    std::filesystem::path folder;
    Generation previousGeneration = 0U;
};

[[nodiscard]] bool RefersToSamePath(
    const std::filesystem::path& first,
    const std::filesystem::path& second) noexcept {
    if (first.empty() || second.empty()) {
        return false;
    }
    std::error_code error;
    if (std::filesystem::equivalent(first, second, error)) {
        return true;
    }
    std::filesystem::path normalizedFirst = first.lexically_normal();
    std::filesystem::path normalizedSecond = second.lexically_normal();
    normalizedFirst.make_preferred();
    normalizedSecond.make_preferred();
    return ::_wcsicmp(
        normalizedFirst.c_str(),
        normalizedSecond.c_str()) == 0;
}

void ShowStartupFailure(const wchar_t* detail) {
    ::MessageBoxW(
        nullptr,
        detail,
        L"序列播放器",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

[[nodiscard]] std::optional<std::filesystem::path> FindChineseUiFont() {
    std::array<wchar_t, MAX_PATH> windowsDirectory{};
    const UINT length = ::GetWindowsDirectoryW(
        windowsDirectory.data(),
        static_cast<UINT>(windowsDirectory.size()));
    if (length == 0 || length >= windowsDirectory.size()) {
        return std::nullopt;
    }

    const std::filesystem::path fontDirectory =
        std::filesystem::path(std::wstring_view(windowsDirectory.data(), length)) / L"Fonts";
    constexpr std::array<const wchar_t*, 3> fontNames{
        L"msyh.ttc",
        L"msyhbd.ttc",
        L"simsun.ttc",
    };

    std::error_code error;
    for (const wchar_t* fontName : fontNames) {
        const std::filesystem::path candidate = fontDirectory / fontName;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
        error.clear();
    }
    return std::nullopt;
}

void ConfigureFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = nullptr;
    if (const auto fontPath = FindChineseUiFont(); fontPath.has_value()) {
        const std::string pathUtf8 = WideToUtf8(fontPath->wstring());
        if (!pathUtf8.empty()) {
            font = io.Fonts->AddFontFromFileTTF(
                pathUtf8.c_str(),
                kUiFontSize,
                nullptr,
                io.Fonts->GetGlyphRangesChineseFull());
        }
    }
    if (font == nullptr) {
        static_cast<void>(io.Fonts->AddFontDefault());
    }
}

void ApplyDpiScale(const std::uint32_t dpi, float& appliedScale) {
    const float nextScale = std::max(static_cast<float>(dpi) / kDefaultDpi, 0.5F);
    ImGuiStyle& style = ImGui::GetStyle();
    if (appliedScale > 0.0F) {
        style.ScaleAllSizes(nextScale / appliedScale);
    }
    style.FontScaleDpi = nextScale;
    appliedScale = nextScale;
}

[[nodiscard]] const char* RenderStatusText(const RenderStatus status) noexcept {
    switch (status) {
        case RenderStatus::ResizeFailed:
            return "交换链尺寸调整失败";
        case RenderStatus::RenderTargetFailed:
            return "渲染目标创建失败";
        case RenderStatus::DeviceRemoved:
            return "Direct3D 设备已移除";
        case RenderStatus::DeviceReset:
            return "Direct3D 设备已重置";
        case RenderStatus::PresentFailed:
            return "画面提交失败";
        case RenderStatus::Success:
        case RenderStatus::Occluded:
            return "Direct3D 渲染状态正常";
    }
    return "Direct3D 未知错误";
}

[[nodiscard]] bool IsFatalRendererStatus(const RenderStatus status) noexcept {
    return status == RenderStatus::RenderTargetFailed ||
        status == RenderStatus::DeviceRemoved ||
        status == RenderStatus::DeviceReset ||
        status == RenderStatus::PresentFailed;
}

[[nodiscard]] bool IsExportBusyForUpdate(
    const exporting::ExportState state) noexcept {
    return state == exporting::ExportState::Preparing ||
        state == exporting::ExportState::Running ||
        state == exporting::ExportState::Retrying ||
        state == exporting::ExportState::Cancelling;
}

[[nodiscard]] std::string UpdateFailureStatus(
    const updating::UpdateFailure& failure) {
    switch (failure.code) {
        case updating::UpdateErrorCode::Network:
            return "无法连接 GitHub，请检查网络后重试";
        case updating::UpdateErrorCode::HttpStatus:
            return failure.httpStatus == 0U
                ? "GitHub 暂时无法提供更新文件"
                : "GitHub 返回 HTTP " +
                    std::to_string(failure.httpStatus);
        case updating::UpdateErrorCode::InvalidVersion:
            return "GitHub Release 的版本信息无效";
        case updating::UpdateErrorCode::InvalidChecksum:
            return "新版 SHA-256 校验失败，已拒绝安装";
        case updating::UpdateErrorCode::InvalidExecutable:
            return "下载文件不是有效的 FramePlayer x64 程序";
        case updating::UpdateErrorCode::ResponseTooLarge:
            return "GitHub 返回的更新文件大小异常";
        case updating::UpdateErrorCode::FileSystem:
            return "无法写入或启动系统临时更新文件";
        case updating::UpdateErrorCode::OutOfMemory:
            return "系统内存不足，无法完成在线更新";
        case updating::UpdateErrorCode::Cancelled:
            return "在线更新已取消";
        case updating::UpdateErrorCode::InvalidConfiguration:
        case updating::UpdateErrorCode::InvalidUrl:
            return "在线更新地址配置无效";
        case updating::UpdateErrorCode::NoUpdateAvailable:
            return "当前没有可下载的新版本";
        case updating::UpdateErrorCode::WorkerStartFailed:
        case updating::UpdateErrorCode::Unexpected:
        case updating::UpdateErrorCode::None:
        default:
            if (!failure.message.empty()) {
                const std::string detail = WideToUtf8(failure.message);
                if (!detail.empty()) {
                    return detail;
                }
            }
            return "在线更新失败，请稍后重试";
    }
}

[[nodiscard]] OnlineUpdateView MakeOnlineUpdateView(
    const updating::UpdateSnapshot& snapshot) {
    OnlineUpdateView view;
    view.currentVersion = snapshot.currentVersion.ToString();
    if (snapshot.latestVersion.has_value()) {
        view.latestVersion = snapshot.latestVersion->ToString();
    }
    view.downloadedBytes = snapshot.downloadedBytes;
    view.totalBytes = snapshot.totalBytes.value_or(0U);

    switch (snapshot.phase) {
        case updating::UpdatePhase::Idle:
            view.phase = OnlineUpdatePhase::Idle;
            view.statusUtf8 = "可连接 GitHub 检查新版本";
            break;
        case updating::UpdatePhase::Checking:
            view.phase = OnlineUpdatePhase::Checking;
            view.statusUtf8 = "正在检查 GitHub Release";
            break;
        case updating::UpdatePhase::UpdateAvailable:
            view.phase = OnlineUpdatePhase::Downloading;
            view.statusUtf8 = "发现新版本，正在准备下载";
            break;
        case updating::UpdatePhase::UpToDate:
            view.phase = OnlineUpdatePhase::UpToDate;
            view.statusUtf8 = "当前已是最新版本";
            break;
        case updating::UpdatePhase::Downloading:
            view.phase = OnlineUpdatePhase::Downloading;
            view.statusUtf8 = "正在下载并校验更新";
            break;
        case updating::UpdatePhase::ReadyToInstall:
            view.phase = OnlineUpdatePhase::ReadyToInstall;
            view.statusUtf8 = "更新已下载并通过 SHA-256 校验";
            break;
        case updating::UpdatePhase::Cancelled:
            view.phase = OnlineUpdatePhase::Failed;
            view.statusUtf8 = "在线更新已取消";
            break;
        case updating::UpdatePhase::Failed:
            view.phase = OnlineUpdatePhase::Failed;
            view.statusUtf8 = UpdateFailureStatus(snapshot.failure);
            break;
    }
    return view;
}

[[nodiscard]] std::string RenderErrorDetail(
    const char* operation,
    const RenderResult result) {
    std::ostringstream stream;
    stream << operation << "：" << RenderStatusText(result.status)
           << "（HRESULT 0x"
           << std::uppercase << std::hex
           << static_cast<std::uint32_t>(result.nativeCode)
           << "）。";
    return stream.str();
}

void ShowRuntimeRenderFailure(const std::string& detail) {
    const std::wstring wideDetail = Utf8ToWide(detail);
    ::MessageBoxW(
        nullptr,
        wideDetail.empty() ? L"Direct3D 运行时发生严重错误。" : wideDetail.c_str(),
        L"序列播放器",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

[[nodiscard]] std::wstring BootstrapFailureText(
    const updating::BootstrapResult& result) {
    std::wstring message = result.message.empty()
        ? L"序列播放器更新失败。"
        : result.message;
    if (result.nativeError != ERROR_SUCCESS) {
        message.append(L"\n\n系统错误：");
        message.append(std::to_wstring(result.nativeError));
    }
    if (result.rollbackError != ERROR_SUCCESS) {
        message.append(L"\n回滚错误：");
        message.append(std::to_wstring(result.rollbackError));
    }
    return message;
}

}  // namespace

int Application::Run(
    const HINSTANCE instance,
    const UpdateCommandLineOptions& commandLine) {
    ImGui_ImplWin32_EnableDpiAwareness();

    Win32Window window;
    if (!window.Create(
            instance,
            L"序列播放器",
            kDefaultWindowWidth,
            kDefaultWindowHeight)) {
        ShowStartupFailure(L"无法创建应用窗口。");
        return 1;
    }

    D3D11Renderer renderer;
    if (!renderer.Initialize(window.Handle())) {
        ShowStartupFailure(L"无法初始化 Direct3D 11。请检查显卡驱动。");
        return 2;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    const bool platformBackendInitialized = ImGui_ImplWin32_Init(window.Handle());
    const bool rendererBackendInitialized =
        ImGui_ImplDX11_Init(renderer.Device(), renderer.Context());
    if (!platformBackendInitialized || !rendererBackendInitialized) {
        if (rendererBackendInitialized) {
            ImGui_ImplDX11_Shutdown();
        }
        if (platformBackendInitialized) {
            ImGui_ImplWin32_Shutdown();
        }
        ImGui::DestroyContext();
        ShowStartupFailure(L"无法初始化界面渲染后端。");
        return 3;
    }

    ConfigureFonts();

    auto player = std::make_unique<ComparisonPlayer>();
    auto exporter = std::make_unique<exporting::FfmpegExportController>();
    auto updateController = std::make_unique<updating::UpdateController>();
    static_cast<void>(updateController->Initialize());
    auto primaryFrameTexture = std::make_unique<FrameTexture>();
    auto secondaryFrameTexture = std::make_unique<FrameTexture>();
    auto maskOverlayTexture =
        std::make_unique<overlay::MaskOverlayTexture>();
    auto playerUi = std::make_unique<PlayerUI>();
    if (!primaryFrameTexture->Initialize(renderer.Device(), renderer.Context()) ||
        !secondaryFrameTexture->Initialize(renderer.Device(), renderer.Context()) ||
        !maskOverlayTexture->Initialize(renderer.Device(), renderer.Context())) {
        playerUi.reset();
        maskOverlayTexture.reset();
        secondaryFrameTexture.reset();
        primaryFrameTexture.reset();
        updateController->Shutdown();
        updateController.reset();
        exporter->Shutdown();
        exporter.reset();
        player->Shutdown();
        player.reset();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ShowStartupFailure(L"无法初始化帧纹理。");
        return 4;
    }

    playerUi->ApplyCodexStyle();
    float appliedDpiScale = 1.0F;
    ApplyDpiScale(window.CurrentDpi(), appliedDpiScale);
    playerUi->SetUiScale(appliedDpiScale);

    std::optional<PendingSequencePreference> pendingSequencePreference;
    std::optional<std::filesystem::path> rememberedSequenceFolder =
        user_settings::LoadLastSequenceFolder();
    const UiActions uiActions{
        [&window]() { return ShowFolderPicker(window.Handle()); },
        [&window]() { return ShowExportFolderPicker(window.Handle()); },
        [&window]() { return ShowPngImagePicker(window.Handle()); },
        [](const std::filesystem::path& filePath) {
            return platform::OpenFileWithDefaultApplication(filePath);
        },
        [&window]() { window.RequestClose(); },
        [&player,
         &pendingSequencePreference,
         &rememberedSequenceFolder]() {
            pendingSequencePreference.reset();
            if (rememberedSequenceFolder.has_value()) {
                static_cast<void>(player->LoadFolder(
                    *rememberedSequenceFolder));
            }
        },
        [&rememberedSequenceFolder]() {
            return rememberedSequenceFolder.has_value();
        },
        [&updateController]() {
            return MakeOnlineUpdateView(updateController->Snapshot());
        },
        [&updateController]() {
            static_cast<void>(updateController->CheckForUpdates());
        },
        [&updateController, &exporter, &window]() {
            if (IsExportBusyForUpdate(exporter->Snapshot().state)) {
                return;
            }
            const updating::BootstrapResult installResult =
                updateController->BeginInstall();
            if (installResult.success) {
                window.RequestClose();
            }
        },
    };

    const auto reportRendererFailure =
        [&playerUi](const char* operation, const RenderResult result) {
            if (result.Succeeded() || result.status == RenderStatus::Occluded) {
                return false;
            }
            const std::string detail = RenderErrorDetail(operation, result);
            playerUi->ReportRendererError(detail);
            if (IsFatalRendererStatus(result.status)) {
                ShowRuntimeRenderFailure(detail);
                return true;
            }
            return false;
        };

    window.ShowMaximized();
    bool startupHealthy = true;
    bool pendingUpdateHealth = commandLine.HasCleanupRequest();
    std::jthread updateCleanupThread;
    auto previousTick = std::chrono::steady_clock::now();
    WindowEvents events;
    app_detail::ApplicationActivityPolicy activityPolicy;
    std::optional<WindowDropEvent> pendingDrop;

    while (startupHealthy && window.PollEvents(events)) {
        if (events.resized.has_value()) {
            const RenderResult resizeResult = renderer.Resize(
                events.resized->width,
                events.resized->height);
            if (reportRendererFailure("窗口尺寸调整", resizeResult)) {
                break;
            }
        }
        if (events.dpiChanged.has_value()) {
            ApplyDpiScale(*events.dpiChanged, appliedDpiScale);
            playerUi->SetUiScale(appliedDpiScale);
        }
        if (events.droppedSourceEvent.has_value()) {
            pendingDrop = std::move(events.droppedSourceEvent);
        }

        const auto currentTick = std::chrono::steady_clock::now();
        const double elapsedSeconds =
            std::chrono::duration<double>(currentTick - previousTick).count();
        previousTick = currentTick;
        const app_detail::BackgroundModeTransition activityTransition =
            activityPolicy.Update(
                events.minimized,
                events.applicationActive,
                elapsedSeconds);
        if (activityTransition ==
            app_detail::BackgroundModeTransition::Entered) {
            player->SetBackgroundResourceMode(true);
        } else if (activityTransition ==
            app_detail::BackgroundModeTransition::Exited) {
            player->SetBackgroundResourceMode(false);
        }
        player->Tick(elapsedSeconds);
        updateController->Tick();

        if (pendingSequencePreference.has_value()) {
            const PlayerSnapshot snapshot = player->Snapshot().primary;
            if (!snapshot.loading) {
                const std::filesystem::path activePath =
                    snapshot.sourcePathUtf8.empty()
                    ? std::filesystem::path{}
                    : std::filesystem::path(
                        Utf8ToWide(snapshot.sourcePathUtf8));
                if (snapshot.sourceKind == SourceKind::PngSequence &&
                    snapshot.generation !=
                        pendingSequencePreference->previousGeneration &&
                    RefersToSamePath(
                        pendingSequencePreference->folder,
                        activePath)) {
                    rememberedSequenceFolder =
                        pendingSequencePreference->folder;
                    static_cast<void>(user_settings::SaveLastSequenceFolder(
                        pendingSequencePreference->folder));
                }
                pendingSequencePreference.reset();
            }
        }

        if (events.minimized) {
            std::this_thread::sleep_for(kOccludedSleepDuration);
            continue;
        }
        if (renderer.IsOccluded() && !renderer.TestOcclusion()) {
            std::this_thread::sleep_for(kOccludedSleepDuration);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        playerUi->Render(
            *player,
            *primaryFrameTexture,
            *secondaryFrameTexture,
            *maskOverlayTexture,
            *exporter,
            uiActions);
        ImGui::Render();

        const RenderResult beginFrameResult = renderer.BeginFrame();
        if (reportRendererFailure("开始渲染", beginFrameResult)) {
            break;
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const RenderResult presentResult = renderer.EndFrame(true);
        if (reportRendererFailure("提交画面", presentResult)) {
            break;
        }

        if (pendingUpdateHealth) {
            const updating::BootstrapResult healthResult =
                updating::SignalUpdateReady(
                    commandLine.updateHealthEventName);
            if (!healthResult.success) {
                const std::wstring detail =
                    BootstrapFailureText(healthResult);
                ::MessageBoxW(
                    window.Handle(),
                    detail.c_str(),
                    L"序列播放器更新失败",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                startupHealthy = false;
                break;
            }
            pendingUpdateHealth = false;
            try {
                const std::filesystem::path cleanupExecutable =
                    commandLine.cleanupUpdateExecutable;
                const std::uint32_t cleanupProcessId =
                    commandLine.updateBootstrapProcessId;
                updateCleanupThread = std::jthread(
                    [cleanupExecutable, cleanupProcessId]() noexcept {
                        updating::CleanupUpdateArtifacts(
                            cleanupExecutable,
                            cleanupProcessId);
                    });
            } catch (...) {
                // A failed best-effort cleanup thread must not invalidate a
                // healthy installed player. The next update can reuse/remove
                // the same versioned temporary file.
            }
        }

        if (pendingDrop.has_value()) {
            WindowDropEvent dropped = std::move(*pendingDrop);
            pendingDrop.reset();
            const bool secondaryTarget =
                playerUi->IsSecondaryViewportAtClientPoint(
                    dropped.clientPoint.x,
                    dropped.clientPoint.y);
            if (secondaryTarget) {
                static_cast<void>(
                    player->LoadSecondarySource(dropped.source.path));
            } else if (dropped.source.kind ==
                DroppedSourceKind::PngSequence) {
                const Generation previousGeneration =
                    player->Snapshot().primary.generation;
                if (player->LoadFolder(dropped.source.path)) {
                    pendingSequencePreference = PendingSequencePreference{
                        dropped.source.path.lexically_normal(),
                        previousGeneration};
                } else {
                    pendingSequencePreference.reset();
                }
            } else {
                pendingSequencePreference.reset();
                static_cast<void>(
                    player->LoadSource(dropped.source.path));
            }
        }
    }

    updateController->Shutdown();
    updateController.reset();
    exporter->Shutdown();
    playerUi.reset();
    exporter.reset();
    maskOverlayTexture->Reset();
    maskOverlayTexture.reset();
    secondaryFrameTexture->Reset();
    secondaryFrameTexture.reset();
    primaryFrameTexture->Reset();
    primaryFrameTexture.reset();
    player->Shutdown();
    player.reset();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    renderer.Shutdown();
    window.Destroy();
    return 0;
}

}  // namespace zt::sequence
