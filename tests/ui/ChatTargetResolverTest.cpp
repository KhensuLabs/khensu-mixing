#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/Copilot/ChatTargetResolver.h"

using namespace khensu;

namespace {

Session makeSession()
{
    Session session;
    AudioFileRef file;
    file.path = "C:/mix.wav";
    session.files.push_back(file);
    return session;
}

} // namespace

TEST_CASE("AI-02 target resolver uses the primary file for empty and mix targets", "[chat_target]")
{
    auto session = makeSession();
    const auto noStem = [](const juce::String&) { return juce::File(); };

    const auto empty = resolveChatTargetStem(&session, {}, noStem);
    CHECK(empty.ok);
    CHECK(empty.targetStem == "mix");
    CHECK(empty.path == "C:/mix.wav");

    const auto mix = resolveChatTargetStem(&session, " MIX ", noStem);
    CHECK(mix.ok);
    CHECK(mix.targetStem == "mix");
    CHECK(mix.path == "C:/mix.wav");
}

TEST_CASE("AI-02 target resolver requires a real separated stem", "[chat_target]")
{
    auto session = makeSession();
    const auto missing = resolveChatTargetStem(&session, "vocals", [](const juce::String&) { return juce::File(); });
    CHECK_FALSE(missing.ok);
    CHECK(missing.path.isEmpty());
    CHECK(missing.error.contains("no separated vocals stem"));

    const auto stemPath = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("khensu-chat-target-vocals.wav");
    stemPath.replaceWithText("test");
    const auto resolved = resolveChatTargetStem(
        &session, " VOCALS ", [&](const juce::String& stem) { return stem == "vocals" ? stemPath : juce::File(); });
    CHECK(resolved.ok);
    CHECK(resolved.targetStem == "vocals");
    CHECK(resolved.path == stemPath.getFullPathName());
    stemPath.deleteFile();
}

TEST_CASE("AI-02 target resolver fails closed without session audio", "[chat_target]")
{
    const auto result = resolveChatTargetStem(nullptr, "vocals", [](const juce::String&) { return juce::File(); });
    CHECK_FALSE(result.ok);
    CHECK(result.error == "No session audio to process.");
}
