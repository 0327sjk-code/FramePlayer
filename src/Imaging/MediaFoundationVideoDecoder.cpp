#include "Imaging/MediaFoundationVideoDecoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace zt::sequence {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000LL;
constexpr std::size_t kMaximumSeekSamples = 1'000U;
constexpr DWORD kFirstVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams =
    static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSource =
    static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

[[nodiscard]] std::string HResultError(
    const char* operation,
    const HRESULT result) {
    std::ostringstream stream;
    stream << operation << "失败，HRESULT=0x" << std::hex
           << static_cast<unsigned long>(result);
    return stream.str();
}

[[nodiscard]] bool IsSupportedDecodePercent(
    const std::uint32_t percent) noexcept {
    constexpr std::array<std::uint32_t, 4U> supported{25U, 50U, 75U, 100U};
    return std::find(supported.begin(), supported.end(), percent) !=
        supported.end();
}

[[nodiscard]] std::uint32_t ScaledDimension(
    const std::uint32_t sourceDimension,
    const std::uint32_t percent) noexcept {
    const std::uint64_t scaled =
        (static_cast<std::uint64_t>(sourceDimension) * percent + 50ULL) /
        100ULL;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1ULL, scaled));
}

class MediaFoundationScope final {
public:
    MediaFoundationScope() {
        const HRESULT result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (SUCCEEDED(result)) {
            started_ = true;
        } else {
            errorUtf8_ = HResultError("初始化 Media Foundation", result);
        }
    }

    ~MediaFoundationScope() {
        if (started_) {
            static_cast<void>(::MFShutdown());
        }
    }

    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;

    [[nodiscard]] bool Started() const noexcept {
        return started_;
    }

    [[nodiscard]] const std::string& Error() const noexcept {
        return errorUtf8_;
    }

private:
    bool started_ = false;
    std::string errorUtf8_;
};

class ComApartmentScope final {
public:
    ComApartmentScope() noexcept {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(result)) {
            ownsInitialization_ = true;
        } else if (result != RPC_E_CHANGED_MODE) {
            error_ = result;
        }
    }

    ~ComApartmentScope() {
        if (ownsInitialization_) {
            ::CoUninitialize();
        }
    }

    ComApartmentScope(const ComApartmentScope&) = delete;
    ComApartmentScope& operator=(const ComApartmentScope&) = delete;

    [[nodiscard]] HRESULT Result() const noexcept {
        return error_;
    }

private:
    bool ownsInitialization_ = false;
    HRESULT error_ = S_OK;
};

[[nodiscard]] HRESULT CreateReaderAttributes(
    ComPtr<IMFAttributes>& attributes) {
    HRESULT result = ::MFCreateAttributes(
        attributes.ReleaseAndGetAddressOf(),
        4U);
    if (FAILED(result)) {
        return result;
    }
    static_cast<void>(attributes->SetUINT32(
        MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
        TRUE));
    static_cast<void>(attributes->SetUINT32(
        MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
        TRUE));
    return S_OK;
}

[[nodiscard]] HRESULT ReadNativeVideoMetadata(
    IMFSourceReader* reader,
    VideoMetadata& metadata) {
    if (reader == nullptr) {
        return E_POINTER;
    }

    ComPtr<IMFMediaType> nativeType;
    HRESULT result = reader->GetNativeMediaType(
        kFirstVideoStream,
        0U,
        nativeType.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    UINT32 width = 0U;
    UINT32 height = 0U;
    result = ::MFGetAttributeSize(
        nativeType.Get(),
        MF_MT_FRAME_SIZE,
        &width,
        &height);
    if (FAILED(result) || width == 0U || height == 0U) {
        return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
    }

    UINT32 numerator = 0U;
    UINT32 denominator = 0U;
    result = ::MFGetAttributeRatio(
        nativeType.Get(),
        MF_MT_FRAME_RATE,
        &numerator,
        &denominator);
    if (FAILED(result) || numerator == 0U || denominator == 0U) {
        return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
    }

    PROPVARIANT duration;
    ::PropVariantInit(&duration);
    result = reader->GetPresentationAttribute(
        kMediaSource,
        MF_PD_DURATION,
        &duration);
    const LONGLONG durationValue = SUCCEEDED(result) && duration.vt == VT_UI8
        ? static_cast<LONGLONG>(duration.uhVal.QuadPart)
        : (SUCCEEDED(result) && duration.vt == VT_I8
            ? duration.hVal.QuadPart
            : 0LL);
    ::PropVariantClear(&duration);
    if (FAILED(result) || durationValue <= 0LL) {
        return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
    }

    const double framesPerSecond =
        static_cast<double>(numerator) / static_cast<double>(denominator);
    const long double estimatedFrames =
        static_cast<long double>(durationValue) *
        static_cast<long double>(framesPerSecond) /
        static_cast<long double>(kHundredNanosecondsPerSecond);
    if (!std::isfinite(framesPerSecond) || framesPerSecond <= 0.0 ||
        !std::isfinite(estimatedFrames) || estimatedFrames < 0.5L ||
        estimatedFrames > static_cast<long double>(
            std::numeric_limits<FrameIndex>::max()) + 1.0L) {
        return MF_E_INVALIDMEDIATYPE;
    }

    metadata.width = width;
    metadata.height = height;
    metadata.framesPerSecond = framesPerSecond;
    metadata.frameCount = static_cast<std::size_t>(
        std::max<long double>(1.0L, std::round(estimatedFrames)));
    metadata.durationHundredNanoseconds = durationValue;
    return S_OK;
}

[[nodiscard]] HRESULT CreateSourceReader(
    const std::filesystem::path& videoFile,
    ComPtr<IMFSourceReader>& reader) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = CreateReaderAttributes(attributes);
    if (FAILED(result)) {
        return result;
    }
    result = ::MFCreateSourceReaderFromURL(
        videoFile.c_str(),
        attributes.Get(),
        reader.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    static_cast<void>(reader->SetStreamSelection(
        kAllStreams,
        FALSE));
    return reader->SetStreamSelection(
        kFirstVideoStream,
        TRUE);
}

[[nodiscard]] std::size_t CheckedPixelBytes(
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) * 4ULL;
    if (bytes == 0ULL ||
        bytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) ||
        bytes > static_cast<std::uint64_t>(
            std::numeric_limits<UINT>::max())) {
        return 0U;
    }
    return static_cast<std::size_t>(bytes);
}

}  // namespace

class MediaFoundationVideoDecoder::Impl final {
public:
    Impl() {
        const HRESULT initializeResult = ::CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);
        if (SUCCEEDED(initializeResult)) {
            ownsComInitialization_ = true;
        } else if (initializeResult != RPC_E_CHANGED_MODE) {
            initializationError_ = HResultError(
                "初始化视频解码线程 COM",
                initializeResult);
            return;
        }

        mediaFoundation_ = std::make_unique<MediaFoundationScope>();
        if (!mediaFoundation_->Started()) {
            initializationError_ = mediaFoundation_->Error();
            return;
        }

        const HRESULT factoryResult = ::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(factoryResult)) {
            initializationError_ = HResultError(
                "创建视频缩放 WIC 工厂",
                factoryResult);
        }
    }

    ~Impl() {
        if (reader_) {
            static_cast<void>(reader_->Flush(kFirstVideoStream));
            static_cast<void>(reader_->SetStreamSelection(
                kFirstVideoStream,
                FALSE));
        }
        reader_.Reset();
        wicFactory_.Reset();
        mediaFoundation_.reset();
        if (ownsComInitialization_) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] VideoDecodeResult Decode(
        const std::filesystem::path& videoFile,
        const FrameIndex frameIndex,
        const Generation generation,
        const std::uint32_t decodePercent,
        const std::atomic_bool* const cancelled) {
        VideoDecodeResult result;
        const auto isCancelled = [cancelled]() noexcept {
            return cancelled != nullptr &&
                cancelled->load(std::memory_order_acquire);
        };
        if (isCancelled()) {
            result.errorUtf8 = "视频解码已取消";
            return result;
        }
        if (!initializationError_.empty()) {
            result.errorUtf8 = initializationError_;
            return result;
        }
        if (!IsSupportedDecodePercent(decodePercent)) {
            result.errorUtf8 = "视频解码比例只支持 25、50、75、100";
            return result;
        }
        if (videoFile.empty()) {
            result.errorUtf8 = "视频路径为空";
            return result;
        }

        if (!reader_ || videoFile != openFile_ ||
            decodePercent != openDecodePercent_) {
            if (!Open(videoFile, decodePercent, result.errorUtf8)) {
                return result;
            }
        }
        if (static_cast<std::size_t>(frameIndex) >= metadata_.frameCount) {
            result.errorUtf8 = "请求的视频帧超过总帧数";
            return result;
        }

        const bool seekRequired = !nextFrameValid_ || nextFrameIndex_ != frameIndex;
        const LONGLONG targetTime = FrameTime(frameIndex);
        if (seekRequired) {
            if (isCancelled()) {
                result.errorUtf8 = "视频解码已取消";
                return result;
            }
            PROPVARIANT position;
            ::PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = targetTime;
            const HRESULT flushResult = reader_->Flush(kFirstVideoStream);
            if (FAILED(flushResult)) {
                result.errorUtf8 = HResultError("刷新视频解码队列", flushResult);
                nextFrameValid_ = false;
                return result;
            }
            const HRESULT seekResult = reader_->SetCurrentPosition(
                GUID_NULL,
                position);
            ::PropVariantClear(&position);
            if (FAILED(seekResult)) {
                result.errorUtf8 = HResultError("定位视频帧", seekResult);
                nextFrameValid_ = false;
                return result;
            }
        }

        ComPtr<IMFSample> sample;
        LONGLONG sampleTime = 0LL;
        const LONGLONG halfFrameDuration = std::max<LONGLONG>(
            1LL,
            FrameDuration() / 2LL);
        for (std::size_t attempt = 0U;
             attempt < kMaximumSeekSamples;
             ++attempt) {
            if (isCancelled()) {
                result.errorUtf8 = "视频解码已取消";
                nextFrameValid_ = false;
                return result;
            }
            DWORD actualStream = 0U;
            DWORD flags = 0U;
            LONGLONG timestamp = 0LL;
            ComPtr<IMFSample> candidate;
            const HRESULT readResult = reader_->ReadSample(
                kFirstVideoStream,
                0U,
                &actualStream,
                &flags,
                &timestamp,
                candidate.ReleaseAndGetAddressOf());
            if (FAILED(readResult)) {
                result.errorUtf8 = HResultError("读取视频帧", readResult);
                nextFrameValid_ = false;
                return result;
            }
            if (isCancelled()) {
                result.errorUtf8 = "视频解码已取消";
                nextFrameValid_ = false;
                return result;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0U) {
                result.errorUtf8 = "视频已到末尾，未读取到目标帧";
                nextFrameValid_ = false;
                return result;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0U) {
                if (!RefreshOutputSize(result.errorUtf8)) {
                    nextFrameValid_ = false;
                    return result;
                }
            }
            if (!candidate) {
                continue;
            }
            if (seekRequired && timestamp + halfFrameDuration < targetTime) {
                continue;
            }
            sample = std::move(candidate);
            sampleTime = timestamp;
            break;
        }
        if (!sample) {
            result.errorUtf8 = "定位视频目标帧超时";
            nextFrameValid_ = false;
            return result;
        }

        result.frame = CopySample(
            sample.Get(),
            frameIndex,
            generation,
            decodePercent,
            result.errorUtf8);
        if (!result.frame) {
            nextFrameValid_ = false;
            return result;
        }

        const long double nextByTimestamp = std::round(
            static_cast<long double>(sampleTime) *
            static_cast<long double>(metadata_.framesPerSecond) /
            static_cast<long double>(kHundredNanosecondsPerSecond));
        const std::size_t decodedIndex = nextByTimestamp >= 0.0L
            ? static_cast<std::size_t>(nextByTimestamp)
            : static_cast<std::size_t>(frameIndex);
        const std::size_t nextIndex = std::max<std::size_t>(
            static_cast<std::size_t>(frameIndex) + 1U,
            decodedIndex + 1U);
        nextFrameValid_ = nextIndex < metadata_.frameCount;
        nextFrameIndex_ = nextFrameValid_
            ? static_cast<FrameIndex>(nextIndex)
            : frameIndex;
        return result;
    }

    void PrepareForProcessShutdown() noexcept {
        static_cast<void>(reader_.Detach());
        static_cast<void>(mediaFoundation_.release());
        openFile_.clear();
        metadata_ = {};
        nextFrameValid_ = false;
    }

private:
    [[nodiscard]] bool Open(
        const std::filesystem::path& videoFile,
        const std::uint32_t decodePercent,
        std::string& errorUtf8) {
        reader_.Reset();
        openFile_.clear();
        nextFrameValid_ = false;
        metadata_ = {};

        HRESULT result = CreateSourceReader(videoFile, reader_);
        if (FAILED(result)) {
            errorUtf8 = HResultError("打开视频", result);
            return false;
        }
        result = ReadNativeVideoMetadata(reader_.Get(), metadata_);
        if (FAILED(result)) {
            errorUtf8 = HResultError("读取视频信息", result);
            reader_.Reset();
            return false;
        }

        targetWidth_ = ScaledDimension(metadata_.width, decodePercent);
        targetHeight_ = ScaledDimension(metadata_.height, decodePercent);

        ComPtr<IMFMediaType> outputType;
        result = ::MFCreateMediaType(outputType.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) {
            result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        if (SUCCEEDED(result)) {
            result = ::MFSetAttributeSize(
                outputType.Get(),
                MF_MT_FRAME_SIZE,
                targetWidth_,
                targetHeight_);
        }
        if (SUCCEEDED(result)) {
            result = reader_->SetCurrentMediaType(
                kFirstVideoStream,
                nullptr,
                outputType.Get());
        }
        if (FAILED(result)) {
            // Some decoders convert to RGB32 but do not expose native scaling.
            // Request full-size RGB32 and use WIC only for the final resize.
            outputType.Reset();
            result = ::MFCreateMediaType(outputType.ReleaseAndGetAddressOf());
            if (SUCCEEDED(result)) {
                result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            }
            if (SUCCEEDED(result)) {
                result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            }
            if (SUCCEEDED(result)) {
                result = reader_->SetCurrentMediaType(
                    kFirstVideoStream,
                    nullptr,
                    outputType.Get());
            }
        }
        if (FAILED(result)) {
            errorUtf8 = HResultError("配置视频 BGRA 输出", result);
            reader_.Reset();
            return false;
        }
        if (!RefreshOutputSize(errorUtf8)) {
            reader_.Reset();
            return false;
        }

        openFile_ = videoFile;
        openDecodePercent_ = decodePercent;
        return true;
    }

    [[nodiscard]] bool RefreshOutputSize(std::string& errorUtf8) {
        ComPtr<IMFMediaType> outputType;
        const HRESULT result = reader_->GetCurrentMediaType(
            kFirstVideoStream,
            outputType.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            errorUtf8 = HResultError("读取视频输出格式", result);
            return false;
        }
        UINT32 width = 0U;
        UINT32 height = 0U;
        const HRESULT sizeResult = ::MFGetAttributeSize(
            outputType.Get(),
            MF_MT_FRAME_SIZE,
            &width,
            &height);
        if (FAILED(sizeResult) || width == 0U || height == 0U) {
            errorUtf8 = FAILED(sizeResult)
                ? HResultError("读取视频输出尺寸", sizeResult)
                : "视频输出尺寸无效";
            return false;
        }
        outputWidth_ = width;
        outputHeight_ = height;
        return true;
    }

    [[nodiscard]] LONGLONG FrameDuration() const noexcept {
        return metadata_.framesPerSecond > 0.0
            ? static_cast<LONGLONG>(std::llround(
                static_cast<double>(kHundredNanosecondsPerSecond) /
                metadata_.framesPerSecond))
            : 1LL;
    }

    [[nodiscard]] LONGLONG FrameTime(
        const FrameIndex frameIndex) const noexcept {
        const long double time =
            static_cast<long double>(frameIndex) *
            static_cast<long double>(kHundredNanosecondsPerSecond) /
            static_cast<long double>(metadata_.framesPerSecond);
        return static_cast<LONGLONG>(std::llround(time));
    }

    [[nodiscard]] std::shared_ptr<DecodedFrame> CopySample(
        IMFSample* sample,
        const FrameIndex frameIndex,
        const Generation generation,
        const std::uint32_t decodePercent,
        std::string& errorUtf8) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = sample->ConvertToContiguousBuffer(
            buffer.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            errorUtf8 = HResultError("合并视频帧缓冲", result);
            return {};
        }

        const std::size_t outputBytes = CheckedPixelBytes(
            outputWidth_,
            outputHeight_);
        if (outputBytes == 0U) {
            errorUtf8 = "视频帧缓冲尺寸超过限制";
            return {};
        }
        std::vector<std::uint8_t> fullPixels(outputBytes);
        const LONG destinationStride = static_cast<LONG>(outputWidth_ * 4U);

        ComPtr<IMF2DBuffer> buffer2d;
        result = buffer.As(&buffer2d);
        if (SUCCEEDED(result) && buffer2d) {
            BYTE* scanline = nullptr;
            LONG pitch = 0L;
            result = buffer2d->Lock2D(&scanline, &pitch);
            if (SUCCEEDED(result)) {
                result = ::MFCopyImage(
                    fullPixels.data(),
                    destinationStride,
                    scanline,
                    pitch,
                    outputWidth_ * 4U,
                    outputHeight_);
                static_cast<void>(buffer2d->Unlock2D());
            }
        } else {
            BYTE* data = nullptr;
            DWORD maximumLength = 0U;
            DWORD currentLength = 0U;
            result = buffer->Lock(&data, &maximumLength, &currentLength);
            if (SUCCEEDED(result)) {
                if (data == nullptr || currentLength < outputBytes) {
                    result = MF_E_BUFFERTOOSMALL;
                } else {
                    std::copy_n(data, outputBytes, fullPixels.data());
                }
                static_cast<void>(buffer->Unlock());
            }
        }
        if (FAILED(result)) {
            errorUtf8 = HResultError("复制视频 BGRA 像素", result);
            return {};
        }
        for (std::size_t index = 3U; index < fullPixels.size(); index += 4U) {
            fullPixels[index] = 0xFFU;
        }

        std::vector<std::uint8_t> finalPixels;
        std::uint32_t finalWidth = outputWidth_;
        std::uint32_t finalHeight = outputHeight_;
        if (outputWidth_ != targetWidth_ || outputHeight_ != targetHeight_) {
            const std::size_t targetBytes = CheckedPixelBytes(
                targetWidth_,
                targetHeight_);
            if (targetBytes == 0U || !wicFactory_) {
                errorUtf8 = "视频缩放缓冲尺寸无效";
                return {};
            }

            ComPtr<IWICBitmap> bitmap;
            result = wicFactory_->CreateBitmapFromMemory(
                outputWidth_,
                outputHeight_,
                GUID_WICPixelFormat32bppBGRA,
                outputWidth_ * 4U,
                static_cast<UINT>(fullPixels.size()),
                fullPixels.data(),
                bitmap.ReleaseAndGetAddressOf());
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(result)) {
                result = wicFactory_->CreateBitmapScaler(
                    scaler.ReleaseAndGetAddressOf());
            }
            if (SUCCEEDED(result)) {
                result = scaler->Initialize(
                    bitmap.Get(),
                    targetWidth_,
                    targetHeight_,
                    WICBitmapInterpolationModeFant);
            }
            if (FAILED(result)) {
                errorUtf8 = HResultError("缩放视频帧", result);
                return {};
            }
            finalPixels.resize(targetBytes);
            result = scaler->CopyPixels(
                nullptr,
                targetWidth_ * 4U,
                static_cast<UINT>(finalPixels.size()),
                finalPixels.data());
            if (FAILED(result)) {
                errorUtf8 = HResultError("复制缩放视频帧", result);
                return {};
            }
            finalWidth = targetWidth_;
            finalHeight = targetHeight_;
        } else {
            finalPixels = std::move(fullPixels);
        }

        auto frame = std::make_shared<DecodedFrame>();
        frame->index = frameIndex;
        frame->generation = generation;
        frame->sourceWidth = metadata_.width;
        frame->sourceHeight = metadata_.height;
        frame->width = finalWidth;
        frame->height = finalHeight;
        frame->strideBytes = finalWidth * 4U;
        frame->decodePercent = decodePercent;
        frame->bgraPixels = std::move(finalPixels);
        return frame;
    }

    bool ownsComInitialization_ = false;
    std::unique_ptr<MediaFoundationScope> mediaFoundation_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<IMFSourceReader> reader_;
    std::filesystem::path openFile_;
    std::uint32_t openDecodePercent_ = 0U;
    VideoMetadata metadata_;
    std::uint32_t targetWidth_ = 0U;
    std::uint32_t targetHeight_ = 0U;
    std::uint32_t outputWidth_ = 0U;
    std::uint32_t outputHeight_ = 0U;
    bool nextFrameValid_ = false;
    FrameIndex nextFrameIndex_ = 0U;
    std::string initializationError_;
};

MediaFoundationVideoDecoder::MediaFoundationVideoDecoder()
    : impl_(std::make_unique<Impl>()) {}

MediaFoundationVideoDecoder::~MediaFoundationVideoDecoder() = default;

VideoDecodeResult MediaFoundationVideoDecoder::Decode(
    const std::filesystem::path& videoFile,
    const FrameIndex frameIndex,
    const Generation generation,
    const std::uint32_t decodePercent,
    const std::atomic_bool* const cancelled) {
    return impl_->Decode(
        videoFile,
        frameIndex,
        generation,
        decodePercent,
        cancelled);
}

void MediaFoundationVideoDecoder::PrepareForProcessShutdown() noexcept {
    impl_->PrepareForProcessShutdown();
}

VideoProbeResult ProbeVideoFile(
    const std::filesystem::path& videoFile) noexcept {
    VideoProbeResult probe;
    try {
        if (videoFile.empty()) {
            probe.errorUtf8 = "视频路径为空";
            return probe;
        }
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(videoFile, fileError) || fileError) {
            probe.errorUtf8 = "视频文件不存在或无法访问";
            return probe;
        }

        ComApartmentScope comApartment;
        if (FAILED(comApartment.Result())) {
            probe.errorUtf8 = HResultError(
                "初始化视频探测 COM",
                comApartment.Result());
            return probe;
        }

        MediaFoundationScope mediaFoundation;
        if (!mediaFoundation.Started()) {
            probe.errorUtf8 = mediaFoundation.Error();
            return probe;
        }

        ComPtr<IMFSourceReader> reader;
        HRESULT result = CreateSourceReader(videoFile, reader);
        if (FAILED(result)) {
            probe.errorUtf8 = HResultError("打开视频", result);
            return probe;
        }
        result = ReadNativeVideoMetadata(reader.Get(), probe.metadata);
        if (FAILED(result)) {
            probe.errorUtf8 = HResultError("读取视频信息", result);
        }
    } catch (...) {
        probe.errorUtf8 = "读取视频信息时发生异常";
    }
    return probe;
}

}  // namespace zt::sequence
