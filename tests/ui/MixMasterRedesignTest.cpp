// tests/ui/MixMasterRedesignTest.cpp
// UX2-2 / UX2-5 / FN2-4: the Mix / Master screen's models - the rail's top
// recommendations, the gain-staging target fit, the rack's knob sizing, the
// transport scrubber's peaks / cache / seek maths, and a loaded Mixer Chain
// preset staged in the one plugin chain (WF-9) instead of posted directly.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "support/FakeAppController.h"
#include "ui/Copilot/MixerChainPresetParams.h"
#include "ui/MixMasterEngine/GainStagingPanel.h"
#include "ui/MixMasterEngine/SignalFlowRackPanel.h"
#include "ui/MixMasterEngine/SuggestionListPanel.h"
#include "ui/shared/TransportWaveform.h"

using namespace khensu;

namespace {
Recommendation rec(const char* id, float severity, int chainPosition)
{
    Recommendation r;
    r.id = id;
    r.severity = severity;
    r.chain_position = chainPosition;
    return r;
}
} // namespace

TEST_CASE("The rail shows the most severe open findings", "[mixmaster][ux2-2]")
{
    const std::vector<Recommendation> recs{rec("a", 40.0f, 3), rec("b", 90.0f, 5), rec("c", 70.0f, 1),
                                           rec("d", 95.0f, 2), rec("e", 10.0f, 0)};
    const std::map<std::string, bool> settled{{"d", true}, {"a", false}};
    const auto top = suggestion_order::topOpen(recs, settled, 3);
    REQUIRE(top.size() == 3);
    CHECK(recs[top[0]].id == "b"); // d is settled
    CHECK(recs[top[1]].id == "c");
    CHECK(recs[top[2]].id == "a");

    const auto all = suggestion_order::chainOrder(recs);
    REQUIRE(all.size() == recs.size());
    CHECK(recs[all.front()].id == "e"); // earliest stage first
    CHECK(recs[all.back()].id == "b");
    CHECK(suggestion_order::topOpen(recs, {}, 10).size() == recs.size());
}

TEST_CASE("Gain staging colours a value only out of its target", "[mixmaster][ux2-2]")
{
    using gain_staging::Fit;
    CHECK(gain_staging::fit(-12.0f, -18.0f, -9.0f) == Fit::InTarget);
    CHECK(gain_staging::fit(-8.0f, -18.0f, -9.0f) == Fit::Near);  // 1 dB over a 9 dB band
    CHECK(gain_staging::fit(-2.0f, -18.0f, -9.0f) == Fit::Far);   // 7 dB over
    CHECK(gain_staging::fit(3.0f, 4.0f, 9.0f) == Fit::Near);
    CHECK(gain_staging::proportion(-18.0f, -36.0f, 0.0f) == Catch::Approx(0.5f));
    CHECK(gain_staging::proportion(9.0f, -36.0f, 0.0f) == Catch::Approx(1.0f)); // clamped
}

TEST_CASE("The rack's knobs fit its width, then scroll", "[mixmaster][ux2-2]")
{
    using rack_layout::ModuleSpec;
    const std::vector<ModuleSpec> modules{{1, 0}, {1, 0}, {3, 0}, {3, 0}, {3, 0}, {9, 1}, {1, 1}, {2, 1}};
    const int wide = rack_layout::knobWidthFor(modules, 4000, 10, 16, 6, 50, 72);
    CHECK(wide == 72); // capped
    const int fit = rack_layout::knobWidthFor(modules, 820, 10, 16, 6, 50, 72);
    CHECK(fit >= 50);
    CHECK(rack_layout::rowWidth(modules, 0, fit, 10, 16, 6) <= 820);
    CHECK(rack_layout::rowWidth(modules, 1, fit, 10, 16, 6) <= 820);
    CHECK(rack_layout::knobWidthFor(modules, 300, 10, 16, 6, 50, 72) == 50); // floor: scrolls sideways
}

TEST_CASE("Transport scrubber maths", "[transport][ux2-5]")
{
    namespace tw = transport_waveform;
    CHECK(tw::timeAtX(50.0f, 100.0f, 200.0) == Catch::Approx(100.0));
    CHECK(tw::timeAtX(-5.0f, 100.0f, 200.0) == Catch::Approx(0.0));
    CHECK(tw::xForTime(50.0, 400.0f, 200.0) == Catch::Approx(100.0f));
    const tw::Regions regions{{10.0, 12.0}, {30.0, 31.5}};
    CHECK(tw::regionAt(regions, 11.0) == 0);
    CHECK(tw::regionAt(regions, 31.0) == 1);
    CHECK(tw::regionAt(regions, 20.0) == -1);
    CHECK(tw::keyboardSeek(10.0, 60.0, juce::KeyPress::rightKey, 5.0) == Catch::Approx(15.0));
    CHECK(tw::keyboardSeek(2.0, 60.0, juce::KeyPress::leftKey, 5.0) == Catch::Approx(0.0));
    CHECK(tw::keyboardSeek(30.0, 60.0, juce::KeyPress::homeKey, 5.0) == Catch::Approx(0.0));
    CHECK(tw::keyboardSeek(30.0, 60.0, 'x', 5.0) == Catch::Approx(30.0));

    const auto cols = tw::columns({0.1f, 0.9f, 0.2f, 0.3f}, 2);
    REQUIRE(cols.size() == 2);
    CHECK(cols[0] == Catch::Approx(0.9f));
    CHECK(cols[1] == Catch::Approx(0.3f));
}

TEST_CASE("Transport peaks are read from the file and cached per file", "[transport][ux2-5]")
{
    namespace tw = transport_waveform;
    // One second at 1 kHz: silence, then a 0.5 burst in the second half.
    juce::AudioBuffer<float> audio(2, 1000);
    audio.clear();
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 500; i < 1000; ++i)
            audio.setSample(ch, i, (i % 2 == 0) ? 0.5f : -0.5f);
    juce::MemoryBlock wav;
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(wav, false);
        auto writer = format.createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(1000.0).withNumChannels(2).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(new juce::MemoryInputStream(wav, false), true));
    REQUIRE(reader != nullptr);
    const auto peaks = tw::computePeaks(*reader, 10);
    REQUIRE(peaks.size() == 10);
    CHECK(peaks[0] == Catch::Approx(0.0f).margin(1e-4));
    CHECK(peaks[9] == Catch::Approx(0.5f).margin(1e-3));
    CHECK(tw::computePeaks(*reader, 10, [] { return true; }).empty()); // abandoned

    tw::PeakCache cache(2);
    cache.put("a", {0.1f});
    cache.put("b", {0.2f});
    REQUIRE(cache.find("a") != nullptr); // a is now the most recent
    cache.put("c", {0.3f});              // evicts b
    CHECK(cache.find("b") == nullptr);
    CHECK(cache.find("a") != nullptr);
    CHECK(cache.find("c") != nullptr);
    CHECK(cache.size() == 2);
}

namespace {
struct StagingController final : test::FakeAppController {
    std::map<juce::String, float> staged;
    juce::String source;
    int stageCalls = 0;
    void stagePluginChain(const std::map<juce::String, float>& params, const juce::String& from) override
    {
        staged = params;
        source = from;
        ++stageCalls;
    }
};
} // namespace

TEST_CASE("A loaded Mixer Chain preset is staged in the one plugin chain (WF-9)", "[mixerchain][wf-9][fn2-4]")
{
    const auto preset = juce::JSON::parse(R"({"name":"Vox","parameter_states":{"hpf_on":1,"hpf_freq":90.5,
                                             "comp_on":true,"bad":"text"}})");
    const auto params = mixer_chain_preset::parameterStates(preset);
    REQUIRE(params.size() == 3); // the text value is skipped
    CHECK(params.at("hpf_freq") == Catch::Approx(90.5f));
    CHECK(params.at("comp_on") == Catch::Approx(1.0f));

    StagingController controller;
    CHECK(mixer_chain_preset::stageLoadedPreset(controller.lifecycle(), params, "Vox"));
    CHECK(controller.stageCalls == 1);
    CHECK(controller.staged == params);
    CHECK(controller.source.contains("Vox"));

    // A preset without parameter states stages nothing.
    CHECK_FALSE(mixer_chain_preset::stageLoadedPreset(controller.lifecycle(), {}, "Empty"));
    CHECK(controller.stageCalls == 1);
}

#include "ui/MixMasterEngine/PresetStore.h"

TEST_CASE("Desktop presets are the shared chain presets the plugin reads (FN2-5)", "[presets][fn2-5]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("shared_presets", "");
    REQUIRE(dir.createDirectory());

    // Desktop -> plugin: a desktop save is a shared preset with the rack's
    // params and the chain as stages.
    const preset_store::Chain chain{preset_store::effect("Limiter", {{"threshold_db", -1.0f}})};
    const preset_store::Params rack{{"comp_on", 1.0f}, {"comp_thresh_db", -20.5f}};
    const auto saved = preset_store::save("Desk master", chain, rack, dir);
    REQUIRE(saved.has_value());
    const auto asPlugin = presets::loadPreset(dir.getChildFile(saved->fileName));
    REQUIRE(asPlugin.has_value());
    CHECK(asPlugin->name == "Desk master");
    CHECK(asPlugin->params == rack);
    REQUIRE(asPlugin->stages.size() == 1);
    CHECK(asPlugin->stages[0].effect == "Limiter");

    // Plugin -> desktop: a preset the plugin saved is listed, and its params
    // come back for the rack.
    presets::ChainPreset fromPlugin;
    fromPlugin.name = "Plugin vocal";
    fromPlugin.source = "Khensu Copilot";
    fromPlugin.params = {{"hpf_on", 1.0f}, {"hpf_freq", 120.0f}};
    juce::File written;
    juce::String error;
    REQUIRE(presets::save(fromPlugin, dir, written, error));
    const auto all = preset_store::list(dir);
    REQUIRE(all.size() == 2);
    const preset_store::Entry* pluginEntry = nullptr;
    for (const auto& e : all)
        if (e.name == "Plugin vocal") pluginEntry = &e;
    REQUIRE(pluginEntry != nullptr);
    CHECK(pluginEntry->paramCount == 2);
    CHECK(pluginEntry->effectCount == 0);
    CHECK(preset_store::loadParams(*pluginEntry, dir) == fromPlugin.params);
    CHECK(preset_store::load(*pluginEntry, dir).empty());
    CHECK(preset_store::remove(*pluginEntry, dir));
    CHECK(preset_store::list(dir).size() == 1);
    dir.deleteRecursively();
}

TEST_CASE("The old desktop preset store migrates once and is kept (FN2-5)", "[presets][fn2-5]")
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("preset_mig", "");
    const auto legacy = root.getChildFile("effect_chain_presets");
    const auto dir = root.getChildFile("presets");
    REQUIRE(legacy.createDirectory());
    REQUIRE(dir.createDirectory());
    legacy.getChildFile("Glue.json").replaceWithText(
        R"({"schema_version":1,"name":"Glue","saved_at":"2026-11-01T10:00:00Z",)"
        R"("effects":[{"effect":"GlueCompressor","parameters":{"ratio":2.0}}]})");
    legacy.getChildFile("Kept.json").replaceWithText(
        R"({"schema_version":1,"name":"Kept","effects":[{"effect":"Limiter","parameters":{}}]})");
    legacy.getChildFile("broken.json").replaceWithText("not json");
    // A shared preset of the same name is never overwritten.
    presets::ChainPreset existing;
    existing.name = "Kept";
    existing.params = {{"lim_on", 1.0f}};
    juce::File written;
    juce::String error;
    REQUIRE(presets::save(existing, dir, written, error));

    CHECK(preset_store::migrateLegacy(legacy, dir) == 1);
    CHECK(legacy.getChildFile("Glue.json").existsAsFile()); // the old files stay
    CHECK(legacy.getChildFile(preset_store::kMigratedMarker).existsAsFile());
    const auto glue = presets::loadPreset(presets::fileFor(dir, "Glue"));
    REQUIRE(glue.has_value());
    REQUIRE(glue->stages.size() == 1);
    CHECK(glue->stages[0].effect == "GlueCompressor");
    CHECK(glue->stages[0].parameters.at("ratio") == Catch::Approx(2.0f));
    const auto kept = presets::loadPreset(presets::fileFor(dir, "Kept"));
    REQUIRE(kept.has_value());
    CHECK(kept->stages.empty()); // untouched
    CHECK(kept->params.count("lim_on") == 1);

    // Once only.
    presets::fileFor(dir, "Glue").deleteFile();
    CHECK(preset_store::migrateLegacy(legacy, dir) == 0);
    CHECK_FALSE(presets::fileFor(dir, "Glue").existsAsFile());
    root.deleteRecursively();
}
