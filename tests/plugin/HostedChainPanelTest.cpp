// tests/plugin/HostedChainPanelTest.cpp
// The hosted-chain UI (HostedChainMiniPanel, "Hosted" tab) and the
// message-thread slot accessors it reads: rows follow the processor's chain,
// row controls drive bypass / wet mix / removal, and a failed load reports
// itself on the status line. Uses the deterministic fakes - no real VST3s.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/ui/HostedChainMiniPanel.h"
#include "FakeHostedInstances.h"

using khensu::HostedChainMiniPanel;
using khensu::KhensuCopilotProcessor;
using khensu::test::FakeLatencyInstance;
using khensu::test::StatefulInstance;

namespace {
std::unique_ptr<FakeLatencyInstance> preparedFake(int latency)
{
    auto fake = std::make_unique<FakeLatencyInstance>(latency);
    fake->prepareToPlay(48000.0, 512);
    return fake;
}

juce::Component* findById(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren())
        if (auto* found = findById(*child, id)) return found;
    return nullptr;
}

struct ProcessorFixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;

    ProcessorFixture()
    {
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.prepareToPlay(48000.0, 512);
    }
    ~ProcessorFixture()
    {
        proc.clearHostedPluginChain();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
};
} // namespace

TEST_CASE("getHostedPluginSlots mirrors add, bypass, wet mix and removal", "[plugin][hostedchain]")
{
    ProcessorFixture f;
    auto& proc = f.proc;
    CHECK(proc.getHostedPluginSlots().empty());

    proc.addPluginInstanceToChain(preparedFake(64), "LatencyA", "C:/plugins/a.vst3");
    proc.addPluginInstanceToChain(std::make_unique<StatefulInstance>(juce::MemoryBlock()), "StatefulB",
                                  "C:/plugins/b.vst3");

    auto slots = proc.getHostedPluginSlots();
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].name == "LatencyA");
    CHECK(slots[0].filePath == "C:/plugins/a.vst3");
    CHECK(slots[0].latencySamples == 64);
    CHECK_FALSE(slots[0].bypassed);
    CHECK(slots[0].wetMix == Catch::Approx(1.0f));
    CHECK(slots[1].name == "StatefulB");

    proc.setHostedPluginBypass(1, true);
    proc.setHostedPluginWetMix(0, 0.4f);
    proc.setHostedPluginWetMix(1, 7.0f); // clamped to [0, 1]
    slots = proc.getHostedPluginSlots();
    CHECK(slots[1].bypassed);
    CHECK(slots[0].wetMix == Catch::Approx(0.4f));
    CHECK(slots[1].wetMix == Catch::Approx(1.0f));

    // Out-of-range indices are ignored rather than touching another slot.
    proc.setHostedPluginBypass(5, true);
    proc.removePluginFromChain(-1);
    CHECK(proc.getHostedPluginSlots() == slots);

    proc.removePluginFromChain(0);
    slots = proc.getHostedPluginSlots();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].name == "StatefulB");
    CHECK(slots[0].bypassed); // the surviving slot keeps its own controls
    CHECK(proc.getNumHostedPlugins() == 1);
}

TEST_CASE("Hosted chain panel rows drive the processor's slots", "[plugin][hostedchain][ui]")
{
    ProcessorFixture f;
    auto& proc = f.proc;
    HostedChainMiniPanel panel(proc);
    panel.setSize(700, 300);
    CHECK(panel.getNumRows() == 0);

    proc.addPluginInstanceToChain(preparedFake(32), "Alpha", "C:/plugins/alpha.vst3");
    proc.addPluginInstanceToChain(preparedFake(0), "Beta", "C:/plugins/beta.vst3");
    panel.refreshFromProcessor();
    REQUIRE(panel.getNumRows() == 2);

    auto* bypass = dynamic_cast<juce::Button*>(findById(panel, "hostedBypass1"));
    auto* wet = dynamic_cast<juce::Slider*>(findById(panel, "hostedWet0"));
    auto* remove = dynamic_cast<juce::Button*>(findById(panel, "hostedRemove0"));
    REQUIRE(bypass != nullptr);
    REQUIRE(wet != nullptr);
    REQUIRE(remove != nullptr);

    // Every row control carries a spoken name and a tooltip.
    for (juce::Component* c : {static_cast<juce::Component*>(bypass), static_cast<juce::Component*>(wet),
                               static_cast<juce::Component*>(remove)}) {
        CHECK(c->getTitle().isNotEmpty());
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*>(c)) CHECK(tip->getTooltip().isNotEmpty());
    }

    bypass->setToggleState(true, juce::sendNotificationSync);
    CHECK(proc.getHostedPluginSlots()[1].bypassed);

    wet->setValue(25.0, juce::sendNotificationSync);
    CHECK(proc.getHostedPluginSlots()[0].wetMix == Catch::Approx(0.25f));

    // A change made elsewhere (e.g. state restore) is picked up on refresh
    // without rebuilding the rows.
    proc.setHostedPluginBypass(1, false);
    panel.refreshFromProcessor();
    CHECK_FALSE(bypass->getToggleState());

    // Removal is deferred past the click; a duplicate click must not remove
    // the slot that slides into index 0 afterwards.
    remove->onClick();
    remove->onClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    const auto slots = proc.getHostedPluginSlots();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].name == "Beta");
    CHECK(panel.getNumRows() == 1);
    CHECK(panel.getStatusText().contains("Removed"));
}

TEST_CASE("Hosted chain panel reports a plugin that fails to load", "[plugin][hostedchain][ui]")
{
    ProcessorFixture f;
    HostedChainMiniPanel panel(f.proc);
    panel.setSize(700, 300);

    const auto bogus = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("khensu-not-a-plugin-" + juce::Uuid().toString() + ".vst3");
    panel.loadPluginFile(bogus);
    CHECK(panel.getStatusText().startsWith("Loading"));

    for (int i = 0; i < 40 && !panel.getStatusText().startsWith("Could not load"); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(25);
    CHECK(panel.getStatusText().startsWith("Could not load"));
    CHECK(f.proc.getNumHostedPlugins() == 0);
    CHECK(panel.getNumRows() == 0);
}
