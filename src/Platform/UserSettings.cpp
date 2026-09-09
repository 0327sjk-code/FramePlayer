#include "Platform/UserSettings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <limits>
#include <string>
#include <vector>

namespace zt::sequence::user_settings {
namespace {

constexpr wchar_t kRegistrySubKey[] = L"Software\\ZTSequencePlayer";
constexpr wchar_t kExportFolderValue[] = L"ExportFolder";
constexpr wchar_t kLastSequenceFolderValue[] = L"LastSequenceFolder";
constexpr wchar_t kKeyboardShuttleSpeedPercentValue[] =
    L"KeyboardShuttleSpeedPercent";
constexpr wchar_t kMaskOverlayImagePathValue[] = L"MaskOverlayImagePath";

class RegistryKey final {
public:
    RegistryKey() = default;

    ~RegistryKey() {
        if (value_ != nullptr) {
            static_cast<void>(::RegCloseKey(value_));
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY* Address() noexcept {
        return &value_;
    }

    [[nodiscard]] HKEY Get() const noexcept {
        return value_;
    }

private:
    HKEY value_ = nullptr;
};

[[nodiscard]] bool CreateWritableSettingsKey(RegistryKey& key) noexcept {
    const LSTATUS createStatus = ::RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRegistrySubKey,
        0U,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        key.Address(),
        nullptr);
    return createStatus == ERROR_SUCCESS && key.Get() != nullptr;
}

[[nodiscard]] std::optional<std::filesystem::path> LoadPathValue(
    const wchar_t* const valueName) noexcept {
    try {
        if (valueName == nullptr || *valueName == L'\0') {
            return std::nullopt;
        }

        DWORD byteCount = 0U;
        const DWORD flags = RRF_RT_REG_SZ | RRF_ZEROONFAILURE;
        const LSTATUS sizeStatus = ::RegGetValueW(
            HKEY_CURRENT_USER,
            kRegistrySubKey,
            valueName,
            flags,
            nullptr,
            nullptr,
            &byteCount);
        if (sizeStatus != ERROR_SUCCESS ||
            byteCount < sizeof(wchar_t) ||
            (byteCount % sizeof(wchar_t)) != 0U) {
            return std::nullopt;
        }

        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(byteCount / sizeof(wchar_t)),
            L'\0');
        DWORD valueType = 0U;
        const LSTATUS readStatus = ::RegGetValueW(
            HKEY_CURRENT_USER,
            kRegistrySubKey,
            valueName,
            flags,
            &valueType,
            buffer.data(),
            &byteCount);
        if (readStatus != ERROR_SUCCESS || valueType != REG_SZ) {
            return std::nullopt;
        }

        while (!buffer.empty() && buffer.back() == L'\0') {
            buffer.pop_back();
        }
        if (buffer.empty()) {
            return std::nullopt;
        }

        return std::filesystem::path(
            std::wstring(buffer.begin(), buffer.end()));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool SavePathValue(
    const wchar_t* const valueName,
    const std::filesystem::path& path) noexcept {
    try {
        if (valueName == nullptr || *valueName == L'\0' || path.empty()) {
            return false;
        }

        const std::wstring value = path.wstring();
        const std::size_t byteCount =
            (value.size() + 1U) * sizeof(wchar_t);
        if (byteCount > static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max())) {
            return false;
        }

        RegistryKey key;
        if (!CreateWritableSettingsKey(key)) {
            return false;
        }

        const LSTATUS writeStatus = ::RegSetValueExW(
            key.Get(),
            valueName,
            0U,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>(byteCount));
        return writeStatus == ERROR_SUCCESS;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::optional<DWORD> LoadDwordValue(
    const wchar_t* const valueName) noexcept {
    if (valueName == nullptr || *valueName == L'\0') {
        return std::nullopt;
    }

    DWORD value = 0U;
    DWORD valueType = 0U;
    DWORD byteCount = static_cast<DWORD>(sizeof(value));
    const DWORD flags = RRF_RT_REG_DWORD | RRF_ZEROONFAILURE;
    const LSTATUS readStatus = ::RegGetValueW(
        HKEY_CURRENT_USER,
        kRegistrySubKey,
        valueName,
        flags,
        &valueType,
        &value,
        &byteCount);
    if (readStatus != ERROR_SUCCESS || valueType != REG_DWORD ||
        byteCount != static_cast<DWORD>(sizeof(value))) {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] bool SaveDwordValue(
    const wchar_t* const valueName,
    const DWORD value) noexcept {
    if (valueName == nullptr || *valueName == L'\0') {
        return false;
    }

    RegistryKey key;
    if (!CreateWritableSettingsKey(key)) {
        return false;
    }

    const LSTATUS writeStatus = ::RegSetValueExW(
        key.Get(),
        valueName,
        0U,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        static_cast<DWORD>(sizeof(value)));
    return writeStatus == ERROR_SUCCESS;
}

}  // namespace

std::optional<std::filesystem::path> LoadExportFolder() noexcept {
    return LoadPathValue(kExportFolderValue);
}

bool SaveExportFolder(const std::filesystem::path& exportFolder) noexcept {
    return SavePathValue(kExportFolderValue, exportFolder);
}

std::optional<std::filesystem::path> LoadLastSequenceFolder() noexcept {
    return LoadPathValue(kLastSequenceFolderValue);
}

bool SaveLastSequenceFolder(
    const std::filesystem::path& sequenceFolder) noexcept {
    return SavePathValue(kLastSequenceFolderValue, sequenceFolder);
}

std::optional<int> LoadKeyboardShuttleSpeedPercent() noexcept {
    const std::optional<DWORD> value =
        LoadDwordValue(kKeyboardShuttleSpeedPercentValue);
    if (!value || *value > static_cast<DWORD>(
            std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

bool SaveKeyboardShuttleSpeedPercent(const int percent) noexcept {
    if (percent < 0) {
        return false;
    }
    return SaveDwordValue(
        kKeyboardShuttleSpeedPercentValue,
        static_cast<DWORD>(percent));
}

std::optional<std::filesystem::path> LoadMaskOverlayImagePath() noexcept {
    return LoadPathValue(kMaskOverlayImagePathValue);
}

bool SaveMaskOverlayImagePath(
    const std::filesystem::path& imagePath) noexcept {
    return SavePathValue(kMaskOverlayImagePathValue, imagePath);
}

}  // namespace zt::sequence::user_settings
