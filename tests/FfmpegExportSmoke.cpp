#include "Core/SequenceScanner.h"
#include "Export/ExportTypes.h"
#include "Export/FfmpegExportController.h"
#include "Imaging/MediaFoundationVideoDecoder.h"
#include "Platform/Utf8.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using zt::sequence::FrameIndex;
using zt::sequence::SequenceExportSnapshot;
using zt::sequence::VideoExportSnapshot;
using zt::sequence::exporting::ExportProgressSnapshot;
using zt::sequence::exporting::ExportState;
using zt::sequence::exporting::NormalizedCrop;

[[nodiscard]] bool IsTerminal(const ExportState state) noexcept {
    return state == ExportState::Completed ||
        state == ExportState::Cancelled ||
        state == ExportState::Failed;
}

[[nodiscard]] const char* StateName(const ExportState state) noexcept {
    switch (state) {
        case ExportState::Idle:
            return "Idle";
        case ExportState::Preparing:
            return "Preparing";
        case ExportState::Running:
            return "Running";
        case ExportState::Retrying:
            return "Retrying";
        case ExportState::Cancelling:
            return "Cancelling";
        case ExportState::Completed:
            return "Completed";
        case ExportState::Cancelled:
            return "Cancelled";
        case ExportState::Failed:
            return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] std::optional<NormalizedCrop> CropForName(
    const std::wstring_view name) noexcept {
    if (name == L"none") {
        return NormalizedCrop{};
    }
    if (name == L"1080x1080") {
        return NormalizedCrop{0.21875, 0.21875, 0.78125, 0.78125};
    }
    if (name == L"1920x1080") {
        return NormalizedCrop{0.0, 0.21875, 1.0, 0.78125};
    }
    if (name == L"1080x1920") {
        return NormalizedCrop{0.21875, 0.0, 0.78125, 1.0};
    }
    if (name == L"864x1080") {
        return NormalizedCrop{0.275, 0.21875, 0.725, 0.78125};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> ParseIndex(
    const wchar_t* text) noexcept {
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<double> ParseFramesPerSecond(
    const wchar_t* text) noexcept {
    try {
        const double parsed = std::stod(text);
        return parsed >= 1.0 && parsed <= 120.0
            ? std::optional<double>(parsed)
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

void PrintProgress(const ExportProgressSnapshot& progress) {
    std::cout << "state=" << StateName(progress.state)
              << " frame=" << progress.completedFrames
              << '/' << progress.totalFrames
              << " attempt=" << progress.attempt
              << " bytes=" << progress.outputBytes
              << " status=" << progress.statusUtf8;
    if (!progress.errorUtf8.empty()) {
        std::cout << " error=" << progress.errorUtf8;
    }
    std::cout << '\n';
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount < 3 || argumentCount > 9) {
        std::cerr
            << "usage: ZTFfmpegExportSmoke <source-path> <output-folder> "
               "[none|1080x1080|1920x1080|1080x1920|864x1080] "
               "[start-index] [end-index] [fps] [cancel-after-ms|-] "
               "[overlay-png]\n";
        return 2;
    }

    const std::filesystem::path sourcePath(arguments[1]);
    const std::filesystem::path outputFolder(arguments[2]);
    const std::wstring_view cropName = argumentCount >= 4
        ? std::wstring_view(arguments[3])
        : std::wstring_view(L"none");
    const auto crop = CropForName(cropName);
    if (!crop) {
        std::cerr << "invalid crop preset\n";
        return 3;
    }

    std::optional<zt::sequence::SequenceScanResult> scan;
    std::optional<zt::sequence::VideoProbeResult> videoProbe;
    std::size_t sourceFrameCount = 0U;
    double defaultFramesPerSecond = 60.0;
    std::error_code sourceError;
    if (std::filesystem::is_directory(sourcePath, sourceError) &&
        !sourceError) {
        scan = zt::sequence::ScanPngFolder(sourcePath);
        if (!*scan) {
            std::cerr << scan->errorUtf8 << '\n';
            return 4;
        }
        sourceFrameCount = scan->frames.size();
    } else {
        sourceError.clear();
        if (!std::filesystem::is_regular_file(sourcePath, sourceError) ||
            sourceError) {
            std::cerr << "source path is not a PNG directory or video file\n";
            return 4;
        }
        videoProbe = zt::sequence::ProbeVideoFile(sourcePath);
        if (!*videoProbe) {
            std::cerr << videoProbe->errorUtf8 << '\n';
            return 4;
        }
        sourceFrameCount = videoProbe->metadata.frameCount;
        defaultFramesPerSecond = videoProbe->metadata.framesPerSecond;
    }
    if (sourceFrameCount == 0U || sourceFrameCount >
        static_cast<std::size_t>(std::numeric_limits<FrameIndex>::max())) {
        std::cerr << "too many frames\n";
        return 5;
    }

    const std::size_t defaultEnd = sourceFrameCount - 1U;
    const auto start = argumentCount >= 5
        ? ParseIndex(arguments[4])
        : std::optional<std::size_t>(0U);
    const auto end = argumentCount >= 6
        ? ParseIndex(arguments[5])
        : std::optional<std::size_t>(defaultEnd);
    const auto framesPerSecond = argumentCount >= 7
        ? ParseFramesPerSecond(arguments[6])
        : std::optional<double>(defaultFramesPerSecond);
    const bool cancelPlaceholder = argumentCount >= 8 &&
        std::wstring_view(arguments[7]) == L"-";
    const auto cancelAfterMilliseconds = argumentCount >= 8 &&
        !cancelPlaceholder
        ? ParseIndex(arguments[7])
        : std::optional<std::size_t>{};
    if (!start || !end || !framesPerSecond ||
        (argumentCount >= 8 && !cancelPlaceholder &&
            !cancelAfterMilliseconds) ||
        *start > *end ||
        *end >= sourceFrameCount) {
        std::cerr << "invalid export range or fps\n";
        return 6;
    }

    std::error_code directoryError;
    static_cast<void>(std::filesystem::create_directories(
        outputFolder,
        directoryError));
    if (directoryError ||
        !std::filesystem::is_directory(outputFolder, directoryError)) {
        std::cerr << "cannot create output folder\n";
        return 7;
    }

    zt::sequence::exporting::FfmpegExportRequest request;
    if (scan.has_value()) {
        SequenceExportSnapshot sequence;
        sequence.sourceGeneration = 1U;
        sequence.sourceFolder = sourcePath;
        sequence.orderedPngFrames = std::move(scan->frames);
        sequence.inclusiveRange = {
            static_cast<FrameIndex>(*start),
            static_cast<FrameIndex>(*end)};
        sequence.framesPerSecond = *framesPerSecond;
        request.source = std::move(sequence);
    } else {
        VideoExportSnapshot video;
        video.sourceGeneration = 1U;
        video.sourceFile = sourcePath.lexically_normal();
        video.inclusiveRange = {
            static_cast<FrameIndex>(*start),
            static_cast<FrameIndex>(*end)};
        video.sourceFramesPerSecond =
            videoProbe->metadata.framesPerSecond;
        video.framesPerSecond = *framesPerSecond;
        video.totalFrames = videoProbe->metadata.frameCount;
        video.sourceWidth = videoProbe->metadata.width;
        video.sourceHeight = videoProbe->metadata.height;
        request.source = std::move(video);
    }
    request.crop = *crop;
    request.outputFolder = outputFolder;
    if (argumentCount >= 9) {
        request.overlayImagePath =
            std::filesystem::path(arguments[8]).lexically_normal();
    }

    zt::sequence::exporting::FfmpegExportController controller;
    if (!controller.Start(std::move(request))) {
        PrintProgress(controller.Snapshot());
        return 8;
    }

    const auto started = std::chrono::steady_clock::now();
    ExportProgressSnapshot previous;
    bool cancellationRequested = false;
    for (;;) {
        const ExportProgressSnapshot progress = controller.Snapshot();
        if (progress.state != previous.state ||
            progress.completedFrames != previous.completedFrames) {
            PrintProgress(progress);
            previous = progress;
        }
        if (IsTerminal(progress.state)) {
            const double elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "elapsed_seconds=" << elapsedSeconds
                      << " output="
                      << zt::sequence::WideToUtf8(
                             progress.finalOutputPath.wstring())
                      << '\n';
            controller.Shutdown();
            const bool expectedResult = cancelAfterMilliseconds.has_value()
                ? progress.state == ExportState::Cancelled
                : progress.state == ExportState::Completed;
            return expectedResult ? 0 : 9;
        }
        if (cancelAfterMilliseconds.has_value() &&
            !cancellationRequested) {
            const double elapsedMilliseconds =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            if (elapsedMilliseconds >=
                static_cast<double>(*cancelAfterMilliseconds)) {
                cancellationRequested = true;
                controller.RequestCancel();
            }
        }
        if (std::chrono::steady_clock::now() - started >
            std::chrono::minutes(2)) {
            controller.RequestCancel();
            controller.Shutdown();
            std::cerr << "export timed out\n";
            return 10;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
