#include <khensu/analysis/TrackTimeline.h>
#include <khensu/analysis/KWeighting.h>
#include <khensu/analysis/TruePeakFilter.h>
#include <khensu/session/AudioFileProber.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace khensu {

namespace {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(const std::array<double, 5>& c) noexcept
    {
        b0 = c[0];
        b1 = c[1];
        b2 = c[2];
        a1 = c[3];
        a2 = c[4];
        z1 = z2 = 0.0;
    }
    double process(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

float lufsOf(double meanSquare) noexcept
{
    return meanSquare > 1e-12 ? static_cast<float>(-0.691 + 10.0 * std::log10(meanSquare)) : -70.0f;
}

/// The 4x interpolated peak around sample i of `x` (n samples), linear.
double interpolatedPeakAround(const float* x, int n, int i)
{
    const auto& ph = analysis::truePeakPhases();
    double best = std::abs(static_cast<double>(x[i]));
    // Outputs between sample i-1 and i+1: positions 4(i-1)+p .. 4(i+1)+p.
    for (int centre = i; centre <= i + 1; ++centre) {
        // y[4n + p] uses x[n - half .. n + half] with the output kTruePeakHalf
        // behind the newest input, i.e. centred on x[n - half + half] = x[n].
        const int newest = centre + analysis::kTruePeakHalf;
        for (int p = 0; p < analysis::kTruePeakOversample; ++p) {
            double acc = 0.0;
            for (int j = 0; j < analysis::kTruePeakPhaseTaps; ++j) {
                const int k = newest - analysis::kTruePeakPhaseTaps + 1 + j;
                if (k >= 0 && k < n) acc += ph.w[p][j] * x[k];
            }
            best = std::max(best, std::abs(acc));
        }
    }
    return best;
}

} // namespace

float TrackTimeline::at(const std::vector<float>& series, double hopSec, double t, float fallback)
{
    if (series.empty() || hopSec <= 0.0 || !std::isfinite(t)) return fallback;
    const auto i = static_cast<long long>(std::llround(t / hopSec));
    if (i < 0 || i >= static_cast<long long>(series.size())) return fallback;
    return series[static_cast<size_t>(i)];
}

TrackTimeline measureTrackTimeline(const std::vector<const float*>& channels, int numSamples, double sampleRate,
                                   const TrackTimelineOptions& options)
{
    TrackTimeline tl;
    const int nch = std::min(static_cast<int>(channels.size()), 2);
    if (nch <= 0 || numSamples <= 0 || sampleRate <= 0.0) return tl;
    tl.sampleRate = sampleRate;
    tl.durationSec = numSamples / sampleRate;
    tl.hopSec = options.hopSec;
    tl.truePeakThresholdDbtp = options.truePeakThresholdDbtp;

    // ---- K-weighted power per 100 ms block; momentary = 4 blocks, short-term = 30.
    const auto coeffs = analysis::kWeightingCoefficients(sampleRate);
    const int hop = std::max(1, static_cast<int>(std::lround(options.hopSec * sampleRate)));
    const int hops = (numSamples + hop - 1) / hop;
    std::vector<double> blockPower(static_cast<size_t>(hops), 0.0);
    tl.peakDbfs.assign(static_cast<size_t>(hops), -240.0f);
    for (int c = 0; c < nch; ++c) {
        Biquad pre, rlb;
        pre.set(coeffs.preFilter);
        rlb.set(coeffs.rlbFilter);
        const float* x = channels[static_cast<size_t>(c)];
        for (int h = 0; h < hops; ++h) {
            const int a = h * hop, b = std::min(numSamples, a + hop);
            double sum = 0.0;
            float peak = 0.0f;
            for (int i = a; i < b; ++i) {
                const double y = rlb.process(pre.process(x[i]));
                sum += y * y;
                peak = std::max(peak, std::abs(x[i]));
            }
            blockPower[static_cast<size_t>(h)] += sum / std::max(1, b - a);
            tl.peakDbfs[static_cast<size_t>(h)] =
                std::max(tl.peakDbfs[static_cast<size_t>(h)], static_cast<float>(analysis::peakToDb(peak)));
        }
    }
    const int mBlocks = std::max(1, static_cast<int>(std::lround(0.4 / options.hopSec)));
    const int sBlocks = std::max(1, static_cast<int>(std::lround(3.0 / options.hopSec)));
    tl.momentaryLufs.resize(static_cast<size_t>(hops));
    tl.shortTermLufs.resize(static_cast<size_t>(hops));
    std::vector<double> prefix(static_cast<size_t>(hops) + 1, 0.0);
    for (int h = 0; h < hops; ++h)
        prefix[static_cast<size_t>(h) + 1] = prefix[static_cast<size_t>(h)] + blockPower[static_cast<size_t>(h)];
    for (int h = 0; h < hops; ++h) {
        const int m0 = std::max(0, h + 1 - mBlocks), s0 = std::max(0, h + 1 - sBlocks);
        tl.momentaryLufs[static_cast<size_t>(h)] =
            lufsOf((prefix[static_cast<size_t>(h) + 1] - prefix[static_cast<size_t>(m0)]) / (h + 1 - m0));
        tl.shortTermLufs[static_cast<size_t>(h)] =
            lufsOf((prefix[static_cast<size_t>(h) + 1] - prefix[static_cast<size_t>(s0)]) / (h + 1 - s0));
    }

    // ---- True-peak events (VIZ-4c).
    const double thresholdLin = std::pow(10.0, options.truePeakThresholdDbtp / 20.0);
    const double candidateLin = thresholdLin * std::pow(10.0, -3.0 / 20.0);
    std::vector<std::pair<double, double>> hits; // (time, linear peak)
    double maxPeak = 0.0;
    for (int c = 0; c < nch; ++c) {
        const float* x = channels[static_cast<size_t>(c)];
        for (int i = 0; i < numSamples; ++i) {
            const double a = std::abs(static_cast<double>(x[i]));
            maxPeak = std::max(maxPeak, a);
            if (a < candidateLin) continue;
            const double tp = interpolatedPeakAround(x, numSamples, i);
            maxPeak = std::max(maxPeak, tp);
            if (tp >= thresholdLin) hits.push_back({i / sampleRate, tp});
        }
    }
    tl.maxTruePeakDbtp = static_cast<float>(analysis::peakToDb(maxPeak));
    std::sort(hits.begin(), hits.end());
    for (const auto& [t, lin] : hits) {
        const float db = static_cast<float>(analysis::peakToDb(lin));
        if (!tl.truePeakEvents.empty() && t - tl.truePeakEvents.back().endSec < 0.05) {
            auto& e = tl.truePeakEvents.back();
            e.endSec = std::max(e.endSec, t);
            e.maxDbtp = std::max(e.maxDbtp, db);
        } else {
            tl.truePeakEvents.push_back({t, t, db});
        }
    }

    // ---- Log-band spectrogram (mono sum, Hann, 2048-point).
    const int kOrder = std::clamp(options.fftOrder, 9, 13), kN = 1 << kOrder;
    juce::dsp::FFT fft(kOrder);
    std::vector<float> window(kN), buf(2 * kN);
    for (int i = 0; i < kN; ++i)
        window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / (kN - 1));
    const int bands = std::max(8, options.bands);
    const double fLo = 30.0, fHi = std::min(16000.0, sampleRate * 0.49);
    std::vector<int> edge(static_cast<size_t>(bands) + 1);
    tl.bandHz.resize(static_cast<size_t>(bands));
    for (int b = 0; b <= bands; ++b) {
        const double hz = fLo * std::pow(fHi / fLo, static_cast<double>(b) / bands);
        edge[static_cast<size_t>(b)] = std::clamp(static_cast<int>(std::lround(hz * kN / sampleRate)), 1, kN / 2);
    }
    for (int b = 0; b < bands; ++b)
        tl.bandHz[static_cast<size_t>(b)] =
            static_cast<float>(fLo * std::pow(fHi / fLo, (b + 0.5) / bands));
    const int frames = std::clamp(numSamples / (kN / 4), 1, std::max(1, options.maxFrames));
    const double frameStep = static_cast<double>(std::max(0, numSamples - kN)) / std::max(1, frames - 1);
    tl.frameSec = frameStep / sampleRate;
    const double norm = 4.0 / (static_cast<double>(kN) * kN); // Hann-scaled full-scale sine ~ 0 dB
    tl.spectrogramDb.assign(static_cast<size_t>(frames), std::vector<float>(static_cast<size_t>(bands), -120.0f));
    for (int f = 0; f < frames; ++f) {
        const int start = static_cast<int>(std::lround(f * frameStep));
        std::fill(buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < kN && start + i < numSamples; ++i) {
            float s = 0.0f;
            for (int c = 0; c < nch; ++c)
                s += channels[static_cast<size_t>(c)][start + i];
            buf[static_cast<size_t>(i)] = s / static_cast<float>(nch) * window[static_cast<size_t>(i)];
        }
        fft.performFrequencyOnlyForwardTransform(buf.data(), true);
        auto& row = tl.spectrogramDb[static_cast<size_t>(f)];
        for (int b = 0; b < bands; ++b) {
            if (options.fineLowEnd) {
                const double loBin = fLo * std::pow(fHi / fLo, static_cast<double>(b) / bands) * kN / sampleRate;
                const double hiBin = fLo * std::pow(fHi / fLo, static_cast<double>(b + 1) / bands) * kN / sampleRate;
                if (hiBin - loBin < 2.0) {
                    // Gaussian (log-parabolic) interpolation through the three
                    // nearest bins: a Hann main lobe is close to a Gaussian, so
                    // a tone's peak lands at its own frequency, not on a bin.
                    const double c = std::clamp(0.5 * (loBin + hiBin), 1.0, kN / 2.0 - 2.0);
                    const int k = static_cast<int>(std::lround(c));
                    const double d = c - k;
                    auto lp = [&](int i) {
                        const double m = buf[static_cast<size_t>(i)];
                        return std::log(m * m + 1e-30);
                    };
                    const double ym = lp(k - 1), y0 = lp(k), yp = lp(k + 1);
                    const double density = std::exp(y0 + 0.5 * d * (yp - ym) + 0.5 * d * d * (yp - 2.0 * y0 + ym));
                    row[static_cast<size_t>(b)] =
                        static_cast<float>(10.0 * std::log10(density * (hiBin - loBin) * norm + 1e-12));
                    continue;
                }
            }
            const int k0 = edge[static_cast<size_t>(b)], k1 = std::max(k0 + 1, edge[static_cast<size_t>(b) + 1]);
            double sum = 0.0;
            for (int k = k0; k < k1 && k <= kN / 2; ++k)
                sum += static_cast<double>(buf[static_cast<size_t>(k)]) * buf[static_cast<size_t>(k)];
            row[static_cast<size_t>(b)] = static_cast<float>(10.0 * std::log10(sum * norm + 1e-12));
        }
    }
    return tl;
}

TrackTimeline measureTrackTimelineFile(const std::string& path, const TrackTimelineOptions& options,
                                       double maxSeconds)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File file(juce::String(juce::CharPointer_UTF8(path.c_str())));
    auto reader = AudioFileProber::openReader(formats, file);
    if (!reader || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0) return {};
    const auto n = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples,
                                                          static_cast<juce::int64>(maxSeconds * reader->sampleRate)));
    const int nch = static_cast<int>(std::min<unsigned int>(reader->numChannels, 2u));
    juce::AudioBuffer<float> buffer(std::max(1, nch), n);
    if (!reader->read(&buffer, 0, n, 0, true, nch > 1)) return {};
    std::vector<const float*> ch;
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        ch.push_back(buffer.getReadPointer(c));
    return measureTrackTimeline(ch, n, reader->sampleRate, options);
}

} // namespace khensu
