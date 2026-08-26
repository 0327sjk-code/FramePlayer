#pragma once

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace zt::sequence::ui {

struct InteractionSample final {
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
};

struct InteractionAnimation final {
    float hover = 0.0F;
    float press = 0.0F;
    float focus = 0.0F;
};

struct InteractionAnimationConfig final {
    static constexpr float kDefaultHoverResponseSeconds = 0.16F;
    static constexpr float kDefaultPressResponseSeconds = 0.14F;
    static constexpr float kDefaultFocusResponseSeconds = 0.18F;
    static constexpr float kDefaultStateRetentionSeconds = 8.0F;
    static constexpr float kDefaultCleanupIntervalSeconds = 1.0F;
    static constexpr float kDefaultSystemPreferenceRefreshSeconds = 1.0F;
    static constexpr float kDefaultMaximumAnimationDeltaSeconds = 0.10F;

    float hoverResponseSeconds = kDefaultHoverResponseSeconds;
    float pressResponseSeconds = kDefaultPressResponseSeconds;
    float focusResponseSeconds = kDefaultFocusResponseSeconds;
    float stateRetentionSeconds = kDefaultStateRetentionSeconds;
    float cleanupIntervalSeconds = kDefaultCleanupIntervalSeconds;
    float systemPreferenceRefreshSeconds =
        kDefaultSystemPreferenceRefreshSeconds;
    float maximumAnimationDeltaSeconds =
        kDefaultMaximumAnimationDeltaSeconds;
};

class InteractionAnimator final {
public:
    explicit InteractionAnimator(
        InteractionAnimationConfig config = InteractionAnimationConfig{});

    InteractionAnimator(const InteractionAnimator&) = delete;
    InteractionAnimator& operator=(const InteractionAnimator&) = delete;
    InteractionAnimator(InteractionAnimator&&) = delete;
    InteractionAnimator& operator=(InteractionAnimator&&) = delete;

    // Call once before submitting animated items for the current ImGui frame.
    void BeginFrame(float deltaSeconds) noexcept;

    // pressed should normally be ImGui::IsItemActive(), so it represents the
    // held state instead of a one-frame click event.
    [[nodiscard]] InteractionAnimation Observe(
        ImGuiID id,
        InteractionSample sample);
    [[nodiscard]] InteractionAnimation ObserveLastItem(ImGuiID id);

    void Forget(ImGuiID id);
    void Clear() noexcept;

    // Call from WM_SETTINGCHANGE for immediate application. BeginFrame also
    // refreshes this preference periodically.
    void RefreshSystemAnimationPreference() noexcept;

    [[nodiscard]] bool AnimationsEnabled() const noexcept;
    [[nodiscard]] bool SystemAnimationPreferenceKnown() const noexcept;
    [[nodiscard]] std::size_t TrackedItemCount() const noexcept;

private:
    struct Entry final {
        InteractionAnimation animation;
        InteractionSample target;
        double lastSeenSeconds = 0.0;
        std::uint64_t lastUpdatedFrame = 0U;
    };

    [[nodiscard]] static InteractionAnimationConfig NormalizeConfig(
        InteractionAnimationConfig config) noexcept;
    [[nodiscard]] float Advance(
        float current,
        bool target,
        float responseSeconds) const noexcept;
    void SnapToTargets() noexcept;
    void RemoveExpiredEntries();

    InteractionAnimationConfig config_;
    std::unordered_map<ImGuiID, Entry> entries_;
    float animationDeltaSeconds_ = 0.0F;
    double elapsedSeconds_ = 0.0;
    double nextCleanupSeconds_ = 0.0;
    double nextSystemPreferenceRefreshSeconds_ = 0.0;
    std::uint64_t frameNumber_ = 0U;
    bool animationsEnabled_ = true;
    bool systemAnimationPreferenceKnown_ = false;
};

}  // namespace zt::sequence::ui
