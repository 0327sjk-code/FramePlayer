#include "Platform/FolderDialog.h"

#include <shobjidl.h>
#include <windows.h>
#include <wrl/client.h>

namespace zt::sequence {
namespace {

[[nodiscard]] std::optional<std::filesystem::path> DialogResultPath(
    IFileOpenDialog& dialog) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IShellItem> item;
    HRESULT result = dialog.GetResult(item.GetAddressOf());
    if (FAILED(result)) {
        return std::nullopt;
    }

    PWSTR fileSystemPath = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
    if (FAILED(result) || fileSystemPath == nullptr) {
        return std::nullopt;
    }

    const std::filesystem::path selectedPath(fileSystemPath);
    ::CoTaskMemFree(fileSystemPath);
    return selectedPath;
}

[[nodiscard]] std::optional<std::filesystem::path> ShowFolderPickerWithTitle(
    const HWND owner,
    const wchar_t* title) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result)) {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options{};
    result = dialog->GetOptions(&options);
    if (FAILED(result)) {
        return std::nullopt;
    }

    result = dialog->SetOptions(
        options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (FAILED(result)) {
        return std::nullopt;
    }
    static_cast<void>(dialog->SetTitle(title));

    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(result)) {
        return std::nullopt;
    }

    return DialogResultPath(*dialog.Get());
}

}  // namespace

std::optional<std::filesystem::path> ShowFolderPicker(const HWND owner) {
    return ShowFolderPickerWithTitle(owner, L"选择 PNG 序列文件夹");
}

std::optional<std::filesystem::path> ShowExportFolderPicker(const HWND owner) {
    return ShowFolderPickerWithTitle(owner, L"选择 MP4 导出文件夹");
}

std::optional<std::filesystem::path> ShowPngImagePicker(const HWND owner) {
    using Microsoft::WRL::ComPtr;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = ::CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result)) {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options{};
    result = dialog->GetOptions(&options);
    if (FAILED(result)) {
        return std::nullopt;
    }
    result = dialog->SetOptions(
        options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (FAILED(result)) {
        return std::nullopt;
    }

    constexpr COMDLG_FILTERSPEC filter{
        L"PNG 图片 (*.png)",
        L"*.png"};
    if (FAILED(dialog->SetFileTypes(1U, &filter))) {
        return std::nullopt;
    }
    static_cast<void>(dialog->SetFileTypeIndex(1U));
    static_cast<void>(dialog->SetDefaultExtension(L"png"));
    static_cast<void>(dialog->SetTitle(L"选择 1080 × 1920 PNG 蒙版"));

    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(result)) {
        return std::nullopt;
    }
    return DialogResultPath(*dialog.Get());
}

}  // namespace zt::sequence
