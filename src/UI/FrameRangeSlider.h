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

}  // namespace zt::sequence::ui
