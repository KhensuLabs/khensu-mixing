#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Multitrack sessions (a folder of bounced stems) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Multitrack.cpp. Usable on its own (`MultitrackApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class MultitrackApi : public virtual BridgeTransport {
  public:
    MultitrackApi() = default;
    explicit MultitrackApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                           int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // ---- Multitrack sessions (folder of bounced stems) ----
    // /analyze/multitrack/*: scan lists, names and validates a folder without
    // decoding it; the job analyzes every stem, scores track-vs-track masking
    // and writes the summed mixdown. Results are the parsed JSON (see
    // ui/SessionIntake/MultitrackModel.h for the typed view of them).
    khensu::Result<juce::var, BridgeError> scanMultitrackFolder(const juce::String& folder);
    khensu::Result<juce::String, BridgeError>
    startMultitrackJob(const juce::String& folder, const juce::StringArray& trackNames = {}, bool writeMixdown = true);
    khensu::Result<juce::var, BridgeError> getMultitrackJobStatus(const juce::String& jobId);
    void cancelMultitrackJob(const juce::String& jobId);
};

} // namespace khensu::ipc
