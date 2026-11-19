// tests/plugin/FakeHostedInstances.h
// Minimal deterministic AudioPluginInstance fakes for KhensuCopilotProcessor
// tests (audits CQ-02/CQ-03/ARCH-04): a latency-bearing unity passthrough and
// a destructor-counting no-op. No plugin scanning, no external binaries.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <limits>
#include <string>
#include <vector>

namespace khensu::test {

/// Unity-gain passthrough that delays by `latencySamples` and reports it,
/// exactly like a lookahead-style third-party plugin. processBlock is
/// allocation-free after prepareToPlay (ring buffer preallocated there).
class FakeLatencyInstance : public juce::AudioPluginInstance {
  public:
    explicit FakeLatencyInstance(int latencySamples, double tailSeconds = 0.0)
        : latency_(latencySamples), tailSeconds_(tailSeconds)
    {
        setLatencySamples(latency_);
    }

    /// Re-reports a new latency the way a real plugin does after insert
    /// (e.g. switching a lookahead mode): AudioProcessor::setLatencySamples
    /// notifies the registered AudioProcessorListeners.
    void changeReportedLatency(int latencySamples) { setLatencySamples(latencySamples); }

    /// Really changes the delay (test-only: reallocates), then reports it.
    void changeLatency(int latencySamples)
    {
        latency_ = latencySamples;
        prepareToPlay(0.0, 0);
        setLatencySamples(latencySamples);
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "FakeLatency";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x4661;
    }

    const juce::String getName() const override { return "FakeLatency"; }

    void prepareToPlay(double, int) override
    {
        ring_.assign(2, std::vector<float>((size_t)juce::jmax(1, latency_), 0.0f));
        writePos_ = 0;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (latency_ <= 0) return;
        const int chans = juce::jmin(buffer.getNumChannels(), (int)ring_.size());
        const int n = buffer.getNumSamples();
        for (int i = 0; i < n; ++i) {
            const size_t pos = (writePos_ + (size_t)i) % (size_t)latency_;
            for (int c = 0; c < chans; ++c) {
                float* d = buffer.getWritePointer(c);
                const float in = d[i];
                d[i] = ring_[(size_t)c][pos];
                ring_[(size_t)c][pos] = in;
            }
        }
        writePos_ = (writePos_ + (size_t)n) % (size_t)latency_;
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return tailSeconds_; }

  private:
    int latency_ = 0;
    double tailSeconds_ = 0.0;
    std::vector<std::vector<float>> ring_;
    size_t writePos_ = 0;
};

/// No-op passthrough that bumps a shared counter on destruction, so tests can
/// assert WHEN hosted instances are actually reclaimed (audit ARCH-04).
class DtorCountingInstance : public juce::AudioPluginInstance {
  public:
    explicit DtorCountingInstance(std::atomic<int>& counter) : counter_(counter) {}
    ~DtorCountingInstance() override { counter_.fetch_add(1); }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "DtorCounter";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x4463;
    }
    const juce::String getName() const override { return "DtorCounter"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

  private:
    std::atomic<int>& counter_;
};

/// No-op instance with an opaque child-state blob, used to prove that the
/// parent VST state retains hosted-plugin state without interpreting it.
class StatefulInstance : public juce::AudioPluginInstance {
  public:
    explicit StatefulInstance(juce::MemoryBlock state) : state_(std::move(state)) {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "Stateful";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x5374;
    }
    const juce::String getName() const override { return "Stateful"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override { destination = state_; }
    void setStateInformation(const void* data, int sizeInBytes) override { state_.replaceAll(data, (size_t)sizeInBytes); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

  private:
    juce::MemoryBlock state_;
};

/// Stereo-by-default unity passthrough that records what the chain manager
/// does to it: bus-layout changes vs prepareToPlay order (M3), the rate and
/// block size it was last prepared with (M7), the largest block it was handed
/// (M4), the MIDI it receives (L8), and optionally emits a note into the
/// MIDI buffer so a following slot can check for leakage.
class RecordingInstance : public juce::AudioPluginInstance {
  public:
    explicit RecordingInstance(bool emitsMidi = false)
        : AudioPluginInstance(BusesProperties()
                                  .withInput("In", juce::AudioChannelSet::stereo(), true)
                                  .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
          emitsMidi_(emitsMidi)
    {
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "Recording";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x5265;
    }
    const juce::String getName() const override { return "Recording"; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet();
        return in == layouts.getMainOutputChannelSet()
               && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }
    void processorLayoutsChanged() override { events.push_back("layout"); }

    void prepareToPlay(double sampleRate, int blockSize) override
    {
        events.push_back("prepare");
        lastPrepareRate = sampleRate;
        lastPrepareBlock = blockSize;
        ++prepareCount;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        maxBlockSeen = juce::jmax(maxBlockSeen, buffer.getNumSamples());
        totalSamplesSeen += buffer.getNumSamples();
        midiEventsSeen += midi.getNumEvents();
        if (emitsMidi_) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return emitsMidi_; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::vector<std::string> events;
    double lastPrepareRate = 0.0;
    int lastPrepareBlock = 0;
    int prepareCount = 0;
    int maxBlockSeen = 0;
    int totalSamplesSeen = 0;
    int midiEventsSeen = 0;

  private:
    bool emitsMidi_ = false;
};

/// Stereo effect with a mono sidechain input that starts ENABLED, like TDR
/// Nova. With `refuseDisable` it insists on the sidechain (layout requests
/// that drop it are rejected). processBlock records whether the host handed
/// it fewer channels than it declared - a real plugin would read past the
/// channel table there - and halves the main channels so tests can tell it
/// ran.
class SidechainInstance : public juce::AudioPluginInstance {
  public:
    explicit SidechainInstance(bool refuseDisable)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                        .withInput("Sidechain", juce::AudioChannelSet::mono(), true)
                                        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          refuseDisable_(refuseDisable)
    {
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "Sidechain";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x5363;
    }
    const juce::String getName() const override { return "Sidechain"; }

    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        if (layout.getMainInputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        if (layout.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
        const bool sidechainOn = layout.inputBuses.size() > 1 && !layout.inputBuses[1].isDisabled();
        return sidechainOn || !refuseDisable_;
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int declared = juce::jmax(getTotalNumInputChannels(), getTotalNumOutputChannels());
        if (buffer.getNumChannels() < declared) undersizedBuffers.fetch_add(1);
        if (buffer.getNumChannels() >= 2) lastRightInput.store(buffer.getSample(1, buffer.getNumSamples() - 1));
        if (buffer.getNumChannels() >= 3) lastSidechainInput.store(buffer.getSample(2, buffer.getNumSamples() - 1));
        for (int c = 0; c < juce::jmin(2, buffer.getNumChannels()); ++c) buffer.applyGain(c, 0, buffer.getNumSamples(), 0.5f);
        processed.fetch_add(1);
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::atomic<int> undersizedBuffers{0};
    std::atomic<int> processed{0};
    std::atomic<float> lastRightInput{0.0f};
    std::atomic<float> lastSidechainInput{-1.0f};

  private:
    bool refuseDisable_ = false;
};

/// A plugin that only does mono (1 in / 1 out) and refuses any other layout,
/// with an optional reported latency. Halves what it is given. On a stereo
/// track it must not be handed the stereo buffer: it would process the left
/// channel and leave the right one dry and undelayed (FIX-14).
class MonoOnlyInstance : public juce::AudioPluginInstance {
  public:
    explicit MonoOnlyInstance(int latencySamples = 0)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("Input", juce::AudioChannelSet::mono(), true)
                                        .withOutput("Output", juce::AudioChannelSet::mono(), true)),
          latency_(juce::jmax(0, latencySamples))
    {
        setLatencySamples(latency_);
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "MonoOnly";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x4d6f;
    }
    const juce::String getName() const override { return "MonoOnly"; }

    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == juce::AudioChannelSet::mono()
               && layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    void prepareToPlay(double, int) override
    {
        ring_.assign((size_t)juce::jmax(1, latency_), 0.0f);
        writePos_ = 0;
    }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        maxChannelsSeen.store(juce::jmax(maxChannelsSeen.load(), buffer.getNumChannels()));
        if (buffer.getNumChannels() < 1) return;
        float* d = buffer.getWritePointer(0);
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            float out = d[i] * 0.5f;
            if (latency_ > 0) {
                const float delayed = ring_[writePos_];
                ring_[writePos_] = out;
                writePos_ = (writePos_ + 1) % ring_.size();
                out = delayed;
            }
            d[i] = out;
        }
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::atomic<int> maxChannelsSeen{0};

  private:
    int latency_ = 0;
    std::vector<float> ring_;
    size_t writePos_ = 0;
};

/// Passthrough whose getStateInformation blocks for `stallMs` - a heavy
/// third-party plugin serialising a big state. The chain manager holds its
/// control lock across that call (captureState), which is exactly what the
/// audio thread must never wait on (AUDIT-004).
class SlowStateInstance : public juce::AudioPluginInstance {
  public:
    explicit SlowStateInstance(int stallMs) : stallMs_(stallMs) {}

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "SlowState";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x5353;
    }
    const juce::String getName() const override { return "SlowState"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override
    {
        insideState.store(true);
        juce::Thread::sleep(stallMs_);
        insideState.store(false);
    }
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::atomic<bool> insideState{false};

  private:
    int stallMs_ = 0;
};

/// A latency-bearing plugin with a 32-channel main bus it refuses to shrink.
/// A non-owning AudioBuffer view of 32 channels heap-allocates, so the audio
/// path must skip it - and therefore the latency report must not count it.
class WideBusInstance : public juce::AudioPluginInstance {
  public:
    WideBusInstance()
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::discreteChannels(32), true)
                                        .withOutput("Out", juce::AudioChannelSet::discreteChannels(32), true))
    {
        setLatencySamples(100);
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "WideBus";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x5742;
    }
    const juce::String getName() const override { return "WideBus"; }
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet().size() == 32 && layout.getMainOutputChannelSet().size() == 32;
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override { processed.fetch_add(1); }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::atomic<int> processed{0};
};

/// Emits NaN from its process call - a misbehaving third-party plugin.
class NanInstance : public juce::AudioPluginInstance {
  public:
    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "NaN";
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x4e61;
    }
    const juce::String getName() const override { return "NaN"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (!emitNan.load()) return;
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.setSample(c, buffer.getNumSamples() / 2, std::numeric_limits<float>::quiet_NaN());
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    std::atomic<bool> emitNan{true};
};

/// Passthrough with a configurable name/manufacturer and a real editor that
/// logs its destruction, so tests can check that the editor goes before the
/// instance (PLG-06) and that the Copilot refuses to host itself (PLG-24).
class EditorLogInstance : public juce::AudioPluginInstance {
  public:
    struct Log {
        std::vector<std::string> events;
    };

    EditorLogInstance(Log& log, juce::String name, juce::String manufacturer = "Test")
        : log_(log), name_(std::move(name)), manufacturer_(std::move(manufacturer))
    {}
    ~EditorLogInstance() override { log_.events.push_back("instance"); }

    class Editor final : public juce::AudioProcessorEditor {
      public:
        Editor(EditorLogInstance& owner) : juce::AudioProcessorEditor(owner), owner_(owner) { setSize(200, 100); }
        ~Editor() override { owner_.log_.events.push_back("editor"); }
        void paint(juce::Graphics&) override {}

      private:
        EditorLogInstance& owner_;
    };

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = name_;
        d.manufacturerName = manufacturer_;
        d.pluginFormatName = "Internal";
        d.uniqueId = 0x4564;
    }
    const juce::String getName() const override { return name_; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return new Editor(*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

  private:
    Log& log_;
    juce::String name_, manufacturer_;
};

} // namespace khensu::test
