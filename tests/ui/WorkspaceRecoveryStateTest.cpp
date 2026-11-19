#include <catch2/catch_test_macros.hpp>
#include "session/WorkspaceRecoveryState.h"

using namespace khensu;

TEST_CASE("workspace recovery state round-trips durable session paths", "[session][recovery]")
{
    const WorkspaceRecoveryState expected{"D:/mixes/song_mastered.wav", "D:/references/target.wav"};
    const auto restored = workspaceRecoveryStateFromJson(workspaceRecoveryStateToJson(expected));

    REQUIRE(restored.has_value());
    CHECK(restored->renderPath == expected.renderPath);
    CHECK(restored->referencePath == expected.referencePath);
}

TEST_CASE("workspace recovery state ignores malformed or incompatible records", "[session][recovery]")
{
    CHECK_FALSE(workspaceRecoveryStateFromJson("not json").has_value());
    CHECK_FALSE(workspaceRecoveryStateFromJson(R"({"version":2,"render_path":"render.wav"})").has_value());
}

TEST_CASE("workspace recovery state keeps the session's Takes bin (WF-5)", "[session][recovery]")
{
    WorkspaceRecoveryState expected{"render.wav", "", {"D:/takes/lead_1.wav", "D:/takes/lead_2.wav"},
                                    "D:/stems/vocals_gainride_1.wav"};
    const auto restored = workspaceRecoveryStateFromJson(workspaceRecoveryStateToJson(expected));
    REQUIRE(restored.has_value());
    CHECK(restored->takes == expected.takes);
    CHECK(restored->vocalPath == expected.vocalPath); // WF-4

    // A save from before the bin restores with an empty one.
    const auto old = workspaceRecoveryStateFromJson(R"({"version":1,"render_path":"r.wav","reference_path":""})");
    REQUIRE(old.has_value());
    CHECK(old->takes.isEmpty());
}
