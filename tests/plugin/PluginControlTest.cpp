// tests/plugin/PluginControlTest.cpp
// Regression tests for the 2026-11-06 audit's plugin control/UI findings:
// PLG-05 host dirty notification, PLG-06 hosted editors + reorder, PLG-07
// brain resource resolution, PLG-09 chat history shape, PLG-11 one analysis
// worker, PLG-12 A/B never leaks into bounces + Blind reveal, PLG-16 server
// cancel, PLG-17 concurrent MixHub register/unregister, PLG-18 role matching,
// PLG-19 AI apply semantics, PLG-20 AI dot / grid controls / resizable editor,
// PLG-23 state version + instance id, PLG-24 no self-hosting.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotAnalysisController.h"
#include "../../plugin-copilot/Source/CopilotServiceJobs.h"
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/mixhub/MixHub.h"
#include "../../plugin-copilot/Source/mixhub/MixHubAnalysis.h"
#include "../../plugin-copilot/Source/ui/ChatConsoleMiniPanel.h"
#include "../../plugin-copilot/Source/ui/DspControlGridPanel.h"
#include "../../plugin-copilot/Source/ui/PluginComparisonStrip.h"
#include "../../plugin-copilot/Source/ui/SystemBrainMiniPanel.h"
#include "FakeHostedInstances.h"

#include <atomic>
#include <chrono>
#include <thread>

using khensu::KhensuCopilotEditor;
using khensu::KhensuCopilotProcessor;
using khensu::test::EditorLogInstance;
using khensu::test::FakeLatencyInstance;
using P = khensu::KhensuCopilotProcessor::ParamIDs;

namespace {
struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    Fixture()
    {
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.prepareToPlay(48000.0, 512);
    }
};

struct DirtyCounter final : public juce::AudioProcessorListener {
    int nonParameterChanges = 0;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged) ++nonParameterChanges;
    }
};

float raw(KhensuCopilotProcessor& proc, const char* id) { return proc.getAPVTS().getRawParameterValue(id)->load(); }

juce::MemoryBlock saveState(KhensuCopilotProcessor& proc)
{
    juce::MemoryBlock block;
    proc.getStateInformation(block);
    return block;
}
} // namespace

TEST_CASE("Non-parameter edits mark the host project dirty (PLG-05)", "[plugin][plg05]")
{
    Fixture f;
    DirtyCounter counter;
    f.proc.addListener(&counter);

    auto step = [&](auto&& action) {
        const int before = counter.nonParameterChanges;
        action();
        return counter.nonParameterChanges > before;
    };
    CHECK(step([&] { f.proc.addPluginInstanceToChain(std::make_unique<FakeLatencyInstance>(0), "A", "C:/a.vst3"); }));
    CHECK(step([&] { f.proc.addPluginInstanceToChain(std::make_unique<FakeLatencyInstance>(0), "B", "C:/b.vst3"); }));
    CHECK(step([&] { f.proc.setHostedPluginBypass(0, true); }));
    CHECK(step([&] { f.proc.setHostedPluginWetMix(0, 0.5f); }));
    CHECK(step([&] { f.proc.moveHostedPlugin(0, 1); }));
    CHECK(step([&] { f.proc.removePluginFromChain(0); }));
    CHECK(step([&] { f.proc.setParamFromAI(P::eqGainDb, 4.0f); }));
    CHECK(step([&] { f.proc.clearAiSet(P::eqGainDb); }));
    CHECK(step([&] { f.proc.setMixHubRoleOverride("Bass"); }));
    CHECK(step([&] { f.proc.clearHostedPluginChain(); }));

    // Loading a state is not an edit.
    const auto state = saveState(f.proc);
    CHECK_FALSE(step([&] { f.proc.setStateInformation(state.getData(), (int)state.getSize()); }));
    f.proc.removeListener(&counter);
}

TEST_CASE("Hosted slots can be reordered and their editors open and close safely (PLG-06)", "[plugin][plg06]")
{
    EditorLogInstance::Log log; // outlives the processor and its instances
    Fixture f;
    REQUIRE(f.proc.addPluginInstanceToChain(std::make_unique<EditorLogInstance>(log, "First"), "First", "C:/1.vst3"));
    REQUIRE(f.proc.addPluginInstanceToChain(std::make_unique<FakeLatencyInstance>(64), "Second", "C:/2.vst3"));
    const int latencyBefore = f.proc.getLatencySamples();

    REQUIRE(f.proc.moveHostedPlugin(1, 0));
    auto slots = f.proc.getHostedPluginSlots();
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].name == "Second");
    CHECK(slots[1].name == "First");
    CHECK(f.proc.getLatencySamples() == latencyBefore);
    CHECK_FALSE(f.proc.moveHostedPlugin(0, 0));
    CHECK_FALSE(f.proc.moveHostedPlugin(0, 5));

    // Processing keeps working across the reorder.
    juce::AudioBuffer<float> buf(2, 512);
    buf.clear();
    juce::MidiBuffer midi;
    f.proc.processBlock(buf, midi);

    REQUIRE(f.proc.openHostedPluginEditor(1));
    CHECK(f.proc.getNumOpenHostedEditors() == 1);
    CHECK(f.proc.isHostedEditorOpen(slots[1].id));
    REQUIRE(f.proc.openHostedPluginEditor(1)); // second open just raises it
    CHECK(f.proc.getNumOpenHostedEditors() == 1);
    // A plugin without its own editor gets the generic one.
    REQUIRE(f.proc.openHostedPluginEditor(0));
    CHECK(f.proc.getNumOpenHostedEditors() == 2);

    // Removing the slot closes its window FIRST: the editor is gone before
    // the instance is destroyed.
    f.proc.removePluginFromChain(1);
    CHECK(f.proc.getNumOpenHostedEditors() == 1);
    REQUIRE_FALSE(log.events.empty());
    CHECK(log.events.front() == "editor");

    f.proc.clearHostedPluginChain();
    CHECK(f.proc.getNumOpenHostedEditors() == 0);
    for (int i = 0; i < 4; ++i) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        f.proc.processBlock(buf, midi);
    }
}

TEST_CASE("The feature encyclopedia resolves from the plugin module, not the host EXE (PLG-07)", "[plugin][plg07]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu-plg07-" + juce::Uuid().toString());
    const auto bundle = root.getChildFile("Khensu Copilot.vst3");
    const auto module = bundle.getChildFile("Contents/x86_64-win/Khensu Copilot.vst3");
    REQUIRE(module.getParentDirectory().createDirectory());
    REQUIRE(module.replaceWithText("binary"));
    const auto json = bundle.getChildFile("Contents/Resources/feature_encyclopedia.json");
    REQUIRE(json.getParentDirectory().createDirectory());
    REQUIRE(json.replaceWithText("{}"));

    CHECK(khensu::SystemBrainMiniPanel::resolveEncyclopediaFile(module) == json);
    // Without the bundled copy the build-tree fallback is used.
    json.deleteFile();
    const auto inTree = root.getChildFile("resources/system_brain/feature_encyclopedia.json");
    REQUIRE(inTree.getParentDirectory().createDirectory());
    REQUIRE(inTree.replaceWithText("{}"));
    CHECK(khensu::SystemBrainMiniPanel::resolveEncyclopediaFile(module) == inTree);
    root.deleteRecursively();

    // The real build ships the JSON inside the built bundle (CMake POST_BUILD).
    const juce::File builtBundle(KHENSU_COPILOT_VST3_PATH);
    if (builtBundle.isDirectory())
        CHECK(builtBundle.getChildFile("Contents/Resources/feature_encyclopedia.json").existsAsFile());
}

TEST_CASE("Chat sends only completed exchanges and keeps no orphan turn on failure (PLG-09)", "[plugin][plg09]")
{
    Fixture f;
    khensu::ChatConsoleMiniPanel chat(f.proc);
    CHECK(chat.historySize() == 0);
    chat.completeExchange("make it wetter", false, "service down");
    CHECK(chat.historySize() == 0); // no orphan user turn
    chat.completeExchange("make it wetter", true, "Try a plate reverb.");
    REQUIRE(chat.historySize() == 2);
    const auto payload = chat.historyPayload();
    REQUIRE(payload.isArray());
    REQUIRE(payload.size() == 2);
    CHECK(payload[0]["role"].toString() == "user");
    CHECK(payload[0]["content"].toString() == "make it wetter");
    CHECK(payload[1]["role"].toString() == "assistant");
}

TEST_CASE("Only one analysis load runs at a time however often it is requested (PLG-11)", "[plugin][plg11]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> running{0}, maxRunning{0}, calls{0}, callbacks{0};
    khensu::CopilotAnalysisController controller(
        [&](const khensu::CopilotAnalysisController::CancelCheck&)
            -> std::optional<khensu::CopilotAnalysisController::LoadedSnapshot> {
            const int now = running.fetch_add(1) + 1;
            int expected = maxRunning.load();
            while (now > expected && !maxRunning.compare_exchange_weak(expected, now)) {}
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(60)); // an uncancellable decode
            running.fetch_sub(1);
            khensu::CopilotAnalysisController::LoadedSnapshot snapshot;
            snapshot.sessionId = "s";
            return snapshot;
        });
    for (int i = 0; i < 10; ++i) controller.loadLastSessionAsync([&callbacks] { callbacks.fetch_add(1); });
    for (int i = 0; i < 200 && controller.getNumRunningWorkers() > 0; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    CHECK(maxRunning.load() == 1);
    CHECK(calls.load() <= 2); // the first, then only the newest queued one
    CHECK(callbacks.load() == 1);
}

TEST_CASE("A/B mode never leaks into an offline render, and Blind can be revealed (PLG-12)", "[plugin][plg12]")
{
    Fixture f;
    f.proc.setAbMode(1);
    CHECK(f.proc.getEffectiveAbMode() == 1);
    f.proc.setNonRealtime(true);
    CHECK(f.proc.getEffectiveAbMode() == 0);
    f.proc.setNonRealtime(false);
    CHECK(f.proc.getEffectiveAbMode() == 1);

    khensu::PluginComparisonStrip strip;
    strip.setSize(500, 30);
    strip.isBlindTrialProcessed = [] { return true; };
    int next = 0;
    strip.onNextBlindTrial = [&next] { ++next; };
    strip.selectMode(0);
    CHECK_FALSE(strip.isRevealVisibleForTest());
    strip.selectMode(3);
    CHECK(strip.isRevealVisibleForTest());
    CHECK(strip.getMatchTextForTest().contains("side hidden"));
    strip.clickRevealForTest();
    CHECK(strip.getMatchTextForTest().contains("B (processed)"));
}

TEST_CASE("The A/B strip says when the heard side is trimmed under the ceiling (FIX-12)", "[plugin][fix12]")
{
    // A component needs the GUI initialised: without it Debug asserts and
    // reports JUCE's singletons as leaked (debug-capture, 2026-11-13).
    juce::ScopedJuceInitialiser_GUI juceInit;
    khensu::PluginComparisonStrip strip;
    strip.setSize(500, 30);
    float trim = 0.0f;
    strip.monitorTrimDb = [&trim] { return trim; };

    strip.selectMode(1); // Original, fits under the ceiling
    CHECK(strip.getMatchTextForTest().contains("Loudness matched"));

    trim = -3.24f; // the matched original would clip: the stage turns it down
    strip.refreshMatchState();
    CHECK(strip.getMatchTextForTest().contains("unmatched"));
    CHECK(strip.getMatchTextForTest().contains("-3.2 dB"));
    CHECK(strip.getMatchTooltipForTest().contains("ceiling"));

    // The editor re-selects the mode on every timer tick: that must not
    // paint the trimmed state as matched again.
    strip.selectMode(1);
    CHECK(strip.getMatchTextForTest().contains("unmatched"));

    trim = -0.01f; // below display resolution: still matched
    strip.refreshMatchState();
    CHECK(strip.getMatchTextForTest().contains("Loudness matched"));

    trim = -2.0f; // Blind: say the trial is not matched, even though it hints at the side
    strip.selectMode(3);
    CHECK(strip.getMatchTextForTest().contains("unmatched"));
}

TEST_CASE("Every non-Finished job wait cancels the server job (PLG-16)", "[plugin][plg16]")
{
    using khensu::copilot::JobWait;
    using khensu::copilot::needsServerCancel;
    CHECK_FALSE(needsServerCancel(JobWait::Finished));
    CHECK(needsServerCancel(JobWait::Failed));
    CHECK(needsServerCancel(JobWait::Cancelled));
    CHECK(needsServerCancel(JobWait::TimedOut));
}

TEST_CASE("Concurrent MixHub register/unregister never leaves a tap without a publisher (PLG-17)",
          "[plugin][plg17][mixhub]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::SharedResourcePointer<khensu::mixhub::MixHub> hub; // keeps one hub alive across taps
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 60; ++i) {
                khensu::mixhub::MixHubTap tap;
                if (!tap.getHub().isPublisherRunning()) failures.fetch_add(1);
            }
        });
    for (auto& t : threads) t.join();
    CHECK(failures.load() == 0);
    CHECK_FALSE(hub->isPublisherRunning());
    {
        khensu::mixhub::MixHubTap tap;
        CHECK(hub->isPublisherRunning());
    }
}

TEST_CASE("Mix roles match whole words, honour the user's choice and spot an untitled master (PLG-18)",
          "[plugin][plg18][mixhub]")
{
    using namespace khensu::mixhub;
    const auto silent = silentBands();
    CHECK(guessRole("Chat FX", silent) != "Hi-hat");
    CHECK(guessRole("Override", silent) != "Hi-hat");
    CHECK(guessRole("Subgroup Vox", silent) == "Vocal");
    CHECK(guessRole("Bass Drum", silent) == "Kick");
    CHECK(guessRole("Vocals", silent) == "Vocal");
    CHECK(guessRole("Hi-Hat", silent) == "Hi-hat");
    CHECK(guessRole("2-Bus", silent) == kMixBusRole);

    auto frame = [](juce::uint64 id, const juce::String& name, float low, float mid, float high) {
        TrackFrame f;
        f.instanceId = id;
        f.trackName = name;
        f.bandDb.fill(-85.0f);
        for (int b = 0; b < kNumBands; ++b) {
            const float hz = bandCentreHz(b);
            f.bandDb[(size_t)b] = hz < 200.0f ? low : (hz < 3000.0f ? mid : high);
        }
        f.role = guessRole(name, f.bandDb);
        f.shortTermLufs = -14.0f;
        return f;
    };
    std::vector<TrackFrame> frames{frame(1, "Kick", -10.0f, -40.0f, -70.0f), frame(2, "Vox", -50.0f, -12.0f, -30.0f),
                                   frame(3, "", -9.0f, -11.0f, -29.0f)};
    CHECK(summarise(frames).contributingTracks == 2); // the untitled master is not a third track
    markImplicitMixBus(frames);
    CHECK(frames[2].role == kMixBusRole);

    Fixture f;
    f.proc.setMixHubRoleOverride("Bass");
    CHECK(f.proc.getMixHubRoleOverride() == "Bass");
    const auto state = saveState(f.proc);
    KhensuCopilotProcessor other;
    other.setStateInformation(state.getData(), (int)state.getSize());
    CHECK(other.getMixHubRoleOverride() == "Bass");
}

TEST_CASE("AI apply switches off stale stages and moves gain relatively (PLG-19)", "[plugin][plg19]")
{
    Fixture f;
    f.proc.applyAiParameterSet({{P::hpfOn, 1.0f}, {P::hpfFreq, 120.0f}, {P::outGainDb, 3.0f}});
    CHECK(raw(f.proc, P::hpfOn) > 0.5f);
    CHECK(raw(f.proc, P::outGainDb) == Catch::Approx(3.0f).margin(0.05f));
    f.proc.applyAiParameterSet({{P::eqOn, 1.0f}, {P::eqGainDb, 2.0f}});
    CHECK(raw(f.proc, P::hpfOn) < 0.5f); // the previous Apply's stage is off
    CHECK(raw(f.proc, P::eqOn) > 0.5f);
    CHECK(raw(f.proc, P::outGainDb) == Catch::Approx(0.0f).margin(0.05f));
    CHECK_FALSE(f.proc.isAiSet(P::hpfOn));

    f.proc.adjustOutGainFromAi(-3.0f);
    f.proc.adjustOutGainFromAi(-2.0f);
    CHECK(raw(f.proc, P::outGainDb) == Catch::Approx(-5.0f).margin(0.05f));
    CHECK(f.proc.isAiSet(P::outGainDb));
}

TEST_CASE("Any manual value change clears the AI dot; the Chain tab covers every parameter (PLG-20)",
          "[plugin][plg20]")
{
    Fixture f;
    f.proc.setParamFromAI(P::eqGainDb, 4.0f);
    REQUIRE(f.proc.isAiSet(P::eqGainDb));
    // Not a mouse drag: a typed value / host automation / keyboard nudge.
    auto* param = f.proc.getAPVTS().getParameter(P::eqGainDb);
    param->setValueNotifyingHost(param->convertTo0to1(-2.0f));
    CHECK_FALSE(f.proc.isAiSet(P::eqGainDb));

    khensu::DspControlGridPanel grid(f.proc);
    // HPF, LPF, 3 x EQ (3), comp (4), multiband (5), de-esser (3, OVH-8),
    // out gain + chain mix, limiter; one shape selector per EQ band.
    CHECK(grid.getNumKnobsForTest() == 26);
    CHECK(grid.getNumTogglesForTest() == 9);
    CHECK(grid.getNumShapeSelectorsForTest() == 3);

    KhensuCopilotEditor editor(f.proc);
    CHECK(editor.isResizable());
    editor.setSize(KhensuCopilotEditor::kMinWidth, KhensuCopilotEditor::kMinHeight);
    editor.setSize(1400, 1000);
    CHECK(editor.getWidth() == 1400);
}

TEST_CASE("Plugin state carries a version, migrates legacy states and keeps a unique instance id (PLG-23/PLG-04)",
          "[plugin][plg23][plg04]")
{
    Fixture f;
    const auto block = saveState(f.proc);
    std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(block.getData(), (int)block.getSize()));
    REQUIRE(xml != nullptr);
    CHECK(xml->getIntAttribute("khensuStateVersion") == KhensuCopilotProcessor::kStateVersion);
    CHECK(xml->getStringAttribute("khensuInstanceId") == f.proc.getInstanceId());

    juce::ValueTree legacy("KhensuCopilotState");
    KhensuCopilotProcessor::migrateState(legacy, 1);
    CHECK(static_cast<int>(legacy.getProperty("khensuStateVersion")) == KhensuCopilotProcessor::kStateVersion);

    // A duplicated track (same state, original still alive) keeps its own id...
    KhensuCopilotProcessor duplicate;
    duplicate.setStateInformation(block.getData(), (int)block.getSize());
    CHECK(duplicate.getInstanceId() != f.proc.getInstanceId());
    CHECK(duplicate.getInstanceId().isNotEmpty());

    // ...while a project reload (no live instance holds the saved id) adopts it.
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.setProperty("khensuInstanceId", "restored-instance-id", nullptr);
    std::unique_ptr<juce::XmlElement> edited(tree.createXml());
    juce::MemoryBlock editedBlock;
    juce::AudioProcessor::copyXmlToBinary(*edited, editedBlock);
    auto holder = std::make_unique<KhensuCopilotProcessor>();
    holder->setStateInformation(editedBlock.getData(), (int)editedBlock.getSize());
    CHECK(holder->getInstanceId() == "restored-instance-id");
}

TEST_CASE("The Copilot refuses to host itself (PLG-24)", "[plugin][plg24]")
{
    EditorLogInstance::Log log; // outlives the processor and its instances
    Fixture f;
    CHECK_FALSE(f.proc.addPluginInstanceToChain(
        std::make_unique<EditorLogInstance>(log, JucePlugin_Name, JucePlugin_Manufacturer), "Khensu Copilot",
        "C:/VST3/Khensu Copilot.vst3"));
    CHECK(f.proc.getNumHostedPlugins() == 0);
    CHECK(f.proc.addPluginInstanceToChain(std::make_unique<EditorLogInstance>(log, "Other Plugin"), "Other",
                                          "C:/VST3/Other.vst3"));

    juce::PluginDescription byPath;
    byPath.name = "Something";
    byPath.fileOrIdentifier = "C:/Program Files/Common Files/VST3/Khensu Copilot.vst3";
    CHECK(KhensuCopilotProcessor::isCopilotItself(byPath));
    juce::PluginDescription other;
    other.name = "Pro-Q 3";
    other.manufacturerName = "FabFilter";
    CHECK_FALSE(KhensuCopilotProcessor::isCopilotItself(other));
}
