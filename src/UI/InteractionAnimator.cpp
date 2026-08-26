#include "UI/InteractionAnimator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace zt::sequence::ui {
namespace {

constexpr float kCompletionAtResponseTime = 0.95F;
constexpr float kSnapDistance = 0.001F;
constexpr float kMinimumPositiveDurationSeconds = 0.001F;

[[nodiscard]] float NormalizedPositiveDuration(
    const float value,
    const float fallback) noexcept {
    if (!std::isfinite(value) || value < kMinimumPositiveDurationSeconds) {
        return fallback;
    }
    return value;
}

[[nodiscard]] float TargetValue(const bool target) noexcept {
    return target ? 1.0F : 0.0F;
}

}  // namespace

InteractionAnimator::InteractionAnimator(InteractionAnimationConfig config)
    : config_(NormalizeConfig(config)),
      nextCleanupSeconds_(
          static_cast<double>(config_.cleanupIntervalSeconds)),
      nextSystemPreferenceRefreshSeconds_(
          static_cast<double>(config_.systemPreferenceRefreshSeconds)) {
    RefreshSystemAnimationPreference();
}

void InteractionAnimator::BeginFrame(const float deltaSeconds) noexcept {
    const float validDelta =
        std::isfinite(deltaSeconds) && deltaSeconds > 0.0F
            ? deltaSeconds
            : 0.0F;

    animationDeltaSeconds_ =
        std::min(validDelta, config_.maximumAnimationDeltaSeconds);
    elapsedSeconds_ += static_cast<double>(validDelta);

    if (frameNumber_ == std::numeric_limits<std::uint64_t>::max()) {
        for (auto& [id, entry] : entries_) {
            static_cast<void>(id);
            entry.lastUpdatedFrame = 0U;
        }
        frameNumber_ = 1U;
    } else {
        ++frameNumber_;
    }

    if (elapsedSeconds_ >= nextSystemPreferenceRefreshSeconds_) {
        RefreshSystemAnimationPreference();
        nextSystemPreferenceRefreshSeconds_ =
            elapsedSeconds_ +
            static_cast<double>(config_.systemPreferenceRefreshSeconds);
    }

    if (elapsedSeconds_ >= nextCleanupSeconds_) {
        RemoveExpiredEntries();
        nextCleanupSeconds_ =
            elapsedSeconds_ +
            static_cast<double>(config_.cleanupIntervalSeconds);
    }
}

InteractionAnimation InteractionAnimator::Observe(
    const ImGuiID id,
    const InteractionSample sample) {
    auto [iterator, inserted] = entries_.try_emplace(id);
    Entry& entry = iterator->second;
    entry.target = sample;
    entry.lastSeenSeconds = elapsedSeconds_;

    if (inserted) {
        entry.lastUpdatedFrame = std::numeric_limits<std::uint64_t>::max();
    }

    if (!animationsEnabled_) {
        entry.animation.hover = TargetValue(sample.hovered);
        entry.animation.press = TargetValue(sample.pressed);
        entry.animation.focus = TargetValue(sample.focused);
        entry.lastUpdatedFrame = frameNumber_;
        return entry.animation;
    }

    if (entry.lastUpdatedFrame != frameNumber_) {
        entry.animation.hover = Advance(
            entry.animation.hover,
            sample.hovered,
            config_.hoverResponseSeconds);
        entry.animation.press = Advance(
            entry.animation.press,
            sample.pressed,
            config_.pressResponseSeconds);
        entry.animation.focus = Advance(
            entry.animation.focus,
            sample.focused,
            config_.focusResponseSeconds);
        entry.lastUpdatedFrame = frameNumber_;
    }

    return entry.animation;
}

InteractionAnimation InteractionAnimator::ObserveLastItem(const ImGuiID id) {
    const InteractionSample sample{
        .hovered = ImGui::IsItemHovered(),
        .pressed = ImGui::IsItemActive(),
        .focused = ImGui::IsItemFocused(),
    };
    return Observe(id, sample);
}

void InteractionAnimator::Forget(const ImGuiID id) {
    entries_.erase(id);
}

void InteractionAnimator::Clear() noexcept {
    entries_.clear();
}

void InteractionAnimator::RefreshSystemAnimationPreference() noexcept {
    BOOL clientAreaAnimationsEnabled = TRUE;
    const BOOL querySucceeded = ::SystemParametersInfoW(
        SPI_GETCLIENTAREAANIMATION,
        0U,
        &clientAreaAnimationsEnabled,
        0U);

    if (querySucceeded == FALSE) {
        systemAnimationPreferenceKnown_ = false;
        return;
    }

    systemAnimationPreferenceKnown_ = true;
    const bool newAnimationsEnabled = clientAreaAnimationsEnabled != FALSE;
    const bool mustSnapToTargets =
        animationsEnabled_ && !newAnimationsEnabled;
    animationsEnabled_ = newAnimationsEnabled;

    if (mustSnapToTargets) {
        SnapToTargets();
    }
}

bool InteractionAnimator::AnimationsEnabled() const noexcept {
    return animationsEnabled_;
}

bool InteractionAnimator::SystemAnimationPreferenceKnown() const noexcept {
    return systemAnimationPreferenceKnown_;
}

std::size_t InteractionAnimator::TrackedItemCount() const noexcept {
    return entries_.size();
}

InteractionAnimationConfig InteractionAnimator::NormalizeConfig(
    InteractionAnimationConfig config) noexcept {
    config.hoverResponseSeconds = NormalizedPositiveDuration(
        config.hoverResponseSeconds,
        InteractionAnimationConfig::kDefaultHoverResponseSeconds);
    config.pressResponseSeconds = NormalizedPositiveDuration(
        config.pressResponseSeconds,
        InteractionAnimationConfig::kDefaultPressResponseSeconds);
    config.focusResponseSeconds = NormalizedPositiveDuration(
        config.focusResponseSeconds,
        InteractionAnimationConfig::kDefaultFocusResponseSeconds);
    config.stateRetentionSeconds = NormalizedPositiveDuration(
        config.stateRetentionSeconds,
        InteractionAnimationConfig::kDefaultStateRetentionSeconds);
    config.cleanupIntervalSeconds = NormalizedPositiveDuration(
        config.cleanupIntervalSeconds,
        InteractionAnimationConfig::kDefaultCleanupIntervalSeconds);
    config.systemPreferenceRefreshSeconds = NormalizedPositiveDuration(
        config.systemPreferenceRefreshSeconds,
        InteractionAnimationConfig::kDefaultSystemPreferenceRefreshSeconds);
    config.maximumAnimationDeltaSeconds = NormalizedPositiveDuration(
        config.maximumAnimationDeltaSeconds,
        InteractionAnimationConfig::kDefaultMaximumAnimationDeltaSeconds);
    return config;
}

float InteractionAnimator::Advance(
    const float current,
    const bool target,
    const float responseSeconds) const noexcept {
    const float targetValue = TargetValue(target);
    if (animationDeltaSeconds_ <= 0.0F) {
        return std::clamp(current, 0.0F, 1.0F);
    }

    const float decayRate =
        -std::log(1.0F - kCompletionAtResponseTime) / responseSeconds;
    const float blend =
        -std::expm1(-decayRate * animationDeltaSeconds_);
    const float advanced = current + ((targetValue - current) * blend);

    if (std::abs(targetValue - advanced) <= kSnapDistance) {
        return targetValue;
    }
    return std::clamp(advanced, 0.0F, 1.0F);
}

void InteractionAnimator::SnapToTargets() noexcept {
    for (auto& [id, entry] : entries_) {
        static_cast<void>(id);
        entry.animation.hover = TargetValue(entry.target.hovered);
        entry.animation.press = TargetValue(entry.target.pressed);
        entry.animation.focus = TargetValue(entry.target.focused);
    }
}

void InteractionAnimator::RemoveExpiredEntries() {
    const double retentionSeconds =
        static_cast<double>(config_.stateRetentionSeconds);
    std::erase_if(entries_, [this, retentionSeconds](const auto& item) {
        const Entry& entry = item.second;
        return (elapsedSeconds_ - entry.lastSeenSeconds) >= retentionSeconds;
    });
}

}  // namespace zt::sequence::ui
