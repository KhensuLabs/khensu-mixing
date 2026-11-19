// shared-core/src/ipc/HttpServiceBridge_Multitrack.cpp
// Multitrack (stem-folder) session endpoints: /analyze/multitrack/*.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

khensu::Result<juce::var, BridgeError> MultitrackApi::scanMultitrackFolder(const juce::String& folder)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::MultitrackScanRequest::folder, folder);
    return postJsonExpectJson("/analyze/multitrack/scan", juce::var(o.get()));
}

khensu::Result<juce::String, BridgeError> MultitrackApi::startMultitrackJob(const juce::String& folder,
                                                                                const juce::StringArray& trackNames,
                                                                                bool writeMixdown)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::MultitrackStartRequest::folder, folder);
    if (!trackNames.isEmpty()) {
        juce::Array<juce::var> names;
        for (const auto& n : trackNames)
            names.add(n);
        o->setProperty(apif::MultitrackStartRequest::track_names, names);
    }
    o->setProperty(apif::MultitrackStartRequest::write_mixdown, writeMixdown);
    return extractJobId(postJson("/analyze/multitrack/start", juce::var(o.get())));
}

khensu::Result<juce::var, BridgeError> MultitrackApi::getMultitrackJobStatus(const juce::String& jobId)
{
    return getJson("/analyze/multitrack/status/" + jobId, pollOptions());
}

void MultitrackApi::cancelMultitrackJob(const juce::String& jobId)
{
    postIgnoringResult("/analyze/multitrack/" + jobId + "/cancel");
}

} // namespace khensu::ipc
