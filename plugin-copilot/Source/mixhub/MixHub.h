// plugin-copilot/Source/mixhub/MixHub.h
// Shared hub between Khensu Copilot instances (the "Mix" tab).
//
// Every processor owns a MixHubTap. On the audio thread the tap only copies a
// mono downmix into a preallocated single-producer FIFO and stores three
// atomics (no locks, no allocation). One process-wide MixHub
// (juce::SharedResourcePointer) runs a low-priority publisher thread that,
// every 100 ms, drains each tap, runs an FFT on the newest window, folds it
// into the shared band grid (MixHubAnalysis.h), smooths it, and stores the
// result as that instance's TrackFrame. Editors read snapshot() on the message
// thread and compute masking / the summed view from it.
//
// Scope: instances in THIS PROCESS only. Hosts that sandbox or bridge plugins
// into separate processes (Bitwig's per-plugin sandboxing, FL Studio's
// bridged mode, some Reaper "run as separate process" settings) give each
// instance its own process, so each one sees only itself. The Mix tab says so.
#pragma once

#include "MixHubAnalysis.h"

#include <JuceHeader.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace khensu::mixhub {

class MixHub;

/// One processor's connection to the hub.
class MixHubTap {
  public:
    MixHubTap();
    ~MixHubTap(); ///< unregisters; returns only once the publisher no longer uses this tap

    /// Message thread / host prepare (never concurrently with pushBlock):
    /// sizes the FIFO and the analysis buffers for this sample rate.
    void prepare(double sampleRate);

    /// AUDIO THREAD. Lock- and allocation-free: copies the mono downmix into
    /// the FIFO (dropping what does not fit) and stores the meter values.
    void pushBlock(const juce::AudioBuffer<float>& buffer, float shortTermLufs, float peakDb, float width) noexcept;

    /// Message thread (updateTrackProperties). Takes a non-real-time lock.
    void setTrackName(const juce::String& name);
    juce::String getTrackName() const;

    /// User-chosen role (PLG-18). Empty = guess from the name / spectrum.
    /// Message thread; persisted in the processor state.
    void setRoleOverride(const juce::String& role);
    juce::String getRoleOverride() const;

    juce::uint64 getInstanceId() const noexcept { return instanceId_; }
    MixHub& getHub() noexcept { return *hub_; }

    /// Publisher side (hub thread or tests, under the hub's analysis lock):
    /// drains the FIFO and, when new audio arrived, fills `out` and returns true.
    bool analyse(double nowMs, TrackFrame& out);

  private:
    juce::SharedResourcePointer<MixHub> hub_; // first member: released last
    juce::uint64 instanceId_ = 0;

    // Audio-thread FIFO (single producer: pushBlock; single consumer: analyse).
    juce::AbstractFifo fifo_{1};
    std::vector<float> fifoData_;
    std::atomic<float> shortTermLufs_{-96.0f}, peakDb_{-96.0f}, width_{0.0f};
    std::atomic<bool> prepared_{false};

    // Publisher-only analysis state (guarded by the hub's analysis lock).
    double sampleRate_ = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft_;
    std::vector<float> history_;  ///< newest fftSize samples, oldest first
    std::vector<float> window_;
    std::vector<float> fftBuffer_;
    std::vector<float> binPower_;
    std::vector<float> scratch_;
    std::array<double, kNumBands> smoothedPower_{};
    double lastAnalysisMs_ = 0.0;
    bool haveSmoothed_ = false;

    mutable juce::CriticalSection nameLock_;
    juce::String trackName_;
    juce::String roleOverride_;

    friend class MixHub;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixHubTap)
};

/// ENH-10 (4): "Unmask" between two instances. The instance that shows the
/// masking pair posts a request; the other instance (the target) picks it up
/// on its own message thread, writes the cut into its built-in EQ, and posts
/// back what it did.
struct UnmaskRequest {
    juce::uint64 source = 0, target = 0;
    bool remove = false; ///< take the source's cut out again
    UnmaskCut cut;
    juce::String sourceName; ///< for the target's undo step
};
struct UnmaskStatus {
    bool applied = false;
    int band = 0; ///< EQ band 1..3 on the target that holds the cut
    UnmaskCut cut;
    juce::String note; ///< what happened, or why not
};

/// The process-wide registry. Normally reached through SharedResourcePointer
/// (one per process); tests may also construct their own to feed frames.
class MixHub final : private juce::Thread {
  public:
    static constexpr double kDefaultExpiryMs = 3000.0;
    static constexpr int kPublishIntervalMs = 100;
    static constexpr int kStopTimeoutMs = 5000; ///< one publish pass is milliseconds; this is a backstop

    MixHub();
    ~MixHub() override;

    /// Called by MixHubTap. The first registration starts the publisher.
    juce::uint64 registerTap(MixHubTap& tap);
    /// Waits for an in-flight publish to finish with `tap`; the last one stops the publisher.
    void unregisterTap(MixHubTap& tap);

    /// Analyses every registered tap and stores the frames of those that had
    /// new audio. The publisher thread calls this; tests may call it directly.
    void publishAll(double nowMs);

    /// Stores (or replaces) one instance's frame.
    void publish(const TrackFrame& frame);

    /// Frames updated within the expiry window, ordered by instance id. An
    /// instance whose audio stopped (track bypassed, frozen, transport idle in
    /// a host that stops processing) drops out until audio resumes.
    std::vector<TrackFrame> snapshot(double nowMs) const;

    void setExpiryMs(double ms) noexcept { expiryMs_.store(ms); }
    /// The publisher thread analyses every kPublishIntervalMs while enabled
    /// (default). Tests pause it and drive publishAll() with their own clock.
    void setAutoPublish(bool enabled) noexcept { autoPublish_.store(enabled); }
    int getNumRegistered() const;
    /// True while the publisher thread runs (it runs whenever a tap exists).
    bool isPublisherRunning() const { return isThreadRunning(); }

    static double nowMs() noexcept { return juce::Time::getMillisecondCounterHiRes(); }

    // ---- Unmask requests between instances (message threads) -------------
    void postUnmask(const UnmaskRequest& request);
    /// The requests waiting for `target`, oldest first; they are removed.
    std::vector<UnmaskRequest> takeUnmaskRequests(juce::uint64 target);
    void setUnmaskStatus(juce::uint64 source, juce::uint64 target, const UnmaskStatus& status);
    void clearUnmaskStatus(juce::uint64 source, juce::uint64 target);
    std::optional<UnmaskStatus> unmaskStatus(juce::uint64 source, juce::uint64 target) const;
    /// True while a request from `source` to `target` waits for the target.
    bool hasPendingUnmask(juce::uint64 source, juce::uint64 target) const;

  private:
    void run() override;
    friend class MixHubTap; // prepare() takes tapsLock_ while it resizes buffers analyse() reads

    /// Registry membership AND publisher start/stop, decided under one lock
    /// (PLG-17): the "first/last tap" decision used to be made under tapsLock_
    /// and acted on under this one, so a concurrent register and unregister
    /// could leave a registered tap with a stopped publisher.
    juce::CriticalSection lifecycleLock_;
    mutable juce::CriticalSection tapsLock_; ///< registry + analysis (never taken on the audio thread)
    std::vector<MixHubTap*> taps_;
    mutable juce::CriticalSection framesLock_;
    std::map<juce::uint64, TrackFrame> frames_;
    std::atomic<juce::uint64> nextId_{1};
    std::atomic<double> expiryMs_{kDefaultExpiryMs};
    std::atomic<bool> autoPublish_{true};
    mutable juce::CriticalSection unmaskLock_;
    std::vector<UnmaskRequest> unmaskRequests_;
    std::map<std::pair<juce::uint64, juce::uint64>, UnmaskStatus> unmaskStatus_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixHub)
};

} // namespace khensu::mixhub
