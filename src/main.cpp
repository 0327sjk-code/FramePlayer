#include "App/Application.h"
#include "App/UpdateCommandLine.h"
#include "Update/SelfUpdateBootstrap.h"

#include <objbase.h>
#include <windows.h>

#include <exception>
#include <string>

namespace {

[[nodiscard]] std::wstring BootstrapErrorMessage(
    const zt::sequence::updating::BootstrapResult& result) {
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

void ShowLaunchError(
    const wchar_t* const title,
    const std::wstring& message) noexcept {
    ::MessageBoxW(
        nullptr,
        message.c_str(),
        title,
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

}  // namespace

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    const int) {
    const zt::sequence::UpdateCommandLineOptions commandLine =
        zt::sequence::ParseUpdateCommandLine();
    if (!commandLine.valid) {
        ShowLaunchError(
            L"序列播放器无法启动",
            commandLine.errorMessage.empty()
                ? L"启动参数无效。"
                : commandLine.errorMessage);
        return 2;
    }

    if (commandLine.applyUpdate) {
        zt::sequence::updating::ApplyUpdateRequest request;
        request.targetExecutable = commandLine.targetExecutable;
        request.parentProcessId = commandLine.parentProcessId;
        request.expectedChecksum = *commandLine.expectedChecksum;
        const zt::sequence::updating::BootstrapResult result =
            zt::sequence::updating::ApplyUpdate(request);
        if (!result.success) {
            ShowLaunchError(
                L"序列播放器更新失败",
                BootstrapErrorMessage(result));
            return 3;
        }
        return 0;
    }

    const HRESULT comResult = ::CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comResult)) {
        ::MessageBoxW(
            nullptr,
            L"无法初始化 Windows COM 服务。",
            L"序列播放器",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 10;
    }

    int exitCode = 0;
    try {
        zt::sequence::Application application;
        exitCode = application.Run(instance, commandLine);
    } catch (const std::exception&) {
        ::MessageBoxW(
            nullptr,
            L"应用发生未处理错误，已安全退出。",
            L"序列播放器",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        exitCode = 11;
    } catch (...) {
        ::MessageBoxW(
            nullptr,
            L"应用发生未知错误，已安全退出。",
            L"序列播放器",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        exitCode = 12;
    }

    ::CoUninitialize();
    return exitCode;
}
