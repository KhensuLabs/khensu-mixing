// CE-PLT-3 native measurement engine - see NativeMeters.h for the contract.
// Every formula here mirrors a named Python function; keep them in lock-step
// (the parity test fails on a drift past 0.05 LU / dB).
// THREAD_CONTEXT(Any)
#include <khensu/analysis/NativeMeters.h>

#include <khensu/analysis/KWeighting.h>
#include <khensu/analysis/TruePeakFilter.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numeric>

namespace khensu::native {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAbsGateLufs = -70.0;
constexpr double kRelGateLu = -20.0; // LRA relative gate (EBU 3342)

using Index = std::int64_t;

/// numpy's round(): to nearest, ties to even (the default FP rounding mode).
Index roundEven(double v) noexcept
{
    return static_cast<Index>(std::nearbyint(v));
}

double lufsFromPower(double p) noexcept
{
    return p > 0.0 ? -0.691 + 10.0 * std::log10(p) : -std::numeric_limits<double>::infinity();
}

struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1 = 0.0, z2 = 0.0;
    /// scipy.signal.lfilter's transposed direct form II.
    inline double process(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// --------------------------------------------------------------------------
// Segment accumulator: every window any meter needs is a union of segments
// between sorted boundaries, so each channel is K-weighted exactly once and
// no full-length power buffer is ever allocated.
// --------------------------------------------------------------------------

struct Segments {
    std::vector<Index> bounds;       ///< sorted, unique, starts at 0, ends at n
    std::vector<double> powerPrefix; ///< prefix sums of channel-weighted K power
    std::vector<Index> nonzeroPrefix;

    double power(Index lo, Index hi) const noexcept { return powerPrefix[at(hi)] - powerPrefix[at(lo)]; }
    Index nonzero(Index lo, Index hi) const noexcept { return nonzeroPrefix[at(hi)] - nonzeroPrefix[at(lo)]; }

    size_t at(Index b) const noexcept
    {
        return static_cast<size_t>(std::lower_bound(bounds.begin(), bounds.end(), b) - bounds.begin());
    }
};

Segments accumulate(const float* const* ch, int numCh, Index n, double sr, const std::vector<double>& gains,
                    std::vector<Index> bounds)
{
    bounds.push_back(0);
    bounds.push_back(n);
    for (auto& b : bounds) b = std::clamp<Index>(b, 0, n);
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    const size_t numSeg = bounds.size() - 1;
    std::vector<double> segPower(numSeg, 0.0);
    std::vector<Index> segNonzero(numSeg, 0);
    const KWeighting kw = kWeighting(sr);

    for (int c = 0; c < numCh; ++c) {
        const float* x = ch[c];
        const double g = c < static_cast<int>(gains.size()) ? gains[static_cast<size_t>(c)] : 1.0;
        Biquad shelf{kw.shelf[0], kw.shelf[1], kw.shelf[2], kw.shelf[3], kw.shelf[4]};
        Biquad rlb{kw.rlb[0], kw.rlb[1], kw.rlb[2], kw.rlb[3], kw.rlb[4]};
        for (size_t s = 0; s < numSeg; ++s) {
            double acc = 0.0;
            Index nz = 0;
            for (Index i = bounds[s]; i < bounds[s + 1]; ++i) {
                const double v = static_cast<double>(x[i]);
                const double y = rlb.process(shelf.process(v));
                acc += y * y;
                nz += (x[i] != 0.0f) ? 1 : 0;
            }
            segPower[s] += g * acc;
            segNonzero[s] += nz;
        }
    }

    Segments out;
    out.bounds = std::move(bounds);
    out.powerPrefix.assign(numSeg + 1, 0.0);
    out.nonzeroPrefix.assign(numSeg + 1, 0);
    for (size_t s = 0; s < numSeg; ++s) {
        out.powerPrefix[s + 1] = out.powerPrefix[s] + segPower[s];
        out.nonzeroPrefix[s + 1] = out.nonzeroPrefix[s] + segNonzero[s];
    }
    return out;
}

struct SeriesGrid {
    Index block = 1, hop = 1;
    bool single = false; ///< n < block: one window over everything
    std::vector<Index> starts;
};

SeriesGrid seriesGrid(Index n, double sr, double blockSec, double hopSec)
{
    SeriesGrid g;
    g.block = std::max<Index>(1, roundEven(blockSec * sr));
    g.hop = std::max<Index>(1, roundEven(hopSec * sr));
    if (n < g.block) {
        g.single = true;
        return g;
    }
    for (Index s = 0; s + g.block <= n; s += g.hop) g.starts.push_back(s);
    return g;
}

void addSeriesBounds(const SeriesGrid& g, std::vector<Index>& bounds)
{
    for (Index s : g.starts) {
        bounds.push_back(s);
        bounds.push_back(s + g.block);
    }
}

/// short_term_loudness_series(): NaN for silent / non-finite windows.
std::vector<double> evalSeries(const Segments& seg, const SeriesGrid& g, Index n)
{
    std::vector<double> out;
    if (n <= 0) return out;
    auto value = [](double meanPower, bool live) {
        const double l = lufsFromPower(std::max(meanPower, 0.0));
        return (std::isfinite(l) && live) ? l : kNaN;
    };
    if (g.single) {
        out.push_back(value(seg.power(0, n) / static_cast<double>(n), seg.nonzero(0, n) > 0));
        return out;
    }
    out.reserve(g.starts.size());
    for (Index s : g.starts)
        out.push_back(value(seg.power(s, s + g.block) / static_cast<double>(g.block),
                            seg.nonzero(s, s + g.block) > 0));
    return out;
}

double nanMax(const std::vector<double>& v)
{
    double m = kNaN;
    for (double x : v)
        if (std::isfinite(x) && !(x <= m)) m = x;
    return m;
}

double nearestRank(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<Index>(std::floor(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[static_cast<size_t>(std::clamp<Index>(idx, 0, static_cast<Index>(v.size()) - 1))];
}

/// loudness_range() on a precomputed 3 s / 0.1 s series.
double lraFromShortTerm(const std::vector<double>& st)
{
    std::vector<double> absGated;
    for (double v : st)
        if (std::isfinite(v) && v >= kAbsGateLufs) absGated.push_back(v);
    if (absGated.size() < 2) return 0.0;
    double meanPow = 0.0;
    for (double v : absGated) meanPow += std::pow(10.0, v / 10.0);
    meanPow /= static_cast<double>(absGated.size());
    const double rel = 10.0 * std::log10(meanPow) + kRelGateLu;
    std::vector<double> kept;
    for (double v : absGated)
        if (v >= rel) kept.push_back(v);
    if (kept.size() < 2) return 0.0;
    return std::max(0.0, nearestRank(kept, 0.95) - nearestRank(kept, 0.10));
}

// True peak: the shared 4x interpolator (analysis/TruePeakFilter.h, MEAS-5).
constexpr int kOversample = analysis::kTruePeakOversample;
constexpr int kTpHalf = analysis::kTruePeakHalf;
constexpr int kTpPhaseTaps = analysis::kTruePeakPhaseTaps;

// --------------------------------------------------------------------------
// Radix-2 FFT for the Welch spectrum (no JUCE dependency in this file).
// --------------------------------------------------------------------------

void fftInPlace(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

constexpr double kOctaveCenters[] = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
constexpr double kTiltCapHz = 20000.0;

} // namespace

// ==========================================================================
// Public API
// ==========================================================================

std::vector<double> bs1770ChannelGains(int numChannels)
{
    if (numChannels == 6) return {1.0, 1.0, 1.0, 0.0, 1.41, 1.41};
    if (numChannels == 8) return {1.0, 1.0, 1.0, 0.0, 1.41, 1.41, 1.0, 1.0};
    static constexpr double plain[] = {1.0, 1.0, 1.0, 1.41, 1.41};
    if (numChannels < 1 || numChannels > 5) return {};
    return std::vector<double>(plain, plain + numChannels);
}

KWeighting kWeighting(double sr)
{
    // The one C++ K-weighting (analysis/KWeighting.h, FIX-11 / MEAS-5).
    const auto c = analysis::kWeightingCoefficients(sr);
    KWeighting k{};
    std::copy(c.preFilter.begin(), c.preFilter.end(), k.shelf);
    std::copy(c.rlbFilter.begin(), c.rlbFilter.end(), k.rlb);
    return k;
}

std::vector<float> loudnessSeries(const float* const* channels, int numChannels, std::int64_t n, double sr,
                                  double blockSec, double hopSec)
{
    std::vector<float> out;
    const auto gains = bs1770ChannelGains(numChannels);
    if (channels == nullptr || n <= 0 || sr <= 0.0 || gains.empty()) return out;
    const auto grid = seriesGrid(n, sr, blockSec, hopSec);
    std::vector<Index> bounds;
    addSeriesBounds(grid, bounds);
    const auto seg = accumulate(channels, numChannels, n, sr, gains, std::move(bounds));
    for (double v : evalSeries(seg, grid, n)) out.push_back(static_cast<float>(v));
    return out;
}

double truePeakLinear(const float* x, std::int64_t n)
{
    if (x == nullptr || n <= 0) return 0.0;
    const auto& f = analysis::truePeakPhases();
    double peak = 0.0;
    for (Index i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::abs(x[i])));

    // Chunked through a zero-padded double buffer so memory stays bounded and
    // every dot product reads one contiguous run (vectorisable).
    constexpr Index kChunk = 1 << 16;
    std::vector<double> buf(static_cast<size_t>(kChunk + 2 * kTpHalf));
    for (Index s = 0; s < n; s += kChunk) {
        const Index e = std::min(n, s + kChunk);
        const Index len = e - s;
        for (Index j = 0; j < len + 2 * kTpHalf; ++j) {
            const Index src = s - kTpHalf + j;
            buf[static_cast<size_t>(j)] = (src >= 0 && src < n) ? static_cast<double>(x[src]) : 0.0;
        }
        for (int p = 0; p < kOversample; ++p) {
            const double* w = f.w[p];
            for (Index i = 0; i < len; ++i) {
                const double* win = buf.data() + i;
                double acc = 0.0;
                for (int j = 0; j < kTpPhaseTaps; ++j) acc += w[j] * win[j];
                peak = std::max(peak, std::abs(acc));
            }
        }
    }
    return peak;
}

LoudnessMeasurement measureLoudness(const float* const* channels, int numChannels, std::int64_t n, double sr)
{
    LoudnessMeasurement m;
    if (channels == nullptr || numChannels <= 0 || n <= 0 || sr <= 0.0) return m;

    double tp = 0.0, sp = 0.0;
    for (int c = 0; c < numChannels; ++c) {
        tp = std::max(tp, truePeakLinear(channels[c], n));
        for (Index i = 0; i < n; ++i) sp = std::max(sp, static_cast<double>(std::abs(channels[c][i])));
    }
    m.truePeakDbtp = analysis::peakToDb(tp);
    m.samplePeakDbfs = analysis::peakToDb(sp);

    const auto gains = bs1770ChannelGains(numChannels);
    if (gains.empty()) {
        m.supportedLayout = false;
        return m;
    }

    const auto stGrid = seriesGrid(n, sr, 3.0, 0.1);
    const auto momGrid = seriesGrid(n, sr, 0.4, 0.1);
    std::vector<Index> bounds;
    addSeriesBounds(stGrid, bounds);
    addSeriesBounds(momGrid, bounds);

    // Integrated-loudness block grid.
    const Index block400 = roundEven(0.4 * sr);
    const bool surround = numChannels == 6 || numChannels == 8;
    struct Block {
        Index lo, hi;
    };
    std::vector<Block> blocks;
    double blockNorm = 0.0;
    if (n >= block400) {
        if (surround) {
            // gated_integrated_from_power(): exact 400 ms / 100 ms grid.
            const Index hop = std::max<Index>(1, roundEven(0.1 * sr));
            for (Index s = 0; s + block400 <= n; s += hop) blocks.push_back({s, s + block400});
            blockNorm = static_cast<double>(block400);
        } else {
            // _pyloudnorm_integrated(): pyloudnorm's block bounds, verbatim.
            const double tg = 0.400;
            const double step = 1.0 - 0.75;
            const double rate = sr;
            const double tLen = static_cast<double>(n) / rate;
            const Index numBlocks = roundEven((tLen - tg) / (tg * step)) + 1;
            for (Index j = 0; j < numBlocks; ++j) {
                const double jd = static_cast<double>(j);
                const auto lo = static_cast<Index>(tg * (jd * step) * rate);
                const auto hi = static_cast<Index>(tg * (jd * step + 1.0) * rate);
                blocks.push_back({std::min(lo, n), std::min(hi, n)});
            }
            blockNorm = tg * rate;
        }
        for (const auto& b : blocks) {
            bounds.push_back(b.lo);
            bounds.push_back(b.hi);
        }
    }

    const auto seg = accumulate(channels, numChannels, n, sr, gains, std::move(bounds));

    const auto st = evalSeries(seg, stGrid, n);
    const auto mom = evalSeries(seg, momGrid, n);
    const auto toFloat = [](const std::vector<double>& v) {
        std::vector<float> out;
        out.reserve(v.size());
        for (const double x : v) out.push_back(static_cast<float>(x));
        return out;
    };
    m.shortTerm = toFloat(st);
    m.momentary = toFloat(mom);
    m.shortTermMaxLufs = nanMax(st);
    m.momentaryMaxLufs = nanMax(mom);

    if (n < block400) return m; // integrated and LRA undefined below one block

    m.lra = lraFromShortTerm(st);

    std::vector<double> z(blocks.size());
    for (size_t j = 0; j < blocks.size(); ++j)
        z[j] = std::max(0.0, seg.power(blocks[j].lo, blocks[j].hi) / blockNorm);

    double absSum = 0.0;
    size_t absN = 0;
    for (double v : z) {
        const double l = lufsFromPower(v);
        if (surround ? l > kAbsGateLufs : l >= kAbsGateLufs) {
            absSum += v;
            ++absN;
        }
    }
    if (absN == 0) return m;
    const double rel = lufsFromPower(absSum / static_cast<double>(absN)) - 10.0;
    double keepSum = 0.0;
    size_t keepN = 0;
    for (double v : z) {
        const double l = lufsFromPower(v);
        if (l > kAbsGateLufs && l > rel) {
            keepSum += v;
            ++keepN;
        }
    }
    if (keepN == 0) return m;
    const double integrated = lufsFromPower(keepSum / static_cast<double>(keepN));
    if (std::isfinite(integrated)) m.integratedLufs = integrated;
    return m;
}

StereoMeasurement measureStereo(const float* left, const float* right, std::int64_t n)
{
    StereoMeasurement s;
    if (left == nullptr || right == nullptr || n <= 0) return s;
    s.stereo = true;
    const double invN = 1.0 / static_cast<double>(n);

    // dsp_metrics.stereo_width: M = (L+R)/2, S = (L-R)/2 mean-square energies.
    double mm = 0.0, ss = 0.0, sumL = 0.0, sumR = 0.0;
    for (Index i = 0; i < n; ++i) {
        const double l = left[i], r = right[i];
        const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
        mm += mid * mid;
        ss += side * side;
        sumL += l;
        sumR += r;
    }
    mm *= invN;
    ss *= invN;
    const double total = mm + ss;
    s.width = (std::isfinite(total) && total > 1e-30) ? std::clamp(2.0 * ss / total, 0.0, 1.0) : 0.0;

    // dsp_metrics.stereo_correlation: mean-removed Pearson, population std,
    // silence guard 1e-9 (both silent -> 1, one silent -> 0).
    const double ml = sumL * invN, mr = sumR * invN;
    double vl = 0.0, vr = 0.0, cov = 0.0;
    for (Index i = 0; i < n; ++i) {
        const double l = left[i] - ml, r = right[i] - mr;
        vl += l * l;
        vr += r * r;
        cov += l * r;
    }
    const double sl = std::sqrt(vl * invN), sr = std::sqrt(vr * invN);
    constexpr double kSilent = 1e-9;
    if (sl < kSilent && sr < kSilent)
        s.correlation = 1.0;
    else if (sl < kSilent || sr < kSilent)
        s.correlation = 0.0;
    else
        s.correlation = std::clamp(cov * invN / (sl * sr), -1.0, 1.0);
    return s;
}

BandSpectrum measureBandSpectrum(const float* const* channels, int numChannels, std::int64_t n, double sr)
{
    BandSpectrum out;
    if (channels == nullptr || numChannels <= 0 || n <= 0 || sr <= 0.0) return out;

    // Mono downmix (5.1 / 7.1 leave the LFE, channel 3, out).
    std::vector<int> use;
    for (int c = 0; c < numChannels; ++c)
        if (!((numChannels == 6 || numChannels == 8) && c == 3)) use.push_back(c);
    const auto nfft = static_cast<Index>(
        std::max(4096.0, std::pow(2.0, std::ceil(std::log2(sr / 10.0)))));
    const Index len = std::max(n, nfft);
    std::vector<double> mono(static_cast<size_t>(len), 0.0);
    for (Index i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int c : use) acc += static_cast<double>(channels[c][i]);
        mono[static_cast<size_t>(i)] = acc / static_cast<double>(use.size());
    }

    // scipy.signal.welch(window="hann", nperseg=nfft, noverlap=nfft//2,
    // scaling="spectrum", detrend="constant", average="mean").
    const Index step = nfft - nfft / 2;
    const Index numSeg = (len - nfft) / step + 1;
    std::vector<double> win(static_cast<size_t>(nfft));
    double winSum = 0.0;
    for (Index i = 0; i < nfft; ++i) {
        win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(nfft));
        winSum += win[static_cast<size_t>(i)];
    }
    const size_t bins = static_cast<size_t>(nfft / 2 + 1);
    std::vector<double> psd(bins, 0.0);
    std::vector<std::complex<double>> buf(static_cast<size_t>(nfft));
    for (Index sgi = 0; sgi < numSeg; ++sgi) {
        const double* x = mono.data() + sgi * step;
        double mean = 0.0;
        for (Index i = 0; i < nfft; ++i) mean += x[i];
        mean /= static_cast<double>(nfft);
        for (Index i = 0; i < nfft; ++i)
            buf[static_cast<size_t>(i)] = {(x[i] - mean) * win[static_cast<size_t>(i)], 0.0};
        fftInPlace(buf);
        for (size_t k = 0; k < bins; ++k) psd[k] += std::norm(buf[k]);
    }
    const double scale = 1.0 / (winSum * winSum * static_cast<double>(numSeg));
    for (size_t k = 0; k < bins; ++k) psd[k] *= scale * ((k == 0 || k == bins - 1) ? 1.0 : 2.0);

    // spectral_tilt_db_per_octave's octave levels.
    const double cap = std::min(sr / 2.0, kTiltCapHz);
    const double binHz = sr / static_cast<double>(nfft);
    std::vector<double> logC;
    for (double fc : kOctaveCenters) {
        const double lo = fc / std::sqrt(2.0);
        const double hi = std::min(fc * std::sqrt(2.0), cap);
        if (hi <= lo) continue;
        const double covered = std::log2(hi / lo);
        if (covered < 0.5) continue;
        double energy = 0.0;
        bool any = false;
        for (size_t k = 0; k < bins; ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f >= lo && f < hi) {
                energy += psd[k];
                any = true;
            }
        }
        if (!any) continue;
        out.centersHz.push_back(fc);
        out.levelsDb.push_back(10.0 * std::log10(energy / covered + 1e-30));
        logC.push_back(std::log2(fc));
    }
    if (out.levelsDb.size() >= 2) {
        const double nPts = static_cast<double>(logC.size());
        const double mx = std::accumulate(logC.begin(), logC.end(), 0.0) / nPts;
        const double my = std::accumulate(out.levelsDb.begin(), out.levelsDb.end(), 0.0) / nPts;
        double sxy = 0.0, sxx = 0.0;
        for (size_t i = 0; i < logC.size(); ++i) {
            sxy += (logC[i] - mx) * (out.levelsDb[i] - my);
            sxx += (logC[i] - mx) * (logC[i] - mx);
        }
        out.tiltDbPerOctave = sxx > 0.0 ? sxy / sxx : 0.0;
    }
    return out;
}

NativeReport measureAll(const float* const* channels, int numChannels, std::int64_t n, double sr)
{
    NativeReport r;
    r.sampleRate = sr;
    r.numChannels = numChannels;
    r.durationSec = sr > 0.0 ? static_cast<double>(n) / sr : 0.0;
    r.loudness = measureLoudness(channels, numChannels, n, sr);
    if (numChannels >= 2 && channels != nullptr) r.stereo = measureStereo(channels[0], channels[1], n);
    r.spectrum = measureBandSpectrum(channels, numChannels, n, sr);
    return r;
}

} // namespace khensu::native
