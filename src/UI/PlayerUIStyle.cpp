#include "UI/PlayerUIInternal.h"

namespace zt::sequence {

using ui_internal::kColorAccent;
using ui_internal::kColorBackground;
using ui_internal::kColorInk;
using ui_internal::kColorMuted;
using ui_internal::kColorPrimary;
using ui_internal::kColorPrimaryHover;
using ui_internal::kColorSurface;
using ui_internal::kColorSurfaceHover;
using ui_internal::kColorSurfaceRaised;
using ui_internal::WithAlpha;

void PlayerUI::ApplyCodexStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0F, 10.0F);
    style.FramePadding = ImVec2(10.0F, 10.0F);
    style.CellPadding = ImVec2(6.0F, 4.0F);
    style.ItemSpacing = ImVec2(8.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 4.0F);
    style.WindowRounding = 10.0F;
    style.ChildRounding = 10.0F;
    style.PopupRounding = 10.0F;
    style.FrameRounding = 8.0F;
    style.GrabRounding = 8.0F;
    style.TabRounding = 8.0F;
    style.ScrollbarRounding = 8.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 0.0F;
    style.FrameBorderSize = 0.0F;
    style.PopupBorderSize = 1.0F;
    style.GrabMinSize = 10.0F;
    style.DisabledAlpha = 0.55F;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kColorInk;
    colors[ImGuiCol_TextDisabled] = kColorMuted;
    colors[ImGuiCol_WindowBg] = kColorBackground;
    colors[ImGuiCol_ChildBg] = kColorSurface;
    colors[ImGuiCol_PopupBg] = kColorSurfaceRaised;
    colors[ImGuiCol_Border] = WithAlpha(kColorSurfaceHover, 0.95F);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_FrameBg] = kColorSurfaceRaised;
    colors[ImGuiCol_FrameBgHovered] = kColorSurfaceHover;
    colors[ImGuiCol_FrameBgActive] = kColorSurfaceHover;
    colors[ImGuiCol_TitleBg] = kColorSurface;
    colors[ImGuiCol_TitleBgActive] = kColorSurface;
    colors[ImGuiCol_MenuBarBg] = kColorSurface;
    colors[ImGuiCol_ScrollbarBg] = kColorBackground;
    colors[ImGuiCol_ScrollbarGrab] = kColorSurfaceRaised;
    colors[ImGuiCol_ScrollbarGrabHovered] = kColorSurfaceHover;
    colors[ImGuiCol_ScrollbarGrabActive] = kColorMuted;
    colors[ImGuiCol_CheckMark] = kColorPrimaryHover;
    colors[ImGuiCol_SliderGrab] = kColorPrimary;
    colors[ImGuiCol_SliderGrabActive] = kColorPrimaryHover;
    colors[ImGuiCol_Button] = kColorSurfaceRaised;
    colors[ImGuiCol_ButtonHovered] = kColorSurfaceHover;
    colors[ImGuiCol_ButtonActive] = WithAlpha(kColorPrimary, 0.78F);
    colors[ImGuiCol_Header] = WithAlpha(kColorPrimary, 0.42F);
    colors[ImGuiCol_HeaderHovered] = WithAlpha(kColorPrimary, 0.58F);
    colors[ImGuiCol_HeaderActive] = WithAlpha(kColorPrimary, 0.72F);
    colors[ImGuiCol_Separator] = kColorSurfaceHover;
    colors[ImGuiCol_SeparatorHovered] = kColorAccent;
    colors[ImGuiCol_SeparatorActive] = kColorAccent;
    colors[ImGuiCol_ResizeGrip] = WithAlpha(kColorMuted, 0.18F);
    colors[ImGuiCol_ResizeGripHovered] = WithAlpha(kColorAccent, 0.55F);
    colors[ImGuiCol_ResizeGripActive] = kColorAccent;
    colors[ImGuiCol_Tab] = kColorSurfaceRaised;
    colors[ImGuiCol_TabHovered] = kColorSurfaceHover;
    colors[ImGuiCol_TabSelected] = WithAlpha(kColorPrimary, 0.64F);
    colors[ImGuiCol_TabSelectedOverline] = kColorPrimaryHover;
    colors[ImGuiCol_TabDimmed] = kColorSurface;
    colors[ImGuiCol_TabDimmedSelected] = kColorSurfaceRaised;
    colors[ImGuiCol_TabDimmedSelectedOverline] = kColorMuted;
    colors[ImGuiCol_PlotLines] = kColorAccent;
    colors[ImGuiCol_PlotHistogram] = kColorPrimary;
    colors[ImGuiCol_TableHeaderBg] = kColorSurfaceRaised;
    colors[ImGuiCol_TableBorderStrong] = kColorSurfaceHover;
    colors[ImGuiCol_TableBorderLight] = kColorSurfaceRaised;
    colors[ImGuiCol_TextSelectedBg] = WithAlpha(kColorAccent, 0.32F);
    colors[ImGuiCol_DragDropTarget] = kColorAccent;
    colors[ImGuiCol_NavCursor] = kColorAccent;
    colors[ImGuiCol_NavWindowingHighlight] =
        WithAlpha(kColorAccent, 0.65F);
    colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.0F, 0.0F, 0.0F, 0.66F);
}

}  // namespace zt::sequence
