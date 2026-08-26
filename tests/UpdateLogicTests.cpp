#include "Update/SemanticVersion.h"
#include "Update/Sha256.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using zt::sequence::updating::ComputeFileSha256;
using zt::sequence::updating::ParseSha256;
using zt::sequence::updating::SemanticVersion;
using zt::sequence::updating::Sha256Digest;
using zt::sequence::updating::Sha256Equals;
using zt::sequence::updating::Sha256ToHex;

[[nodiscard]] bool Expect(
    const bool condition,
    const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

class TemporaryFile final {
public:
    TemporaryFile() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            (L"ZTFramePlayerUpdate_" +
             std::to_wstring(::GetCurrentProcessId()) + L"_" +
             std::to_wstring(nonce) + L".bin");
    }

    ~TemporaryFile() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

    [[nodiscard]] bool WriteOneMillionAsciiA() const {
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }

        constexpr std::size_t kFileBytes = 1'000'000U;
        constexpr std::array<char, 16U * 1024U> kChunk = [] {
            std::array<char, 16U * 1024U> chunk{};
            chunk.fill('a');
            return chunk;
        }();
        std::size_t remaining = kFileBytes;
        while (remaining != 0U) {
            const std::size_t writeBytes =
                remaining < kChunk.size() ? remaining : kChunk.size();
            output.write(
                kChunk.data(), static_cast<std::streamsize>(writeBytes));
            if (!output.good()) {
                return false;
            }
            remaining -= writeBytes;
        }
        return true;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool TestSemanticVersionParsingAndFormatting() {
    bool passed = true;

    const std::optional<SemanticVersion> zero =
        SemanticVersion::Parse("0.0.0");
    const std::optional<SemanticVersion> threeComponent =
        SemanticVersion::Parse("12.34.56");
    const std::optional<SemanticVersion> fourComponent =
        SemanticVersion::Parse("12.34.56.78");
    const std::optional<SemanticVersion> maximum =
        SemanticVersion::Parse("65535.65535.65535.65535");
    const std::optional<SemanticVersion> wide =
        SemanticVersion::Parse(L"7.8.9.10");

    passed &= Expect(
        zero.has_value() && *zero == SemanticVersion(0U, 0U, 0U),
        "parse zero semantic version");
    passed &= Expect(
        threeComponent.has_value() &&
            *threeComponent == SemanticVersion(12U, 34U, 56U) &&
            threeComponent->ToString() == "12.34.56" &&
            threeComponent->ToWString() == L"12.34.56",
        "parse and format three-component semantic version");
    passed &= Expect(
        fourComponent.has_value() &&
            *fourComponent == SemanticVersion(12U, 34U, 56U, 78U) &&
            fourComponent->ToString() == "12.34.56.78",
        "parse and format four-component semantic version");
    passed &= Expect(
        maximum.has_value() &&
            *maximum == SemanticVersion(65535U, 65535U, 65535U, 65535U),
        "parse maximum semantic-version components");
    passed &= Expect(
        wide.has_value() && wide->ToWString() == L"7.8.9.10",
        "parse wide semantic version");
    passed &= Expect(
        SemanticVersion(1U, 2U, 3U, 0U).ToString() == "1.2.3",
        "canonical formatting omits a zero build component");

    constexpr std::array<std::string_view, 19U> kInvalidVersions{
        "",
        "1",
        "1.2",
        "1.2.3.4.5",
        ".1.2.3",
        "1..2.3",
        "1.2.3.",
        "01.2.3",
        "1.02.3",
        "1.2.03",
        "1.2.3.00",
        "65536.0.0",
        "0.65536.0",
        "0.0.65536",
        "0.0.0.65536",
        "-1.2.3",
        "+1.2.3",
        "1.2.x",
        " 1.2.3"};
    for (const std::string_view invalid : kInvalidVersions) {
        passed &= Expect(
            !SemanticVersion::Parse(invalid).has_value(),
            "reject malformed semantic version");
    }
    passed &= Expect(
        !SemanticVersion::Parse("1.2.3\n").has_value(),
        "reject trailing semantic-version whitespace");
    return passed;
}

[[nodiscard]] bool TestSemanticVersionOrdering() {
    bool passed = true;
    passed &= Expect(
        SemanticVersion(1U, 0U, 0U) > SemanticVersion(0U, 65535U, 65535U),
        "semantic-version major comparison");
    passed &= Expect(
        SemanticVersion(1U, 2U, 0U) > SemanticVersion(1U, 1U, 65535U),
        "semantic-version minor comparison");
    passed &= Expect(
        SemanticVersion(1U, 2U, 3U) > SemanticVersion(1U, 2U, 2U, 65535U),
        "semantic-version patch comparison");
    passed &= Expect(
        SemanticVersion(1U, 2U, 3U, 1U) >
            SemanticVersion(1U, 2U, 3U),
        "semantic-version build comparison");
    passed &= Expect(
        SemanticVersion(65535U, 65535U, 65535U, 65535U) >
            SemanticVersion(65535U, 65535U, 65535U, 65534U),
        "semantic-version maximum boundary comparison");
    passed &= Expect(
        SemanticVersion(1U, 2U, 3U) == SemanticVersion(1U, 2U, 3U, 0U),
        "three-component version equals explicit zero build");
    return passed;
}

[[nodiscard]] bool TestSha256ParsingAndFormatting() {
    constexpr std::string_view kLowercaseDigest =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    constexpr std::string_view kUppercaseDigest =
        "BA7816BF8F01CFEA414140DE5DAE2223"
        "B00361A396177A9CB410FF61F20015AD";

    bool passed = true;
    const std::optional<Sha256Digest> lowercase = ParseSha256(kLowercaseDigest);
    const std::optional<Sha256Digest> uppercase = ParseSha256(kUppercaseDigest);
    passed &= Expect(
        lowercase.has_value() && uppercase.has_value(),
        "parse lowercase and uppercase SHA-256 digests");
    if (!lowercase.has_value() || !uppercase.has_value()) {
        return false;
    }

    passed &= Expect(
        Sha256Equals(*lowercase, *uppercase),
        "SHA-256 equality ignores source hex casing");
    passed &= Expect(
        Sha256ToHex(*uppercase) == kLowercaseDigest,
        "SHA-256 formatting is canonical lowercase");

    Sha256Digest different = *lowercase;
    different.back() = static_cast<std::uint8_t>(
        static_cast<unsigned int>(different.back()) ^ 0x01U);
    passed &= Expect(
        !Sha256Equals(*lowercase, different),
        "SHA-256 equality detects a changed byte");

    passed &= Expect(
        !ParseSha256(kLowercaseDigest.substr(0U, 63U)).has_value(),
        "reject short SHA-256 digest");
    const std::string longDigest = std::string(kLowercaseDigest) + "0";
    passed &= Expect(
        !ParseSha256(longDigest).has_value(),
        "reject long SHA-256 digest");
    std::string invalidCharacter(kLowercaseDigest);
    invalidCharacter[17U] = 'g';
    passed &= Expect(
        !ParseSha256(invalidCharacter).has_value(),
        "reject non-hexadecimal SHA-256 character");
    passed &= Expect(
        !ParseSha256(std::string(" ") + std::string(kLowercaseDigest)).has_value(),
        "reject SHA-256 envelope whitespace");
    passed &= Expect(
        !ParseSha256("0x" + std::string(kLowercaseDigest)).has_value(),
        "reject SHA-256 prefix");
    return passed;
}

[[nodiscard]] bool TestFileSha256() {
    constexpr std::string_view kOneMillionAsciiADigest =
        "cdc76e5c9914fb9281a1c7e284d73e67"
        "f1809a48a497200e046d39ccc7112cd0";

    TemporaryFile file;
    bool passed = Expect(
        file.WriteOneMillionAsciiA(),
        "write multi-chunk SHA-256 test file");
    if (!passed) {
        return false;
    }

    DWORD error = ERROR_INVALID_FUNCTION;
    const std::optional<Sha256Digest> digest =
        ComputeFileSha256(file.Path(), &error);
    passed &= Expect(
        digest.has_value() && error == ERROR_SUCCESS,
        "compute file SHA-256 with Windows CNG");
    if (digest.has_value()) {
        passed &= Expect(
            Sha256ToHex(*digest) == kOneMillionAsciiADigest,
            "streamed file SHA-256 matches the known digest");
    }

    const std::filesystem::path missingPath =
        file.Path().parent_path() /
        (file.Path().filename().wstring() + L".missing");
    error = ERROR_SUCCESS;
    passed &= Expect(
        !ComputeFileSha256(missingPath, &error).has_value() &&
            error != ERROR_SUCCESS,
        "missing file reports a SHA-256 error");
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= TestSemanticVersionParsingAndFormatting();
    passed &= TestSemanticVersionOrdering();
    passed &= TestSha256ParsingAndFormatting();
    passed &= TestFileSha256();
    return passed ? 0 : 1;
}
