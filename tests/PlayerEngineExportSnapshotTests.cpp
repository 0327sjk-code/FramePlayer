#include "Core/PlayerEngine.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using zt::sequence::FrameRequestKind;
using zt::sequence::PlayerEngine;
using zt::sequence::PlayerSnapshot;
using zt::sequence::ScrubUpdateResult;
using zt::sequence::SequenceExportSnapshot;

inline constexpr std::array<std::uint8_t, 70> kOnePixelPng{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
    0x54, 0x78, 0xDA, 0x63, 0x64, 0xF8, 0xCF, 0xF0,
    0x1F, 0x00, 0x05, 0x02, 0x02, 0x00, 0x0A, 0x3D,
    0x02, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

class TemporarySequence final {
public:
    TemporarySequence() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        root_ = std::filesystem::temp_directory_path() /
            ("ZTSequencePlayerExportSnapshot_" + std::to_string(nonce));
        std::error_code error;
        std::filesystem::create_directories(root_, error);
        ready_ = !error;
    }

    ~TemporarySequence() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TemporarySequence(const TemporarySequence&) = delete;
    TemporarySequence& operator=(const TemporarySequence&) = delete;

    [[nodiscard]] bool WriteFrame(const std::wstring& name) const {
        if (!ready_) {
            return false;
        }
        std::ofstream output(root_ / name, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(kOnePixelPng.data()),
            static_cast<std::streamsize>(kOnePixelPng.size()));
        return output.good();
    }

    [[nodiscard]] const std::filesystem::path& Root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
    bool ready_ = false;
};

[[nodiscard]] bool Expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool WaitForCommittedSequence(
    PlayerEngine& engine,
    PlayerSnapshot& snapshot) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = engine.Snapshot();
        if (!snapshot.loading) {
            return snapshot.hasSequence;
        }
        std::this_thread::sleep_for(10ms);
    }
    snapshot = engine.Snapshot();
    return false;
}

}  // namespace

int main() {
    TemporarySequence sequence;
    bool passed = true;
    passed &= Expect(sequence.WriteFrame(L"Frame.10.png"), "write frame 10");
    passed &= Expect(sequence.WriteFrame(L"Frame.2.png"), "write frame 2");
    passed &= Expect(sequence.WriteFrame(L"Frame.1.png"), "write frame 1");
    if (!passed) {
        return 1;
    }

    PlayerEngine engine;
    passed &= Expect(engine.LoadFolder(sequence.Root()), "submit sequence load");

    PlayerSnapshot playerSnapshot;
    passed &= Expect(
        WaitForCommittedSequence(engine, playerSnapshot),
        "commit sequence before export snapshot");
    if (!playerSnapshot.hasSequence) {
        std::cerr << playerSnapshot.errorUtf8 << '\n';
        engine.Shutdown();
        return 1;
    }

    engine.BeginScrub();
    const ScrubUpdateResult scrubUpdate = engine.UpdateScrub(999U);
    passed &= Expect(
        scrubUpdate.accepted && scrubUpdate.scrubbing,
        "sequence scrub update is accepted atomically");
    passed &= Expect(
        scrubUpdate.requestedFrame == 2U
            && scrubUpdate.generation == playerSnapshot.generation,
        "sequence scrub result returns the clamped frame and generation");
    playerSnapshot = engine.Snapshot();
    passed &= Expect(
        playerSnapshot.scrubbing && playerSnapshot.requestedFrame == 2U,
        "sequence scrub result matches committed engine state");
    engine.EndScrub();

    engine.Seek(0U);
    passed &= Expect(
        engine.Snapshot().requestedFrame == 0U,
        "seek replaces a pending post-scrub hot region");
    engine.BeginScrub();
    (void)engine.UpdateScrub(2U);
    engine.EndScrub();
    engine.StepFrame(-1);
    passed &= Expect(
        engine.Snapshot().requestedFrame == 1U,
        "frame step replaces a pending post-scrub hot region");
    engine.BeginScrub();
    (void)engine.UpdateScrub(2U);
    engine.EndScrub();
    engine.RequestFrame(0U, FrameRequestKind::InteractiveSeek);
    passed &= Expect(
        engine.Snapshot().requestedFrame == 0U,
        "interactive request replaces a pending post-scrub hot region");
    engine.SetBackgroundResourceMode(true);
    engine.BeginScrub();
    (void)engine.UpdateScrub(2U);
    engine.EndScrub();
    engine.SetBackgroundResourceMode(false);
    engine.Seek(1U);
    passed &= Expect(
        engine.Snapshot().requestedFrame == 1U,
        "foreground restore resumes scheduling after a background-mode scrub");

    engine.SetPlaybackRange(1U, 2U);
    engine.SetFramesPerSecond(48.0);
    const std::optional<SequenceExportSnapshot> captured =
        engine.CaptureExportSnapshot();
    passed &= Expect(captured.has_value(), "capture immutable export snapshot");
    if (captured) {
        passed &= Expect(
            captured->sourceGeneration == playerSnapshot.generation,
            "snapshot generation matches committed session");
        passed &= Expect(
            captured->orderedPngFrames.size() == 3U,
            "snapshot contains complete PNG file list");
        passed &= Expect(
            captured->orderedPngFrames[0].relativePath == L"Frame.1.png" &&
                captured->orderedPngFrames[1].relativePath == L"Frame.2.png" &&
                captured->orderedPngFrames[2].relativePath == L"Frame.10.png",
            "snapshot preserves natural numeric order");
        passed &= Expect(
            captured->inclusiveRange.startFrame == 1U &&
                captured->inclusiveRange.endFrame == 2U,
            "snapshot captures inclusive playback range");
        passed &= Expect(
            captured->framesPerSecond == 48.0,
            "snapshot captures fixed export frame rate");

        engine.SetPlaybackRange(0U, 0U);
        engine.SetFramesPerSecond(60.0);
        passed &= Expect(
            captured->inclusiveRange.startFrame == 1U &&
                captured->inclusiveRange.endFrame == 2U &&
                captured->framesPerSecond == 48.0,
            "captured request is unaffected by later engine settings");
    }

    engine.Shutdown();
    const ScrubUpdateResult rejectedScrub = engine.UpdateScrub(0U);
    passed &= Expect(
        !rejectedScrub.accepted && !rejectedScrub.scrubbing
            && rejectedScrub.generation == 0U,
        "shutdown engine rejects scrub updates");
    passed &= Expect(
        !engine.CaptureExportSnapshot().has_value(),
        "shutdown engine rejects new export snapshots");
    return passed ? 0 : 1;
}
