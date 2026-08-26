#pragma once

#include <algorithm>

namespace zt::sequence::app_detail {

inline constexpr double kInactiveBackgroundDelaySeconds = 20.0;

enum class BackgroundModeTransition {
    None = 0,
    Entered,
    Exited,
};

class ApplicationActivityPolicy final {
public:
    [[nodiscard]] BackgroundModeTransition Update(
        const bool minimized,
        const bool applicationActive,
        const double elapsedSeconds) noexcept {
        if (minimized) {
            inactiveElapsedSeconds_ = kInactiveBackgroundDelaySeconds;
            return SetBackgroundMode(true);
        }

        if (applicationActive) {
            inactiveElapsedSeconds_ = 0.0;
            return SetBackgroundMode(false);
        }

        if (backgroundMode_) {
            return BackgroundModeTransition::None;
        }

        inactiveElapsedSeconds_ = std::min(
            kInactiveBackgroundDelaySeconds,
            inactiveElapsedSeconds_ + std::max(elapsedSeconds, 0.0));
        if (inactiveElapsedSeconds_ >= kInactiveBackgroundDelaySeconds) {
            return SetBackgroundMode(true);
        }
        return BackgroundModeTransition::None;
    }

    [[nodiscard]] bool BackgroundMode() const noexcept {
        return backgroundMode_;
    }

    [[nodiscard]] double InactiveElapsedSeconds() const noexcept {
        return inactiveElapsedSeconds_;
    }

private:
    [[nodiscard]] BackgroundModeTransition SetBackgroundMode(
        const bool enabled) noexcept {
        if (backgroundMode_ == enabled) {
            return BackgroundModeTransition::None;
        }
        backgroundMode_ = enabled;
        return enabled
            ? BackgroundModeTransition::Entered
            : BackgroundModeTransition::Exited;
    }

    bool backgroundMode_ = false;
    double inactiveElapsedSeconds_ = 0.0;
};

}  // namespace zt::sequence::app_detail
