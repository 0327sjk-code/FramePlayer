#include "Update/Sha256.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using zt::sequence::updating::ComputeFileSha256;
using zt::sequence::updating::Sha256Digest;
using zt::sequence::updating::Sha256Equals;
using zt::sequence::updating::Sha256ToHex;

constexpr DWORD kProcessTimeoutMs = 20'000U;

class ScopedHandle final {
public:
    explicit ScopedHandle(const HANDLE handle = nullptr) noexcept
        : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_{};
};

[[nodiscard]] bool Expect(
    const bool condition,
    const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] std::optional<std::filesystem::path>
CurrentExecutablePath() {
    std::vector<wchar_t> buffer(512U, L'\0');
    while (buffer.size() <= 32'768U) {
        const DWORD length = ::GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(
                buffer.data(),
                static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2U, L'\0');
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> TemporaryPath() {
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;) {
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0U) {
            return std::nullopt;
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(
                buffer.data(),
                static_cast<std::size_t>(length)));
        }
        if (length >= 32'767U) {
            return std::nullopt;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1U, L'\0');
    }
}

[[nodiscard]] bool WriteMarkerNextToSelf() {
    const std::optional<std::filesystem::path> executable =
        CurrentExecutablePath();
    if (!executable.has_value()) {
        return false;
    }
    const std::filesystem::path marker =
        executable->parent_path() / L"previous-launched.marker";
    const ScopedHandle file(::CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr char payload[] = "restored";
    DWORD written = 0U;
    return ::WriteFile(
               file.Get(),
               payload,
               static_cast<DWORD>(sizeof(payload) - 1U),
               &written,
               nullptr) != FALSE &&
        written == sizeof(payload) - 1U;
}

[[nodiscard]] bool CreateEmptyFile(const std::filesystem::path& path) {
    const ScopedHandle file(::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    return file.Get() != INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool WaitForFile(
    const std::filesystem::path& path,
    const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

struct ProcessResult final {
    bool started{};
    bool completed{};
    DWORD exitCode{STILL_ACTIVE};
};

[[nodiscard]] ProcessResult RunBootstrap(
    const std::filesystem::path& bootstrap,
    const std::filesystem::path& target,
    const Sha256Digest& checksum) {
    std::wstring commandLine;
    const std::string checksumText = Sha256ToHex(checksum);
    commandLine.reserve(
        bootstrap.native().size() + target.native().size() +
        checksumText.size() + 128U);
    commandLine.push_back(L'"');
    commandLine.append(bootstrap.native());
    commandLine.append(L"\" --apply-update --target-exe=\"");
    commandLine.append(target.native());
    commandLine.append(
        L"\" --parent-pid=4294967294 --expected-sha256=");
    for (const char character : checksumText) {
        commandLine.push_back(static_cast<wchar_t>(character));
    }
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(
            bootstrap.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            bootstrap.parent_path().c_str(),
            &startup,
            &process) == FALSE) {
        return {};
    }
    static_cast<void>(::CloseHandle(process.hThread));
    const ScopedHandle processHandle(process.hProcess);
    ProcessResult result;
    result.started = true;
    result.completed = ::WaitForSingleObject(
        processHandle.Get(), kProcessTimeoutMs) == WAIT_OBJECT_0;
    if (result.completed) {
        static_cast<void>(::GetExitCodeProcess(
            processHandle.Get(), &result.exitCode));
    }
    return result;
}

[[nodiscard]] bool CopyFileChecked(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    return ::CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
}

[[nodiscard]] std::optional<Sha256Digest> FileHash(
    const std::filesystem::path& path) {
    DWORD error = ERROR_SUCCESS;
    return ComputeFileSha256(path, &error);
}

[[nodiscard]] bool TestScenario(
    const std::filesystem::path& testExecutable,
    const std::filesystem::path& harnessExecutable,
    const std::filesystem::path& updateRoot,
    const std::filesystem::path& testRoot,
    const std::wstring_view scenarioName,
    const bool forceHealthFailure) {
    const std::filesystem::path bootstrapDirectory =
        updateRoot / std::filesystem::path(scenarioName);
    const std::filesystem::path targetDirectory =
        testRoot / std::filesystem::path(scenarioName);
    std::error_code directoryError;
    std::filesystem::create_directories(
        bootstrapDirectory, directoryError);
    if (directoryError) {
        return Expect(false, "create bootstrap test directory");
    }
    std::filesystem::create_directories(targetDirectory, directoryError);
    if (directoryError) {
        return Expect(false, "create target test directory");
    }

    const std::filesystem::path bootstrap =
        bootstrapDirectory / L"FramePlayer.exe";
    const std::filesystem::path target =
        targetDirectory / L"FramePlayer.exe";
    bool passed = true;
    passed &= Expect(
        CopyFileChecked(harnessExecutable, bootstrap),
        "copy bootstrap harness");
    passed &= Expect(
        CopyFileChecked(testExecutable, target),
        "copy previous executable");
    if (!passed) {
        return false;
    }

    const std::optional<Sha256Digest> bootstrapHash = FileHash(bootstrap);
    const std::optional<Sha256Digest> previousHash = FileHash(target);
    passed &= Expect(
        bootstrapHash.has_value() && previousHash.has_value(),
        "hash test executables");
    if (!passed) {
        return false;
    }

    if (forceHealthFailure) {
        passed &= Expect(
            CreateEmptyFile(
                bootstrapDirectory / L"force-health-failure.flag"),
            "create synthetic health failure flag");
    }

    const ProcessResult process = RunBootstrap(
        bootstrap, target, *bootstrapHash);
    passed &= Expect(process.started, "start self-update bootstrap");
    passed &= Expect(process.completed, "self-update bootstrap completes");
    if (forceHealthFailure) {
        passed &= Expect(
            process.exitCode != 0U,
            "health failure returns a failure exit code");
        passed &= Expect(
            WaitForFile(
                targetDirectory / L"previous-launched.marker",
                std::chrono::seconds(5)),
            "rollback restarts the previous executable");
        const std::optional<Sha256Digest> restoredHash = FileHash(target);
        passed &= Expect(
            restoredHash.has_value() &&
                Sha256Equals(*restoredHash, *previousHash),
            "rollback restores the previous executable bytes");
    } else {
        passed &= Expect(
            process.exitCode == 0U,
            "healthy update returns success");
        passed &= Expect(
            WaitForFile(
                targetDirectory / L"new-launched.marker",
                std::chrono::seconds(5)),
            "healthy installed executable reports startup");
        const std::optional<Sha256Digest> installedHash = FileHash(target);
        passed &= Expect(
            installedHash.has_value() &&
                Sha256Equals(*installedHash, *bootstrapHash),
            "healthy update installs the verified executable bytes");
    }

    passed &= Expect(
        !std::filesystem::exists(
            targetDirectory / L"FramePlayer.old.exe"),
        "self-update leaves no old executable after completion");
    passed &= Expect(
        !std::filesystem::exists(
            targetDirectory / L"FramePlayer.new.exe"),
        "self-update leaves no staging executable after completion");
    return passed;
}

}  // namespace

int main(const int argumentCount, const char* const arguments[]) {
    if (argumentCount == 1) {
        return WriteMarkerNextToSelf() ? 0 : 80;
    }
    if (argumentCount != 2) {
        std::cerr << "Expected the bootstrap harness path.\n";
        return 2;
    }

    const std::optional<std::filesystem::path> testExecutable =
        CurrentExecutablePath();
    const std::optional<std::filesystem::path> temporaryPath =
        TemporaryPath();
    if (!Expect(testExecutable.has_value(), "resolve test executable") ||
        !Expect(temporaryPath.has_value(), "resolve temporary directory")) {
        return 1;
    }

    const std::filesystem::path harnessExecutable =
        std::filesystem::absolute(
            std::filesystem::path(arguments[1])).lexically_normal();
    const std::wstring unique =
        std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetTickCount64());
    const std::filesystem::path updateRoot =
        *temporaryPath / L"FramePlayer-Update";
    const std::filesystem::path testRoot =
        *temporaryPath / (L"FramePlayer-SelfUpdate-Test-" + unique);

    bool passed = true;
    passed &= TestScenario(
        *testExecutable,
        harnessExecutable,
        updateRoot,
        testRoot,
        L"bootstrap-success-" + unique,
        false);
    passed &= TestScenario(
        *testExecutable,
        harnessExecutable,
        updateRoot,
        testRoot,
        L"bootstrap-failure-" + unique,
        true);

    std::error_code cleanupError;
    static_cast<void>(std::filesystem::remove_all(testRoot, cleanupError));
    std::filesystem::remove_all(
        updateRoot / (L"bootstrap-success-" + unique), cleanupError);
    std::filesystem::remove_all(
        updateRoot / (L"bootstrap-failure-" + unique), cleanupError);
    if (std::filesystem::exists(updateRoot, cleanupError) &&
        std::filesystem::is_empty(updateRoot, cleanupError)) {
        static_cast<void>(std::filesystem::remove(updateRoot, cleanupError));
    }
    passed &= Expect(!cleanupError, "clean self-update test artifacts");
    return passed ? 0 : 1;
}
