#pragma once

#include "Core/PlayerTypes.h"

#include <filesystem>
#include <memory>
#include <string>

namespace zt::sequence {

struct ImageDecodeResult {
    std::shared_ptr<DecodedFrame> frame;
    std::string errorUtf8;

    [[nodiscard]] explicit operator bool() const noexcept {
        return frame != nullptr && errorUtf8.empty();
    }
};

class WicImageDecoder final {
public:
    WicImageDecoder();
    ~WicImageDecoder();

    WicImageDecoder(const WicImageDecoder&) = delete;
    WicImageDecoder& operator=(const WicImageDecoder&) = delete;
    WicImageDecoder(WicImageDecoder&&) = delete;
    WicImageDecoder& operator=(WicImageDecoder&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] const std::string& InitializationError() const noexcept;
    [[nodiscard]] ImageDecodeResult Decode(
        const std::filesystem::path& file,
        FrameIndex frameIndex,
        Generation generation,
        std::uint32_t decodePercent) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zt::sequence
