#pragma once
#include <khensu/ipc/BridgeTransport.h>
#include <khensu/ipc/bridge/AgentApi.h>
#include <khensu/ipc/bridge/AlbumApi.h>
#include <khensu/ipc/bridge/AnalysisApi.h>
#include <khensu/ipc/bridge/CeDeliveryApi.h>
#include <khensu/ipc/bridge/CollabApi.h>
#include <khensu/ipc/bridge/CopilotApi.h>
#include <khensu/ipc/bridge/DeliveryApi.h>
#include <khensu/ipc/bridge/IntelligenceApi.h>
#include <khensu/ipc/bridge/MlApi.h>
#include <khensu/ipc/bridge/MultitrackApi.h>
#include <khensu/ipc/bridge/PerformanceApi.h>
#include <khensu/ipc/bridge/UpdatesApi.h>

namespace khensu::ipc {

/// The whole service client (ENG-4): the ILocalServiceBridge seam, the
/// identity gate and every domain client (khensu/ipc/bridge/*Api.h) over one
/// BridgeTransport. A caller that needs one domain can include and construct
/// that client alone.
class HttpServiceBridge final : public ILocalServiceBridge,
                                public AgentApi,
                                public AlbumApi,
                                public AnalysisApi,
                                public CeDeliveryApi,
                                public CollabApi,
                                public CopilotApi,
                                public DeliveryApi,
                                public IntelligenceApi,
                                public MlApi,
                                public MultitrackApi,
                                public PerformanceApi,
                                public UpdatesApi {
  public:
    /// See BridgeTransport: `timeout_ms` is the default connect/receive
    /// timeout, and the address defaults to kDefaultBaseUrl.
    explicit HttpServiceBridge(std::string base_url = kDefaultBaseUrl, int timeout_ms = kDefaultTimeoutMs,
                               int retry_delay_ms = kDefaultRetryDelayMs);

    khensu::Result<DeepAnalysisResponse, BridgeError> analyze(const std::string& file_path, AnalysisMode mode) override;

    khensu::Result<khensu::SectionAnalysis, BridgeError> analyzeSections(const std::string& file_path) override;

    khensu::Result<khensu::ReferenceDeltas, BridgeError> analyzeReference(const std::string& target_path,
                                                                          const std::string& reference_path) override;

    khensu::Result<std::string, BridgeError> startAnalysisJob(const std::string& file_path, AnalysisMode mode) override;
    khensu::Result<AnalysisJobStatus, BridgeError> getAnalysisProgress(const std::string& job_id) override;
    void cancelAnalysisJob(const std::string& job_id) override;

    khensu::Result<std::string, BridgeError> startSeparationJob(const std::string& file_path,
                                                                const std::string& output_dir,
                                                                const std::string& method = "hpss") override;
    khensu::Result<SeparationJobStatus, BridgeError> getSeparationProgress(const std::string& job_id) override;
    void cancelSeparationJob(const std::string& job_id) override;

    khensu::Result<IntegrityResult, BridgeError> checkIntegrity(const std::string& file_path) override;

    khensu::Result<std::vector<ExportTarget>, BridgeError> getExportTargets() override;

    khensu::Result<std::vector<GenreReferencePoint>, BridgeError>
    getReferenceLibraryPoints(const std::string& genre_id) override;

    khensu::Result<StemMaskingResult, BridgeError> analyzeStemMasking(const std::string& vocals_path,
                                                                      const std::string& drums_path,
                                                                      const std::string& bass_path,
                                                                      const std::string& other_path) override;

    khensu::Result<SpectrotemporalResult, BridgeError>
    analyzeSpectrotemporal(const std::string& file_path, int n_bands = 24, int target_frames = 200) override;

    bool is_service_available() const noexcept override;

    /// Ask the locally-owned service to reject new work, mark active jobs
    /// interrupted, and exit after responding. Used by PythonServiceManager
    /// before it falls back to a hard child-process kill.
    khensu::Result<void, BridgeError> requestGracefulShutdown();

    void seekTransport(float position_sec) override;

    /// Stops and joins the process-wide background workers (the latest-wins
    /// seekTransport dispatcher). Call while JUCE is still initialised - the
    /// desktop app after its pools drain, the plugin when its last instance
    /// is destroyed - so no send runs during static destruction. A later
    /// seekTransport() lazily starts a fresh worker.
    static void shutdownBackgroundWorkers();

    /// Service identity gate (AUDIT-020), process-wide. While required and
    /// not verified, every request except GET /health fails fast with
    /// ServiceUnavailable instead of sending the bearer token (and file paths
    /// / chat) to whatever process answers on the port. The desktop app turns
    /// it on and PythonServiceManager flips "verified" after each HMAC
    /// challenge on /health. The VST3 plugin, which has no service manager,
    /// turns on self-verify as well (FIX-7). Off by default (tests).
    static void setServiceIdentityRequired(bool required) noexcept;
    static void setServiceIdentityVerified(bool verified) noexcept;
    static bool serviceIdentityVerified() noexcept;
    static bool serviceIdentityRequired() noexcept;

    /// Self-verify mode (FIX-7): while the gate is required, the bridge
    /// proves the listener itself - GET /health?nonce=<fresh hex> must come
    /// back with HMAC-SHA256(token, nonce) - before the first authenticated
    /// request and again once the last proof is kIdentityProofMaxAgeMs old.
    /// A listener that fails is never sent the token. For a process with no
    /// PythonServiceManager (the plugin); the desktop app leaves it off.
    static void setServiceIdentitySelfVerify(bool selfVerify) noexcept;
    static bool serviceIdentitySelfVerify() noexcept;
    static constexpr int kIdentityProofMaxAgeMs = 10000;
    /// How long a failed proof stands before the next request probes again.
    static constexpr int kIdentityRetryAfterFailureMs = 1000;


    // ---- Guardrail quarantine override (audit AI-01) ----
    /// POST /copilot/override_guardrail - promotes a guardrail-quarantined
    /// render to its intended output path anyway, on explicit expert
    /// authority. `expertNote` must be non-empty (the server enforces this;
    /// there is no silent-override path). Returns the error detail string on
    /// failure (quarantine file missing, path mismatch, etc.) so the caller
    /// can show it, rather than just a bool.
    khensu::Result<void, BridgeError> overrideGuardrail(const juce::String& sourcePath,
                                                        const juce::String& quarantinePath,
                                                        const juce::String& outputPath,
                                                        const juce::String& expertNote,
                                                        const juce::String& sessionId = {},
                                                        const juce::String& sourceRecommendationId = {},
                                                        const juce::String& decisionHash = {});

  private:
    khensu::Result<DeepAnalysisResponse, BridgeError> try_once(const std::string& file_path, AnalysisMode mode);
};

} // namespace khensu::ipc
