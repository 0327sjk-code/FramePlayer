#pragma once

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace zt::sequence {

enum class DroppedSourceKind {
    PngSequence,
    Video,
};

struct DroppedSource {
    std::filesystem::path path;
    DroppedSourceKind kind = DroppedSourceKind::PngSequence;
};

namespace dropped_source {

enum class EntryKind {
    Directory,
    RegularFile,
};

[[nodiscard]] inline std::wstring LowercaseExtension(
    const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return extension;
}

[[nodiscard]] inline bool IsSupportedVideoExtension(
    const std::wstring_view extension) noexcept {
    constexpr std::array<std::wstring_view, 7U> kVideoExtensions{
        L".mp4",
        L".mov",
        L".m4v",
        L".wmv",
        L".avi",
        L".mkv",
        L".webm",
    };
    return std::find(kVideoExtensions.begin(), kVideoExtensions.end(), extension) !=
        kVideoExtensions.end();
}

[[nodiscard]] inline std::optional<DroppedSource> ClassifyExistingPath(
    const std::filesystem::path& path,
    const EntryKind entryKind) {
    if (path.empty()) {
        return std::nullopt;
    }

    if (entryKind == EntryKind::Directory) {
        return DroppedSource{path, DroppedSourceKind::PngSequence};
    }

    const std::wstring extension = LowercaseExtension(path);
    if (extension == L".png") {
        const std::filesystem::path sequenceDirectory = path.parent_path();
        if (sequenceDirectory.empty()) {
            return std::nullopt;
        }
        return DroppedSource{sequenceDirectory, DroppedSourceKind::PngSequence};
    }
    if (IsSupportedVideoExtension(extension)) {
        return DroppedSource{path, DroppedSourceKind::Video};
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<DroppedSource> ClassifyExistingPath(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error) {
        return std::nullopt;
    }
    if (std::filesystem::is_directory(status)) {
        return ClassifyExistingPath(path, EntryKind::Directory);
    }
    if (std::filesystem::is_regular_file(status)) {
        return ClassifyExistingPath(path, EntryKind::RegularFile);
    }
    return std::nullopt;
}

}  // namespace dropped_source
}  // namespace zt::sequence
