#include "Imaging/WicImageDecoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

namespace zt::sequence {
namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string HResultError(const char* operation, const HRESULT result) {
    std::ostringstream stream;
    stream << operation << " 失败，HRESULT=0x" << std::hex
           << static_cast<unsigned long>(result);
    return stream.str();
}

[[nodiscard]] bool IsSupportedDecodePercent(const std::uint32_t percent) noexcept {
    constexpr std::array<std::uint32_t, 4> kSupportedPercents{25U, 50U, 75U, 100U};
    for (const std::uint32_t supported : kSupportedPercents) {
        if (percent == supported) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint32_t ScaledDimension(
    const std::uint32_t sourceDimension,
    const std::uint32_t percent) noexcept {
    const std::uint64_t scaled =
        (static_cast<std::uint64_t>(sourceDimension) * percent + 50ULL) / 100ULL;
    return static_cast<std::uint32_t>(scaled == 0 ? 1 : scaled);
}

}  // namespace

class WicImageDecoder::Impl final {
public:
    Impl() {
        const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initializeResult)) {
            ownsComInitialization_ = true;
        } else if (initializeResult != RPC_E_CHANGED_MODE) {
            initializationError_ = HResultError("初始化 COM", initializeResult);
            return;
        }

        const HRESULT factoryResult = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
        if (FAILED(factoryResult)) {
            initializationError_ = HResultError("创建 WIC 工厂", factoryResult);
        }
    }

    ~Impl() {
        factory_.Reset();
        if (ownsComInitialization_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool IsReady() const noexcept {
        return factory_ != nullptr && initializationError_.empty();
    }

    [[nodiscard]] ImageDecodeResult Decode(
        const std::filesystem::path& file,
        const FrameIndex frameIndex,
        const Generation generation,
        const std::uint32_t decodePercent) const {
        ImageDecodeResult result;
        if (!IsReady()) {
            result.errorUtf8 = initializationError_.empty()
                ? "WIC 解码器不可用"
                : initializationError_;
            return result;
        }
        if (!IsSupportedDecodePercent(decodePercent)) {
            result.errorUtf8 = "解码比例只支持 25、50、75、100";
            return result;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT operationResult = factory_->CreateDecoderFromFilename(
            file.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            decoder.ReleaseAndGetAddressOf());
        if (FAILED(operationResult)) {
            result.errorUtf8 = HResultError("打开 PNG", operationResult);
            return result;
        }

        ComPtr<IWICBitmapFrameDecode> sourceFrame;
        operationResult = decoder->GetFrame(0, sourceFrame.ReleaseAndGetAddressOf());
        if (FAILED(operationResult)) {
            result.errorUtf8 = HResultError("读取 PNG 首帧", operationResult);
            return result;
        }

        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        operationResult = sourceFrame->GetSize(&sourceWidth, &sourceHeight);
        if (FAILED(operationResult) || sourceWidth == 0 || sourceHeight == 0) {
            result.errorUtf8 = FAILED(operationResult)
                ? HResultError("读取 PNG 尺寸", operationResult)
                : "PNG 尺寸无效";
            return result;
        }

        ComPtr<IWICFormatConverter> converter;
        operationResult = factory_->CreateFormatConverter(converter.ReleaseAndGetAddressOf());
        if (FAILED(operationResult)) {
            result.errorUtf8 = HResultError("创建 BGRA 转换器", operationResult);
            return result;
        }
        operationResult = converter->Initialize(
            sourceFrame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(operationResult)) {
            result.errorUtf8 = HResultError("转换为 32bpp BGRA", operationResult);
            return result;
        }

        const std::uint32_t targetWidth = ScaledDimension(sourceWidth, decodePercent);
        const std::uint32_t targetHeight = ScaledDimension(sourceHeight, decodePercent);
        IWICBitmapSource* copySource = converter.Get();
        ComPtr<IWICBitmapScaler> scaler;
        if (targetWidth != sourceWidth || targetHeight != sourceHeight) {
            operationResult = factory_->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf());
            if (FAILED(operationResult)) {
                result.errorUtf8 = HResultError("创建 WIC 缩放器", operationResult);
                return result;
            }
            operationResult = scaler->Initialize(
                converter.Get(),
                targetWidth,
                targetHeight,
                WICBitmapInterpolationModeFant);
            if (FAILED(operationResult)) {
                result.errorUtf8 = HResultError("执行 Fant 高质量缩放", operationResult);
                return result;
            }
            copySource = scaler.Get();
        }

        const std::uint64_t strideBytes64 = static_cast<std::uint64_t>(targetWidth) * 4ULL;
        const std::uint64_t pixelBytes64 = strideBytes64 * targetHeight;
        if (strideBytes64 > std::numeric_limits<UINT>::max()
            || pixelBytes64 > std::numeric_limits<UINT>::max()
            || pixelBytes64 > std::numeric_limits<std::size_t>::max()) {
            result.errorUtf8 = "解码后图片尺寸超过 WIC 单帧缓冲限制";
            return result;
        }

        auto decodedFrame = std::make_shared<DecodedFrame>();
        decodedFrame->index = frameIndex;
        decodedFrame->generation = generation;
        decodedFrame->sourceWidth = sourceWidth;
        decodedFrame->sourceHeight = sourceHeight;
        decodedFrame->width = targetWidth;
        decodedFrame->height = targetHeight;
        decodedFrame->strideBytes = static_cast<std::uint32_t>(strideBytes64);
        decodedFrame->decodePercent = decodePercent;
        decodedFrame->bgraPixels.resize(static_cast<std::size_t>(pixelBytes64));

        operationResult = copySource->CopyPixels(
            nullptr,
            static_cast<UINT>(strideBytes64),
            static_cast<UINT>(pixelBytes64),
            decodedFrame->bgraPixels.data());
        if (FAILED(operationResult)) {
            result.errorUtf8 = HResultError("复制 BGRA 像素", operationResult);
            return result;
        }

        result.frame = std::move(decodedFrame);
        return result;
    }

    [[nodiscard]] const std::string& InitializationError() const noexcept {
        return initializationError_;
    }

private:
    bool ownsComInitialization_ = false;
    ComPtr<IWICImagingFactory> factory_;
    std::string initializationError_;
};

WicImageDecoder::WicImageDecoder()
    : impl_(std::make_unique<Impl>()) {}

WicImageDecoder::~WicImageDecoder() = default;

bool WicImageDecoder::IsReady() const noexcept {
    return impl_ != nullptr && impl_->IsReady();
}

const std::string& WicImageDecoder::InitializationError() const noexcept {
    return impl_->InitializationError();
}

ImageDecodeResult WicImageDecoder::Decode(
    const std::filesystem::path& file,
    const FrameIndex frameIndex,
    const Generation generation,
    const std::uint32_t decodePercent) const {
    return impl_->Decode(file, frameIndex, generation, decodePercent);
}

}  // namespace zt::sequence
