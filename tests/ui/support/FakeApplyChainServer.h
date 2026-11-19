// tests/ui/support/FakeApplyChainServer.h
// Minimal single-connection-at-a-time HTTP/1.1 server standing in for the
// Python service, so RenderJobManager/panel network code paths (not just the
// "nothing is listening" failure path) can be exercised: cancel-after-jobId-
// assignment round trips, the service-loss polling bound, 409 abstentions,
// and happy-path completions. Point a bridge/panel/RenderJobManager at this
// via its DI'd baseUrl instead of the real 127.0.0.1:47239.
//
// Promoted 2026-07-18 out of RenderJobManagerTest.cpp (where it originated)
// so MasteringAgentPanelTest.cpp/SectionIntensityPanelTest.cpp can reuse it
// instead of duplicating the HTTP parsing logic - see
// docs/superpowers/plans/2026-07-18-appcontroller-testability-facade.md.
#pragma once
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace khensu::test {

class FakeApplyChainServer {
  public:
    struct Response {
        int status = 200;
        juce::String body;
    };

    explicit FakeApplyChainServer(int port) : port_(port)
    {
        REQUIRE(listener_.createListener(port_, "127.0.0.1"));
        thread_ = std::thread([this] { run(); });
    }

    ~FakeApplyChainServer()
    {
        running_ = false;
        listener_.close();
        if (thread_.joinable()) thread_.join();
    }

    juce::String baseUrl() const { return "http://127.0.0.1:" + juce::String(port_); }

    /// Answers every request matching "METHOD path" with `r` (path is an
    /// exact match, no query-string handling needed by any caller here).
    void setResponse(const juce::String& key, Response r)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[key] = r;
    }

    int requestCount(const juce::String& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = counts_.find(key);
        return it == counts_.end() ? 0 : it->second;
    }

    /// The most recent request body seen for "METHOD path" (empty if none) -
    /// lets a test assert what was actually sent over the wire, e.g. that
    /// target_stem made it into the apply-chain request (audit AI-02).
    juce::String lastBody(const juce::String& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = bodies_.find(key);
        return it == bodies_.end() ? juce::String() : it->second;
    }

    /// The raw request headers most recently seen for "METHOD path" (empty
    /// if none) - lets a test assert what was sent, e.g. that the bearer
    /// token never goes to the unauthenticated /health route.
    juce::String lastHeaders(const juce::String& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = headers_.find(key);
        return it == headers_.end() ? juce::String() : it->second;
    }

  private:
    void run()
    {
        while (running_) {
            std::unique_ptr<juce::StreamingSocket> conn(listener_.waitForNextConnection());
            if (!conn || !running_) continue; // listener closed from the destructor
            handleConnection(*conn);
        }
    }

    void handleConnection(juce::StreamingSocket& sock)
    {
        // Localhost, small JSON bodies: read until the header terminator is
        // seen, then (if Content-Length says there's a body) until that many
        // body bytes have arrived. Good enough for this fixed request shape -
        // not a general-purpose HTTP parser.
        juce::String raw;
        char buf[4096];
        for (;;) {
            if (!sock.waitUntilReady(true, 2000)) return;
            const int n = sock.read(buf, sizeof(buf), false);
            if (n <= 0) return;
            raw += juce::String::fromUTF8(buf, n);
            const auto headerEnd = raw.indexOf("\r\n\r\n");
            if (headerEnd < 0) continue;
            const auto headers = raw.substring(0, headerEnd);
            int contentLength = 0;
            for (const auto& line : juce::StringArray::fromLines(headers))
                if (line.startsWithIgnoreCase("Content-Length:"))
                    contentLength = line.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
            const auto bodySoFar = raw.substring(headerEnd + 4);
            if (bodySoFar.getNumBytesAsUTF8() >= (size_t)contentLength) break;
        }

        const auto requestLine = raw.upToFirstOccurrenceOf("\r\n", false, false);
        const auto method = requestLine.upToFirstOccurrenceOf(" ", false, false);
        const auto path = requestLine.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
        const auto key = method + " " + path;

        const auto body = raw.substring(raw.indexOf("\r\n\r\n") + 4);

        Response resp{200, "{}"};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counts_[key];
            bodies_[key] = body;
            headers_[key] = raw.substring(0, raw.indexOf("\r\n\r\n"));
            const auto it = responses_.find(key);
            if (it != responses_.end()) resp = it->second;
        }

        const auto bodyBytes = resp.body.getNumBytesAsUTF8();
        juce::String out = "HTTP/1.1 " + juce::String(resp.status) + " x\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + juce::String((int)bodyBytes) + "\r\n"
                          "Connection: close\r\n\r\n" + resp.body;
        const auto outUtf8 = out.toUTF8();
        sock.write(outUtf8.getAddress(), (int)strlen(outUtf8.getAddress()));
    }

    int port_;
    juce::StreamingSocket listener_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    mutable std::mutex mutex_;
    std::map<juce::String, Response> responses_;
    std::map<juce::String, int> counts_;
    std::map<juce::String, juce::String> bodies_;
    std::map<juce::String, juce::String> headers_;
};

} // namespace khensu::test
