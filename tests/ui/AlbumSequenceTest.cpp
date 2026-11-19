// tests/ui/AlbumSequenceTest.cpp
// Album master: the editable sequence model (order, ISRC / UPC checks that
// mirror album_master.py, trims from the consistency response) and the bridge
// wire bodies / status parsing for /album/master/*.
#include <catch2/catch_test_macros.hpp>
#include "ui/ExportDelivery/AlbumSequence.h"
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>

using namespace khensu;
using khensu::test::FakeApplyChainServer;

TEST_CASE("Album sequence: ISRC canonicalisation mirrors the service", "[album]")
{
    CHECK(*album_seq::canonicalIsrc("US-S1Z-99-00001") == "USS1Z9900001");
    CHECK(*album_seq::canonicalIsrc("gbaye0601498") == "GBAYE0601498");
    CHECK(album_seq::canonicalIsrc("")->isEmpty()); // no ISRC is allowed
    CHECK_FALSE(album_seq::canonicalIsrc("US-S1Z-99-0001").has_value());  // 11 chars
    CHECK_FALSE(album_seq::canonicalIsrc("1S-S1Z-99-00001").has_value()); // country must be letters
    CHECK_FALSE(album_seq::canonicalIsrc("US-S1Z-9A-00001").has_value()); // year must be digits
    CHECK_FALSE(album_seq::canonicalIsrc("US_S1Z_99_00001").has_value());
}

TEST_CASE("Album sequence: UPC-A / EAN-13 check digits", "[album]")
{
    CHECK(album_seq::isValidCatalog(""));
    CHECK(album_seq::isValidCatalog("036000291452"));
    CHECK(album_seq::isValidCatalog("4006381333931"));
    CHECK_FALSE(album_seq::isValidCatalog("4006381333932"));
    CHECK_FALSE(album_seq::isValidCatalog("12345"));
    CHECK_FALSE(album_seq::isValidCatalog("40063813339AB"));
}

TEST_CASE("Album sequence: CD time formatting", "[album]")
{
    CHECK(album_seq::msfFromFrames(0) == "00:00:00");
    CHECK(album_seq::msfFromFrames(150) == "00:02:00");
    CHECK(album_seq::msfFromFrames(75 * 61 + 74) == "01:01:74");
}

TEST_CASE("Album sequence: reorder, remove, trims and validation", "[album]")
{
    album_seq::AlbumSequence seq;
    seq.setPaths({"C:/a/One.wav", "C:/a/Two.wav", "C:/a/Three.wav"});
    REQUIRE(seq.size() == 3);
    CHECK(seq.tracks()[0].title == "One");

    CHECK(seq.moveDown(0));
    CHECK(seq.tracks()[0].title == "Two");
    CHECK_FALSE(seq.moveUp(0));
    CHECK_FALSE(seq.moveDown(2));
    CHECK(seq.remove(2));
    CHECK(seq.size() == 2);

    juce::var resp = juce::JSON::parse(R"({"tracks":[{"path":"C:/a/One.wav","gain_db":-1.5},
                                                     {"path":"C:/a/Missing.wav","gain_db":3.0}]})");
    CHECK(seq.applyConsistency(resp) == 1);
    CHECK(seq.tracks()[1].gainDb == -1.5); // matched by path, after the reorder

    CHECK(seq.validationErrors("").isEmpty());
    seq.tracks()[0].isrc = "USS1Z9900001";
    seq.tracks()[1].isrc = "US-S1Z-99-00001"; // duplicate once canonical
    CHECK(seq.validationErrors("").size() == 1);
    seq.tracks()[1].isrc = "nope";
    CHECK(seq.validationErrors("4006381333932").size() == 2); // bad ISRC + bad catalogue
}

TEST_CASE("Album master bridge: plan / start bodies and status parsing", "[album]")
{
    FakeApplyChainServer server(19891);
    server.setResponse("POST /album/master/plan", {200, R"({"total_msf":"00:12:00","tracks":[],"warnings":[]})"});
    server.setResponse("POST /album/master/start", {200, R"({"job_id":"job-album","total":2,"output_dir":"Z:/m"})"});
    server.setResponse("GET /album/master/status/job-album",
                       {200, R"({"status":"running","progress":{"total":2,"done":1,"current":"track 2"},)"
                             R"("output_dir":"Z:/m","result":null,"error":null})"});

    ipc::HttpServiceBridge::AlbumMasterSpec spec;
    ipc::HttpServiceBridge::AlbumTrackSpec a, b;
    a.path = "Z:/a.wav";
    a.isrc = "USS1Z9900001";
    b.path = "Z:/b.wav";
    b.crossfadeSec = 1.5;
    b.gainDb = -0.75;
    spec.tracks = {a, b};
    spec.catalog = "4006381333931";
    spec.ddp = false;
    spec.outputDir = "Z:/m";

    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    REQUIRE(bridge.albumMasterPlan(spec).is_ok());
    const auto plan = juce::JSON::parse(server.lastBody("POST /album/master/plan"));
    CHECK(plan["catalog"].toString() == "4006381333931");
    CHECK(plan["tracks"][1]["crossfade_sec"] == juce::var(1.5));
    CHECK(plan["tracks"][0]["isrc"].toString() == "USS1Z9900001");
    REQUIRE(plan.getDynamicObject() != nullptr);
    CHECK_FALSE(plan.getDynamicObject()->hasProperty("ddp")); // outputs only on /start

    const auto started = bridge.startAlbumMasterJob(spec);
    REQUIRE(started.is_ok());
    CHECK(started.value() == "job-album");
    const auto body = juce::JSON::parse(server.lastBody("POST /album/master/start"));
    CHECK(body["ddp"] == juce::var(false));
    CHECK(body["output_dir"].toString() == "Z:/m");
    CHECK(body["tracks"][1]["gain_db"] == juce::var(-0.75));

    const auto st = bridge.getAlbumMasterJobStatus("job-album");
    REQUIRE(st.is_ok());
    CHECK(st.value().status == "running");
    CHECK(st.value().done == 1);
    CHECK(st.value().total == 2);
    CHECK(st.value().current == "track 2");
    CHECK(st.value().error.isEmpty());
}
