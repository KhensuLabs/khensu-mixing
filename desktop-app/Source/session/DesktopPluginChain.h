// desktop-app/Source/session/DesktopPluginChain.h
// Hosts third-party VST3 (and, on macOS, AU) effects/instruments inside the
// desktop app's chain builder. Owns an ordered list of plugin instances and
// renders an audio file through them offline - latency-compensated, with
// per-slot bypass - so the result can be auditioned through the transport
// A/B like every other render.
//
// Loading goes through JUCE's AudioPluginFormatManager on the message thread;
// rendering runs on a background thread while the chain is locked. Instances
// can also be injected directly (tests use fake AudioPluginInstances).
// THREAD_CONTEXT(UIThread) except renderFile(), which is BackgroundThread.
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

class DesktopPluginChain {
  public:
    struct SlotInfo {
        juce::String name;
        juce::String filePath;
        juce::String formatName;
        bool bypassed = false;
        bool isInstrument = false;
        int latencySamples = 0;
    };

    struct RenderResult {
        bool ok = false;
        juce::String error;
        juce::File output;
        int compensatedLatencySamples = 0;
    };

    static constexpr int kMaxSlots = 16;
    static constexpr double kDefaultSampleRate = 48000.0;
    static constexpr int kRenderBlock = 512;
    /// Upper bound on any one slot's self-reported latency during render.
    static constexpr double kMaxSlotLatencySec = 5.0;

    DesktopPluginChain();
    ~DesktopPluginChain();

    /// Loads the plugin at `pluginFile` (a .vst3 bundle / .component) on the
    /// message thread and appends it. `onDone(ok, errorOrName)`.
    void loadPluginAsync(const juce::File& pluginFile, std::function<void(bool, juce::String)> onDone);

    /// Appends an already-created instance. Returns false when the chain is
    /// full. The chain prepares it at render time.
    bool addInstance(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& filePath = {});

    bool remove(int index);
    bool move(int from, int to);
    bool setBypassed(int index, bool bypassed);
    void clear();

    int size() const;
    std::vector<SlotInfo> slots() const;
    /// Sum of the latencies of every active (non-bypassed) slot.
    int totalLatencySamples() const;

    /// The instance at `index` for opening its editor (message thread only).
    juce::AudioPluginInstance* instanceAt(int index) const;

    /// Renders `input` through the active slots into a stereo 32-bit float
    /// WAV at `output` (float so plugin overs above 0 dBFS survive), streamed
    /// block by block rather than holding the file in memory. Latency is
    /// compensated (the output is trimmed by the chain's reported latency and
    /// the tail flushed), so A/B stays sample-aligned. `shouldCancel` is
    /// polled between blocks; a cancelled render leaves no file.
    RenderResult renderFile(const juce::File& input, const juce::File& output, std::function<bool()> shouldCancel = {},
                            const juce::File& midiFile = {});

    /// Renders an in-memory buffer (the core of renderFile; exposed for tests).
    /// Returns the compensated latency in samples.
    /// `midi` (sample positions at `sampleRate`) drives instrument slots;
    /// effects ignore it.
    int renderBuffer(juce::AudioBuffer<float>& audio, double sampleRate, std::function<bool()> shouldCancel = {},
                     const juce::MidiMessageSequence* midi = nullptr);

    /// The ordered slot list (paths, bypass, per-plugin state as base64) for
    /// session persistence. Plugin internals come from getStateInformation.
    juce::ValueTree toValueTree() const;

    /// The (path, bypassed) pairs of a saved slot list, in order.
    static std::vector<std::pair<juce::String, bool>> savedSlotPaths(const juce::ValueTree& tree);

    /// Replaces the chain with a saved one: re-loads each plugin path in
    /// order, restores its state and bypass, then calls onDone(loadedCount).
    void restoreAsync(const juce::ValueTree& tree, std::function<void(int)> onDone);

  private:
    /// Fills dest[0, n) (2 channels) with source samples [start, start + n).
    using BlockReader = std::function<void(juce::AudioBuffer<float>& dest, juce::int64 start, int n)>;
    /// Appends src[offset, offset + n) (2 channels) to the output; false = failed.
    using BlockWriter = std::function<bool(const juce::AudioBuffer<float>& src, int offset, int n)>;
    /// The streaming core shared by renderBuffer / renderFile. Returns the
    /// compensated latency; `writeFailed` reports a writer error.
    int renderStream(juce::int64 numSamples, double sampleRate, const BlockReader& read, const BlockWriter& write,
                     std::function<bool()> shouldCancel, const juce::MidiMessageSequence* midi, bool& writeFailed);

    struct Slot {
        // shared so an offline render can keep its instances alive WITHOUT
        // holding lock_ for the whole render (that froze every UI read of the
        // chain - size(), slots(), the editor - for the render's duration).
        std::shared_ptr<juce::AudioPluginInstance> instance;
        juce::String filePath;
        bool bypassed = false;
    };

    mutable juce::CriticalSection lock_;
    /// Serialises renders (they reconfigure the shared instances); never
    /// taken by the UI-facing accessors.
    juce::CriticalSection renderLock_;
    std::vector<Slot> slots_;
    std::unique_ptr<juce::AudioPluginFormatManager> formatManager_;
    /// Async load callbacks hold a weak_ptr to this and bail once it expires.
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DesktopPluginChain)
};

} // namespace khensu
