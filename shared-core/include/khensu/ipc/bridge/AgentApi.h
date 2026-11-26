#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Command agent (CE-AG-1/2/4) (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Agent.cpp. Usable on its own (`AgentApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class AgentApi : public virtual BridgeTransport {
  public:
    AgentApi() = default;
    explicit AgentApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                      int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // CE-AG-1 / CE-AG-2 / CE-AG-4 (assistant) - HttpServiceBridge_Agent.cpp.
    // Parsed JSON; read through api::fields (AgentMixStatusResponse,
    // ProvidersResponse, CommandParseResponse, CommandJobStatusResponse).
    struct AgentMixOptions {
        juce::String filePath;
        juce::String sessionId;
        juce::String outputDir; ///< empty: "<source>_automix" beside the source
        double targetLufs = -14.0;
        int maxSteps = 6;
        bool includeLoudness = true;
        juce::var findings; ///< optional array of AgentFindingIn objects
    };
    /// POST /agent/mix -> job id (the verified auto-mix agent).
    khensu::Result<juce::String, BridgeError> startAgentMixJob(const AgentMixOptions& options);
    /// GET /agent/mix/{id}: status, live progress (step log) and the result.
    khensu::Result<juce::var, BridgeError> getAgentMixJobStatus(const juce::String& jobId);
    /// GET /agent/sessions?limit=N (AUTO-5): the sessions in khensu.db, most
    /// recently updated first, each with its file paths (AgentSessionsResponse).
    khensu::Result<juce::var, BridgeError> listAgentSessions(int limit = 100);
    /// POST /agent/findings (AUTO-5): the measured findings of `filePath`
    /// (or, when it is empty, of `sessionId`'s first file) against the
    /// default ranges (AgentFindingsResponse). Decodes and measures the file.
    khensu::Result<juce::var, BridgeError> agentFindings(const juce::String& filePath,
                                                         const juce::String& sessionId = {});
    void cancelAgentMixJob(const juce::String& jobId);
    /// GET /copilot/providers: configured choice, detected local servers,
    /// GGUF status, cloud availability and the provider in use per purpose.
    khensu::Result<juce::var, BridgeError> getLlmProviders(bool refresh);
    /// POST /command/parse: a free-text instruction -> previewable plan.
    khensu::Result<juce::var, BridgeError> parseCommand(const juce::String& text, const juce::String& filePath,
                                                        const std::map<juce::String, juce::String>& stemPaths,
                                                        bool allowLlm = true);
    /// POST /command/execute -> job id rendering a previewed plan.
    khensu::Result<juce::String, BridgeError> startCommandExecute(const juce::String& planId,
                                                                  const juce::String& sessionId);
    khensu::Result<juce::var, BridgeError> getCommandExecuteStatus(const juce::String& jobId);
    void cancelCommandExecute(const juce::String& jobId);
    // CE-AG-4 push-to-talk - HttpServiceBridge_Agent.cpp.
    /// POST /command/voice: transcribe a recorded WAV (faster-whisper, the
    /// `[asr]` extra; 503 without it) and parse it like /command/parse. The
    /// VoiceCommandResponse is a CommandParseResponse plus `transcript`.
    khensu::Result<juce::var, BridgeError> voiceCommand(const juce::String& audioPath, const juce::String& filePath,
                                                        const std::map<juce::String, juce::String>& stemPaths,
                                                        bool allowLlm = true);
};

} // namespace khensu::ipc
