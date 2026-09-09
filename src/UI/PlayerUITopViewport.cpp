#include "UI/PlayerUIInternal.h"

#include "Core/ComparisonPlayer.h"
#include "Overlay/MaskOverlayTexture.h"
#include "Render/FrameTexture.h"
#include "UI/ComparisonCanvasLayout.h"
#include "UI/PlayerUILogic.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace zt::sequence {

using ui_internal::EllipsizedText;
using ui_internal::kColorBorder;
using ui_internal::kColorInk;
using ui_internal::kColorMask;
using ui_internal::kColorMaskBorder;
using ui_internal::kColorMuted;
using ui_internal::kColorPrimary;
using ui_internal::kColorSurface;
using ui_internal::kColorSurfaceRaised;
using ui_internal::kColorViewportBackground;
using ui_internal::kColorViewportBadge;
using ui_internal::kColorViewportBadgeText;
using ui_internal::kControlHeight;
using ui_internal::kViewportScrubThresholdPixels;
using ui_internal::TooltipForLastItem;
using ui_internal::WithAlpha;

namespace {

inline constexpr ImU32 kComparisonCanvasBlack = IM_COL32(0, 0, 0, 255);

void DrawCenteredText(
    ImDrawList* const drawList,
    const char* const text,
    const ImVec2 minimum,
    const ImVec2 maximum,
    const ImU32 color,
    const float verticalOffset = 0.0F) {
    if (drawList == nullptr || text == nullptr) {
        return;
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    drawList->AddText(
        ImVec2(
            minimum.x + ((maximum.x - minimum.x) - textSize.x) * 0.5F,
            minimum.y + ((maximum.y - minimum.y) - textSize.y) * 0.5F +
                verticalOffset),
        color,
        text);
}

}  // namespace

void PlayerUI::Impl::RenderTopBar(const PlayerSnapshot& snapshot) {
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float topBarHeight = TopBarHeightForWidth(availableWidth);
    const float horizontalPadding = Scale(16.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::BeginChild(
        "##TopBar",
        ImVec2(0.0F, topBarHeight),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::SetCursorPos(ImVec2(horizontalPadding, Scale(4.0F)));
    RenderSequenceIdentity(
        snapshot,
        std::max(
            Scale(1.0F),
            ImGui::GetWindowSize().x - (horizontalPadding * 2.0F)));

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PlayerUI::Impl::RenderSequenceIdentity(
    const PlayerSnapshot& snapshot,
    const float availableWidth) {
    ImGui::SetCursorPosY(
        ImGui::GetCursorPosY() +
        std::max(
            0.0F,
            (Scale(kControlHeight) - ImGui::GetTextLineHeight()) * 0.5F));
    const char* sequencePath =
        snapshot.hasSource && !snapshot.folderUtf8.empty()
        ? snapshot.folderUtf8.c_str()
        : "未加载来源";
    if (!snapshot.hasSource) {
        EllipsizedText(sequencePath, availableWidth, kColorInk);
        TooltipForLastItem(sequencePath);
        return;
    }

    const std::string frameCount =
        "· " + std::to_string(snapshot.totalFrames) + " 帧";
    const float frameCountWidth = ImGui::CalcTextSize(frameCount.c_str()).x;
    const float spacing = Scale(8.0F);
    const float pathWidth = std::max(
        Scale(24.0F),
        availableWidth - frameCountWidth - spacing);
    EllipsizedText(sequencePath, pathWidth, kColorInk);
    TooltipForLastItem(sequencePath);
    ImGui::SameLine(0.0F, spacing);
    ImGui::TextColored(kColorMuted, "%s", frameCount.c_str());
}

void PlayerUI::Impl::RenderViewport(
    ComparisonPlayer& player,
    const ComparisonPlayerSnapshot& comparisonSnapshot,
    FrameTexture& primaryFrameTexture,
    FrameTexture& secondaryFrameTexture,
    const overlay::MaskOverlayTexture& maskOverlayTexture,
    const float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorViewportBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(Scale(12.0F), Scale(12.0F)));
    ImGui::BeginChild(
        "##Viewport",
        ImVec2(0.0F, height),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    primaryViewportRect_.valid = false;
    secondaryViewportRect_.valid = false;

    PlayerSnapshot primarySnapshot = comparisonSnapshot.primary;
    PlayerSnapshot secondarySnapshot = comparisonSnapshot.secondary;
    if (comparisonSnapshot.active) {
        primarySnapshot.playing = comparisonSnapshot.playing;
        primarySnapshot.buffering = comparisonSnapshot.buffering;
        primarySnapshot.currentFrame = comparisonSnapshot.currentFrame;
        primarySnapshot.requestedFrame = comparisonSnapshot.requestedFrame;
        primarySnapshot.displayRevision = comparisonSnapshot.pairRevision;
        primarySnapshot.displayFrame =
            comparisonSnapshot.primaryDisplayFrame;

        secondarySnapshot.playing = comparisonSnapshot.playing;
        secondarySnapshot.buffering = comparisonSnapshot.buffering;
        secondarySnapshot.currentFrame = comparisonSnapshot.currentFrame;
        secondarySnapshot.requestedFrame = comparisonSnapshot.requestedFrame;
        secondarySnapshot.displayRevision = comparisonSnapshot.pairRevision;
        secondarySnapshot.displayFrame =
            comparisonSnapshot.secondaryDisplayFrame;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (!comparisonSnapshot.enabled) {
        if (!primarySnapshot.hasSource) {
            RenderEmptyOrLoading(
                primarySnapshot,
                available,
                ViewportPane::Primary,
                false);
        } else {
            RenderSequenceViewport(
                player,
                primarySnapshot,
                primaryFrameTexture,
                available,
                ViewportPane::Primary,
                false,
                primarySnapshot.totalFrames,
                true,
                FrameTextureUploadDomain::PlayerEngine,
                maskOverlayTexture);
        }
    } else {
        const float dividerWidth = Scale(2.0F);
        const float paneWidth = std::max(
            1.0F,
            (available.x - dividerWidth) * 0.5F);
        const ImVec2 paneSize{paneWidth, std::max(1.0F, available.y)};

        if (!primarySnapshot.hasSource) {
            RenderEmptyOrLoading(
                primarySnapshot,
                paneSize,
                ViewportPane::Primary,
                true);
        } else {
            RenderSequenceViewport(
                player,
                primarySnapshot,
                primaryFrameTexture,
                paneSize,
                ViewportPane::Primary,
                true,
                comparisonSnapshot.active
                    ? comparisonSnapshot.totalFrames
                    : primarySnapshot.totalFrames,
                !comparisonSnapshot.active ||
                    comparisonSnapshot.primaryFrameAvailable,
                comparisonSnapshot.active
                    ? FrameTextureUploadDomain::ComparisonPair
                    : FrameTextureUploadDomain::PlayerEngine,
                maskOverlayTexture);
        }

        ImGui::SameLine(0.0F, dividerWidth);
        if (!secondarySnapshot.hasSource) {
            RenderEmptyOrLoading(
                secondarySnapshot,
                paneSize,
                ViewportPane::Secondary,
                true);
        } else {
            RenderSequenceViewport(
                player,
                secondarySnapshot,
                secondaryFrameTexture,
                paneSize,
                ViewportPane::Secondary,
                true,
                comparisonSnapshot.active
                    ? comparisonSnapshot.totalFrames
                    : secondarySnapshot.totalFrames,
                !comparisonSnapshot.active ||
                    comparisonSnapshot.secondaryFrameAvailable,
                comparisonSnapshot.active
                    ? FrameTextureUploadDomain::ComparisonPair
                    : FrameTextureUploadDomain::PlayerEngine,
                maskOverlayTexture);
        }

        if (primaryViewportRect_.valid && secondaryViewportRect_.valid) {
            const float dividerX =
                (primaryViewportRect_.maximumX +
                    secondaryViewportRect_.minimumX) * 0.5F;
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(dividerX, primaryViewportRect_.minimumY),
                ImVec2(dividerX, primaryViewportRect_.maximumY),
                ImGui::GetColorU32(kColorBorder),
                Scale(1.0F));
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void PlayerUI::Impl::RenderEmptyOrLoading(
    const PlayerSnapshot& snapshot,
    const ImVec2 available,
    const ViewportPane pane,
    const bool comparisonLayout) {
    const ImVec2 interactionSize{
        std::max(1.0F, available.x),
        std::max(1.0F, available.y)};
    const char* const identifier = pane == ViewportPane::Primary
        ? "##PrimaryViewportEmpty"
        : "##SecondaryViewportEmpty";
    ImGui::InvisibleButton(
        identifier,
        interactionSize,
        ImGuiButtonFlags_MouseButtonLeft);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ScreenRect& screenRect = pane == ViewportPane::Primary
        ? primaryViewportRect_
        : secondaryViewportRect_;
    screenRect = {
        minimum.x,
        minimum.y,
        maximum.x,
        maximum.y,
        true};
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        activeViewportPane_ = pane;
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(minimum, maximum, true);
    if (snapshot.loading) {
        DrawCenteredText(
            drawList,
            pane == ViewportPane::Primary
                ? "正在准备主画面…"
                : "正在准备对比画面…",
            minimum,
            maximum,
            ImGui::GetColorU32(kColorMuted),
            -Scale(14.0F));
        const float skeletonWidth = std::min(
            Scale(260.0F),
            interactionSize.x * 0.52F);
        const ImVec2 skeletonMinimum{
            minimum.x + (interactionSize.x - skeletonWidth) * 0.5F,
            minimum.y + interactionSize.y * 0.5F + Scale(14.0F)};
        drawList->AddRectFilled(
            skeletonMinimum,
            ImVec2(
                skeletonMinimum.x + skeletonWidth,
                skeletonMinimum.y + Scale(8.0F)),
            ImGui::GetColorU32(kColorSurfaceRaised),
            Scale(4.0F));
    } else if (pane == ViewportPane::Secondary && comparisonLayout) {
        DrawCenteredText(
            drawList,
            "拖入视频或 PNG 序列",
            minimum,
            maximum,
            ImGui::GetColorU32(kColorInk),
            -Scale(12.0F));
        DrawCenteredText(
            drawList,
            "将文件或文件夹拖到右侧画布",
            minimum,
            maximum,
            ImGui::GetColorU32(kColorMuted),
            Scale(14.0F));
    } else {
        DrawCenteredText(
            drawList,
            "拖入 PNG 序列文件夹或视频文件",
            minimum,
            maximum,
            ImGui::GetColorU32(kColorInk),
            -Scale(12.0F));
        DrawCenteredText(
            drawList,
            "也可以使用下方的打开文件夹或打开上次序列",
            minimum,
            maximum,
            ImGui::GetColorU32(kColorMuted),
            Scale(14.0F));
    }
    drawList->PopClipRect();

    if (comparisonLayout && activeViewportPane_ == pane) {
        drawList->AddRect(
            minimum,
            maximum,
            ImGui::GetColorU32(kColorPrimary),
            0.0F,
            0,
            Scale(1.5F));
    }
    RenderViewportBadges(
        snapshot,
        PaneState(pane),
        pane,
        comparisonLayout,
        minimum,
        maximum,
        drawList);
}

void PlayerUI::Impl::RenderSequenceViewport(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    FrameTexture& frameTexture,
    const ImVec2 available,
    const ViewportPane pane,
    const bool comparisonLayout,
    const std::size_t transportTotalFrames,
    const bool frameAvailable,
    const FrameTextureUploadDomain uploadDomain,
    const overlay::MaskOverlayTexture& maskOverlayTexture) {
    ViewportPaneState& paneState = PaneState(pane);
    const ImVec2 interactionSize{
        std::max(1.0F, available.x),
        std::max(1.0F, available.y)};
    const char* const identifier = pane == ViewportPane::Primary
        ? "##PrimaryViewportScrub"
        : "##SecondaryViewportScrub";
    ImGui::InvisibleButton(
        identifier,
        interactionSize,
        ImGuiButtonFlags_MouseButtonLeft);

    const ImVec2 viewportMin = ImGui::GetItemRectMin();
    const ImVec2 viewportMax = ImGui::GetItemRectMax();
    ScreenRect& screenRect = pane == ViewportPane::Primary
        ? primaryViewportRect_
        : secondaryViewportRect_;
    screenRect = {
        viewportMin.x,
        viewportMin.y,
        viewportMax.x,
        viewportMax.y,
        true};

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
        (ImGui::IsItemHovered() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                ImGui::GetIO().MouseWheel != 0.0F))) {
        activeViewportPane_ = pane;
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    HandleViewportScrub(
        player,
        snapshot,
        pane,
        transportTotalFrames);
    drawList->PushClipRect(viewportMin, viewportMax, true);

    const bool frameOutsideSource = !frameAvailable;
    const bool textureMatchesFrame = snapshot.displayFrame != nullptr &&
        frameTexture.MatchesUploadKey(MakeFrameTextureUploadKey(
            *snapshot.displayFrame,
            snapshot.displayRevision,
            uploadDomain));
    const std::uint32_t originalSourceWidth =
        snapshot.displayFrame != nullptr &&
            snapshot.displayFrame->sourceWidth > 0U
        ? snapshot.displayFrame->sourceWidth
        : snapshot.sourceWidth;
    const std::uint32_t originalSourceHeight =
        snapshot.displayFrame != nullptr &&
            snapshot.displayFrame->sourceHeight > 0U
        ? snapshot.displayFrame->sourceHeight
        : snapshot.sourceHeight;
    const ui::NormalizedMaskOpening requestedOpening = comparisonLayout
        ? ui::MaskOpeningForPreset(maskPreset_)
        : ui::MaskOpeningForPresetAndSource(
            maskPreset_,
            originalSourceWidth,
            originalSourceHeight);
    const ui_detail::ComparisonCanvasLayout comparisonCanvasLayout =
        comparisonLayout
        ? ui_detail::CalculateComparisonCanvasLayout(
            originalSourceWidth,
            originalSourceHeight,
            requestedOpening)
        : ui_detail::ComparisonCanvasLayout{};
    const bool hasVisibleLayout = !comparisonLayout ||
        comparisonCanvasLayout.hasVisibleContent;
    if (textureMatchesFrame &&
        frameTexture.Width() > 0U &&
        frameTexture.Height() > 0U &&
        !frameOutsideSource &&
        hasVisibleLayout) {
        const ui::NormalizedMaskOpening sourceUv = comparisonLayout
            ? comparisonCanvasLayout.sourceUv
            : ui::FullNormalizedMaskOpening();
        const ui::MaskDisplayRect comparisonCanvas = comparisonLayout
            ? ui_detail::FitComparisonCanvasToDisplay(
                comparisonCanvasLayout,
                interactionSize.x,
                interactionSize.y,
                pane == ViewportPane::Primary)
            : ui::MaskDisplayRect{};
        const ui_detail::FittedSize fitted = comparisonLayout
            ? ui_detail::FittedSize{
                comparisonCanvas.maximumX - comparisonCanvas.minimumX,
                comparisonCanvas.maximumY - comparisonCanvas.minimumY}
            : ui_detail::FitInside(
                static_cast<float>(frameTexture.Width()),
                static_cast<float>(frameTexture.Height()),
                interactionSize.x,
                interactionSize.y);
        const ui_detail::ViewportSize fittedImage{
            fitted.width,
            fitted.height};
        const float fittedOffsetX = comparisonLayout
            ? comparisonCanvas.minimumX
            : 0.0F;
        const float fittedOffsetY = comparisonLayout
            ? comparisonCanvas.minimumY
            : 0.0F;
        const ImVec2 navigationViewportMin{
            viewportMin.x + fittedOffsetX,
            viewportMin.y + fittedOffsetY};
        const ui_detail::ViewportSize viewportSize = comparisonLayout
            ? ui_detail::ViewportSize{fitted.width, fitted.height}
            : ui_detail::ViewportSize{
                interactionSize.x,
                interactionSize.y};
        HandleViewportNavigation(
            paneState,
            navigationViewportMin,
            fittedImage,
            viewportSize);
        const ui_detail::ViewportImageRect imageRect =
            ui_detail::CalculateViewportImageRect(
                paneState.transform,
                ui_detail::ViewportPoint{
                    navigationViewportMin.x,
                    navigationViewportMin.y},
                fittedImage,
                viewportSize);
        const ui::MaskDisplayRect canvasDisplay{
            imageRect.minimumX,
            imageRect.minimumY,
            imageRect.maximumX,
            imageRect.maximumY};
        ui::MaskDisplayRect openingDisplay =
            ui::MapMaskOpeningToDisplay(requestedOpening, canvasDisplay);
        if (comparisonLayout) {
            drawList->AddRectFilled(
                ImVec2(canvasDisplay.minimumX, canvasDisplay.minimumY),
                ImVec2(canvasDisplay.maximumX, canvasDisplay.maximumY),
                kComparisonCanvasBlack);
            openingDisplay = ui_detail::MapComparisonOpeningToDisplay(
                comparisonCanvasLayout,
                canvasDisplay);
            drawList->PushClipRect(
                ImVec2(openingDisplay.minimumX, openingDisplay.minimumY),
                ImVec2(openingDisplay.maximumX, openingDisplay.maximumY),
                true);
        }
        const ui::MaskDisplayRect contentDisplay = comparisonLayout
            ? ui_detail::MapComparisonContentToDisplay(
                comparisonCanvasLayout,
                canvasDisplay)
            : canvasDisplay;
        const ImVec2 imageMin{
            contentDisplay.minimumX,
            contentDisplay.minimumY};
        const ImVec2 imageMax{
            contentDisplay.maximumX,
            contentDisplay.maximumY};
        drawList->AddImage(
            ImTextureRef(
                reinterpret_cast<void*>(frameTexture.ShaderResourceView())),
            imageMin,
            imageMax,
            ImVec2(sourceUv.minimumX, sourceUv.minimumY),
            ImVec2(sourceUv.maximumX, sourceUv.maximumY));
        if (comparisonLayout) {
            drawList->PopClipRect();
        }
        if (!comparisonLayout) {
            RenderMaskOverlay(
                imageMin,
                imageMax,
                requestedOpening,
                drawList);
        }
        RenderPngMaskOverlay(
            openingDisplay,
            maskOverlayTexture,
            drawList);
    } else if (!frameOutsideSource) {
        const char* preparing = snapshot.buffering
            ? "正在同步缓冲…"
            : "正在准备画面…";
        DrawCenteredText(
            drawList,
            preparing,
            viewportMin,
            viewportMax,
            ImGui::GetColorU32(kColorMuted));
    }

    drawList->PopClipRect();
    if (paneState.panning ||
        (ImGui::IsItemHovered() &&
            ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (comparisonLayout && activeViewportPane_ == pane) {
        drawList->AddRect(
            viewportMin,
            viewportMax,
            ImGui::GetColorU32(kColorPrimary),
            0.0F,
            0,
            Scale(1.5F));
    }
    RenderViewportBadges(
        snapshot,
        paneState,
        pane,
        comparisonLayout,
        viewportMin,
        viewportMax,
        drawList);
}

void PlayerUI::Impl::HandleViewportNavigation(
    ViewportPaneState& paneState,
    const ImVec2 viewportMin,
    const ui_detail::ViewportSize fittedImage,
    const ui_detail::ViewportSize viewportSize) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.AppFocusLost || !ImGui::IsMousePosValid(&io.MousePos)) {
        paneState.panning = false;
        return;
    }
    const ui_detail::ViewportPoint mouse{
        io.MousePos.x - viewportMin.x,
        io.MousePos.y - viewportMin.y};
    const bool hovered = ImGui::IsItemHovered();

    if (paneState.panning) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            paneState.panning = false;
        } else {
            const ui_detail::ViewportPoint delta{
                mouse.x - paneState.panOriginMouse.x,
                mouse.y - paneState.panOriginMouse.y};
            paneState.transform = ui_detail::PanViewport(
                paneState.panOriginTransform,
                delta,
                fittedImage,
                viewportSize);
            return;
        }
    }

    paneState.transform = ui_detail::ClampViewportCenter(
        paneState.transform,
        fittedImage,
        viewportSize);
    if (viewportScrubbing_ || viewportScrubArmed_) {
        return;
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        paneState.panning = true;
        paneState.panOriginMouse = mouse;
        paneState.panOriginTransform = paneState.transform;
        return;
    }

    if (hovered && io.MouseWheel != 0.0F) {
        paneState.transform = ui_detail::ZoomViewportByWheel(
            paneState.transform,
            io.MouseWheel,
            mouse,
            fittedImage,
            viewportSize);
    }
}

void PlayerUI::Impl::RenderMaskOverlay(
    const ImVec2 imageMin,
    const ImVec2 imageMax,
    const ui::NormalizedMaskOpening normalizedOpening,
    ImDrawList* drawList) const {
    if (drawList == nullptr || !ui::HasMask(maskPreset_) ||
        ui::IsFullNormalizedMaskOpening(normalizedOpening)) {
        return;
    }

    const ui::MaskDisplayRect imageRect{
        imageMin.x,
        imageMin.y,
        imageMax.x,
        imageMax.y};
    const ui::MaskDisplayRect opening =
        ui::MapMaskOpeningToDisplay(normalizedOpening, imageRect);
    const ImU32 maskColor = ImGui::GetColorU32(kColorMask);

    drawList->AddRectFilled(
        imageMin,
        ImVec2(imageMax.x, opening.minimumY),
        maskColor);
    drawList->AddRectFilled(
        ImVec2(imageMin.x, opening.maximumY),
        imageMax,
        maskColor);
    drawList->AddRectFilled(
        ImVec2(imageMin.x, opening.minimumY),
        ImVec2(opening.minimumX, opening.maximumY),
        maskColor);
    drawList->AddRectFilled(
        ImVec2(opening.maximumX, opening.minimumY),
        ImVec2(imageMax.x, opening.maximumY),
        maskColor);
    const ImVec2 boundaryMinimum{
        std::floor(opening.minimumX) + 0.5F,
        std::floor(opening.minimumY) + 0.5F};
    const ImVec2 boundaryMaximum{
        std::ceil(opening.maximumX) - 0.5F,
        std::ceil(opening.maximumY) - 0.5F};
    drawList->AddRect(
        boundaryMinimum,
        boundaryMaximum,
        ImGui::GetColorU32(WithAlpha(kColorMask, 0.42F)),
        0.0F,
        0,
        Scale(3.0F));
    drawList->AddRect(
        boundaryMinimum,
        boundaryMaximum,
        ImGui::GetColorU32(kColorMaskBorder),
        0.0F,
        0,
        Scale(1.0F));
}

void PlayerUI::Impl::RenderPngMaskOverlay(
    const ui::MaskDisplayRect openingDisplay,
    const overlay::MaskOverlayTexture& maskOverlayTexture,
    ImDrawList* drawList) const {
    if (drawList == nullptr || !IsMaskOverlayActive() ||
        !maskOverlayTexture.IsLoaded() ||
        openingDisplay.maximumX <= openingDisplay.minimumX ||
        openingDisplay.maximumY <= openingDisplay.minimumY) {
        return;
    }

    drawList->AddImage(
        ImTextureRef(reinterpret_cast<void*>(
            maskOverlayTexture.ShaderResourceView())),
        ImVec2(openingDisplay.minimumX, openingDisplay.minimumY),
        ImVec2(openingDisplay.maximumX, openingDisplay.maximumY));
}

void PlayerUI::Impl::HandleViewportScrub(
    ComparisonPlayer& player,
    const PlayerSnapshot& snapshot,
    const ViewportPane pane,
    const std::size_t transportTotalFrames) {
    ViewportPaneState& paneState = PaneState(pane);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !paneState.panning) {
        activeViewportPane_ = pane;
        viewportScrubArmed_ = true;
        viewportScrubOriginFrame_ = snapshot.currentFrame;
        viewportScrubOriginX_ = io.MousePos.x;
        viewportScrubLastMouseX_ = viewportScrubOriginX_;
        lastViewportRequest_ = snapshot.currentFrame;
    }

    if (viewportScrubArmed_ && ImGui::IsMousePosValid(&io.MousePos)) {
        viewportScrubLastMouseX_ = io.MousePos.x;
    }

    if (viewportScrubArmed_ &&
        ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(
            ImGuiMouseButton_Left,
            kViewportScrubThresholdPixels)) {
        if (!viewportScrubbing_) {
            viewportScrubbing_ = true;
            player.BeginScrub();
        }
        const float deltaPixels =
            viewportScrubLastMouseX_ - viewportScrubOriginX_;
        const FrameIndex requested = ui_detail::ScrubTargetFrame(
            viewportScrubOriginFrame_,
            deltaPixels,
            transportTotalFrames);
        if (requested != lastViewportRequest_) {
            lastViewportRequest_ = requested;
            player.UpdateScrub(requested);
        }
    }

    const bool shouldFinish = viewportScrubArmed_ && (
        ImGui::IsItemDeactivated() ||
        !ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        paneState.panning ||
        io.AppFocusLost);
    if (shouldFinish) {
        if (viewportScrubbing_) {
            const float finalDeltaPixels =
                viewportScrubLastMouseX_ - viewportScrubOriginX_;
            const FrameIndex finalRequest = ui_detail::ScrubTargetFrame(
                viewportScrubOriginFrame_,
                finalDeltaPixels,
                transportTotalFrames);
            if (finalRequest != lastViewportRequest_) {
                lastViewportRequest_ = finalRequest;
                player.UpdateScrub(finalRequest);
            }
            player.EndScrub();
        }
        viewportScrubbing_ = false;
        viewportScrubArmed_ = false;
    }
}

void PlayerUI::Impl::RenderViewportBadges(
    const PlayerSnapshot& snapshot,
    const ViewportPaneState& paneState,
    const ViewportPane pane,
    const bool comparisonLayout,
    const ImVec2 viewportMin,
    const ImVec2 viewportMax,
    ImDrawList* drawList) const {
    if (drawList == nullptr) {
        return;
    }

    float leftBadgeY = viewportMin.y + Scale(10.0F);
    const auto drawLeftBadge = [this, viewportMin, drawList, &leftBadgeY](
                                   const std::string& label) {
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 badgeMin{
            viewportMin.x + Scale(10.0F),
            leftBadgeY};
        const ImVec2 badgeMax{
            badgeMin.x + textSize.x + Scale(16.0F),
            badgeMin.y + Scale(26.0F)};
        drawList->AddRectFilled(
            badgeMin,
            badgeMax,
            ImGui::GetColorU32(kColorViewportBadge),
            Scale(6.0F));
        drawList->AddText(
            ImVec2(
                badgeMin.x + Scale(8.0F),
                badgeMin.y + Scale(5.0F)),
            ImGui::GetColorU32(kColorViewportBadgeText),
            label.c_str());
        leftBadgeY = badgeMax.y + Scale(6.0F);
    };

    if (comparisonLayout) {
        drawLeftBadge(pane == ViewportPane::Primary
            ? "主画面"
            : "对比画面");
    }
    if (snapshot.buffering) {
        std::ostringstream stream;
        stream << "同步缓冲 · 前方 " << std::fixed << std::setprecision(1)
               << snapshot.readyAheadSeconds << " 秒";
        drawLeftBadge(stream.str());
    }

    float rightBadgeY = viewportMin.y + Scale(10.0F);
    const auto drawRightBadge = [this, viewportMax, drawList, &rightBadgeY](
                                    const std::string& label) {
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        const ImVec2 badgeMax{
            viewportMax.x - Scale(10.0F),
            rightBadgeY + Scale(26.0F)};
        const ImVec2 badgeMin{
            badgeMax.x - textSize.x - Scale(16.0F),
            rightBadgeY};
        drawList->AddRectFilled(
            badgeMin,
            badgeMax,
            ImGui::GetColorU32(kColorViewportBadge),
            Scale(6.0F));
        drawList->AddText(
            ImVec2(
                badgeMin.x + Scale(8.0F),
                badgeMin.y + Scale(5.0F)),
            ImGui::GetColorU32(kColorViewportBadgeText),
            label.c_str());
        rightBadgeY = badgeMax.y + Scale(6.0F);
    };

    if (!ui_detail::IsDefaultViewportTransform(paneState.transform)) {
        const auto zoomPercent = static_cast<int>(
            std::lround(static_cast<double>(paneState.transform.zoom) * 100.0));
        drawRightBadge("视图 · " + std::to_string(zoomPercent) + "%");
    }
    if (snapshot.decodePercent < 100U) {
        drawRightBadge(
            std::to_string(snapshot.decodePercent) + "% 缩放预览");
    }
    if (ui::HasMask(maskPreset_)) {
        drawRightBadge(
            "遮罩 · " + std::string(ui::MaskPresetLabel(maskPreset_)));
    }
    if (IsMaskOverlayActive()) {
        drawRightBadge("PNG 蒙版");
    }
}

}  // namespace zt::sequence
