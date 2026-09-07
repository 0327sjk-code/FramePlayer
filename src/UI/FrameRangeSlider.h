#pragma once

#include "Core/PlayerTypes.h"
#include "UI/InteractionAnimator.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>

namespace zt::sequence::ui {

enum class FrameRangeHandle : std::uint8_t {
    None,
    Start,
    End,
};

struct FrameRangeSliderState final {
    FrameRangeHandle activeHandle = FrameRangeHandle::None;
    FrameRangeHandle lastHandle = FrameRangeHandle::End;
};

[[nodiscard]] bool FrameRangeSlider(
    InteractionAnimator& animator,
    const char* label,
    FrameIndex& startFrame,
    FrameIndex& endFrame,
    std::size_t totalFrames,
    ImVec2 size,
    float uiScale,
    FrameRangeSliderState& state);

// A single-handle variant used for an inclusive zero-based frame offset. The
// caller owns commit timing, so dragging can update a local candidate without
// scheduling source work until the item is released.
[[nodiscard]] bool FramePositionSlider(
    InteractionAnimator& animator,
    const char* label,
    FrameIndex& frame,
    FrameIndex maximumFrame,
    ImVec2 size,
    float uiScale);

}  // namespace zt::sequence::ui
