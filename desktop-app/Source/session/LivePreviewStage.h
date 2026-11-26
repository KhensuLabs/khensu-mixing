// desktop-app/Source/session/LivePreviewStage.h
// STU-9: the live preview stage of PlaybackEngine's chain. Runs a native
// effect chain (dsp::RealtimeChain - the offline renderer's own stages,
// block-wise) on what the transport plays, so an edit is heard before Keep
// renders it. It never writes a file and never registers a version: Keep /
// Apply still go through renders().renderVersion() and the fix check.
//
// Lock-free hand-over: the message thread builds a whole new RealtimeChain
// per edit and publishes it through one atomic slot (`pending_`); the audio
// thread swaps it in, crossfades from the old chain over kCrossfadeMs (a new
// chain's filters start cold) and hands the old one back through a few
// retire slots (`retired_`), which the message thread empties. The audio
// thread never allocates, frees or locks; with no free slot it keeps an old
// chain parked (not processed) and does not take a new one until the
// message thread has collected.
// THREAD_CONTEXT: setChain / clear / plan on the message thread;
// getNextAudioBlock on the audio thread.
#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include <khensu/dsp/RealtimeChain.h>

namespace khensu {

class LivePreviewStage final : public juce::AudioSource, private juce::Timer {
  public:
    static constexpr double kCrossfadeMs = 30.0;
    /// The chains' internal block (independent of the device's).
    static constexpr int kChainBlock = 2048;

    explicit LivePreviewStage(juce::AudioSource& input);
    ~LivePreviewStage() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override { input_.releaseResources(); }
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

    // ---- Message thread ----
    /// Starts (or updates) the preview with `chain`; an empty chain previews
    /// nothing but keeps the stage on. Returns the plan (what runs live).
    const std::vector<dsp::RealtimeChain::StagePlan>& setChain(std::vector<dsp::ChainStageSpec> chain);
    /// Crossfades back to the plain signal and stops previewing.
    void clear();
    bool isEnabled() const noexcept { return enabled_; }
    const std::vector<dsp::RealtimeChain::StagePlan>& plan() const noexcept { return plan_; }
    /// Deletes a chain the audio thread handed back (also on a timer).
    void collectGarbage();

    /// The rate chains are built at before a device reports one.
    static constexpr double kFallbackRate = 48000.0;

  private:
    void timerCallback() override;
    void publish(dsp::RealtimeChain* chain);
    dsp::RealtimeChain* build(const std::vector<dsp::ChainStageSpec>& chain);
    /// Audio thread: retire a parked old chain, then take a pending one.
    void takePending() noexcept;
    void finishFade() noexcept;
    /// Audio thread: hands `chain` to a free retire slot; false when full.
    bool tryRetire(dsp::RealtimeChain* chain) noexcept;
    bool hasFreeRetireSlot() const noexcept;

    juce::AudioSource& input_;
    std::atomic<dsp::RealtimeChain*> pending_{nullptr};
    static constexpr int kRetireSlots = 4;
    std::array<std::atomic<dsp::RealtimeChain*>, kRetireSlots> retired_{};
    std::atomic<double> sampleRate_{0.0};

    // Audio thread only.
    dsp::RealtimeChain* active_ = nullptr; ///< null = the plain signal
    dsp::RealtimeChain* fading_ = nullptr; ///< the chain faded out of (null = the plain signal)
    bool fadeActive_ = false;
    int fadePos_ = 0, fadeSamples_ = 1440;
    juce::AudioBuffer<float> old_; ///< the input for the outgoing chain during a fade

    // Message thread only.
    bool enabled_ = false;
    std::vector<dsp::ChainStageSpec> specs_;
    std::vector<dsp::RealtimeChain::StagePlan> plan_;
    double builtRate_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LivePreviewStage)
};

} // namespace khensu
