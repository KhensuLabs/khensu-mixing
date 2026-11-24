// tests/plugin/PluginUndoHistoryTest.cpp
// UX2-6b: the Copilot's undo history - one step per gesture, preset loads,
// Apply top fix and snapshot recalls as one named step each, redo, the
// History menu, Ctrl+Z in the editor, host automation never a step.
// UX2-6a: the shared knob's readout, typed values and Alt-click reset.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotUndoHistory.h"
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/ui/DspControlGridPanel.h"
#include "../../plugin-copilot/Source/ui/PresetBar.h"

using khensu::KhensuCopilotEditor;
using khensu::KhensuCopilotProcessor;
using khensu::copilot::ParamUndoHistory;
using khensu::ui::KhensuKnob;
using P = khensu::KhensuCopilotProcessor::ParamIDs;

namespace {
struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    juce::File presetDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("khensu-undo-presets", "");
    Fixture()
    {
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.setUserPresetDirectoryForTesting(presetDir);
        proc.setLegacyUserPresetDirectoryForTesting(presetDir.getChildFile("legacy"));
    }
    ~Fixture() { presetDir.deleteRecursively(); }
    ParamUndoHistory& history() { return proc.undoHistory(); }
};

float plain(KhensuCopilotProcessor& proc, const char* id) { return proc.getParamPlain(id); }

/// A host-automation style write: no gesture.
void automate(KhensuCopilotProcessor& proc, const char* id, float value)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(value));
}

/// A user gesture on one parameter through several intermediate values.
void gesture(KhensuCopilotProcessor& proc, const char* id, std::initializer_list<float> values)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->beginChangeGesture();
    for (float v : values) p->setValueNotifyingHost(p->convertTo0to1(v));
    p->endChangeGesture();
}

juce::MouseEvent mouseAt(juce::Component& c, juce::ModifierKeys mods, int clicks)
{
    const auto pos = c.getLocalBounds().getCentre().toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &c, &c, juce::Time::getCurrentTime(), pos, juce::Time::getCurrentTime(), clicks, false);
}
} // namespace

TEST_CASE("A knob gesture is one undo step, named after the control (UX2-6b)", "[plugin][ux2-6b][undo]")
{
    Fixture f;
    CHECK_FALSE(f.history().canUndo());
    gesture(f.proc, P::eq2GainDb, {-0.5f, -1.2f, -2.4f, -3.0f});
    REQUIRE(f.history().canUndo());
    CHECK(f.history().undoNames().size() == 1);
    CHECK(f.history().undoNames()[0] == "EQ 2 gain -3.0 dB");

    // The grid's knobs go through the same gesture path (drag start/end).
    khensu::DspControlGridPanel grid(f.proc);
    auto* knob = grid.knobForTest(P::hpfFreq);
    REQUIRE(knob != nullptr);
    {
        const juce::Slider::ScopedDragNotification drag(*knob);
        knob->setValue(100.0, juce::sendNotificationSync);
        knob->setValue(140.0, juce::sendNotificationSync);
        knob->setValue(160.0, juce::sendNotificationSync);
    }
    CHECK(f.history().undoNames().size() == 2);
    CHECK(f.history().undoNames()[0] == "HPF freq 160 Hz");

    // A click without movement is no step; neither is host automation.
    {
        const juce::Slider::ScopedDragNotification drag(*knob);
    }
    automate(f.proc, P::compThreshDb, -30.0f);
    automate(f.proc, P::outGainDb, 4.0f);
    CHECK(f.history().undoNames().size() == 2);

    // Undo puts back the gesture's start, not an intermediate value.
    REQUIRE(f.history().undo());
    CHECK(plain(f.proc, P::hpfFreq) == Catch::Approx(80.0f).margin(0.5f));
    REQUIRE(f.history().undo());
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(0.0f).margin(0.01f));
    CHECK_FALSE(f.history().canUndo());
    // Undo never touched the automated parameters.
    CHECK(plain(f.proc, P::compThreshDb) == Catch::Approx(-30.0f).margin(0.05f));

    // Redo after undo, in order.
    REQUIRE(f.history().canRedo());
    CHECK(f.history().redoNames()[0] == "EQ 2 gain -3.0 dB");
    REQUIRE(f.history().redo());
    CHECK(plain(f.proc, P::eq2GainDb) == Catch::Approx(-3.0f).margin(0.05f));
    REQUIRE(f.history().redo());
    CHECK(plain(f.proc, P::hpfFreq) == Catch::Approx(160.0f).margin(0.5f));
    CHECK_FALSE(f.history().canRedo());

    // A new change after an undo drops the redo branch.
    f.history().undo();
    gesture(f.proc, P::limOn, {1.0f});
    CHECK_FALSE(f.history().canRedo());
    CHECK(f.history().undoNames()[0] == "Limiter on");
}

TEST_CASE("Preset loads, Apply top fix and snapshot recalls are one step each (UX2-6b)", "[plugin][ux2-6b][undo]")
{
    Fixture f;
    int podcast = -1;
    for (int i = 0; i < f.proc.getNumPrograms(); ++i)
        if (f.proc.getProgramName(i) == "Podcast voice") podcast = i;
    REQUIRE(podcast > 0);

    // Factory preset: many parameters, one step.
    f.proc.loadProgramFromUi(podcast);
    REQUIRE(f.history().undoNames().size() == 1);
    CHECK(f.history().undoNames()[0] == "Preset: Podcast voice");
    CHECK(plain(f.proc, P::deessOn) > 0.5f);
    f.history().undo();
    CHECK(plain(f.proc, P::deessOn) < 0.5f);
    CHECK(plain(f.proc, P::hpfOn) < 0.5f);
    f.history().redo();
    CHECK(plain(f.proc, P::deessOn) > 0.5f);
    CHECK(plain(f.proc, P::compThreshDb) == Catch::Approx(-24.0f).margin(0.05f));

    // User preset: one step.
    juce::String error;
    gesture(f.proc, P::outGainDb, {-2.0f});
    REQUIRE(f.proc.saveUserPreset("Bus glue", error));
    f.proc.loadProgramFromUi(0);
    const auto listed = f.proc.listUserPresets();
    REQUIRE(listed.size() == 1);
    REQUIRE(f.proc.loadUserPreset(listed[0].file, error));
    CHECK(f.history().undoNames()[0] == "Preset: Bus glue");
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-2.0f).margin(0.05f));
    f.history().undo();
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(0.0f).margin(0.05f));

    // Apply top fix at 70 %: one step; undo also drops the live fix.
    f.proc.loadProgramFromUi(0);
    automate(f.proc, P::fixAmount, 0.7f);
    khensu::Recommendation rec;
    rec.id = "harsh-3k";
    rec.what = "Harsh upper mids";
    rec.safe_to_auto_apply = true;
    rec.suggested_parameters = {{"eq_freq_hz", 3000.0f}, {"eq_gain_db", -6.0f}, {"eq_q", 1.4f}};
    const auto result = f.proc.applyLiveFix(rec);
    REQUIRE(result.applied);
    CHECK(f.history().undoNames()[0] == "Apply top fix 70 %");
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(-4.2f).margin(0.05f));
    CHECK(f.proc.getLiveFixId() == "harsh-3k");
    f.history().undo();
    CHECK(plain(f.proc, P::eqGainDb) == Catch::Approx(0.0f).margin(0.05f));
    CHECK(f.proc.getLiveFixId().empty());

    // Snapshot recall: one step; undo switches back with both sides intact.
    gesture(f.proc, P::outGainDb, {3.0f});
    f.proc.switchSnapshot(1); // B starts as a copy of A
    gesture(f.proc, P::outGainDb, {-6.0f});
    f.proc.switchSnapshot(0);
    CHECK(f.history().undoNames()[0] == "Snapshot A");
    CHECK(f.history().undoNames()[1] == "Output gain -6.0 dB");
    CHECK(f.history().undoNames()[2] == "Snapshot B");
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(3.0f).margin(0.05f));
    f.history().undo(); // back on B
    CHECK(f.proc.getActiveSnapshot() == 1);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(-6.0f).margin(0.05f));
    f.history().redo(); // on A again
    CHECK(f.proc.getActiveSnapshot() == 0);
    CHECK(plain(f.proc, P::outGainDb) == Catch::Approx(3.0f).margin(0.05f));
}

TEST_CASE("The History menu lists the last 20 steps by name and jumps to one (UX2-6b)", "[plugin][ux2-6b][undo]")
{
    Fixture f;
    for (int i = 1; i <= 25; ++i) gesture(f.proc, P::compThreshDb, {-static_cast<float>(i)});
    const auto names = f.history().undoNames(20);
    CHECK(names.size() == 20);
    CHECK(names[0] == "Comp threshold -25.0 dB"); // most recent first
    CHECK(names[19] == "Comp threshold -6.0 dB");

    khensu::PresetBar bar(f.proc);
    bar.refresh();
    CHECK(bar.undoButton().isEnabled());
    CHECK_FALSE(bar.redoButton().isEnabled());
    CHECK(bar.undoButton().getTooltip().contains("Comp threshold -25.0 dB"));
    auto items = bar.historyMenuTextForTesting();
    CHECK(items.size() == 21); // 20 steps + Clear history
    CHECK(items[0] == "Comp threshold -25.0 dB");

    // Jump back three steps.
    f.history().undoSteps(3);
    CHECK(plain(f.proc, P::compThreshDb) == Catch::Approx(-22.0f).margin(0.05f));
    bar.refresh();
    CHECK(bar.redoButton().isEnabled());
    items = bar.historyMenuTextForTesting();
    CHECK(items.contains("Comp threshold -25.0 dB")); // offered as redo
    f.history().redoSteps(3);
    CHECK(plain(f.proc, P::compThreshDb) == Catch::Approx(-25.0f).margin(0.05f));

    // A restored project starts a new history.
    juce::MemoryBlock state;
    f.proc.getStateInformation(state);
    f.proc.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK_FALSE(f.history().canUndo());
    bar.refresh();
    CHECK_FALSE(bar.undoButton().isEnabled());
}

TEST_CASE("Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y undo and redo in the editor (UX2-6b)", "[plugin][ux2-6b][undo]")
{
    Fixture f;
    KhensuCopilotEditor ed(f.proc);
    gesture(f.proc, P::eqQ, {2.5f});
    const auto cmd = juce::ModifierKeys::commandModifier;
    CHECK(ed.keyPressed(juce::KeyPress('Z', cmd, 0)));
    CHECK(plain(f.proc, P::eqQ) == Catch::Approx(1.0f).margin(0.01f));
    CHECK(ed.keyPressed(juce::KeyPress('Z', cmd | juce::ModifierKeys::shiftModifier, 0)));
    CHECK(plain(f.proc, P::eqQ) == Catch::Approx(2.5f).margin(0.01f));
    CHECK(ed.keyPressed(juce::KeyPress('Z', cmd, 0)));
    CHECK(ed.keyPressed(juce::KeyPress('Y', cmd, 0)));
    CHECK(plain(f.proc, P::eqQ) == Catch::Approx(2.5f).margin(0.01f));
    // The strip's Undo of an apply is a step of its own, not a stray one per parameter.
    khensu::Recommendation rec;
    rec.id = "x";
    rec.safe_to_auto_apply = true;
    rec.suggested_parameters = {{"eq_freq_hz", 3000.0f}, {"eq_gain_db", -6.0f}};
    REQUIRE(ed.applySuggestion(rec));
    ed.undoLastApplyForTesting();
    CHECK(f.history().undoNames()[0] == "Undo top fix");
    CHECK(f.history().undoNames()[1].startsWith("Apply top fix"));
}

TEST_CASE("Step names read like the controls (UX2-6b)", "[plugin][ux2-6b][undo]")
{
    CHECK(ParamUndoHistory::friendlyName("EQ2 Gain") == "EQ 2 gain");
    CHECK(ParamUndoHistory::friendlyName("Comp Threshold") == "Comp threshold");
    CHECK(ParamUndoHistory::friendlyName("HPF Freq") == "HPF freq");
    CHECK(ParamUndoHistory::unitFor("crossover_low_freq") == KhensuKnob::Unit::Hz);
    CHECK(ParamUndoHistory::unitFor("comp_attack_ms") == KhensuKnob::Unit::Ms);
    CHECK(ParamUndoHistory::unitFor("comp_ratio") == KhensuKnob::Unit::Ratio);
    CHECK(ParamUndoHistory::unitFor("eq3_q") == KhensuKnob::Unit::Q);
    CHECK(ParamUndoHistory::unitFor("chain_mix") == KhensuKnob::Unit::Percent);
    CHECK(ParamUndoHistory::unitFor("deess_range_db") == KhensuKnob::Unit::Db);

    Fixture f;
    gesture(f.proc, P::eq3Type, {2.0f});
    CHECK(f.history().undoNames()[0] == "EQ 3 shape High shelf");
    gesture(f.proc, P::compRatio, {4.5f});
    CHECK(f.history().undoNames()[0] == "Comp ratio 4.5:1");
    gesture(f.proc, P::chainMix, {0.7f});
    CHECK(f.history().undoNames()[0] == "Chain mix 70 %");
}

TEST_CASE("The knob shows its value with a unit, types a value and Alt-click resets (UX2-6a)",
          "[plugin][ux2-6a][knob]")
{
    using U = KhensuKnob::Unit;
    CHECK(KhensuKnob::formatValue(80.0, U::Hz) == "80 Hz");
    CHECK(KhensuKnob::formatValue(1200.0, U::Hz) == "1.2 kHz");
    CHECK(KhensuKnob::formatValue(2500.0, U::Hz) == "2.5 kHz");
    CHECK(KhensuKnob::formatValue(18000.0, U::Hz) == "18 kHz");
    CHECK(KhensuKnob::formatValue(-18.0, U::Db) == "-18.0 dB");
    CHECK(KhensuKnob::formatValue(3.0, U::Db) == "+3.0 dB");
    CHECK(KhensuKnob::formatValue(-0.01, U::Db) == "0.0 dB");
    CHECK(KhensuKnob::formatValue(5.0, U::Ms) == "5.0 ms");
    CHECK(KhensuKnob::formatValue(120.0, U::Ms) == "120 ms");
    CHECK(KhensuKnob::formatValue(4.0, U::Ratio) == "4.0:1");
    CHECK(KhensuKnob::formatValue(0.7, U::Percent) == "70 %");
    CHECK(KhensuKnob::formatValue(1.414, U::Q) == "1.41");
    CHECK(KhensuKnob::parseValue("1.2k", U::Hz, 0.0) == Catch::Approx(1200.0));
    CHECK(KhensuKnob::parseValue("1.2 kHz", U::Hz, 0.0) == Catch::Approx(1200.0));
    CHECK(KhensuKnob::parseValue("-3 dB", U::Db, 0.0) == Catch::Approx(-3.0));
    CHECK(KhensuKnob::parseValue("70%", U::Percent, 0.0) == Catch::Approx(0.7));
    CHECK(KhensuKnob::parseValue("1.5 s", U::Ms, 0.0) == Catch::Approx(1500.0));
    CHECK(KhensuKnob::parseValue("4:1", U::Ratio, 0.0) == Catch::Approx(4.0));
    CHECK(KhensuKnob::parseValue("loud", U::Db, 7.0) == Catch::Approx(7.0));

    Fixture f;
    khensu::DspControlGridPanel grid(f.proc);
    grid.setSize(900, 400);
    auto* knob = grid.knobForTest(P::eqFreq);
    REQUIRE(knob != nullptr);
    CHECK(knob->getTextFromValue(knob->getValue()) == "1 kHz");
    CHECK(knob->getTooltip().contains("Alt/Option-click"));
    CHECK(grid.getNumKnobsForTest() == 26);

    // Double-click (or Enter) opens the inline editor; it never resets.
    automate(f.proc, P::eqFreq, 2500.0f);
    knob->mouseDoubleClick(mouseAt(*knob, {}, 2));
    REQUIRE(knob->isValueEditorShowing());
    CHECK(plain(f.proc, P::eqFreq) == Catch::Approx(2500.0f).margin(1.0f));
    CHECK(knob->valueEditorForTest()->getText() == "2.5 kHz");
    // Escape cancels.
    knob->valueEditorForTest()->setText("4k", juce::dontSendNotification);
    knob->hideValueEditor(false);
    CHECK_FALSE(knob->isValueEditorShowing());
    CHECK(plain(f.proc, P::eqFreq) == Catch::Approx(2500.0f).margin(1.0f));
    // Return commits, as one undo step.
    CHECK(knob->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    REQUIRE(knob->isValueEditorShowing());
    knob->valueEditorForTest()->setText("1.2 kHz", juce::dontSendNotification);
    knob->hideValueEditor(true);
    CHECK(plain(f.proc, P::eqFreq) == Catch::Approx(1200.0f).margin(1.0f));
    CHECK(f.history().undoNames()[0] == "EQ freq 1.2 kHz");

    // Alt-click resets to the default, as one step.
    knob->mouseDown(mouseAt(*knob, juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier, 1));
    knob->mouseUp(mouseAt(*knob, juce::ModifierKeys::altModifier, 1));
    CHECK(plain(f.proc, P::eqFreq) == Catch::Approx(1000.0f).margin(1.0f));
    CHECK(f.history().undoNames()[0] == "EQ freq 1 kHz");
    f.history().undo();
    CHECK(plain(f.proc, P::eqFreq) == Catch::Approx(1200.0f).margin(1.0f));
}

namespace {
juce::File agentShotsDir()
{
    // <build>/tests/Release/KhensuPluginTests.exe -> <build>/agentG-shots
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getParentDirectory()
                   .getParentDirectory()
                   .getChildFile("agentG-shots");
    dir.createDirectory();
    return dir;
}

void writeShot(juce::Component& c, const juce::String& name)
{
    const auto image = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    const auto file = agentShotsDir().getChildFile(name + ".png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
}
} // namespace

TEST_CASE("The DSP tab lays every knob out inside the grid at medium and large sizes (UX2-6a)",
          "[plugin][ux2-6a][knob]")
{
    Fixture f;
    using ViewSize = KhensuCopilotProcessor::ViewSize;
    KhensuCopilotEditor ed(f.proc);
    ed.selectTabByShortcut(1); // DSP
    gesture(f.proc, P::eq2GainDb, {-3.0f});
    gesture(f.proc, P::compOn, {1.0f});
    for (auto [size, name] : {std::pair{ViewSize::Medium, "dsp-tab-medium"}, std::pair{ViewSize::Large, "dsp-tab-large"}}) {
        ed.setView(false, size);
        writeShot(ed, name);
    }
    // Every knob of the grid is inside it, at full size, with room for its value.
    khensu::DspControlGridPanel panel(f.proc);
    for (auto [w, h] : {std::pair{900, 392}, std::pair{1230, 610}}) {
        panel.setSize(w, h);
        for (auto* child : panel.getChildren())
            if (auto* knob = dynamic_cast<KhensuKnob*>(child)) {
                INFO(knob->getTitle() << " at " << w << "x" << h);
                CHECK(panel.getLocalBounds().contains(knob->getBounds()));
                CHECK_FALSE(KhensuKnob::layoutFor(knob->getLocalBounds().toFloat(), true).value.isEmpty());
                CHECK_FALSE(KhensuKnob::layoutFor(knob->getLocalBounds().toFloat(), true).label.isEmpty());
            }
    }
}
