#pragma once

#include <JuceHeader.h>
#include <khensu/common/Result.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <functional>
#include <optional>

namespace khensu::copilot {

/// Cancellable long-running service calls for the plugin's background pool
/// (audit H3). The blocking variants (runFullIntake ~60 s, copilotChat and
/// generateMixerChain = a full LLM round trip) ignored ThreadPoolJob::
/// shouldExit(), so the processor's bounded removeAllJobs() wait expired and
/// ~ThreadPool fell through to killThread() inside the host. These start the
/// service-side job, poll it with short per-request timeouts, and cancel it
/// (server-side too) as soon as `shouldCancel` fires.

using ShouldCancel = std::function<bool()>;

/// True when the job running on this thread was asked to stop: the
/// processor's ThreadPool signals shouldExit() from its destructor (plugin
/// unload). Falls back to a juce::Thread's exit flag; false elsewhere.
bool currentBackgroundJobShouldExit() noexcept;

enum class JobWait { Finished, Cancelled, TimedOut, Failed };

/// The server-side job must be cancelled for EVERY wait outcome except
/// Finished (PLG-16): a Failed wait (lost contact) used to return with the
/// job still running on the service.
constexpr bool needsServerCancel(JobWait wait) noexcept { return wait != JobWait::Finished; }

/// Polls a service job. `poll` returns std::nullopt for a transient failure,
/// true once the job is terminal, false while it is still running. Sleeps
/// between polls in short slices so a cancel is seen within ~25 ms.
template <typename PollFn>
JobWait waitForServiceJob(PollFn&& poll, const ShouldCancel& shouldCancel, int deadlineMs, int pollIntervalMs = 250,
                          int maxConsecutiveFailures = 20)
{
    const auto cancelled = [&shouldCancel] { return shouldCancel && shouldCancel(); };
    const juce::uint32 startMs = juce::Time::getMillisecondCounter();
    const auto elapsed = [startMs] { return static_cast<int>(juce::Time::getMillisecondCounter() - startMs); };
    int failures = 0;
    for (;;) {
        if (cancelled()) return JobWait::Cancelled;
        if (elapsed() > deadlineMs) return JobWait::TimedOut;

        for (int slept = 0; slept < pollIntervalMs; slept += 25) {
            juce::Thread::sleep(25);
            if (cancelled()) return JobWait::Cancelled;
        }

        const std::optional<bool> terminal = poll();
        if (!terminal.has_value()) {
            if (++failures >= maxConsecutiveFailures) return JobWait::Failed;
            continue;
        }
        failures = 0;
        if (*terminal) return JobWait::Finished;
    }
}

/// /analyze/full_intake as a cancellable job. True when the intake completed.
bool runFullIntakeCancellable(const juce::String& filePath, const ShouldCancel& shouldCancel);

/// /copilot/chat as a cancellable job (same request body as copilotChat).
khensu::Result<ipc::HttpServiceBridge::CopilotChatResponse, ipc::BridgeError>
copilotChatCancellable(const juce::String& message, const juce::var& history, const juce::var& analysisResult,
                       double bpm, const juce::String& personality, const ShouldCancel& shouldCancel);

/// /copilot/generate_mixer_chain as a cancellable job.
khensu::Result<ipc::HttpServiceBridge::MixerChainResponse, ipc::BridgeError>
generateMixerChainCancellable(const juce::String& targetChannel, const juce::String& genre, const juce::String& energy,
                              const juce::String& mood, double bpm, const juce::String& styleDescription,
                              const juce::StringArray& enabledEcosystems, const ShouldCancel& shouldCancel);

} // namespace khensu::copilot
