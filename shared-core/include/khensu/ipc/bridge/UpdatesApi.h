#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Release-availability check (GET /updates/check) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Updates.cpp. Usable on its own (`UpdatesApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class UpdatesApi : public virtual BridgeTransport {
  public:
    UpdatesApi() = default;
    explicit UpdatesApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                        int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // ---- Release-availability check (GET /updates/check) ----
    // Online-gated and throttled (an hour) by the service, unless forced -
    // the manual "Check for updates"; it only reads a release manifest, never
    // downloads anything. status is one of
    // "disabled", "update_available", "up_to_date", "unavailable".
    struct UpdateCheckResult {
        juce::String status;
        juce::String currentVersion;
        juce::String latestVersion; ///< empty unless the manifest was read
        juce::String url;           ///< https release page, or empty
        juce::String downloadUrl;   ///< https link to the release's installer for this OS, or empty
        juce::String downloadState; ///< none | downloading | ready | failed (background download)
        juce::String installerPath; ///< the downloaded, SHA-256 verified installer once ready
        bool installerReady() const { return downloadState == "ready" && installerPath.isNotEmpty(); }
        bool fromCache = false;
        bool updateAvailable() const { return status == "update_available"; }
    };
    khensu::Result<UpdateCheckResult, BridgeError> checkForUpdates(const juce::String& currentVersion,
                                                                   bool force = false);
};

} // namespace khensu::ipc
