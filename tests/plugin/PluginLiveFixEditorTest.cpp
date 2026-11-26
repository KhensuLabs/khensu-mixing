// tests/plugin/PluginLiveFixEditorTest.cpp
// OVH-8: the live top fix (shared mapper, safe_to_auto_apply gate, Amount
// re-applies, Undo); UI-8: Compact view and the Small / Medium / Large sizes,
// persisted per instance; UI-5: the telemetry the new meters read (true peak,
// gated integrated loudness, the vectorscope's sample tap). Writes a PNG of
// the editor in every view to build/agentB-shots/ for review.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotRealtimeTelemetry.h"
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"

#include <cmath>

using khensu::CopilotRealtimeTelemetry;
using khensu::KhensuCopilotEditor;
using khensu::KhensuCopilotProcessor;
using khensu::Recommendation;
using P = khensu::KhensuCopilotProcessor::ParamIDs;
using ViewSize = khensu::KhensuCopilotProcessor::ViewSize;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    Fixture() { proc.setPlayConfigDetails(2, 2, 48000.0, 512); }
};

float plain(KhensuCopilotProcessor& proc, const char* id) { return proc.getParamPlain(id); }

void setPlain(KhensuCopilotProcessor& proc, const char* id, float value)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(value));
}

/// Lets the processor's async re-apply run.
void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }

Recommendation eqFix(bool safe)
{
    Recommendation rec;
    rec.id = "harsh-3k";
    rec.what = "Harsh upper mids around 3 kHz";
    rec.suggested_action = "Cut 3 kHz by 6 dB";
    rec.severity = 60.0f;
    rec.safe_to_auto_apply = safe;
    rec.auto_apply_rationale = safe ? "" : "Tonal change; check by ear";
    rec.suggested_parameters = {{"eq_freq_hz", 3000.0f}, {"eq_gain_db", -6.0f}, {"eq_q", 1.4f}};
    return rec;
}

juce::File shotsDir()
{
    // <build>/tests/Release/KhensuPluginTests.exe -> <build>/agentB-shots
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getParentDirectory()
                   .getParentDirectory()
                   .getChildFile("agentB-shots");
    dir.createDirectory();
    return dir;
}

bool snapshotHasContent(KhensuCopilotEditor& ed, const juce::String& name)
{
    const auto image = ed.createComponentSnapshot(ed.getLocalBounds(), true, 1.0f);
    juce::PNGImageFormat png;
    const auto file = shotsDir().getChildFile(name + ".png");
    file.deleteFile();
    {
        juce::FileOutputStream out(file);
        if (out.openedOk()) png.writeImageToStream(image, out);
    }
    const auto corner = image.getPixelAt(0, 0);
    int differing = 0;
    for (int y = 0; y < image.getHeight(); y += 12)
        for (int x = 0; x < image.getWidth(); x += 12)
            if (image.getPixelAt(x, y) != corner) ++differing;
    return differing > 50;
}
} // namespace

TEST_CASE("A fix not marked safe is not auto-applied, and says why (OVH-8)", "[plugin][ovh8][livefix]")
{
    Fixture f;
    const float before = plain(f.proc, P::eqGainDb);
    auto result = f.proc.applyLiveFix(eqFix(false));
    CHECK_FALSE(result.applied);
    CHECK(result.blockedReason.contains("Not applied automatically"));
    CHECK(result.blockedReason.contains("check by ear"));
    CHECK_FALSE(result.mapped.empty()); // what it would do is still known
    CHECK(plain(f.proc, P::eqGainDb) == before);
    CHECK_FALSE(f.proc.hasLiveFix());

    result = f.proc.applyLiveFix(eqFix(false), false); // the user's explicit override
    CHECK(result.applied);
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-6.0f).margin(0.05f));
}

TEST_CASE("Turning Amount re-applies the live fix over its base (OVH-8)", "[plugin][ovh8][livefix]")
{
    Fixture f;
    setPlain(f.proc, P::eqOn, 1.0f); // band 1 busy: the fix takes band 2
    setPlain(f.proc, P::outGainDb, 1.0f);
    auto rec = eqFix(true);
    rec.suggested_parameters["reduce_gain_db"] = 2.0f;
    const auto result = f.proc.applyLiveFix(rec);
    REQUIRE(result.applied);
    CHECK(f.proc.hasLiveFix());
    CHECK(f.proc.getLiveFixId() == "harsh-3k");
    CHECK(plain(f.proc, P::eq2On) > 0.5f);
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(-6.0f).margin(0.05f));
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-1.0f).margin(0.05f));
    CHECK(f.proc.isAiSet(P::eq2GainDb));

    setPlain(f.proc, P::fixAmount, 0.5f); // the knob / host automation
    pump();
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(-3.0f).margin(0.05f));
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(0.0f).margin(0.05f)); // 1 - 2 x 0.5

    // Pressing Apply again does not stack the fix on itself.
    f.proc.applyLiveFix(rec);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(0.0f).margin(0.05f));

    setPlain(f.proc, P::fixAmount, 0.0f); // 0 %: the touched parameters go back
    pump();
    CHECK(plain(f.proc, P::eq2On) < 0.5f);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(1.0f).margin(0.05f));
    CHECK(plain(f.proc, P::eqOn) > 0.5f); // never touched

    f.proc.clearLiveFix();
    setPlain(f.proc, P::fixAmount, 1.0f);
    pump();
    CHECK(plain(f.proc, P::eq2On) < 0.5f); // no live fix, nothing re-applied
}

TEST_CASE("Editor Auto-Apply goes through the mapper, gates unsafe fixes and undoes (OVH-8 / UX-10)",
          "[plugin][ovh8][ux10]")
{
    Fixture f;
    KhensuCopilotEditor ed(f.proc);
    const float before = f.proc.getAPVTS().getParameter(P::eqGainDb)->getValue();

    CHECK_FALSE(ed.applySuggestion(eqFix(false)));
    CHECK(ed.applyStripTextForTesting().contains("Not applied automatically"));
    CHECK(f.proc.getAPVTS().getParameter(P::eqGainDb)->getValue() == before);
    ed.applyAnywayForTesting(); // the strip's Apply anyway
    CHECK(ed.applyStripTextForTesting().startsWith("Applied 3 parameters"));
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-6.0f).margin(0.05f));

    ed.undoLastApplyForTesting();
    CHECK(f.proc.getAPVTS().getParameter(P::eqGainDb)->getValue() == before);
    CHECK_FALSE(f.proc.hasLiveFix()); // Amount no longer drives it
}

TEST_CASE("Apply top fix picks the most severe open finding the chain can fix (OVH-8)", "[plugin][ovh8][topfix]")
{
    Fixture f;
    KhensuCopilotEditor ed(f.proc);
    Recommendation advice;
    advice.id = "hiss";
    advice.what = "Noisy";
    advice.severity = 90.0f;
    advice.safe_to_auto_apply = true;
    // Nothing the chain can do (stereo_width maps onto the width stage since STU-10).
    advice.suggested_parameters = {{"denoise_reduction_db", 6.0f}};
    Recommendation minor = eqFix(true);
    minor.id = "minor";
    minor.severity = 20.0f;
    const auto top = eqFix(true);
    auto& control = ed.topFixControl();
    control.setRecommendations({advice, minor, top}, nullptr);
    REQUIRE(control.topFix().has_value());
    CHECK(control.topFix()->id == "harsh-3k");
    CHECK(control.findingTextForTesting() == "Harsh upper mids around 3 kHz");

    control.applyButton().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    CHECK(f.proc.getLiveFixId() == "harsh-3k");
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-6.0f).margin(0.05f));
    control.refresh();
    CHECK(control.reasonTextForTesting().startsWith("Live"));

    // The Amount knob is the fix_amount parameter (host gestures).
    auto& knob = control.amountKnob();
    knob.setValue(0.25, juce::sendNotificationSync);
    pump();
    CHECK(plain(f.proc, P::fixAmount) == Catch::Approx(0.25f).margin(0.01f));
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-1.5f).margin(0.05f));
    // Shift-drag moves 1/8 as far as a plain drag.
    knob.setValue(0.5, juce::sendNotificationSync);
    const double coarse = knob.simulateDragForTest(0.0f, -22.0f, false) - 0.5;
    knob.setValue(0.5, juce::sendNotificationSync);
    const double fine = knob.simulateDragForTest(0.0f, -22.0f, true) - 0.5;
    CHECK(coarse == Catch::Approx(0.1).margin(0.011));
    CHECK(fine == Catch::Approx(coarse / 8.0).margin(0.003));
}

TEST_CASE("Compact and the three full sizes, persisted per instance (UI-8)", "[plugin][ui8]")
{
    Fixture f;
    {
        KhensuCopilotEditor ed(f.proc);
        CHECK(ed.getWidth() == KhensuCopilotEditor::kMediumWidth); // the default
        CHECK(snapshotHasContent(ed, "copilot-medium-960x720"));

        ed.setView(false, ViewSize::Small);
        CHECK(ed.getWidth() == 720);
        CHECK(ed.getHeight() == 540);
        CHECK(snapshotHasContent(ed, "copilot-small-720x540"));

        ed.setView(false, ViewSize::Large);
        CHECK(ed.getWidth() == 1280);
        CHECK(ed.getHeight() == 960);
        CHECK(snapshotHasContent(ed, "copilot-large-1280x960"));

        ed.selectTabByShortcut(1); // DSP
        CHECK(snapshotHasContent(ed, "copilot-large-dsp-tab"));
        ed.selectTabByShortcut(2);
        CHECK(f.proc.getEditorViewState().tab == 2);

        ed.setView(true, ViewSize::Large);
        CHECK(ed.isCompactView());
        CHECK(ed.getWidth() == KhensuCopilotEditor::kCompactWidth);
        CHECK(ed.getHeight() == KhensuCopilotEditor::kCompactHeight);
        CHECK(ed.compactTopFixControl().isVisible());
        CHECK(ed.compactFindings().isVisible());
        CHECK_FALSE(ed.findChildWithID("tabs")->isVisible());
        CHECK(ed.findChildWithID("comparisonStrip")->isVisible());
        // Everything compact lays out inside the window.
        for (auto* child : ed.getChildren())
            if (child->isVisible()) {
                INFO(child->getTitle() << " / " << child->getComponentID());
                CHECK(ed.getLocalBounds().contains(child->getBounds()));
            }
        // With findings loaded, as a user sees it.
        Recommendation a = eqFix(true), b = eqFix(false), c = eqFix(true);
        b.id = "b";
        b.what = "Sibilance on the lead vocal";
        b.severity = 75.0f;
        b.suggested_parameters = {{"deess_threshold_db", -28.0f}, {"deess_freq_hz", 7000.0f}};
        c.id = "c";
        c.what = "Low end a little light for the genre";
        c.severity = 30.0f;
        ed.compactFindings().setFindings({b, a, c});
        ed.compactTopFixControl().setRecommendations({a, b, c}, nullptr);
        CHECK(ed.compactFindings().numFindings() == 3);
        CHECK(snapshotHasContent(ed, "copilot-compact-560x380"));
    }
    // A new editor (the host reopening it) comes back compact on the same tab.
    const auto saved = f.proc.getEditorViewState();
    CHECK(saved.compact);
    CHECK(saved.size == ViewSize::Large);
    KhensuCopilotEditor reopened(f.proc);
    CHECK(reopened.isCompactView());
    CHECK(reopened.getCurrentTabIndex() == 2);
    reopened.setView(false, saved.size);
    CHECK(reopened.getWidth() == 1280);
}

TEST_CASE("Telemetry reads inter-sample true peaks per channel (UI-5)", "[plugin][ui5][truepeak]")
{
    CopilotRealtimeTelemetry telemetry;
    telemetry.prepareToPlay(48000.0, 2);
    // fs/4 at 45 degrees: every sample sits at 0.707 of the waveform's peak.
    juce::AudioBuffer<float> buffer(2, 480);
    long long n = 0;
    for (int b = 0; b < 20; ++b) {
        for (int i = 0; i < 480; ++i) {
            const double phase = kPi / 2.0 * static_cast<double>(n + i) + kPi / 4.0;
            buffer.setSample(0, i, static_cast<float>(0.5 * std::sin(phase)));
            buffer.setSample(1, i, static_cast<float>(0.25 * std::sin(phase)));
        }
        n += 480;
        telemetry.process(buffer);
    }
    float l = 0.0f, r = 0.0f;
    telemetry.takeTruePeaks(l, r);
    CHECK(l == Catch::Approx(-6.02f).margin(0.25f)); // sample peak alone reads -9.03
    CHECK(r == Catch::Approx(-12.04f).margin(0.25f));
    // Taking starts a new interval.
    telemetry.takeTruePeaks(l, r);
    CHECK(l < -200.0f);
    // The vectorscope's tap got every sample.
    CHECK(telemetry.scopeRing().available() == juce::jmin(20 * 480, telemetry.scopeRing().capacity()));
}

TEST_CASE("Integrated loudness gates quiet passages and resets on request (UI-5)", "[plugin][ui5][integrated]")
{
    CopilotRealtimeTelemetry telemetry;
    telemetry.prepareToPlay(48000.0, 2);
    long long n = 0;
    auto play = [&](double seconds, float amp) {
        juce::AudioBuffer<float> buffer(2, 480);
        const int blocks = static_cast<int>(seconds * 100.0);
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < 480; ++i) {
                const auto v = static_cast<float>(amp * std::sin(2.0 * kPi * 997.0 * static_cast<double>(n + i) / 48000.0));
                buffer.setSample(0, i, v);
                buffer.setSample(1, i, v);
            }
            n += 480;
            telemetry.process(buffer);
        }
    };
    CHECK(telemetry.snapshot().integrated_lufs < -90.0f);
    play(5.0, 0.1f); // -20 LUFS (stereo 997 Hz)
    CHECK(telemetry.snapshot().integrated_lufs == Catch::Approx(-20.0f).margin(0.3f));
    play(5.0, 0.001f); // -60 LUFS: under the relative gate, so ignored
    CHECK(telemetry.snapshot().integrated_lufs == Catch::Approx(-20.0f).margin(0.3f));
    telemetry.requestIntegratedReset();
    play(3.0, 0.01f); // -40 LUFS
    CHECK(telemetry.snapshot().integrated_lufs == Catch::Approx(-40.0f).margin(0.3f));
}
