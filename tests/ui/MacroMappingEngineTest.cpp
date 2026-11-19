// tests/ui/MacroMappingEngineTest.cpp
// MIDI/OSC macro mapping: learn mode, range/curve/invert, one-control-one-
// macro replacement, normalisation, and persistence round-trip.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "midi/MacroMappingEngine.h"

using Catch::Approx;
using khensu::MacroMappingEngine;
using Source = MacroMappingEngine::Source;
using Type = MacroMappingEngine::SourceType;

namespace {
Source cc(int number, int channel = 0) { return {Type::MidiCC, channel, number, {}}; }
Source osc(const char* address) { return {Type::Osc, -1, 0, address}; }
} // namespace

TEST_CASE("Learn mode binds the next control to the armed macro", "[macro-map]")
{
    MacroMappingEngine e({"Presence", "Width"});
    CHECK_FALSE(e.armLearn("Nope"));
    REQUIRE(e.armLearn("Presence"));
    REQUIRE(e.learnTarget().has_value());

    const auto first = e.handle(cc(74), 0.5);
    REQUIRE(first.size() == 1);
    CHECK(first[0].target == "Presence");
    CHECK(first[0].value == Approx(50.0));
    CHECK_FALSE(e.learnTarget().has_value());
    REQUIRE(e.mappings().size() == 1);

    // Same CC drives it from now on; other controls do nothing.
    CHECK(e.handle(cc(74), 1.0)[0].value == Approx(100.0));
    CHECK(e.handle(cc(75), 1.0).empty());
    CHECK(e.handle(cc(74, 3), 0.0).empty()); // learned on channel 1 only

    // Re-learning a macro moves it to the new control and keeps its range.
    auto m = e.mappings()[0];
    m.minValue = 20.0;
    m.maxValue = 40.0;
    REQUIRE(e.updateMapping(0, m));
    e.armLearn("Presence");
    e.handle(osc("/fader/1"), 0.0);
    REQUIRE(e.mappings().size() == 1);
    CHECK(e.mappings()[0].source.type == Type::Osc);
    CHECK(e.handle(osc("/fader/1"), 1.0)[0].value == Approx(40.0));
    CHECK(e.handle(cc(74), 1.0).empty());
}

TEST_CASE("One control drives at most one macro", "[macro-map]")
{
    MacroMappingEngine e({"Presence", "Width"});
    MacroMappingEngine::Mapping a;
    a.source = cc(10);
    a.target = "Presence";
    REQUIRE(e.setMapping(a));
    a.target = "Width";
    REQUIRE(e.setMapping(a)); // same CC: replaces
    REQUIRE(e.mappings().size() == 1);
    CHECK(e.mappings()[0].target == "Width");
    a.target = "Unknown";
    CHECK_FALSE(e.setMapping(a));
    CHECK(e.removeMapping(0));
    CHECK_FALSE(e.removeMapping(0));
}

TEST_CASE("Curves, ranges and invert shape the response", "[macro-map]")
{
    MacroMappingEngine::Mapping m;
    m.minValue = 10.0;
    m.maxValue = 90.0;
    CHECK(m.apply(0.5) == Approx(50.0));
    CHECK(m.apply(2.0) == Approx(90.0)); // clamped
    m.curve = MacroMappingEngine::Curve::Exponential;
    CHECK(m.apply(0.5) == Approx(30.0));
    m.curve = MacroMappingEngine::Curve::Logarithmic;
    CHECK(m.apply(0.25) == Approx(50.0));
    m.curve = MacroMappingEngine::Curve::SCurve;
    CHECK(m.apply(0.5) == Approx(50.0));
    CHECK(m.apply(0.1) < 10.0 + 80.0 * 0.1);
    m.curve = MacroMappingEngine::Curve::Linear;
    m.invert = true;
    CHECK(m.apply(0.0) == Approx(90.0));
    // Reversed range works too.
    m.invert = false;
    m.minValue = 100.0;
    m.maxValue = 0.0;
    CHECK(m.apply(0.25) == Approx(75.0));
}

TEST_CASE("MIDI and OSC values normalise from their native resolution", "[macro-map]")
{
    CHECK(MacroMappingEngine::normaliseMidi(127, 7) == Approx(1.0));
    CHECK(MacroMappingEngine::normaliseMidi(64, 0) == Approx(64.0 / 127.0)); // 0 bits = MIDI 1.0
    CHECK(MacroMappingEngine::normaliseMidi(8192, 14) == Approx(8192.0 / 16383.0));
    CHECK(MacroMappingEngine::normaliseMidi(0xFFFFFFFFu, 32) == Approx(1.0));
    CHECK(MacroMappingEngine::normaliseOsc(juce::var(0.25)) == Approx(0.25));
    CHECK(MacroMappingEngine::normaliseOsc(juce::var(127)) == Approx(1.0));
    CHECK(MacroMappingEngine::normaliseOsc(juce::var(3.0)) == Approx(1.0));
}

TEST_CASE("Macro mappings persist and reload, skipping bad rows", "[macro-map]")
{
    MacroMappingEngine e({"Presence", "Width", "Reverb"});
    MacroMappingEngine::Mapping a;
    a.source = cc(1, 2);
    a.target = "Presence";
    a.minValue = 5.0;
    a.maxValue = 60.0;
    a.curve = MacroMappingEngine::Curve::SCurve;
    a.invert = true;
    e.setMapping(a);
    MacroMappingEngine::Mapping b;
    b.source = osc("/khensu/width");
    b.target = "Width";
    e.setMapping(b);

    auto json = juce::JSON::toString(e.toVar());
    auto parsed = juce::JSON::parse(json);
    // Append malformed rows: unknown type, OSC without leading slash, unknown target.
    if (auto* arr = parsed.getArray()) {
        arr->add(juce::JSON::parse(R"({"type": "sysex", "target": "Reverb"})"));
        arr->add(juce::JSON::parse(R"({"type": "osc", "osc_address": "bad", "target": "Reverb"})"));
        arr->add(juce::JSON::parse(R"({"type": "cc", "number": 9, "target": "Ghost"})"));
    }

    MacroMappingEngine restored({"Presence", "Width", "Reverb"});
    CHECK(restored.fromVar(parsed) == 2);
    REQUIRE(restored.mappings().size() == 2);
    const auto& r = restored.mappings()[0];
    CHECK(r.source.channel == 2);
    CHECK(r.minValue == Approx(5.0));
    CHECK(r.curve == MacroMappingEngine::Curve::SCurve);
    CHECK(r.invert);
    CHECK(restored.mappings()[1].source.oscAddress == "/khensu/width");
    CHECK(r.source.describe() == "MIDI CC 1 (ch 3)");
}
