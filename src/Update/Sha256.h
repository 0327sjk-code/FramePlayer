#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zt::sequence::updating {

inline constexpr std::size_t kSha256DigestSize = 32U;
using Sha256Digest = std::array<std::uint8_t, kSha256DigestSize>;

// Parses exactly 64 hexadecimal characters. Whitespace and prefixes such as
// "0x" are intentionally rejected so a release manifest is unambiguous.
[[nodiscard]] std::optional<Sha256Digest> ParseSha256(
    std::string_view text) noexcept;

// Streams the file through Windows CNG rather than loading it into memory.
// On failure, returns std::nullopt and stores either a Win32 error code or the
// raw CNG NTSTATUS value in error when that pointer is non-null.
[[nodiscard]] std::optional<Sha256Digest> ComputeFileSha256(
    const std::filesystem::path& path,
    DWORD* error) noexcept;

[[nodiscard]] bool Sha256Equals(
    const Sha256Digest& first,
    const Sha256Digest& second) noexcept;

// Produces the canonical lowercase 64-character representation.
[[nodiscard]] std::string Sha256ToHex(const Sha256Digest& digest);

}  // namespace zt::sequence::updating
