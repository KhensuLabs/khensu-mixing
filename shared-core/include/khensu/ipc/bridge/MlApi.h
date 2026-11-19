#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Learned models and embeddings (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Ml.cpp. Usable on its own (`MlApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class MlApi : public virtual BridgeTransport {
  public:
    MlApi() = default;
    explicit MlApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs, int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // CE-PLT-1 / CE-AI-2 / CE-AI-3 (ML and processing) - HttpServiceBridge_Ml.cpp.
    // Parsed JSON; read it through api::fields::AccelResponse,
    // TasteModelStatus, TasteRankResponse and FitChainResponse.
    /// GET /diagnostics/accel: accelerator preference, available providers /
    /// devices and the ones chosen.
    khensu::Result<juce::var, BridgeError> accelDiagnostics();
    /// FEAT-10: GET /components; POST /components/install/start {id};
    /// GET /components/install/status/{job_id}.
    khensu::Result<juce::var, BridgeError> listComponents();
    /// FEAT-4: POST /copilot/stem_rebalance/suggest and /copilot/stem_rebalance.
    khensu::Result<juce::var, BridgeError> stemRebalanceSuggest(const std::map<std::string, std::string>& stems,
                                                                const juce::String& genreId);
    khensu::Result<juce::var, BridgeError> stemRebalance(const std::map<std::string, std::string>& stems,
                                                         const std::map<std::string, double>& gainsDb,
                                                         const juce::String& outputPath,
                                                         const juce::String& originalMix = {});
    khensu::Result<juce::var, BridgeError> startComponentInstall(const juce::String& componentId);
    khensu::Result<juce::var, BridgeError> componentInstallStatus(const juce::String& jobId);
    /// The service's diagnostics for a support bundle (REF-9): queue depths
    /// and LLM spend, memory by cache and pool (PERF-7), the security-event feed, analysis-cache stats and the
    /// model registry, as (file name, JSON text). A call that failed gives an
    /// empty text, so a bundle still builds while the service is down.
    std::vector<std::pair<juce::String, juce::String>> diagnosticsSnapshots();
    /// GET /taste/model/status and POST /taste/model/train.
    khensu::Result<juce::var, BridgeError> tasteModelStatus();
    khensu::Result<juce::var, BridgeError> tasteModelTrain();
    /// POST /taste/model/rank: `items` is an array of {id, severity, stage,
    /// stem_or_bus, is_taste_decision}. Cold start returns the input order.
    khensu::Result<juce::var, BridgeError> tasteModelRank(const juce::var& items);
    /// POST /render/fit_chain: exactly one of referencePath / genre. Empty
    /// outputPath with render=true lets the service name the file.
    khensu::Result<juce::var, BridgeError> fitChain(const juce::String& filePath, const juce::String& referencePath,
                                                    const juce::String& genre, double amount, bool render,
                                                    const juce::String& outputPath = {});
};

} // namespace khensu::ipc
