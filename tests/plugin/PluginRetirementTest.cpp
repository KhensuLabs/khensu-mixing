// tests/plugin/PluginRetirementTest.cpp
// Hosted-plugin retirement without processBlock (audit ARCH-04): repeated
// add/remove/clear with the transport stopped (epoch frozen) used to retain
// third-party instances until the NEXT mutation. The message-thread reaper
// must reclaim them via its quiescence path within a bounded time.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "FakeHostedInstances.h"

using khensu::KhensuCopilotProcessor;
using khensu::test::DtorCountingInstance;

namespace {

void pumpMessageLoop(int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

} // namespace

TEST_CASE("Retired chains are reclaimed without any processBlock", "[plugin][arch04]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> dtors{0};

    {
        KhensuCopilotProcessor proc;
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.prepareToPlay(48000.0, 512);

        // Mutation spam with the audio epoch frozen: add 4, remove 2, clear.
        for (int i = 0; i < 4; ++i)
            proc.addPluginInstanceToChain(std::make_unique<DtorCountingInstance>(dtors), "DtorCounter");
        proc.removePluginFromChain(0);
        proc.removePluginFromChain(0);
        proc.clearHostedPluginChain();

        // Nothing has processed audio, so mutation-time epoch pruning freed
        // nothing. Give the reaper two periods: tick 1 records the frozen
        // epoch and stamps quiescence; tick 2 frees everything retired
        // before tick 1; one more period of margin for timer jitter.
        pumpMessageLoop(1600);

        REQUIRE(dtors.load() == 4);
        REQUIRE(proc.getNumHostedPlugins() == 0);
    }
    // Destructor path frees anything left regardless.
    pumpMessageLoop(50);
    REQUIRE(dtors.load() == 4);
}

TEST_CASE("Destroying the processor reclaims everything immediately", "[plugin][arch04]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> dtors{0};

    {
        KhensuCopilotProcessor proc;
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        proc.prepareToPlay(48000.0, 512);
        for (int i = 0; i < 3; ++i)
            proc.addPluginInstanceToChain(std::make_unique<DtorCountingInstance>(dtors), "DtorCounter");
        // No reaper time given - teardown itself must not leak.
    }
    pumpMessageLoop(100); // the destructor releases via callAsync
    REQUIRE(dtors.load() == 3);
}
