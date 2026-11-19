#pragma once

#include <JuceHeader.h>
#include "GrowableDelayLine.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

/// Enables a hosted instance's main buses at `channels` (1 or 2) and
/// disables its auxiliary (sidechain) buses, which the chain has no audio
/// for. Returns true when the instance's layout actually changed.
bool configureHostedLayout(juce::AudioPluginInstance& instance, int channels);

/// Owns the immutable hosted-plugin snapshots and their control/audio-thread
/// handoff. The processor only needs to prepare it, process one block, and
/// consume the latency of the path that actually ran.
class HostedPluginChainManager final {
  public:
    /// Portable data captured in the parent VST3 state. `pluginState` is
    /// opaque to Khensu and is passed back verbatim to the hosted plugin after
    /// it has been recreated and prepared on the message thread.
    struct PersistedSlot {
        juce::String name;
        juce::String filePath;
        bool bypassed = false;
        float wetMix = 1.0f;
        juce::MemoryBlock pluginState;
    };

    struct Slot {
        /// Unique per manager - lets the UI tell two copies of one plugin apart.
        uint64_t id = 0;
        juce::String name;
        juce::String filePath;
        /// Null for a placeholder: a saved slot whose plugin could not be
        /// restored (missing VST3, load failure). The audio thread skips it;
        /// its saved state is persisted unchanged so nothing is lost.
        std::shared_ptr<juce::AudioPluginInstance> instance;
        juce::MemoryBlock placeholderState;
        std::atomic<bool> bypassed{false};
        std::atomic<float> wetMix{1.0f};
        /// Audio-thread ramp toward `wetMix`, or toward 0 while bypassed
        /// (AUDIT-061: no per-block zipper; FIX-14: bypass crossfades).
        juce::LinearSmoothedValue<float> wetSmoothed{1.0f};

        // The audio thread uses this preallocated line to align the dry side
        // of the slot with the hosted instance's reported latency. It is fed
        // on every block (whatever the wet amount, bypassed or not), so moving
        // the wet control never replays stale history. Sized at prepare for
        // the instance's latency (at least one second); a plugin that raises
        // its latency later gets a bigger line from reserveDryLines()
        // (FIX-14), up to kMaxCompensatedLatencySeconds. See
        // compensatedLatency().
        copilot::GrowableDelayLine dryCompDelay;

        /// Listener registered on `instance` (latency changes); removed in
        /// the destructor so a retired instance never calls back.
        juce::AudioProcessorListener* listener = nullptr;

        // A plugin that keeps more channels than the host buffer carries (a
        // sidechain bus it refuses to disable) is processed through this
        // preallocated, wider buffer instead of reading past the host's
        // channel table. Empty when the plugin fits the host buffer.
        juce::AudioBuffer<float> wideScratch;

        // A mono-only plugin on a stereo track runs on this preallocated
        // one-channel buffer: the two channels are summed into it and the
        // result is written back to both (FIX-14). Handed the stereo buffer,
        // it processed the left channel and left the right dry and undelayed.
        juce::AudioBuffer<float> monoScratch;

        bool isPlaceholder() const noexcept { return instance == nullptr; }

        Slot() = default;
        ~Slot();
        JUCE_DECLARE_NON_COPYABLE(Slot)
    };

    /// Lightweight per-slot view for the editor's hosted-chain panel. Unlike
    /// captureState() it never calls the hosted plugin's state APIs, so it is
    /// cheap enough to poll from a UI timer.
    struct SlotInfo {
        uint64_t id = 0;
        juce::String name;
        juce::String filePath;
        bool bypassed = false;
        float wetMix = 1.0f;
        int latencySamples = 0;
        bool missing = false; ///< placeholder for a plugin that could not be restored

        bool operator==(const SlotInfo& o) const noexcept
        {
            return id == o.id && name == o.name && filePath == o.filePath && bypassed == o.bypassed
                   && std::abs(wetMix - o.wetMix) < 1.0e-4f && latencySamples == o.latencySamples
                   && missing == o.missing;
        }
        bool operator!=(const SlotInfo& o) const noexcept { return !(*this == o); }
    };

    HostedPluginChainManager();
    ~HostedPluginChainManager();

    /// THREAD_CONTEXT(UIThread). Configures every hosted instance's buses for
    /// `preparedChannels`, then prepares it at the host's rate/block size.
    void prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels, bool nonRealtime = false);
    /// Releases every hosted instance's resources (host stopped playback).
    void releaseResources();
    /// Records the host's offline-render flag. Lock-free and allocation-free
    /// (AUDIT-004): the JUCE VST3 wrapper calls this from the audio callback
    /// on every block. processBlock() forwards it to the hosted instances it
    /// is about to run, and instances added later inherit it.
    void setNonRealtime(bool nonRealtime) noexcept;

    /// Called (from whatever thread the hosted plugin reports on - possibly
    /// the audio thread) when a hosted instance's latency changes. Must be
    /// real-time safe; the processor triggers an AsyncUpdater. Set once,
    /// before any instance is added.
    void setLatencyChangedCallback(std::function<void()> callback);

    /// THREAD_CONTEXT(UIThread). The longest tail among the slots that carry
    /// signal (a bypassed slot plays its dry signal, so its tail is not heard),
    /// in seconds; `kInfiniteTailFallbackSeconds` when any of them
    /// reports an infinite (or absurdly long) tail.
    double getTailLengthSeconds() const;
    static constexpr double kInfiniteTailFallbackSeconds = 30.0;
    /// Longest per-slot latency the dry line compensates. A slot reporting
    /// more is compensated, and reported, at this much.
    static constexpr double kMaxCompensatedLatencySeconds = 30.0;

    /// Processes the hosted chain and advances the retirement epoch. Returns
    /// the latency of the slots that ran this block. A bypassed slot still
    /// runs and counts: it crossfades to its dry signal, delayed by the same
    /// latency, so bypass never moves the audio or the host's compensation
    /// (FIX-14).
    /// Host blocks larger than the prepared block size are split, so hosted
    /// instances never see more than they were prepared for. `playHead` is
    /// forwarded to the hosted instances.
    int processBlock(juce::AudioBuffer<float>& buffer, bool bypassed, juce::AudioPlayHead* playHead = nullptr);

    /// THREAD_CONTEXT(UIThread). Configures the instance's bus layout, then
    /// prepares it at the CURRENT prepared rate/block size (an async load may
    /// have been created at a stale rate), then restores `pluginState`.
    void addPluginInstance(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& name,
                           const juce::String& filePath = {}, bool bypassed = false, float wetMix = 1.0f,
                           const juce::MemoryBlock* pluginState = nullptr);
    /// THREAD_CONTEXT(UIThread). Appends a placeholder for a saved slot whose
    /// plugin could not be restored, keeping its position, controls and state.
    void addPlaceholder(const PersistedSlot& saved);
    void removePlugin(int slotIndex);
    /// THREAD_CONTEXT(UIThread). Moves slot `fromIndex` to `toIndex` (PLG-06)
    /// with the same copy-and-publish snapshot handoff as add/remove: the
    /// audio thread switches to the reordered chain at a block boundary, and
    /// the slots (instances, delay lines, smoothers) move with their state.
    /// Returns false for an out-of-range index or a no-op move.
    bool moveSlot(int fromIndex, int toIndex);
    void clear();
    void setBypass(int slotIndex, bool bypassed);
    void setWetMix(int slotIndex, float wetMix);

    /// THREAD_CONTEXT(UIThread). Grows each active slot's dry line to its
    /// plugin's current latency (capped at kMaxCompensatedLatencySeconds),
    /// so a plugin that raised its latency after insert is compensated in
    /// full without waiting for the host to re-prepare (FIX-14). Allocates
    /// off the audio thread; also frees lines the audio thread replaced.
    void reserveDryLines();

    int getNumPlugins() const noexcept;
    /// Sum of the latencies of every slot processBlock() actually runs: not a
    /// placeholder, and able to run at the prepared channel count (the same
    /// fit test the audio path uses). Neither the wet amount nor bypass
    /// matters - the dry side is always delayed to match (AUDIT-061, FIX-14).
    int getActiveLatency() const noexcept;

    /// THREAD_CONTEXT(UIThread). Capture a stable control-thread snapshot;
    /// hosted state APIs are deliberately never called by processBlock().
    /// Placeholders are included with their saved state.
    std::vector<PersistedSlot> captureState() const;

    /// THREAD_CONTEXT(UIThread). Snapshot of the active chain's slots in
    /// processing order (index == the slotIndex the mutators take).
    std::vector<SlotInfo> getSlotInfo() const;

    /// THREAD_CONTEXT(UIThread). The live instance of the slot with `slotId`
    /// (null for a placeholder or an unknown id). Used to open its editor; the
    /// returned owner keeps the instance alive while its window exists.
    std::shared_ptr<juce::AudioPluginInstance> getInstanceForSlotId(uint64_t slotId) const;

    /// Runs the message-thread retirement policy. Destruction of retired
    /// third-party instances happens after the lock is released.
    void reap();

    /// Retired snapshots still waiting for the reaper (bounded by design).
    size_t getNumRetiredChains() const;

    int getPreparedBlockSize() const noexcept;
    double getPreparedSampleRate() const noexcept;

  private:
    using Chain = std::vector<std::shared_ptr<Slot>>;

    struct RetiredChain {
        std::shared_ptr<Chain> chain;
        uint64_t releaseAfterEpoch = 0;
        uint32_t retiredAtTick = 0;
    };

    void pruneCompletedRetirements(uint64_t completedEpoch);
    void publishReplacement(std::shared_ptr<Chain> replacement, uint64_t completedEpoch);
    void appendSlot(std::shared_ptr<Slot> slot);
    void prepareSlotLocked(Slot& slot);
    void notifyLatencyChanged();

    class LatencyListener;
    std::unique_ptr<LatencyListener> latencyListener_;
    std::function<void()> latencyChanged_;

    double sampleRate_ = 44100.0;
    int blockSize_ = 512;
    int preparedChannels_ = 2;
    std::atomic<bool> nonRealtime_{false};
    juce::AudioBuffer<float> partialWetDry_;
    /// One MIDI buffer reused per slot (cleared before each): no slot's MIDI
    /// output leaks into the next. clear() keeps the capacity, and the
    /// reserve is sized well above any realistic per-block hosted MIDI output
    /// so the audio thread never grows it (AUDIT-085).
    juce::MidiBuffer midi_;
    static constexpr size_t kMidiReserveBytes = 64 * 1024;

    // The audio callback only acquires a raw immutable snapshot pointer. It
    // never increments a shared_ptr reference count, whose implementation may
    // take an internal lock. Control-thread publication retains the old owner
    // until readers have left the callback; see processBlock() and reap().
    std::shared_ptr<Chain> activeChainOwner_{std::make_shared<Chain>()};
    std::atomic<Chain*> activeChain_{activeChainOwner_.get()};
    std::vector<RetiredChain> retiredChains_;
    std::atomic<uint32_t> audioReaders_{0};
    std::atomic<uint64_t> epoch_{0};
    mutable juce::CriticalSection mutex_;
    uint64_t nextSlotId_ = 1;

    static constexpr size_t kMaxRetiredChains = 8;
    uint32_t reaperTick_ = 0;
    uint64_t reaperLastSeenEpoch_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HostedPluginChainManager)
};

} // namespace khensu
