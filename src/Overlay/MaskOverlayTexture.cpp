#include "Overlay/MaskOverlayTexture.h"

#include "Imaging/PngImageInfo.h"
#include "Overlay/MaskOverlaySpec.h"

#include <optional>
#include <utility>

namespace zt::sequence::overlay {

bool MaskOverlayTexture::Initialize(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context) {
    Reset();
    initialized_ = texture_.Initialize(device, context);
    return initialized_;
}

MaskOverlayLoadResult MaskOverlayTexture::Load(
    const std::filesystem::path& pngPath) {
    if (!initialized_) {
        return {false, "PNG 蒙版纹理尚未初始化"};
    }
    if (!decoder_.IsReady()) {
        return {
            false,
            decoder_.InitializationError().empty()
                ? "WIC PNG 解码器不可用"
                : decoder_.InitializationError()};
    }

    std::string imageInfoError;
    const std::optional<PngImageInfo> imageInfo =
        ReadPngImageInfo(pngPath, imageInfoError);
    if (!imageInfo) {
        return {false, std::move(imageInfoError)};
    }
    if (!IsRequiredMaskOverlaySize(imageInfo->width, imageInfo->height)) {
        return {
            false,
            "PNG 蒙版尺寸必须严格为 1080 × 1920，当前为 " +
                std::to_string(imageInfo->width) + " × " +
                std::to_string(imageInfo->height)};
    }

    const ImageDecodeResult decoded = decoder_.Decode(
        pngPath,
        0U,
        0U,
        100U);
    if (!decoded) {
        return {
            false,
            decoded.errorUtf8.empty()
                ? "PNG 蒙版解码失败"
                : decoded.errorUtf8};
    }
    if (!IsRequiredMaskOverlaySize(
            decoded.frame->width,
            decoded.frame->height)) {
        return {false, "PNG 蒙版解码后的尺寸不是 1080 × 1920"};
    }

    ++uploadRevision_;
    if (uploadRevision_ == 0U) {
        uploadRevision_ = 1U;
    }
    const FrameTextureUploadKey uploadKey{
        FrameTextureUploadDomain::MaskOverlay,
        0U,
        0U,
        uploadRevision_};
    if (!texture_.Upload(*decoded.frame, uploadKey)) {
        return {false, "PNG 蒙版无法上传到 Direct3D 纹理"};
    }

    loadedPath_ = pngPath.lexically_normal();
    return {true, {}};
}

void MaskOverlayTexture::Reset() {
    texture_.Reset();
    loadedPath_.clear();
    uploadRevision_ = 0U;
    initialized_ = false;
}

bool MaskOverlayTexture::IsLoaded() const noexcept {
    return !loadedPath_.empty() && texture_.ShaderResourceView() != nullptr;
}

ID3D11ShaderResourceView* MaskOverlayTexture::ShaderResourceView() const noexcept {
    return texture_.ShaderResourceView();
}

const std::filesystem::path& MaskOverlayTexture::LoadedPath() const noexcept {
    return loadedPath_;
}

}  // namespace zt::sequence::overlay
