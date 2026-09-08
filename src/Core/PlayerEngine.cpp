#include "Core/PlayerEngine.h"

#include "Core/PlayerEngineInternal.h"

#include <memory>

namespace zt::sequence {

PlayerEngine::PlayerEngine()
    : impl_(std::make_unique<Impl>()) {}

PlayerEngine::~PlayerEngine() = default;

bool PlayerEngine::LoadFolder(const std::filesystem::path& folder) {
    return impl_->LoadFolder(folder);
}

bool PlayerEngine::LoadSource(const std::filesystem::path& sourcePath) {
    return impl_->LoadSource(sourcePath);
}

bool PlayerEngine::ReloadFolder() {
    return impl_->ReloadFolder();
}

std::optional<ExportSourceSnapshot> PlayerEngine::CaptureExportSnapshot() const {
    return impl_->CaptureExportSnapshot();
}

void PlayerEngine::Tick(const double elapsedSeconds) {
    impl_->Tick(elapsedSeconds);
}

void PlayerEngine::SetExternalClockEnabled(const bool enabled) {
    impl_->SetExternalClockEnabled(enabled);
}

void PlayerEngine::RequestFrame(
    const FrameIndex frame,
    const FrameRequestKind requestKind,
    const int direction) {
    impl_->RequestFrame(frame, requestKind, direction);
}

void PlayerEngine::TogglePlayback() {
    impl_->TogglePlayback();
}

void PlayerEngine::SetPlaying(const bool playing) {
    impl_->SetPlaying(playing);
}

void PlayerEngine::BeginShuttlePlayback(
    const int direction,
    const double speedScale) {
    impl_->BeginShuttlePlayback(direction, speedScale);
}

void PlayerEngine::EndShuttlePlayback() {
    impl_->EndShuttlePlayback();
}

void PlayerEngine::StepFrame(const int delta) {
    impl_->StepFrame(delta);
}

void PlayerEngine::BeginScrub() {
    impl_->BeginScrub();
}

ScrubUpdateResult PlayerEngine::UpdateScrub(const FrameIndex frame) {
    return impl_->UpdateScrub(frame);
}

void PlayerEngine::EndScrub() {
    impl_->EndScrub();
}

void PlayerEngine::Seek(const FrameIndex frame) {
    impl_->Seek(frame);
}

void PlayerEngine::SeekNormalized(const double normalizedPosition) {
    impl_->SeekNormalized(normalizedPosition);
}

void PlayerEngine::SetPlaybackRange(
    const FrameIndex startFrame,
    const FrameIndex endFrame) {
    impl_->SetPlaybackRange(startFrame, endFrame);
}

void PlayerEngine::SetLoopPlayback(const bool enabled) {
    impl_->SetLoopPlayback(enabled);
}

void PlayerEngine::SetFramesPerSecond(const double framesPerSecond) {
    impl_->SetFramesPerSecond(framesPerSecond);
}

void PlayerEngine::SetDecodePercent(const std::uint32_t percent) {
    impl_->SetDecodePercent(percent);
}

void PlayerEngine::SetMemoryLimitBytes(const std::uint64_t bytes) {
    impl_->SetMemoryLimitBytes(bytes);
}

void PlayerEngine::SetResourceBudget(const EngineResourceBudget& budget) {
    impl_->SetResourceBudget(budget);
}

void PlayerEngine::SetBackgroundResourceMode(const bool enabled) {
    impl_->SetBackgroundResourceMode(enabled);
}

PlayerSnapshot PlayerEngine::Snapshot() const {
    return impl_->Snapshot();
}

void PlayerEngine::Shutdown() {
    impl_->Shutdown();
}

}  // namespace zt::sequence
