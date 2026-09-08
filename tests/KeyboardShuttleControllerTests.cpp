#include "UI/KeyboardShuttleController.h"

#include <iostream>

namespace {

using zt::sequence::ui_detail::KeyboardShuttleAction;
using zt::sequence::ui_detail::KeyboardShuttleController;
using zt::sequence::ui_detail::KeyboardShuttleInput;
using zt::sequence::ui_detail::kKeyboardShuttlePlaybackRate;

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

[[nodiscard]] KeyboardShuttleInput RightInput(
    const bool pressed,
    const bool down,
    const double elapsedSeconds = 0.0) noexcept {
    KeyboardShuttleInput input;
    input.navigationAllowed = true;
    input.rightPressed = pressed;
    input.rightDown = down;
    input.elapsedSeconds = elapsedSeconds;
    return input;
}

[[nodiscard]] bool NoAction(const KeyboardShuttleAction& action) noexcept {
    return action.stepDirection == 0 && action.beginDirection == 0 &&
        !action.endShuttle;
}

[[nodiscard]] bool TestTapAndHold() {
    bool passed = true;
    KeyboardShuttleController controller;

    KeyboardShuttleAction action = controller.Update(RightInput(true, true));
    passed &= Expect(
        action.stepDirection == 1 && action.beginDirection == 0 &&
            !action.endShuttle,
        "right press must perform exactly one immediate frame step");

    action = controller.Update(RightInput(false, true, 0.10));
    passed &= Expect(NoAction(action), "hold must not start before threshold 1");
    action = controller.Update(RightInput(false, true, 0.10));
    passed &= Expect(NoAction(action), "hold must not start before threshold 2");
    action = controller.Update(RightInput(false, true, 0.04));
    passed &= Expect(NoAction(action), "hold must remain pending at 0.24 seconds");
    action = controller.Update(RightInput(false, true, 0.01));
    passed &= Expect(
        action.beginDirection == 1 && action.stepDirection == 0 &&
            !action.endShuttle && controller.IsActive(),
        "hold must start one forward shuttle at 0.25 seconds");

    action = controller.Update(RightInput(false, true, 1.0));
    passed &= Expect(
        NoAction(action) && controller.IsActive(),
        "active hold must not repeatedly restart shuttle playback");
    action = controller.Update(RightInput(false, false));
    passed &= Expect(
        action.endShuttle && !controller.IsActive(),
        "releasing an active direction key must end shuttle playback");
    action = controller.Update(RightInput(false, false, 1.0));
    passed &= Expect(
        NoAction(action),
        "released direction key must remain idle");
    return passed;
}

[[nodiscard]] bool TestShortBackwardTap() {
    bool passed = true;
    KeyboardShuttleController controller;
    KeyboardShuttleInput input;
    input.navigationAllowed = true;
    input.leftPressed = true;
    input.leftDown = true;
    KeyboardShuttleAction action = controller.Update(input);
    passed &= Expect(
        action.stepDirection == -1 && action.beginDirection == 0,
        "left press must perform exactly one immediate backward step");

    input.leftPressed = false;
    input.leftDown = false;
    input.elapsedSeconds = 1.0;
    action = controller.Update(input);
    passed &= Expect(
        NoAction(action) && !controller.IsActive(),
        "short left tap must never enter continuous playback");
    return passed;
}

[[nodiscard]] bool TestInterruptionAndReleaseGate() {
    bool passed = true;
    KeyboardShuttleController controller;
    static_cast<void>(controller.Update(RightInput(true, true)));
    static_cast<void>(controller.Update(RightInput(false, true, 0.10)));
    static_cast<void>(controller.Update(RightInput(false, true, 0.10)));
    static_cast<void>(controller.Update(RightInput(false, true, 0.05)));
    passed &= Expect(controller.IsActive(), "precondition: shuttle is active");

    KeyboardShuttleInput interrupted = RightInput(false, true);
    interrupted.interrupted = true;
    KeyboardShuttleAction action = controller.Update(interrupted);
    passed &= Expect(
        action.endShuttle && !controller.IsActive(),
        "Space, focus loss, or another command must end active shuttle");

    action = controller.Update(RightInput(false, true, 1.0));
    passed &= Expect(
        NoAction(action),
        "interrupted hold must remain blocked until physical release");
    static_cast<void>(controller.Update(RightInput(false, false)));
    action = controller.Update(RightInput(true, true));
    passed &= Expect(
        action.stepDirection == 1,
        "fresh press after release must restore one-frame navigation");
    return passed;
}

[[nodiscard]] bool TestInvalidRoutingAndDualKeys() {
    bool passed = true;
    KeyboardShuttleController controller;

    KeyboardShuttleInput blocked = RightInput(true, true);
    blocked.navigationAllowed = false;
    KeyboardShuttleAction action = controller.Update(blocked);
    passed &= Expect(
        NoAction(action),
        "text input, popup, loading, or active widgets must block navigation");
    blocked.rightPressed = false;
    blocked.navigationAllowed = true;
    blocked.elapsedSeconds = 1.0;
    action = controller.Update(blocked);
    passed &= Expect(
        NoAction(action),
        "blocked press must not start after routing becomes available");

    KeyboardShuttleInput release;
    release.navigationAllowed = true;
    static_cast<void>(controller.Update(release));
    KeyboardShuttleInput dual;
    dual.navigationAllowed = true;
    dual.leftPressed = true;
    dual.rightPressed = true;
    dual.leftDown = true;
    dual.rightDown = true;
    action = controller.Update(dual);
    passed &= Expect(
        NoAction(action),
        "simultaneous opposite direction keys must not move the player");
    return passed;
}

}  // namespace

static_assert(kKeyboardShuttlePlaybackRate == 0.80);

int main() {
    const bool passed = TestTapAndHold() &&
        TestShortBackwardTap() &&
        TestInterruptionAndReleaseGate() &&
        TestInvalidRoutingAndDualKeys();
    return passed ? 0 : 1;
}
