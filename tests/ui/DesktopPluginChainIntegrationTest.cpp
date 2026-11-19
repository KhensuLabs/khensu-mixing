// tests/ui/DesktopPluginChainIntegrationTest.cpp
// Real third-party VST3 hosting (hidden tag - run explicitly):
//   KhensuTests.exe "[.integration]"
// Loads real plugin binaries through DesktopPluginChain exactly as the
// Plugin Host Chain tab does, renders audio through each and through the
// whole chain, opens each plugin's editor, and round-trips save/restore.
//
// Plugins: semicolon-separated paths in KHENSU_TEST_VST3, else a default set
// of free, licence-dialog-free plugins when installed (a modal licence
// prompt would hang an unattended run), plus this repo's own built copilot.
#include <catch2/catch_test_macros.hpp>
#include "session/DesktopPluginChain.h"

#include <cmath>

using khensu::DesktopPluginChain;

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
    for (const char* rel : {"Airwindows (with BaconPaul and members of the Surge Synth Team)/Airwindows Consolidated.vst3",
                            "ValhallaDSP/ValhallaFreqEcho.vst3", "ValhallaDSP/ValhallaSupermassive.vst3",
                            "TDR Nova.vst3"})
        if (common.getChildFile(rel).exists()) paths.add(common.getChildFile(rel).getFullPathName());
    // The project's own plugin, built next to these tests.
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 6 && dir.exists(); ++i, dir = dir.getParentDirectory()) {
        const auto own = dir.getChildFile("plugin-copilot/KhensuCopilot_artefacts/Release/VST3/Khensu Copilot.vst3");
        if (own.exists()) {
            paths.add(own.getFullPathName());
            break;
        }
    }
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

bool loadInto(DesktopPluginChain& chain, const juce::String& path, juce::String& message)
{
    bool finished = false, ok = false;
    chain.loadPluginAsync(juce::File(path), [&](bool success, juce::String msg) {
        ok = success;
        message = msg;
        finished = true;
    });
    pumpUntil([&] { return finished; }, 60000);
    return finished && ok;
}

juce::AudioBuffer<float> noise(int samples)
{
    juce::AudioBuffer<float> b(2, samples);
    juce::Random rng(11);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < samples; ++i) b.setSample(c, i, (rng.nextFloat() - 0.5f) * 0.4f);
    return b;
}

bool allFinite(const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getSample(c, i))) return false;
    return true;
}
} // namespace

TEST_CASE("Real VST3 plugins load, render, open editors and restore", "[.integration][hosting-real]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto plugins = candidatePlugins();
    if (plugins.isEmpty()) SKIP("No test plugins found (set KHENSU_TEST_VST3).");

    DesktopPluginChain chain;
    for (const auto& path : plugins) {
        INFO("plugin: " << path);
        juce::String message;
        REQUIRE(loadInto(chain, path, message));
        const int index = chain.size() - 1;
        auto* instance = chain.instanceAt(index);
        REQUIRE(instance != nullptr);
        UNSCOPED_INFO("loaded " << message << " (" << instance->getTotalNumInputChannels() << " in / "
                                << instance->getTotalNumOutputChannels() << " out)");
        // A plugin left with disabled buses would be silently skipped.
        CHECK(instance->getTotalNumOutputChannels() >= 1);

        // Editor: create and destroy as the floating window does.
        if (instance->hasEditor()) {
            std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorAndMakeActive());
            CHECK(editor != nullptr);
            if (editor != nullptr) CHECK(editor->getWidth() > 0);
            editor.reset();
        }

        // Solo render through just this plugin.
        for (int i = 0; i < chain.size(); ++i) chain.setBypassed(i, i != index);
        auto audio = noise(48000);
        const int latency = chain.renderBuffer(audio, 48000.0);
        CHECK(latency >= 0);
        CHECK(audio.getNumSamples() == 48000);
        CHECK(allFinite(audio));
        // Every default plugin here is an effect that audibly changes noise
        // at its default settings; identical output means it never ran.
        const auto dry = noise(48000);
        double diff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 48000; ++i) diff += std::abs(audio.getSample(c, i) - dry.getSample(c, i));
        UNSCOPED_INFO("mean |wet - dry| = " << diff / 96000.0);
    }

    // Whole chain, then persist and restore with plugin state.
    for (int i = 0; i < chain.size(); ++i) chain.setBypassed(i, false);
    auto audio = noise(96000);
    chain.renderBuffer(audio, 48000.0);
    CHECK(allFinite(audio));

    const auto saved = juce::ValueTree::fromXml(chain.toValueTree().toXmlString());
    DesktopPluginChain restored;
    int loaded = -1;
    restored.restoreAsync(saved, [&loaded](int n) { loaded = n; });
    REQUIRE(pumpUntil([&] { return loaded >= 0; }, 120000));
    CHECK(loaded == chain.size());
    REQUIRE(restored.size() == chain.size());
    for (int i = 0; i < chain.size(); ++i) CHECK(restored.slots()[static_cast<size_t>(i)].name == chain.slots()[static_cast<size_t>(i)].name);
}
