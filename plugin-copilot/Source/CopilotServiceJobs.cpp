#include "CopilotServiceJobs.h"
#include "CopilotServiceClient.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu::copilot {

namespace af = khensu::api::fields;

namespace {

// Each blocking request bounds how long a cancel can go unnoticed, so keep
// them short (ServiceClient::kJobRequestMs); the job itself may run for
// minutes on the service.
constexpr int kIntakeDeadlineMs = 10 * 60 * 1000;
constexpr int kLlmDeadlineMs = 5 * 60 * 1000;

ipc::HttpServiceBridge makeBridge()
{
    return ServiceClient::bridge(ServiceClient::kJobRequestMs);
}

ipc::BridgeError waitError(JobWait wait, const std::string& what)
{
    switch (wait) {
    case JobWait::Cancelled: return {ipc::BridgeError::Kind::ServiceUnavailable, what + " cancelled"};
    case JobWait::TimedOut: return {ipc::BridgeError::Kind::Timeout, what + " timed out"};
    case JobWait::Failed:
    case JobWait::Finished:
    default: return {ipc::BridgeError::Kind::ServiceUnavailable, "Lost contact with the service during " + what};
    }
}

} // namespace

bool currentBackgroundJobShouldExit() noexcept
{
    if (auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob()) return job->shouldExit();
    return juce::Thread::currentThreadShouldExit();
}

bool runFullIntakeCancellable(const juce::String& filePath, const ShouldCancel& shouldCancel)
{
    auto bridge = makeBridge();
    const auto started = bridge.startFullIntake(filePath);
    const auto jobId = started[af::FullIntakeJobResponse::job_id].toString();
    if (jobId.isEmpty()) return false;

    bool completed = false;
    const auto wait = waitForServiceJob(
        [&bridge, &jobId, &completed]() -> std::optional<bool> {
            const auto status = bridge.fullIntakeStatus(jobId);
            if (!status.isObject()) return std::nullopt;
            const auto state = status[af::FullIntakeJobResponse::state].toString();
            if (state == khensu::api::job_status::completed) {
                completed = true;
                return true;
            }
            // Any other terminal state ends the wait without a result.
            return khensu::api::isTerminalStatus(state);
        },
        shouldCancel, kIntakeDeadlineMs);
    // Every outcome other than Finished leaves the server job running (PLG-16):
    // Failed (lost contact) included, so cancel it too. A cancel for a job
    // that already ended is a harmless no-op on the service.
    if (needsServerCancel(wait)) bridge.cancelFullIntake(jobId);
    return wait == JobWait::Finished && completed;
}

khensu::Result<ipc::HttpServiceBridge::CopilotChatResponse, ipc::BridgeError>
copilotChatCancellable(const juce::String& message, const juce::var& history, const juce::var& analysisResult,
                       double bpm, const juce::String& personality, const ShouldCancel& shouldCancel)
{
    using Response = ipc::HttpServiceBridge::CopilotChatResponse;
    using ChatResult = khensu::Result<Response, ipc::BridgeError>;

    auto bridge = makeBridge();
    const auto started = bridge.startChatJob(message, history, analysisResult, "", bpm, {}, "", personality);
    if (started.is_err()) return ChatResult::err(started.error());
    const auto jobId = started.value();

    std::optional<ChatResult> outcome;
    const auto wait = waitForServiceJob(
        [&bridge, &jobId, &outcome]() -> std::optional<bool> {
            auto status = bridge.getChatJobStatus(jobId);
            if (status.is_err()) return std::nullopt;
            const auto& s = status.value();
            if (s.status == khensu::api::job_status::completed) {
                if (s.result.has_value())
                    outcome = ChatResult::ok(*s.result);
                else
                    outcome = ChatResult::err({ipc::BridgeError::Kind::ParseError, "Chat job finished without a reply"});
                return true;
            }
            if (khensu::api::isTerminalStatus(s.status)) { // failed / cancelled / interrupted
                outcome = ChatResult::err({ipc::BridgeError::Kind::HttpError,
                                           "Chat " + s.status.toStdString()
                                               + (s.error.isNotEmpty() ? ": " + s.error.toStdString() : "")});
                return true;
            }
            return false;
        },
        shouldCancel, kLlmDeadlineMs);

    if (wait == JobWait::Finished && outcome.has_value()) return *outcome;
    if (needsServerCancel(wait)) bridge.cancelChatJob(jobId); // PLG-16: every non-Finished outcome
    return ChatResult::err(waitError(wait, "chat request"));
}

khensu::Result<ipc::HttpServiceBridge::MixerChainResponse, ipc::BridgeError>
generateMixerChainCancellable(const juce::String& targetChannel, const juce::String& genre, const juce::String& energy,
                              const juce::String& mood, double bpm, const juce::String& styleDescription,
                              const juce::StringArray& enabledEcosystems, const ShouldCancel& shouldCancel)
{
    using Response = ipc::HttpServiceBridge::MixerChainResponse;
    using ChainResult = khensu::Result<Response, ipc::BridgeError>;

    auto bridge = makeBridge();
    const auto started =
        bridge.startMixerChainJob(targetChannel, genre, energy, mood, bpm, styleDescription, enabledEcosystems);
    if (started.is_err()) return ChainResult::err(started.error());
    const auto jobId = started.value();

    std::optional<ChainResult> outcome;
    const auto wait = waitForServiceJob(
        [&bridge, &jobId, &outcome]() -> std::optional<bool> {
            auto status = bridge.getMixerChainJobStatus(jobId);
            if (status.is_err()) return std::nullopt;
            const auto& s = status.value();
            if (s.status == khensu::api::job_status::completed) {
                if (s.result.has_value())
                    outcome = ChainResult::ok(*s.result);
                else
                    outcome = ChainResult::err(
                        {ipc::BridgeError::Kind::ParseError, "Mixer-chain job finished without a result"});
                return true;
            }
            if (khensu::api::isTerminalStatus(s.status)) { // failed / cancelled / interrupted
                outcome = ChainResult::err({ipc::BridgeError::Kind::HttpError,
                                            "Mixer chain " + s.status.toStdString()
                                                + (s.error.isNotEmpty() ? ": " + s.error.toStdString() : "")});
                return true;
            }
            return false;
        },
        shouldCancel, kLlmDeadlineMs);

    if (wait == JobWait::Finished && outcome.has_value()) return *outcome;
    if (needsServerCancel(wait)) bridge.cancelMixerChainJob(jobId); // PLG-16: every non-Finished outcome
    return ChainResult::err(waitError(wait, "mixer-chain generation"));
}

} // namespace khensu::copilot
