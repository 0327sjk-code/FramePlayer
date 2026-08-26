#include "Export/FfmpegProcess.h"

#include "Platform/Utf8.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <limits>
#include <mutex>
#include <utility>

namespace zt::sequence::exporting {
namespace {

constexpr std::size_t kInitialSearchPathCharacters = 512U;
constexpr std::size_t kReadBufferBytes = 4096U;
constexpr std::size_t kMaximumDiagnosticBytes = 16U * 1024U;
constexpr DWORD kCancellationExitCode = ERROR_CANCELLED;

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void Reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle_));
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

[[nodiscard]] std::string WindowsErrorMessage(
    const char* operation,
    const DWORD errorCode) {
    wchar_t* messageBuffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<wchar_t*>(&messageBuffer),
        0,
        nullptr);

    std::wstring message;
    if (length > 0U && messageBuffer != nullptr) {
        message.assign(messageBuffer, length);
        static_cast<void>(::LocalFree(messageBuffer));
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' ||
                message.back() == L' ')) {
            message.pop_back();
        }
    }

    std::string result(operation);
    result.append("失败（");
    result.append(std::to_string(errorCode));
    result.append("）");
    const std::string messageUtf8 = WideToUtf8(message);
    if (!messageUtf8.empty()) {
        result.append("：");
        result.append(messageUtf8);
    }
    return result;
}

[[nodiscard]] bool NeedsWindowsQuoting(
    const std::wstring_view argument) noexcept {
    if (argument.empty()) {
        return true;
    }
    return std::any_of(
        argument.begin(),
        argument.end(),
        [](const wchar_t character) {
            return character == L'"' ||
                std::iswspace(static_cast<wint_t>(character)) != 0;
        });
}

[[nodiscard]] std::wstring QuoteWindowsArgument(
    const std::wstring_view argument) {
    if (!NeedsWindowsQuoting(argument)) {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashCount = 0U;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashCount;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashCount * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslashCount = 0U;
            continue;
        }
        quoted.append(backslashCount, L'\\');
        backslashCount = 0U;
        quoted.push_back(character);
    }
    quoted.append(backslashCount * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::wstring BuildCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring commandLine = QuoteWindowsArgument(executable.wstring());
    for (const std::wstring& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine.append(QuoteWindowsArgument(argument));
    }
    return commandLine;
}

void AppendBoundedDiagnostic(
    std::string& destination,
    const std::string_view line) {
    if (line.empty()) {
        return;
    }
    if (!destination.empty()) {
        destination.push_back('\n');
    }
    destination.append(line);
    if (destination.size() > kMaximumDiagnosticBytes) {
        destination.erase(
            0U,
            destination.size() - kMaximumDiagnosticBytes);
    }
}

[[nodiscard]] bool IsProgressKey(const std::string_view key) noexcept {
    return key == "frame" || key == "out_time_us" || key == "out_time_ms" ||
        key == "out_time" || key == "speed" || key == "fps" ||
        key == "bitrate" || key == "total_size" || key == "dup_frames" ||
        key == "drop_frames" || key == "stream_0_0_q" ||
        key == "progress";
}

void ConsumeLine(
    std::string line,
    const FfmpegProgressCallback& callback,
    std::string& diagnostic) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const std::size_t equals = line.find('=');
    if (equals != std::string::npos) {
        const std::string_view key(line.data(), equals);
        const std::string_view value(
            line.data() + equals + 1U,
            line.size() - equals - 1U);
        if (IsProgressKey(key)) {
            if (callback) {
                callback(key, value);
            }
            return;
        }
    }
    AppendBoundedDiagnostic(diagnostic, line);
}

}  // namespace

class FfmpegProcess::Impl final {
public:
    std::mutex mutex;
    HANDLE activeProcess = nullptr;
};

std::optional<std::filesystem::path> FindFfmpegExecutable(
    std::string& errorUtf8) noexcept {
    errorUtf8.clear();
    try {
        std::vector<wchar_t> buffer(kInitialSearchPathCharacters, L'\0');
        for (;;) {
            if (buffer.size() >
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
                errorUtf8 = "ffmpeg.exe 搜索路径过长";
                return std::nullopt;
            }

            const DWORD length = ::SearchPathW(
                nullptr,
                L"ffmpeg.exe",
                nullptr,
                static_cast<DWORD>(buffer.size()),
                buffer.data(),
                nullptr);
            if (length == 0U) {
                errorUtf8 = "未找到 ffmpeg.exe，请将 FFmpeg 加入 PATH";
                return std::nullopt;
            }
            if (length < buffer.size()) {
                return std::filesystem::path(
                    std::wstring(buffer.data(), static_cast<std::size_t>(length)));
            }
            buffer.assign(static_cast<std::size_t>(length) + 1U, L'\0');
        }
    } catch (...) {
        errorUtf8 = "搜索 ffmpeg.exe 时发生异常";
        return std::nullopt;
    }
}

FfmpegProcess::FfmpegProcess()
    : impl_(std::make_unique<Impl>()) {}

FfmpegProcess::~FfmpegProcess() {
    RequestTerminate();
}

FfmpegProcessResult FfmpegProcess::Run(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::stop_token stopToken,
    const FfmpegProgressCallback& progressCallback) noexcept {
    FfmpegProcessResult result;
    try {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;

        HANDLE rawRead = nullptr;
        HANDLE rawWrite = nullptr;
        if (::CreatePipe(
                &rawRead,
                &rawWrite,
                &securityAttributes,
                0U) == FALSE) {
            result.diagnosticUtf8 = WindowsErrorMessage(
                "创建 FFmpeg 输出管道",
                ::GetLastError());
            return result;
        }
        UniqueHandle outputRead(rawRead);
        UniqueHandle outputWrite(rawWrite);
        if (::SetHandleInformation(
                outputRead.Get(),
                HANDLE_FLAG_INHERIT,
                0U) == FALSE) {
            result.diagnosticUtf8 = WindowsErrorMessage(
                "配置 FFmpeg 输出管道",
                ::GetLastError());
            return result;
        }

        UniqueHandle nullInput(::CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (nullInput.Get() == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            static_cast<void>(nullInput.Release());
            result.diagnosticUtf8 = WindowsErrorMessage(
                "打开 FFmpeg 空输入",
                error);
            return result;
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = nullInput.Get();
        startupInfo.hStdOutput = outputWrite.Get();
        startupInfo.hStdError = outputWrite.Get();

        PROCESS_INFORMATION processInformation{};
        std::wstring commandLine = BuildCommandLine(executable, arguments);
        std::vector<wchar_t> mutableCommandLine(
            commandLine.begin(),
            commandLine.end());
        mutableCommandLine.push_back(L'\0');

        const BOOL created = ::CreateProcessW(
            executable.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInformation);
        if (created == FALSE) {
            result.diagnosticUtf8 = WindowsErrorMessage(
                "启动 FFmpeg",
                ::GetLastError());
            return result;
        }

        result.started = true;
        UniqueHandle process(processInformation.hProcess);
        UniqueHandle processThread(processInformation.hThread);
        outputWrite.Reset();
        nullInput.Reset();

        {
            std::scoped_lock lock(impl_->mutex);
            impl_->activeProcess = process.Get();
        }
        if (stopToken.stop_requested()) {
            RequestTerminate();
        }

        std::array<char, kReadBufferBytes> readBuffer{};
        std::string pending;
        for (;;) {
            DWORD bytesRead = 0U;
            const BOOL read = ::ReadFile(
                outputRead.Get(),
                readBuffer.data(),
                static_cast<DWORD>(readBuffer.size()),
                &bytesRead,
                nullptr);
            if (read == FALSE) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_BROKEN_PIPE) {
                    AppendBoundedDiagnostic(
                        result.diagnosticUtf8,
                        WindowsErrorMessage("读取 FFmpeg 输出", error));
                }
                break;
            }
            if (bytesRead == 0U) {
                break;
            }

            pending.append(readBuffer.data(), bytesRead);
            std::size_t newline = pending.find('\n');
            while (newline != std::string::npos) {
                ConsumeLine(
                    pending.substr(0U, newline),
                    progressCallback,
                    result.diagnosticUtf8);
                pending.erase(0U, newline + 1U);
                newline = pending.find('\n');
            }
        }
        if (!pending.empty()) {
            ConsumeLine(
                std::move(pending),
                progressCallback,
                result.diagnosticUtf8);
        }

        static_cast<void>(::WaitForSingleObject(process.Get(), INFINITE));
        DWORD exitCode = std::numeric_limits<DWORD>::max();
        if (::GetExitCodeProcess(process.Get(), &exitCode) == FALSE) {
            AppendBoundedDiagnostic(
                result.diagnosticUtf8,
                WindowsErrorMessage("读取 FFmpeg 退出代码", ::GetLastError()));
        }
        result.exitCode = exitCode;
        result.cancelled = stopToken.stop_requested();

        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->activeProcess == process.Get()) {
                impl_->activeProcess = nullptr;
            }
        }
        return result;
    } catch (...) {
        result.cancelled = stopToken.stop_requested();
        result.diagnosticUtf8 = "运行 FFmpeg 时发生异常";
        std::scoped_lock lock(impl_->mutex);
        impl_->activeProcess = nullptr;
        return result;
    }
}

void FfmpegProcess::RequestTerminate() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->activeProcess != nullptr) {
        static_cast<void>(::TerminateProcess(
            impl_->activeProcess,
            kCancellationExitCode));
    }
}

}  // namespace zt::sequence::exporting
