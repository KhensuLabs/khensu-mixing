// tests/plugin/PluginPresetStateTest.cpp
// UI-9 factory presets (host programs), user preset files (FN2-5: the
// shared chain preset JSON, migrated from the old XML files) and the A/B
// snapshots; UI-8 editor view persistence; state v3 (new parameters
// appended, migrate from v2, byte-stable round trips); the mapper's ranges
// against the real parameter layout (OVH-8).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotPresets.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include <khensu/mixerchain/RecommendationDspMapper.h>

using khensu::KhensuCopilotProcessor;
using P = khensu::KhensuCopilotProcessor::ParamIDs;

namespace {
struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    juce::File presetDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("khensu-copilot-presets", "");
    Fixture()
    {
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.setUserPresetDirectoryForTesting(presetDir);
        proc.setLegacyUserPresetDirectoryForTesting(presetDir.getChildFile("legacy"));
    }
    ~Fixture() { presetDir.deleteRecursively(); }
};

float plain(KhensuCopilotProcessor& proc, const char* id) { return proc.getParamPlain(id); }

void setPlain(KhensuCopilotProcessor& proc, const char* id, float value)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(value));
}

juce::MemoryBlock save(KhensuCopilotProcessor& proc)
{
    juce::MemoryBlock block;
    proc.getStateInformation(block);
    return block;
}
} // namespace

TEST_CASE("Factory presets are host programs that set every DSP parameter (UI-9)", "[plugin][ui9][presets]")
{
    Fixture f;
    REQUIRE(f.proc.getNumPrograms() == khensu::copilot::presets::numFactoryPresets());
    REQUIRE(f.proc.getNumPrograms() >= 6);
    CHECK(f.proc.getCurrentProgram() == 0);
    CHECK(f.proc.getProgramName(0) == "Bypass (flat)");

    int podcast = -1;
    for (int i = 0; i < f.proc.getNumPrograms(); ++i)
        if (f.proc.getProgramName(i) == "Podcast voice") podcast = i;
    REQUIRE(podcast > 0);

    setPlain(f.proc, P::lpfOn, 1.0f); // a stage the preset does not name
    f.proc.setCurrentProgram(podcast);
    CHECK(f.proc.getCurrentProgram() == podcast);
    CHECK(f.proc.getCurrentPresetName() == "Podcast voice");
    CHECK(plain(f.proc, P::deessOn) > 0.5f);
    CHECK(plain(f.proc, P::hpfFreq) == Catch::Approx(80.0f).margin(0.5f));
    CHECK(plain(f.proc, P::eqType) == Catch::Approx(1.0f)); // low shelf
    CHECK(plain(f.proc, P::lpfOn) < 0.5f);                  // back to its default

    // Program 0 is the plugin's defaults.
    f.proc.setCurrentProgram(0);
    for (auto* p : f.proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
            if (ranged->getParameterID() != P::bypass && ranged->getParameterID() != P::fixAmount) {
                INFO(ranged->getParameterID());
                CHECK(p->getValue() == Catch::Approx(p->getDefaultValue()).margin(1e-6));
            }

    // Out-of-range programs are ignored.
    f.proc.setCurrentProgram(99);
    CHECK(f.proc.getCurrentProgram() == 0);
}

TEST_CASE("A user preset round-trips through its file (UI-9)", "[plugin][ui9][presets]")
{
    Fixture f;
    setPlain(f.proc, P::compOn, 1.0f);
    setPlain(f.proc, P::compThreshDb, -27.5f);
    setPlain(f.proc, P::eq2Type, 2.0f);
    setPlain(f.proc, P::deessFreq, 7300.0f);
    juce::String error;
    REQUIRE(f.proc.saveUserPreset("My Bus: glue/2", error));
    CHECK(f.proc.getCurrentPresetName() == "My Bus: glue/2");
    const auto files = f.proc.listUserPresets();
    REQUIRE(files.size() == 1);
    CHECK(files[0].preset.name == "My Bus: glue/2");
    CHECK(files[0].file.getFileName().endsWith(khensu::presets::kChainPresetExtension));
    CHECK(files[0].preset.source == "Khensu Copilot");
    // Every DSP parameter (+ chain mix) is in the file, by its plugin id.
    CHECK(files[0].preset.params.size() == static_cast<size_t>(KhensuCopilotProcessor::kNumDspParams) + 1);
    CHECK(files[0].preset.params.at(P::compThreshDb) == Catch::Approx(-27.5f).margin(0.05f));

    f.proc.setCurrentProgram(0);
    CHECK(plain(f.proc, P::compOn) < 0.5f);
    REQUIRE(f.proc.loadUserPreset(files[0].file, error));
    CHECK(plain(f.proc, P::compOn) > 0.5f);
    CHECK(plain(f.proc, P::compThreshDb) == Catch::Approx(-27.5f).margin(0.05f));
    CHECK(plain(f.proc, P::eq2Type) == Catch::Approx(2.0f));
    CHECK(plain(f.proc, P::deessFreq) == Catch::Approx(7300.0f).margin(1.0f));
    CHECK(f.proc.getCurrentPresetName() == "My Bus: glue/2");

    // A foreign file is refused without touching the parameters, and a bad
    // file in the folder is skipped when listing.
    auto foreign = f.proc.getUserPresetDirectory().getChildFile(juce::String("bad") + khensu::presets::kChainPresetExtension);
    foreign.replaceWithText("<NotAPreset/>");
    CHECK_FALSE(f.proc.loadUserPreset(foreign, error));
    CHECK(error.isNotEmpty());
    CHECK(plain(f.proc, P::compOn) > 0.5f);
    CHECK(f.proc.listUserPresets().size() == 1);

    // A desktop preset: unknown ids are ignored, the desktop-only stages are
    // ignored by the plugin and kept when the plugin saves over it.
    khensu::presets::ChainPreset desk;
    desk.name = "Desk chain";
    desk.source = "Khensu Mixing";
    desk.params = {{P::hpfOn, 1.0f}, {P::hpfFreq, 120.0f}, {"some_future_param", 3.0f}};
    desk.stages = {{"reverb", {{"mix", 0.25f}}}};
    juce::File written;
    REQUIRE(khensu::presets::save(desk, f.proc.getUserPresetDirectory(), written, error));
    REQUIRE(f.proc.loadUserPreset(written, error));
    CHECK(plain(f.proc, P::hpfOn) > 0.5f);
    CHECK(plain(f.proc, P::hpfFreq) == Catch::Approx(120.0f).margin(0.5f));
    CHECK(plain(f.proc, P::compOn) < 0.5f); // not named: back to its default
    REQUIRE(f.proc.saveUserPreset("Desk chain", error));
    const auto resaved = khensu::presets::loadPreset(written);
    REQUIRE(resaved.has_value());
    CHECK(resaved->stages.size() == 1);
    CHECK(resaved->source == "Khensu Copilot");
    // A stage-only preset has nothing for the built-in chain.
    khensu::presets::ChainPreset rackOnly;
    rackOnly.name = "Rack only";
    rackOnly.stages = {{"reverb", {{"mix", 0.25f}}}};
    REQUIRE(khensu::presets::save(rackOnly, f.proc.getUserPresetDirectory(), written, error));
    CHECK_FALSE(f.proc.loadUserPreset(written, error));
}

TEST_CASE("Old XML user presets move into the shared folder once (FN2-5)", "[plugin][fn2-5][presets]")
{
    Fixture f;
    const auto legacy = f.proc.getLegacyUserPresetDirectory();
    REQUIRE(legacy.createDirectory());
    // What the pre-FN2-5 plugin wrote.
    legacy.getChildFile("Vocal bus.khensupreset")
        .replaceWithText("<KhensuCopilotPreset version=\"1\" name=\"Vocal bus\">"
                         "<PARAM id=\"deess_on\" value=\"1\"/><PARAM id=\"deess_freq\" value=\"7100\"/>"
                         "<PARAM id=\"comp_ratio\" value=\"3.5\"/></KhensuCopilotPreset>");
    legacy.getChildFile("broken.khensupreset").replaceWithText("not xml");

    const auto listed = f.proc.listUserPresets();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].preset.name == "Vocal bus");
    CHECK(listed[0].preset.source.contains("migrated"));
    CHECK(listed[0].preset.params.at("deess_freq") == Catch::Approx(7100.0f));
    // The original is kept, renamed; a file that was never a preset stays.
    CHECK_FALSE(legacy.getChildFile("Vocal bus.khensupreset").existsAsFile());
    CHECK(legacy.getChildFile("Vocal bus.khensupreset.migrated").existsAsFile());
    CHECK(legacy.getChildFile("broken.khensupreset").existsAsFile());
    // Listing again does not duplicate it.
    CHECK(f.proc.listUserPresets().size() == 1);

    juce::String error;
    REQUIRE(f.proc.loadUserPreset(listed[0].file, error));
    CHECK(plain(f.proc, P::deessOn) > 0.5f);
    CHECK(plain(f.proc, P::compRatio) == Catch::Approx(3.5f).margin(0.05f));

    // Factory presets are untouched: still built in, still the programs.
    CHECK(f.proc.getNumPrograms() == khensu::copilot::presets::numFactoryPresets());
    CHECK(f.proc.getProgramName(0) == "Bypass (flat)");
    for (const auto& entry : f.proc.listUserPresets())
        for (int i = 0; i < f.proc.getNumPrograms(); ++i) CHECK(entry.preset.name != f.proc.getProgramName(i));
}

TEST_CASE("A/B snapshots keep two parameter sets and copy one over the other (UI-9)", "[plugin][ui9][snapshots]")
{
    Fixture f;
    CHECK(f.proc.getActiveSnapshot() == 0);
    setPlain(f.proc, P::outGainDb, 3.0f);
    f.proc.switchSnapshot(1); // B starts as a copy of A
    CHECK(f.proc.getActiveSnapshot() == 1);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(3.0f).margin(0.05f));
    setPlain(f.proc, P::outGainDb, -6.0f);
    f.proc.switchSnapshot(0);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(3.0f).margin(0.05f));
    f.proc.switchSnapshot(1);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-6.0f).margin(0.05f));
    // Copy B over A, then A is B.
    f.proc.copyActiveSnapshotToOther();
    f.proc.switchSnapshot(0);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-6.0f).margin(0.05f));
}

TEST_CASE("State v3 round-trips the view, program, preset and snapshot byte-for-byte (UI-8/UI-9)",
          "[plugin][ui8][ui9][state]")
{
    Fixture f;
    f.proc.setCurrentProgram(2);
    setPlain(f.proc, P::deessRangeDb, 9.5f);
    f.proc.switchSnapshot(1);
    setPlain(f.proc, P::eq3Type, 1.0f);
    KhensuCopilotProcessor::EditorViewState view;
    view.compact = true;
    view.size = KhensuCopilotProcessor::ViewSize::Large;
    view.tab = 3;
    f.proc.setEditorViewState(view);
    const auto first = save(f.proc);

    std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(first.getData(), (int)first.getSize()));
    REQUIRE(xml != nullptr);
    CHECK(xml->getIntAttribute("khensuStateVersion") == 3);
    CHECK(xml->getIntAttribute("khensuViewCompact") == 1);
    CHECK(xml->getIntAttribute("khensuViewSize") == 2);
    CHECK(xml->getIntAttribute("khensuViewTab") == 3);
    CHECK(xml->getIntAttribute("khensuProgram") == 2);

    KhensuCopilotProcessor restored;
    restored.setStateInformation(first.getData(), (int)first.getSize());
    CHECK(restored.lastStateRejection().isEmpty());
    CHECK(restored.getEditorViewState() == view);
    CHECK(restored.getCurrentProgram() == 2);
    CHECK(restored.getCurrentPresetName() == f.proc.getCurrentPresetName());
    CHECK(restored.getActiveSnapshot() == 1);
    CHECK(restored.getParamPlain(P::eq3Type) == Catch::Approx(1.0f));
    CHECK(restored.getParamPlain(P::deessRangeDb) == Catch::Approx(9.5f).margin(0.05f));
    // Saving what was loaded gives the same bytes (host duplicate checks).
    const auto second = save(restored);
    CHECK(second == first);
    // ...and the held snapshot survives: switching back finds A.
    restored.switchSnapshot(0);
    CHECK(restored.getParamPlain(P::eq3Type) == Catch::Approx(0.0f));
}

TEST_CASE("A v2 state loads with the new parameters at their defaults (state v3 migration)",
          "[plugin][state][migration]")
{
    Fixture f;
    setPlain(f.proc, P::compOn, 1.0f);
    setPlain(f.proc, P::deessOn, 1.0f);
    const auto block = save(f.proc);
    std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(block.getData(), (int)block.getSize()));
    REQUIRE(xml != nullptr);
    // Make it what a v2 build wrote: version 2, none of the v3 parameters
    // or properties.
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.setProperty("khensuStateVersion", 2, nullptr);
    for (const auto* prop : {"khensuViewCompact", "khensuViewSize", "khensuViewTab", "khensuProgram",
                             "khensuPresetName", "khensuSnapActive", "khensuSnapOther"})
        tree.removeProperty(prop, nullptr);
    for (int i = tree.getNumChildren() - 1; i >= 0; --i) {
        const auto id = tree.getChild(i).getProperty("id").toString();
        if (id.startsWith("deess_") || id.endsWith("_type") || id == "fix_amount") tree.removeChild(i, nullptr);
    }
    std::unique_ptr<juce::XmlElement> v2(tree.createXml());
    juce::MemoryBlock v2Block;
    juce::AudioProcessor::copyXmlToBinary(*v2, v2Block);

    KhensuCopilotProcessor loaded;
    loaded.setStateInformation(v2Block.getData(), (int)v2Block.getSize());
    CHECK(loaded.lastStateRejection().isEmpty());
    CHECK(loaded.getParamPlain(P::compOn) > 0.5f); // the v2 values load
    CHECK(loaded.getParamPlain(P::deessOn) < 0.5f); // the new ones default
    CHECK(loaded.getParamPlain(P::deessFreq) == Catch::Approx(6500.0f).margin(1.0f));
    CHECK(loaded.getParamPlain(P::eqType) == Catch::Approx(0.0f));
    CHECK(loaded.getParamPlain(P::fixAmount) == Catch::Approx(1.0f));
    CHECK(loaded.getEditorViewState() == KhensuCopilotProcessor::EditorViewState{});
    // It saves as v3 from then on.
    const auto resaved = save(loaded);
    std::unique_ptr<juce::XmlElement> v3(juce::AudioProcessor::getXmlFromBinary(resaved.getData(), (int)resaved.getSize()));
    CHECK(v3->getIntAttribute("khensuStateVersion") == KhensuCopilotProcessor::kStateVersion);
    juce::ValueTree migrated("KhensuCopilotState");
    KhensuCopilotProcessor::migrateState(migrated, 2);
    CHECK(static_cast<int>(migrated.getProperty("khensuStateVersion")) == 3);
}

TEST_CASE("The mapper's parameter ranges match the plugin's layout (OVH-8)", "[plugin][ovh8][mapper]")
{
    Fixture f;
    int checked = 0;
    for (auto* p : f.proc.getParameters()) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p);
        if (ranged == nullptr) continue;
        float lo = 0.0f, hi = 0.0f;
        if (!khensu::mixerchain::dspParamRange(ranged->getParameterID().toStdString(), lo, hi)) continue;
        INFO(ranged->getParameterID());
        CHECK(ranged->getNormalisableRange().start == Catch::Approx(lo));
        CHECK(ranged->getNormalisableRange().end == Catch::Approx(hi));
        ++checked;
    }
    CHECK(checked >= 20);
    // The v3 ids agree between the mapper and the processor.
    CHECK(juce::String(khensu::mixerchain::DspParamIdsV3::deessThreshDb) == P::deessThreshDb);
    CHECK(juce::String(khensu::mixerchain::DspParamIdsV3::fixAmount) == P::fixAmount);
}
