// Optional-field parsing for /analyze/sections (audit AUD-03).
//
// The service's SectionResult model types `lufs` as `float | None`, so a
// section too short to measure comes back as JSON null. The bridge used to
// `static_cast<float>` that directly.
//
// This is a CHARACTERIZATION test, not a regression test for a live defect:
// juce::var::null converts to 0.0, which is already the "not computed"
// sentinel SectionData documents, so the cast and parseOptFloat(.., 0.0f)
// currently agree on every value the service can send. Verified empirically --
// this file passes against both the pre- and post-AUD-03 bridge.
//
// The change is still worth having, and this test is what makes it safe: it
// pins the *contract* (null and absent both mean not-computed; real readings
// round-trip) so the agreement stops being a coincidence of JUCE's conversion
// rules. Without it, changing the fallback to any non-zero value -- or the
// service ever sending a non-numeric -- would silently feed a fabricated
// loudness measurement into SectionFlowRecommender.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>

using khensu::ipc::HttpServiceBridge;

TEST_CASE("analyzeSections parses null and missing optional fields as not-computed", "[ipc][sections]")
{
    khensu::test::FakeApplyChainServer server(19731);
    server.setResponse("POST /analyze/sections", {200, R"({
        "section_analysis": [
            {"label": "intro", "start_sec": 0.0, "duration_sec": 1.2, "lufs": null},
            {"label": "verse", "start_sec": 1.2, "duration_sec": 30.0, "lufs": -14.5,
             "crest_db": 9.5, "peak_dbfs": -1.2, "stereo_correlation": 0.62},
            {"label": "outro", "start_sec": 31.2, "duration_sec": 0.8}
        ],
        "total_duration_sec": 32.0
    })"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto result = bridge.analyzeSections("C:/track.wav");
    REQUIRE(result.is_ok());

    const auto& sections = result.value().sections;
    REQUIRE(sections.size() == 3);

    SECTION("an explicit JSON null yields the not-computed sentinel, not a real reading")
    {
        // The bug: this arrived as 0.0f via the cast. It must still be 0.0f, but
        // because parseOptFloat *chose* the sentinel -- not because a cast
        // happened to produce it.
        CHECK(sections[0].lufs_integrated == 0.0f);
        // And it must be excluded by the same `< -0.5f` predicate
        // SectionFlowRecommender uses to pick valid sections.
        CHECK_FALSE(sections[0].lufs_integrated < -0.5f);
    }

    SECTION("an absent key is treated the same as null")
    {
        CHECK(sections[2].lufs_integrated == 0.0f);
    }

    SECTION("a real measurement still round-trips")
    {
        CHECK(sections[1].lufs_integrated == -14.5f);
        CHECK(sections[1].crest_db == 9.5f);
        CHECK(sections[1].peak_dbfs == -1.2f);
        CHECK(sections[1].stereo_correlation == 0.62f);
        CHECK(sections[1].lufs_integrated < -0.5f); // counted as valid
    }

    SECTION("unspecified optionals keep their documented per-field defaults")
    {
        CHECK(sections[0].noise_floor_db == -90.0f);
        CHECK(sections[0].harshness_score == -1.0f);
        CHECK(sections[0].sibilance_score == -1.0f);
        CHECK(sections[0].boundary_confidence == -1.0f);
    }

    CHECK(result.value().section_count == 3);
}
