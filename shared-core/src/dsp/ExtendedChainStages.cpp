// ENH-6: DynamicEQ (optionally keyed), MultibandCompressor, MidSideProcessor,
// Exciter, BassMonoMaker and NoiseGate for the native chain renderer - see
// ExtendedChainStages.h. Each cites the Python function it reproduces;
// change the two together (tests/shared-core/test_extended_chain_stages.cpp
// and the render parity fixture pin them).
#include <khensu/dsp/ExtendedChainStages.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace khensu::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMinMargin = 8192;
constexpr int kMaxMargin = 1 << 21;
constexpr int kMinChunk = 65536;

double dbToGain(double db)
{
    return std::pow(10.0, db / 20.0);
}

double param(const std::map<std::string, double>& p, const char* key, double fallback)
{
    const auto it = p.find(key);
    return it != p.end() ? it->second : fallback;
}

/// _ar_follower's coefficient: exp(-1 / (max(floor, ms) * 1e-3 * sr)).
double followerCoeff(double ms, double floorMs, double sampleRate)
{
    return std::exp(-1.0 / (std::max(floorMs, ms) * 0.001 * sampleRate));
}

/// Streamed input history for the chunked zero-phase stages: samples from
/// start_ on, read with scipy's odd extension past either end of the stream
/// (2 x[edge] - x[mirror]), zero beyond the mirror's reach.
class StreamWindow {
  public:
    void prepare(int numChannels, size_t reserveSamples)
    {
        hist_.assign(static_cast<size_t>(std::max(0, numChannels)), {});
        for (auto& h : hist_) h.reserve(reserveSamples);
        start_ = received_ = 0;
        ended_ = false;
    }
    void reset()
    {
        for (auto& h : hist_) h.clear();
        start_ = received_ = 0;
        ended_ = false;
    }
    size_t numChannels() const noexcept { return hist_.size(); }
    long long received() const noexcept { return received_; }
    bool ended() const noexcept { return ended_; }
    void markEnded() noexcept { ended_ = true; }

    void append(const PlanarBlock& b)
    {
        const int n = b.numSamples();
        if (n <= 0 || hist_.empty()) return;
        for (size_t c = 0; c < hist_.size(); ++c) {
            if (b.ch.empty()) break;
            const auto& src = b.ch[std::min(c, b.ch.size() - 1)];
            hist_[c].insert(hist_[c].end(), src.begin(), src.begin() + n);
        }
        received_ += n;
    }

    double at(size_t c, long long i) const noexcept
    {
        if (received_ == 0) return 0.0;
        const auto raw = [&](long long k) {
            if (k < start_ || k >= received_) return 0.0;
            return hist_[c][static_cast<size_t>(k - start_)];
        };
        if (i < 0) {
            if (-i >= received_) return 0.0;
            return 2.0 * raw(0) - raw(-i);
        }
        if (i >= received_) {
            const long long mirror = 2 * (received_ - 1) - i;
            if (mirror < start_) return 0.0;
            return 2.0 * raw(received_ - 1) - raw(mirror);
        }
        return raw(i);
    }

    void fill(size_t c, long long from, int count, double* dst) const noexcept
    {
        for (int k = 0; k < count; ++k) dst[k] = at(c, from + k);
    }

    /// Forgets samples before `index` (no allocation: a front erase).
    void dropBefore(long long index)
    {
        if (index <= start_) return;
        const long long drop = std::min(index, received_) - start_;
        if (drop <= 0) return;
        for (auto& h : hist_) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(drop));
        start_ += drop;
    }

  private:
    std::vector<std::vector<double>> hist_;
    long long start_ = 0, received_ = 0;
    bool ended_ = false;
};

/// scipy.signal.sosfiltfilt's default padlen for these sections.
int scipyPadlen(const std::vector<BiquadCoefficients>& sos) noexcept
{
    int zb = 0, za = 0;
    for (const auto& s : sos) {
        if (s.b2 == 0.0) ++zb;
        if (s.a2 == 0.0) ++za;
    }
    return 3 * (2 * static_cast<int>(sos.size()) + 1 - std::min(zb, za));
}

constexpr long long kUnknownEnd = std::numeric_limits<long long>::max();

/// sosfiltfilt over a window: x[j] is stream sample winStart + j, `last` the
/// stream's last sample (kUnknownEnd while more is coming). Exactly scipy's
/// edges: the signal is odd-extended by padlen samples (re-extended from x
/// itself, so a second pass pads its own input as a second sosfiltfilt
/// would), the forward pass starts in steady state on the first padded
/// sample and the backward pass on the last forward output - both by holding
/// that value constant over the rest of the margin, which brings a
/// zero-state filter to the same state.
void zeroPhase(BiquadCascade& f, double* x, int n, long long winStart, long long last) noexcept
{
    const int pad = scipyPadlen(f.sections());
    const auto inWin = [winStart, n](long long a) { return a >= winStart && a < winStart + n; };
    const auto at = [x, winStart](long long a) -> double& { return x[a - winStart]; };
    if (winStart < 0 && inWin(0)) {
        const double x0 = at(0);
        for (long long a = winStart; a < 0; ++a) at(a) = inWin(-a) ? 2.0 * x0 - at(-a) : 2.0 * x0;
        if (winStart < -pad) {
            const double v = at(-pad);
            for (long long a = winStart; a < -pad; ++a) at(a) = v;
        }
    }
    const long long winEnd = winStart + n - 1;
    const bool endInWin = last != kUnknownEnd && last < winEnd && inWin(last);
    if (endInWin) {
        const double xl = at(last);
        for (long long a = last + 1; a <= winEnd; ++a) {
            const long long m = 2 * last - a;
            at(a) = inWin(m) ? 2.0 * xl - at(m) : 2.0 * xl;
        }
    }
    f.reset();
    f.process(0, x, n);
    if (endInWin && last + pad < winEnd) {
        const double v = at(last + pad);
        for (long long a = last + pad + 1; a <= winEnd; ++a) at(a) = v;
    }
    std::reverse(x, x + n);
    f.reset();
    f.process(0, x, n);
    std::reverse(x, x + n);
}

BiquadCascade monoCascade(std::vector<BiquadCoefficients> sections)
{
    BiquadCascade c;
    c.setSections(std::move(sections), 1);
    return c;
}

/// A stage computed over chunks of the stream with `margin` samples of real
/// context either side (see ExtendedChainStages.h). Subclasses fill out_ in
/// render(); a stage with a whole-signal profile runs the same chunks
/// through consumeProfile() first.
class ChunkedStage : public ChainStage {
  public:
    void push(PlanarBlock& io) override
    {
        if (bypass_) return;
        in_.append(io);
        for (auto& c : io.ch) c.clear();
        drain(io, false);
    }

    void finish(PlanarBlock& tail) override
    {
        tail.ch.assign(static_cast<size_t>(numChannels_), {});
        if (bypass_) return;
        in_.markEnded();
        drain(tail, true);
    }

  protected:
    explicit ChunkedStage(int numChannels) : numChannels_(numChannels) {}

    void configure(int margin, bool bypass)
    {
        bypass_ = bypass;
        margin_ = std::clamp(margin, kMinMargin, kMaxMargin);
        chunk_ = std::max(kMinChunk, 2 * margin_);
        if (bypass_) return;
        const auto span = static_cast<size_t>(chunk_ + 2 * margin_);
        in_.prepare(numChannels_, span + 2 * static_cast<size_t>(kMinChunk));
        out_.assign(static_cast<size_t>(numChannels_), std::vector<double>(static_cast<size_t>(chunk_), 0.0));
    }

    int span(int len) const noexcept { return len + 2 * margin_; }
    size_t spanCapacity() const noexcept { return static_cast<size_t>(chunk_ + 2 * margin_); }

    /// Output for input samples [start, start + len) into out_[c][0..len).
    virtual void render(long long start, int len) = 0;
    /// The profile pass's view of the same chunk (only stages that profile).
    virtual void consumeProfile(long long /*start*/, int /*len*/) {}
    /// Whether everything render() reads up to `through` has arrived.
    virtual bool canEmit(long long /*through*/) const { return true; }
    virtual void afterEmit(long long /*emitPos*/) {}

    void loadWindow(size_t c, long long start, int len, double* dst) const
    {
        in_.fill(c, start - margin_, span(len), dst);
    }
    /// zeroPhase over a window loaded for [start, start + len) of `w`.
    void zeroPhaseWindow(BiquadCascade& f, double* x, long long start, int len, const StreamWindow& w) const
    {
        zeroPhase(f, x, span(len), start - margin_, w.ended() ? w.received() - 1 : kUnknownEnd);
    }
    void zeroPhaseWindow(BiquadCascade& f, double* x, long long start, int len) const
    {
        zeroPhaseWindow(f, x, start, len, in_);
    }

    /// Profile pass helpers: the same chunking, results to consumeProfile().
    void profileBlock(const PlanarBlock& in)
    {
        if (bypass_) return;
        in_.append(in);
        PlanarBlock none;
        drain(none, false, true);
    }
    void endProfileStream()
    {
        if (bypass_) return;
        in_.markEnded();
        PlanarBlock none;
        drain(none, true, true);
        in_.reset();
        emitPos_ = 0;
    }

    int numChannels_;
    int margin_ = kMinMargin;
    int chunk_ = kMinChunk;
    bool bypass_ = false;
    StreamWindow in_;
    long long emitPos_ = 0;
    std::vector<std::vector<double>> out_;

  private:
    void drain(PlanarBlock& out, bool atEnd, bool profiling = false)
    {
        while (true) {
            const long long avail = in_.received() - emitPos_;
            if (avail <= 0) break;
            int len = 0;
            if (avail >= static_cast<long long>(chunk_) + margin_ && canEmit(emitPos_ + chunk_ + margin_))
                len = chunk_;
            else if (atEnd)
                len = static_cast<int>(std::min<long long>(chunk_, avail));
            else
                break;
            if (profiling) {
                consumeProfile(emitPos_, len);
            } else {
                render(emitPos_, len);
                if (out.ch.size() != static_cast<size_t>(numChannels_)) out.ch.resize(static_cast<size_t>(numChannels_));
                for (size_t c = 0; c < out.ch.size(); ++c)
                    out.ch[c].insert(out.ch[c].end(), out_[c].begin(), out_[c].begin() + len);
            }
            emitPos_ += len;
            in_.dropBefore(std::max(0LL, emitPos_ - margin_));
            afterEmit(emitPos_);
        }
    }
};

// ---- DynamicEQ ---------------------------------------------------------------

/// dsp_dynamics._apply_dynamic_eq, plus the native shape / key options.
class DynamicEqStage final : public ChunkedStage {
  public:
    DynamicEqStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : ChunkedStage(numChannels)
    {
        const double nyq = sampleRate * 0.5;
        const double f = p.at("frequency_hz");
        const int shape = static_cast<int>(std::lround(param(p, "shape", 0.0)));
        useKey_ = param(p, "use_key", 0.0) >= 0.5;
        bool bypass = false;
        std::vector<BiquadCoefficients> sos;
        if (shape == 1 || shape == 2) {
            // Shelf: the band is everything below (above) frequency_hz.
            const double cutoff = std::min(f, nyq * 0.9);
            sos = butterworth(shape == 1 ? ButterworthType::Lowpass : ButterworthType::Highpass, 4, sampleRate, cutoff);
        } else {
            // Octave-symmetric band edges: bandwidth ~ 1/q octaves around f.
            const double k = std::pow(2.0, 1.0 / (2.0 * std::max(0.3, p.at("q"))));
            const double lo = f / k;
            const double hi = std::min(nyq * 0.95, f * k);
            bypass = lo >= hi || lo < 10.0; // the service returns the input unchanged
            if (!bypass) sos = butterworth(ButterworthType::Bandpass, 4, sampleRate, lo, hi);
        }
        configure(bypass ? 0 : zeroPhaseSettleSamples(sos), bypass);
        if (bypass) return;
        filter_ = monoCascade(std::move(sos));
        atk_ = followerCoeff(p.at("attack_ms"), 0.1, sampleRate);
        rel_ = followerCoeff(p.at("release_ms"), 1.0, sampleRate);
        threshold_ = dbToGain(p.at("threshold_db"));
        invRatio_ = 1.0 / std::max(1.0, p.at("ratio"));
        floor_ = dbToGain(-std::max(0.0, p.at("max_reduction_db")));
        band_.assign(static_cast<size_t>(numChannels), std::vector<double>(spanCapacity(), 0.0));
    }

    bool wantsKey() const override { return useKey_ && !bypass_; }
    void attachKey(int numKeyChannels) override
    {
        if (!wantsKey() || numKeyChannels <= 0) return;
        keyed_ = true;
        key_.prepare(numKeyChannels, spanCapacity() + 2 * static_cast<size_t>(kMinChunk));
        keyBand_.assign(static_cast<size_t>(numKeyChannels), std::vector<double>(spanCapacity(), 0.0));
    }
    void pushKey(const PlanarBlock& key) override
    {
        if (keyed_) key_.append(key);
    }
    void endKey() override
    {
        if (keyed_) key_.markEnded();
    }

  protected:
    bool canEmit(long long through) const override
    {
        return !keyed_ || key_.ended() || key_.received() >= through;
    }
    void afterEmit(long long emitPos) override
    {
        if (keyed_) key_.dropBefore(std::max(0LL, emitPos - margin_));
    }

    void render(long long start, int len) override
    {
        const int n = span(len);
        for (size_t c = 0; c < band_.size(); ++c) {
            loadWindow(c, start, len, band_[c].data());
            zeroPhaseWindow(filter_, band_[c].data(), start, len);
        }
        if (keyed_) {
            for (size_t c = 0; c < keyBand_.size(); ++c) {
                key_.fill(c, start - margin_, n, keyBand_[c].data());
                zeroPhaseWindow(filter_, keyBand_[c].data(), start, len, key_);
            }
        }
        const auto& det = keyed_ ? keyBand_ : band_;
        const double detChannels = static_cast<double>(det.size());
        for (int k = 0; k < len; ++k) {
            const auto idx = static_cast<size_t>(margin_ + k);
            double rect = 0.0;
            for (const auto& d : det) rect += std::abs(d[idx]);
            rect /= detChannels;
            env_ = rect > env_ ? atk_ * env_ + (1.0 - atk_) * rect : rel_ * env_ + (1.0 - rel_) * rect;
            const double over = std::max(env_ / threshold_, 1e-12);
            double gr = over > 1.0 ? std::pow(over, invRatio_ - 1.0) : 1.0;
            gr = std::max(gr, floor_);
            for (size_t c = 0; c < band_.size(); ++c) {
                const double x = in_.at(c, start + k);
                const double b = band_[c][idx];
                out_[c][static_cast<size_t>(k)] = (x - b) + b * gr;
            }
        }
    }

  private:
    BiquadCascade filter_;
    double atk_ = 0.0, rel_ = 0.0, threshold_ = 1.0, invRatio_ = 1.0, floor_ = 0.0;
    double env_ = 0.0;
    bool useKey_ = false, keyed_ = false;
    StreamWindow key_;
    std::vector<std::vector<double>> band_, keyBand_;
};

// ---- MultibandCompressor -----------------------------------------------------

/// The glue compressor core (_apply_glue_compressor with knee 6 dB, peak
/// detector, fixed release, makeup 0, mix 1) as a streaming gain computer.
struct GlueCore {
    double atk = 0.0, rel = 0.0, logThreshold = 0.0, slope = 0.0, half = 3.0, kneeDiv = 12.0, makeup = 1.0;
    double env = 0.0;

    void setup(double thresholdDb, double ratio, double attackMs, double releaseMs, double makeupDb, double sr)
    {
        atk = followerCoeff(attackMs, 0.1, sr);
        rel = followerCoeff(releaseMs, 1.0, sr);
        logThreshold = std::log10(std::max(dbToGain(thresholdDb), 1e-12));
        slope = 1.0 / std::max(1.0, ratio) - 1.0;
        constexpr double knee = 6.0;
        half = knee * 0.5;
        kneeDiv = 2.0 * knee;
        makeup = dbToGain(makeupDb);
    }
    /// One linked detector sample in, the gain out.
    double gain(double rect) noexcept
    {
        env = rect > env ? atk * env + (1.0 - atk) * rect : rel * env + (1.0 - rel) * rect;
        const double level = 20.0 * (std::log10(std::max(env, 1e-12)) - logThreshold);
        double grDb = 0.0;
        if (level >= half)
            grDb = level * slope;
        else if (level > -half)
            grDb = ((level + half) * (level + half) / kneeDiv) * slope;
        return std::pow(10.0, grDb / 20.0) * makeup;
    }
};

class MultibandCompressorStage final : public ChunkedStage {
  public:
    MultibandCompressorStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : ChunkedStage(numChannels)
    {
        const double nyq = sampleRate * 0.5;
        const double lowX = p.at("low_crossover_hz");
        const double highX = p.at("high_crossover_hz");
        // _band_split: high-pass when lo > 20 (cutoff held under 0.9 x
        // Nyquist), low-pass when hi < 0.95 x Nyquist.
        lowLp_ = monoCascade(butterworth(ButterworthType::Lowpass, 4, sampleRate, lowX));
        lowLpOn_ = lowX < nyq * 0.95;
        midHpOn_ = lowX > 20.0;
        midHp_ = monoCascade(butterworth(ButterworthType::Highpass, 4, sampleRate, std::min(lowX, nyq * 0.9)));
        midLpOn_ = highX < nyq * 0.95;
        midLp_ = monoCascade(butterworth(ButterworthType::Lowpass, 4, sampleRate, std::min(highX, nyq * 0.9)));
        highHpOn_ = highX > 20.0;
        highHp_ = monoCascade(butterworth(ButterworthType::Highpass, 4, sampleRate, std::min(highX, nyq * 0.9)));
        const int margin = std::max({zeroPhaseSettleSamples(lowLp_.sections()),
                                     zeroPhaseSettleSamples(midHp_.sections()) +
                                         zeroPhaseSettleSamples(midLp_.sections()),
                                     zeroPhaseSettleSamples(highHp_.sections())});
        configure(margin, false);

        const double sharedAtk = p.at("attack_ms"), sharedRel = p.at("release_ms"), sharedMakeup = p.at("makeup_db");
        const char* names[3] = {"low", "mid", "high"};
        for (int b = 0; b < 3; ++b) {
            const std::string n(names[b]);
            const auto key = [&n](const char* suffix) { return n + suffix; };
            const auto find = [&p](const std::string& k, double fallback) {
                const auto it = p.find(k);
                return it != p.end() ? it->second : fallback;
            };
            // The service applies makeup once to the sum; a per-band makeup
            // replaces that band's share of it.
            const double makeupDb = find(key("_makeup_db"), sharedMakeup);
            core_[b].setup(p.at(key("_threshold_db")), p.at(key("_ratio")), find(key("_attack_ms"), sharedAtk),
                           find(key("_release_ms"), sharedRel), makeupDb, sampleRate);
        }
        for (auto& band : bands_)
            band.assign(static_cast<size_t>(numChannels), std::vector<double>(spanCapacity(), 0.0));
    }

  protected:
    void render(long long start, int len) override
    {
        const int n = span(len);
        for (size_t c = 0; c < static_cast<size_t>(numChannels_); ++c) {
            auto& lo = bands_[0][c];
            auto& mid = bands_[1][c];
            auto& hi = bands_[2][c];
            loadWindow(c, start, len, lo.data());
            std::copy(lo.begin(), lo.begin() + n, mid.begin());
            std::copy(lo.begin(), lo.begin() + n, hi.begin());
            if (lowLpOn_) zeroPhaseWindow(lowLp_, lo.data(), start, len);
            if (midHpOn_) zeroPhaseWindow(midHp_, mid.data(), start, len);
            if (midLpOn_) zeroPhaseWindow(midLp_, mid.data(), start, len);
            if (highHpOn_) zeroPhaseWindow(highHp_, hi.data(), start, len);
        }
        for (int k = 0; k < len; ++k) {
            const auto idx = static_cast<size_t>(margin_ + k);
            double gains[3];
            for (size_t b = 0; b < 3; ++b) {
                double rect = 0.0; // linked: the loudest channel
                for (const auto& ch : bands_[b]) rect = std::max(rect, std::abs(ch[idx]));
                gains[b] = core_[b].gain(rect);
            }
            for (size_t c = 0; c < static_cast<size_t>(numChannels_); ++c)
                out_[c][static_cast<size_t>(k)] = bands_[0][c][idx] * gains[0] + bands_[1][c][idx] * gains[1] +
                                                  bands_[2][c][idx] * gains[2];
        }
    }

  private:
    BiquadCascade lowLp_, midHp_, midLp_, highHp_;
    bool lowLpOn_ = true, midHpOn_ = true, midLpOn_ = true, highHpOn_ = true;
    GlueCore core_[3];
    std::vector<std::vector<double>> bands_[3]; ///< [band][channel][span]
};

// ---- Exciter -----------------------------------------------------------------

/// dsp_color._apply_exciter: per channel, band = HP(x), sat = HP(tanh(d (band
/// + e band^2))) scaled to band_rms / sat_rms over the whole signal (the
/// profile pass), out = x + mix sat.
class ExciterStage final : public ChunkedStage {
  public:
    ExciterStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : ChunkedStage(numChannels)
    {
        const double nyq = sampleRate * 0.5;
        const double f = p.at("frequency_hz");
        mix_ = p.at("mix");
        drive_ = std::max(0.5, p.at("drive"));
        even_ = p.at("even_harmonics");
        const bool bypass = mix_ <= 0.0 || f >= nyq * 0.9;
        std::vector<BiquadCoefficients> sos;
        if (!bypass) sos = butterworth(ButterworthType::Highpass, 4, sampleRate, f);
        // Two zero-phase passes in a row: context for both.
        configure(bypass ? 0 : 2 * zeroPhaseSettleSamples(sos), bypass);
        if (bypass) return;
        hp_ = monoCascade(std::move(sos));
        band_.assign(spanCapacity(), 0.0);
        sat_.assign(spanCapacity(), 0.0);
        bandSq_.assign(static_cast<size_t>(numChannels), 0.0);
        satSq_.assign(static_cast<size_t>(numChannels), 0.0);
        scale_.assign(static_cast<size_t>(numChannels), 1.0);
    }

    bool needsProfile() const override { return !bypass_; }
    void profile(const PlanarBlock& in) override { profileBlock(in); }
    void endProfile() override
    {
        endProfileStream();
        for (size_t c = 0; c < scale_.size(); ++c) {
            // band_rms / sat_rms (the sample count cancels).
            scale_[c] = satSq_[c] > 0.0 && std::sqrt(satSq_[c] / std::max(1LL, profiled_)) > 1e-12
                            ? std::sqrt(bandSq_[c] / satSq_[c])
                            : 1.0;
        }
    }
    void adoptProfile(const ChainStage& other) override
    {
        if (const auto* o = dynamic_cast<const ExciterStage*>(&other)) scale_ = o->scale_;
    }

  protected:
    void consumeProfile(long long start, int len) override
    {
        for (size_t c = 0; c < static_cast<size_t>(numChannels_); ++c) {
            compute(c, start, len);
            for (int k = 0; k < len; ++k) {
                const auto idx = static_cast<size_t>(margin_ + k);
                bandSq_[c] += band_[idx] * band_[idx];
                satSq_[c] += sat_[idx] * sat_[idx];
            }
        }
        profiled_ += len;
    }

    void render(long long start, int len) override
    {
        for (size_t c = 0; c < static_cast<size_t>(numChannels_); ++c) {
            compute(c, start, len);
            const double g = mix_ * scale_[c];
            for (int k = 0; k < len; ++k)
                out_[c][static_cast<size_t>(k)] = in_.at(c, start + k) + g * sat_[static_cast<size_t>(margin_ + k)];
        }
    }

  private:
    void compute(size_t c, long long start, int len)
    {
        const int n = span(len);
        loadWindow(c, start, len, band_.data());
        zeroPhaseWindow(hp_, band_.data(), start, len);
        for (int k = 0; k < n; ++k) {
            const double b = band_[static_cast<size_t>(k)];
            sat_[static_cast<size_t>(k)] = std::tanh(drive_ * (b + even_ * b * b));
        }
        zeroPhaseWindow(hp_, sat_.data(), start, len);
    }

    BiquadCascade hp_;
    double mix_ = 0.0, drive_ = 4.0, even_ = 0.35;
    std::vector<double> band_, sat_, bandSq_, satSq_, scale_;
    long long profiled_ = 0;
};

// ---- BassMonoMaker -----------------------------------------------------------

/// dsp_spatial._apply_bass_mono_maker: L/R = LP(mid) + HP(L/R), zero-phase;
/// a mono file passes unchanged (front pair only).
class BassMonoMakerStage final : public ChunkedStage {
  public:
    BassMonoMakerStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : ChunkedStage(numChannels)
    {
        const double nyq = sampleRate * 0.5;
        const double cutoff = std::min(std::max(20.0, p.at("crossover_hz")), nyq * 0.9);
        const bool bypass = numChannels < 2;
        if (!bypass) {
            lp_ = monoCascade(butterworth(ButterworthType::Lowpass, 4, sampleRate, cutoff));
            hp_ = monoCascade(butterworth(ButterworthType::Highpass, 4, sampleRate, cutoff));
        }
        configure(bypass ? 0
                         : std::max(zeroPhaseSettleSamples(lp_.sections()), zeroPhaseSettleSamples(hp_.sections())),
                  bypass);
        if (bypass) return;
        for (auto& b : buf_) b.assign(spanCapacity(), 0.0);
    }

  protected:
    void render(long long start, int len) override
    {
        const int n = span(len);
        auto& l = buf_[0];
        auto& r = buf_[1];
        auto& mid = buf_[2];
        loadWindow(0, start, len, l.data());
        loadWindow(1, start, len, r.data());
        for (int k = 0; k < n; ++k) {
            const auto i = static_cast<size_t>(k);
            mid[i] = (l[i] + r[i]) * 0.5;
        }
        zeroPhaseWindow(lp_, mid.data(), start, len);
        zeroPhaseWindow(hp_, l.data(), start, len);
        zeroPhaseWindow(hp_, r.data(), start, len);
        for (int k = 0; k < len; ++k) {
            const auto idx = static_cast<size_t>(margin_ + k);
            out_[0][static_cast<size_t>(k)] = mid[idx] + l[idx];
            out_[1][static_cast<size_t>(k)] = mid[idx] + r[idx];
        }
        // Channels past the front pair pass through.
        for (size_t c = 2; c < static_cast<size_t>(numChannels_); ++c)
            for (int k = 0; k < len; ++k) out_[c][static_cast<size_t>(k)] = in_.at(c, start + k);
    }

  private:
    BiquadCascade lp_, hp_;
    std::vector<double> buf_[3];
};

// ---- MidSideProcessor --------------------------------------------------------

/// dsp_spatial._apply_mid_side_processor (mid / side gains) plus the native
/// per-path EQ. Causal biquads, in place: no latency, no allocation.
class MidSideStage final : public ChainStage {
  public:
    bool isCausalInPlace() const override { return true; } // STU-9
    MidSideStage(const std::map<std::string, double>& p, double sampleRate)
    {
        midGain_ = dbToGain(p.at("mid_gain_db"));
        sideGain_ = dbToGain(p.at("side_gain_db"));
        std::vector<BiquadCoefficients> mid, side;
        const auto bell = [&p, sampleRate](const char* prefix, std::vector<BiquadCoefficients>& into) {
            const std::string pre(prefix);
            const double g = param(p, (pre + "_eq_gain_db").c_str(), 0.0);
            if (g == 0.0) return; // an exact bypass, not a unity biquad
            into.push_back(peakFilter(sampleRate, param(p, (pre + "_eq_frequency_hz").c_str(), 1000.0),
                                      param(p, (pre + "_eq_q").c_str(), 0.7071067811865476), g));
        };
        const double sideHp = param(p, "side_highpass_frequency_hz", 0.0);
        if (sideHp > 20.0) {
            auto hp = butterworth(ButterworthType::Highpass, 2, sampleRate, std::min(sideHp, sampleRate * 0.45));
            side.insert(side.end(), hp.begin(), hp.end());
        }
        bell("mid", mid);
        bell("side", side);
        midOn_ = !mid.empty();
        sideOn_ = !side.empty();
        if (midOn_) mid_.setSections(std::move(mid), 1);
        if (sideOn_) side_.setSections(std::move(side), 1);
    }

    void push(PlanarBlock& io) override
    {
        if (io.ch.size() < 2) return; // a mono source has no side to act on
        auto& l = io.ch[0];
        auto& r = io.ch[1];
        for (size_t i = 0; i < l.size(); ++i) {
            double m = (l[i] + r[i]) * 0.5;
            double s = (l[i] - r[i]) * 0.5;
            if (midOn_) mid_.process(0, &m, 1);
            if (sideOn_) side_.process(0, &s, 1);
            m *= midGain_;
            s *= sideGain_;
            l[i] = m + s;
            r[i] = m - s;
        }
    }

  private:
    double midGain_ = 1.0, sideGain_ = 1.0;
    bool midOn_ = false, sideOn_ = false;
    BiquadCascade mid_, side_;
};

// ---- NoiseGate ---------------------------------------------------------------

/// juce::dsp::NoiseGate (pedalboard.NoiseGate), per channel: an RMS
/// BallisticsFilter (attack 0, release 50 ms), then a peak one at the gate's
/// attack / release, gain = 1 over the threshold, (env / threshold)^(ratio -
/// 1) under it.
class NoiseGateStage final : public ChainStage {
  public:
    bool isCausalInPlace() const override { return true; } // STU-9
    NoiseGateStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
    {
        const double thresholdDb = p.at("threshold_db");
        threshold_ = thresholdDb > -200.0 ? dbToGain(thresholdDb) : 0.0;
        thresholdInverse_ = threshold_ > 0.0 ? 1.0 / threshold_ : 0.0;
        ratio_ = p.at("ratio");
        const double expFactor = -2.0 * kPi * 1000.0 / sampleRate;
        const auto cte = [expFactor](double ms) { return ms < 1.0e-3 ? 0.0 : std::exp(expFactor / ms); };
        rmsAttack_ = cte(0.0);
        rmsRelease_ = cte(50.0);
        envAttack_ = cte(p.at("attack_ms"));
        envRelease_ = cte(p.at("release_ms"));
        rms_.assign(static_cast<size_t>(numChannels), 0.0);
        env_.assign(static_cast<size_t>(numChannels), 0.0);
    }

    void push(PlanarBlock& io) override
    {
        for (size_t c = 0; c < io.ch.size() && c < rms_.size(); ++c) {
            double y1 = rms_[c], y2 = env_[c];
            for (auto& s : io.ch[c]) {
                const double sq = s * s;
                y1 = sq + (sq > y1 ? rmsAttack_ : rmsRelease_) * (y1 - sq);
                const double rms = std::sqrt(y1);
                y2 = rms + (rms > y2 ? envAttack_ : envRelease_) * (y2 - rms);
                const double gain = y2 > threshold_ ? 1.0 : std::pow(y2 * thresholdInverse_, ratio_ - 1.0);
                s *= gain;
            }
            rms_[c] = y1;
            env_[c] = y2;
        }
    }

  private:
    double threshold_ = 0.0, thresholdInverse_ = 0.0, ratio_ = 10.0;
    double rmsAttack_ = 0.0, rmsRelease_ = 0.0, envAttack_ = 0.0, envRelease_ = 0.0;
    std::vector<double> rms_, env_;
};

} // namespace

int zeroPhaseSettleSamples(const std::vector<BiquadCoefficients>& sections)
{
    double r = 0.0;
    for (const auto& s : sections) {
        const double disc = s.a1 * s.a1 - 4.0 * s.a2;
        if (disc >= 0.0) {
            const double sq = std::sqrt(disc);
            r = std::max({r, std::abs((-s.a1 + sq) * 0.5), std::abs((-s.a1 - sq) * 0.5)});
        } else {
            r = std::max(r, std::sqrt(std::abs(s.a2)));
        }
    }
    if (r <= 0.0) return 0;
    if (r >= 1.0) return kMaxMargin;
    const double n = 1.5 * std::log(1e-7) / std::log(r);
    return static_cast<int>(std::min(std::ceil(n), static_cast<double>(kMaxMargin)));
}

std::unique_ptr<ChainStage> makeDynamicEqStage(const std::map<std::string, double>& p, double sampleRate,
                                               int numChannels)
{
    return std::make_unique<DynamicEqStage>(p, sampleRate, numChannels);
}

std::unique_ptr<ChainStage> makeMultibandCompressorStage(const std::map<std::string, double>& p, double sampleRate,
                                                         int numChannels)
{
    return std::make_unique<MultibandCompressorStage>(p, sampleRate, numChannels);
}

std::unique_ptr<ChainStage> makeMidSideStage(const std::map<std::string, double>& p, double sampleRate,
                                             int /*numChannels*/)
{
    return std::make_unique<MidSideStage>(p, sampleRate);
}

std::unique_ptr<ChainStage> makeExciterStage(const std::map<std::string, double>& p, double sampleRate,
                                             int numChannels)
{
    return std::make_unique<ExciterStage>(p, sampleRate, numChannels);
}

std::unique_ptr<ChainStage> makeBassMonoMakerStage(const std::map<std::string, double>& p, double sampleRate,
                                                   int numChannels)
{
    return std::make_unique<BassMonoMakerStage>(p, sampleRate, numChannels);
}

std::unique_ptr<ChainStage> makeNoiseGateStage(const std::map<std::string, double>& p, double sampleRate,
                                               int numChannels)
{
    return std::make_unique<NoiseGateStage>(p, sampleRate, numChannels);
}

} // namespace khensu::dsp
