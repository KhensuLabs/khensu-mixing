// Hosted-plugin state is part of the parent VST3 project state (audit CXX-04).
// This test does not require a real child VST3: it verifies that Khensu treats
// a child state blob as opaque and preserves the associated slot controls.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "FakeHostedInstances.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

using khensu::KhensuCopilotProcessor;
using khensu::test::StatefulInstance;

namespace {
struct ThreadTrackingParameterListener final : juce::AudioProcessorParameter::Listener {
    void parameterValueChanged(int, float) override
    {
        callbackCount.fetch_add(1, std::memory_order_relaxed);
        callbackThread = std::this_thread::get_id();
    }
    void parameterGestureChanged(int, bool) override {}

    std::atomic<int> callbackCount{0};
    std::thread::id callbackThread;
};
} // namespace

TEST_CASE("Hosted plugin chain state is embedded in parent VST state", "[plugin][cxx04]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);

    constexpr char childState[] = "opaque-child-state-v1";
    juce::MemoryBlock expected(childState, sizeof(childState));
    proc.addPluginInstanceToChain(std::make_unique<StatefulInstance>(expected), "Stateful", "C:/plugins/stateful.vst3");
    proc.setHostedPluginBypass(0, true);
    proc.setHostedPluginWetMix(0, 0.35f);

    juce::MemoryBlock encoded;
    proc.getStateInformation(encoded);
    const std::unique_ptr<juce::XmlElement> xml(proc.getXmlFromBinary(encoded.getData(), (int)encoded.getSize()));
    REQUIRE(xml != nullptr);
    const auto parent = juce::ValueTree::fromXml(*xml);
    const auto chain = parent.getChildWithName("KhensuHostedChain");
    REQUIRE(chain.isValid());
    REQUIRE(static_cast<int>(chain.getProperty("version")) == 1);
    REQUIRE(chain.getNumChildren() == 1);

    const auto slot = chain.getChild(0);
    CHECK(slot.getProperty("name").toString() == "Stateful");
    CHECK(slot.getProperty("filePath").toString() == "C:/plugins/stateful.vst3");
    CHECK(static_cast<bool>(slot.getProperty("bypassed")));
    const float wetMix = static_cast<float>(slot.getProperty("wetMix"));
    CHECK(wetMix == Catch::Approx(0.35f));
    const auto* savedState = slot.getProperty("pluginState").getBinaryData();
    REQUIRE(savedState != nullptr);
    CHECK(*savedState == expected);
}

TEST_CASE("Worker parameter updates notify the host on the message thread", "[plugin][cxx05]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    auto* parameter = proc.getAPVTS().getParameter(KhensuCopilotProcessor::ParamIDs::eqGainDb);
    auto* rawValue = proc.getAPVTS().getRawParameterValue(KhensuCopilotProcessor::ParamIDs::eqGainDb);
    REQUIRE(parameter != nullptr);
    REQUIRE(rawValue != nullptr);

    ThreadTrackingParameterListener listener;
    parameter->addListener(&listener);
    const auto messageThread = std::this_thread::get_id();
    std::thread worker([&proc] { proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqGainDb, -3.0f); });
    worker.join();

    // The audio-facing value is visible immediately, but the listener/host
    // notification has not executed on the worker thread.
    CHECK(rawValue->load(std::memory_order_relaxed) == Catch::Approx(-3.0f));
    CHECK(listener.callbackCount.load(std::memory_order_relaxed) == 0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(listener.callbackCount.load(std::memory_order_relaxed) == 1);
    CHECK(listener.callbackThread == messageThread);
    parameter->removeListener(&listener);
}

TEST_CASE("A saved hosted slot without a wetMix property restores fully wet", "[plugin][audit085]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::MemoryBlock encoded;
    {
        KhensuCopilotProcessor source;
        source.setPlayConfigDetails(2, 2, 48000.0, 512);
        source.prepareToPlay(48000.0, 512);
        source.addPluginInstanceToChain(std::make_unique<StatefulInstance>(juce::MemoryBlock()), "Old",
                                        "C:/plugins/not-installed-here.vst3");
        source.getStateInformation(encoded);
    }

    // An older / hand-edited project: drop the slot's wetMix property.
    const std::unique_ptr<juce::XmlElement> xml(
        KhensuCopilotProcessor::getXmlFromBinary(encoded.getData(), static_cast<int>(encoded.getSize())));
    REQUIRE(xml != nullptr);
    auto tree = juce::ValueTree::fromXml(*xml);
    auto slot = tree.getChildWithName("KhensuHostedChain").getChild(0);
    REQUIRE(slot.isValid());
    slot.removeProperty("wetMix", nullptr);
    juce::MemoryBlock edited;
    KhensuCopilotProcessor::copyXmlToBinary(*tree.createXml(), edited);

    KhensuCopilotProcessor restored;
    restored.setPlayConfigDetails(2, 2, 48000.0, 512);
    restored.prepareToPlay(48000.0, 512);
    restored.setStateInformation(edited.getData(), static_cast<int>(edited.getSize()));
    for (int i = 0; i < 20 && restored.isHostedRestorePending(); ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);

    const auto slots = restored.getHostedPluginSlots();
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].missing); // the path does not exist here: a placeholder
    CHECK(slots[0].wetMix == Catch::Approx(1.0f)); // was 0 (a silent slot) before AUDIT-085
}

TEST_CASE("Bool parameters round-trip exactly through get/setStateInformation", "[plugin][pluginval]")
{
    // pluginval's state-restoration sequence: read getValue(), save, write a
    // different raw value on the same side of 0.5, restore, compare. APVTS
    // stores only the snapped bool and skips restoring an unchanged one, so
    // the raw value used to stay at the second write (0.95 vs 0.55) while the
    // VST3 host's cache held the first.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);

    int boolParams = 0;
    for (auto* parameter : proc.getParameters()) {
        auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(parameter);
        if (boolParam == nullptr) continue;
        ++boolParams;
        for (const auto [first, second] : {std::pair{0.55f, 0.95f}, std::pair{0.2f, 0.45f}, std::pair{0.3f, 0.9f}}) {
            parameter->setValueNotifyingHost(first);
            juce::MemoryBlock state;
            proc.getStateInformation(state);
            parameter->setValueNotifyingHost(second);
            proc.setStateInformation(state.getData(), (int)state.getSize());
            CHECK(parameter->getValue() == Catch::Approx(first).margin(1e-6));
            CHECK(boolParam->get() == (first >= 0.5f));

            // strictness >= 8: the state read back must equal what was set.
            juce::MemoryBlock again;
            proc.getStateInformation(again);
            CHECK(again == state);
        }
    }
    CHECK(boolParams >= 4);
}

TEST_CASE("Invalid state blobs are ignored without touching the current state", "[plugin][clap]")
{
    // clap-validator state-invalid-random: random bytes (and near-valid blobs)
    // must never change the plugin's state.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    auto* gain = proc.getAPVTS().getParameter(KhensuCopilotProcessor::ParamIDs::eqGainDb);
    REQUIRE(gain != nullptr);
    gain->setValueNotifyingHost(0.8f);

    juce::MemoryBlock before;
    proc.getStateInformation(before);
    const float gainBefore = gain->getValue();

    auto checkUnchanged = [&](const juce::MemoryBlock& blob) {
        proc.setStateInformation(blob.getData(), (int)blob.getSize());
        CHECK(proc.lastStateRejection().isNotEmpty());
        CHECK(gain->getValue() == Catch::Approx(gainBefore).margin(1e-6));
        juce::MemoryBlock after;
        proc.getStateInformation(after);
        CHECK(after == before);
    };

    juce::Random rng(20261107);
    for (int round = 0; round < 64; ++round) {
        juce::MemoryBlock blob((size_t)(rng.nextInt(4096) + 1));
        for (size_t i = 0; i < blob.getSize(); ++i) blob[i] = (char)rng.nextInt(256);
        if (round % 2 == 1 && blob.getSize() >= 8) { // a valid magic, random everything else
            const juce::uint32 magic = 0x21324356;
            std::memcpy(blob.getData(), &magic, 4);
        }
        checkUnchanged(blob);
    }

    proc.setStateInformation(nullptr, 0); // empty
    CHECK(proc.lastStateRejection().isNotEmpty());

    // A well-formed header whose declared length overruns the data.
    {
        juce::MemoryBlock truncated(before);
        truncated.setSize(before.getSize() / 2);
        checkUnchanged(truncated);
    }
    // Valid XML with a foreign root.
    {
        juce::MemoryBlock foreign;
        juce::AudioProcessor::copyXmlToBinary(juce::XmlElement("SomeOtherPlugin"), foreign);
        checkUnchanged(foreign);
    }
    // Our root, but a non-finite parameter value and a bogus version.
    {
        const std::unique_ptr<juce::XmlElement> xml(
            KhensuCopilotProcessor::getXmlFromBinary(before.getData(), (int)before.getSize()));
        REQUIRE(xml != nullptr);
        auto tree = juce::ValueTree::fromXml(*xml);
        auto nanTree = tree.createCopy();
        for (int i = 0; i < nanTree.getNumChildren(); ++i)
            if (nanTree.getChild(i).hasType("PARAM")) {
                nanTree.getChild(i).setProperty("value", "nan", nullptr);
                break;
            }
        juce::MemoryBlock nanBlob;
        juce::AudioProcessor::copyXmlToBinary(*nanTree.createXml(), nanBlob);
        checkUnchanged(nanBlob);

        auto badVersion = tree.createCopy();
        badVersion.setProperty("khensuStateVersion", "x7", nullptr);
        juce::MemoryBlock badVersionBlob;
        juce::AudioProcessor::copyXmlToBinary(*badVersion.createXml(), badVersionBlob);
        checkUnchanged(badVersionBlob);

        auto noParams = tree.createCopy();
        for (int i = noParams.getNumChildren(); --i >= 0;)
            if (noParams.getChild(i).hasType("PARAM")) noParams.removeChild(i, nullptr);
        juce::MemoryBlock noParamsBlob;
        juce::AudioProcessor::copyXmlToBinary(*noParams.createXml(), noParamsBlob);
        checkUnchanged(noParamsBlob);
    }

    // And a real state still restores.
    gain->setValueNotifyingHost(0.2f);
    proc.setStateInformation(before.getData(), (int)before.getSize());
    CHECK(proc.lastStateRejection().isEmpty());
    CHECK(gain->getValue() == Catch::Approx(gainBefore).margin(1e-6));
}

TEST_CASE("The last state rejection is handed out as a copy (FIX-18)", "[plugin][fix18]")
{
    // Hosts call setStateInformation off the message thread. A reference to
    // the member would be read while that thread rewrites the string.
    CHECK_FALSE(std::is_reference_v<decltype(std::declval<const KhensuCopilotProcessor&>().lastStateRejection())>);
}

TEST_CASE("The last state rejection can be read while a host restores state (FIX-18)", "[plugin][fix18race]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);

    std::atomic<int> rounds{0};
    std::thread host([&] {
        juce::Random rng(18);
        for (int round = 0; round < 300; ++round) {
            juce::MemoryBlock blob((size_t)(rng.nextInt(512) + 1));
            for (size_t i = 0; i < blob.getSize(); ++i) blob[i] = (char)rng.nextInt(256);
            proc.setStateInformation(blob.getData(), (int)blob.getSize());
            proc.setStateInformation(nullptr, 0);
            rounds.fetch_add(1);
        }
    });
    int totalLength = 0;
    while (rounds.load() < 300) totalLength += proc.lastStateRejection().length();
    host.join();
    CHECK(totalLength >= 0);
    CHECK(proc.lastStateRejection().isNotEmpty());
}
