#pragma once

#include "Platform/DroppedSource.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

struct HWND__;
using HWND = HWND__*;
struct HINSTANCE__;
using HINSTANCE = HINSTANCE__*;

namespace zt::sequence {

struct WindowSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct WindowClientPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct WindowDropEvent {
    DroppedSource source;
    WindowClientPoint clientPoint;
};

struct WindowEvents {
    bool quitRequested = false;
    bool minimized = false;
    bool applicationActive = true;
    std::optional<WindowSize> resized;
    std::optional<std::uint32_t> dpiChanged;
    std::optional<WindowDropEvent> droppedSourceEvent;
};

class Win32Window final {
public:
    Win32Window();
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    [[nodiscard]] bool Create(
        HINSTANCE instance,
        std::wstring_view title,
        std::uint32_t logicalWidth,
        std::uint32_t logicalHeight);
    void Show(int showCommand);
    [[nodiscard]] bool PollEvents(WindowEvents& events);
    void RequestClose();
    void Destroy();

    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] std::uint32_t CurrentDpi() const noexcept;

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace zt::sequence
