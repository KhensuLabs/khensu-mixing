#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

khensu::Result<HttpServiceBridge::UpdateCheckResult, BridgeError>
UpdatesApi::checkForUpdates(const juce::String& currentVersion, bool force)
{
    // The service may make one network round trip (6 s fetch timeout).
    const auto query = "/updates/check?current_version=" + juce::URL::addEscapeChars(currentVersion, true) +
                       (force ? "&force=true" : "");
    const auto resp = getJson(query, objectResponse(10000));
    if (resp.is_err()) return forwardError<UpdateCheckResult>(resp);
    auto* obj = resp.value().getDynamicObject();
    if (!obj || !obj->hasProperty(apif::UpdateCheckResponse::status))
        return shapeError<UpdateCheckResult>("Invalid JSON from /updates/check");

    UpdateCheckResult r;
    r.status = obj->getProperty(apif::UpdateCheckResponse::status).toString();
    r.currentVersion = obj->getProperty(apif::UpdateCheckResponse::current_version).toString();
    const auto latest = obj->getProperty(apif::UpdateCheckResponse::latest_version);
    if (!latest.isVoid()) r.latestVersion = latest.toString();
    const auto url = obj->getProperty(apif::UpdateCheckResponse::url);
    if (!url.isVoid()) r.url = url.toString();
    const auto download = obj->getProperty(apif::UpdateCheckResponse::download_url);
    if (!download.isVoid()) r.downloadUrl = download.toString();
    const auto state = obj->getProperty(apif::UpdateCheckResponse::download_state);
    r.downloadState = state.isVoid() ? juce::String("none") : state.toString();
    const auto installer = obj->getProperty(apif::UpdateCheckResponse::installer_path);
    if (!installer.isVoid()) r.installerPath = installer.toString();
    r.fromCache = static_cast<bool>(obj->getProperty(apif::UpdateCheckResponse::from_cache));
    return khensu::Result<UpdateCheckResult, BridgeError>::ok(r);
}

} // namespace khensu::ipc
