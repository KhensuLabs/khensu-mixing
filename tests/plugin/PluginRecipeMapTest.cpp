// tests/plugin/PluginRecipeMapTest.cpp
// STU-17: "Load into DSP chain" from a generated mixer-chain recipe fills the
// plugin's whole state-v5 chain - six EQ bands chosen by importance, shelves /
// notches / cuts as kinds, dynamic and M/S bands, and the width, saturation,
// transient and bass mono stages - and lists what it could not map the way
// Apply all lists its skipped findings.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/FixBatch.h"
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"

#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/mixerchain/RecommendationDspMapper.h>

using khensu::KhensuCopilotProcessor;
using P = khensu::KhensuCopilotProcessor::ParamIDs;
using Plugin = khensu::ipc::HttpServiceBridge::MixerChainPlugin;
namespace mc = khensu::mixerchain;

namespace {
Plugin plug(const char* name, const char* category, const char* settings)
{
    return Plugin{name, category, settings, "stock"};
}

float at(const mc::RecipeDspMapping& m, const char* id)
{
    const auto it = m.values.find(id);
    REQUIRE(it != m.values.end());
    return it->second;
}

float asFloat(mc::EqBandShape s) { return static_cast<float>(static_cast<int>(s)); }
float asFloat(mc::EqBandKind k) { return static_cast<float>(static_cast<int>(k)); }
float asFloat(mc::EqBandChannels c) { return static_cast<float>(static_cast<int>(c)); }

/// Seven EQ moves plus a frequency-only high cut, over two EQ plugins.
std::vector<Plugin> sevenMoveRecipe()
{
    return {
        plug("Pro-Q", "EQ", "Low Shelf: 100 Hz +1 dB, Bell: 400 Hz -4 dB, Presence: 3 kHz +2.5 dB, Air: 12 kHz +3 dB"),
        plug("Pultec", "EQ", "LF Boost: 60 Hz +5 dB, HF Cut: 16 kHz, Low Mid Cut: 250 Hz -1.5 dB, Notch: 1 kHz"),
    };
}

struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    Fixture() { proc.setPlayConfigDetails(2, 2, 48000.0, 512); }
};
} // namespace

TEST_CASE("A recipe with more than six EQ moves fills six bands in importance order (STU-17)",
          "[plugin][stu17][recipe]")
{
    const auto m = mc::mapRecipeToDsp(sevenMoveRecipe());
    const auto& b = mc::kEqBandParamIds;
    // Notch (a filter counts as 6 dB), +5, -4, +3, +2.5, -1.5; the +1 dB shelf is left out.
    CHECK(at(m, b[0].kind) == asFloat(mc::EqBandKind::Notch));
    CHECK(at(m, b[0].freq) == Catch::Approx(1000.0f));
    CHECK(at(m, b[1].freq) == Catch::Approx(60.0f));
    CHECK(at(m, b[1].gain) == Catch::Approx(5.0f));
    CHECK(at(m, b[1].type) == asFloat(mc::EqBandShape::LowShelf));
    CHECK(at(m, b[2].freq) == Catch::Approx(400.0f));
    CHECK(at(m, b[2].gain) == Catch::Approx(-4.0f));
    CHECK(at(m, b[2].type) == asFloat(mc::EqBandShape::Bell));
    CHECK(at(m, b[3].freq) == Catch::Approx(12000.0f));
    CHECK(at(m, b[3].type) == asFloat(mc::EqBandShape::HighShelf));
    CHECK(at(m, b[4].freq) == Catch::Approx(3000.0f));
    CHECK(at(m, b[4].gain) == Catch::Approx(2.5f));
    CHECK(at(m, b[5].freq) == Catch::Approx(250.0f));
    CHECK(at(m, b[5].gain) == Catch::Approx(-1.5f)); // "Cut" in the label
    for (const auto& band : b) {
        CHECK(at(m, band.on) == 1.0f);
        CHECK(at(m, band.mode) == asFloat(mc::EqBandChannels::Stereo));
        CHECK(at(m, band.dynOn) == 0.0f);
    }
    for (int i = 1; i < mc::kNumEqBands; ++i) CHECK(at(m, b[i].kind) == asFloat(mc::EqBandKind::Shape));
    // The gain-less high cut went to the low-pass stage.
    CHECK(at(m, P::lpfOn) == 1.0f);
    CHECK(at(m, P::lpfFreq) == Catch::Approx(16000.0f));
    // The seventh move is reported, not silently dropped.
    REQUIRE(m.skipped.size() == 1);
    CHECK(m.skipped[0].plugin == "Pro-Q");
    CHECK(m.skipped[0].reason == mc::RecipeSkipReason::NoFreeEqBand);
    CHECK(m.skipped[0].detail.find("100 Hz") != std::string::npos);
    CHECK(m.mappedPlugins == std::vector<std::string>{"Pro-Q", "Pultec"});
}

TEST_CASE("A filter cut takes a band once its HPF / LPF stage is used (STU-17)", "[plugin][stu17][recipe]")
{
    const auto m = mc::mapRecipeToDsp(
        {plug("EQ Eight", "EQ", "HPF: 30 Hz, Sub Cut: 90 Hz, Presence Boost: 3.5 kHz +2 dB")});
    CHECK(at(m, P::hpfFreq) == Catch::Approx(30.0f));
    const auto& b = mc::kEqBandParamIds;
    CHECK(at(m, b[0].kind) == asFloat(mc::EqBandKind::HighPass)); // 6 dB worth: above the +2 dB bell
    CHECK(at(m, b[0].freq) == Catch::Approx(90.0f));
    CHECK(at(m, b[1].freq) == Catch::Approx(3500.0f));
    CHECK(m.values.count(b[2].on) == 0);
}

TEST_CASE("Dynamic EQ and M/S entries map to the dynamic and mid / side modes (STU-17)", "[plugin][stu17][recipe]")
{
    const auto m = mc::mapRecipeToDsp({
        plug("Pro-Q 3", "Dynamic EQ", "Harsh: 3 kHz -4 dB, Threshold: -24 dB"),
        plug("Fabfilter M/S", "M/S EQ", "Side: 200 Hz -3 dB, Mid: 2 kHz +1.5 dB"),
        plug("EQ Eight", "EQ", "Air: 12 kHz +2 dB (side), Dynamic cut: 6 kHz -2.5 dB threshold -30 dB"),
    });
    const auto& b = mc::kEqBandParamIds;
    // Band 1: the dynamic cut - a 0 dB bell that turns down by up to 4 dB over -24 dB.
    CHECK(at(m, b[0].freq) == Catch::Approx(3000.0f));
    CHECK(at(m, b[0].dynOn) == 1.0f);
    CHECK(at(m, b[0].gain) == Catch::Approx(0.0f));
    CHECK(at(m, b[0].dynThreshDb) == Catch::Approx(-24.0f));
    CHECK(at(m, b[0].dynRangeDb) == Catch::Approx(4.0f));
    CHECK(at(m, b[0].mode) == asFloat(mc::EqBandChannels::Stereo));
    // Band 2: the M/S EQ's side cut; band 3: the inline dynamic cut.
    CHECK(at(m, b[1].freq) == Catch::Approx(200.0f));
    CHECK(at(m, b[1].mode) == asFloat(mc::EqBandChannels::Side));
    CHECK(at(m, b[1].dynOn) == 0.0f);
    CHECK(at(m, b[2].freq) == Catch::Approx(6000.0f));
    CHECK(at(m, b[2].dynOn) == 1.0f);
    CHECK(at(m, b[2].dynThreshDb) == Catch::Approx(-30.0f));
    CHECK(at(m, b[2].dynRangeDb) == Catch::Approx(2.5f));
    // Band 4: the side air shelf (the "(side)" aside is kept); band 5: the mid lift.
    CHECK(at(m, b[3].freq) == Catch::Approx(12000.0f));
    CHECK(at(m, b[3].type) == asFloat(mc::EqBandShape::HighShelf));
    CHECK(at(m, b[3].mode) == asFloat(mc::EqBandChannels::Side));
    CHECK(at(m, b[4].freq) == Catch::Approx(2000.0f));
    CHECK(at(m, b[4].mode) == asFloat(mc::EqBandChannels::Mid));
    CHECK(m.values.count(b[5].on) == 0);
    CHECK(m.skipped.empty());
}

TEST_CASE("Width, saturation, transient and bass mono recipes set their v5 stages (STU-17)",
          "[plugin][stu17][recipe]")
{
    const auto m = mc::mapRecipeToDsp({
        plug("Ozone Imager", "Stereo Imager", "Width: 120%, Mono Below: 100 Hz"),
        plug("Decapitator", "Saturation", "Drive: 30%, Mix: 50%"),
        plug("SPL Transient Designer", "Transient Shaper", "Attack: +4 dB, Sustain: -2 dB"),
        plug("Aphex", "Exciter", "Amount: 50%"), // a second saturation: the stage is taken
    });
    CHECK(at(m, P::widthOn) == 1.0f);
    CHECK(at(m, P::widthAmount) == Catch::Approx(1.2f));
    CHECK(at(m, P::bassMonoOn) == 1.0f);
    CHECK(at(m, P::bassMonoFreq) == Catch::Approx(100.0f));
    CHECK(at(m, P::satOn) == 1.0f);
    CHECK(at(m, P::satDriveDb) == Catch::Approx(30.0f * 0.18f));
    CHECK(at(m, P::satMix) == Catch::Approx(0.5f));
    CHECK(at(m, P::tsOn) == 1.0f);
    CHECK(at(m, P::tsAttackDb) == Catch::Approx(4.0f));
    CHECK(at(m, P::tsSustainDb) == Catch::Approx(-2.0f));
    REQUIRE(m.skipped.size() == 1);
    CHECK(m.skipped[0].plugin == "Aphex");
    CHECK(m.skipped[0].reason == mc::RecipeSkipReason::StageTaken);
    CHECK(m.skipped[0].detail.find("Decapitator") != std::string::npos);

    // On their own: an exciter is the saturation stage, clamped to its range;
    // a mono maker with no crossover uses the sub-bass default; "+20%" widens
    // from unchanged; a master desk's "Width: 100%" changes nothing.
    const auto hot = mc::mapRecipeToDsp({plug("Exciter", "Exciter", "Drive: 40 dB")});
    CHECK(at(hot, P::satDriveDb) == Catch::Approx(24.0f));
    const auto mono = mc::mapRecipeToDsp({plug("Mono Maker", "Bass Mono", "")});
    CHECK(at(mono, P::bassMonoFreq) == Catch::Approx(mc::kSubBassMonoHz));
    const auto wider = mc::mapRecipeToDsp({plug("Widener", "Stereo Widener", "Width: +20%")});
    CHECK(at(wider, P::widthAmount) == Catch::Approx(1.2f));
    const auto desk = mc::mapRecipeToDsp({plug("bx_masterdesk", "Master Desk", "Width: 100%, Density: 30%")});
    CHECK(desk.values.count(P::widthOn) == 0);
}

TEST_CASE("What a recipe cannot map is listed as skipped, with the reason (STU-17)", "[plugin][stu17][recipe]")
{
    const std::vector<Plugin> recipe = {
        plug("UAD 1176", "Compressor", "Ratio: 4:1, Attack: 3ms, Threshold: -18 dB"),
        plug("LA-2A", "Compressor", "Peak Reduction: 40"),
        plug("Valhalla Room", "Reverb", "Decay: 1.2s, Mix: 15%"),
        plug("Pro-MB", "Multiband Compressor", "Low band: -3 dB"),
        plug("RX De-noise", "Denoise", "Reduction: 6 dB"),
        plug("Shadow Hills", "Transient Shaping", "Optical: -3 dB GR"),
    };
    const auto m = mc::mapRecipeToDsp(recipe);
    CHECK(at(m, P::compOn) == 1.0f);
    CHECK(at(m, P::compRatio) == Catch::Approx(4.0f));
    CHECK(m.values.count(P::compMultiband) == 0);
    CHECK(m.mappedPlugins == std::vector<std::string>{"UAD 1176"});

    const auto batch = khensu::copilot::recipeBatch(m);
    CHECK(batch.appliedIds == std::vector<std::string>{"UAD 1176"});
    REQUIRE(batch.skipped.size() == 5);
    CHECK(batch.skipped[0].what == "LA-2A");
    CHECK(batch.skipped[0].reason.contains("already set that stage"));
    CHECK(batch.skipped[0].reason.contains("UAD 1176"));
    CHECK(batch.skipped[1].what == "Valhalla Room");
    CHECK(batch.skipped[1].reason.contains("no stage for it"));
    CHECK(batch.skipped[1].reason.contains("Reverb"));
    CHECK(batch.skipped[2].reason.contains("Multiband Compressor"));
    CHECK(batch.skipped[3].reason.contains("no stage for it"));
    CHECK(batch.skipped[4].reason.contains("no setting in its description could be read"));

    // Through the editor: one apply, the strip says what went in and what was skipped.
    Fixture f;
    khensu::KhensuCopilotEditor ed(f.proc);
    CHECK(ed.applyChainRecipe(m));
    CHECK(ed.applyStripTextForTesting().startsWith("Loaded 1 plugin into the DSP chain"));
    CHECK(ed.applyStripTextForTesting().contains("5 skipped"));
    const auto details = ed.applyStripDetailsForTesting();
    CHECK(details.contains("Skipped:"));
    CHECK(details.contains("Valhalla Room"));
    CHECK(details.contains("Compressor"));

    // Nothing mappable: the chain is left alone and the strip says so.
    CHECK_FALSE(ed.applyChainRecipe(mc::mapRecipeToDsp({recipe[2]})));
    CHECK(ed.applyStripTextForTesting().contains("1 skipped"));
    CHECK(f.proc.getParamPlain(P::compOn) > 0.5f);
}

TEST_CASE("Load into DSP chain applies all six bands and the stages, one undo away (STU-17)",
          "[plugin][stu17][recipe]")
{
    Fixture f;
    khensu::KhensuCopilotEditor ed(f.proc);
    auto recipe = sevenMoveRecipe();
    recipe.push_back(plug("Imager", "Stereo Imager", "Width: 80%"));
    recipe.push_back(plug("Tape", "Tape Saturation", "Drive: 4 dB"));
    khensu::ipc::HttpServiceBridge::MixerChainResponse response;
    response.plugins = recipe;
    ed.mixerChainPanelForTesting().loadRecipeForTesting(response);

    const auto& b = mc::kEqBandParamIds;
    for (const auto& band : b) CHECK(f.proc.getParamPlain(band.on) > 0.5f);
    CHECK(f.proc.getParamPlain(b[0].kind) == Catch::Approx(asFloat(mc::EqBandKind::Notch)));
    CHECK(f.proc.getParamPlain(b[5].freq) == Catch::Approx(250.0f).margin(1.0f));
    CHECK(f.proc.getParamPlain(P::widthOn) > 0.5f);
    CHECK(f.proc.getParamPlain(P::widthAmount) == Catch::Approx(0.8f).margin(0.01f));
    CHECK(f.proc.getParamPlain(P::satDriveDb) == Catch::Approx(4.0f).margin(0.05f));
    CHECK(f.proc.isAiSet(b[5].on));
    CHECK(ed.applyStripTextForTesting().contains("1 skipped"));
    CHECK(ed.mixerChainPanelForTesting().statusTextForTesting().contains("1 skipped"));

    ed.undoLastApplyForTesting();
    CHECK(f.proc.getParamPlain(b[5].on) < 0.5f);
    CHECK(f.proc.getParamPlain(P::widthOn) < 0.5f);
}
