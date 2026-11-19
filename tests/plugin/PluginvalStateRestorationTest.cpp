// tests/plugin/PluginvalStateRestorationTest.cpp
// pluginval's "Plugin state restoration" check, replayed through the REAL VST3
// wrapper: the freshly built "Khensu Copilot.vst3" is loaded with JUCE's VST3
// host and driven exactly as pluginval drives it, including its randomised
// order's hard case where "Fuzz parameters" runs immediately before.
//
// Why this exists: under `pluginval --randomise`, v1.0.3 reported
// "HPF On not restored on setStateInformation" (the first parameter only).
// The cause was pluginval 1.0.3's own host, built on JUCE f9b6e2e: its
// AudioPluginInstance parameter setValue() only wrote the host's cache and
// flagged it for the *next process() call*. The fuzz test never processes, so
// its last values never reached the plugin, and the restoration test then
// took its "expected" value from that undelivered cache. The plugin's saved
// state honestly held the value the plugin actually had. Only the first
// parameter failed, because the first setStateInformation re-syncs every host
// value. pluginval 1.0.4's host also pushes setValue() to the edit
// controller (JUCE's EditControllerParameterDispatcher), so the edit is
// delivered and the round trip holds: the same seed (0x122925b) fails on
// 1.0.3 and passes on 1.0.4 with an identical test order. CI pins 1.0.4.
//
// This test pins the plugin's half of that contract with a host that DOES
// deliver edits: values written from a non-message thread (as pluginval's
// test thread does) must be the ones saved, restored, and read back,
// for every automatable parameter including the bool switches.
#include <catch2/catch_test_macros.hpp>

#include <JuceHeader.h>

#include <thread>

#ifndef KHENSU_COPILOT_VST3_PATH
#define KHENSU_COPILOT_VST3_PATH ""
#endif

namespace {

std::vector<juce::AudioProcessorParameter*> nonBypassAutomatable(juce::AudioPluginInstance& instance)
{
    // pluginval's getNonBypassAutomatableParameters().
    std::vector<juce::AudioProcessorParameter*> params;
    auto* bypass = instance.getBypassParameter();
    for (auto* p : instance.getParameters())
        if (p->isAutomatable() && p != bypass) params.push_back(p);
    return params;
}

// pluginval runs every test body on its own thread and hops to the message
// thread only for get/setState. Run `fn` on a worker and pump the message
// loop until it finishes, so the host's off-thread parameter path is used.
template <typename Fn> void onTestThread(Fn&& fn)
{
    std::atomic<bool> done{false};
    std::thread worker([&] {
        fn();
        done = true;
    });
    while (!done) juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    worker.join();
}

std::unique_ptr<juce::AudioPluginInstance> loadBuiltCopilot(juce::String& error)
{
    const juce::File bundle(juce::String(KHENSU_COPILOT_VST3_PATH));
    if (!bundle.exists()) {
        error = "built VST3 not found at " + bundle.getFullPathName();
        return nullptr;
    }
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, bundle.getFullPathName());
    if (types.isEmpty()) {
        error = "no plugin types in " + bundle.getFullPathName();
        return nullptr;
    }
    return format.createInstanceFromDescription(*types[0], 48000.0, 512, error);
}

} // namespace

TEST_CASE("pluginval state restoration holds through the real VST3 wrapper after a parameter fuzz",
          "[plugin][pluginval][vst3-roundtrip]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::String error;
    auto instance = loadBuiltCopilot(error);
    if (instance == nullptr) SKIP(error.toStdString());

    // pluginval prepares the instance when it opens it; no audio is processed
    // by the state tests, so nothing but the edit controller can carry edits.
    instance->prepareToPlay(48000.0, 512);

    juce::Random r(0x122925b); // the seed that reproduced the v1.0.3 failure
    const auto params = nonBypassAutomatable(*instance);
    REQUIRE(params.size() >= 10);

    for (int round = 0; round < 3; ++round) {
        // "Fuzz parameters": random raw values, the last one sticks.
        onTestThread([&] {
            for (auto* p : params)
                for (int i = 0; i < 5; ++i) p->setValue(r.nextFloat());
        });

        // "Plugin state restoration", verbatim.
        juce::MemoryBlock originalState;
        instance->getStateInformation(originalState);
        for (auto* parameter : params) {
            const float originalValue = parameter->getValue();
            onTestThread([&] { parameter->setValue(r.nextFloat()); });
            instance->setStateInformation(originalState.getData(), (int)originalState.getSize());
            INFO(parameter->getName(64).toStdString() << " round " << round);
            CHECK(std::abs(parameter->getValue() - originalValue) <= 0.1f);
        }

        // strictness >= 8: the state read back equals what the host set.
        juce::MemoryBlock duplicateState;
        instance->getStateInformation(duplicateState);
        CHECK(duplicateState == originalState);
    }

    instance->releaseResources();
}
