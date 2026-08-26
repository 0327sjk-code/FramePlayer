#include "Update/UpdateController.h"

#include "App/ProductInfo.h"
#include "Update/UpdateCoordinator.h"
#include "Update/detail/SelfUpdatePath.h"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <utility>

namespace zt::sequence::updating {
namespace {

[[nodiscard]] BootstrapResult InvalidInstallRequest(
    const std::wstring_view message) noexcept {
    BootstrapResult result;
    result.stage = BootstrapStage::ValidateRequest;
    result.nativeError = ERROR_INVALID_STATE;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

}  // namespace

UpdateController::UpdateController() = default;

UpdateController::~UpdateController() {
    Shutdown();
}

bool UpdateController::Initialize() noexcept {
    if (coordinator_) {
        return true;
    }
    shuttingDown_ = false;
    installLaunched_ = false;
    downloadRequested_ = false;
    localSnapshot_.reset();
    try {
        const std::optional<SemanticVersion> currentVersion =
            SemanticVersion::Parse(product::Version);
        if (!currentVersion.has_value()) {
            PublishLocalFailure(
                UpdateErrorCode::InvalidConfiguration,
                L"程序版本号无效，在线更新不可用。");
            return false;
        }

        GitHubUpdateClientOptions options;
        options.versionUrl = product::UpdateVersionUrl;
        options.releaseDownloadBaseUrl =
            product::UpdateReleaseDownloadBaseUrl;
        options.userAgent =
            std::wstring(product::Name) + L"/" + product::Version;
        coordinator_ = std::make_unique<UpdateCoordinator>(
            *currentVersion,
            std::move(options));
        return true;
    } catch (...) {
        coordinator_.reset();
        PublishLocalFailure(
            UpdateErrorCode::OutOfMemory,
            L"在线更新模块初始化失败；播放器功能不受影响。");
        return false;
    }
}

void UpdateController::Tick() noexcept {
    if (!coordinator_ || shuttingDown_ || localSnapshot_.has_value()) {
        return;
    }
    try {
        const UpdateSnapshot snapshot = coordinator_->Snapshot();
        if (snapshot.phase == UpdatePhase::UpdateAvailable &&
            !downloadRequested_) {
            downloadRequested_ = true;
            if (!coordinator_->DownloadAvailableUpdate()) {
                downloadRequested_ = false;
                PublishLocalFailure(
                    UpdateErrorCode::WorkerStartFailed,
                    L"发现新版本，但无法启动后台下载任务。");
            }
        } else if (snapshot.phase == UpdatePhase::ReadyToInstall ||
                   snapshot.phase == UpdatePhase::Failed ||
                   snapshot.phase == UpdatePhase::Cancelled ||
                   snapshot.phase == UpdatePhase::UpToDate) {
            downloadRequested_ = false;
        }
    } catch (...) {
        PublishLocalFailure(
            UpdateErrorCode::Unexpected,
            L"处理在线更新状态时发生错误；播放器功能不受影响。");
    }
}

bool UpdateController::CheckForUpdates() noexcept {
    if (!coordinator_ || shuttingDown_ || installLaunched_) {
        return false;
    }
    try {
        if (coordinator_->IsBusy()) {
            return false;
        }
        localSnapshot_.reset();
        downloadRequested_ = false;
        if (!coordinator_->CheckForUpdates()) {
            PublishLocalFailure(
                UpdateErrorCode::WorkerStartFailed,
                L"无法启动更新检查，请稍后重试。");
            return false;
        }
        return true;
    } catch (...) {
        PublishLocalFailure(
            UpdateErrorCode::Unexpected,
            L"无法启动更新检查，请稍后重试。");
        return false;
    }
}

BootstrapResult UpdateController::BeginInstall() noexcept {
    if (!coordinator_ || shuttingDown_ || installLaunched_) {
        return InvalidInstallRequest(L"当前没有可安装的更新。");
    }
    try {
        const UpdateSnapshot snapshot = coordinator_->Snapshot();
        if (snapshot.phase != UpdatePhase::ReadyToInstall ||
            snapshot.downloadedFile.empty() ||
            !snapshot.verifiedChecksum.has_value()) {
            return InvalidInstallRequest(L"新版尚未下载完成。");
        }

        DWORD pathError = ERROR_SUCCESS;
        const std::filesystem::path currentExecutable =
            detail::CurrentExecutablePath(&pathError);
        if (currentExecutable.empty()) {
            PublishLocalFailure(
                UpdateErrorCode::FileSystem,
                L"无法确定当前程序路径，不能启动更新。",
                pathError);
            return InvalidInstallRequest(L"无法确定当前程序路径。");
        }

        BootstrapResult result = LaunchApplyUpdate(
            snapshot.downloadedFile,
            currentExecutable,
            ::GetCurrentProcessId(),
            *snapshot.verifiedChecksum);
        if (!result.success) {
            PublishLocalFailure(
                UpdateErrorCode::FileSystem,
                result.message.empty()
                    ? L"无法启动更新引导程序。"
                    : result.message,
                result.nativeError);
            return result;
        }
        installLaunched_ = true;
        return result;
    } catch (...) {
        PublishLocalFailure(
            UpdateErrorCode::Unexpected,
            L"提交更新安装任务时发生错误。");
        return InvalidInstallRequest(L"提交更新安装任务时发生错误。");
    }
}

UpdateSnapshot UpdateController::Snapshot() const noexcept {
    if (localSnapshot_.has_value()) {
        return *localSnapshot_;
    }
    if (coordinator_) {
        return coordinator_->Snapshot();
    }
    UpdateSnapshot snapshot;
    const std::optional<SemanticVersion> version =
        SemanticVersion::Parse(product::Version);
    if (version.has_value()) {
        snapshot.currentVersion = *version;
    }
    snapshot.phase = UpdatePhase::Failed;
    snapshot.failure.code = UpdateErrorCode::InvalidConfiguration;
    snapshot.failure.message = L"在线更新模块不可用。";
    return snapshot;
}

bool UpdateController::InstallWasLaunched() const noexcept {
    return installLaunched_;
}

void UpdateController::Shutdown() noexcept {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    UpdateSnapshot finalSnapshot;
    if (coordinator_) {
        coordinator_->SetStatusCallback({});
        coordinator_->CancelAndWait();
        finalSnapshot = coordinator_->Snapshot();
        coordinator_.reset();
    }
    if (!installLaunched_ && !finalSnapshot.downloadedFile.empty()) {
        RemoveDownloadedFileBestEffort(finalSnapshot.downloadedFile);
    }
    localSnapshot_.reset();
    downloadRequested_ = false;
}

void UpdateController::PublishLocalFailure(
    const UpdateErrorCode code,
    std::wstring message,
    const std::uint32_t nativeCode) noexcept {
    try {
        UpdateSnapshot snapshot;
        if (coordinator_) {
            snapshot = coordinator_->Snapshot();
        } else if (const std::optional<SemanticVersion> version =
                       SemanticVersion::Parse(product::Version);
                   version.has_value()) {
            snapshot.currentVersion = *version;
        }
        snapshot.phase = UpdatePhase::Failed;
        snapshot.failure.code = code;
        snapshot.failure.nativeCode = nativeCode;
        snapshot.failure.message = std::move(message);
        localSnapshot_ = std::move(snapshot);
    } catch (...) {
        localSnapshot_.reset();
    }
}

void UpdateController::RemoveDownloadedFileBestEffort(
    const std::filesystem::path& file) const noexcept {
    try {
        if (file.empty()) {
            return;
        }
        static_cast<void>(::DeleteFileW(file.c_str()));
        const std::filesystem::path versionDirectory = file.parent_path();
        static_cast<void>(::RemoveDirectoryW(versionDirectory.c_str()));
        const std::filesystem::path updateRoot = versionDirectory.parent_path();
        static_cast<void>(::RemoveDirectoryW(updateRoot.c_str()));
    } catch (...) {
    }
}

}  // namespace zt::sequence::updating
