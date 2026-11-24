#pragma once

#include <JuceHeader.h>
#include <khensu/analysis/TruePeakFilter.h>
#include <khensu/ui/StereoSampleRing.h>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <array>

namespace khensu {

class AudioRingBuffer {
  public:
    AudioRingBuffer() : fifo(1) {}

    void setSize(int channels, int capacity)
    {
        buffer.setSize(channels, capacity);
        fifo.setTotalSize(capacity);
    }

    void reset()
    {
        fifo.reset();
        buffer.clear();
    }

    void write(const juce::AudioBuffer<float>& source)
    {
        const int numSamples = source.getNumSamples();
        const int numChannels = source.getNumChannels();
        if (numSamples <= 0 || numChannels <= 0 || buffer.getNumChannels() <= 0) return;

        int start1, size1, start2, size2;
        fifo.prepareToWrite(numSamples, start1, size1, start2, size2);

        // A mono source fills every ring channel from channel 0, matching the
        // mirrored peak meters - otherwise the right ring channel kept stale
        // or silent data and readers showed a dead right meter (audit L11).
        const bool mirrorMono = numChannels == 1;
        const int channelsToWrite = mirrorMono ? buffer.getNumChannels() : std::min(numChannels, buffer.getNumChannels());
        for (int c = 0; c < channelsToWrite; ++c) {
            const int src = mirrorMono ? 0 : c;
            if (size1 > 0) buffer.copyFrom(c, start1, source.getReadPointer(src), size1);
            if (size2 > 0) buffer.copyFrom(c, start2, source.getReadPointer(src, size1), size2);
        }
        fifo.finishedWrite(size1 + size2);
    }

    int read(juce::AudioBuffer<float>& dest)
    {
        const int numSamples = dest.getNumSamples();
        const int numChannels = dest.getNumChannels();
        if (numSamples <= 0 || numChannels <= 0 || buffer.getNumChannels() <= 0) return 0;

        int start1, size1, start2, size2;
        const int available = fifo.getNumReady();
        const int numToRead = std::min(numSamples, available);
        if (numToRead <= 0) return 0;

        fifo.prepareToRead(numToRead, start1, size1, start2, size2);

        const int channelsToCopy = std::min(numChannels, buffer.getNumChannels());
        if (size1 > 0) {
            for (int c = 0; c < channelsToCopy; ++c)
                dest.copyFrom(c, 0, buffer.getReadPointer(c, start1), size1);
        }
        if (size2 > 0) {
            for (int c = 0; c < channelsToCopy; ++c)
                dest.copyFrom(c, size1, buffer.getReadPointer(c, start2), size2);
        }
        fifo.finishedRead(size1 + size2);
        return size1 + size2;
    }

    int getNumReady() const noexcept { return fifo.getNumReady(); }

  private:
    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> buffer;
};

/// Audio-thread telemetry boundary for the plugin processor.
///
/// The audio callback writes atomics and the lock-free ring buffer; the
/// transport worker and editor read snapshots without taking a lock. Host
/// position capture intentionally remains before DSP processing, while meter
/// and ring-buffer publication remain after the output stage.
///
/// Loudness follows ITU-R BS.1770 / EBU R128 (PLG-13): K-weighted energy is
/// summed into 100 ms bins (the K-weighting is shared-core's
/// khensu::analysis::kWeightingCoefficients, the same filter the desktop
/// analyzer runs - FIX-11), momentary loudness is the RECTANGULAR mean of the
/// last 4 bins (400 ms) and short-term of the last 30 (3 s), both updated
/// every 100 ms - not the one-pole averages it used before. Correlation and
/// width keep a 400 ms one-pole average (they are not BS.1770 quantities).
///
/// UI-5 additions, all lock-free and allocation-free on the audio thread:
///  - integrated loudness: every 400 ms momentary block (100 ms hop) above
///    the -70 LUFS absolute gate goes into a 0.1 LU histogram (counts and
///    energy sums), and the BS.1770 relative gate (-10 LU) is evaluated over
///    it each 100 ms - no block list grows. requestIntegratedReset() restarts it.
///  - true peak per channel with the shared 4x interpolator (TruePeakFilter.h,
///    MEAS-5), evaluated only around samples within 6 dB of the block's
///    sample peak (an inter-sample peak needs a large neighbour); the
///    highest value is accumulated until the UI takes it.
///  - a StereoSampleRing of the output for the vectorscope.
class CopilotRealtimeTelemetry {
  public:
    struct Snapshot {
        float position_sec = 0.0f;
        bool is_playing = false;
        float left_peak_db = -96.0f;
        float right_peak_db = -96.0f;
        float momentary_lufs = -96.0f;
        float short_term_lufs = -96.0f;
        float correlation = 0.0f;
        float width = 1.0f;
        float integrated_lufs = -96.0f; ///< -96 until a block passes the gates
    };

    /// The host position of the current block (CE-DAW-3 time stamps).
    struct HostTransport {
        bool timeValid = false; ///< the host reported a timeline position
        bool playing = false;
        double timeSec = 0.0;   ///< timeline position of the block's first sample
    };

    void prepareToPlay(double sampleRate, int channels);
    /// THREAD_CONTEXT(AudioThread). Publishes position/playing/BPM for the
    /// meters and returns them at full precision for the block. Falls back to
    /// getTimeInSamples() / sampleRate when the host gives no seconds.
    HostTransport captureHostTransport(juce::AudioProcessor& processor);
    void process(const juce::AudioBuffer<float>& buffer);

    Snapshot snapshot() const noexcept;
    float getBpm() const noexcept { return bpm_.load(std::memory_order_relaxed); }
    /// The host's loop region in beats (0 = none reported) and whether it loops (FEAT-7).
    double getLoopBeats() const noexcept { return loopBeats_.load(std::memory_order_relaxed); }
    bool isLooping() const noexcept { return looping_.load(std::memory_order_relaxed); }

    int readRingBuffer(juce::AudioBuffer<float>& dest) { return ringBuffer_.read(dest); }

    /// UI thread: the highest true peak (dBTP) per channel since the previous
    /// call (or prepare), then starts a new interval. -240 when silent.
    void takeTruePeaks(float& leftDbtp, float& rightDbtp) noexcept;
    /// Any thread: the integrated loudness restarts with the next block.
    void requestIntegratedReset() noexcept { resetIntegrated_.store(true, std::memory_order_release); }
    /// The output for the vectorscope (audio thread produces, UI consumes).
    ui::StereoSampleRing& scopeRing() noexcept { return scopeRing_; }
    int getRingBufferAvailable() const noexcept { return ringBuffer_.getNumReady(); }

  private:
    /// One K-weighting stage. Double precision, like the desktop analyzer's:
    /// in float the RLB high-pass (poles just inside the unit circle) rounds
    /// differently and the two meters drift apart.
    struct KWeightBiquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

        void reset() noexcept { z1 = z2 = 0.0; }

        /// Loads {b0, b1, b2, a1, a2} and clears the state.
        void set(const std::array<double, 5>& c) noexcept
        {
            b0 = c[0];
            b1 = c[1];
            b2 = c[2];
            a1 = c[3];
            a2 = c[4];
            reset();
        }
    };

    AudioRingBuffer ringBuffer_;
    ui::StereoSampleRing scopeRing_{8192};

    /// Streaming 4x true-peak interpolator for one channel.
    struct TruePeakDetector {
        static constexpr int kWindow = analysis::kTruePeakPhaseTaps; // 65
        std::array<float, 256> history{};
        int pos = 0;
        void reset() noexcept
        {
            history.fill(0.0f);
            pos = 0;
        }
        /// Pushes `x`; returns the largest |interpolated| value around the
        /// window's centre when that centre is at least `threshold`, else 0.
        float push(float x, float threshold, const float* coeffs) noexcept;
    };
    TruePeakDetector truePeak_[2];
    /// TruePeakPhases as float, phase-major (4 x 65), filled in prepareToPlay.
    std::array<float, analysis::kTruePeakOversample * analysis::kTruePeakPhaseTaps> truePeakCoeffs_{};
    std::atomic<float> truePeakAccum_[2]{}; ///< linear, highest since the last take
    float lastBlockPeak_[2] = {0.0f, 0.0f};
    static void accumulateMax(std::atomic<float>& target, float value) noexcept;

    // Integrated loudness: gated-block histogram (see the class comment).
    static constexpr int kGateBins = 800; ///< -70 .. +10 LUFS in 0.1 LU
    std::array<std::uint32_t, kGateBins> gateCount_{};
    std::array<double, kGateBins> gateEnergy_{};
    int binsSinceReset_ = 0;
    std::atomic<float> integratedLufs_{-96.0f};
    std::atomic<bool> resetIntegrated_{false};
    void resetIntegrated() noexcept;
    void addGatedBlock(double meanPower) noexcept;
    double currentSampleRate_ = 44100.0;
    KWeightBiquad kPre_[2], kRlb_[2];

    std::atomic<float> leftPeakDb_{-96.0f};
    std::atomic<float> rightPeakDb_{-96.0f};
    std::atomic<float> positionSec_{0.0f};
    std::atomic<bool> isPlaying_{false};
    std::atomic<float> momentaryLufs_{-96.0f};
    std::atomic<float> shortTermLufs_{-96.0f};
    std::atomic<float> correlation_{0.0f};
    std::atomic<float> stereoWidth_{1.0f};
    std::atomic<float> bpm_{120.0f};
    std::atomic<double> loopBeats_{0.0};
    std::atomic<bool> looping_{false};

    static constexpr int kMomentaryBins = 4;  ///< 400 ms
    static constexpr int kShortTermBins = 30; ///< 3 s
    void resetLoudnessWindows() noexcept;
    void publishLoudness() noexcept;
    int binSamples_ = 4410;   ///< samples per 100 ms bin
    int binFill_ = 0;         ///< samples summed into the open bin
    double binEnergy_ = 0.0;  ///< K-weighted energy of the open bin
    std::array<double, kShortTermBins> bins_{}; ///< completed bins, ring
    int binPos_ = 0;          ///< next ring slot to write

    double emaLR_ = 0.0, emaLL_ = 0.0, emaRR_ = 0.0;
    double emaMid_ = 0.0, emaSide_ = 0.0;
};

} // namespace khensu
