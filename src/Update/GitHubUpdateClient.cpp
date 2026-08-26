#include "Update/GitHubUpdateClient.h"

#include "Update/Sha256.h"
#include "Update/detail/UpdateError.h"
#include "Update/detail/UpdateFileStore.h"
#include "Update/detail/WinHttpTransport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zt::sequence::updating {
namespace {

bool IsAsciiEnvelopeWhitespace(const char character) noexcept {
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n';
}

void NotifyCachedProgress(
    const DownloadProgressCallback& callback,
    const std::uint64_t bytes) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(bytes, bytes);
    } catch (...) {
        // Consumer callback failures do not invalidate an existing cache file.
    }
}

[[nodiscard]] std::wstring BuildReleaseAssetUrl(
    std::wstring baseUrl,
    const SemanticVersion& version,
    const std::wstring_view assetName) {
    if (baseUrl.empty()) {
        return {};
    }
    if (baseUrl.back() != L'/') {
        baseUrl.push_back(L'/');
    }
    baseUrl.push_back(L'v');
    baseUrl.append(version.ToWString());
    baseUrl.push_back(L'/');
    baseUrl.append(assetName);
    return baseUrl;
}

[[nodiscard]] bool ValidateExecutableFileVersion(
    const std::filesystem::path& executable,
    const SemanticVersion& expectedVersion,
    UpdateFailure* const failure) {
    DWORD ignoredHandle = 0U;
    const DWORD infoBytes = ::GetFileVersionInfoSizeW(
        executable.c_str(), &ignoredHandle);
    if (infoBytes == 0U) {
        if (failure != nullptr) {
            *failure = detail::MakeNativeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable has no readable version resource",
                ::GetLastError());
        }
        return false;
    }

    std::vector<std::byte> versionInfo(infoBytes);
    if (::GetFileVersionInfoW(
            executable.c_str(),
            0U,
            infoBytes,
            versionInfo.data()) == FALSE) {
        if (failure != nullptr) {
            *failure = detail::MakeNativeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable version resource could not be read",
                ::GetLastError());
        }
        return false;
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoBytes = 0U;
    if (::VerQueryValueW(
            versionInfo.data(),
            L"\\",
            reinterpret_cast<void**>(&fixedInfo),
            &fixedInfoBytes) == FALSE ||
        fixedInfo == nullptr ||
        fixedInfoBytes < sizeof(VS_FIXEDFILEINFO) ||
        fixedInfo->dwSignature != VS_FFI_SIGNATURE) {
        if (failure != nullptr) {
            *failure = detail::MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable version resource is invalid.");
        }
        return false;
    }

    const SemanticVersion actualVersion(
        HIWORD(fixedInfo->dwFileVersionMS),
        LOWORD(fixedInfo->dwFileVersionMS),
        HIWORD(fixedInfo->dwFileVersionLS),
        LOWORD(fixedInfo->dwFileVersionLS));
    if (actualVersion != expectedVersion) {
        if (failure != nullptr) {
            *failure = detail::MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable FileVersion does not match version.txt.");
        }
        return false;
    }
    return true;
}

}  // namespace

class GitHubUpdateClient::Impl final {
public:
    explicit Impl(GitHubUpdateClientOptions options)
        : options_(std::move(options)),
          transport_(options_),
          fileStore_(options_.maxExecutableBytes) {}

    [[nodiscard]] VersionFetchResult FetchLatestVersion(
        const std::stop_token stopToken) noexcept {
        try {
            std::scoped_lock operationLock(operationMutex_);
            transport_.BeginOperation();
            if (transport_.CancellationRequested(stopToken)) {
                return {std::nullopt, detail::MakeCancelledFailure()};
            }

            std::vector<std::byte> body;
            const std::uint64_t initialCapacity = std::min<std::uint64_t>(
                options_.maxVersionBytes, 4'096);
            body.reserve(static_cast<std::size_t>(initialCapacity));
            const detail::HttpTransferResult transfer = transport_.Get(
                options_.versionUrl,
                options_.maxVersionBytes,
                [&body](
                    const std::span<const std::byte> chunk,
                    UpdateFailure*) {
                    body.insert(body.end(), chunk.begin(), chunk.end());
                    return true;
                },
                {},
                stopToken);
            if (transfer.failure.HasError()) {
                return {std::nullopt, transfer.failure};
            }

            std::string text;
            text.reserve(body.size());
            for (const std::byte value : body) {
                const unsigned int byteValue =
                    std::to_integer<unsigned int>(value);
                if (byteValue == 0U || byteValue > 0x7FU) {
                    return {
                        std::nullopt,
                        detail::MakeFailure(
                            UpdateErrorCode::InvalidVersion,
                            L"version.txt must contain an ASCII numeric version.")};
                }
                text.push_back(static_cast<char>(byteValue));
            }

            const auto first = std::find_if_not(
                text.begin(), text.end(), IsAsciiEnvelopeWhitespace);
            const auto last = std::find_if_not(
                text.rbegin(), text.rend(), IsAsciiEnvelopeWhitespace).base();
            if (first >= last) {
                return {
                    std::nullopt,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidVersion,
                        L"version.txt is empty.")};
            }
            const std::string_view trimmed(
                &*first,
                static_cast<std::size_t>(last - first));
            const std::optional<SemanticVersion> version =
                SemanticVersion::Parse(trimmed);
            if (!version.has_value()) {
                return {
                    std::nullopt,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidVersion,
                        L"version.txt must contain exactly three or four numeric components.")};
            }
            return {version, {}};
        } catch (const std::bad_alloc&) {
            return {
                std::nullopt,
                detail::MakeFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to check for updates.")};
        } catch (...) {
            return {
                std::nullopt,
                detail::MakeFailure(
                    UpdateErrorCode::Unexpected,
                    L"Unexpected failure while checking for updates.")};
        }
    }

    [[nodiscard]] ExecutableDownloadResult DownloadExecutable(
        const SemanticVersion& version,
        const DownloadProgressCallback& progress,
        const std::stop_token stopToken) noexcept {
        try {
            std::scoped_lock operationLock(operationMutex_);
            transport_.BeginOperation();
            if (transport_.CancellationRequested(stopToken)) {
                return {{}, 0, false, detail::MakeCancelledFailure()};
            }

            std::vector<std::byte> checksumBody;
            const std::wstring checksumUrl = BuildReleaseAssetUrl(
                options_.releaseDownloadBaseUrl,
                version,
                L"FramePlayer.exe.sha256");
            const std::wstring executableUrl = BuildReleaseAssetUrl(
                options_.releaseDownloadBaseUrl,
                version,
                L"FramePlayer.exe");
            if (checksumUrl.empty() || executableUrl.empty()) {
                return {
                    {}, 0, false,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidConfiguration,
                        L"The versioned release download URL is not configured.")};
            }
            const std::uint64_t checksumCapacity =
                std::min<std::uint64_t>(
                    options_.maxChecksumBytes,
                    4'096U);
            checksumBody.reserve(static_cast<std::size_t>(checksumCapacity));
            const detail::HttpTransferResult checksumTransfer =
                transport_.Get(
                    checksumUrl,
                    options_.maxChecksumBytes,
                    [&checksumBody](
                        const std::span<const std::byte> chunk,
                        UpdateFailure*) {
                        checksumBody.insert(
                            checksumBody.end(),
                            chunk.begin(),
                            chunk.end());
                        return true;
                    },
                    {},
                    stopToken);
            if (checksumTransfer.failure.HasError()) {
                return {{}, 0, false, checksumTransfer.failure};
            }

            std::string checksumText;
            checksumText.reserve(checksumBody.size());
            for (const std::byte value : checksumBody) {
                const unsigned int byteValue =
                    std::to_integer<unsigned int>(value);
                if (byteValue == 0U || byteValue > 0x7FU) {
                    return {
                        {}, 0, false,
                        detail::MakeFailure(
                            UpdateErrorCode::InvalidChecksum,
                            L"FramePlayer.exe.sha256 must contain an ASCII SHA-256 digest.")};
                }
                checksumText.push_back(static_cast<char>(byteValue));
            }
            const auto checksumFirst = std::find_if_not(
                checksumText.begin(),
                checksumText.end(),
                IsAsciiEnvelopeWhitespace);
            const auto checksumLast = std::find_if_not(
                checksumText.rbegin(),
                checksumText.rend(),
                IsAsciiEnvelopeWhitespace).base();
            if (checksumFirst >= checksumLast) {
                return {
                    {}, 0, false,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidChecksum,
                        L"FramePlayer.exe.sha256 is empty.")};
            }
            const std::string_view checksumValue(
                &*checksumFirst,
                static_cast<std::size_t>(checksumLast - checksumFirst));
            const std::optional<Sha256Digest> expectedChecksum =
                ParseSha256(checksumValue);
            if (!expectedChecksum.has_value()) {
                return {
                    {}, 0, false,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidChecksum,
                        L"FramePlayer.exe.sha256 must contain exactly 64 hexadecimal characters.")};
            }

            ExecutableDownloadResult result = fileStore_.Acquire(
                version,
                *expectedChecksum,
                [this, &progress, stopToken, &executableUrl](
                    const detail::HttpChunkWriter& writer) {
                    return transport_.Get(
                        executableUrl,
                        options_.maxExecutableBytes,
                        writer,
                        progress,
                        stopToken);
                });
            if (result.Succeeded()) {
                UpdateFailure versionFailure;
                if (!ValidateExecutableFileVersion(
                        result.filePath,
                        version,
                        &versionFailure)) {
                    static_cast<void>(::DeleteFileW(result.filePath.c_str()));
                    static_cast<void>(::RemoveDirectoryW(
                        result.filePath.parent_path().c_str()));
                    result.filePath.clear();
                    result.downloadedBytes = 0U;
                    result.reusedCachedFile = false;
                    result.failure = std::move(versionFailure);
                    return result;
                }
                result.verifiedChecksum = *expectedChecksum;
            }
            if (result.Succeeded() && result.reusedCachedFile) {
                NotifyCachedProgress(progress, result.downloadedBytes);
            }
            return result;
        } catch (const std::bad_alloc&) {
            return {
                {}, 0, false,
                detail::MakeFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to download the update.")};
        } catch (...) {
            return {
                {}, 0, false,
                detail::MakeFailure(
                    UpdateErrorCode::Unexpected,
                    L"Unexpected failure while downloading the update.")};
        }
    }

    void Cancel() noexcept {
        transport_.Cancel();
    }

private:
    GitHubUpdateClientOptions options_;
    detail::WinHttpTransport transport_;
    detail::UpdateFileStore fileStore_;
    std::mutex operationMutex_;
};

GitHubUpdateClient::GitHubUpdateClient(GitHubUpdateClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

GitHubUpdateClient::~GitHubUpdateClient() = default;

VersionFetchResult GitHubUpdateClient::FetchLatestVersion(
    const std::stop_token stopToken) noexcept {
    return impl_->FetchLatestVersion(stopToken);
}

ExecutableDownloadResult GitHubUpdateClient::DownloadExecutable(
    const SemanticVersion& version,
    DownloadProgressCallback progress,
    const std::stop_token stopToken) noexcept {
    return impl_->DownloadExecutable(version, progress, stopToken);
}

void GitHubUpdateClient::Cancel() noexcept {
    impl_->Cancel();
}

}  // namespace zt::sequence::updating
