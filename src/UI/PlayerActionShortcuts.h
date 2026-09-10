#pragma once

#include <cstdint>

namespace zt::sequence::ui_detail {

enum class PlayerActionShortcutCommand : std::uint8_t {
    None,
    OpenLastSequence,
    ReloadSequence,
    ResetViewport,
    ExportMp4,
    TogglePortraitMask,
};

struct PlayerActionShortcutRoutingState final {
    bool applicationActive = false;
    bool popupOpen = false;
    bool wantsTextInput = false;
    bool anyItemActive = false;
    bool loading = false;
    bool canOpenLastSequence = false;
    bool canReloadSequence = false;
    bool canExport = false;
    bool exportBusy = false;
};

struct PlayerActionShortcutPressState final {
    bool openLastSequence = false;
    bool reloadSequence = false;
    bool resetViewport = false;
    bool exportMp4 = false;
    bool togglePortraitMask = false;
};

[[nodiscard]] inline constexpr bool ShouldHandlePlayerActionShortcuts(
    const PlayerActionShortcutRoutingState& state) noexcept {
    return state.applicationActive && !state.popupOpen &&
        !state.wantsTextInput && !state.anyItemActive;
}

// Resolves at most one action per frame in the same order as the documented
// shortcut list. Capability flags mirror the disabled states of the matching
// UI controls, so keyboard and mouse activation stay behaviorally identical.
[[nodiscard]] inline constexpr PlayerActionShortcutCommand
ResolvePlayerActionShortcutCommand(
    const PlayerActionShortcutRoutingState& routing,
    const PlayerActionShortcutPressState& pressed) noexcept {
    if (!ShouldHandlePlayerActionShortcuts(routing)) {
        return PlayerActionShortcutCommand::None;
    }
    if (pressed.openLastSequence && !routing.loading &&
        routing.canOpenLastSequence) {
        return PlayerActionShortcutCommand::OpenLastSequence;
    }
    if (pressed.reloadSequence && !routing.loading &&
        routing.canReloadSequence) {
        return PlayerActionShortcutCommand::ReloadSequence;
    }
    if (pressed.resetViewport) {
        return PlayerActionShortcutCommand::ResetViewport;
    }
    if (pressed.exportMp4 && !routing.loading && routing.canExport &&
        !routing.exportBusy) {
        return PlayerActionShortcutCommand::ExportMp4;
    }
    if (pressed.togglePortraitMask) {
        return PlayerActionShortcutCommand::TogglePortraitMask;
    }
    return PlayerActionShortcutCommand::None;
}

}  // namespace zt::sequence::ui_detail
