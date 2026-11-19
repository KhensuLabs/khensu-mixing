#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CopilotTransportController.h"

#include <atomic>
#include <map>

namespace {

using khensu::CopilotTransportController;
using khensu::ITransportBridge;
using khensu::ipc::BridgeError;

class FakeTransportBridge final : public ITransportBridge {
  public:
    bool postResult = true;
    SeekState seek;
    PluginParams params;
    int postCalls = 0;
    int seekCalls = 0;
    int paramsCalls = 0;

    bool postPlayhead(const PlayheadState& state) override
    {
        lastState = state;
        ++postCalls;
        return postResult;
    }

    SeekResult pollSeek(int sinceGeneration) override
    {
        ++seekCalls;
        lastSince = sinceGeneration;
        return khensu::Result<SeekState, BridgeError>::ok(seek);
    }

    PluginParamsResult pollPluginParams(const juce::String& instanceId) override
    {
        ++paramsCalls;
        lastParamsInstance = instanceId;
        return khensu::Result<PluginParams, BridgeError>::ok(params);
    }

    int lastSince = -2;
    juce::String lastParamsInstance;

    PlayheadState lastState;
};

} // namespace

TEST_CASE("Transport controller pollOnce is deterministic with an injected bridge", "[plugin][arch05]")
{
    std::atomic<int> seekCount{0};
    std::atomic<int> parameterCount{0};
    float receivedSeek = 0.0f;
    juce::String receivedParameter;
    float receivedValue = 0.0f;

    CopilotTransportController controller(
        [] { return true; },
        [] {
            ITransportBridge::PlayheadState state;
            state.position_sec = 12.5f;
            state.is_playing = true;
            return state;
        },
        [&](float position) {
            receivedSeek = position;
            ++seekCount;
        },
        [&](const juce::String& name, float value) {
            receivedParameter = name;
            receivedValue = value;
            ++parameterCount;
        });

    FakeTransportBridge bridge;
    bridge.seek.pending = true;
    bridge.seek.position_sec = 7.25f;
    bridge.params.generation = 1;
    bridge.params.params.emplace("eq_gain_db", -3.0f);

    controller.pollOnce(bridge);

    // AUDIT-003: the first successful poll only records the generation the
    // service is already holding (a push from before this instance existed);
    // applying it would overwrite the DAW-restored state.
    CHECK(parameterCount.load() == 0);

    // A later, different generation is a genuinely new desktop push.
    bridge.params.generation = 2;
    controller.pollOnce(bridge);

    REQUIRE(bridge.postCalls == 2);
    REQUIRE(bridge.seekCalls == 2);
    REQUIRE(bridge.paramsCalls == 2);
    CHECK(bridge.lastState.position_sec == Catch::Approx(12.5f));
    CHECK(bridge.lastState.is_playing);
    CHECK(controller.serviceHealthStatus() == khensu::ipc::ServiceHealthStatus::Connected);
    CHECK(controller.serviceHealthConsecutiveFailures() == 0);
    CHECK(controller.requestedSeekSec() == Catch::Approx(7.25f));
    CHECK(seekCount.load() == 2);
    CHECK(receivedSeek == Catch::Approx(7.25f));
    CHECK(parameterCount.load() == 1);
    CHECK(receivedParameter == "eq_gain_db");
    CHECK(receivedValue == Catch::Approx(-3.0f));

    // The same generation is idempotent even when the service returns it
    // again on a later poll.
    controller.pollOnce(bridge);
    CHECK(parameterCount.load() == 1);

    bridge.postResult = false;
    controller.pollOnce(bridge);
    CHECK(controller.serviceHealthStatus() == khensu::ipc::ServiceHealthStatus::Lost);
    CHECK(controller.serviceHealthConsecutiveFailures() == 1);
}

TEST_CASE("Transport controller does not touch an injected bridge before preparation", "[plugin][arch05]")
{
    CopilotTransportController controller(
        [] { return false; }, [] { return ITransportBridge::PlayheadState{}; }, nullptr, nullptr);
    FakeTransportBridge bridge;

    controller.pollOnce(bridge);

    CHECK(bridge.postCalls == 0);
    CHECK(bridge.seekCalls == 0);
    CHECK(bridge.paramsCalls == 0);
    CHECK(controller.serviceHealthStatus() == khensu::ipc::ServiceHealthStatus::NeverConnected);
}

TEST_CASE("Transport controller treats the first param poll as a baseline even with no push yet", "[plugin][arch05]")
{
    int parameterCount = 0;
    CopilotTransportController controller(
        [] { return true; }, [] { return ITransportBridge::PlayheadState{}; }, nullptr,
        [&](const juce::String&, float) { ++parameterCount; });
    FakeTransportBridge bridge;

    // Nothing pushed yet this service lifetime.
    bridge.params.generation = 0;
    controller.pollOnce(bridge);
    CHECK(parameterCount == 0);

    // The desktop's first push after the instance came up applies.
    bridge.params.generation = 1;
    bridge.params.params.emplace("eq_gain_db", 2.0f);
    controller.pollOnce(bridge);
    CHECK(parameterCount == 1);

    // Re-polling the same generation stays idempotent.
    controller.pollOnce(bridge);
    CHECK(parameterCount == 1);
}


namespace {

/// In-memory model of the service's per-instance transport contract (PLG-04):
/// playhead keyed by instance_id, seeks never consumed (generation + since),
/// params targeted by target_instance_id.
struct FakeTransportService {
    std::map<juce::String, ITransportBridge::PlayheadState> playheads;
    float seekPos = 0.0f;
    int seekGeneration = 0;
    std::map<juce::String, float> params;
    juce::String paramsTarget;
    int paramsGeneration = 0;

    void seek(float pos)
    {
        seekPos = pos;
        ++seekGeneration;
    }
    void pushParams(std::map<juce::String, float> p, const juce::String& target)
    {
        params = std::move(p);
        paramsTarget = target;
        ++paramsGeneration;
    }
};

class ServiceBackedBridge final : public ITransportBridge {
  public:
    explicit ServiceBackedBridge(FakeTransportService& service) : service_(service) {}
    bool postPlayhead(const PlayheadState& state) override
    {
        service_.playheads[state.instance_id] = state;
        return true;
    }
    SeekResult pollSeek(int since) override
    {
        SeekState s;
        s.position_sec = service_.seekPos;
        s.generation = service_.seekGeneration;
        s.pending = service_.seekGeneration > since;
        return khensu::Result<SeekState, BridgeError>::ok(s);
    }
    PluginParamsResult pollPluginParams(const juce::String& instanceId) override
    {
        PluginParams p;
        p.generation = service_.paramsGeneration;
        if (service_.paramsTarget.isEmpty() || service_.paramsTarget == instanceId) p.params = service_.params;
        return khensu::Result<PluginParams, BridgeError>::ok(p);
    }

  private:
    FakeTransportService& service_;
};

struct InstanceUnderTest {
    explicit InstanceUnderTest(const juce::String& id, float position)
        : controller([] { return true; },
                     [position] {
                         ITransportBridge::PlayheadState state;
                         state.position_sec = position;
                         return state;
                     },
                     [this](float pos) {
                         ++seeks;
                         lastSeek = pos;
                     },
                     [this](const juce::String& name, float value) { applied[name] = value; })
    {
        controller.setInstanceId(id);
    }
    int seeks = 0;
    float lastSeek = 0.0f;
    std::map<juce::String, float> applied;
    CopilotTransportController controller;
};

} // namespace

TEST_CASE("Transport targets each Copilot instance separately (PLG-04)", "[plugin][plg04]")
{
    FakeTransportService service;
    ServiceBackedBridge bridge(service);
    InstanceUnderTest vocal("vocal-id", 1.0f), master("master-id", 2.0f);

    // Baseline polls (AUDIT-003): nothing applied, both instances registered.
    vocal.controller.pollOnce(bridge);
    master.controller.pollOnce(bridge);
    REQUIRE(service.playheads.size() == 2);
    CHECK(service.playheads["vocal-id"].position_sec == Catch::Approx(1.0f));
    CHECK(service.playheads["master-id"].position_sec == Catch::Approx(2.0f));

    // A seek reaches EVERY instance exactly once (it used to be consumed by
    // whichever instance polled first).
    service.seek(42.0f);
    vocal.controller.pollOnce(bridge);
    master.controller.pollOnce(bridge);
    vocal.controller.pollOnce(bridge);
    master.controller.pollOnce(bridge);
    CHECK(vocal.seeks == 1);
    CHECK(master.seeks == 1);
    CHECK(vocal.lastSeek == Catch::Approx(42.0f));
    CHECK(master.lastSeek == Catch::Approx(42.0f));

    // A targeted parameter push only lands on its target.
    service.pushParams({{"eq_gain_db", 4.0f}}, "master-id");
    vocal.controller.pollOnce(bridge);
    master.controller.pollOnce(bridge);
    CHECK(vocal.applied.empty());
    REQUIRE(master.applied.count("eq_gain_db") == 1);
    CHECK(master.applied["eq_gain_db"] == Catch::Approx(4.0f));

    // An untargeted push reaches both.
    service.pushParams({{"comp_ratio", 3.0f}}, {});
    vocal.controller.pollOnce(bridge);
    master.controller.pollOnce(bridge);
    CHECK(vocal.applied.count("comp_ratio") == 1);
    CHECK(master.applied.count("comp_ratio") == 1);
}

TEST_CASE("Transport controller sends its instance id with every route (PLG-04)", "[plugin][plg04]")
{
    CopilotTransportController controller(
        [] { return true; }, [] { return ITransportBridge::PlayheadState{}; }, nullptr, nullptr);
    controller.setInstanceId("abc-123");
    FakeTransportBridge bridge;
    bridge.seek.generation = 5;
    controller.pollOnce(bridge);
    CHECK(bridge.lastState.instance_id == "abc-123");
    CHECK(bridge.lastParamsInstance == "abc-123");
    CHECK(bridge.lastSince == 0); // first poll: non-consuming read from generation 0
    controller.pollOnce(bridge);
    CHECK(bridge.lastSince == 5); // then the last generation it saw
}
