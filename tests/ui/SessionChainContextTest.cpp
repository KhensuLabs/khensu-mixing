#include <catch2/catch_test_macros.hpp>
#include "ui/Copilot/SessionChainContext.h"
#include "ui/Copilot/SessionTargetContext.h"

using namespace khensu;

TEST_CASE("Session chain context maps intake template targets to generator choices", "[session][chain]")
{
    CreativeTarget target;
    target.primary_genre = "rap";
    target.mood = "confident";
    target.energy = 70.0f;
    target.bpm = 140.0f;

    const auto context = sessionChainContextForTarget(target);
    CHECK(context.genre == "Hip-Hop");
    CHECK(context.mood == "Modern");
    CHECK(context.energy == "High");
    CHECK(context.bpm == 140.0);
}

TEST_CASE("Session chain context normalizes non-generator genres and moods", "[session][chain]")
{
    CreativeTarget target;
    target.primary_genre = "neo-soul";
    target.mood = "sultry";
    target.energy = 45.0f;

    const auto context = sessionChainContextForTarget(target);
    CHECK(context.genre == "R&B");
    CHECK(context.mood == "Warm");
    CHECK(context.energy == "Mid");
}

TEST_CASE("Session chain context safely defaults unknown template values", "[session][chain]")
{
    CreativeTarget target;
    target.primary_genre = "unclassified";
    target.mood = "";
    target.energy = 10.0f;

    const auto context = sessionChainContextForTarget(target);
    CHECK(context.genre == "Pop");
    CHECK(context.mood == "Modern");
    CHECK(context.energy == "Low");
    CHECK(context.bpm == 0.0);
}

TEST_CASE("The copilot gets the session target the user filled in (WF-11)", "[session][chat]")
{
    CreativeTarget target;
    CHECK(chat_context::sessionTarget(target).isVoid()); // nothing filled in, nothing sent

    target.primary_genre = "shoegaze";
    target.mood = "longing";
    target.playback_intent = "headphone";
    target.vision_statement = "a bedroom at 3am in summer";
    const auto v = chat_context::sessionTarget(target);
    CHECK(v["genre"].toString() == "shoegaze");
    CHECK(v["mood"].toString() == "longing");
    CHECK(v["playback_intent"].toString() == "headphone");
    CHECK(v["vision"].toString() == "a bedroom at 3am in summer");
    CHECK_FALSE(v.hasProperty("sub_genre")); // empty fields are left out
}
