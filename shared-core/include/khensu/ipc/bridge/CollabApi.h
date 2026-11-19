#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Collaboration and trust (CE-COL-1..4, CE-TRU-1/3) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Collab.cpp. Usable on its own (`CollabApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class CollabApi : public virtual BridgeTransport {
  public:
    CollabApi() = default;
    explicit CollabApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                       int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // CE-COL-1..4 / CE-TRU-1 / CE-TRU-3 (collaboration and trust) -
    // HttpServiceBridge_Collab.cpp. Parsed JSON; read it through
    // api::fields::SessionComment*, RenderVersion*, Bundle*, Revision*,
    // Listening*, ReferenceProfile*, CalibrationTableResponse and
    // EvidenceReportResponse.
    /// GET /comments?session_id=
    khensu::Result<juce::var, BridgeError> listComments(const juce::String& sessionId, bool includeResolved = true);
    /// POST /comments. `endS` < 0 = a point comment.
    khensu::Result<juce::var, BridgeError> addComment(const juce::String& sessionId, const juce::String& text,
                                                      double startS, double endS, const juce::String& author);
    /// PATCH /comments/{id}: `body` holds the CommentUpdateRequest fields to change.
    khensu::Result<juce::var, BridgeError> updateComment(const juce::String& commentId, const juce::var& body);
    /// DELETE /comments/{id}; true on success.
    bool deleteComment(const juce::String& commentId);
    /// GET /versions?session_id= ({versions, branches}).
    khensu::Result<juce::var, BridgeError> listRenderVersions(const juce::String& sessionId);
    /// POST /versions/sync: `nodes` is an array of {path, parent_path}.
    khensu::Result<juce::var, BridgeError> syncRenderVersions(const juce::String& sessionId, const juce::var& nodes,
                                                              const juce::String& activeBranch);
    /// PATCH /versions/{id} with {label?, tags?}.
    khensu::Result<juce::var, BridgeError> updateRenderVersion(const juce::String& versionId, const juce::var& body);
    /// POST /versions/branches.
    khensu::Result<juce::var, BridgeError> createRenderBranch(const juce::String& sessionId, const juce::String& name,
                                                              const juce::String& baseVersionId);
    /// POST /bundle/export, /bundle/inspect, /bundle/import, /revision/report,
    /// /references/*, /report/evidence: the body is the request model as JSON.
    khensu::Result<juce::var, BridgeError> postCollab(const std::string& route, const juce::var& body,
                                                      int timeoutMs = 0);
    /// GET /listening/status, /listening/results, /references/profiles,
    /// /calibration/findings (query string included in `pathAndQuery`).
    khensu::Result<juce::var, BridgeError> getCollab(const juce::String& pathAndQuery);
    /// DELETE /references/profiles/{id}; true on success.
    bool deleteReferenceProfile(const juce::String& profileId);
};

} // namespace khensu::ipc
