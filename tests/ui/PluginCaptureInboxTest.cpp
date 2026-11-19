// tests/ui/PluginCaptureInboxTest.cpp
// FEAT-7: the desktop turns the plugin's captures into sessions.
#include <catch2/catch_test_macros.hpp>
#include "session/PluginCaptureInbox.h"
#include <khensu/session/PluginCapture.h>

using namespace khensu;

TEST_CASE("a pending capture becomes a session with the followed session's target (FEAT-7)", "[capture]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_cap_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    REQUIRE(dir.createDirectory());
    SessionManager db(":memory:");
    CreativeTarget target;
    target.primary_genre = "techno";
    const auto followed = db.createSession("Main mix", target).value();

    REQUIRE(dir.getChildFile("cap1.wav").replaceWithText("x"));
    plugin_capture::Manifest m;
    m.audioFile = "cap1.wav";
    m.name = "Plugin capture (8 bars)";
    m.followedSessionId = juce::String(followed.id);
    m.sampleRate = 48000.0;
    m.seconds = 15.0;
    REQUIRE(dir.getChildFile(juce::String("cap1") + plugin_capture::kPendingSuffix)
                .replaceWithText(plugin_capture::toJson(m)));
    // A manifest whose audio never arrived is set aside, not retried forever.
    m.audioFile = "missing.wav";
    REQUIRE(dir.getChildFile(juce::String("cap2") + plugin_capture::kPendingSuffix)
                .replaceWithText(plugin_capture::toJson(m)));

    const auto imported = capture_inbox::importPending(db, dir);
    REQUIRE(imported.size() == 1);
    CHECK(imported[0].name == "Plugin capture (8 bars)");
    const auto s = db.loadSession(imported[0].sessionId).value();
    REQUIRE(s.has_value());
    CHECK(s->target.primary_genre == "techno");
    REQUIRE(s->files.size() == 1);
    CHECK(juce::File(juce::String(s->files[0].path)) == dir.getChildFile("cap1.wav"));
    CHECK(plugin_capture::pending(dir).isEmpty());
    CHECK(capture_inbox::importPending(db, dir).empty()); // imported once
    dir.deleteRecursively();
}
