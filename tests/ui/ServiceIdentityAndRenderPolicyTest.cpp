// tests/ui/ServiceIdentityAndRenderPolicyTest.cpp
// AUDIT-020: the desktop trusts a /health listener only when it proves it can
// read our token file (HMAC-SHA256(token, nonce)), not merely because it
// prints the right version string.
// AUDIT-058: session deletion removes only renders this app wrote.
#include <catch2/catch_test_macros.hpp>
#include "PythonServiceManager.h"
#include "support/FakeApplyChainServer.h"
#include "ui/shared/RenderPaths.h"
#include <khensu/common/HmacSha256.h>
#include <khensu/ipc/HttpServiceBridge.h>

using khensu::PythonServiceManager;
using Verdict = PythonServiceManager::HealthVerdict;

namespace {
juce::var healthBody(const juce::String& version, const juce::String& proof = {}, bool withProof = true)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("service", "khensu-services");
    o->setProperty("version", version);
    if (withProof) o->setProperty("proof", proof);
    return juce::var(o);
}
} // namespace

TEST_CASE("classifyHealthResponse verifies the HMAC proof, not just the version", "[service][identity]")
{
    const juce::String token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const juce::String nonce = "feedfacecafebeef";
    const juce::String version = PythonServiceManager::kExpectedServiceVersion;
    const juce::String proof(khensu::HmacSha256::hmacHex(token.toStdString(), nonce.toStdString()));

    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, proof), token, nonce) == Verdict::Verified);
    // Case of the hex digits doesn't matter.
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, proof.toUpperCase()), token, nonce) ==
          Verdict::Verified);
    // An impostor that copies the version string but can't read the token.
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, {}, false), token, nonce) ==
          Verdict::Unverified);
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, juce::String::repeatedString("a", 64)),
                                                       token, nonce) == Verdict::Unverified);
    // A replayed proof for a different nonce.
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, proof), token, "0000") ==
          Verdict::Unverified);
    // Without our token nothing can be verified.
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody(version, proof), {}, nonce) == Verdict::Unverified);
    // An old khensu build is "stale" (killed and replaced), anything else invalid.
    CHECK(PythonServiceManager::classifyHealthResponse(healthBody("0.0.1", proof), token, nonce) == Verdict::Stale);
    CHECK(PythonServiceManager::classifyHealthResponse(juce::var("not json"), token, nonce) == Verdict::Invalid);
}

TEST_CASE("isAppRenderFileFor accepts only this app's render names beside the source", "[render][erasure]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-render-policy");
    const auto source = dir.getChildFile("My Song.wav");

    // What uniqueRenderFile() produces, old and new stamp formats, plus the
    // getNonexistentSibling() variants and a render of a render.
    CHECK(khensu::isAppRenderFileFor(dir.getChildFile("My Song_eq_20261015-120000-3.wav"), source));
    CHECK(khensu::isAppRenderFileFor(dir.getChildFile("My Song_matched_20261015-120000.wav"), source));
    CHECK(khensu::isAppRenderFileFor(dir.getChildFile("My Song_eq_20261015-120000-3_2.wav"), source));
    CHECK(khensu::isAppRenderFileFor(dir.getChildFile("My Song_eq_20261015-120000-3 (2).wav"), source));
    CHECK(khensu::isAppRenderFileFor(dir.getChildFile("My Song_eq_20261015-120000-1_fix_20261015-120100-2.wav"),
                                     source));

    // Never the source itself, a user's own similarly named file, another
    // folder, or another format.
    CHECK_FALSE(khensu::isAppRenderFileFor(source, source));
    CHECK_FALSE(khensu::isAppRenderFileFor(dir.getChildFile("My Song (final mix).wav"), source));
    CHECK_FALSE(khensu::isAppRenderFileFor(dir.getChildFile("My Song_v2.wav"), source));
    CHECK_FALSE(khensu::isAppRenderFileFor(dir.getChildFile("Other_eq_20261015-120000-3.wav"), source));
    CHECK_FALSE(khensu::isAppRenderFileFor(dir.getChildFile("sub").getChildFile("My Song_eq_20261015-120000-3.wav"),
                                           source));
    CHECK_FALSE(khensu::isAppRenderFileFor(dir.getChildFile("My Song_eq_20261015-120000-3.mp3"), source));
}

TEST_CASE("the bearer token is never sent to the unauthenticated /health route", "[service][identity]")
{
    // /health is the one route allowed before the listener has proved its
    // identity, so a squatter on the port must not collect the token from an
    // availability probe; authenticated routes still carry it.
    // Another test's AppController may have switched the process-wide gate
    // on; this case is about headers, not the gate.
    khensu::ipc::HttpServiceBridge::setServiceIdentityRequired(false);
    khensu::test::FakeApplyChainServer server(19782);
    server.setResponse("GET /health", {200, R"({"status":"ok"})"});
    server.setResponse("GET /analyze/metadata", {200, R"({"service_version":"x"})"});

    khensu::ipc::HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    CHECK(bridge.is_service_available());
    REQUIRE(server.requestCount("GET /health") == 1);
    CHECK_FALSE(server.lastHeaders("GET /health").containsIgnoreCase("Authorization"));

    CHECK(bridge.getServiceMetadata().is_ok());
    REQUIRE(server.requestCount("GET /analyze/metadata") == 1);
    CHECK(server.lastHeaders("GET /analyze/metadata").containsIgnoreCase("Authorization: Bearer"));
}
