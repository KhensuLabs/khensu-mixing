#pragma once

#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/ServiceHealthTracker.h>
#include <atomic>
#include <functional>
#include <memory>

namespace khensu {

class ITransportBridge {
  public:
    using PlayheadState = ipc::HttpServiceBridge::PlayheadState;
    using SeekState = ipc::HttpServiceBridge::SeekState;
    using PluginParams = ipc::HttpServiceBridge::PluginParams;
    using SeekResult = khensu::Result<SeekState, ipc::BridgeError>;
    using PluginParamsResult = khensu::Result<PluginParams, ipc::BridgeError>;

    virtual ~ITransportBridge() = default;
    /// `state.instance_id` names the posting instance (PLG-04).
    virtual bool postPlayhead(const PlayheadState& state) = 0;
    /// `sinceGeneration` is the last seek generation this instance applied
    /// (0 before the first poll); with it the service never consumes a seek.
    virtual SeekResult pollSeek(int sinceGeneration) = 0;
    /// Returns params only when the push was untargeted or targeted here.
    virtual PluginParamsResult pollPluginParams(const juce::String& instanceId) = 0;
};

/// Owns the desktop/plugin transport polling lifecycle. Processor audio state
/// stays in the processor; this worker receives a snapshot provider and sends
/// updates back through narrow sinks, so the bridge/thread policy is isolated.
class CopilotTransportController final : public juce::Thread {
  public:
    using PlayheadProvider = std::function<ipc::HttpServiceBridge::PlayheadState()>;
    using PreparedProvider = std::function<bool()>;
    using SeekSink = std::function<void(float)>;
    using ParameterSink = std::function<void(const juce::String&, float)>;
    using BridgeFactory = std::function<std::unique_ptr<ITransportBridge>()>;

    CopilotTransportController(PreparedProvider preparedProvider, PlayheadProvider playheadProvider,
                                SeekSink seekSink, ParameterSink parameterSink, BridgeFactory bridgeFactory = {});
    ~CopilotTransportController() override;

    /// Executes one transport poll without waiting or touching the network
    /// itself. The worker calls this with its injected bridge; tests can call it
    /// with a fake bridge to cover health and sink behavior deterministically.
    void pollOnce(ITransportBridge& bridge);

    ipc::ServiceHealthStatus serviceHealthStatus() const noexcept
    {
        return serviceHealthStatus_.load(std::memory_order_relaxed);
    }
    int serviceHealthConsecutiveFailures() const noexcept
    {
        return serviceHealthFailures_.load(std::memory_order_relaxed);
    }
    float requestedSeekSec() const noexcept { return requestedSeekSec_.load(std::memory_order_relaxed); }

    /// The stable per-instance id every transport route is keyed by (PLG-04).
    /// Any thread; the worker reads it on each tick, so a restored id takes
    /// effect on the next poll.
    void setInstanceId(const juce::String& instanceId);
    juce::String instanceId() const;

  private:
    void run() override;

    PreparedProvider preparedProvider_;
    PlayheadProvider playheadProvider_;
    SeekSink seekSink_;
    ParameterSink parameterSink_;
    BridgeFactory bridgeFactory_;
    ipc::ServiceHealthTracker healthTracker_;
    std::atomic<int> lastParamGeneration_{0};
    bool paramBaselineSet_ = false; ///< worker-only: first param poll is baseline-only
    int lastSeekGeneration_ = -1;   ///< worker-only: last seek generation seen
    bool seekBaselineSet_ = false;  ///< worker-only: first seek poll is baseline-only
    mutable juce::CriticalSection instanceIdLock_;
    juce::String instanceId_;
    std::atomic<ipc::ServiceHealthStatus> serviceHealthStatus_{ipc::ServiceHealthStatus::NeverConnected};
    std::atomic<int> serviceHealthFailures_{0};
    std::atomic<float> requestedSeekSec_{0.0f};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotTransportController)
};

} // namespace khensu
