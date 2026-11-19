// tests/plugin/HostedChainRealPluginTest.cpp
// Real third-party VST3s in the copilot plugin's hosted chain (hidden tag -
// run explicitly):
//   KhensuPluginTests.exe "[.integration]"
// Drives the "Hosted" tab exactly as its file chooser does
// (HostedChainMiniPanel::loadPluginFile -> addPluginToChainAsync), then runs
// audio through the processor, checks the reported latency covers the hosted
// slots, and round-trips the chain through
// get/setStateInformation. This is the automated half of the "load a real
// .vst3 through the Hosted tab" check; looking at the tab inside a DAW is
// still a manual step.
//
// Plugins: semicolon-separated paths in KHENSU_TEST_VST3, else a default set
// of free, licence-dialog-free plugins when installed (a modal licence prompt
// would hang an unattended run).
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/ui/HostedChainMiniPanel.h"

#include <cmath>

using khensu::HostedChainMiniPanel;
using khensu::KhensuCopilotProcessor;

namespace {
juce::StringArray candidatePlugins()
{
    juce::StringArray paths;
    const auto env = juce::SystemStats::getEnvironmentVariable("KHENSU_TEST_VST3", {});
    if (env.isNotEmpty()) {
        paths.addTokens(env, ";", "\"");
        paths.removeEmptyStrings();
        return paths;
    }
    const juce::File common("C:/Program Files/Common Files/VST3");
    for (const char* rel :
         {"Airwindows (with BaconPaul and members of the Surge Synth Team)/Airwindows Consolidated.vst3",
          "ValhallaDSP/ValhallaFreqEcho.vst3", "ValhallaDSP/ValhallaSupermassive.vst3", "TDR Nova.vst3"})
        if (common.getChildFile(rel).exists()) paths.add(common.getChildFile(rel).getFullPathName());
    return paths;
}

template <typename Pred> bool pumpUntil(Pred done, int timeoutMs)
{
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!done()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    return true;
}

bool allFinite(const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getSample(c, i))) return false;
    return true;
}

void runBlocks(KhensuCopilotProcessor& proc, int blocks)
{
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng(7);
    for (int b = 0; b < blocks; ++b) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
                buffer.setSample(c, i, (rng.nextFloat() - 0.5f) * 0.4f);
        proc.processBlock(buffer, midi);
        REQUIRE(allFinite(buffer));
    }
}
} // namespace

TEST_CASE("Hosted tab loads real VST3s, processes, reports latency and restores",
          "[.integration][plugin][hostedchain][hosting-real]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto plugins = candidatePlugins();
    if (plugins.isEmpty()) SKIP("No test plugins found (set KHENSU_TEST_VST3).");

    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);

    {
        HostedChainMiniPanel panel(proc);
        panel.setSize(700, 300);

        for (const auto& path : plugins) {
            INFO("plugin: " << path);
            const auto before = proc.getNumHostedPlugins();
            panel.loadPluginFile(juce::File(path));
            REQUIRE(pumpUntil([&] { return !panel.getStatusText().startsWith("Loading"); }, 60000));
            UNSCOPED_INFO("status: " << panel.getStatusText());
            REQUIRE(panel.getStatusText().startsWith("Added"));
            CHECK(proc.getNumHostedPlugins() == before + 1);
            panel.refreshFromProcessor();
            CHECK(panel.getNumRows() == proc.getNumHostedPlugins());
        }

        const auto slots = proc.getHostedPluginSlots();
        REQUIRE(static_cast<int>(slots.size()) == plugins.size());
        int hostedLatency = 0;
        for (const auto& s : slots) {
            UNSCOPED_INFO(s.name << ": latency " << s.latencySamples);
            CHECK(s.latencySamples >= 0);
            hostedLatency += s.latencySamples;
        }

        runBlocks(proc, 200);
        // The host must be told about latency the hosted slots add.
        CHECK(proc.getLatencySamples() >= hostedLatency);

        // Per-slot bypass and wet mix keep the output finite.
        proc.setHostedPluginBypass(0, true);
        proc.setHostedPluginWetMix(static_cast<int>(slots.size()) - 1, 0.5f);
        runBlocks(proc, 50);
    }

    juce::MemoryBlock state;
    proc.getStateInformation(state);
    const auto names = proc.getHostedPluginSlots();

    KhensuCopilotProcessor restored;
    restored.setPlayConfigDetails(2, 2, 48000.0, 512);
    restored.prepareToPlay(48000.0, 512);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(pumpUntil([&] { return !restored.isHostedRestorePending(); }, 120000));
    const auto back = restored.getHostedPluginSlots();
    REQUIRE(back.size() == names.size());
    for (size_t i = 0; i < back.size(); ++i) {
        CHECK(back[i].name == names[i].name);
        CHECK(back[i].bypassed == names[i].bypassed);
    }
    runBlocks(restored, 50);

    restored.clearHostedPluginChain();
    proc.clearHostedPluginChain();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}
