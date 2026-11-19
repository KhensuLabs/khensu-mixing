// Dual-path spectral analysis: biquad bands (balance) + 4096-pt FFT (rms per band, centroid, tilt).
// THREAD_CONTEXT(WorkerThread)
#include <khensu/analysis/SpectralAnalyzer.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

// x86-only SSE fast path. arm64/other targets (e.g. the macOS universal build's
// arm64 slice) compile the portable scalar path instead — <immintrin.h> and the
// SSE intrinsics below do not exist there. Keep the two paths bit-parity so the
// analyzer's numbers are identical regardless of architecture (see the
// scalar-vs-SIMD parity test).
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define KHENSU_SPECTRAL_X86_SSE 1
#include <immintrin.h>
#else
#define KHENSU_SPECTRAL_X86_SSE 0
#endif

static constexpr double kPi = 3.14159265358979323846;

namespace khensu {

// ---------------------------------------------------------------------------
// Biquad band-pass: 2nd-order, 1-octave bandwidth (Q = sqrt(2))
// ---------------------------------------------------------------------------

void SpectralAnalyzer::makeBandPassFilter(double fs, double fc, Biquad& bq)
{
    const double Q = 1.41421356; // sqrt(2), 1-octave bandwidth
    const double w0 = 2.0 * kPi * fc / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * Q);
    const double a0 = 1.0 + alpha;

    bq.b0 = alpha / a0;
    bq.b1 = 0.0;
    bq.b2 = -alpha / a0;
    bq.a1 = -2.0 * cosw0 / a0;
    bq.a2 = (1.0 - alpha) / a0;
    bq.reset();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SpectralAnalyzer::reset(double sampleRate, int numChannels)
{
    auto layout = ChannelLayout::fromChannelCount(numChannels);
    reset(sampleRate, layout.is_ok() ? layout.value() : ChannelLayout::unspecified(numChannels));
}

void SpectralAnalyzer::reset(double sampleRate, const ChannelLayout& layout)
{
    m_sampleRate = sampleRate;
    m_numChannels = std::min(layout.numChannels, kMaxChannels);

    // Build the contributing-channel list once: LFE carries band-limited
    // effects content, not programme material, so including it would skew the
    // centroid, band balance, and tilt toward the sub range.
    m_numMixChannels = 0;
    for (int c = 0; c < m_numChannels; ++c)
        if (!layout.isLFE(c)) m_mixChannels[static_cast<size_t>(m_numMixChannels++)] = c;
    if (m_numMixChannels == 0) { // pathological: LFE-only source
        m_mixChannels[0] = 0;
        m_numMixChannels = 1;
    }

    m_sampleCount = 0;
    m_bandSumSq.fill(0.0);

    m_fft = std::make_unique<juce::dsp::FFT>(12); // Order 12 = 4096 points

    // Build biquad band-pass filters
    for (int b = 0; b < kNumBands; ++b) {
        const double nyquist = sampleRate * 0.5;
        if (kCenterHz[b] < nyquist * 0.9)
            makeBandPassFilter(sampleRate, kCenterHz[b], m_bp[b]);
        else
            m_bp[b] = Biquad{};
    }

    // Build Hann window for FFT frames
    for (int i = 0; i < kFftSize; ++i)
        m_window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (kFftSize - 1))));

    // Initialise FFT accumulator
    m_fftBuf.assign(kFftSize, 0.0f);
    m_fftBufSide.assign(kFftSize, 0.0f);
    m_fftWritePos = 0;
    m_magSumSq.fill(0.0);
    m_magSumSqSide.fill(0.0);
    m_fftFrameCount = 0;
    m_centroidSum = 0.0;
    m_fftWorkBuf.fill(0.0f);
}

double SpectralAnalyzer::accumulateFrame(const float* frame, const float* window, float* work,
                                         std::array<double, kFftSize / 2>& target) const
{
    if (!m_fft) return 0.0;

    // ---- Apply the window to the first half of the work buffer ----
#if KHENSU_SPECTRAL_X86_SSE
    if (m_useSimd) {
        // SSE2 fast path: 4 floats at a time.
        for (int i = 0; i < kFftSize; i += 4) {
            __m128 f = _mm_loadu_ps(&frame[i]);
            __m128 w = _mm_loadu_ps(&window[i]);
            _mm_storeu_ps(&work[i], _mm_mul_ps(f, w));
        }
    } else
#endif
    {
        // Portable scalar path (bit-identical float ops to the SSE path).
        for (int i = 0; i < kFftSize; ++i)
            work[i] = frame[i] * window[i];
    }

    // Zero out the second half of the work buffer (required for real-to-complex transform)
    std::fill(work + kFftSize, work + 2 * kFftSize, 0.0f);

    // Perform real-only forward transform in-place
    m_fft->performRealOnlyForwardTransform(work);

    // ---- Accumulate squared magnitudes (one-sided spectrum) ----
#if KHENSU_SPECTRAL_X86_SSE
    if (m_useSimd) {
        // SSE2 fast path: 2 complex numbers (re,im,re,im) at a time.
        for (int k = 0; k < kFftSize / 2; k += 2) {
            __m128 val = _mm_loadu_ps(&work[2 * k]);
            __m128 valSq = _mm_mul_ps(val, val);
            __m128 valSqSwap = _mm_shuffle_ps(valSq, valSq, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(valSq, valSqSwap);

            alignas(16) float temp[4];
            _mm_store_ps(temp, sums);

            target[k] += static_cast<double>(temp[0]);
            target[k + 1] += static_cast<double>(temp[2]);
        }
    } else
#endif
    {
        // Portable scalar path: re*re + im*im per bin, matching the SSE reduction.
        for (int k = 0; k < kFftSize / 2; ++k) {
            const float re = work[2 * k];
            const float im = work[2 * k + 1];
            const float reSq = re * re;
            const float imSq = im * im;
            target[k] += static_cast<double>(reSq + imSq);
        }
    }

    // ---- librosa spectral centroid of this frame (bin units) ----
    // Magnitude (not power) weighting over the same one-sided bins. Scalar
    // double on both kernels so SIMD/scalar parity holds exactly.
    double magSum = 0.0, weighted = 0.0;
    for (int k = 0; k < kFftSize / 2; ++k) {
        const double re = static_cast<double>(work[2 * k]);
        const double im = static_cast<double>(work[2 * k + 1]);
        const double mag = std::sqrt(re * re + im * im);
        magSum += mag;
        weighted += mag * static_cast<double>(k);
    }
    return magSum > 1e-12 ? weighted / magSum : 0.0;
}

void SpectralAnalyzer::processFFTFrame(const float* frame, std::array<double, kFftSize / 2>& target, bool isMid)
{
    const double centroidBins = accumulateFrame(frame, m_window.data(), m_fftWorkBuf.data(), target);
    if (isMid) m_centroidSum += centroidBins;
}

void SpectralAnalyzer::process(const float** channelData, int numChannels, int numSamples)
{
    const int ch = std::min(numChannels, m_numChannels);

    for (int i = 0; i < numSamples; ++i) {
        // ---- Biquad path: mono mix (LFE excluded) ----
        double mono = 0.0;
        int contributing = 0;
        for (int m = 0; m < m_numMixChannels; ++m) {
            const int c = m_mixChannels[static_cast<size_t>(m)];
            if (c >= ch) continue;
            mono += static_cast<double>(channelData[c][i]);
            ++contributing;
        }
        mono /= (contributing > 0 ? contributing : 1);

        // ---- Side calculation ----
        double left = static_cast<double>(channelData[0][i]);
        double right = (ch > 1) ? static_cast<double>(channelData[1][i]) : left;
        double side = (left - right) * 0.5;

        for (int b = 0; b < kNumBands; ++b) {
            const double y = m_bp[b].process(mono);
            m_bandSumSq[b] += y * y;
        }
        ++m_sampleCount;

        // ---- FFT path: overlap-add ring buffer ----
        m_fftBuf[m_fftWritePos] = static_cast<float>(mono);
        m_fftBufSide[m_fftWritePos] = static_cast<float>(side);
        ++m_fftWritePos;

        if (m_fftWritePos == kFftSize) {
            // Run FFT on current full frame for Mid and Side
            processFFTFrame(m_fftBuf.data(), m_magSumSq, true);
            processFFTFrame(m_fftBufSide.data(), m_magSumSqSide, false);
            ++m_fftFrameCount;

            // Shift by hop (50% overlap): move the second half to the start
            std::copy(m_fftBuf.begin() + kHopSize, m_fftBuf.end(), m_fftBuf.begin());
            std::copy(m_fftBufSide.begin() + kHopSize, m_fftBufSide.end(), m_fftBufSide.begin());
            m_fftWritePos = kHopSize;
        }
    }
}

namespace {

// dsp_metrics.OCTAVE_CENTERS_HZ / TILT_CAP_HZ - the tilt is defined once, on
// the Python side's nominal centres and cap, so both languages agree.
constexpr double kTiltCentersHz[] = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
constexpr double kTiltCapHz = 20000.0;

/// dsp_metrics.spectral_tilt_db_per_octave on an averaged one-sided power
/// spectrum: least-squares slope of octave-band POWER (dB) vs log2(fc).
/// Octaves with less than half their width below the cap are skipped and a
/// partly-covered top octave is rescaled by its covered fraction, as in
/// Python. Unlike Python, a band with no energy is skipped rather than fitted
/// at -300 dB (a band-limited file would otherwise read as a cliff).
float octaveBandPowerTilt(const double* binPower, int numBins, double binHz, double sampleRate)
{
    const double cap = std::min(sampleRate * 0.5, kTiltCapHz);
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    int n = 0;
    for (double fc : kTiltCentersHz) {
        const double lo = fc / std::sqrt(2.0);
        const double hi = std::min(fc * std::sqrt(2.0), cap);
        if (hi <= lo) continue;
        const double covered = std::log2(hi / lo); // 1.0 = whole octave usable
        if (covered < 0.5) continue;
        double energy = 0.0;
        bool anyBin = false;
        for (int k = 0; k < numBins; ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f < lo) continue;
            if (f >= hi) break;
            energy += binPower[k];
            anyBin = true;
        }
        if (!anyBin || energy <= 1e-30) continue;
        const double x = std::log2(fc);
        const double y = 10.0 * std::log10(energy / covered);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
        ++n;
    }
    if (n < 2) return 0.0f;
    const double denom = n * sumXX - sumX * sumX;
    if (std::abs(denom) <= 1e-9) return 0.0f;
    return static_cast<float>((n * sumXY - sumX * sumY) / denom);
}

} // namespace

AnalysisResult::SpectralData SpectralAnalyzer::getResult() const
{
    AnalysisResult::SpectralData result;
    result.rms_per_octave_db.resize(kNumBands, -90.0f);
    result.rms_mid_per_octave_db.resize(kNumBands, -90.0f);
    result.rms_side_per_octave_db.resize(kNumBands, -90.0f);
    result.fft_mid_magnitudes.resize(kFftSize / 2, -90.0f);
    result.fft_side_magnitudes.resize(kFftSize / 2, -90.0f);

    if (m_sampleCount == 0) return result;

    // ---- Balance from biquad path ----
    double lowPow = 0, midPow = 0, highPow = 0;
    for (int b = 0; b < kNumBands; ++b) {
        if (b < 3)
            lowPow += m_bandSumSq[b];
        else if (b < 7)
            midPow += m_bandSumSq[b];
        else
            highPow += m_bandSumSq[b];
    }
    const double totalBandPow = lowPow + midPow + highPow;
    if (totalBandPow > 1e-15) {
        result.balance_low = static_cast<float>(lowPow / totalBandPow);
        result.balance_mid = static_cast<float>(midPow / totalBandPow);
        result.balance_high = static_cast<float>(highPow / totalBandPow);
    }

    // ---- Spectrum source: the accumulated frames, or (a file shorter than
    // one frame) a single zero-padded frame measured now in the same units ----
    const std::array<double, kFftSize / 2>* mid = &m_magSumSq;
    const std::array<double, kFftSize / 2>* side = &m_magSumSqSide;
    double frames = static_cast<double>(m_fftFrameCount);
    double centroidSum = m_centroidSum;
    std::array<double, kFftSize / 2> shortMid{}, shortSide{};
    if (m_fftFrameCount == 0) {
        const int n = m_fftWritePos; // samples buffered so far (< kFftSize)
        if (n < 2) return result;
        // Hann over the n real samples, zero beyond. The per-bin
        // normalisation below assumes a full kFftSize-sample window
        // (sum w^2 = 3N/8); a length-n window has 3n/8, so scale by N/n.
        std::vector<float> win(kFftSize, 0.0f);
        for (int i = 0; i < n; ++i)
            win[static_cast<size_t>(i)] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (n - 1))));
        std::vector<float> work(2 * kFftSize, 0.0f);
        centroidSum = accumulateFrame(m_fftBuf.data(), win.data(), work.data(), shortMid);
        accumulateFrame(m_fftBufSide.data(), win.data(), work.data(), shortSide);
        const double scale = static_cast<double>(kFftSize) / static_cast<double>(n);
        for (int k = 0; k < kFftSize / 2; ++k) {
            shortMid[static_cast<size_t>(k)] *= scale;
            shortSide[static_cast<size_t>(k)] *= scale;
        }
        mid = &shortMid;
        side = &shortSide;
        frames = 1.0;
    }

    const double binHz = m_sampleRate / kFftSize;
    result.centroid_hz = static_cast<float>(centroidSum / frames * binHz);

    // ---- Per-octave RMS from FFT path (high-resolution) ----
    for (int b = 0; b < kNumBands; ++b) {
        const double fcLow = kCenterHz[b] / std::sqrt(2.0);  // -0.5 octave
        const double fcHigh = kCenterHz[b] * std::sqrt(2.0); // +0.5 octave

        const int binLow = std::max(0, static_cast<int>(fcLow / binHz));
        const int binHigh = std::min(kFftSize / 2 - 1, static_cast<int>(fcHigh / binHz));

        if (binLow > binHigh) continue; // stays at the -90 dB floor

        double bandMagSqMid = 0.0;
        double bandMagSqSide = 0.0;
        for (int k = binLow; k <= binHigh; ++k) {
            bandMagSqMid += (*mid)[static_cast<size_t>(k)];
            bandMagSqSide += (*side)[static_cast<size_t>(k)];
        }

        const double count = frames * (binHigh - binLow + 1);

        // Mid
        const double meanSqMid = bandMagSqMid / std::max(count, 1.0);
        const double meanSqNormMid = (meanSqMid / static_cast<double>(kFftSize * kFftSize)) * (8.0 / 3.0);
        result.rms_mid_per_octave_db[b] =
            meanSqNormMid > 1e-30 ? static_cast<float>(10.0 * std::log10(meanSqNormMid)) : -90.0f;
        result.rms_per_octave_db[b] = result.rms_mid_per_octave_db[b];

        // Side
        const double meanSqSide = bandMagSqSide / std::max(count, 1.0);
        const double meanSqNormSide = (meanSqSide / static_cast<double>(kFftSize * kFftSize)) * (8.0 / 3.0);
        result.rms_side_per_octave_db[b] =
            meanSqNormSide > 1e-30 ? static_cast<float>(10.0 * std::log10(meanSqNormSide)) : -90.0f;
    }

    std::array<double, kFftSize / 2> midPerFrame{};
    for (int k = 0; k < kFftSize / 2; ++k) {
        // Mid
        const double binMagSqMid = (*mid)[static_cast<size_t>(k)] / frames;
        midPerFrame[static_cast<size_t>(k)] = binMagSqMid;
        const double normMid = (binMagSqMid / static_cast<double>(kFftSize * kFftSize)) * (8.0 / 3.0);
        result.fft_mid_magnitudes[k] = normMid > 1e-30 ? static_cast<float>(10.0 * std::log10(normMid)) : -90.0f;

        // Side
        const double binMagSqSide = (*side)[static_cast<size_t>(k)] / frames;
        const double normSide = (binMagSqSide / static_cast<double>(kFftSize * kFftSize)) * (8.0 / 3.0);
        result.fft_side_magnitudes[k] = normSide > 1e-30 ? static_cast<float>(10.0 * std::log10(normSide)) : -90.0f;
    }

    // ---- Spectral tilt (octave-band power, the Python definition) ----
    result.tilt_db_per_octave = octaveBandPowerTilt(midPerFrame.data(), kFftSize / 2, binHz, m_sampleRate);

    return result;
}

} // namespace khensu
