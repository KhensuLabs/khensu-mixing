#pragma once
#include <khensu/ipc/ILocalServiceBridge.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/ChordAnalysis.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace khensu::ipc {

/// Per-request knobs for the bridge's shared request helpers. Every bridge
/// method routes through one implementation (connect, status check, one 401
/// token-refresh retry, bounded body read, JSON parse, error mapping); these
/// options carry the few ways the call sites legitimately differ.
struct BridgeRequestOptions {
    /// How a non-2xx response's FastAPI {"detail": ...} body feeds the error.
    enum class ErrorDetail { Ignore, Append, Replace };

    int timeoutMs = 0;      ///< connection timeout; <= 0 uses the bridge's constructor timeout
    int readDeadlineMs = 0; ///< body-read deadline (-> Kind::Timeout); <= 0 uses 30 s
    /// Job-status polls: any status other than 200 maps to ServiceUnavailable
    /// ("Could not poll <path>") so pollers treat it as transient.
    bool non200IsUnavailable = false;
    bool requireObject = false;      ///< a body that isn't a JSON object is a ParseError
    bool parseBody = true;           ///< false: a 2xx succeeds with an empty var, body unread
    bool retryOnUnauthorized = true; ///< on 401, re-read token.txt and retry once
    ErrorDetail errorDetail = ErrorDetail::Ignore;
    std::string unreachableMessage; ///< empty: "Could not reach <path>" / "Could not poll <path>"
};

/// The request path every service client shares (ENG-4): the service's
/// address and timeouts, the bearer token, the identity gate and the request
/// helpers. Each domain client (khensu/ipc/bridge/*Api.h) derives from it
/// virtually, so HttpServiceBridge - which derives from all of them - holds
/// one transport. Implemented in src/ipc/HttpServiceBridge.cpp.
class BridgeTransport {
  public:
    /// Single source of truth for the local Python service's address - both
    /// desktop-app (PythonServiceManager::kBaseUrl) and plugin-copilot
    /// reference this instead of each hardcoding their own literal.
    static constexpr const char* kDefaultBaseUrl = "http://127.0.0.1:47239";
    static constexpr int kDefaultTimeoutMs = 30000;
    static constexpr int kDefaultRetryDelayMs = 2000;

    /// `timeout_ms` is the default connect/receive timeout. It bounds how long
    /// a SYNCHRONOUS route may compute (no headers arrive before the handler
    /// returns); the known heavy wrappers use sized per-call timeouts instead,
    /// and a request that runs out of time reports BridgeError::Kind::Timeout.
    explicit BridgeTransport(std::string base_url = kDefaultBaseUrl, int timeout_ms = kDefaultTimeoutMs,
                             int retry_delay_ms = kDefaultRetryDelayMs);

  protected:
    using JsonResult = khensu::Result<juce::var, BridgeError>;

    /// Connect (one 401 token-refresh retry), check the status, and hand back
    /// the open body stream - or nullptr with `error` filled in. `postBody`
    /// == nullptr sends the request without a body (GET/DELETE). `method`
    /// == nullptr uses JUCE's default verb (POST with a body, else GET).
    std::unique_ptr<juce::InputStream> openStream(const char* method, const juce::String& pathAndQuery,
                                                  const juce::String* postBody, const BridgeRequestOptions& options,
                                                  BridgeError& error) const;

    /// True when something accepts TCP connections at base_url_ (a quick
    /// probe used to tell a slow service from an unreachable one, CORE-03).
    bool serviceIsListening() const;

    /// The single request implementation every JSON bridge call goes through:
    /// openStream() + bounded body read (deadline -> Kind::Timeout) + parse.
    JsonResult sendRequest(const char* method, const juce::String& pathAndQuery, const juce::String* postBody,
                           const BridgeRequestOptions& options) const;

    /// POST a JSON body and return the parsed JSON response (or a BridgeError).
    JsonResult postJson(const juce::String& path, const juce::var& body,
                        const BridgeRequestOptions& options = {}) const;
    /// POST an already-serialised body (may be empty).
    JsonResult postText(const juce::String& path, const juce::String& body,
                        const BridgeRequestOptions& options = {}) const;
    /// GET `pathAndQuery` (query string included) and return the parsed JSON.
    JsonResult getJson(const juce::String& pathAndQuery, const BridgeRequestOptions& options = {}) const;
    /// DELETE; true on a 2xx status.
    bool sendDelete(const juce::String& path) const;
    /// POST with an empty body whose result is ignored (job cancellation).
    /// SYNCHRONOUS - blocks the calling thread for up to its 2 s timeout; the
    /// old name "postFireAndForget" suggested otherwise (audit §8.3).
    void postIgnoringResult(const juce::String& path) const;

    /// Legacy name kept for the batch of thin juce::var wrappers: the
    /// connection timeout doubles as the read deadline (clamped 1 s..5 min).
    JsonResult postJsonExpectJson(const juce::String& path, const juce::var& body) const;

    /// Returns the auth header string. The token is cached process-wide (all
    /// bridge instances share it) and re-read automatically when token.txt's
    /// modification time changes (service restart rotates it);
    /// forceRefresh=true re-reads unconditionally (called on HTTP 401).
    juce::String getHeaders(bool forceRefresh = false) const;

    std::string base_url_;
    int timeout_ms_;
    int retry_delay_ms_;
};

} // namespace khensu::ipc
