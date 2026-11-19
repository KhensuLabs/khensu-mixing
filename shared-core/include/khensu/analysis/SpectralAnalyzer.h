#pragma once
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/ChannelLayout.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

namespace khensu {

/// Spectral analysis with dual paths:
///  1. Biquad octave bands (10 bands, real-time path): used for balance_low/mid/high
///     from the biquad energy sums.
///  2. 4096-point Radix-2 DIT FFT (high-resolution path): accumulates sample blocks
///     and computes an averaged magnitude spectrum, then bins it into the same 10
///     octave bands for rms_per_octave_db. The FFT bands offer ~10.8 Hz resolution
///     at 44.1 kHz (vs the 1-octave biquad width), eliminating mid-range confusion
///     between boxiness (~250-400 Hz) and low-mid mud (~100-250 Hz).
///
/// Cross-language definitions (one definition per metric, shared with the
/// Python service so both sides score against the same genre targets):
///  - centroid_hz is librosa's spectral centroid: per FFT frame the
///    MAGNITUDE-weighted mean bin frequency, averaged over frames (silent
///    frames count as 0, as in librosa). AUDIT-054: the old power-weighted
///    mean of the 10 biquad band centres read pink noise at ~3 kHz vs
///    librosa's ~7 kHz.
///  - tilt_db_per_octave is the least-squares slope of octave-band POWER
///    (sum of bin powers in [fc/sqrt2, fc*sqrt2), capped at 20 kHz and
///    rescaled by the covered fraction of the octave) against log2(fc) -
///    dsp_metrics.spectral_tilt_db_per_octave. Pink noise ~0, white ~+3.
///    AUDIT-009: the old per-bin-density slope read 3.01 dB/oct lower.
///  - rms_per_octave_db stays the mean per-bin power of each octave (its
///    display/consumer units are unchanged); files shorter than one FFT frame
///    are measured from one zero-padded frame in the SAME units (AUDIT-088 -
///    they used to fall back to biquad band power, a different scale).
///
/// THREAD_CONTEXT(WorkerThread)
class SpectralAnalyzer {
  public:
    static constexpr int kNumBands = 10;
    static constexpr int kMaxChannels = 8;
    static constexpr int kFftSize = 4096; // Radix-2, ~10.8 Hz resolution @ 44.1kHz

    /// Layout-aware reset. LFE channels are excluded from the analysed mono
    /// mix so an effects channel's sub energy cannot skew centroid/balance/tilt.
    void reset(double sampleRate, const ChannelLayout& layout);

    /// Backward-compatible overload; resolves the layout from the count.
    void reset(double sampleRate, int numChannels);

    void process(const float** channelData, int numChannels, int numSamples);
    AnalysisResult::SpectralData getResult() const;

    /// Select the inner-loop kernel. The x86 SSE fast path is only compiled on
    /// x86 targets; on arm64/other architectures the scalar path always runs and
    /// this flag has no effect. Primarily a test seam for scalar-vs-SIMD parity.
    void setUseSimd(bool use) noexcept { m_useSimd = use; }

  private:
    // ---- Biquad path (centroid + balance) ----
    struct Biquad {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process(double x) noexcept
        {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0.0; }
    };

    static void makeBandPassFilter(double fs, double fc, Biquad& bq);

    double m_sampleRate = 48000.0;
    int m_numChannels = 2;
    long long m_sampleCount = 0;
    bool m_useSimd = true; // ignored on non-x86 builds (scalar path only)

    /// Channel indices contributing to the analysed mono mix (LFE excluded).
    std::array<int, kMaxChannels> m_mixChannels{};
    int m_numMixChannels = 0;

    /// One band-pass per band over the mono mix (the old per-channel array
    /// built kMaxChannels filters per band and only ever ran channel 0's).
    std::array<Biquad, kNumBands> m_bp;
    std::array<double, kNumBands> m_bandSumSq{};

    static constexpr double kCenterHz[kNumBands] = {31.25,  62.5,   125.0,  250.0,  500.0,
                                                    1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

    // ---- FFT path (high-resolution band RMS) ----
    /// Hann window coefficients pre-computed in reset()
    std::array<float, kFftSize> m_window{};

    /// Accumulated mono sample ring for the current FFT frame (Mid)
    std::vector<float> m_fftBuf;
    /// Accumulated difference sample ring for the current FFT frame (Side)
    std::vector<float> m_fftBufSide;
    int m_fftWritePos = 0;

    /// Averaged magnitude spectrum (kFftSize/2 bins), accumulated across all frames (Mid)
    std::array<double, kFftSize / 2> m_magSumSq{};
    /// Averaged magnitude spectrum (kFftSize/2 bins), accumulated across all frames (Side)
    std::array<double, kFftSize / 2> m_magSumSqSide{};
    int m_fftFrameCount = 0;

    /// Hop size for overlapping FFT frames (50% overlap)
    static constexpr int kHopSize = kFftSize / 2;

    std::unique_ptr<juce::dsp::FFT> m_fft;
    std::array<float, 2 * kFftSize> m_fftWorkBuf{};

    /// Running sum of per-frame (librosa-style) centroids over the Mid frames.
    double m_centroidSum = 0.0;

    /// Window `frame` (kFftSize samples) with `window`, transform it in `work`
    /// (2*kFftSize floats) and add the one-sided |X|^2 into `target`. Returns
    /// the frame's magnitude-weighted centroid in bin units (0 when silent).
    double accumulateFrame(const float* frame, const float* window, float* work,
                           std::array<double, kFftSize / 2>& target) const;

    /// Dispatch a mono frame of kFftSize samples through the FFT and accumulate
    void processFFTFrame(const float* frame, std::array<double, kFftSize / 2>& target, bool isMid);
};

} // namespace khensu
