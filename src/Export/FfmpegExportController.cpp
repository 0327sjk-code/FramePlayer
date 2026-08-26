#include "Export/FfmpegExportController.h"

#include "Export/ExportNaming.h"
#include "Export/ExportPolicy.h"
#include "Export/FfconcatManifest.h"
#include "Export/FfmpegProcess.h"
#include "Export/Image2SequenceInput.h"
#include "Platform/Utf8.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace zt::sequence::exporting {
namespace {

std::atomic<std::uint64_t> gNextExportJobId{1U};

[[nodiscard]] bool IsActiveState(const ExportState state) noexcept {
    return state == ExportState::Preparing || state == ExportState::Running ||
        state == ExportState::Retrying || state == ExportState::Cancelling;
}

[[nodiscard]] std::uint64_t NextExportJobId() noexcept {
    std::uint64_t identifier = gNextExportJobId.fetch_add(
        1U,
        std::memory_order_relaxed);
    if (identifier == 0U) {
        identifier = gNextExportJobId.fetch_add(
            1U,
            std::memory_order_relaxed);
    }
    return identifier;
}

[[nodiscard]] std::optional<std::uint64_t> ParseUnsigned64(
    std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r')) {
        text.remove_suffix(1U);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string PathForStatus(
    const std::filesystem::path& path) {
    const std::string utf8 = WideToUtf8(path.wstring());
    return utf8.empty() ? std::string("<path>") : utf8;
}

[[nodiscard]] std::string FileOperationError(
    const char* operation,
    const std::filesystem::path& path,
    const std::error_code& error) {
    std::string message(operation);
    message.append("：");
    message.append(PathForStatus(path));
    message.append("（");
    message.append(std::to_string(error.value()));
    message.push_back(')');
    return message;
}

[[nodiscard]] bool RemoveFileIfPresent(
    const std::filesystem::path& path,
    std::string* errorUtf8 = nullptr) noexcept {
    if (path.empty()) {
        return true;
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (!error) {
        return removed || !std::filesystem::exists(path, error);
    }
    if (errorUtf8 != nullptr) {
        *errorUtf8 = FileOperationError("无法删除临时文件", path, error);
    }
    return false;
}

class TemporaryExportFiles final {
public:
    ~TemporaryExportFiles() {
        Cleanup();
    }

    TemporaryExportFiles(const TemporaryExportFiles&) = delete;
    TemporaryExportFiles& operator=(const TemporaryExportFiles&) = delete;

    TemporaryExportFiles() = default;

    std::filesystem::path manifest;
    std::filesystem::path part;

    void Cleanup() noexcept {
        static_cast<void>(RemoveFileIfPresent(manifest));
        static_cast<void>(RemoveFileIfPresent(part));
    }
};

[[nodiscard]] std::filesystem::path BuildTemporaryPath(
    const std::filesystem::path& finalPath,
    const std::wstring_view role,
    const std::uint64_t jobId,
    const std::wstring_view extension) {
    std::wstring fileName = finalPath.filename().wstring();
    fileName.push_back(L'.');
    fileName.append(role);
    fileName.push_back(L'.');
    fileName.append(std::to_wstring(::GetCurrentProcessId()));
    fileName.push_back(L'.');
    fileName.append(std::to_wstring(jobId));
    fileName.append(extension);
    return finalPath.parent_path() / std::filesystem::path(std::move(fileName));
}

struct CommitResult final {
    bool succeeded = false;
    std::filesystem::path finalPath;
    std::string errorUtf8;
};

[[nodiscard]] CommitResult CommitPartFile(
    const std::filesystem::path& outputFolder,
    const std::filesystem::path& partPath) noexcept {
    CommitResult result;
    try {
        constexpr std::uint32_t kMaximumCollisionRetries = 128U;
        for (std::uint32_t retry = 0U;
             retry < kMaximumCollisionRetries;
             ++retry) {
            const std::filesystem::path candidate =
                FindAvailableMp4ExportPath(outputFolder);
            std::error_code renameError;
            std::filesystem::rename(partPath, candidate, renameError);
            if (!renameError) {
                result.succeeded = true;
                result.finalPath = candidate;
                return result;
            }

            std::error_code existsError;
            const bool candidateNowExists =
                std::filesystem::exists(candidate, existsError);
            if (!existsError && candidateNowExists) {
                continue;
            }
            result.errorUtf8 = FileOperationError(
                "提交 MP4 失败",
                candidate,
                renameError);
            return result;
        }
        result.errorUtf8 = "提交 MP4 失败：文件名碰撞次数过多";
        return result;
    } catch (const std::exception& exception) {
        result.errorUtf8 = std::string("提交 MP4 失败：") + exception.what();
        return result;
    } catch (...) {
        result.errorUtf8 = "提交 MP4 时发生异常";
        return result;
    }
}

[[nodiscard]] std::optional<std::uint64_t> ReadFileSize(
    const std::filesystem::path& path,
    std::string& errorUtf8) noexcept {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        errorUtf8 = FileOperationError("无法读取 MP4 大小", path, error);
        return std::nullopt;
    }
    if (size == 0U) {
        errorUtf8 = "FFmpeg 生成了空 MP4 文件";
        return std::nullopt;
    }
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(size);
}

[[nodiscard]] std::string FfmpegFailureMessage(
    const FfmpegProcessResult& result) {
    std::string message;
    if (!result.started) {
        message = "FFmpeg 未能启动";
    } else {
        message = "FFmpeg 编码失败，退出代码 ";
        message.append(std::to_string(result.exitCode));
    }
    if (!result.diagnosticUtf8.empty()) {
        message.append("：");
        message.append(result.diagnosticUtf8);
    }
    return message;
}

}  // namespace

class FfmpegExportController::Impl final {
public:
    [[nodiscard]] bool Start(FfmpegExportRequest request) {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (shutdown_) {
            return false;
        }

        {
            std::scoped_lock stateLock(stateMutex_);
            if (IsActiveState(progress_.state)) {
                return false;
            }
        }

        if (worker_.joinable()) {
            worker_.join();
        }

        const std::uint64_t jobId = NextExportJobId();
        const auto frameCount = CountExportFrames(request.sequence);
        const auto initialBitRate = frameCount
            ? CalculateInitialVideoBitRate(
                  *frameCount,
                  request.sequence.framesPerSecond)
            : std::nullopt;
        if (!frameCount || !initialBitRate || request.outputFolder.empty()) {
            std::scoped_lock stateLock(stateMutex_);
            progress_ = {};
            progress_.jobId = jobId;
            progress_.state = ExportState::Failed;
            progress_.statusUtf8 = "无法开始导出";
            progress_.errorUtf8 = request.outputFolder.empty()
                ? "导出目录为空"
                : "序列范围或帧率无效";
            return false;
        }

        {
            std::scoped_lock stateLock(stateMutex_);
            progress_ = {};
            progress_.jobId = jobId;
            progress_.state = ExportState::Preparing;
            progress_.totalFrames = *frameCount;
            progress_.statusUtf8 = "正在准备 MP4 导出";
        }

        try {
            worker_ = std::jthread(
                [this, jobId, request = std::move(request)](
                    const std::stop_token stopToken) mutable {
                    RunJob(jobId, std::move(request), stopToken);
                });
        } catch (const std::exception& exception) {
            SetFailed(
                jobId,
                std::string("无法创建导出线程：") + exception.what());
            return false;
        } catch (...) {
            SetFailed(jobId, "无法创建导出线程");
            return false;
        }
        return true;
    }

    void RequestCancel() noexcept {
        try {
            std::scoped_lock lifecycleLock(lifecycleMutex_);
            CancelLocked();
        } catch (...) {
        }
    }

    [[nodiscard]] ExportProgressSnapshot Snapshot() const {
        std::scoped_lock stateLock(stateMutex_);
        return progress_;
    }

    void Shutdown() noexcept {
        try {
            std::scoped_lock lifecycleLock(lifecycleMutex_);
            shutdown_ = true;
            CancelLocked();
            if (worker_.joinable()) {
                worker_.join();
            }
        } catch (...) {
        }
    }

private:
    void CancelLocked() noexcept {
        bool shouldCancel = false;
        {
            std::scoped_lock stateLock(stateMutex_);
            shouldCancel = IsActiveState(progress_.state);
            if (shouldCancel && progress_.state != ExportState::Cancelling) {
                progress_.state = ExportState::Cancelling;
                progress_.statusUtf8 = "正在取消导出";
                progress_.errorUtf8.clear();
            }
        }
        if (shouldCancel) {
            if (worker_.joinable()) {
                worker_.request_stop();
            }
            process_.RequestTerminate();
        }
    }

    [[nodiscard]] bool IsCancelled(
        const std::uint64_t jobId,
        const std::stop_token stopToken) const noexcept {
        if (stopToken.stop_requested()) {
            return true;
        }
        try {
            std::scoped_lock stateLock(stateMutex_);
            return progress_.jobId != jobId ||
                progress_.state == ExportState::Cancelling ||
                progress_.state == ExportState::Cancelled;
        } catch (...) {
            return true;
        }
    }

    void SetFailed(
        const std::uint64_t jobId,
        std::string errorUtf8) noexcept {
        try {
            std::scoped_lock stateLock(stateMutex_);
            if (progress_.jobId != jobId ||
                progress_.state == ExportState::Completed) {
                return;
            }
            if (progress_.state == ExportState::Cancelling ||
                progress_.state == ExportState::Cancelled) {
                progress_.state = ExportState::Cancelled;
                progress_.statusUtf8 = "导出已取消";
                progress_.errorUtf8.clear();
                return;
            }
            progress_.state = ExportState::Failed;
            progress_.statusUtf8 = "MP4 导出失败";
            progress_.errorUtf8 = std::move(errorUtf8);
        } catch (...) {
        }
    }

    void SetCancelled(const std::uint64_t jobId) noexcept {
        try {
            std::scoped_lock stateLock(stateMutex_);
            if (progress_.jobId != jobId ||
                progress_.state == ExportState::Completed) {
                return;
            }
            progress_.state = ExportState::Cancelled;
            progress_.statusUtf8 = "导出已取消";
            progress_.errorUtf8.clear();
        } catch (...) {
        }
    }

    void UpdateAttempt(
        const std::uint64_t jobId,
        const std::uint32_t attempt) noexcept {
        try {
            std::scoped_lock stateLock(stateMutex_);
            if (progress_.jobId != jobId ||
                progress_.state == ExportState::Cancelling) {
                return;
            }
            progress_.attempt = attempt;
            progress_.completedFrames = 0U;
            progress_.outputBytes = 0U;
            progress_.state = attempt == 1U
                ? ExportState::Running
                : ExportState::Retrying;
            progress_.statusUtf8 = attempt == 1U
                ? "正在使用 NVIDIA 编码 MP4"
                : "文件超过 90 MB，正在降低码率重试";
            progress_.errorUtf8.clear();
        } catch (...) {
        }
    }

    void UpdateProgress(
        const std::uint64_t jobId,
        const std::size_t completedFrames,
        const std::optional<std::uint64_t> outputBytes) noexcept {
        try {
            std::scoped_lock stateLock(stateMutex_);
            if (progress_.jobId != jobId ||
                !IsActiveState(progress_.state) ||
                progress_.state == ExportState::Cancelling) {
                return;
            }
            progress_.completedFrames = std::min(
                completedFrames,
                progress_.totalFrames);
            if (outputBytes) {
                progress_.outputBytes = *outputBytes;
            }
        } catch (...) {
        }
    }

    void RunJob(
        const std::uint64_t jobId,
        FfmpegExportRequest request,
        const std::stop_token stopToken) noexcept {
        TemporaryExportFiles temporaryFiles;
        const auto finishCancelled = [&]() {
            temporaryFiles.Cleanup();
            SetCancelled(jobId);
        };
        const auto finishFailed = [&](std::string errorUtf8) {
            temporaryFiles.Cleanup();
            SetFailed(jobId, std::move(errorUtf8));
        };

        try {
            if (IsCancelled(jobId, stopToken)) {
                finishCancelled();
                return;
            }

            std::error_code directoryError;
            const bool outputIsDirectory = std::filesystem::is_directory(
                request.outputFolder,
                directoryError);
            if (directoryError || !outputIsDirectory) {
                finishFailed(directoryError
                    ? FileOperationError(
                          "无法访问导出目录",
                          request.outputFolder,
                          directoryError)
                    : "导出目录不存在");
                return;
            }

            std::string ffmpegError;
            const auto ffmpegPath = FindFfmpegExecutable(ffmpegError);
            if (!ffmpegPath) {
                finishFailed(std::move(ffmpegError));
                return;
            }

            const auto frameCount = CountExportFrames(request.sequence);
            const auto initialBitRate = frameCount
                ? CalculateInitialVideoBitRate(
                      *frameCount,
                      request.sequence.framesPerSecond)
                : std::nullopt;
            if (!frameCount || !initialBitRate) {
                finishFailed("导出范围或帧率无效");
                return;
            }
            const std::optional<Image2SequenceInput> image2Input =
                DetectImage2SequenceInput(request.sequence);

            const std::size_t firstFrameIndex =
                request.sequence.inclusiveRange.startFrame;
            std::string dimensionsError;
            const auto dimensions = ReadPngDimensions(
                request.sequence.orderedPngFrames[firstFrameIndex].path,
                dimensionsError);
            if (!dimensions) {
                finishFailed(std::move(dimensionsError));
                return;
            }
            const auto pixelCrop = ResolvePixelCrop(
                request.crop,
                dimensions->width,
                dimensions->height);
            if (!pixelCrop) {
                finishFailed("遮罩裁切范围无效");
                return;
            }

            std::filesystem::path proposedFinalPath;
            try {
                proposedFinalPath = FindAvailableMp4ExportPath(
                    request.outputFolder);
            } catch (const std::exception& exception) {
                finishFailed(
                    std::string("无法生成 MP4 文件名：") + exception.what());
                return;
            }
            temporaryFiles.part = BuildTemporaryPath(
                proposedFinalPath,
                L"part",
                jobId,
                L".mp4");
            FfmpegInputSpec frozenInput;
            if (image2Input) {
                frozenInput.kind = FfmpegInputKind::Image2Sequence;
                frozenInput.path = image2Input->patternPath;
                frozenInput.startNumber = image2Input->startNumber;
            } else {
                temporaryFiles.manifest = BuildTemporaryPath(
                    proposedFinalPath,
                    L"manifest",
                    jobId,
                    L".ffconcat");
                frozenInput.kind = FfmpegInputKind::FfconcatManifest;
                frozenInput.path = temporaryFiles.manifest;
            }

            {
                std::scoped_lock stateLock(stateMutex_);
                if (progress_.jobId == jobId) {
                    progress_.finalOutputPath = proposedFinalPath;
                }
            }

            std::string cleanupError;
            if (!RemoveFileIfPresent(temporaryFiles.manifest, &cleanupError) ||
                !RemoveFileIfPresent(temporaryFiles.part, &cleanupError)) {
                finishFailed(std::move(cleanupError));
                return;
            }
            if (frozenInput.kind == FfmpegInputKind::FfconcatManifest) {
                std::string manifestError;
                if (!WriteFfconcatManifest(
                        temporaryFiles.manifest,
                        request.sequence,
                        manifestError)) {
                    finishFailed(std::move(manifestError));
                    return;
                }
            }

            std::uint64_t videoBitRate = *initialBitRate;
            for (std::uint32_t attempt = 1U;
                 attempt <= kMaximumEncodingAttempts;
                 ++attempt) {
                if (IsCancelled(jobId, stopToken)) {
                    finishCancelled();
                    return;
                }
                if (!RemoveFileIfPresent(temporaryFiles.part, &cleanupError)) {
                    finishFailed(std::move(cleanupError));
                    return;
                }

                UpdateAttempt(jobId, attempt);
                std::size_t observedFrames = 0U;
                bool observedProgressEnd = false;
                const std::vector<std::wstring> arguments = BuildFfmpegArguments(
                    frozenInput,
                    temporaryFiles.part,
                    *frameCount,
                    request.sequence.framesPerSecond,
                    videoBitRate,
                    *pixelCrop,
                    dimensions->width,
                    dimensions->height);

                const FfmpegProcessResult processResult = process_.Run(
                    *ffmpegPath,
                    arguments,
                    stopToken,
                    [&](const std::string_view key,
                        const std::string_view value) {
                        if (key == "frame") {
                            if (const auto parsed = ParseUnsigned64(value)) {
                                const std::uint64_t maximumSize =
                                    static_cast<std::uint64_t>(
                                        std::numeric_limits<std::size_t>::max());
                                const std::size_t frameValue =
                                    static_cast<std::size_t>(std::min(
                                        *parsed,
                                        maximumSize));
                                observedFrames = std::max(
                                    observedFrames,
                                    frameValue);
                                UpdateProgress(
                                    jobId,
                                    observedFrames,
                                    std::nullopt);
                            }
                        } else if (key == "total_size") {
                            if (const auto parsed = ParseUnsigned64(value)) {
                                UpdateProgress(
                                    jobId,
                                    observedFrames,
                                    parsed);
                            }
                        } else if (key == "progress" && value == "end") {
                            observedProgressEnd = true;
                        }
                    });

                if (IsCancelled(jobId, stopToken) ||
                    processResult.cancelled) {
                    finishCancelled();
                    return;
                }
                if (!processResult.started || processResult.exitCode != 0U) {
                    finishFailed(FfmpegFailureMessage(processResult));
                    return;
                }
                if (!observedProgressEnd || observedFrames != *frameCount) {
                    finishFailed(
                        "FFmpeg 未报告精确的导出帧数（期望 " +
                        std::to_string(*frameCount) + "，实际 " +
                        std::to_string(observedFrames) + "）");
                    return;
                }

                std::string sizeError;
                const auto outputBytes = ReadFileSize(
                    temporaryFiles.part,
                    sizeError);
                if (!outputBytes) {
                    finishFailed(std::move(sizeError));
                    return;
                }
                UpdateProgress(jobId, *frameCount, outputBytes);

                if (*outputBytes > kMaximumOutputBytes) {
                    const auto nextBitRate = CalculateRetryVideoBitRate(
                        videoBitRate,
                        *outputBytes);
                    if (attempt >= kMaximumEncodingAttempts || !nextBitRate) {
                        finishFailed(
                            "MP4 在降低码率后仍超过 90 MB（" +
                            std::to_string(*outputBytes) + " 字节）");
                        return;
                    }
                    if (!RemoveFileIfPresent(
                            temporaryFiles.part,
                            &cleanupError)) {
                        finishFailed(std::move(cleanupError));
                        return;
                    }
                    videoBitRate = *nextBitRate;
                    continue;
                }

                if (!RemoveFileIfPresent(
                        temporaryFiles.manifest,
                        &cleanupError)) {
                    finishFailed(std::move(cleanupError));
                    return;
                }

                CommitResult commit;
                bool cancelledBeforeCommit = false;
                {
                    // Cancellation and the final rename share this lock. Once
                    // the rename succeeds and state becomes Completed, a late
                    // cancel is a no-op and cannot remove the finished MP4.
                    std::scoped_lock stateLock(stateMutex_);
                    cancelledBeforeCommit =
                        stopToken.stop_requested() ||
                        progress_.jobId != jobId ||
                        progress_.state == ExportState::Cancelling ||
                        progress_.state == ExportState::Cancelled;
                    if (!cancelledBeforeCommit) {
                        commit = CommitPartFile(
                            request.outputFolder,
                            temporaryFiles.part);
                        if (commit.succeeded) {
                            progress_.state = ExportState::Completed;
                            progress_.completedFrames = *frameCount;
                            progress_.outputBytes = *outputBytes;
                            progress_.finalOutputPath = commit.finalPath;
                            progress_.statusUtf8 = "MP4 导出完成";
                            progress_.errorUtf8.clear();
                        }
                    }
                }
                if (cancelledBeforeCommit) {
                    finishCancelled();
                    return;
                }
                if (!commit.succeeded) {
                    finishFailed(std::move(commit.errorUtf8));
                    return;
                }
                return;
            }

            finishFailed("MP4 导出重试次数已用尽");
        } catch (const std::exception& exception) {
            finishFailed(std::string("MP4 导出异常：") + exception.what());
        } catch (...) {
            finishFailed("MP4 导出发生未知异常");
        }
    }

    mutable std::mutex stateMutex_;
    std::mutex lifecycleMutex_;
    ExportProgressSnapshot progress_;
    std::jthread worker_;
    FfmpegProcess process_;
    bool shutdown_ = false;
};

FfmpegExportController::FfmpegExportController()
    : impl_(std::make_unique<Impl>()) {}

FfmpegExportController::~FfmpegExportController() {
    Shutdown();
}

bool FfmpegExportController::Start(FfmpegExportRequest request) {
    return impl_->Start(std::move(request));
}

void FfmpegExportController::RequestCancel() noexcept {
    impl_->RequestCancel();
}

ExportProgressSnapshot FfmpegExportController::Snapshot() const {
    return impl_->Snapshot();
}

void FfmpegExportController::Shutdown() noexcept {
    impl_->Shutdown();
}

}  // namespace zt::sequence::exporting
