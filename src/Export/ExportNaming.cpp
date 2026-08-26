#include "Export/ExportNaming.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace zt::sequence::exporting {
namespace {

constexpr std::size_t kMinimumCollisionDigits = 2U;
constexpr wchar_t kFallbackDirectoryName[] = L"Export";
constexpr wchar_t kMp4Extension[] = L".mp4";

[[nodiscard]] std::filesystem::path DirectoryLeaf(
    const std::filesystem::path& exportDirectory) {
    if (exportDirectory.empty()) {
        throw std::invalid_argument("export directory is empty");
    }

    std::filesystem::path current = exportDirectory.lexically_normal();
    std::filesystem::path leaf = current.filename();
    while (leaf.empty()) {
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
        leaf = current.filename();
    }

    if (leaf.empty() || leaf == L"." || leaf == L"..") {
        return std::filesystem::path(kFallbackDirectoryName);
    }
    return leaf;
}

[[nodiscard]] std::wstring CollisionSuffix(
    const std::uint64_t collisionIndex) {
    if (collisionIndex == 0U) {
        return {};
    }

    std::wstring suffix = std::to_wstring(collisionIndex);
    if (suffix.size() < kMinimumCollisionDigits) {
        suffix.insert(
            suffix.begin(),
            kMinimumCollisionDigits - suffix.size(),
            L'0');
    }
    return suffix;
}

}  // namespace

std::filesystem::path BuildMp4ExportCandidate(
    const std::filesystem::path& exportDirectory,
    const std::uint64_t collisionIndex) {
    const std::filesystem::path leaf = DirectoryLeaf(exportDirectory);
    std::wstring fileName = leaf.wstring();
    fileName += CollisionSuffix(collisionIndex);
    fileName += kMp4Extension;
    return exportDirectory / std::filesystem::path(std::move(fileName));
}

std::filesystem::path FindAvailableMp4ExportPath(
    const std::filesystem::path& exportDirectory,
    const ExportPathExists& pathExists) {
    if (!pathExists) {
        throw std::invalid_argument("path existence predicate is empty");
    }

    for (std::uint64_t collisionIndex = 0U;; ++collisionIndex) {
        const std::filesystem::path candidate = BuildMp4ExportCandidate(
            exportDirectory,
            collisionIndex);
        if (!pathExists(candidate)) {
            return candidate;
        }
        if (collisionIndex == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("MP4 export name sequence exhausted");
        }
    }
}

std::filesystem::path FindAvailableMp4ExportPath(
    const std::filesystem::path& exportDirectory) {
    return FindAvailableMp4ExportPath(
        exportDirectory,
        [](const std::filesystem::path& candidate) {
            std::error_code error;
            const bool exists = std::filesystem::exists(candidate, error);
            if (error) {
                throw std::filesystem::filesystem_error(
                    "cannot inspect MP4 export candidate",
                    candidate,
                    error);
            }
            return exists;
        });
}

}  // namespace zt::sequence::exporting
