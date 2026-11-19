#include "CopilotTransportController.h"
#include "CopilotServiceClient.h"

namespace khensu {

namespace {

class HttpTransportBridgeAdapter final : public ITransportBridge {
  public:
    HttpTransportBridgeAdapter() : bridge_(copilot::ServiceClient::bridge(copilot::ServiceClient::kTransportRequestMs)) {}

    bool postPlayhead(const PlayheadState& state) override { return bridge_.postPlayhead(state); }
    SeekResult pollSeek(int sinceGeneration) override { return bridge_.pollSeek(sinceGeneration); }
    PluginParamsResult pollPluginParams(const juce::String& instanceId) override
    {
        return bridge_.pollPluginParams(instanceId);
    }

  private:
    ipc::HttpServiceBridge bridge_;
};

} // namespace

CopilotTransportController::CopilotTransportController(PreparedProvider preparedProvider,
                                                         PlayheadProvider playheadProvider, SeekSink seekSink,
                                                         ParameterSink parameterSink, BridgeFactory bridgeFactory)
    : juce::Thread("khensu-transport-sync"), preparedProvider_(std::move(preparedProvider)),
      playheadProvider_(std::move(playheadProvider)), seekSink_(std::move(seekSink)),
      parameterSink_(std::move(parameterSink)), bridgeFactory_(std::move(bridgeFactory))
{
    if (!bridgeFactory_) bridgeFactory_ = [] { return std::make_unique<HttpTransportBridgeAdapter>(); };
}

CopilotTransportController::~CopilotTransportController()
{
    stopThread(4000);
}

void CopilotTransportController::setInstanceId(const juce::String& instanceId)
{
    const juce::ScopedLock sl(instanceIdLock_);
    instanceId_ = instanceId;
}

juce::String CopilotTransportController::instanceId() const
{
    const juce::ScopedLock sl(instanceIdLock_);
    return instanceId_;
}

void CopilotTransportController::run()
{
    // One bridge instance for the worker lifetime means the auth token is read
    // once, not on every five-Hz tick.
    auto bridge = bridgeFactory_ ? bridgeFactory_() : nullptr;
    if (!bridge) return;

    while (!threadShouldExit()) {
        wait(200); // interrupted immediately by stopThread's notify
        if (threadShouldExit()) return;
        pollOnce(*bridge);
    }
}

void CopilotTransportController::pollOnce(ITransportBridge& bridge)
{
    // DAW plugin scans instantiate briefly and should not spam the service
    // before the processor has prepared a real audio path.
    if (!preparedProvider_ || !preparedProvider_()) return;

    const auto id = instanceId();
    auto state = playheadProvider_ ? playheadProvider_() : ITransportBridge::PlayheadState{};
    state.instance_id = id; // keyed per instance: several Copilots no longer share one slot (PLG-04)
    const bool ok = bridge.postPlayhead(state);
    serviceHealthStatus_.store(healthTracker_.recordTick(ok), std::memory_order_relaxed);
    serviceHealthFailures_.store(healthTracker_.consecutiveFailures(), std::memory_order_relaxed);

    if (threadShouldExit()) return;

    // VST3 offers no API to move the host transport, so the requested
    // position is surfaced to the editor through the sink. Seeks are no longer
    // consumed by the first poller (PLG-04): each instance remembers the last
    // generation it applied, so every instance sees every seek once.
    // since=0 on the first poll: a non-consuming read even then (omitting
    // "since" selects the legacy consume-on-read path).
    if (auto seek = bridge.pollSeek(seekBaselineSet_ ? juce::jmax(0, lastSeekGeneration_) : 0); seek.is_ok()) {
        const auto& value = seek.value();
        bool apply = false;
        if (value.generation < 0) {
            apply = value.pending; // legacy service: consume-on-read
        } else if (!seekBaselineSet_) {
            // A seek requested before this instance existed is history, not
            // a request to this instance (same baseline rule as AUDIT-003).
            seekBaselineSet_ = true;
            lastSeekGeneration_ = value.generation;
        } else if (value.generation > lastSeekGeneration_) {
            lastSeekGeneration_ = value.generation;
            apply = true;
        }
        if (apply) {
            requestedSeekSec_.store(value.position_sec, std::memory_order_relaxed);
            if (seekSink_) seekSink_(value.position_sec);
        }
    }

    if (threadShouldExit()) return;

    // Apply desktop-pushed DSP targets on this owned worker. This avoids a
    // queued callback outliving the processor and does not use gestures,
    // which are message-thread-only.
    if (auto params = bridge.pollPluginParams(id); params.is_ok()) {
        const int generation = params.value().generation;
        // AUDIT-003: the service keeps returning the last push for its whole
        // lifetime. The first successful poll only records that generation as
        // a baseline - applying it would overwrite the DAW-restored state of
        // every instance in a freshly opened project with one stale push.
        if (!paramBaselineSet_) {
            paramBaselineSet_ = true;
            lastParamGeneration_.store(generation, std::memory_order_relaxed);
            return;
        }
        if (generation != lastParamGeneration_.load(std::memory_order_relaxed) && !params.value().params.empty()) {
            lastParamGeneration_.store(generation, std::memory_order_relaxed);
            if (parameterSink_) {
                for (const auto& [name, value] : params.value().params) parameterSink_(name, value);
            }
        }
    }
}

} // namespace khensu
