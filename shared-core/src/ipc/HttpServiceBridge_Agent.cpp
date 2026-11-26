// shared-core/src/ipc/HttpServiceBridge_Agent.cpp
// CE-AG-1 / CE-AG-2 / CE-AG-4: the verified auto-mix agent (/agent/mix*), the
// LLM provider detection report (/copilot/providers) and natural-language
// mix commands (/command/*). Results are the parsed JSON; the panels read
// them through the generated api::fields names.
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {
/// A synchronous route that may run DSP (section analysis) or a local LLM:
/// minutes, not the 30 s default, and the FastAPI detail kept for the UI.
BridgeRequestOptions slowRouteOptions(int bridgeTimeoutMs)
{
    BridgeRequestOptions o;
    o.timeoutMs = heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, bridgeTimeoutMs);
    o.readDeadlineMs = o.timeoutMs;
    o.requireObject = true;
    o.errorDetail = BridgeRequestOptions::ErrorDetail::Replace;
    return o;
}

BridgeRequestOptions detailOptions()
{
    BridgeRequestOptions o;
    o.requireObject = true;
    o.errorDetail = BridgeRequestOptions::ErrorDetail::Replace;
    return o;
}
} // namespace

khensu::Result<juce::String, BridgeError> AgentApi::startAgentMixJob(const AgentMixOptions& options)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::AgentMixRequest::file_path, options.filePath);
    if (options.sessionId.isNotEmpty()) o->setProperty(apif::AgentMixRequest::session_id, options.sessionId);
    if (options.outputDir.isNotEmpty()) o->setProperty(apif::AgentMixRequest::output_dir, options.outputDir);
    o->setProperty(apif::AgentMixRequest::target_lufs, options.targetLufs);
    o->setProperty(apif::AgentMixRequest::max_steps, options.maxSteps);
    o->setProperty(apif::AgentMixRequest::include_loudness, options.includeLoudness);
    if (!options.findings.isVoid()) o->setProperty(apif::AgentMixRequest::findings, options.findings);
    return extractJobId(postJson("/agent/mix", juce::var(o.get()), detailOptions()));
}

khensu::Result<juce::var, BridgeError> AgentApi::getAgentMixJobStatus(const juce::String& jobId)
{
    return getJson("/agent/mix/" + jobId, pollOptions());
}

void AgentApi::cancelAgentMixJob(const juce::String& jobId)
{
    postIgnoringResult("/agent/mix/" + jobId + "/cancel");
}

khensu::Result<juce::var, BridgeError> AgentApi::listAgentSessions(int limit)
{
    return getJson(juce::String(khensu::api::routes::get_agent_sessions.path) + "?limit=" +
                       juce::String(juce::jlimit(1, 500, limit)),
                   detailOptions());
}

khensu::Result<juce::var, BridgeError> AgentApi::agentFindings(const juce::String& filePath,
                                                               const juce::String& sessionId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    if (filePath.isNotEmpty()) o->setProperty(apif::AgentFindingsRequest::file_path, filePath);
    if (sessionId.isNotEmpty()) o->setProperty(apif::AgentFindingsRequest::session_id, sessionId);
    // Measuring decodes the whole file: give it a render's time, not the default.
    auto opts = detailOptions();
    opts.timeoutMs = heavyTimeoutMs(heavy_timeout::kStemAnalysisMs, timeout_ms_);
    opts.readDeadlineMs = opts.timeoutMs;
    return postJson(khensu::api::routes::post_agent_findings.path, juce::var(o.get()), opts);
}

khensu::Result<juce::var, BridgeError> AgentApi::getLlmProviders(bool refresh)
{
    // Probing three localhost ports takes about a second when none answer.
    auto opts = objectResponse(15000);
    opts.readDeadlineMs = 15000;
    return getJson(refresh ? juce::String("/copilot/providers?refresh=true") : juce::String("/copilot/providers"),
                   opts);
}

khensu::Result<juce::var, BridgeError>
AgentApi::parseCommand(const juce::String& text, const juce::String& filePath,
                                const std::map<juce::String, juce::String>& stemPaths, bool allowLlm)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CommandParseRequest::text, text);
    if (filePath.isNotEmpty()) o->setProperty(apif::CommandParseRequest::file_path, filePath);
    if (!stemPaths.empty()) {
        juce::DynamicObject::Ptr stems = new juce::DynamicObject();
        for (const auto& [name, path] : stemPaths)
            if (path.isNotEmpty()) stems->setProperty(juce::Identifier(name), path);
        o->setProperty(apif::CommandParseRequest::stem_paths, juce::var(stems.get()));
    }
    o->setProperty(apif::CommandParseRequest::allow_llm, allowLlm);
    return postJson("/command/parse", juce::var(o.get()), slowRouteOptions(timeout_ms_));
}

// CE-AG-4 push-to-talk
khensu::Result<juce::var, BridgeError>
AgentApi::voiceCommand(const juce::String& audioPath, const juce::String& filePath,
                                const std::map<juce::String, juce::String>& stemPaths, bool allowLlm)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::VoiceCommandRequest::audio_path, audioPath);
    if (filePath.isNotEmpty()) o->setProperty(apif::VoiceCommandRequest::file_path, filePath);
    if (!stemPaths.empty()) {
        juce::DynamicObject::Ptr stems = new juce::DynamicObject();
        for (const auto& [name, path] : stemPaths)
            if (path.isNotEmpty()) stems->setProperty(juce::Identifier(name), path);
        o->setProperty(apif::VoiceCommandRequest::stem_paths, juce::var(stems.get()));
    }
    o->setProperty(apif::VoiceCommandRequest::allow_llm, allowLlm);
    // The first call loads the Whisper model: the slow-route deadline.
    return postJson("/command/voice", juce::var(o.get()), slowRouteOptions(timeout_ms_));
}

khensu::Result<juce::String, BridgeError> AgentApi::startCommandExecute(const juce::String& planId,
                                                                                 const juce::String& sessionId)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::CommandExecuteRequest::plan_id, planId);
    if (sessionId.isNotEmpty()) o->setProperty(apif::CommandExecuteRequest::session_id, sessionId);
    return extractJobId(postJson("/command/execute", juce::var(o.get()), detailOptions()));
}

khensu::Result<juce::var, BridgeError> AgentApi::getCommandExecuteStatus(const juce::String& jobId)
{
    return getJson("/command/execute/" + jobId, pollOptions());
}

void AgentApi::cancelCommandExecute(const juce::String& jobId)
{
    postIgnoringResult("/command/execute/" + jobId + "/cancel");
}

} // namespace khensu::ipc
