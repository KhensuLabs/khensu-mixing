// tests/plugin/CopilotApplyAllUnmaskTest.cpp
// ENH-10 (3) "Apply all safe fixes" (the batch mapping, one undo step, the
// Amount knob re-applying all of them, the skipped list) and (4) the Mix
// Hub's Unmask (the cut it computes and the cross-instance write into the
// other instance's EQ, with removal).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/FixBatch.h"
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/mixhub/MixHubAnalysis.h"

using khensu::KhensuCopilotProcessor;
using khensu::Recommendation;
using P = khensu::KhensuCopilotProcessor::ParamIDs;

namespace {
Recommendation rec(const std::string& id, bool safe, std::map<std::string, float> params, float severity = 50.0f)
{
    Recommendation r;
    r.id = id;
    r.what = "Finding " + id;
    r.severity = severity;
    r.safe_to_auto_apply = safe;
    r.suggested_parameters = std::move(params);
    return r;
}

std::vector<Recommendation> sampleFindings()
{
    return {
        rec("harsh", true, {{"eq_freq_hz", 3000.0f}, {"eq_gain_db", -4.0f}, {"eq_q", 1.4f}}, 80.0f),
        rec("thin", true, {{"eq_freq_hz", 200.0f}, {"eq_gain_db", 2.0f}}, 70.0f),
        rec("taste", false, {{"eq_freq_hz", 8000.0f}, {"eq_gain_db", 3.0f}}, 65.0f),
        rec("dynamic", true, {{"comp_threshold_db", -18.0f}, {"comp_ratio", 3.0f}}, 60.0f),
        rec("squash", true, {{"comp_threshold_db", -30.0f}, {"comp_ratio", 8.0f}}, 55.0f),
        rec("narrow", true, {{"stereo_width", 1.3f}}, 50.0f),
        rec("loud", true, {{"reduce_gain_db", 1.5f}}, 40.0f),
    };
}

float plain(KhensuCopilotProcessor& proc, const char* id) { return proc.getParamPlain(id); }

void setPlain(KhensuCopilotProcessor& proc, const char* id, float value)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(value));
}

void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }

struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    Fixture() { proc.setPlayConfigDetails(2, 2, 48000.0, 512); }
};

} // namespace

TEST_CASE("Apply all maps every safe fix onto its own stage and says what it skipped (ENH-10)",
          "[plugin][enh10][applyall]")
{
    const auto none = [](const std::string&) { return 0.0f; };
    const auto batch = khensu::copilot::combineFixes(sampleFindings(), 1.0f, none, true);
    // STU-10: the width fix maps onto the width stage now.
    CHECK(batch.appliedIds == std::vector<std::string>{"harsh", "thin", "dynamic", "narrow", "loud"});
    // Each EQ move on its own band, in order.
    CHECK(batch.values.at("eq_on") == 1.0f);
    CHECK(batch.values.at("eq_freq") == Catch::Approx(3000.0f));
    CHECK(batch.values.at("eq_gain_db") == Catch::Approx(-4.0f));
    CHECK(batch.values.at("eq2_on") == 1.0f);
    CHECK(batch.values.at("eq2_freq") == Catch::Approx(200.0f));
    CHECK(batch.values.at("comp_ratio") == Catch::Approx(3.0f));
    CHECK(batch.values.at("out_gain_db") == Catch::Approx(-1.5f));
    CHECK(batch.values.at("width_on") == 1.0f);
    CHECK(batch.values.at("width_amount") == Catch::Approx(1.3f));
    REQUIRE(batch.skipped.size() == 2);
    CHECK(batch.skipped[0].id == "taste");
    CHECK(batch.skipped[0].reason.contains("judgement"));
    CHECK(batch.skipped[1].id == "squash"); // the first compressor setting wins
    CHECK(batch.skipped[1].reason.contains("overwrite"));

    // A key the chain still cannot render is named in the reason.
    const auto denoise = khensu::copilot::combineFixes({rec("hiss", true, {{"denoise_reduction_db", 6.0f}})}, 1.0f,
                                                       none, true);
    REQUIRE(denoise.skipped.size() == 1);
    CHECK(denoise.skipped[0].reason.contains("cannot render"));
    CHECK(denoise.skipped[0].reason.contains("denoise_reduction_db")); // says which
}

TEST_CASE("Apply all is one undo step and stays live under Amount (ENH-10)", "[plugin][enh10][applyall]")
{
    Fixture f;
    setPlain(f.proc, P::outGainDb, 1.0f);
    f.proc.undoHistory().clear();
    const auto result = f.proc.applyAllSafeFixes(sampleFindings());
    REQUIRE(result.applied);
    CHECK(f.proc.getLiveFixCount() == 5);
    CHECK(f.proc.isLiveFix("harsh"));
    CHECK(f.proc.isLiveFix("loud"));
    CHECK_FALSE(f.proc.isLiveFix("taste"));
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-4.0f).margin(0.05f));
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(2.0f).margin(0.05f));
    CHECK(plain(f.proc, P::compOn) > 0.5f);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-0.5f).margin(0.05f)); // relative: 1 - 1.5
    CHECK(f.proc.isAiSet(P::eq2GainDb));

    // Amount re-applies every one of them.
    setPlain(f.proc, P::fixAmount, 0.5f);
    pump();
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-2.0f).margin(0.05f));
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(1.0f).margin(0.05f));
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(0.25f).margin(0.06f)); // the parameter steps 0.1 dB
    setPlain(f.proc, P::fixAmount, 1.0f);
    pump();

    // One step: one undo puts every parameter back and ends the live fix.
    const auto names = f.proc.undoHistory().undoNames();
    REQUIRE_FALSE(names.isEmpty());
    CHECK(names[0].startsWith("Apply all safe fixes"));
    f.proc.undoHistory().undo();
    CHECK(plain(f.proc, P::eqOn) < 0.5f);
    CHECK(plain(f.proc, P::eq2On) < 0.5f);
    CHECK(plain(f.proc, P::compOn) < 0.5f);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(1.0f).margin(0.05f));
    CHECK(f.proc.getLiveFixCount() == 0);
}

TEST_CASE("The editor's Apply all lists the skipped findings (ENH-10)", "[plugin][enh10][applyall]")
{
    Fixture f;
    khensu::KhensuCopilotEditor ed(f.proc);
    CHECK(ed.applyAllFixes(sampleFindings()));
    const auto text = ed.applyStripTextForTesting();
    CHECK(text.startsWith("Applied 5 fixes"));
    CHECK(text.contains("2 skipped"));
    ed.undoLastApplyForTesting();
    CHECK(plain(f.proc, P::eqOn) < 0.5f);

    // Nothing safe: nothing applied, and the strip says so.
    CHECK_FALSE(ed.applyAllFixes({rec("taste", false, {{"eq_freq_hz", 8000.0f}, {"eq_gain_db", 3.0f}})}));
    CHECK(ed.applyStripTextForTesting().contains("1 skipped"));
}

TEST_CASE("Unmask cut follows the masking overlap, bounded (ENH-10)", "[plugin][enh10][unmask]")
{
    khensu::mixhub::MaskingConflict c;
    c.instanceA = 1;
    c.instanceB = 2;
    c.band = 12;
    c.bandLo = 11;
    c.bandHi = 13;
    c.freqHz = khensu::mixhub::bandCentreHz(12);
    c.levelADb = -20.0f;
    c.levelBDb = -20.0f; // fully overlapping: the deepest cut
    auto cut = khensu::mixhub::unmaskCutFor(c);
    CHECK(cut.gainDb == Catch::Approx(-khensu::mixhub::kMaxUnmaskCutDb));
    CHECK(cut.freqHz == Catch::Approx(c.freqHz));
    CHECK(cut.q >= 0.7f);
    CHECK(cut.q <= 4.0f);
    c.levelBDb = -26.0f; // at the edge of the masking window: a light cut
    cut = khensu::mixhub::unmaskCutFor(c);
    CHECK(cut.gainDb == Catch::Approx(-1.0f));
    c.bandLo = c.bandHi = 12; // a narrower overlap: a narrower bell
    CHECK(khensu::mixhub::unmaskCutFor(c).q > cut.q);
}

TEST_CASE("Unmask writes the cut into the other instance's EQ, and removes it (ENH-10)", "[plugin][enh10][unmask]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor vocal, bed;
    vocal.setPlayConfigDetails(2, 2, 48000.0, 512);
    bed.setPlayConfigDetails(2, 2, 48000.0, 512);
    // The bed's band 6 is already in use: the cut takes band 5 (STU-10: the
    // cuts take the bands from the top down).
    setPlain(bed, P::eq6On, 1.0f);
    setPlain(bed, P::eq6GainDb, 1.5f);
    // Band 5 is off but was left a side-only high-pass with dynamics on.
    const auto& band5 = khensu::mixerchain::kEqBandParamIds[4];
    setPlain(bed, band5.kind, 2.0f);
    setPlain(bed, band5.mode, 2.0f);
    setPlain(bed, band5.dynOn, 1.0f);

    khensu::mixhub::MaskingConflict c;
    c.instanceA = vocal.getMixHubInstanceId();
    c.instanceB = bed.getMixHubInstanceId();
    c.nameA = "Vocal";
    c.nameB = "Music bed";
    c.band = c.bandLo = c.bandHi = 14;
    c.freqHz = 2500.0f;
    c.levelADb = -18.0f;
    c.levelBDb = -19.0f;

    juce::String message;
    REQUIRE(vocal.requestUnmask(c, message));
    CHECK(message.contains("Music bed"));
    auto& hub = vocal.getMixHub();
    CHECK(hub.hasPendingUnmask(c.instanceA, c.instanceB));

    // The vocal instance does not apply its own request; the bed does.
    vocal.processMixHubCommands();
    CHECK(hub.hasPendingUnmask(c.instanceA, c.instanceB));
    bed.processMixHubCommands();
    CHECK_FALSE(hub.hasPendingUnmask(c.instanceA, c.instanceB));

    const auto expected = khensu::mixhub::unmaskCutFor(c);
    CHECK(bed.unmaskBandFor(c.instanceA) == 5);
    CHECK(plain(bed, P::eq5On) > 0.5f);
    CHECK(plain(bed, P::eq5Freq) == Catch::Approx(2500.0f).margin(5.0f));
    CHECK(plain(bed, P::eq5GainDb) == Catch::Approx(expected.gainDb).margin(0.05f));
    CHECK(plain(bed, P::eq5GainDb) >= -khensu::mixhub::kMaxUnmaskCutDb - 0.01f);
    CHECK(plain(bed, P::eq5Q) == Catch::Approx(expected.q).margin(0.05f));
    CHECK(plain(bed, P::eq5Type) == Catch::Approx(0.0f)); // a bell
    CHECK(plain(bed, band5.kind) == 0.0f);                // a plain stereo, static band (STU-10)
    CHECK(plain(bed, band5.mode) == 0.0f);
    CHECK(plain(bed, band5.dynOn) < 0.5f);
    CHECK(plain(bed, P::eq6GainDb) == Catch::Approx(1.5f).margin(0.05f)); // untouched
    CHECK(plain(vocal, P::eq5On) < 0.5f); // the asking track is not cut
    const auto status = hub.unmaskStatus(c.instanceA, c.instanceB);
    REQUIRE(status.has_value());
    CHECK(status->applied);
    CHECK(status->band == 5);

    // The bed's undo takes the cut out as one step.
    CHECK(bed.undoHistory().undoNames()[0].startsWith("Unmask cut for"));

    // A second Unmask updates the same band instead of taking another.
    c.levelBDb = -23.0f;
    REQUIRE(vocal.requestUnmask(c, message));
    bed.processMixHubCommands();
    CHECK(bed.unmaskBandFor(c.instanceA) == 5);
    CHECK(plain(bed, P::eqOn) < 0.5f);
    CHECK(plain(bed, P::eq5GainDb) == Catch::Approx(khensu::mixhub::unmaskCutFor(c).gainDb).margin(0.05f));

    // Remove: the band goes back to how it was.
    vocal.requestRemoveUnmask(c.instanceB);
    bed.processMixHubCommands();
    CHECK(bed.unmaskBandFor(c.instanceA) == 0);
    CHECK(plain(bed, P::eq5On) < 0.5f);
    CHECK(plain(bed, P::eq5GainDb) == Catch::Approx(0.0f).margin(0.05f));
    CHECK(plain(bed, band5.kind) == 2.0f); // and its kind, channels and dynamics
    CHECK(plain(bed, band5.mode) == 2.0f);
    CHECK(plain(bed, band5.dynOn) > 0.5f);
    CHECK_FALSE(hub.unmaskStatus(c.instanceA, c.instanceB).has_value());

    // A pair without this instance is refused.
    khensu::mixhub::MaskingConflict other = c;
    other.instanceA = 991;
    other.instanceB = 992;
    CHECK_FALSE(vocal.requestUnmask(other, message));

    // No free band: the target says so instead of overwriting one.
    for (const auto& band : khensu::mixerchain::kEqBandParamIds) setPlain(bed, band.on, 1.0f);
    REQUIRE(vocal.requestUnmask(c, message));
    bed.processMixHubCommands();
    const auto refused = hub.unmaskStatus(c.instanceA, c.instanceB);
    REQUIRE(refused.has_value());
    CHECK_FALSE(refused->applied);
    CHECK(refused->note.contains("No free EQ band"));
}
