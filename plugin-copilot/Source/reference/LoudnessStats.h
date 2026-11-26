// plugin-copilot/Source/reference/LoudnessStats.h
// ENH-10: one loudness / dynamics measure for both sides of the reference
// comparison - the host signal (fed on the audio thread) and the reference
// file (fed offline on a worker). Same definition on both sides, so a delta
// between them is a delta in the audio, not in the meters.
//
//  - K-weighting: analysis::kWeightingCoefficients (the shared BS.1770 filter).
//  - 100 ms blocks. Momentary (400 ms) loudness of every block goes into a
//    histogram (0.1 LU bins, -70 .. +5 LUFS); integrated loudness is the
//    BS.1770 two-gate mean read back from it (-70 LUFS absolute, -10 LU
//    relative), to within the 0.1 LU bin width.
//  - Short-term (3 s) loudness of every block goes into a second histogram;
//    loudness range is EBU Tech 3342: absolute -70, relative -20, then the
//    10th to 95th percentile.
//  - Crest factor: sample peak over RMS of the blocks above -70 LUFS (silence
//    between takes does not lower the RMS).
//
// The audio thread only writes fixed arrays and relaxed atomics: no locks, no
// allocation. result() may run on any thread while process() runs; it then
// reads a block or two out of step, which a meter does not care about.
#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>

namespace khensu::copilot::reference {

class LoudnessStats {
  public:
    static constexpr float kFloorLufs = -70.0f;
    static constexpr float kTopLufs = 5.0f;
    static constexpr float kBinLu = 0.1f;
    static constexpr int kBins = 750; ///< (kTopLufs - kFloorLufs) / kBinLu
    static constexpr int kShortTermBlocks = 30; ///< 3 s of 100 ms blocks
    static constexpr int kMomentaryBlocks = 4;  ///< 400 ms

    struct Result {
        float integratedLufs = -96.0f; ///< -96 until a block passes the gates
        float shortTermLufs = -96.0f;  ///< the latest 3 s
        float loudnessRangeLu = 0.0f;
        float crestDb = 0.0f;          ///< sample peak over gated RMS
        float peakDb = -96.0f;
        bool valid = false;            ///< at least one block over the absolute gate
    };

    /// Prepared for 48 kHz and zeroed (the atomics' value-initialisation is
    /// not something to lean on in C++17).
    LoudnessStats() noexcept;

    /// Not concurrent with process(). Sets the filters for `sampleRate`.
    void prepare(double sampleRate) noexcept;
    /// Clears every histogram and running value. Audio-thread safe; from any
    /// other thread use requestReset() instead.
    void reset() noexcept;
    /// Any thread: the next process() call starts from zero.
    void requestReset() noexcept { resetRequested_.store(true, std::memory_order_release); }

    /// Audio thread (or the offline analysis). Up to two channels are measured.
    void process(const float* const* channels, int numChannels, int numSamples) noexcept;

    Result result() const noexcept;
    /// The gated integrated loudness, refreshed every 100 ms block (-96 when none).
    float integratedLufs() const noexcept { return integratedLufs_.load(std::memory_order_relaxed); }
    /// The short-term loudness of the newest complete block (-96 when none).
    float shortTermLufs() const noexcept { return shortTermLufs_.load(std::memory_order_relaxed); }

  private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void finishBlock() noexcept;
    static int binOf(double lufs) noexcept;
    double integratedFromHistogram() const noexcept;
    /// Mean-square power at each bin's centre loudness (filled once).
    std::array<double, kBins> binPower_{};

    double sampleRate_ = 48000.0;
    int blockLength_ = 4800;
    std::array<Biquad, 2> pre_{}, rlb_{};
    std::array<Biquad, 2> preTemplate_{}, rlbTemplate_{};

    // Audio-thread running state.
    int blockFill_ = 0;
    double blockPower_ = 0.0; ///< K-weighted sum over the block (channels summed)
    double blockRawSq_ = 0.0; ///< un-weighted sum of squares (crest)
    int blockRawCount_ = 0;
    std::array<double, kShortTermBlocks> recent_{}; ///< mean-square power of the latest blocks
    int recentPos_ = 0;
    int recentCount_ = 0;
    std::atomic<bool> resetRequested_{false};

    // Shared with readers (relaxed atomics, one writer).
    std::array<std::atomic<juce::uint32>, kBins> momentaryHist_{};
    std::array<std::atomic<juce::uint32>, kBins> shortTermHist_{};
    std::atomic<double> gatedRawSq_{0.0};
    std::atomic<double> gatedRawCount_{0.0};
    std::atomic<float> peak_{0.0f};
    std::atomic<float> shortTermLufs_{-96.0f};
    std::atomic<float> integratedLufs_{-96.0f};
};

} // namespace khensu::copilot::reference
