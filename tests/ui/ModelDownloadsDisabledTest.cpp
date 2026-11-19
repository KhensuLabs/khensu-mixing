// tests/ui/ModelDownloadsDisabledTest.cpp
// The model-download mutations used to return bool, so a 403 "Model downloads
// are disabled in Settings" reached the user as "service unreachable". They
// now return the BridgeError; these tests pin that the 403 detail survives the
// bridge and that the UI-facing hint says "turned off", not "unreachable".
#include <catch2/catch_test_macros.hpp>
#include "support/FakeApplyChainServer.h"
#include "ui/shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>

using khensu::classifyServiceError;
using khensu::serviceErrorDetail;
using khensu::ipc::HttpServiceBridge;
using Kind = khensu::ipc::BridgeError::Kind;

namespace {
const juce::String kDisabledBody = R"({"detail": "Model downloads are disabled in Settings"})";

void checkDisabledError(const khensu::ipc::BridgeError& e)
{
    CHECK(e.kind == Kind::HttpError);
    CHECK(e.http_status == 403);
    CHECK(serviceErrorDetail(e) == "Model downloads are disabled in Settings");
    const auto hint = classifyServiceError(e);
    CHECK(hint.message.containsIgnoreCase("turned off in Settings"));
    CHECK_FALSE(hint.message.containsIgnoreCase("reachable"));
    CHECK_FALSE(khensu::serviceErrorLine(e).containsIgnoreCase("unreachable"));
}
} // namespace

TEST_CASE("Watermark precache 403 surfaces 'disabled in Settings', not unreachable", "[ipc][integration][errors]")
{
    khensu::test::FakeApplyChainServer server(19861);
    server.setResponse("POST /analyze/models/watermark/precache", {403, kDisabledBody});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto r = bridge.startWatermarkModelPrecache();
    REQUIRE(r.is_err());
    checkDisabledError(r.error());
    CHECK(server.requestCount("POST /analyze/models/watermark/precache") == 1);
}

TEST_CASE("Demucs start_download 403 surfaces 'disabled in Settings', not unreachable", "[ipc][integration][errors]")
{
    khensu::test::FakeApplyChainServer server(19862);
    server.setResponse("POST /copilot/start_download", {403, kDisabledBody});
    server.setResponse("POST /copilot/pause_download", {200, R"({"status":"success"})"});
    server.setResponse("POST /copilot/stop_download", {200, R"({"status":"success"})"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto started = bridge.startModelDownload();
    REQUIRE(started.is_err());
    checkDisabledError(started.error());

    CHECK(bridge.pauseModelDownload().is_ok());
    CHECK(bridge.stopModelDownload().is_ok());
}

TEST_CASE("set_models_dir keeps the 422 detail for a rejected folder", "[ipc][integration][errors]")
{
    khensu::test::FakeApplyChainServer server(19863);
    server.setResponse("POST /copilot/set_models_dir",
                       {422, R"({"detail": "models_dir must be a local path, not a UNC/network path"})"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    const auto r = bridge.setModelsDir("//server/share/models");
    REQUIRE(r.is_err());
    CHECK(r.error().kind == Kind::HttpError);
    CHECK(r.error().http_status == 422);
    CHECK(serviceErrorDetail(r.error()).contains("not a UNC/network path"));
}

TEST_CASE("Model-download mutations still report a down service as unreachable", "[ipc][errors]")
{
    // Nothing listens on 19864 - the genuine "service down" case keeps its hint.
    HttpServiceBridge bridge("http://127.0.0.1:19864", 500, 0);
    const auto r = bridge.startModelDownload();
    REQUIRE(r.is_err());
    CHECK(r.error().kind == Kind::ServiceUnavailable);
    CHECK(classifyServiceError(r.error()).message.containsIgnoreCase("isn't reachable"));
}
