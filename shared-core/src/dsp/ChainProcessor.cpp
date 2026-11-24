// OVH-9: native chain stages matched to the service's processors - see
// ChainProcessor.h. Each stage cites the Python function it reproduces;
// change the two together (tests/shared-core/test_offline_chain_renderer.cpp
// pins them through fixtures written by scripts/gen_render_parity_fixtures.py).
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/IirDesign.h>
#include <khensu/dsp/SpectralDenoiser.h>
#include <khensu/dsp/TapeSaturator.h>
#include <khensu/dsp/TruePeakOversampler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace khensu::dsp {

namespace {

// ---- parameters (dsp_params._clamped_params) ---------------------------------

struct ParamRule {
    const char* name;
    double lo, hi, fallback; ///< clamp range; `fallback` = the processor's own default
};

struct EffectRules {
    const char* effect;
    std::vector<ParamRule> params;
};

constexpr double kPedalboardQ = 0.7071067690849304; // float32(1/sqrt(2)), pedalboard's default q

const std::vector<EffectRules>& rules()
{
    static const std::vector<EffectRules> table = {
        {"HighpassFilter", {{"cutoff_frequency_hz", 20.0, 2000.0, 50.0}, {"slope_db_per_oct", 6.0, 48.0, 12.0}}},
        {"LowpassFilter", {{"cutoff_frequency_hz", 500.0, 20000.0, 50.0}, {"slope_db_per_oct", 6.0, 48.0, 12.0}}},
        {"PeakFilter",
         {{"cutoff_frequency_hz", 20.0, 20000.0, 440.0}, {"gain_db", -24.0, 24.0, 0.0}, {"q", 0.1, 18.0, kPedalboardQ}}},
        {"LowShelfFilter",
         {{"cutoff_frequency_hz", 20.0, 20000.0, 440.0}, {"gain_db", -24.0, 24.0, 0.0}, {"q", 0.1, 2.0, kPedalboardQ}}},
        {"HighShelfFilter",
         {{"cutoff_frequency_hz", 20.0, 20000.0, 440.0}, {"gain_db", -24.0, 24.0, 0.0}, {"q", 0.1, 2.0, kPedalboardQ}}},
        {"Compressor",
         {{"threshold_db", -60.0, 0.0, 0.0},
          {"ratio", 1.0, 30.0, 1.0},
          {"attack_ms", 0.05, 500.0, 1.0},
          {"release_ms", 5.0, 3000.0, 100.0}}},
        {"DeEsser",
         {{"threshold_db", -60.0, 0.0, -30.0}, {"ratio", 1.0, 20.0, 4.0}, {"frequency_hz", 3000.0, 10000.0, 6500.0}}},
        {"Gain", {{"gain_db", -36.0, 24.0, 0.0}}},
        {"Limiter", {{"threshold_db", -30.0, 0.0, -1.0}, {"release_ms", 5.0, 2000.0, 60.0}}},
        // FN2-2a - defaults are the Python functions' keyword defaults.
        {"SpectralDenoise", {{"reduction_db", 0.0, 24.0, 12.0}, {"sensitivity", 0.5, 4.0, 3.0}}},
        {"TransientShaper",
         {{"attack_gain_db", -24.0, 24.0, 0.0},
          {"sustain_gain_db", -24.0, 24.0, 0.0},
          {"attack_ms", 1.0, 50.0, 3.0},
          {"release_ms", 20.0, 1000.0, 200.0}}},
        {"TapeSaturation",
         {{"drive_db", 0.0, 24.0, 6.0}, {"warmth", 0.0, 1.0, 0.5}, {"mix", 0.0, 1.0, 1.0}, {"output_trim_db", -24.0, 6.0, 0.0}}},
        {"StereoWidener", {{"width", 0.0, 2.0, 1.0}}},
    };
    return table;
}

const EffectRules* rulesFor(const std::string& effect)
{
    for (const auto& r : rules())
        if (effect == r.effect) return &r;
    return nullptr;
}

bool endsWith(const std::string& s, const char* suffix)
{
    const std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

/// Every rule's value: the given one clamped (frequencies also below
/// 0.45 x the rate, as the service does once it knows the rate), else the
/// processor's default. nullopt + `error` for a non-finite value or a
/// frequency range the rate cannot hold.
std::optional<std::map<std::string, double>> resolveParams(const ChainStageSpec& spec, double sampleRate,
                                                           std::string& error)
{
    const auto* r = rulesFor(spec.effect);
    if (r == nullptr) {
        error = spec.effect + " needs the analysis engine";
        return std::nullopt;
    }
    std::map<std::string, double> out;
    for (const auto& p : r->params) {
        const std::string key(p.name);
        double hi = p.hi;
        const bool isFrequency = endsWith(key, "frequency_hz") || endsWith(key, "crossover_hz");
        auto it = spec.parameters.find(key);
        if (it == spec.parameters.end()) {
            out[key] = p.fallback;
            continue;
        }
        const double v = static_cast<double>(it->second);
        if (!std::isfinite(v)) {
            error = "Non-finite DSP parameter: " + spec.effect + "." + key;
            return std::nullopt;
        }
        if (sampleRate > 0.0 && isFrequency) {
            hi = std::min(hi, sampleRate * 0.45);
            if (hi < p.lo) {
                error = "This sample rate cannot safely run " + spec.effect + "." + key;
                return std::nullopt;
            }
        }
        out[key] = std::clamp(v, p.lo, hi);
    }
    return out;
}

// ---- stages ------------------------------------------------------------------

class GainStage final : public ChainStage {
  public:
    explicit GainStage(double gainDb) : gain_(std::pow(10.0, gainDb / 20.0)) {}
    void push(PlanarBlock& io) override
    {
        for (auto& c : io.ch)
            for (auto& s : c) s *= gain_;
    }

  private:
    double gain_;
};

class IirStage final : public ChainStage {
  public:
    IirStage(std::vector<BiquadCoefficients> sections, int numChannels)
    {
        cascade_.setSections(std::move(sections), numChannels);
    }
    void push(PlanarBlock& io) override
    {
        for (size_t c = 0; c < io.ch.size(); ++c)
            cascade_.process(static_cast<int>(c), io.ch[c].data(), static_cast<int>(io.ch[c].size()));
    }

  private:
    BiquadCascade cascade_;
};

/// juce::dsp::Compressor (pedalboard.Compressor): a peak BallisticsFilter per
/// channel (attack when rising, release when falling, cte = exp(-2 pi 1000 /
/// (sr t_ms))), then gain = (env / threshold)^(1/ratio - 1) above threshold.
class CompressorStage final : public ChainStage {
  public:
    CompressorStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
    {
        const double thresholdDb = p.at("threshold_db");
        threshold_ = thresholdDb > -200.0 ? std::pow(10.0, thresholdDb / 20.0) : 0.0;
        thresholdInverse_ = 1.0 / threshold_;
        ratioInverse_ = 1.0 / p.at("ratio");
        const double expFactor = -2.0 * 3.14159265358979323846 * 1000.0 / sampleRate;
        const auto cte = [expFactor](double ms) { return ms < 1.0e-3 ? 0.0 : std::exp(expFactor / ms); };
        cteAttack_ = cte(p.at("attack_ms"));
        cteRelease_ = cte(p.at("release_ms"));
        env_.assign(static_cast<size_t>(numChannels), 0.0);
    }
    void push(PlanarBlock& io) override
    {
        for (size_t c = 0; c < io.ch.size() && c < env_.size(); ++c) {
            double y = env_[c];
            for (auto& s : io.ch[c]) {
                const double in = std::abs(s);
                const double cte = in > y ? cteAttack_ : cteRelease_;
                y = in + cte * (y - in);
                const double gain = y < threshold_ ? 1.0 : std::pow(y * thresholdInverse_, ratioInverse_ - 1.0);
                s *= gain;
            }
            env_[c] = y;
        }
    }

  private:
    double threshold_ = 1.0, thresholdInverse_ = 1.0, ratioInverse_ = 1.0, cteAttack_ = 0.0, cteRelease_ = 0.0;
    std::vector<double> env_;
};

/// dsp_dynamics._apply_deesser: band = sosfiltfilt(butter(4, [lo, hi], band)),
/// env = one-pole (8 ms) of |band|, out = (x - band) + band * gr with
/// gr = (env / threshold)^(1/ratio - 1) over the threshold. The zero-phase
/// band is computed over chunks with kMargin samples of real context either
/// side (the filter's ring-down is a few ms), odd-extended at the file's ends;
/// so the stage holds back kChunk + kMargin samples.
class DeEsserStage final : public ChainStage {
  public:
    static constexpr int kChunk = 65536;
    static constexpr int kMargin = 8192;

    DeEsserStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : numChannels_(numChannels)
    {
        const double nyq = sampleRate * 0.5;
        const double f = p.at("frequency_hz");
        const double lo = std::max(2000.0, f / 1.6);
        const double hi = std::min(nyq * 0.95, f * 1.6);
        bypass_ = lo >= hi;
        if (!bypass_) sections_ = butterworth(ButterworthType::Bandpass, 4, sampleRate, lo, hi);
        envCoeff_ = std::exp(-1.0 / (0.008 * sampleRate));
        threshold_ = std::pow(10.0, p.at("threshold_db") / 20.0);
        invRatio_ = 1.0 / std::max(1.0, p.at("ratio"));
        hist_.assign(static_cast<size_t>(numChannels), {});
        env_.assign(static_cast<size_t>(numChannels), 0.0);
    }

    void push(PlanarBlock& io) override
    {
        if (bypass_) return;
        for (size_t c = 0; c < hist_.size() && c < io.ch.size(); ++c)
            hist_[c].insert(hist_[c].end(), io.ch[c].begin(), io.ch[c].end());
        received_ += io.numSamples();
        io.clear();
        while (received_ - emitPos_ >= static_cast<long long>(kChunk) + kMargin) emit(kChunk, io);
    }

    void finish(PlanarBlock& tail) override
    {
        tail.ch.assign(static_cast<size_t>(numChannels_), {});
        if (bypass_) return;
        while (emitPos_ < received_) emit(static_cast<int>(std::min<long long>(kChunk, received_ - emitPos_)), tail);
    }

  private:
    /// x[i] for any i, with scipy's odd extension past either end.
    double sample(size_t c, long long i) const
    {
        const auto& h = hist_[c];
        const auto at = [&](long long k) {
            k = std::clamp(k, histStart_, received_ - 1);
            return h[static_cast<size_t>(k - histStart_)];
        };
        if (i < 0) return 2.0 * at(0) - at(-i);
        if (i >= received_) return 2.0 * at(received_ - 1) - at(2 * (received_ - 1) - i);
        return at(i);
    }

    void emit(int len, PlanarBlock& out)
    {
        if (out.ch.size() != static_cast<size_t>(numChannels_)) out.ch.assign(static_cast<size_t>(numChannels_), {});
        const long long start = emitPos_ - kMargin;
        const int span = len + 2 * kMargin;
        std::vector<double> win(static_cast<size_t>(span));
        BiquadCascade cascade;
        for (size_t c = 0; c < hist_.size(); ++c) {
            for (int k = 0; k < span; ++k) win[static_cast<size_t>(k)] = sample(c, start + k);
            cascade.setSections(sections_, 1);
            cascade.process(0, win.data(), span);
            std::reverse(win.begin(), win.end());
            cascade.reset();
            cascade.process(0, win.data(), span);
            std::reverse(win.begin(), win.end());
            double env = env_[c];
            auto& dst = out.ch[c];
            dst.reserve(dst.size() + static_cast<size_t>(len));
            for (int k = 0; k < len; ++k) {
                const double band = win[static_cast<size_t>(kMargin + k)];
                const double x = sample(c, emitPos_ + k);
                env = (1.0 - envCoeff_) * std::abs(band) + envCoeff_ * env;
                const double over = env / threshold_;
                const double gr = over > 1.0 ? std::pow(over, invRatio_ - 1.0) : 1.0;
                dst.push_back((x - band) + band * gr);
            }
            env_[c] = env;
        }
        emitPos_ += len;
        // Keep kMargin samples of history before the next chunk.
        const long long keepFrom = std::max(0LL, emitPos_ - kMargin);
        if (keepFrom > histStart_) {
            const auto drop = static_cast<size_t>(keepFrom - histStart_);
            for (auto& h : hist_) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(drop));
            histStart_ = keepFrom;
        }
    }

    int numChannels_;
    bool bypass_ = true;
    std::vector<BiquadCoefficients> sections_;
    double envCoeff_ = 0.0, threshold_ = 1.0, invRatio_ = 1.0;
    std::vector<std::vector<double>> hist_; ///< samples from histStart_ on
    std::vector<double> env_;
    long long histStart_ = 0, received_ = 0, emitPos_ = 0;
};

/// dsp_dynamics._apply_transient_shaper: one linked detector (mean |x| over
/// the channels), a fast (1 ms / 10 ms) and a slow (attack_ms / release_ms)
/// attack-release follower (_ar_follower: e = c e + (1 - c) x, c = attack
/// while the input is above the envelope), transient = clip((fast - slow) /
/// max(slow, 1e-6), 0, 1), gain = sustain + (attack - sustain) transient.
class TransientShaperStage final : public ChainStage {
  public:
    TransientShaperStage(const std::map<std::string, double>& p, double sampleRate)
    {
        const auto coeff = [sampleRate](double ms) { return std::exp(-1.0 / (std::max(1.0, ms) * 0.001 * sampleRate)); };
        fastAtk_ = std::exp(-1.0 / (0.001 * sampleRate));
        fastRel_ = std::exp(-1.0 / (0.010 * sampleRate));
        slowAtk_ = coeff(p.at("attack_ms"));
        slowRel_ = coeff(p.at("release_ms"));
        attack_ = std::pow(10.0, p.at("attack_gain_db") / 20.0);
        sustain_ = std::pow(10.0, p.at("sustain_gain_db") / 20.0);
    }
    void push(PlanarBlock& io) override
    {
        const int n = io.numSamples();
        const auto channels = static_cast<double>(io.ch.size());
        for (int i = 0; i < n; ++i) {
            const auto k = static_cast<size_t>(i);
            double level = 0.0;
            for (const auto& c : io.ch) level += std::abs(c[k]);
            level /= channels;
            fast_ = level > fast_ ? fastAtk_ * fast_ + (1.0 - fastAtk_) * level : fastRel_ * fast_ + (1.0 - fastRel_) * level;
            slow_ = level > slow_ ? slowAtk_ * slow_ + (1.0 - slowAtk_) * level : slowRel_ * slow_ + (1.0 - slowRel_) * level;
            const double amount = std::clamp((fast_ - slow_) / std::max(slow_, 1e-6), 0.0, 1.0);
            const double gain = sustain_ + (attack_ - sustain_) * amount;
            for (auto& c : io.ch) c[k] *= gain;
        }
    }

  private:
    double fastAtk_ = 0, fastRel_ = 0, slowAtk_ = 0, slowRel_ = 0, attack_ = 1, sustain_ = 1;
    double fast_ = 0.0, slow_ = 0.0;
};

/// dsp_spatial._apply_stereo_widener: side scaled by `width` in M/S; a mono
/// file passes unchanged (front pair only - for stereo, the whole buffer).
class StereoWidenerStage final : public ChainStage {
  public:
    explicit StereoWidenerStage(double width) : width_(width) {}
    void push(PlanarBlock& io) override
    {
        if (io.ch.size() < 2) return;
        auto& l = io.ch[0];
        auto& r = io.ch[1];
        for (size_t i = 0; i < l.size(); ++i) {
            const double mid = (l[i] + r[i]) * 0.5;
            const double side = (l[i] - r[i]) * 0.5 * width_;
            l[i] = mid + side;
            r[i] = mid - side;
        }
    }

  private:
    double width_;
};

/// tp_limiter.true_peak_limit, streamed. Per sample n: the 8x true-peak
/// envelope e[n] (max of the n and n-1 blocks, never below |x[n]|); required
/// reduction u[n] = max(0, 20 log10(e[n] / ceiling)); hold m[k] = max(u[k ..
/// k+L]) on a timeline starting at -L; release v[k] = max(m[k], a v[k-1]);
/// reduction r[n] = max(mean(v[n-L+1 .. n]), u[n]); y[n] = x[n] 10^(-r/20).
/// Output n needs input up to n + L + 32, so the stage holds that many back.
/// The residual static trim (re-measure, scale under the ceiling) is a
/// whole-file step: OfflineChainRenderer does it when the limiter is last.
class LimiterStage final : public ChainStage {
  public:
    LimiterStage(const std::map<std::string, double>& p, double sampleRate, int numChannels)
        : numChannels_(numChannels)
    {
        ceiling_ = std::pow(10.0, (p.at("threshold_db") - 0.05) / 20.0);
        lookahead_ = std::max(1, static_cast<int>(std::lround(1.5e-3 * sampleRate)));
        release_ = std::exp(-1.0 / (std::max(1.0, p.at("release_ms")) * 1e-3 * sampleRate));
        env_.prepare(numChannels, 8);
        frame_.assign(static_cast<size_t>(numChannels), 0.0);
        x_.assign(static_cast<size_t>(numChannels), {});
        vRing_.assign(static_cast<size_t>(lookahead_), 0.0);
        for (int k = -lookahead_; k < 0; ++k) pushU(k, 0.0, nullptr); // u = 0 before the start
    }

    void push(PlanarBlock& io) override
    {
        const int n = io.numSamples();
        PlanarBlock out;
        out.ch.assign(static_cast<size_t>(numChannels_), {});
        for (int i = 0; i < n; ++i) {
            double peak = 0.0;
            for (int c = 0; c < numChannels_; ++c) {
                const double s = io.ch[static_cast<size_t>(c)][static_cast<size_t>(i)];
                frame_[static_cast<size_t>(c)] = s;
                x_[static_cast<size_t>(c)].push_back(s);
                peak = std::max(peak, std::abs(s));
            }
            absX_.push_back(peak);
            double e0 = 0.0;
            if (env_.push(frame_.data(), e0)) envelopeReady(e0, out);
        }
        io = std::move(out);
    }

    void finish(PlanarBlock& tail) override
    {
        tail.ch.assign(static_cast<size_t>(numChannels_), {});
        double e0 = 0.0;
        for (int i = env_.tailFrames(); i > 0; --i)
            if (env_.pushZero(e0)) envelopeReady(e0, tail);
        // u = 0 past the end: the forward window reads zeros there.
        const long long end = nextU_ + lookahead_;
        while (nextU_ < end) pushU(nextU_, 0.0, &tail);
    }

  private:
    /// e0 for the next sample whose envelope was outstanding.
    void envelopeReady(double e0, PlanarBlock& out)
    {
        const long long n = nextU_;
        const double e1 = std::max(e0, absX_[static_cast<size_t>(n - absBase_)]);
        const double e = std::max(e1, prevE1_);
        prevE1_ = e1;
        const double u = std::max(0.0, 20.0 * std::log10(std::max(e, 1e-30) / ceiling_));
        pushU(n, u, &out);
    }

    /// u[k] arrives; m[k - L] is then known, and with it v[k - L] and, once
    /// k - L >= 0, output sample k - L.
    void pushU(long long k, double u, PlanarBlock* out)
    {
        if (k >= 0) {
            uFifo_.push_back(u);
            nextU_ = k + 1;
        }
        while (!window_.empty() && window_.back().second <= u) window_.pop_back();
        window_.emplace_back(k, u);
        while (window_.front().first < k - lookahead_) window_.pop_front();
        const long long j = k - lookahead_; // the index m now describes
        if (j < -lookahead_) return;
        const double m = window_.front().second;
        vPrev_ = std::max(m, release_ * vPrev_);
        vSum_ += vPrev_ - vRing_[static_cast<size_t>(vPos_)];
        vRing_[static_cast<size_t>(vPos_)] = vPrev_;
        if (++vPos_ == lookahead_) {
            vPos_ = 0;
            vSum_ = 0.0; // re-sum on each wrap so rounding cannot drift
            for (double v : vRing_) vSum_ += v;
        }
        if (j < 0 || out == nullptr) return;
        // Output sample j: the trailing mean of v over the last L values.
        const double uj = uFifo_.front();
        uFifo_.pop_front();
        const double reduction = std::max(vSum_ / lookahead_, uj);
        const double gain = reduction > 0.0 ? std::pow(10.0, -reduction / 20.0) : 1.0;
        for (int c = 0; c < numChannels_; ++c) {
            auto& q = x_[static_cast<size_t>(c)];
            out->ch[static_cast<size_t>(c)].push_back(q.front() * gain);
            q.pop_front();
        }
        absX_.pop_front();
        ++absBase_;
    }

    int numChannels_;
    double ceiling_ = 1.0, release_ = 0.0;
    int lookahead_ = 1;
    OversampledEnvelope env_;
    std::vector<double> frame_;
    std::vector<std::deque<double>> x_; ///< input not yet output
    std::deque<double> absX_;           ///< max |x| over channels, same span
    long long absBase_ = 0;             ///< index of absX_.front()
    std::deque<double> uFifo_;          ///< u for samples not yet output
    long long nextU_ = 0;               ///< next sample whose u is outstanding
    double prevE1_ = 0.0;
    std::deque<std::pair<long long, double>> window_; ///< sliding max of u
    double vPrev_ = 0.0;
    std::vector<double> vRing_;
    int vPos_ = 0;
    double vSum_ = 0.0;
};

} // namespace

bool isNativeChainEffect(const std::string& effect)
{
    return rulesFor(effect) != nullptr;
}

std::vector<std::string> effectsNeedingEngine(const std::vector<ChainStageSpec>& chain)
{
    std::vector<std::string> out;
    for (const auto& s : chain)
        if (!isNativeChainEffect(s.effect) && std::find(out.begin(), out.end(), s.effect) == out.end())
            out.push_back(s.effect);
    return out;
}

double chainOutputCeilingDbtp(const std::vector<ChainStageSpec>& chain)
{
    std::optional<double> ceiling;
    for (const auto& s : chain) {
        const char* key = s.effect == "Limiter" ? "threshold_db" : s.effect == "Clipper" ? "ceiling_db" : nullptr;
        if (key == nullptr) continue;
        const double lo = s.effect == "Limiter" ? -30.0 : -12.0;
        const double fallback = s.effect == "Limiter" ? -1.0 : -0.3;
        auto it = s.parameters.find(key);
        ceiling = (it != s.parameters.end() && std::isfinite(it->second))
                      ? std::clamp(static_cast<double>(it->second), lo, 0.0)
                      : fallback;
    }
    return ceiling.value_or(-1.0);
}

double limiterTargetDbtp(const ChainStageSpec& limiter)
{
    std::string error;
    const auto p = resolveParams(limiter, 0.0, error);
    return (p ? p->at("threshold_db") : -1.0) - 0.05;
}

std::unique_ptr<ChainStage> makeChainStage(const ChainStageSpec& spec, double sampleRate, int numChannels,
                                           std::string& error)
{
    const auto resolved = resolveParams(spec, sampleRate, error);
    if (!resolved) return nullptr;
    const auto& p = *resolved;
    const auto& e = spec.effect;
    if (e == "HighpassFilter" || e == "LowpassFilter") {
        // dsp_filters.butterworth_filter: order = round(slope / 6) in 1..8
        // (Python rounds half to even, as nearbyint does), cutoff held below
        // 0.45 x the rate.
        const int order = std::clamp(static_cast<int>(std::nearbyint(p.at("slope_db_per_oct") / 6.0)), 1, 8);
        const double cutoff = std::min(std::max(1.0, p.at("cutoff_frequency_hz")), sampleRate * 0.45);
        return std::make_unique<IirStage>(
            butterworth(e == "HighpassFilter" ? ButterworthType::Highpass : ButterworthType::Lowpass, order,
                        sampleRate, cutoff),
            numChannels);
    }
    if (e == "PeakFilter" || e == "LowShelfFilter" || e == "HighShelfFilter") {
        const double f = p.at("cutoff_frequency_hz"), g = p.at("gain_db"), q = p.at("q");
        const auto c = e == "PeakFilter" ? peakFilter(sampleRate, f, q, g)
                       : e == "LowShelfFilter" ? lowShelf(sampleRate, f, q, g)
                                               : highShelf(sampleRate, f, q, g);
        return std::make_unique<IirStage>(std::vector<BiquadCoefficients>{c}, numChannels);
    }
    if (e == "Compressor") return std::make_unique<CompressorStage>(p, sampleRate, numChannels);
    if (e == "DeEsser") return std::make_unique<DeEsserStage>(p, sampleRate, numChannels);
    if (e == "Gain") return std::make_unique<GainStage>(p.at("gain_db"));
    if (e == "Limiter") return std::make_unique<LimiterStage>(p, sampleRate, numChannels);
    if (e == "SpectralDenoise")
        return makeSpectralDenoiseStage(p.at("reduction_db"), p.at("sensitivity"), numChannels);
    if (e == "TransientShaper") return std::make_unique<TransientShaperStage>(p, sampleRate);
    if (e == "TapeSaturation")
        return makeTapeSaturationStage(p.at("drive_db"), p.at("warmth"), p.at("mix"), p.at("output_trim_db"),
                                       sampleRate, numChannels);
    if (e == "StereoWidener") return std::make_unique<StereoWidenerStage>(p.at("width"));
    error = e + " needs the analysis engine";
    return nullptr;
}

bool ChainProcessor::prepare(const std::vector<ChainStageSpec>& chain, double sampleRate, int numChannels,
                             std::string& error)
{
    stages_.clear();
    specs_ = chain;
    sampleRate_ = sampleRate;
    numChannels_ = numChannels;
    if (sampleRate <= 0.0 || numChannels <= 0) {
        error = "Invalid audio format";
        return false;
    }
    for (const auto& spec : chain) {
        auto stage = makeChainStage(spec, sampleRate, numChannels, error);
        if (!stage) {
            stages_.clear();
            return false;
        }
        stages_.push_back(std::move(stage));
    }
    return true;
}

void ChainProcessor::process(PlanarBlock block, const Sink& sink)
{
    for (auto& stage : stages_) {
        stage->push(block);
        if (block.numSamples() == 0) break;
    }
    if (block.numSamples() > 0 && sink) sink(block);
}

void ChainProcessor::finish(const Sink& sink)
{
    for (size_t k = 0; k < stages_.size(); ++k) {
        PlanarBlock tail;
        stages_[k]->finish(tail);
        for (size_t j = k + 1; j < stages_.size() && tail.numSamples() > 0; ++j) stages_[j]->push(tail);
        if (tail.numSamples() > 0 && sink) sink(tail);
    }
}

bool ChainProcessor::needsProfilePass() const
{
    return std::any_of(stages_.begin(), stages_.end(), [](const auto& s) { return s->needsProfile(); });
}

bool ChainProcessor::runProfilePasses(const Replay& replay, std::string& error)
{
    for (size_t k = 0; k < stages_.size(); ++k) {
        if (!stages_[k]->needsProfile()) continue;
        // Fresh copies of the stages in front: the real ones must start the
        // render pass from their initial state.
        std::vector<std::unique_ptr<ChainStage>> front;
        for (size_t j = 0; j < k; ++j) {
            auto copy = makeChainStage(specs_[j], sampleRate_, numChannels_, error);
            if (!copy) return false;
            if (copy->needsProfile()) copy->adoptProfile(*stages_[j]);
            front.push_back(std::move(copy));
        }
        auto& target = *stages_[k];
        const auto consume = [&front, &target](PlanarBlock&& block) {
            for (auto& stage : front) {
                stage->push(block);
                if (block.numSamples() == 0) return;
            }
            target.profile(block);
        };
        if (!replay(consume)) {
            error = "Stopped while profiling " + specs_[k].effect;
            return false;
        }
        for (size_t j = 0; j < front.size(); ++j) {
            PlanarBlock tail;
            front[j]->finish(tail);
            for (size_t i = j + 1; i < front.size() && tail.numSamples() > 0; ++i) front[i]->push(tail);
            if (tail.numSamples() > 0) target.profile(tail);
        }
        target.endProfile();
    }
    return true;
}

} // namespace khensu::dsp
