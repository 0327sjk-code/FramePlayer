#pragma once

#include <algorithm>
#include <cmath>

namespace zt::sequence::ui_detail {

inline constexpr double kKeyboardShuttleActivationSeconds = 0.25;
inline constexpr double kKeyboardShuttlePlaybackRate = 0.80;
inline constexpr double kMaximumKeyboardShuttleInputStepSeconds = 0.10;

struct KeyboardShuttleInput final {
    bool navigationAllowed = false;
    bool interrupted = false;
    bool leftPressed = false;
    bool rightPressed = false;
    bool leftDown = false;
    bool rightDown = false;
    double elapsedSeconds = 0.0;
};

struct KeyboardShuttleAction final {
    int stepDirection = 0;
    int beginDirection = 0;
    bool endShuttle = false;
};

// Separates one-frame taps from held-key shuttle playback without depending on
// the operating system's keyboard-repeat delay or repeat frequency.
class KeyboardShuttleController final {
public:
    [[nodiscard]] KeyboardShuttleAction Update(
        const KeyboardShuttleInput& input) noexcept {
        const bool anyDirectionDown = input.leftDown || input.rightDown;
        if (input.interrupted || !input.navigationAllowed) {
            return Cancel(anyDirectionDown);
        }

        if (blockedUntilRelease_) {
            if (!anyDirectionDown) {
                blockedUntilRelease_ = false;
            }
            return {};
        }

        if ((input.leftDown && input.rightDown) ||
            (input.leftPressed && input.rightPressed)) {
            return Cancel(anyDirectionDown);
        }

        const int pressedDirection = input.leftPressed
            ? -1
            : (input.rightPressed ? 1 : 0);
        const int downDirection = input.leftDown
            ? -1
            : (input.rightDown ? 1 : 0);

        if (pressedDirection != 0) {
            KeyboardShuttleAction action;
            action.endShuttle = active_;
            action.stepDirection = pressedDirection;
            active_ = false;
            trackingHold_ = downDirection == pressedDirection;
            direction_ = pressedDirection;
            heldSeconds_ = 0.0;
            return action;
        }

        if (active_) {
            if (downDirection != direction_) {
                return Cancel(anyDirectionDown);
            }
            return {};
        }

        if (!trackingHold_) {
            return {};
        }
        if (downDirection != direction_) {
            Reset();
            return {};
        }

        if (std::isfinite(input.elapsedSeconds) &&
            input.elapsedSeconds > 0.0) {
            heldSeconds_ += std::min(
                input.elapsedSeconds,
                kMaximumKeyboardShuttleInputStepSeconds);
        }
        if (heldSeconds_ + 1.0e-9 <
            kKeyboardShuttleActivationSeconds) {
            return {};
        }

        active_ = true;
        trackingHold_ = false;
        heldSeconds_ = 0.0;
        return KeyboardShuttleAction{0, direction_, false};
    }

    void Reset() noexcept {
        trackingHold_ = false;
        active_ = false;
        blockedUntilRelease_ = false;
        direction_ = 0;
        heldSeconds_ = 0.0;
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return active_;
    }

private:
    [[nodiscard]] KeyboardShuttleAction Cancel(
        const bool blockUntilRelease) noexcept {
        KeyboardShuttleAction action;
        action.endShuttle = active_;
        Reset();
        blockedUntilRelease_ = blockUntilRelease;
        return action;
    }

    bool trackingHold_ = false;
    bool active_ = false;
    bool blockedUntilRelease_ = false;
    int direction_ = 0;
    double heldSeconds_ = 0.0;
};

}  // namespace zt::sequence::ui_detail
