// desktop-app/Source/services/IServiceLifecycle.h
#pragma once
#include <JuceHeader.h>
#include <optional>
#include <functional>
#include <map>
#include <vector>
#include "../ui/shared/PythonStatus.h"

namespace khensu {

namespace ipc {
struct GenreReferencePoint;
}

/// The local Python service and the Copilot plugin link (ENG-1): restart and
/// diagnostics, the provenance outbox, plugin parameter push / fetch and the
/// reference library fetch. Message thread; callbacks on the message thread.
class IServiceLifecycle {
  public:
    virtual ~IServiceLifecycle() = default;

    /// What the sidebar shows for the service.
    virtual PythonStatus getPythonServiceStatus() const noexcept = 0;
    /// Off the message thread; a second request while one runs is dropped.
    virtual void restartPythonService() = 0;
    virtual void copyServiceLogsToClipboard() = 0;
    virtual void diagnoseServiceStatus() = 0;

    virtual void dispatchOutboxIfNeeded() = 0;
    virtual int undeliveredOutboxCount() const noexcept = 0;

    // ---- Copilot plugin ----
    virtual void pushPluginParams(const std::map<juce::String, float>& params) = 0;
    virtual void fetchPluginParams(std::function<void(const std::map<juce::String, float>&)> onComplete) = 0;

    // ---- The plugin chain (WF-9): one model, one editor ----
    // The plugin's built-in DSP parameters are the one chain model; the
    // Signal Flow Rack (Mix / Master) is its one desktop editor. Other
    // surfaces (Mixer Chain, onboarding) stage a chain here instead of
    // pushing their own; the editor shows it and applies or discards it.
    struct StagedPluginChain {
        std::map<juce::String, float> params;
        juce::String source; ///< who proposed it ("Mixer Chain", "Onboarding")
    };
    virtual void stagePluginChain(const std::map<juce::String, float>& params, const juce::String& source) = 0;
    /// The staged chain, if any.
    virtual std::optional<StagedPluginChain> stagedPluginChain() const = 0;
    /// Sends the staged chain to the target instance(s) and clears it.
    virtual void applyStagedPluginChain() = 0;
    virtual void discardStagedPluginChain() = 0;
    /// A chain was staged, applied or discarded.
    virtual void addPluginChainListener(void* key, std::function<void()> cb) = 0;
    virtual void removePluginChainListener(void* key) = 0;
    /// "" = every instance (PLG-04).
    virtual juce::String pluginTargetInstance() const = 0;
    virtual void setPluginTargetInstance(const juce::String& instanceId) = 0;
    virtual const std::vector<juce::String>& knownPluginInstances() const noexcept = 0;

    /// Reference-library sample points for one genre (empty on any error).
    virtual void getReferenceLibraryPoints(const juce::String& genreId,
                                           std::function<void(std::vector<ipc::GenreReferencePoint>)> onComplete) = 0;
};

} // namespace khensu
