// tests/ui/DeliveryComplianceBridgeTest.cpp
// Broadcast / podcast delivery targets reach the desktop intact: the new
// export-target fields (category, tolerance, mono measurement) parse, and the
// typed delivery-compliance call keeps every check's verdict, delta and fix.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>

using namespace khensu;
using khensu::test::FakeApplyChainServer;

TEST_CASE("Delivery targets: export targets carry category, tolerance and mono measurement", "[delivery]")
{
    FakeApplyChainServer server(19931);
    server.setResponse(
        "GET /analyze/export-targets",
        {200, R"({"targets":[)"
              R"({"id":"spotify","name":"Spotify","category":"streaming","lufs_integrated":-14.0,)"
              R"("integrated_tolerance_lu":null,"true_peak_dbtp":-1.0,"lra_max":null,"short_term_max_lufs":null,)"
              R"("measurement":"as_delivered","standard":"s","notes":"n"},)"
              R"({"id":"podcast_mono","name":"Podcast - mono","category":"podcast","lufs_integrated":-19.0,)"
              R"("integrated_tolerance_lu":1.0,"true_peak_dbtp":-1.0,"lra_max":null,"short_term_max_lufs":null,)"
              R"("measurement":"mono","standard":"Apple","notes":"n"},)"
              R"({"id":"ebu_r128","name":"Broadcast - EBU R128","category":"broadcast","lufs_integrated":-23.0,)"
              R"("integrated_tolerance_lu":0.5,"true_peak_dbtp":-1.0,"lra_max":20.0,"short_term_max_lufs":-18.0,)"
              R"("measurement":"as_delivered","standard":"EBU R128","notes":"n"}]})"});

    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    auto res = bridge.getExportTargets();
    REQUIRE(res.is_ok());
    const auto& t = res.value();
    REQUIRE(t.size() == 3);
    CHECK(t[0].category == "streaming");
    CHECK_FALSE(t[0].integrated_tolerance_lu.has_value()); // advisory target
    CHECK(t[1].measurement == "mono");
    CHECK_THAT(*t[1].integrated_tolerance_lu, Catch::Matchers::WithinAbs(1.0, 1e-9));
    CHECK_THAT(*t[2].short_term_max_lufs, Catch::Matchers::WithinAbs(-18.0, 1e-9));
    CHECK_THAT(*t[2].lra_max, Catch::Matchers::WithinAbs(20.0, 1e-9));
}

TEST_CASE("Delivery targets: compliance result keeps verdicts, deltas and fixes", "[delivery]")
{
    FakeApplyChainServer server(19932);
    server.setResponse(
        "POST /analyze/delivery_compliance",
        {200, R"({"file_path":"Z:/a.wav","channels":2,)"
              R"("measurements":{"integrated_lufs":-20.0,"true_peak_dbtp":-0.5},)"
              R"("mono_measurements":{"integrated_lufs":-23.0,"true_peak_dbtp":-0.5},)"
              R"("results":[{"target_id":"ebu_r128","name":"Broadcast - EBU R128","category":"broadcast",)"
              R"("measurement":"as_delivered","overall":"fail","checks":[)"
              R"({"id":"integrated","status":"fail","measured":-20.0,"target":-23.0,"delta":3.0,)"
              R"("note":"-20.0 LUFS against -23 LUFS +/-0.5 LU.","fix":"Lower the level by 3.0 dB."},)"
              R"({"id":"short_term_max","status":"pass","measured":null,"target":-18.0,"delta":null,)"
              R"("note":"ok","fix":""}]}]})"});

    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    auto res = bridge.analyzeDeliveryCompliance("Z:/a.wav", {"ebu_r128"});
    REQUIRE(res.is_ok());

    const auto body = juce::JSON::parse(server.lastBody("POST /analyze/delivery_compliance"));
    REQUIRE(body["target_ids"].isArray());
    CHECK(body["target_ids"][0].toString() == "ebu_r128");

    const auto& out = res.value();
    CHECK(out.channels == 2);
    REQUIRE(out.results.size() == 1);
    const auto& r = out.results.front();
    CHECK(r.targetId == "ebu_r128");
    CHECK(r.overall == "fail");
    REQUIRE(r.checks.size() == 2);
    CHECK(r.checks[0].status == "fail");
    CHECK_THAT(*r.checks[0].delta, Catch::Matchers::WithinAbs(3.0, 1e-9));
    CHECK(r.checks[0].fix == "Lower the level by 3.0 dB.");
    CHECK_FALSE(r.checks[1].measured.has_value()); // JSON null stays "not measured"
    CHECK_FALSE(r.checks[1].delta.has_value());
}

TEST_CASE("Delivery targets: a reply without results is a parse error, not an empty pass", "[delivery]")
{
    FakeApplyChainServer server(19933);
    server.setResponse("POST /analyze/delivery_compliance", {200, R"({"channels":2})"});
    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    auto res = bridge.analyzeDeliveryCompliance("Z:/a.wav");
    REQUIRE(res.is_err());
    CHECK(res.error().kind == ipc::BridgeError::Kind::ParseError);
}
