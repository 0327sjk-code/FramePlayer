#include "Update/Sha256.h"

#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <vector>

namespace zt::sequence::updating {
namespace {

class FileHandle final {
public:
    explicit FileHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~FileHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle_));
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) = delete;
    FileHandle& operator=(FileHandle&&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class AlgorithmHandle final {
public:
    AlgorithmHandle() noexcept = default;

    ~AlgorithmHandle() {
        if (handle_ != nullptr) {
            static_cast<void>(::BCryptCloseAlgorithmProvider(handle_, 0U));
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    AlgorithmHandle(AlgorithmHandle&&) = delete;
    AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE* Put() noexcept { return &handle_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle final {
public:
    HashHandle() noexcept = default;

    ~HashHandle() {
        if (handle_ != nullptr) {
            static_cast<void>(::BCryptDestroyHash(handle_));
        }
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    HashHandle(HashHandle&&) = delete;
    HashHandle& operator=(HashHandle&&) = delete;

    [[nodiscard]] BCRYPT_HASH_HANDLE* Put() noexcept { return &handle_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE Get() const noexcept { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_{};
};

void StoreError(DWORD* const error, const DWORD value) noexcept {
    if (error != nullptr) {
        *error = value;
    }
}

[[nodiscard]] DWORD CngError(const NTSTATUS status) noexcept {
    return static_cast<DWORD>(static_cast<ULONG>(status));
}

[[nodiscard]] std::optional<std::uint8_t> HexValue(
    const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] bool ReadProperty(
    const BCRYPT_ALG_HANDLE algorithm,
    const wchar_t* const name,
    ULONG* const value,
    DWORD* const error) noexcept {
    ULONG bytesWritten = 0U;
    const NTSTATUS status = ::BCryptGetProperty(
        algorithm,
        name,
        reinterpret_cast<PUCHAR>(value),
        static_cast<ULONG>(sizeof(*value)),
        &bytesWritten,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        StoreError(error, CngError(status));
        return false;
    }
    if (bytesWritten != static_cast<ULONG>(sizeof(*value))) {
        StoreError(error, ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<Sha256Digest> ComputeFileSha256Impl(
    const std::filesystem::path& path,
    DWORD* const error) {
    FileHandle file(::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file) {
        StoreError(error, ::GetLastError());
        return std::nullopt;
    }

    AlgorithmHandle algorithm;
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(
        algorithm.Put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (!BCRYPT_SUCCESS(status)) {
        StoreError(error, CngError(status));
        return std::nullopt;
    }

    ULONG objectLength = 0U;
    ULONG hashLength = 0U;
    if (!ReadProperty(
            algorithm.Get(), BCRYPT_OBJECT_LENGTH, &objectLength, error) ||
        !ReadProperty(
            algorithm.Get(), BCRYPT_HASH_LENGTH, &hashLength, error)) {
        return std::nullopt;
    }
    if (objectLength == 0U ||
        hashLength != static_cast<ULONG>(kSha256DigestSize)) {
        StoreError(error, ERROR_INVALID_DATA);
        return std::nullopt;
    }

    std::vector<UCHAR> hashObject(objectLength);
    HashHandle hash;
    status = ::BCryptCreateHash(
        algorithm.Get(),
        hash.Put(),
        hashObject.data(),
        objectLength,
        nullptr,
        0U,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        StoreError(error, CngError(status));
        return std::nullopt;
    }

    constexpr std::size_t kReadBufferBytes = 64U * 1024U;
    static_assert(kReadBufferBytes <= std::numeric_limits<DWORD>::max());
    std::array<UCHAR, kReadBufferBytes> readBuffer{};
    while (true) {
        DWORD bytesRead = 0U;
        if (!::ReadFile(
                file.Get(),
                readBuffer.data(),
                static_cast<DWORD>(readBuffer.size()),
                &bytesRead,
                nullptr)) {
            StoreError(error, ::GetLastError());
            return std::nullopt;
        }
        if (bytesRead == 0U) {
            break;
        }
        status = ::BCryptHashData(
            hash.Get(), readBuffer.data(), bytesRead, 0U);
        if (!BCRYPT_SUCCESS(status)) {
            StoreError(error, CngError(status));
            return std::nullopt;
        }
    }

    Sha256Digest digest{};
    status = ::BCryptFinishHash(
        hash.Get(),
        digest.data(),
        static_cast<ULONG>(digest.size()),
        0U);
    if (!BCRYPT_SUCCESS(status)) {
        StoreError(error, CngError(status));
        return std::nullopt;
    }

    StoreError(error, ERROR_SUCCESS);
    return digest;
}

}  // namespace

std::optional<Sha256Digest> ParseSha256(const std::string_view text) noexcept {
    if (text.size() != kSha256DigestSize * 2U) {
        return std::nullopt;
    }

    Sha256Digest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const std::optional<std::uint8_t> high = HexValue(text[index * 2U]);
        const std::optional<std::uint8_t> low =
            HexValue(text[index * 2U + 1U]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        digest[index] = static_cast<std::uint8_t>(
            static_cast<unsigned int>(*high) * 16U +
            static_cast<unsigned int>(*low));
    }
    return digest;
}

std::optional<Sha256Digest> ComputeFileSha256(
    const std::filesystem::path& path,
    DWORD* const error) noexcept {
    StoreError(error, ERROR_SUCCESS);
    try {
        return ComputeFileSha256Impl(path, error);
    } catch (const std::bad_alloc&) {
        StoreError(error, ERROR_NOT_ENOUGH_MEMORY);
        return std::nullopt;
    } catch (...) {
        StoreError(error, ERROR_GEN_FAILURE);
        return std::nullopt;
    }
}

bool Sha256Equals(
    const Sha256Digest& first,
    const Sha256Digest& second) noexcept {
    unsigned int difference = 0U;
    for (std::size_t index = 0U; index < first.size(); ++index) {
        difference |= static_cast<unsigned int>(first[index]) ^
            static_cast<unsigned int>(second[index]);
    }
    return difference == 0U;
}

std::string Sha256ToHex(const Sha256Digest& digest) {
    constexpr std::array<char, 16U> kHexCharacters{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const unsigned int value = digest[index];
        result[index * 2U] = kHexCharacters[value >> 4U];
        result[index * 2U + 1U] = kHexCharacters[value & 0x0FU];
    }
    return result;
}

}  // namespace zt::sequence::updating
