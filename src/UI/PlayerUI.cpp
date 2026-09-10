#include "UI/PlayerUIInternal.h"

namespace zt::sequence {

PlayerUI::PlayerUI()
    : impl_(new Impl()) {}

PlayerUI::~PlayerUI() {
    delete impl_;
    impl_ = nullptr;
}

void PlayerUI::SetUiScale(const float scale) {
    if (impl_ != nullptr) {
        impl_->SetUiScale(scale);
    }
}

void PlayerUI::ReportRendererError(const std::string_view detail) {
    if (impl_ != nullptr) {
        impl_->ReportRendererError(detail);
    }
}

void PlayerUI::Render(
    ComparisonPlayer& player,
    FrameTexture& primaryFrameTexture,
    FrameTexture& secondaryFrameTexture,
    overlay::MaskOverlayTexture& maskOverlayTexture,
    exporting::FfmpegExportController& exporter,
    const UiActions& actions,
    const bool applicationActive) {
    impl_->Render(
        player,
        primaryFrameTexture,
        secondaryFrameTexture,
        maskOverlayTexture,
        exporter,
        actions,
        applicationActive);
}

bool PlayerUI::IsSecondaryViewportAtClientPoint(
    const std::int32_t clientX,
    const std::int32_t clientY) const noexcept {
    return impl_ != nullptr &&
        impl_->IsSecondaryViewportAtClientPoint(clientX, clientY);
}

}  // namespace zt::sequence
