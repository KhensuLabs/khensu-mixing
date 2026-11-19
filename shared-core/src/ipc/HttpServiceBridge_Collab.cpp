// CE-COL-1..4 / CE-TRU-1 / CE-TRU-3 (collaboration and trust): session
// comments, render versions, bundles, revision reports, the LAN listening
// room, reference profiles, the calibration table and the evidence report.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {
/// Bundle export / import with audio, a two-file revision report and the
/// listening room's loudness-matched copies can take a while.
constexpr int kCollabHeavyMs = 5 * 60 * 1000;

juce::String escaped(const juce::String& s) { return juce::URL::addEscapeChars(s, true); }
} // namespace

khensu::Result<juce::var, BridgeError> CollabApi::listComments(const juce::String& sessionId,
                                                                       bool includeResolved)
{
    return getJson("/comments?session_id=" + escaped(sessionId) +
                       (includeResolved ? juce::String() : juce::String("&include_resolved=false")),
                   objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::addComment(const juce::String& sessionId,
                                                                     const juce::String& text, double startS,
                                                                     double endS, const juce::String& author)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CommentCreateRequest::session_id, sessionId);
    o->setProperty(apif::CommentCreateRequest::text, text);
    o->setProperty(apif::CommentCreateRequest::start_s, startS);
    if (endS >= 0.0) o->setProperty(apif::CommentCreateRequest::end_s, endS);
    o->setProperty(apif::CommentCreateRequest::author, author);
    return postJson("/comments", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::updateComment(const juce::String& commentId,
                                                                        const juce::var& body)
{
    const auto text = juce::JSON::toString(body, true);
    return sendRequest("PATCH", "/comments/" + escaped(commentId), &text, objectResponse());
}

bool CollabApi::deleteComment(const juce::String& commentId)
{
    return sendDelete("/comments/" + escaped(commentId));
}

khensu::Result<juce::var, BridgeError> CollabApi::listRenderVersions(const juce::String& sessionId)
{
    return getJson("/versions?session_id=" + escaped(sessionId), objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::syncRenderVersions(const juce::String& sessionId,
                                                                             const juce::var& nodes,
                                                                             const juce::String& activeBranch)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::VersionSyncRequest::session_id, sessionId);
    o->setProperty(apif::VersionSyncRequest::nodes, nodes.isArray() ? nodes : juce::var(juce::Array<juce::var>()));
    o->setProperty(apif::VersionSyncRequest::branch, activeBranch);
    return postJson("/versions/sync", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::updateRenderVersion(const juce::String& versionId,
                                                                              const juce::var& body)
{
    const auto text = juce::JSON::toString(body, true);
    return sendRequest("PATCH", "/versions/" + escaped(versionId), &text, objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::createRenderBranch(const juce::String& sessionId,
                                                                             const juce::String& name,
                                                                             const juce::String& baseVersionId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::BranchCreateRequest::session_id, sessionId);
    o->setProperty(apif::BranchCreateRequest::name, name);
    o->setProperty(apif::BranchCreateRequest::base_version_id, baseVersionId);
    return postJson("/versions/branches", juce::var(o.get()), objectResponse());
}

khensu::Result<juce::var, BridgeError> CollabApi::postCollab(const std::string& route, const juce::var& body,
                                                                     int timeoutMs)
{
    const int ms = timeoutMs > 0 ? timeoutMs : kCollabHeavyMs;
    return postJson(juce::String(route), body, objectResponse(heavyTimeoutMs(ms, timeout_ms_)));
}

khensu::Result<juce::var, BridgeError> CollabApi::getCollab(const juce::String& pathAndQuery)
{
    return getJson(pathAndQuery, objectResponse());
}

bool CollabApi::deleteReferenceProfile(const juce::String& profileId)
{
    return sendDelete("/references/profiles/" + escaped(profileId));
}

} // namespace khensu::ipc
