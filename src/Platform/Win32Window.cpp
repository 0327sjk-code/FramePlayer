#include "Platform/Win32Window.h"

#include "Platform/ResourceIds.h"

#include "imgui.h"
#include "imgui_impl_win32.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windows.h>

#include <string>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND windowHandle,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace zt::sequence {
namespace {

constexpr wchar_t kWindowClassName[] = L"ZTSequencePlayer.MainWindow";
constexpr std::uint32_t kDefaultDpi = 96;
constexpr std::uint32_t kMinimumLogicalWidth = 800;
constexpr std::uint32_t kMinimumLogicalHeight = 560;

[[nodiscard]] HICON LoadApplicationIcon(
    const HINSTANCE instance,
    const int width,
    const int height) {
    return static_cast<HICON>(::LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_ZT_SEQUENCE_PLAYER),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR));
}

[[nodiscard]] std::optional<DroppedSource> ResolveDroppedSource(const HDROP drop) {
    const UINT itemCount = ::DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    for (UINT index = 0; index < itemCount; ++index) {
        const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }

        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        if (::DragQueryFileW(drop, index, value.data(), length + 1U) == 0) {
            continue;
        }
        value.resize(length);
        const std::optional<DroppedSource> source =
            dropped_source::ClassifyExistingPath(std::filesystem::path(value));
        if (source.has_value()) {
            return source;
        }
    }

    return std::nullopt;
}

[[nodiscard]] WindowClientPoint ResolveDropClientPoint(
    const HWND windowHandle,
    const HDROP drop) noexcept {
    POINT clientPoint{};
    if (::DragQueryPoint(drop, &clientPoint) != FALSE) {
        return {
            static_cast<std::int32_t>(clientPoint.x),
            static_cast<std::int32_t>(clientPoint.y)};
    }

    // WM_DROPFILES normally supplies a client-area point. If Windows reports
    // a non-client drop, use the cursor and explicitly convert it back into
    // the same client coordinate space consumed by the ImGui hit rectangles.
    POINT screenPoint{};
    if (::GetCursorPos(&screenPoint) != FALSE &&
        ::ScreenToClient(windowHandle, &screenPoint) != FALSE) {
        return {
            static_cast<std::int32_t>(screenPoint.x),
            static_cast<std::int32_t>(screenPoint.y)};
    }
    return {};
}

[[nodiscard]] std::optional<WindowDropEvent> ResolveWindowDropEvent(
    const HWND windowHandle,
    const HDROP drop) {
    std::optional<DroppedSource> source = ResolveDroppedSource(drop);
    if (!source.has_value()) {
        return std::nullopt;
    }
    return WindowDropEvent{
        std::move(*source),
        ResolveDropClientPoint(windowHandle, drop)};
}

void ApplyNativeWindowAppearance(const HWND windowHandle) {
    constexpr DWORD immersiveDarkModeAttribute = 20;
    constexpr DWORD cornerPreferenceAttribute = 33;
    constexpr DWORD roundCornerPreference = 2;
    const BOOL enableDarkMode = TRUE;

    static_cast<void>(::DwmSetWindowAttribute(
        windowHandle,
        immersiveDarkModeAttribute,
        &enableDarkMode,
        sizeof(enableDarkMode)));
    static_cast<void>(::DwmSetWindowAttribute(
        windowHandle,
        cornerPreferenceAttribute,
        &roundCornerPreference,
        sizeof(roundCornerPreference)));
}

}  // namespace

class Win32Window::Impl final {
public:
    [[nodiscard]] bool Create(
        const HINSTANCE instance,
        const std::wstring_view title,
        const std::uint32_t logicalWidth,
        const std::uint32_t logicalHeight) {
        if (windowHandle_ != nullptr || instance == nullptr) {
            return false;
        }

        instance_ = instance;
        currentDpi_ = ::GetDpiForSystem();
        if (currentDpi_ == 0) {
            currentDpi_ = kDefaultDpi;
        }

        largeIcon_ = LoadApplicationIcon(
            instance_,
            ::GetSystemMetrics(SM_CXICON),
            ::GetSystemMetrics(SM_CYICON));
        smallIcon_ = LoadApplicationIcon(
            instance_,
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON));
        if (largeIcon_ == nullptr || smallIcon_ == nullptr) {
            ReleaseApplicationIcons();
            instance_ = nullptr;
            return false;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &StaticWindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hIcon = largeIcon_;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hIconSm = smallIcon_;

        classAtom_ = ::RegisterClassExW(&windowClass);
        if (classAtom_ == 0) {
            ReleaseApplicationIcons();
            instance_ = nullptr;
            return false;
        }

        const DWORD style = WS_OVERLAPPEDWINDOW;
        const DWORD extendedStyle = WS_EX_ACCEPTFILES;
        RECT windowRect{
            0,
            0,
            ::MulDiv(static_cast<int>(logicalWidth), static_cast<int>(currentDpi_), kDefaultDpi),
            ::MulDiv(static_cast<int>(logicalHeight), static_cast<int>(currentDpi_), kDefaultDpi),
        };
        static_cast<void>(::AdjustWindowRectExForDpi(
            &windowRect,
            style,
            FALSE,
            extendedStyle,
            currentDpi_));

        const std::wstring ownedTitle(title);
        windowHandle_ = ::CreateWindowExW(
            extendedStyle,
            kWindowClassName,
            ownedTitle.c_str(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr,
            nullptr,
            instance_,
            this);
        if (windowHandle_ == nullptr) {
            Destroy();
            return false;
        }

        static_cast<void>(::SendMessageW(
            windowHandle_,
            WM_SETICON,
            ICON_BIG,
            reinterpret_cast<LPARAM>(largeIcon_)));
        static_cast<void>(::SendMessageW(
            windowHandle_,
            WM_SETICON,
            ICON_SMALL,
            reinterpret_cast<LPARAM>(smallIcon_)));
        ::DragAcceptFiles(windowHandle_, TRUE);
        ApplyNativeWindowAppearance(windowHandle_);

        RECT clientRect{};
        if (::GetClientRect(windowHandle_, &clientRect) != FALSE) {
            pendingResize_ = WindowSize{
                static_cast<std::uint32_t>(clientRect.right - clientRect.left),
                static_cast<std::uint32_t>(clientRect.bottom - clientRect.top),
            };
        }
        return true;
    }

    void Show(const int showCommand) const {
        if (windowHandle_ != nullptr) {
            ::ShowWindow(windowHandle_, showCommand);
            ::UpdateWindow(windowHandle_);
        }
    }

    [[nodiscard]] bool PollEvents(WindowEvents& events) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) {
                quitRequested_ = true;
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        events = {};
        events.quitRequested = quitRequested_;
        events.minimized = minimized_;
        events.applicationActive = applicationActive_;
        events.resized = std::exchange(pendingResize_, std::nullopt);
        events.dpiChanged = std::exchange(pendingDpi_, std::nullopt);
        events.droppedSourceEvent = std::exchange(
            pendingDroppedSourceEvent_,
            std::nullopt);
        return !quitRequested_;
    }

    void RequestClose() const {
        if (windowHandle_ != nullptr) {
            ::PostMessageW(windowHandle_, WM_CLOSE, 0, 0);
        }
    }

    void Destroy() {
        if (windowHandle_ != nullptr) {
            ::DragAcceptFiles(windowHandle_, FALSE);
            ::DestroyWindow(windowHandle_);
            windowHandle_ = nullptr;
        }
        if (classAtom_ != 0 && instance_ != nullptr) {
            ::UnregisterClassW(kWindowClassName, instance_);
            classAtom_ = 0;
        }
        ReleaseApplicationIcons();
        instance_ = nullptr;
    }

    [[nodiscard]] HWND Handle() const noexcept {
        return windowHandle_;
    }

    [[nodiscard]] std::uint32_t CurrentDpi() const noexcept {
        return currentDpi_;
    }

private:
    void ReleaseApplicationIcons() noexcept {
        if (smallIcon_ != nullptr && smallIcon_ != largeIcon_) {
            static_cast<void>(::DestroyIcon(smallIcon_));
        }
        if (largeIcon_ != nullptr) {
            static_cast<void>(::DestroyIcon(largeIcon_));
        }
        smallIcon_ = nullptr;
        largeIcon_ = nullptr;
    }

    static LRESULT CALLBACK StaticWindowProcedure(
        const HWND windowHandle,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            self->windowHandle_ = windowHandle;
            ::SetWindowLongPtrW(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self != nullptr) {
            return self->WindowProcedure(windowHandle, message, wParam, lParam);
        }
        return ::DefWindowProcW(windowHandle, message, wParam, lParam);
    }

    LRESULT WindowProcedure(
        const HWND windowHandle,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) {
        if (ImGui::GetCurrentContext() != nullptr &&
            ImGui_ImplWin32_WndProcHandler(windowHandle, message, wParam, lParam) != 0) {
            return 1;
        }

        switch (message) {
        case WM_ACTIVATEAPP:
            applicationActive_ = wParam != FALSE;
            return 0;

        case WM_SIZE:
            minimized_ = wParam == SIZE_MINIMIZED;
            if (!minimized_) {
                pendingResize_ = WindowSize{
                    static_cast<std::uint32_t>(LOWORD(lParam)),
                    static_cast<std::uint32_t>(HIWORD(lParam)),
                };
            }
            return 0;

        case WM_DPICHANGED: {
            currentDpi_ = HIWORD(wParam);
            pendingDpi_ = currentDpi_;
            const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(
                windowHandle,
                nullptr,
                suggestedRect->left,
                suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }

        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            std::optional<WindowDropEvent> dropEvent =
                ResolveWindowDropEvent(windowHandle, drop);
            pendingDroppedSourceEvent_ = std::move(dropEvent);
            ::DragFinish(drop);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* minimumInfo = reinterpret_cast<MINMAXINFO*>(lParam);
            minimumInfo->ptMinTrackSize.x =
                ::MulDiv(static_cast<int>(kMinimumLogicalWidth), static_cast<int>(currentDpi_), kDefaultDpi);
            minimumInfo->ptMinTrackSize.y =
                ::MulDiv(static_cast<int>(kMinimumLogicalHeight), static_cast<int>(currentDpi_), kDefaultDpi);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0U) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            ::DestroyWindow(windowHandle);
            return 0;

        case WM_DESTROY:
            quitRequested_ = true;
            ::PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            ::SetWindowLongPtrW(windowHandle, GWLP_USERDATA, 0);
            windowHandle_ = nullptr;
            break;

        default:
            break;
        }

        return ::DefWindowProcW(windowHandle, message, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND windowHandle_ = nullptr;
    HICON largeIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    ATOM classAtom_ = 0;
    std::uint32_t currentDpi_ = kDefaultDpi;
    bool quitRequested_ = false;
    bool minimized_ = false;
    bool applicationActive_ = true;
    std::optional<WindowSize> pendingResize_;
    std::optional<std::uint32_t> pendingDpi_;
    std::optional<WindowDropEvent> pendingDroppedSourceEvent_;
};

Win32Window::Win32Window() : impl_(new Impl()) {}

Win32Window::~Win32Window() {
    Destroy();
    delete std::exchange(impl_, nullptr);
}

bool Win32Window::Create(
    const HINSTANCE instance,
    const std::wstring_view title,
    const std::uint32_t logicalWidth,
    const std::uint32_t logicalHeight) {
    return impl_ != nullptr && impl_->Create(instance, title, logicalWidth, logicalHeight);
}

void Win32Window::Show(const int showCommand) {
    if (impl_ != nullptr) {
        impl_->Show(showCommand);
    }
}

bool Win32Window::PollEvents(WindowEvents& events) {
    return impl_ != nullptr && impl_->PollEvents(events);
}

void Win32Window::RequestClose() {
    if (impl_ != nullptr) {
        impl_->RequestClose();
    }
}

void Win32Window::Destroy() {
    if (impl_ != nullptr) {
        impl_->Destroy();
    }
}

HWND Win32Window::Handle() const noexcept {
    return impl_ != nullptr ? impl_->Handle() : nullptr;
}

std::uint32_t Win32Window::CurrentDpi() const noexcept {
    return impl_ != nullptr ? impl_->CurrentDpi() : kDefaultDpi;
}

}  // namespace zt::sequence
