#include "App/UpdateCommandLine.h"
#include "Update/SelfUpdateBootstrap.h"
#include "Update/detail/SelfUpdatePath.h"

#include <windows.h>

#include <filesystem>
#include <iostream>

namespace {

[[nodiscard]] bool WriteMarker(
    const std::filesystem::path& marker) noexcept {
    const HANDLE file = ::CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr char payload[] = "ready";
    DWORD written = 0U;
    const BOOL succeeded = ::WriteFile(
        file,
        payload,
        static_cast<DWORD>(sizeof(payload) - 1U),
        &written,
        nullptr);
    static_cast<void>(::FlushFileBuffers(file));
    static_cast<void>(::CloseHandle(file));
    return succeeded != FALSE && written == sizeof(payload) - 1U;
}

}  // namespace

int wmain() {
    const zt::sequence::UpdateCommandLineOptions commandLine =
        zt::sequence::ParseUpdateCommandLine();
    if (!commandLine.valid) {
        std::wcerr << L"Invalid test command line: "
                   << commandLine.errorMessage << L'\n';
        return 90;
    }

    if (commandLine.applyUpdate) {
        zt::sequence::updating::ApplyUpdateRequest request;
        request.targetExecutable = commandLine.targetExecutable;
        request.parentProcessId = commandLine.parentProcessId;
        request.expectedChecksum = *commandLine.expectedChecksum;
        const zt::sequence::updating::BootstrapResult result =
            zt::sequence::updating::ApplyUpdate(request);
        if (!result.success) {
            std::wcerr << L"Bootstrap failure stage="
                       << static_cast<unsigned int>(result.stage)
                       << L" native=" << result.nativeError
                       << L" rollback=" << result.rollbackError << L'\n';
            return 91;
        }
        return 0;
    }

    if (!commandLine.HasCleanupRequest()) {
        return 0;
    }

    const std::filesystem::path failureFlag =
        commandLine.cleanupUpdateExecutable.parent_path() /
        L"force-health-failure.flag";
    if (::GetFileAttributesW(failureFlag.c_str()) !=
        INVALID_FILE_ATTRIBUTES) {
        return 42;
    }

    const zt::sequence::updating::BootstrapResult health =
        zt::sequence::updating::SignalUpdateReady(
            commandLine.updateHealthEventName);
    if (!health.success) {
        return 43;
    }
    zt::sequence::updating::CleanupUpdateArtifacts(
        commandLine.cleanupUpdateExecutable,
        commandLine.updateBootstrapProcessId);

    DWORD pathError = ERROR_SUCCESS;
    const std::filesystem::path currentExecutable =
        zt::sequence::updating::detail::CurrentExecutablePath(&pathError);
    if (currentExecutable.empty() || pathError != ERROR_SUCCESS) {
        return 44;
    }
    return WriteMarker(
               currentExecutable.parent_path() / L"new-launched.marker")
        ? 0
        : 45;
}
