#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Delivery extensions (CE-DEL-1..4) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_CeDelivery.cpp. Usable on its own (`CeDeliveryApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class CeDeliveryApi : public virtual BridgeTransport {
  public:
    CeDeliveryApi() = default;
    explicit CeDeliveryApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                           int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // CE-DEL-1..4 (delivery) - HttpServiceBridge_CeDelivery.cpp. Parsed JSON;
    // read it through api::fields::ImmersiveCheckResponse,
    // ProvenanceSignResponse / ProvenanceReadResponse, StreamPreviewResponse
    // and DeliveryPackageResult.
    /// POST /delivery/immersive_check (layout, multichannel loudness, fold-down
    /// and binaural previews, Atmos-music grading). `outputDir` empty = the
    /// service default beside the source.
    khensu::Result<juce::var, BridgeError> immersiveCheck(const juce::String& filePath,
                                                          const juce::String& outputDir = {});
    /// POST /provenance/sign. `action` created | edited; `aiUse` none |
    /// ai_assisted | ai_generated. The unsigned sidecar is always written.
    khensu::Result<juce::var, BridgeError> provenanceSign(const juce::String& filePath, const juce::String& action,
                                                          const juce::String& aiUse,
                                                          const juce::String& outputPath = {});
    /// POST /provenance/read: embedded manifest summary + sidecar.
    khensu::Result<juce::var, BridgeError> provenanceRead(const juce::String& filePath);
    /// POST /delivery/stream_preview. Empty codec / 0 bitrate = the
    /// platform's default; empty device = none.
    khensu::Result<juce::var, BridgeError> streamPreview(const juce::String& filePath, const juce::String& platform,
                                                         const juce::String& codec = {}, int bitrateKbps = 0,
                                                         const juce::String& device = {});
    /// GET /delivery/stream_preview/options: the platforms, codecs and devices
    /// the service supports, and whether ffmpeg is there for a real codec round trip.
    khensu::Result<juce::var, BridgeError> streamPreviewOptions();
    /// GET /delivery/immersive_targets: the immersive delivery targets (limits
    /// per standard) the immersive check knows.
    khensu::Result<juce::var, BridgeError> immersiveTargets();
    /// GET /delivery/profiles (distributor profiles).
    khensu::Result<juce::var, BridgeError> getDeliveryProfiles();
    struct DeliveryPackageOptions {
        juce::String filePath;
        juce::String outputDir; ///< empty = <source dir>/<name>_delivery
        juce::String profileId{"streaming_distributor"};
        juce::String stemsDir;   ///< a separation folder, or empty
        juce::StringArray stems; ///< extra stem files
        juce::String cleanVersionPath;
        juce::String admPath;
        juce::String aiUse{"none"};
        juce::String title, performer, isrc;
        bool sign = true;
    };
    struct DeliveryPackageJobStatus {
        juce::String status; ///< api::job_status values
        int total = 0;
        int done = 0;
        juce::String current; ///< the step running now (empty when idle/done)
        juce::String outputDir;
        juce::String error;
        juce::var result; ///< DeliveryPackageResult when completed
    };
    /// POST /delivery/package -> job id.
    khensu::Result<juce::String, BridgeError> startDeliveryPackageJob(const DeliveryPackageOptions& options);
    khensu::Result<DeliveryPackageJobStatus, BridgeError> getDeliveryPackageJobStatus(const juce::String& jobId);
    void cancelDeliveryPackageJob(const juce::String& jobId);
};

} // namespace khensu::ipc
