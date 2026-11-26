// tests/ui/MidiCiResponderTest.cpp
// STU-13: the MIDI-CI responder answers a real juce::midi_ci initiator -
// discovery, the ResourceList, one read-only property per mappable control,
// and refuses a set.
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <optional>
#include "midi/MidiCiResponder.h"

using namespace khensu;
namespace ci = juce::midi_ci;

namespace {
using Packet = std::pair<std::uint8_t, std::vector<std::byte>>;

struct QueueOut final : ci::DeviceMessageHandler {
    std::deque<Packet> queue;
    void processMessage(juce::ump::BytesOnGroup b) override
    {
        queue.emplace_back(b.group, std::vector<std::byte>(b.bytes.begin(), b.bytes.end()));
    }
};
} // namespace

TEST_CASE("STU-13: a MIDI-CI initiator discovers the app and reads its controls", "[midi][midi-ci]")
{
    juce::ScopedJuceInitialiser_GUI init;
    MacroMappingEngine engine({"Presence", "De-essing"});
    MacroMappingEngine::Mapping mapping;
    mapping.source = {MacroMappingEngine::SourceType::MidiCC, 0, 21, {}};
    mapping.target = "Presence";
    mapping.minValue = 10.0;
    mapping.maxValue = 90.0;
    REQUIRE(engine.setMapping(mapping));

    MidiCiResponder responder(nullptr, engine, [](const juce::String& t) { return t == "Presence" ? 42.0 : 7.0; });
    CHECK_FALSE(responder.isEnabled()); // off by default
    CHECK(responder.muid() == 0u);
    std::deque<Packet> toInitiator;
    responder.setSender([&toInitiator](juce::ump::BytesOnGroup b) {
        toInitiator.emplace_back(b.group, std::vector<std::byte>(b.bytes.begin(), b.bytes.end()));
    });
    responder.setEnabled(true);
    REQUIRE(responder.isEnabled());

    QueueOut toResponder;
    ci::Device initiator(ci::DeviceOptions{}
                             .withOutputs({&toResponder})
                             .withFeatures(ci::DeviceFeatures{}.withPropertyExchangeSupported(true))
                             .withMaxSysExSize(512));
    auto pump = [&] {
        for (int guard = 0; guard < 1000 && (!toResponder.queue.empty() || !toInitiator.empty()); ++guard) {
            if (!toResponder.queue.empty()) {
                const auto p = toResponder.queue.front();
                toResponder.queue.pop_front();
                responder.receive({p.first, juce::Span<const std::byte>(p.second.data(), p.second.size())});
            }
            if (!toInitiator.empty()) {
                const auto p = toInitiator.front();
                toInitiator.pop_front();
                initiator.processMessage({p.first, juce::Span<const std::byte>(p.second.data(), p.second.size())});
            }
        }
    };

    initiator.sendDiscovery();
    pump();
    const auto found = initiator.getDiscoveredMuids();
    REQUIRE(found.size() == 1);
    CHECK(found[0].get() == responder.muid());
    initiator.sendPropertyCapabilitiesInquiry(found[0]);
    pump();

    auto get = [&](const juce::String& resource, int& status) {
        std::optional<juce::var> body;
        ci::PropertyRequestHeader h;
        h.resource = resource;
        initiator.sendPropertyGetInquiry(found[0], h, [&](const ci::PropertyExchangeResult& r) {
            if (r.getError()) return;
            status = r.getHeaderAsReplyHeader().status;
            body = ci::Encodings::jsonFrom7BitText(r.getBody());
        });
        pump();
        return body;
    };

    int status = 0;
    const auto list = get("ResourceList", status);
    REQUIRE(list.has_value());
    CHECK(status == 200);
    REQUIRE(list->size() == 2);
    CHECK((*list)[0]["resource"].toString() == "X-Macro-Presence");
    CHECK((*list)[1]["resource"].toString() == "X-Macro-De-essing");
    CHECK((*list)[0]["canSet"].toString() == "none");

    const auto presence = get("X-Macro-Presence", status);
    REQUIRE(presence.has_value());
    CHECK(status == 200);
    CHECK(static_cast<double>((*presence)["value"]) == 42.0);
    CHECK((*presence)["mapping"]["source"].toString().contains("21"));
    CHECK(static_cast<double>((*presence)["mapping"]["valueAtMax"]) == 90.0);
    const auto deess = get("X-Macro-De-essing", status);
    REQUIRE(deess.has_value());
    CHECK(static_cast<double>((*deess)["value"]) == 7.0);
    CHECK((*deess)["mapping"].isVoid());

    status = 0;
    get("X-Macro-Nothing", status);
    CHECK(status == 404);

    int setStatus = 0;
    ci::PropertyRequestHeader setHeader;
    setHeader.resource = "X-Macro-Presence";
    const auto payload = ci::Encodings::jsonTo7BitText(juce::var(5.0));
    initiator.sendPropertySetInquiry(found[0], setHeader, payload, [&](const ci::PropertyExchangeResult& r) {
        if (!r.getError()) setStatus = r.getHeaderAsReplyHeader().status;
    });
    pump();
    CHECK(setStatus == 405);

    responder.setEnabled(false);
    CHECK(responder.muid() == 0u);
}

TEST_CASE("STU-13: property names are clean ASCII resource ids", "[midi][midi-ci]")
{
    CHECK(MidiCiResponder::resourceNameFor("De-essing") == "X-Macro-De-essing");
    CHECK(MidiCiResponder::resourceNameFor("Low end (sub)") == "X-Macro-Lowendsub");
}
