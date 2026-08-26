#include "Update/detail/UpdateFileStore.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using zt::sequence::updating::ComputeFileSha256;
using zt::sequence::updating::ParseSha256;
using zt::sequence::updating::SemanticVersion;
using zt::sequence::updating::Sha256Digest;
using zt::sequence::updating::Sha256Equals;
using zt::sequence::updating::Sha256ToHex;
using zt::sequence::updating::UpdateErrorCode;
using zt::sequence::updating::UpdateFailure;
using zt::sequence::updating::detail::HttpChunkWriter;
using zt::sequence::updating::detail::HttpTransferResult;
using zt::sequence::updating::detail::UpdateFileStore;

constexpr std::size_t kTransferChunkBytes = 16U * 1024U;
constexpr std::size_t kInterruptedTransferBytes = 4U * 1024U;

[[nodiscard]] bool Expect(
    const bool condition,
    const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] UpdateFailure MakeTestFailure(
    const UpdateErrorCode code,
    const std::wstring_view message,
    const std::uint32_t nativeCode = 0U) {
    UpdateFailure failure;
    failure.code = code;
    failure.nativeCode = nativeCode;
    failure.message.assign(message);
    return failure;
}

[[nodiscard]] bool IsChecksumEnvelopeWhitespace(
    const char character) noexcept {
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n';
}

// Mirrors the ASCII envelope trimming performed by GitHubUpdateClient before
// it delegates the release asset payload to the strict SHA-256 parser.
[[nodiscard]] std::optional<Sha256Digest> ParseChecksumAssetContract(
    std::string_view assetBody) noexcept {
    while (!assetBody.empty() &&
           IsChecksumEnvelopeWhitespace(assetBody.front())) {
        assetBody.remove_prefix(1U);
    }
    while (!assetBody.empty() &&
           IsChecksumEnvelopeWhitespace(assetBody.back())) {
        assetBody.remove_suffix(1U);
    }
    return ParseSha256(assetBody);
}

[[nodiscard]] std::optional<std::filesystem::path>
CurrentExecutablePath() {
    constexpr std::size_t kInitialPathCharacters = 512U;
    constexpr std::size_t kMaximumPathCharacters = 32'768U;
    std::vector<wchar_t> buffer(kInitialPathCharacters);

    while (buffer.size() <= kMaximumPathCharacters) {
        const DWORD capacity = static_cast<DWORD>(buffer.size());
        const DWORD length = ::GetModuleFileNameW(
            nullptr, buffer.data(), capacity);
        if (length == 0U) {
            return std::nullopt;
        }
        if (length < capacity) {
            return std::filesystem::path(std::wstring(
                buffer.data(), static_cast<std::size_t>(length)));
        }
        if (buffer.size() == kMaximumPathCharacters) {
            return std::nullopt;
        }
        const std::size_t nextSize = buffer.size() * 2U;
        buffer.resize(
            nextSize < kMaximumPathCharacters
                ? nextSize
                : kMaximumPathCharacters);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> UpdateRootPath() {
    constexpr DWORD kInitialPathCharacters = 512U;
    std::vector<wchar_t> buffer(kInitialPathCharacters);

    for (;;) {
        const DWORD capacity = static_cast<DWORD>(buffer.size());
        const DWORD length = ::GetTempPathW(capacity, buffer.data());
        if (length == 0U) {
            return std::nullopt;
        }
        if (length < capacity) {
            return std::filesystem::path(std::wstring(
                       buffer.data(), static_cast<std::size_t>(length))) /
                L"FramePlayer-Update";
        }
        if (length > 32'768U) {
            return std::nullopt;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1U);
    }
}

[[nodiscard]] SemanticVersion MakeTestVersion(
    const std::uint16_t ordinal) noexcept {
    LARGE_INTEGER performanceCounter{};
    static_cast<void>(::QueryPerformanceCounter(&performanceCounter));
    const std::uint64_t counter =
        static_cast<std::uint64_t>(performanceCounter.QuadPart);
    const std::uint64_t entropy =
        counter ^ static_cast<std::uint64_t>(::GetTickCount64());
    constexpr std::uint16_t kTestMajor =
        std::numeric_limits<std::uint16_t>::max();
    const std::uint16_t processComponent = static_cast<std::uint16_t>(
        ::GetCurrentProcessId() & 0xFFFFU);
    const std::uint16_t counterComponent = static_cast<std::uint16_t>(
        entropy & 0xFFFFULL);
    const std::uint16_t buildBase = static_cast<std::uint16_t>(
        (entropy >> 16U) & 0xFFF0ULL);
    const std::uint16_t buildComponent = static_cast<std::uint16_t>(
        buildBase | (ordinal & 0x000FU));
    return SemanticVersion(
        kTestMajor,
        processComponent,
        counterComponent,
        buildComponent);
}

class OwnedVersionDirectory final {
public:
    OwnedVersionDirectory(
        std::filesystem::path updateRoot,
        const SemanticVersion& version)
        : updateRoot_(std::move(updateRoot)),
          versionDirectory_(updateRoot_ / version.ToWString()),
          finalPath_(versionDirectory_ / L"FramePlayer.exe"),
          partialPath_(finalPath_.wstring() + L".part") {}

    ~OwnedVersionDirectory() { static_cast<void>(Cleanup()); }

    OwnedVersionDirectory(const OwnedVersionDirectory&) = delete;
    OwnedVersionDirectory& operator=(const OwnedVersionDirectory&) = delete;
    OwnedVersionDirectory(OwnedVersionDirectory&&) = delete;
    OwnedVersionDirectory& operator=(OwnedVersionDirectory&&) = delete;

    [[nodiscard]] bool Prepare() { return Cleanup(); }

    [[nodiscard]] bool Cleanup() noexcept {
        std::error_code error;
        static_cast<void>(
            std::filesystem::remove_all(versionDirectory_, error));
        if (error) {
            return false;
        }

        const bool rootExists = std::filesystem::exists(updateRoot_, error);
        if (error || !rootExists) {
            return !error;
        }
        const bool rootIsEmpty =
            std::filesystem::is_empty(updateRoot_, error);
        if (error || !rootIsEmpty) {
            return !error;
        }
        static_cast<void>(std::filesystem::remove(updateRoot_, error));
        return !error;
    }

    [[nodiscard]] bool HasNoTransactionArtifacts() const {
        std::error_code error;
        const bool finalExists = std::filesystem::exists(finalPath_, error);
        if (error) {
            return false;
        }
        const bool partialExists =
            std::filesystem::exists(partialPath_, error);
        if (error) {
            return false;
        }
        const bool versionExists =
            std::filesystem::exists(versionDirectory_, error);
        return !error && !finalExists && !partialExists && !versionExists;
    }

    [[nodiscard]] bool HasNoEmptyUpdateRoot() const {
        std::error_code error;
        const bool rootExists = std::filesystem::exists(updateRoot_, error);
        if (error || !rootExists) {
            return !error;
        }
        const bool rootIsEmpty =
            std::filesystem::is_empty(updateRoot_, error);
        return !error && !rootIsEmpty;
    }

    [[nodiscard]] const std::filesystem::path& FinalPath() const noexcept {
        return finalPath_;
    }

    [[nodiscard]] const std::filesystem::path& PartialPath() const noexcept {
        return partialPath_;
    }

private:
    std::filesystem::path updateRoot_;
    std::filesystem::path versionDirectory_;
    std::filesystem::path finalPath_;
    std::filesystem::path partialPath_;
};

struct ExecutableFixture final {
    std::filesystem::path path;
    Sha256Digest checksum{};
    std::uint64_t bytes{};
};

[[nodiscard]] std::optional<ExecutableFixture> LoadExecutableFixture() {
    const std::optional<std::filesystem::path> executablePath =
        CurrentExecutablePath();
    if (!executablePath.has_value()) {
        return std::nullopt;
    }

    std::error_code fileSizeError;
    const std::uintmax_t fileBytes =
        std::filesystem::file_size(*executablePath, fileSizeError);
    static_assert(
        std::numeric_limits<std::uintmax_t>::max() <=
        std::numeric_limits<std::uint64_t>::max());
    if (fileSizeError || fileBytes == 0U) {
        return std::nullopt;
    }

    DWORD hashError = ERROR_SUCCESS;
    const std::optional<Sha256Digest> checksum =
        ComputeFileSha256(*executablePath, &hashError);
    if (!checksum.has_value() || hashError != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return ExecutableFixture{
        *executablePath,
        *checksum,
        static_cast<std::uint64_t>(fileBytes)};
}

[[nodiscard]] bool TestChecksumReleaseAssetContract(
    const ExecutableFixture& executable) {
    const std::string digestText = Sha256ToHex(executable.checksum);
    const std::string plainAssetWithCrLf = digestText + "\r\n";
    const std::optional<Sha256Digest> parsedPlainAsset =
        ParseChecksumAssetContract(plainAssetWithCrLf);

    bool passed = Expect(
        parsedPlainAsset.has_value() &&
            Sha256Equals(*parsedPlainAsset, executable.checksum),
        "accept a pure 64-hex checksum release asset with CRLF");

    const std::string checksumToolAsset =
        digestText + "  FramePlayer.exe\r\n";
    passed &= Expect(
        !ParseChecksumAssetContract(checksumToolAsset).has_value(),
        "reject a checksum release asset that includes a file name");
    return passed;
}

[[nodiscard]] HttpTransferResult StreamExecutable(
    const ExecutableFixture& executable,
    const HttpChunkWriter& writer) {
    HttpTransferResult result;
    result.contentLength = executable.bytes;

    std::ifstream input(executable.path, std::ios::binary);
    if (!input) {
        result.failure = MakeTestFailure(
            UpdateErrorCode::FileSystem,
            L"The test could not open its own executable.");
        return result;
    }

    std::array<std::byte, kTransferChunkBytes> buffer{};
    while (input) {
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize readBytes = input.gcount();
        if (readBytes <= 0) {
            break;
        }

        UpdateFailure writeFailure;
        const std::size_t chunkBytes =
            static_cast<std::size_t>(readBytes);
        if (!writer(
                std::span<const std::byte>(buffer.data(), chunkBytes),
                &writeFailure)) {
            result.failure = writeFailure.HasError()
                ? std::move(writeFailure)
                : MakeTestFailure(
                      UpdateErrorCode::FileSystem,
                      L"The update writer rejected a test chunk.");
            return result;
        }
        result.bytesTransferred +=
            static_cast<std::uint64_t>(chunkBytes);
    }

    if (input.bad() || result.bytesTransferred != executable.bytes) {
        result.failure = MakeTestFailure(
            UpdateErrorCode::FileSystem,
            L"The test could not read its own executable completely.");
    }
    return result;
}

[[nodiscard]] HttpTransferResult WriteOneChunkThenFail(
    const ExecutableFixture& executable,
    const HttpChunkWriter& writer,
    bool* const chunkWasWritten) {
    HttpTransferResult result;
    result.contentLength = executable.bytes;

    std::ifstream input(executable.path, std::ios::binary);
    if (!input) {
        result.failure = MakeTestFailure(
            UpdateErrorCode::FileSystem,
            L"The test could not open its own executable.");
        return result;
    }

    std::array<std::byte, kInterruptedTransferBytes> chunk{};
    input.read(
        reinterpret_cast<char*>(chunk.data()),
        static_cast<std::streamsize>(chunk.size()));
    const std::streamsize readBytes = input.gcount();
    if (readBytes <= 0) {
        result.failure = MakeTestFailure(
            UpdateErrorCode::FileSystem,
            L"The test executable did not contain a transfer chunk.");
        return result;
    }

    UpdateFailure writeFailure;
    const std::size_t chunkBytes = static_cast<std::size_t>(readBytes);
    if (!writer(
            std::span<const std::byte>(chunk.data(), chunkBytes),
            &writeFailure)) {
        result.failure = writeFailure.HasError()
            ? std::move(writeFailure)
            : MakeTestFailure(
                  UpdateErrorCode::FileSystem,
                  L"The update writer rejected the interrupted test chunk.");
        return result;
    }

    if (chunkWasWritten != nullptr) {
        *chunkWasWritten = true;
    }
    result.bytesTransferred = static_cast<std::uint64_t>(chunkBytes);
    result.failure = MakeTestFailure(
        UpdateErrorCode::Network,
        L"Synthetic transfer interruption after a successful write.",
        ERROR_CONNECTION_ABORTED);
    return result;
}

[[nodiscard]] bool TestPublishAndReuse(
    const ExecutableFixture& executable,
    const std::filesystem::path& updateRoot) {
    const SemanticVersion version =
        MakeTestVersion(std::uint16_t{1U});
    OwnedVersionDirectory ownedDirectory(updateRoot, version);
    bool passed = Expect(
        ownedDirectory.Prepare(),
        "prepare publish-and-reuse transaction directory");
    if (!passed) {
        return false;
    }

    UpdateFileStore store(executable.bytes);
    std::uint32_t transferCalls = 0U;
    const auto transfer = [&](const HttpChunkWriter& writer) {
        ++transferCalls;
        return StreamExecutable(executable, writer);
    };
    const auto first = store.Acquire(
        version, executable.checksum, transfer);
    passed &= Expect(first.Succeeded(), "publish a valid update executable");
    passed &= Expect(
        !first.reusedCachedFile,
        "first valid update acquisition is not reported as reused");
    passed &= Expect(
        first.downloadedBytes == executable.bytes,
        "published update reports the complete byte count");
    passed &= Expect(
        first.filePath.lexically_normal() ==
            ownedDirectory.FinalPath().lexically_normal(),
        "publish the update to the versioned temporary destination");
    passed &= Expect(
        std::filesystem::is_regular_file(ownedDirectory.FinalPath()),
        "published update final file exists");
    passed &= Expect(
        !std::filesystem::exists(ownedDirectory.PartialPath()),
        "published update leaves no partial file");

    DWORD publishedHashError = ERROR_SUCCESS;
    const std::optional<Sha256Digest> publishedChecksum =
        ComputeFileSha256(ownedDirectory.FinalPath(), &publishedHashError);
    passed &= Expect(
        publishedChecksum.has_value() &&
            publishedHashError == ERROR_SUCCESS &&
            Sha256Equals(*publishedChecksum, executable.checksum),
        "published update preserves the expected SHA-256 checksum");

    const auto shouldNotTransfer = [&](const HttpChunkWriter&) {
        ++transferCalls;
        HttpTransferResult unexpected;
        unexpected.failure = MakeTestFailure(
            UpdateErrorCode::Unexpected,
            L"Cached update unexpectedly invoked the transfer operation.");
        return unexpected;
    };
    const auto second = store.Acquire(
        version, executable.checksum, shouldNotTransfer);
    passed &= Expect(second.Succeeded(), "reuse a validated cached update");
    passed &= Expect(
        second.reusedCachedFile,
        "second valid update acquisition is reported as reused");
    passed &= Expect(
        second.downloadedBytes == executable.bytes,
        "reused update reports the cached file byte count");
    passed &= Expect(
        second.filePath.lexically_normal() ==
            ownedDirectory.FinalPath().lexically_normal(),
        "reused update returns the versioned temporary destination");
    passed &= Expect(
        transferCalls == 1U,
        "cached update reuse performs no second transfer");

    passed &= Expect(
        ownedDirectory.Cleanup(),
        "clean publish-and-reuse test version directory");
    passed &= Expect(
        ownedDirectory.HasNoTransactionArtifacts(),
        "publish-and-reuse cleanup leaves no owned artifact");
    passed &= Expect(
        ownedDirectory.HasNoEmptyUpdateRoot(),
        "publish-and-reuse cleanup leaves no empty update root");
    return passed;
}

[[nodiscard]] bool TestInvalidChecksumCleanup(
    const ExecutableFixture& executable,
    const std::filesystem::path& updateRoot) {
    const SemanticVersion version =
        MakeTestVersion(std::uint16_t{2U});
    OwnedVersionDirectory ownedDirectory(updateRoot, version);
    bool passed = Expect(
        ownedDirectory.Prepare(),
        "prepare invalid-checksum transaction directory");
    if (!passed) {
        return false;
    }

    Sha256Digest incorrectChecksum = executable.checksum;
    incorrectChecksum.front() = static_cast<std::uint8_t>(
        static_cast<unsigned int>(incorrectChecksum.front()) ^ 0x01U);
    UpdateFileStore store(executable.bytes);
    const auto result = store.Acquire(
        version,
        incorrectChecksum,
        [&](const HttpChunkWriter& writer) {
            return StreamExecutable(executable, writer);
        });

    passed &= Expect(
        !result.Succeeded(),
        "reject an update with an incorrect SHA-256 checksum");
    passed &= Expect(
        result.failure.code == UpdateErrorCode::InvalidChecksum,
        "incorrect SHA-256 reports InvalidChecksum");
    passed &= Expect(
        result.filePath.empty(),
        "incorrect SHA-256 does not publish a final path");
    passed &= Expect(
        ownedDirectory.HasNoTransactionArtifacts(),
        "incorrect SHA-256 leaves neither partial nor final files");
    passed &= Expect(
        ownedDirectory.HasNoEmptyUpdateRoot(),
        "incorrect SHA-256 leaves no empty update root");

    passed &= Expect(
        ownedDirectory.Cleanup(),
        "clean invalid-checksum test version directory");
    return passed;
}

[[nodiscard]] bool TestInterruptedTransferCleanup(
    const ExecutableFixture& executable,
    const std::filesystem::path& updateRoot) {
    const SemanticVersion version =
        MakeTestVersion(std::uint16_t{3U});
    OwnedVersionDirectory ownedDirectory(updateRoot, version);
    bool passed = Expect(
        ownedDirectory.Prepare(),
        "prepare interrupted-transfer transaction directory");
    if (!passed) {
        return false;
    }
    passed &= Expect(
        executable.bytes > kInterruptedTransferBytes,
        "test executable is large enough for a mid-transfer failure");

    bool chunkWasWritten = false;
    UpdateFileStore store(executable.bytes);
    const auto result = store.Acquire(
        version,
        executable.checksum,
        [&](const HttpChunkWriter& writer) {
            return WriteOneChunkThenFail(
                executable, writer, &chunkWasWritten);
        });

    passed &= Expect(
        chunkWasWritten,
        "interrupted transfer writes a partial chunk before failing");
    passed &= Expect(
        !result.Succeeded(),
        "propagate a transfer failure after a partial write");
    passed &= Expect(
        result.failure.code == UpdateErrorCode::Network &&
            result.failure.nativeCode == ERROR_CONNECTION_ABORTED,
        "preserve the interrupted transfer failure");
    passed &= Expect(
        ownedDirectory.HasNoTransactionArtifacts(),
        "interrupted transfer leaves neither partial nor final files");
    passed &= Expect(
        ownedDirectory.HasNoEmptyUpdateRoot(),
        "interrupted transfer leaves no empty update root");

    passed &= Expect(
        ownedDirectory.Cleanup(),
        "clean interrupted-transfer test version directory");
    return passed;
}

}  // namespace

int main() {
    const std::optional<ExecutableFixture> executable =
        LoadExecutableFixture();
    if (!Expect(
            executable.has_value(),
            "load the current test executable and its SHA-256 checksum")) {
        return 1;
    }

    const std::optional<std::filesystem::path> updateRoot = UpdateRootPath();
    if (!Expect(
            updateRoot.has_value(),
            "resolve the current user's temporary update root")) {
        return 1;
    }

    bool passed = true;
    passed &= TestChecksumReleaseAssetContract(*executable);
    passed &= TestPublishAndReuse(*executable, *updateRoot);
    passed &= TestInvalidChecksumCleanup(*executable, *updateRoot);
    passed &= TestInterruptedTransferCleanup(*executable, *updateRoot);
    return passed ? 0 : 1;
}
