// tests/plugin/CopilotFixAutomationTest.cpp
// CE-DAW-4: "Write fix as automation" - a fix becomes ramped curves on the
// Copilot's own parameters, played inside change gestures while the host
// plays through the fix's span, so a host in write/touch mode records it.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/automation/FixAutomation.h"

#include <map>
#include <string>
#include <vector>

using Catch::Approx;
using khensu::KhensuCopilotProcessor;
using khensu::automation::curveFromFix;
using khensu::automation::FixAutomationWriter;
using khensu::automation::IGestureSink;
using khensu::automation::Lane;

namespace {

struct RecordingSink final : IGestureSink {
    struct Event {
        char kind; // 'b' begin, 's' set, 'e' end
        std::string id;
        float value;
    };
    std::vector<Event> events;
    bool beginGesture(const std::string& id) override
    {
        events.push_back({'b', id, 0.0f});
        return true;
    }
    void setValue(const std::string& id, float v) override { events.push_back({'s', id, v}); }
    void endGesture(const std::string& id) override { events.push_back({'e', id, 0.0f}); }
    int count(char kind, const std::string& id) const
    {
        int n = 0;
        for (const auto& e : events)
            if (e.kind == kind && e.id == id) ++n;
        return n;
    }
    std::vector<float> values(const std::string& id) const
    {
        std::vector<float> out;
        for (const auto& e : events)
            if (e.kind == 's' && e.id == id) out.push_back(e.value);
        return out;
    }
};

float zeroBaseline(const std::string& id) { return id == "out_gain_db" ? 1.0f : 0.0f; }

} // namespace

TEST_CASE("Lane interpolates, holds, and steps right-continuously", "[plugin][cedaw4]")
{
    Lane lane;
    lane.points = {{1.0, 0.0f}, {2.0, 10.0f}, {2.0, -5.0f}, {3.0, -5.0f}};
    CHECK(lane.valueAt(0.0) == 0.0f);
    CHECK(lane.valueAt(1.5) == Approx(5.0f));
    CHECK(lane.valueAt(2.0) == Approx(-5.0f)); // the later point at a step wins
    CHECK(lane.valueAt(9.0) == -5.0f);
}

TEST_CASE("curveFromFix maps suggested parameters, ramps, and enables the stage", "[plugin][cedaw4]")
{
    const std::map<std::string, float> fix{{"eq_gain_db", -3.0f}, {"eq_freq_hz", 250.0f}, {"reduce_gain_db", 2.0f},
                                           {"saturation_drive", 1.0f}};
    const auto curve = curveFromFix(fix, 10.0, 14.0, 0.5, zeroBaseline);
    std::map<std::string, const Lane*> byId;
    for (const auto& lane : curve.lanes) byId[lane.paramId] = &lane;
    REQUIRE(byId.count("eq_gain_db") == 1);
    REQUIRE(byId.count("eq_freq") == 1);
    REQUIRE(byId.count("eq_on") == 1);
    REQUIRE(byId.count("out_gain_db") == 1);
    REQUIRE(curve.unmappedKeys == std::vector<std::string>{"saturation_drive"});
    CHECK(byId["eq_gain_db"]->valueAt(9.0) == 0.0f);
    CHECK(byId["eq_gain_db"]->valueAt(9.75) == Approx(-1.5f));
    CHECK(byId["eq_gain_db"]->valueAt(12.0) == -3.0f);
    CHECK(byId["eq_gain_db"]->valueAt(14.5) == 0.0f);
    // Relative gain from the CURRENT output gain (1 dB) - a 2 dB cut.
    CHECK(byId["out_gain_db"]->valueAt(12.0) == Approx(-1.0f));
    CHECK(curve.startSec() == Approx(9.5));
    CHECK(curve.endSec() == Approx(14.5));
    CHECK(curveFromFix(fix, 5.0, 5.0, 0.5, zeroBaseline).empty());
}

TEST_CASE("FixAutomationWriter writes inside gestures only while the host plays the span", "[plugin][cedaw4]")
{
    RecordingSink sink;
    FixAutomationWriter writer;
    writer.arm(curveFromFix({{"eq_gain_db", -6.0f}}, 10.0, 12.0, 0.0, zeroBaseline), sink);
    REQUIRE(writer.state() == FixAutomationWriter::State::Armed);

    writer.tick(5.0, true, sink);  // before the span: nothing
    writer.tick(10.5, false, sink); // in the span but stopped: nothing
    CHECK(sink.events.empty());

    for (double t = 9.9; t < 12.3; t += 1.0 / 30.0) writer.tick(t, true, sink);
    CHECK(writer.state() == FixAutomationWriter::State::Finished);
    CHECK(writer.progress() == Approx(1.0));
    for (const char* id : {"eq_gain_db", "eq_on"}) {
        INFO(id);
        CHECK(sink.count('b', id) == 1);
        CHECK(sink.count('e', id) == 1);
    }
    // Every set happens between its lane's begin and end.
    bool open = false;
    for (const auto& e : sink.events) {
        if (e.id != "eq_gain_db") continue;
        if (e.kind == 'b') open = true;
        if (e.kind == 'e') open = false;
        if (e.kind == 's') CHECK(open);
    }
    const auto gains = sink.values("eq_gain_db");
    REQUIRE_FALSE(gains.empty());
    CHECK(gains.front() == Approx(-6.0f));
    CHECK(gains.back() == Approx(0.0f)); // lands on the restored value at the end
}

TEST_CASE("FixAutomationWriter closes gestures on stop and re-arms for another pass", "[plugin][cedaw4]")
{
    RecordingSink sink;
    FixAutomationWriter writer;
    writer.arm(curveFromFix({{"comp_ratio", 3.0f}}, 0.0, 4.0, 0.0, zeroBaseline), sink);
    writer.tick(1.0, true, sink);
    REQUIRE(writer.state() == FixAutomationWriter::State::Writing);
    writer.tick(1.1, false, sink); // host stopped mid-span
    CHECK(writer.state() == FixAutomationWriter::State::Armed);
    CHECK(sink.count('b', "comp_ratio") == 1);
    CHECK(sink.count('e', "comp_ratio") == 1);
    writer.tick(0.5, true, sink); // a second pass writes again
    CHECK(writer.state() == FixAutomationWriter::State::Writing);
    CHECK(sink.count('b', "comp_ratio") == 2);
    writer.cancel(sink);
    CHECK(writer.state() == FixAutomationWriter::State::Idle);
    CHECK(sink.count('e', "comp_ratio") == 2);
}

TEST_CASE("The processor arms a fix on its own exposed parameters with host gestures", "[plugin][cedaw4]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, 48000.0, 512);
    proc.prepareToPlay(48000.0, 512);

    struct GestureListener final : juce::AudioProcessorParameter::Listener {
        int begins = 0, ends = 0, changes = 0;
        void parameterValueChanged(int, float) override { ++changes; }
        void parameterGestureChanged(int, bool starting) override { (starting ? begins : ends) += 1; }
    } listener;
    auto* gain = proc.getAPVTS().getParameter(KhensuCopilotProcessor::ParamIDs::eqGainDb);
    REQUIRE(gain != nullptr);
    gain->addListener(&listener);

    khensu::timeline::TimedIssue issue;
    issue.startSec = 0.0;
    issue.endSec = 1.0;
    issue.title = "Boxy 300 Hz";
    issue.suggestedParameters = {{"eq_gain_db", -4.0f}, {"unknown_knob", 1.0f}};
    juce::String message;
    REQUIRE(proc.armFixAutomation(issue, message));
    CHECK(proc.getFixAutomationState() == FixAutomationWriter::State::Armed);

    khensu::timeline::TimedIssue none;
    none.startSec = 0.0;
    none.endSec = 1.0;
    none.suggestedParameters = {{"unknown_knob", 1.0f}};
    juce::String noneMessage;
    CHECK_FALSE(proc.armFixAutomation(none, noneMessage));
    CHECK(noneMessage.isNotEmpty());

    // Drive the host: play through the span; the writer's 30 Hz timer runs
    // on the message thread and must wrap its writes in gestures.
    struct PlayHead final : juce::AudioPlayHead {
        double t = 0.5;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setTimeInSeconds(t);
            info.setIsPlaying(true);
            return info;
        }
    } playHead;
    proc.setPlayHead(&playHead);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    buf.clear();
    proc.processBlock(buf, midi); // telemetry now reports t = 0.5, playing
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    CHECK(proc.getFixAutomationState() == FixAutomationWriter::State::Writing);
    CHECK(listener.begins == 1);
    CHECK(listener.changes >= 1);
    CHECK(proc.getParamPlain("eq_gain_db") == Approx(-4.0f).margin(0.05f));

    playHead.t = 2.0; // past the span (end + ramp = 1.25 s)
    proc.processBlock(buf, midi);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    CHECK(proc.getFixAutomationState() == FixAutomationWriter::State::Finished);
    CHECK(listener.ends == 1);
    CHECK(proc.getParamPlain("eq_gain_db") == Approx(0.0f).margin(0.05f));

    REQUIRE(proc.armFixAutomation(issue, message));
    proc.cancelFixAutomation();
    CHECK(proc.getFixAutomationState() == FixAutomationWriter::State::Idle);
    gain->removeListener(&listener);
    proc.setPlayHead(nullptr);
}
