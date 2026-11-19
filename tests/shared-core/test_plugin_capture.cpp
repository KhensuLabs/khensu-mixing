// tests/shared-core/test_plugin_capture.cpp
// FEAT-7: the plugin-to-desktop capture manifest.
#include <catch2/catch_test_macros.hpp>
#include <khensu/session/PluginCapture.h>

using namespace khensu;

TEST_CASE("a capture manifest round-trips and only names a file beside it (FEAT-7)", "[capture]")
{
    plugin_capture::Manifest m;
    m.audioFile = "capture_20261117-120000.wav";
    m.name = "Plugin capture 12:00 (8 bars)";
    m.followedSessionId = "abc";
    m.bpm = 128.0;
    m.sampleRate = 48000.0;
    m.seconds = 15.0;
    const auto back = plugin_capture::fromJson(plugin_capture::toJson(m));
    REQUIRE(back.has_value());
    CHECK(back->audioFile == m.audioFile);
    CHECK(back->followedSessionId == "abc");
    CHECK(back->bpm == 128.0);

    for (const char* bad : {"../x.wav", "C:/x.wav", "sub/x.wav", "sub\\x.wav", ""}) {
        m.audioFile = bad;
        CHECK_FALSE(plugin_capture::fromJson(plugin_capture::toJson(m)).has_value());
    }
    CHECK_FALSE(plugin_capture::fromJson("{\"version\": 2}").has_value());
}

TEST_CASE("capture lengths in bars and beats (FEAT-7)", "[capture]")
{
    CHECK(plugin_capture::samplesForBars(8, 120.0, 48000.0) == 16 * 48000); // 8 bars of 4/4 at 120 = 16 s
    CHECK(plugin_capture::samplesForBeats(6.0, 90.0, 44100.0) == 4 * 44100);
    CHECK(plugin_capture::samplesForBars(8, 0.0, 48000.0) == 0);
}
