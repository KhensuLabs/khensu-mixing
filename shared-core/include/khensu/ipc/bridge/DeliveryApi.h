#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Delivery & QA toolset (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Delivery.cpp. Usable on its own (`DeliveryApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class DeliveryApi : public virtual BridgeTransport {
  public:
    DeliveryApi() = default;
    explicit DeliveryApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                         int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // ── Broadcast / podcast delivery compliance ─────────────────────────
    // (HttpServiceBridge_Delivery.cpp)
    struct DeliveryCheck {
        std::string id;     // integrated | true_peak | loudness_range | short_term_max | channels
        std::string status; // pass | warn | fail
        std::optional<double> measured, target, delta;
        std::string note, fix;
    };
    struct DeliveryTargetResult {
        std::string targetId, name, category;
        std::string measurement; // as_delivered | mono
        std::string overall;     // pass | warn | fail
        std::vector<DeliveryCheck> checks;
    };
    struct DeliveryComplianceResult {
        int channels = 0;
        std::vector<DeliveryTargetResult> results;
    };
    /// POST /analyze/delivery_compliance - grade the file against delivery
    /// loudness targets (ids from getExportTargets(); empty = every broadcast
    /// and podcast target). A mono target is metered on the mono fold-down.
    khensu::Result<DeliveryComplianceResult, BridgeError>
    analyzeDeliveryCompliance(const std::string& file_path, const std::vector<std::string>& target_ids = {});
};

} // namespace khensu::ipc
