// tests/ui/DesktopPluginChainTest.cpp
// Desktop third-party plugin hosting: chain order, bypass, latency-
// compensated offline render, MIDI-driven instruments, and persistence.
// Uses fake AudioPluginInstances - no plugin binaries or scanning.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "session/DesktopPluginChain.h"

using Catch::Approx;
using khensu::DesktopPluginChain;

namespace {
/// Minimal configurable fake: gain, fixed latency (a pure delay), or a
/// "synth" that writes a constant while any MIDI note is held.
class FakeInstance final : public juce::AudioPluginInstance {
  public:
    FakeInstance(juce::String name, float gain, int latency, bool instrument = false)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), !instrument)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
          name_(std::move(name)), gain_(gain), latency_(latency), instrument_(instrument)
    {
        setLatencySamples(latency_);
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = name_;
        d.pluginFormatName = "Fake";
        d.isInstrument = instrument_;
    }
    const juce::String getName() const override { return name_; }
    void prepareToPlay(double, int) override
    {
        ring_.assign(2, std::vector<float>(static_cast<size_t>(juce::jmax(1, latency_)), 0.0f));
        pos_ = 0;
        ++prepareCount;
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        if (instrument_) {
            for (const auto meta : midi) {
                if (meta.getMessage().isNoteOn()) noteOn_ = true;
                if (meta.getMessage().isNoteOff()) noteOn_ = false;
            }
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.addSample(c, i, noteOn_ ? 0.5f : 0.0f);
            return;
        }
        buffer.applyGain(gain_);
        if (latency_ <= 0) return;
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            for (int c = 0; c < 2; ++c) {
                const float in = buffer.getSample(c, i);
                buffer.setSample(c, i, ring_[static_cast<size_t>(c)][pos_]);
                ring_[static_cast<size_t>(c)][pos_] = in;
            }
            pos_ = (pos_ + 1) % static_cast<size_t>(latency_);
        }
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override { dest.append("gain", 4); }
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return instrument_; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int prepareCount = 0;

  private:
    juce::String name_;
    float gain_;
    int latency_;
    bool instrument_;
    bool noteOn_ = false;
    std::vector<std::vector<float>> ring_;
    size_t pos_ = 0;
};

juce::AudioBuffer<float> impulse(int length, int at)
{
    juce::AudioBuffer<float> b(2, length);
    b.clear();
    b.setSample(0, at, 1.0f);
    b.setSample(1, at, 1.0f);
    return b;
}
} // namespace

TEST_CASE("Hosted chain applies slots in order and honours bypass", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    DesktopPluginChain chain;
    REQUIRE(chain.addInstance(std::make_unique<FakeInstance>("Half", 0.5f, 0), "C:/half.vst3"));
    REQUIRE(chain.addInstance(std::make_unique<FakeInstance>("Quarter", 0.25f, 0)));
    REQUIRE(chain.size() == 2);
    CHECK(chain.slots()[0].name == "Half");
    CHECK(chain.slots()[0].filePath == "C:/half.vst3");

    auto a = impulse(2000, 100);
    CHECK(chain.renderBuffer(a, 48000.0) == 0);
    CHECK(a.getSample(0, 100) == Approx(0.125f));

    REQUIRE(chain.setBypassed(1, true));
    auto b = impulse(2000, 100);
    chain.renderBuffer(b, 48000.0);
    CHECK(b.getSample(1, 100) == Approx(0.5f));

    REQUIRE(chain.move(1, 0));
    CHECK(chain.slots()[0].name == "Quarter");
    CHECK(chain.slots()[0].bypassed);
    CHECK_FALSE(chain.move(0, 5));
    REQUIRE(chain.remove(0));
    CHECK(chain.size() == 1);
    CHECK_FALSE(chain.remove(3));
}

TEST_CASE("Hosted chain render compensates total plugin latency", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    DesktopPluginChain chain;
    chain.addInstance(std::make_unique<FakeInstance>("Look-ahead", 1.0f, 300));
    chain.addInstance(std::make_unique<FakeInstance>("Linear phase", 1.0f, 700));
    CHECK(chain.totalLatencySamples() == 1000);

    auto audio = impulse(4000, 1234);
    CHECK(chain.renderBuffer(audio, 44100.0) == 1000);
    CHECK(audio.getNumSamples() == 4000);
    // The impulse comes back at its original position: A/B stays aligned.
    CHECK(audio.getSample(0, 1234) == Approx(1.0f));
    CHECK(audio.getSample(0, 1233) == Approx(0.0f));

    // Bypassing a slot removes its latency from the compensation.
    chain.setBypassed(1, true);
    CHECK(chain.totalLatencySamples() == 300);
    auto again = impulse(4000, 1234);
    CHECK(chain.renderBuffer(again, 44100.0) == 300);
    CHECK(again.getSample(1, 1234) == Approx(1.0f));
}

TEST_CASE("Instrument slots play MIDI during render", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    DesktopPluginChain chain;
    chain.addInstance(std::make_unique<FakeInstance>("Synth", 1.0f, 0, true));
    CHECK(chain.slots()[0].isInstrument);

    juce::MidiMessageSequence seq;
    seq.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 1000.0);
    seq.addEvent(juce::MidiMessage::noteOff(1, 60), 3000.0);
    juce::AudioBuffer<float> audio(2, 5000);
    audio.clear();
    chain.renderBuffer(audio, 48000.0, {}, &seq);
    CHECK(audio.getSample(0, 500) == Approx(0.0f));
    CHECK(audio.getSample(0, 2000) == Approx(0.5f)); // note held
    CHECK(audio.getSample(0, 4500) == Approx(0.0f));
}

TEST_CASE("Hosted chain render stops when cancelled and caps slot count", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    DesktopPluginChain chain;
    for (int i = 0; i < DesktopPluginChain::kMaxSlots; ++i)
        REQUIRE(chain.addInstance(std::make_unique<FakeInstance>("Unity", 1.0f, 0)));
    CHECK_FALSE(chain.addInstance(std::make_unique<FakeInstance>("Overflow", 1.0f, 0)));
    CHECK_FALSE(chain.addInstance(nullptr));

    int polls = 0;
    auto audio = impulse(48000, 10);
    chain.renderBuffer(audio, 48000.0, [&polls] { return ++polls > 2; });
    CHECK(polls == 3); // stopped at the first poll that returned true
}

TEST_CASE("Hosted chain persists order, bypass and plugin state", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    DesktopPluginChain chain;
    chain.addInstance(std::make_unique<FakeInstance>("A", 1.0f, 0), "C:/Plugins/A.vst3");
    chain.addInstance(std::make_unique<FakeInstance>("B", 1.0f, 0), "C:/Plugins/B.vst3");
    chain.setBypassed(1, true);

    const auto tree = chain.toValueTree();
    const auto reparsed = juce::ValueTree::fromXml(tree.toXmlString());
    const auto paths = DesktopPluginChain::savedSlotPaths(reparsed);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0].first == "C:/Plugins/A.vst3");
    CHECK_FALSE(paths[0].second);
    CHECK(paths[1].second);
    CHECK(reparsed.getChild(0)["state"].toString().isNotEmpty());
    CHECK(DesktopPluginChain::savedSlotPaths(juce::ValueTree("Other")).empty());
}

TEST_CASE("Hosted chain renders a WAV file end to end", "[hosting]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-hosting-test");
    dir.createDirectory();
    const auto in = dir.getChildFile("in.wav"), out = dir.getChildFile("out.wav");
    {
        in.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = in.createOutputStream();
        auto w = wav.createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
        REQUIRE(w != nullptr);
        juce::AudioBuffer<float> b(2, 4800);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4800; ++i) b.setSample(c, i, 0.4f);
        w->writeFromAudioSampleBuffer(b, 0, 4800);
    }
    DesktopPluginChain chain;
    chain.addInstance(std::make_unique<FakeInstance>("Half", 0.5f, 64));
    const auto r = chain.renderFile(in, out);
    REQUIRE(r.ok);
    CHECK(r.compensatedLatencySamples == 64);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(out));
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == 4800);
    juce::AudioBuffer<float> got(2, 4800);
    reader->read(&got, 0, 4800, 0, true, true);
    CHECK(got.getSample(0, 2000) == Approx(0.2f).margin(1e-4));
    CHECK(got.getSample(0, 0) == Approx(0.2f).margin(1e-4)); // latency trimmed, no leading silence
    reader.reset();
    dir.deleteRecursively();
}

TEST_CASE("Hosted chain render writes float and keeps plugin overs (AUDIT-065)", "[hosting][audit065]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-hosting-float");
    dir.createDirectory();
    const auto in = dir.getChildFile("in.wav"), out = dir.getChildFile("out.wav");
    {
        in.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = in.createOutputStream();
        auto w = wav.createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(w != nullptr);
        juce::AudioBuffer<float> b(1, 10000); // mono, spans many render blocks
        for (int i = 0; i < 10000; ++i) b.setSample(0, i, 0.8f);
        w->writeFromAudioSampleBuffer(b, 0, 10000);
    }
    DesktopPluginChain chain;
    chain.addInstance(std::make_unique<FakeInstance>("Hot", 2.0f, 100)); // +6 dB: 0.8 -> 1.6
    const auto r = chain.renderFile(in, out);
    REQUIRE(r.ok);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(out));
    REQUIRE(reader != nullptr);
    CHECK(reader->usesFloatingPointData);
    CHECK(reader->bitsPerSample == 32);
    CHECK(reader->numChannels == 2); // mono source widened to the stereo chain
    CHECK(reader->lengthInSamples == 10000);
    juce::AudioBuffer<float> got(2, 10000);
    reader->read(&got, 0, 10000, 0, false, false);
    CHECK(got.getSample(0, 5000) == Approx(1.6f).margin(1e-3)); // over preserved, not clipped at 1.0
    CHECK(got.getSample(1, 9999) == Approx(1.6f).margin(1e-3)); // latency tail flushed to the end
    reader.reset();

    // A cancelled render leaves no output file behind.
    out.deleteFile();
    const auto cancelled = chain.renderFile(in, out, [] { return true; });
    CHECK_FALSE(cancelled.ok);
    CHECK_FALSE(out.existsAsFile());
    dir.deleteRecursively();
}
