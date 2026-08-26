#pragma once

#include <filesystem>
#include <string>

namespace zt::sequence::platform {

struct FileLaunchResult final {
    bool succeeded = false;
    std::string errorUtf8;
};

// Opens a regular file through the user's Windows default file association.
// The implementation suppresses ShellExecute error UI so callers can report
// failures through their own non-modal interface.
[[nodiscard]] FileLaunchResult OpenFileWithDefaultApplication(
    const std::filesystem::path& filePath);

}  // namespace zt::sequence::platform
