#include <catch2/catch_test_macros.hpp>

#include "BackgroundReadKey.h"

#include <string>
#include <vector>

namespace {

// A deterministic stand-in for the AppController message-thread completion
// boundary. Readers capture a key before doing I/O; a delayed completion can
// publish only if it still identifies the active session, source and stem.
class AppControllerReadLifecycleHarness final {
  public:
    khensu::BackgroundReadKey request(std::string session, std::string path, std::string item)
    {
        current_ = {std::move(session), std::move(path), std::move(item), ++generation_};
        return current_;
    }

    void complete(const khensu::BackgroundReadKey& key, std::string payload)
    {
        if (key.matches(current_)) published_.push_back(std::move(payload));
    }

    const std::vector<std::string>& published() const noexcept { return published_; }

  private:
    khensu::BackgroundReadKey current_;
    std::uint64_t generation_ = 0;
    std::vector<std::string> published_;
};

} // namespace

TEST_CASE("AppController delayed readers publish only the final rapid-switch state", "[cq06][lifecycle]")
{
    AppControllerReadLifecycleHarness harness;
    const auto oldMix = harness.request("session-a", "C:/mix/old.wav", "waveform");
    const auto vocalStem = harness.request("session-a", "C:/mix/vocals.wav", "vocals");
    const auto newSession = harness.request("session-b", "C:/mix/new.wav", "waveform");
    const auto finalDrums = harness.request("session-b", "C:/mix/drums.wav", "drums");

    harness.complete(oldMix, "old waveform");
    harness.complete(vocalStem, "stale vocals");
    harness.complete(newSession, "stale session waveform");
    harness.complete(finalDrums, "current drums");

    REQUIRE(harness.published() == std::vector<std::string>{"current drums"});
}
