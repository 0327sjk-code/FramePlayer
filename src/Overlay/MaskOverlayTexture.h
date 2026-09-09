#pragma once

#include "Imaging/WicImageDecoder.h"
#include "Render/FrameTexture.h"

#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace zt::sequence::overlay {

struct MaskOverlayLoadResult final {
    bool succeeded = false;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return succeeded;
    }
};

// Owns the single process-lifetime PNG overlay texture. Loading is
// transactional: decode and size validation complete before the current GPU
// texture and remembered path are replaced.
class MaskOverlayTexture final {
public:
    MaskOverlayTexture() = default;
    ~MaskOverlayTexture() = default;

    MaskOverlayTexture(const MaskOverlayTexture&) = delete;
    MaskOverlayTexture& operator=(const MaskOverlayTexture&) = delete;

    [[nodiscard]] bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context);
    [[nodiscard]] MaskOverlayLoadResult Load(
        const std::filesystem::path& pngPath);
    void Reset();

    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept;
    [[nodiscard]] const std::filesystem::path& LoadedPath() const noexcept;

private:
    WicImageDecoder decoder_;
    FrameTexture texture_;
    std::filesystem::path loadedPath_;
    std::uint64_t uploadRevision_ = 0U;
    bool initialized_ = false;
};

}  // namespace zt::sequence::overlay
