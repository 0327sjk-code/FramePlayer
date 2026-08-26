#include "Update/SelfUpdateBootstrap.h"

#include "Update/detail/SelfUpdatePath.h"
#include "Update/detail/SelfUpdateProcess.h"

#include <windows.h>

#include <filesystem>
#include <new>
#include <string_view>
#include <utility>

namespace zt::sequence::updating {
namespace {

constexpr DWORD PreviousProcessExitTimeoutMs = 120'000;
constexpr DWORD BootstrapCleanupTimeoutMs = 30'000;
constexpr DWORD InstalledHealthTimeoutMs = 120'000;
constexpr DWORD InstalledTerminationTimeoutMs = 5'000;
constexpr wchar_t InstallMutexName[] = L"Local\\FramePlayer.UpdateInstall";

class ScopedInstallMutex final {
public:
    ScopedInstallMutex() noexcept = default;
    ~ScopedInstallMutex() {
        if (acquired_) {
            static_cast<void>(::ReleaseMutex(handle_));
        }
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }

    ScopedInstallMutex(const ScopedInstallMutex&) = delete;
    ScopedInstallMutex& operator=(const ScopedInstallMutex&) = delete;

    [[nodiscard]] bool TryAcquire(DWORD* const error) noexcept {
        if (error != nullptr) {
            *error = ERROR_SUCCESS;
        }
        handle_ = ::CreateMutexW(nullptr, FALSE, InstallMutexName);
        if (handle_ == nullptr) {
            if (error != nullptr) {
                *error = ::GetLastError();
            }
            return false;
        }

        const DWORD waitResult = ::WaitForSingleObject(handle_, 0U);
        if (waitResult == WAIT_OBJECT_0 ||
            waitResult == WAIT_ABANDONED) {
            acquired_ = true;
            return true;
        }
        if (error != nullptr) {
            *error = waitResult == WAIT_TIMEOUT
                ? ERROR_BUSY
                : ::GetLastError();
            if (*error == ERROR_SUCCESS) {
                *error = ERROR_GEN_FAILURE;
            }
        }
        return false;
    }

private:
    HANDLE handle_{};
    bool acquired_{};
};

class ScopedStagingFile final {
public:
    explicit ScopedStagingFile(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~ScopedStagingFile() {
        if (active_ && !path_.empty()) {
            static_cast<void>(::DeleteFileW(path_.c_str()));
        }
    }

    ScopedStagingFile(const ScopedStagingFile&) = delete;
    ScopedStagingFile& operator=(const ScopedStagingFile&) = delete;

    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

class ScopedExecutableReadLock final {
public:
    ScopedExecutableReadLock() noexcept = default;
    ~ScopedExecutableReadLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    ScopedExecutableReadLock(const ScopedExecutableReadLock&) = delete;
    ScopedExecutableReadLock& operator=(const ScopedExecutableReadLock&) = delete;

    [[nodiscard]] bool Open(
        const std::filesystem::path& executable,
        DWORD* const error) noexcept {
        handle_ = ::CreateFileW(
            executable.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (error != nullptr) {
                *error = ERROR_SUCCESS;
            }
            return true;
        }
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] BootstrapResult Failure(
    const BootstrapStage stage,
    const DWORD nativeError,
    const std::wstring_view message,
    const DWORD rollbackError = ERROR_SUCCESS) noexcept {
    BootstrapResult result;
    result.stage = stage;
    result.nativeError = nativeError;
    result.rollbackError = rollbackError;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

[[nodiscard]] BootstrapResult Success() noexcept {
    BootstrapResult result;
    result.success = true;
    return result;
}

[[nodiscard]] BootstrapResult FailureAndRestartCurrentExecutable(
    const BootstrapStage failureStage,
    const DWORD nativeError,
    const std::wstring_view message,
    const std::filesystem::path& targetExecutable) noexcept {
    DWORD restartError = ERROR_SUCCESS;
    try {
        if (!detail::LaunchExecutableNormally(
                targetExecutable,
                &restartError)) {
            if (restartError == ERROR_SUCCESS) {
                restartError = ERROR_GEN_FAILURE;
            }
        }
    } catch (const std::bad_alloc&) {
        restartError = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        restartError = ERROR_GEN_FAILURE;
    }
    return Failure(
        restartError == ERROR_SUCCESS
            ? failureStage
            : BootstrapStage::RestartPreviousExecutable,
        nativeError,
        message,
        restartError);
}

[[nodiscard]] bool FlushFileContents(
    const std::filesystem::path& file,
    DWORD* const error) noexcept {
    const HANDLE handle = ::CreateFileW(
        file.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    const BOOL flushed = ::FlushFileBuffers(handle);
    const DWORD flushError = flushed != FALSE
        ? ERROR_SUCCESS
        : ::GetLastError();
    static_cast<void>(::CloseHandle(handle));
    if (error != nullptr) {
        *error = flushError;
    }
    return flushed != FALSE;
}

[[nodiscard]] DWORD RestorePreviousExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    if (::MoveFileExW(
            backupExecutable.c_str(),
            targetExecutable.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        return ERROR_SUCCESS;
    }
    return ::GetLastError();
}

[[nodiscard]] std::filesystem::path StagingPathFor(
    const std::filesystem::path& targetExecutable) {
    return targetExecutable.parent_path() /
        (targetExecutable.stem().native() + L".new" +
         targetExecutable.extension().native());
}

[[nodiscard]] bool RequireUnusedBackupPath(
    const std::filesystem::path& backupExecutable,
    DWORD* const error) noexcept {
    const DWORD attributes = ::GetFileAttributesW(backupExecutable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = ::GetLastError();
        if (nativeError == ERROR_FILE_NOT_FOUND ||
            nativeError == ERROR_PATH_NOT_FOUND) {
            if (error != nullptr) {
                *error = ERROR_SUCCESS;
            }
            return true;
        }
        if (error != nullptr) {
            *error = nativeError;
        }
        return false;
    }
    if (error != nullptr) {
        *error = ERROR_ALREADY_EXISTS;
    }
    return false;
}

[[nodiscard]] bool PrepareStagingPath(
    const std::filesystem::path& stagingExecutable,
    DWORD* const error) noexcept {
    const DWORD attributes = ::GetFileAttributesW(stagingExecutable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = ::GetLastError();
        if (nativeError == ERROR_FILE_NOT_FOUND ||
            nativeError == ERROR_PATH_NOT_FOUND) {
            if (error != nullptr) {
                *error = ERROR_SUCCESS;
            }
            return true;
        }
        if (error != nullptr) {
            *error = nativeError;
        }
        return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        if (error != nullptr) {
            *error = ERROR_CANT_ACCESS_FILE;
        }
        return false;
    }
    if (::DeleteFileW(stagingExecutable.c_str()) == FALSE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    return true;
}

[[nodiscard]] bool ValidateUpdateEndpoints(
    const std::filesystem::path& downloadedExecutable,
    const bool requireDownloadedProductName,
    const std::filesystem::path& targetExecutable,
    DWORD* error) {
    return detail::ValidateExecutablePath(
               downloadedExecutable,
               requireDownloadedProductName,
               error) &&
        detail::ValidateExecutablePath(targetExecutable, true, error) &&
        !detail::PathsEqualInsensitive(
            downloadedExecutable,
            targetExecutable);
}

[[nodiscard]] DWORD RestoreAndRestartPreviousExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    const DWORD restoreError = RestorePreviousExecutable(
        targetExecutable,
        backupExecutable);
    if (restoreError != ERROR_SUCCESS) {
        return restoreError;
    }

    try {
        DWORD launchError = ERROR_SUCCESS;
        if (!detail::LaunchExecutableNormally(targetExecutable, &launchError)) {
            return launchError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : launchError;
        }
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}

[[nodiscard]] BootstrapResult InstallFailure(
    const BootstrapStage failureStage,
    const DWORD nativeError,
    const std::wstring_view message,
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    const DWORD rollbackError = RestoreAndRestartPreviousExecutable(
        targetExecutable,
        backupExecutable);
    return Failure(
        rollbackError == ERROR_SUCCESS
            ? failureStage
            : BootstrapStage::Rollback,
        nativeError,
        message,
        rollbackError);
}

[[nodiscard]] BootstrapResult HealthFailure(
    detail::InstalledProcess& installedProcess,
    DWORD nativeError,
    const std::wstring_view message,
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    if (nativeError == ERROR_SUCCESS) {
        nativeError = ERROR_PROCESS_ABORTED;
    }

    DWORD terminationError = ERROR_SUCCESS;
    if (!detail::TerminateInstalledProcess(
            installedProcess,
            InstalledTerminationTimeoutMs,
            &terminationError)) {
        return Failure(
            BootstrapStage::Rollback,
            nativeError,
            L"新版未通过启动检查，且无法确认其进程已经停止；旧版备份已保留，未执行危险回滚。",
            terminationError == ERROR_SUCCESS
                ? ERROR_PROCESS_ABORTED
                : terminationError);
    }
    const DWORD rollbackError = RestoreAndRestartPreviousExecutable(
        targetExecutable,
        backupExecutable);
    return Failure(
        rollbackError == ERROR_SUCCESS
            ? BootstrapStage::WaitForInstalledExecutableHealth
            : BootstrapStage::Rollback,
        nativeError,
        message,
        rollbackError);
}

}  // namespace

BootstrapResult ApplyUpdate(const ApplyUpdateRequest& request) noexcept {
    std::filesystem::path targetExecutable;
    std::filesystem::path backupExecutable;
    bool rollbackRequired = false;
    try {
        DWORD nativeError = ERROR_SUCCESS;
        const std::filesystem::path bootstrapExecutable =
            detail::CurrentExecutablePath(&nativeError);
        targetExecutable = request.targetExecutable.lexically_normal();

        if (!ValidateUpdateEndpoints(
                bootstrapExecutable,
                false,
                targetExecutable,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_PARAMETER;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"更新文件或安装路径无效。路径必须是绝对 EXE 路径，且 EXE 本身不能是重解析文件。");
        }

        const std::optional<Sha256Digest> bootstrapChecksum =
            ComputeFileSha256(bootstrapExecutable, &nativeError);
        if (!bootstrapChecksum.has_value() ||
            !Sha256Equals(*bootstrapChecksum, request.expectedChecksum)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_CRC;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"更新引导程序与已验证的 SHA-256 不一致，已停止安装。");
        }

        ScopedInstallMutex installMutex;
        if (!installMutex.TryAcquire(&nativeError)) {
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                nativeError == ERROR_BUSY
                    ? L"另一个 FramePlayer 更新正在进行，本次更新已取消。"
                    : L"无法建立 FramePlayer 更新互斥锁。");
        }

        if (!detail::WaitForProcessExit(
                request.parentProcessId,
                PreviousProcessExitTimeoutMs,
                &nativeError)) {
            return Failure(
                BootstrapStage::WaitForPreviousProcess,
                nativeError,
                L"等待旧版 FramePlayer 退出失败。");
        }

        backupExecutable = detail::BackupPathFor(targetExecutable);
        if (!RequireUnusedBackupPath(backupExecutable, &nativeError)) {
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::PreservePreviousExecutable,
                nativeError,
                nativeError == ERROR_ALREADY_EXISTS
                    ? L"检测到尚未清理的旧版备份，已停止更新以免覆盖恢复文件。"
                    : L"无法检查旧版备份路径。",
                targetExecutable);
        }

        const std::filesystem::path stagingExecutable =
            StagingPathFor(targetExecutable);
        if (!PrepareStagingPath(stagingExecutable, &nativeError)) {
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"无法准备新版程序的同目录临时文件。",
                targetExecutable);
        }
        ScopedStagingFile stagingCleanup(stagingExecutable);

        if (::CopyFileW(
                bootstrapExecutable.c_str(),
                stagingExecutable.c_str(),
                TRUE) == FALSE) {
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::InstallExecutable,
                ::GetLastError(),
                L"复制新版 FramePlayer.exe 到同目录临时文件失败。",
                targetExecutable);
        }

        if (!FlushFileContents(stagingExecutable, &nativeError)) {
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"新版同目录临时文件无法完整写入磁盘。",
                targetExecutable);
        }

        if (!detail::VerifyInstalledCopy(
                bootstrapExecutable,
                stagingExecutable,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_CRC;
            }
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"新版同目录临时文件校验失败。",
                targetExecutable);
        }

        if (!detail::NormalizeInstalledExecutableAttributes(
                stagingExecutable,
                &nativeError)) {
            return FailureAndRestartCurrentExecutable(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"无法清除新版临时文件属性。",
                targetExecutable);
        }

        if (::ReplaceFileW(
                targetExecutable.c_str(),
                stagingExecutable.c_str(),
                backupExecutable.c_str(),
                REPLACEFILE_WRITE_THROUGH,
                nullptr,
                nullptr) == FALSE) {
            const DWORD replaceError = ::GetLastError();
            DWORD rollbackError = ERROR_SUCCESS;
            const DWORD backupAttributes =
                ::GetFileAttributesW(backupExecutable.c_str());
            if (backupAttributes != INVALID_FILE_ATTRIBUTES) {
                if ((backupAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                         FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                    rollbackError = ERROR_CANT_ACCESS_FILE;
                } else {
                    rollbackError = RestorePreviousExecutable(
                        targetExecutable,
                        backupExecutable);
                }
            } else {
                const DWORD targetAttributes =
                    ::GetFileAttributesW(targetExecutable.c_str());
                if (targetAttributes == INVALID_FILE_ATTRIBUTES) {
                    rollbackError = ERROR_FILE_NOT_FOUND;
                }
            }
            if (rollbackError == ERROR_SUCCESS) {
                return FailureAndRestartCurrentExecutable(
                    BootstrapStage::PreservePreviousExecutable,
                    replaceError,
                    L"无法以原子方式替换 FramePlayer.exe；已恢复并重新启动旧版。",
                    targetExecutable);
            }
            return Failure(
                BootstrapStage::Rollback,
                replaceError,
                L"原子替换失败且无法完整恢复旧版入口；恢复文件已尽量保留。",
                rollbackError);
        }
        stagingCleanup.Release();
        rollbackRequired = true;

        if (!detail::VerifyInstalledCopy(
                bootstrapExecutable,
                targetExecutable,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_CRC;
            }
            return InstallFailure(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"原子替换后的新版文件校验失败，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        detail::InstalledProcess installedProcess;
        if (!detail::LaunchInstalledExecutable(
                targetExecutable,
                bootstrapExecutable,
                &installedProcess,
                &nativeError)) {
            return InstallFailure(
                BootstrapStage::LaunchInstalledExecutable,
                nativeError,
                L"启动新版 FramePlayer 失败，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        const detail::InstalledHealthOutcome healthOutcome =
            detail::WaitForInstalledHealth(
                installedProcess,
                InstalledHealthTimeoutMs,
                &nativeError);
        if (healthOutcome != detail::InstalledHealthOutcome::Ready) {
            if (healthOutcome == detail::InstalledHealthOutcome::ProcessExited) {
                nativeError = ERROR_PROCESS_ABORTED;
            }
            return HealthFailure(
                installedProcess,
                nativeError,
                healthOutcome == detail::InstalledHealthOutcome::Timeout
                    ? L"新版 FramePlayer 未在 120 秒内完成初始化，已恢复并重新启动旧版。"
                    : L"新版 FramePlayer 在完成初始化前退出，已恢复并重新启动旧版。",
                targetExecutable,
                backupExecutable);
        }

        detail::RemoveFileBestEffort(backupExecutable);
        rollbackRequired = false;
        return Success();
    } catch (const std::bad_alloc&) {
        const DWORD rollbackError = rollbackRequired
            ? RestoreAndRestartPreviousExecutable(
                  targetExecutable,
                  backupExecutable)
            : ERROR_SUCCESS;
        return Failure(
            rollbackError == ERROR_SUCCESS
                ? (rollbackRequired
                       ? BootstrapStage::InstallExecutable
                       : BootstrapStage::ValidateRequest)
                : BootstrapStage::Rollback,
            ERROR_NOT_ENOUGH_MEMORY,
            rollbackRequired
                ? L"执行更新时内存不足，已尝试恢复并重新启动旧版。"
                : L"执行更新时内存不足。",
            rollbackError);
    } catch (...) {
        const DWORD rollbackError = rollbackRequired
            ? RestoreAndRestartPreviousExecutable(
                  targetExecutable,
                  backupExecutable)
            : ERROR_SUCCESS;
        return Failure(
            rollbackError == ERROR_SUCCESS
                ? (rollbackRequired
                       ? BootstrapStage::InstallExecutable
                       : BootstrapStage::ValidateRequest)
                : BootstrapStage::Rollback,
            ERROR_GEN_FAILURE,
            rollbackRequired
                ? L"执行更新时发生未知错误，已尝试恢复并重新启动旧版。"
                : L"执行更新时发生未知错误。",
            rollbackError);
    }
}

BootstrapResult LaunchApplyUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    const std::uint32_t parentProcessId,
    const Sha256Digest& expectedChecksum) noexcept {
    try {
        DWORD nativeError = ERROR_SUCCESS;
        const std::filesystem::path normalizedDownload =
            downloadedExecutable.lexically_normal();
        const std::filesystem::path normalizedTarget =
            targetExecutable.lexically_normal();
        if (parentProcessId == 0 ||
            !ValidateUpdateEndpoints(
                normalizedDownload,
                true,
                normalizedTarget,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_PARAMETER;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"下载文件或安装路径无效，无法启动更新程序。");
        }

        ScopedExecutableReadLock downloadLock;
        if (!downloadLock.Open(normalizedDownload, &nativeError)) {
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"无法锁定已验证的更新文件，已拒绝启动。");
        }

        const std::optional<Sha256Digest> actualChecksum =
            ComputeFileSha256(normalizedDownload, &nativeError);
        if (!actualChecksum.has_value() ||
            !Sha256Equals(*actualChecksum, expectedChecksum)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_CRC;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"更新文件在安装前发生变化，已拒绝启动。");
        }

        if (!detail::LaunchBootstrapExecutable(
                normalizedDownload,
                normalizedTarget,
                parentProcessId,
                expectedChecksum,
                &nativeError)) {
            return Failure(
                BootstrapStage::LaunchBootstrapExecutable,
                nativeError,
                L"无法启动 FramePlayer 更新程序。");
        }
        return Success();
    } catch (const std::bad_alloc&) {
        return Failure(
            BootstrapStage::LaunchBootstrapExecutable,
            ERROR_NOT_ENOUGH_MEMORY,
            L"启动更新程序时内存不足。");
    } catch (...) {
        return Failure(
            BootstrapStage::LaunchBootstrapExecutable,
            ERROR_GEN_FAILURE,
            L"启动更新程序时发生未知错误。");
    }
}

BootstrapResult SignalUpdateReady(
    const std::wstring_view healthEventName) noexcept {
    try {
        DWORD nativeError = ERROR_SUCCESS;
        if (healthEventName.empty() ||
            !detail::SignalHealthEvent(healthEventName, &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_NAME;
            }
            return Failure(
                BootstrapStage::SignalInstalledExecutableReady,
                nativeError,
                L"无法向更新引导程序报告新版初始化完成。");
        }
        return Success();
    } catch (const std::bad_alloc&) {
        return Failure(
            BootstrapStage::SignalInstalledExecutableReady,
            ERROR_NOT_ENOUGH_MEMORY,
            L"报告新版初始化状态时内存不足。");
    } catch (...) {
        return Failure(
            BootstrapStage::SignalInstalledExecutableReady,
            ERROR_GEN_FAILURE,
            L"报告新版初始化状态时发生未知错误。");
    }
}

void CleanupUpdateArtifacts(
    const std::filesystem::path& bootstrapExecutable,
    const std::uint32_t bootstrapProcessId) noexcept {
    try {
        if (bootstrapProcessId == 0 ||
            bootstrapProcessId == ::GetCurrentProcessId()) {
            return;
        }

        DWORD ignoredError = ERROR_SUCCESS;
        const std::filesystem::path normalizedBootstrap =
            bootstrapExecutable.lexically_normal();
        if (!detail::IsOwnedTemporaryUpdateExecutable(
                normalizedBootstrap,
                &ignoredError)) {
            return;
        }

        if (!detail::WaitForProcessExit(
                bootstrapProcessId,
                BootstrapCleanupTimeoutMs,
                &ignoredError)) {
            return;
        }

        detail::RemoveFileBestEffort(normalizedBootstrap);
        const std::filesystem::path versionDirectory =
            normalizedBootstrap.parent_path();
        static_cast<void>(::RemoveDirectoryW(versionDirectory.c_str()));
        const std::filesystem::path updateRoot =
            versionDirectory.parent_path();
        static_cast<void>(::RemoveDirectoryW(updateRoot.c_str()));
    } catch (...) {
        // Cleanup must never prevent the installed application from starting.
    }
}

}  // namespace zt::sequence::updating
