// tests/shared-core/test_service_identity.cpp
// FIX-7: a bridge in self-verify mode (the VST3 plugin's) proves the listener
// with the HMAC /health challenge itself before it sends the bearer token.
// Before FIX-7 the plugin never armed the identity gate, so without the
// desktop app it handed the token to whatever answered on the port.
#include <catch2/catch_test_macros.hpp>

#include <khensu/common/AppPaths.h>
#include <khensu/common/HmacSha256.h>
#include <khensu/ipc/HttpServiceBridge.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using khensu::ipc::BridgeError;
using khensu::ipc::HttpServiceBridge;

namespace {

std::string tokenOnDisk()
{
    return juce::File(juce::String(khensu::AppPaths::dataDirectory()) + "/token.txt")
        .loadFileAsString()
        .trim()
        .toStdString();
}

/// One-connection-at-a-time stand-in for the service. GET /health?nonce=N is
/// answered with HMAC(token, N) when `honest`, with a wrong proof otherwise;
/// every other request gets 200 "{}". Counts requests that carried a token.
class IdentityServer {
  public:
    IdentityServer(int port, bool honest, std::string token)
        : port_(port), honest_(honest), token_(std::move(token))
    {
        REQUIRE(listener_.createListener(port_, "127.0.0.1"));
        thread_ = std::thread([this] { run(); });
    }
    ~IdentityServer()
    {
        running_ = false;
        listener_.close();
        if (thread_.joinable()) thread_.join();
    }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::atomic<int> healthProbes{0};
    std::atomic<int> tokenRequests{0};

  private:
    void run()
    {
        while (running_) {
            std::unique_ptr<juce::StreamingSocket> conn(listener_.waitForNextConnection());
            if (!conn || !running_) continue;
            juce::String req;
            char buf[4096];
            while (req.indexOf("\r\n\r\n") < 0) {
                if (!conn->waitUntilReady(true, 2000)) break;
                const int n = conn->read(buf, sizeof(buf), false);
                if (n <= 0) break;
                req += juce::String::fromUTF8(buf, n);
            }
            const auto target = req.upToFirstOccurrenceOf("\r\n", false, false)
                                    .fromFirstOccurrenceOf(" ", false, false)
                                    .upToFirstOccurrenceOf(" ", false, false);
            juce::String body = "{}";
            if (target.startsWith("/health")) {
                ++healthProbes;
                const auto nonce = target.fromFirstOccurrenceOf("nonce=", false, false).toStdString();
                const std::string proof =
                    honest_ ? khensu::HmacSha256::hmacHex(token_, nonce) : std::string(64, '0');
                body = "{\"service\":\"khensu-services\",\"version\":\"0\",\"proof\":\"" + juce::String(proof) + "\"}";
            }
            if (req.containsIgnoreCase("\r\nAuthorization:")) ++tokenRequests;
            const juce::String response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                          juce::String(body.getNumBytesAsUTF8()) +
                                          "\r\nConnection: close\r\n\r\n" + body;
            conn->write(response.toRawUTF8(), static_cast<int>(response.getNumBytesAsUTF8()));
        }
    }

    int port_;
    bool honest_;
    std::string token_;
    juce::StreamingSocket listener_;
    std::thread thread_;
    std::atomic<bool> running_{true};
};

/// The gate is process-wide: put it back the way the other tests expect.
struct GateScope {
    GateScope()
    {
        HttpServiceBridge::setServiceIdentityVerified(false);
        HttpServiceBridge::setServiceIdentityRequired(true);
        HttpServiceBridge::setServiceIdentitySelfVerify(true);
    }
    ~GateScope()
    {
        HttpServiceBridge::setServiceIdentitySelfVerify(false);
        HttpServiceBridge::setServiceIdentityRequired(false);
        HttpServiceBridge::setServiceIdentityVerified(false);
    }
};

} // namespace

TEST_CASE("A self-verifying bridge never sends the token to a listener that fails the challenge (FIX-7)",
          "[ipc][identity][fix7]")
{
    GateScope gate;
    IdentityServer impostor(19931, false, "not-the-token");
    HttpServiceBridge bridge(impostor.baseUrl(), 2000, 0);

    const auto result = bridge.getExportTargets();
    REQUIRE(result.is_err());
    CHECK(result.error().kind == BridgeError::Kind::ServiceUnavailable);
    CHECK(impostor.healthProbes.load() >= 1); // it was asked to prove itself
    CHECK(impostor.tokenRequests.load() == 0); // and never got the token
    CHECK_FALSE(HttpServiceBridge::serviceIdentityVerified());
}

TEST_CASE("A self-verifying bridge talks to a listener that proves it holds the token (FIX-7)",
          "[ipc][identity][fix7]")
{
    const std::string token = tokenOnDisk();
    if (token.empty()) SKIP("no token.txt on this machine - the honest service cannot be simulated");

    GateScope gate;
    IdentityServer service(19932, true, token);
    HttpServiceBridge bridge(service.baseUrl(), 2000, 0);

    (void)bridge.getExportTargets(); // "{}" may not parse as targets; the send is what matters
    CHECK(service.healthProbes.load() == 1);
    CHECK(service.tokenRequests.load() == 1);
    CHECK(HttpServiceBridge::serviceIdentityVerified());

    // A fresh proof is reused rather than probing before every request.
    (void)bridge.getExportTargets();
    CHECK(service.healthProbes.load() == 1);
    CHECK(service.tokenRequests.load() == 2);
}

TEST_CASE("Concurrent requests share one failed identity probe instead of queueing one each (FIX-7)",
          "[ipc][identity][fix7]")
{
    // Three plugin instances' transport threads, the service down: each used
    // to wait for the others' probes and then probe again, so the last one
    // overran the processor destructor's 4 s join and was killed.
    GateScope gate;
    IdentityServer impostor(19934, false, "not-the-token");
    std::vector<std::thread> callers;
    std::atomic<int> refused{0};
    for (int i = 0; i < 4; ++i)
        callers.emplace_back([&] {
            HttpServiceBridge bridge(impostor.baseUrl(), 2000, 0);
            if (bridge.getExportTargets().is_err()) ++refused;
        });
    for (auto& t : callers) t.join();
    CHECK(refused.load() == 4);
    CHECK(impostor.healthProbes.load() == 1);
    CHECK(impostor.tokenRequests.load() == 0);

    // Straight after a failed proof the gate answers from it, without a probe.
    HttpServiceBridge again(impostor.baseUrl(), 2000, 0);
    CHECK(again.getExportTargets().is_err());
    CHECK(impostor.healthProbes.load() == 1);
}

TEST_CASE("Without self-verify the gate still fails fast and probes nothing (FIX-7)", "[ipc][identity][fix7]")
{
    GateScope gate;
    HttpServiceBridge::setServiceIdentitySelfVerify(false); // the desktop app's mode: its manager proves
    IdentityServer service(19933, true, "unused");
    HttpServiceBridge bridge(service.baseUrl(), 2000, 0);

    const auto result = bridge.getExportTargets();
    REQUIRE(result.is_err());
    CHECK(result.error().kind == BridgeError::Kind::ServiceUnavailable);
    CHECK(service.healthProbes.load() == 0);
    CHECK(service.tokenRequests.load() == 0);
}
