// STU-9: live preview of a native chain - see RealtimeChain.h.
// STU-18: the CausalEquivalent stand-ins from RealtimeStages.h.
#include <khensu/dsp/RealtimeChain.h>

#include <khensu/dsp/IirDesign.h>
#include <khensu/dsp/LookaheadBrickwallLimiter.h>
#include <khensu/dsp/RealtimeStages.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace khensu::dsp {

namespace {

constexpr double kMatchSeconds = 0.8;     ///< power averaging time constant
constexpr double kSilencePower = 1.0e-7;  ///< -70 dBFS mean square: freeze the match
constexpr float kMaxMatchDb = 18.0f;
constexpr double kLimiterLookaheadSeconds = 1.5e-3; ///< the offline Limiter's lookahead

using Params = std::map<std::string, double>;

double paramOr(const ChainStageSpec& spec, const char* key, double fallback, double lo, double hi)
{
    const auto it = spec.parameters.find(key);
    if (it == spec.parameters.end() || !std::isfinite(it->second)) return fallback;
    return std::clamp(static_cast<double>(it->second), lo, hi);
}

float dbToGain(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

double at(const Params& p, const char* key, double fallback)
{
    const auto it = p.find(key);
    return it != p.end() ? it->second : fallback;
}

/// The RBJ Q whose band is `octaves` wide (between the -3 dB points of the
/// band-pass the dynamic band keys on).
float qForOctaves(double octaves)
{
    const double n = std::max(0.05, octaves);
    const double k = std::pow(2.0, n);
    return static_cast<float>(std::sqrt(k) / (k - 1.0));
}

// ---- the stand-ins -----------------------------------------------------------

/// Limiter: the plugin's sample-peak lookahead limiter at the same ceiling.
class LimiterLive final : public RealtimeChain::LiveStage {
  public:
    LimiterLive(const ChainStageSpec& spec, double sampleRate, int numChannels)
    {
        const int lookahead = std::max(1, static_cast<int>(std::lround(kLimiterLookaheadSeconds * sampleRate)));
        limiter_.prepare(sampleRate, numChannels, lookahead);
        ceiling_ = dbToGain(limiterTargetDbtp(spec));
    }
    void process(float* const* channels, int numChannels, int numSamples) noexcept override
    {
        limiter_.process(channels, numChannels, numSamples, ceiling_, true);
    }
    int latencySamples() const noexcept override { return limiter_.latencySamples(); }

  private:
    LookaheadBrickwallLimiter limiter_;
    float ceiling_ = 1.0f;
};

/// BassMonoMaker: an LR4 split at the same crossover.
class BassMonoLive final : public RealtimeChain::LiveStage {
  public:
    BassMonoLive(const Params& p, double sampleRate)
    {
        mono_.prepare(sampleRate);
        mono_.setCrossover(static_cast<float>(at(p, "crossover_hz", RealtimeBassMono::kDefaultCrossoverHz)));
    }
    void process(float* const* channels, int numChannels, int numSamples) noexcept override
    {
        mono_.process(channels, numChannels, numSamples); // the front pair; mono passes
    }

  private:
    RealtimeBassMono mono_;
};

/// TapeSaturation: the same curve without oversampling.
class SaturationLive final : public RealtimeChain::LiveStage {
  public:
    SaturationLive(const Params& p, double sampleRate)
    {
        sat_.prepare(sampleRate);
        settings_.driveDb = static_cast<float>(at(p, "drive_db", 6.0));
        settings_.warmth = static_cast<float>(at(p, "warmth", 0.5));
        settings_.mix = static_cast<float>(at(p, "mix", 1.0));
        settings_.trimDb = static_cast<float>(at(p, "output_trim_db", 0.0));
    }
    void process(float* const* channels, int numChannels, int numSamples) noexcept override
    {
        sat_.process(channels, numChannels, numSamples, settings_);
    }

  private:
    RealtimeSaturation sat_;
    RealtimeSaturation::Settings settings_;
};

/// DynamicEQ / DeEsser: one self-keyed dynamic band (a static gain of 0 dB
/// that dips by the reduction).
class DynamicBandLive final : public RealtimeChain::LiveStage {
  public:
    DynamicBandLive(double sampleRate, int numChannels, RealtimeEqBand::Type type, float freqHz, float q,
                    const RealtimeEqBand::Dynamics& dynamics)
    {
        band_.prepare(sampleRate, numChannels);
        band_.setBand(type, RealtimeEqBand::Channels::Stereo, freqHz, q, 0.0f, true);
        band_.setDynamics(dynamics);
    }
    void process(float* const* channels, int numChannels, int numSamples) noexcept override
    {
        band_.process(channels, numChannels, numSamples);
    }

  private:
    RealtimeEqBand band_;
};

/// Exciter: band = HP(x), sat = HP(tanh(drive (band + even band^2))),
/// out = x + mix * scale * sat - HP the offline stage's 4th-order Butterworth
/// run twice (the same magnitude as its zero-phase pass, minimum phase), and
/// scale = band RMS / sat RMS over a running ~0.5 s average (the render uses
/// the whole track's).
class ExciterLive final : public RealtimeChain::LiveStage {
  public:
    ExciterLive(const Params& p, double sampleRate, int numChannels, int maxBlock)
    {
        mix_ = at(p, "mix", 0.3);
        drive_ = std::max(0.5, at(p, "drive", 4.0));
        even_ = at(p, "even_harmonics", 0.35);
        const double f = std::min(at(p, "frequency_hz", 3000.0), sampleRate * 0.45);
        auto sos = butterworth(ButterworthType::Highpass, 4, sampleRate, f);
        auto twice = sos;
        twice.insert(twice.end(), sos.begin(), sos.end());
        numChannels_ = std::max(1, numChannels);
        bandHp_.setSections(twice, numChannels_);
        satHp_.setSections(std::move(twice), numChannels_);
        band_.assign(static_cast<size_t>(maxBlock), 0.0);
        sat_.assign(static_cast<size_t>(maxBlock), 0.0);
        bandPow_.assign(static_cast<size_t>(numChannels_), 0.0);
        satPow_.assign(static_cast<size_t>(numChannels_), 0.0);
        avg_ = std::exp(-1.0 / (0.5 * sampleRate));
    }
    void process(float* const* channels, int numChannels, int numSamples) noexcept override
    {
        const int nch = std::min(numChannels, numChannels_);
        const int n = std::min(numSamples, static_cast<int>(band_.size()));
        for (int c = 0; c < nch; ++c) {
            const auto ch = static_cast<size_t>(c);
            float* x = channels[c];
            for (int i = 0; i < n; ++i) band_[static_cast<size_t>(i)] = static_cast<double>(x[i]);
            bandHp_.process(c, band_.data(), n);
            for (int i = 0; i < n; ++i) {
                const double b = band_[static_cast<size_t>(i)];
                sat_[static_cast<size_t>(i)] = std::tanh(drive_ * (b + even_ * b * b));
            }
            satHp_.process(c, sat_.data(), n);
            double bp = bandPow_[ch], sp = satPow_[ch];
            for (int i = 0; i < n; ++i) {
                const double b = band_[static_cast<size_t>(i)], s = sat_[static_cast<size_t>(i)];
                bp = avg_ * bp + (1.0 - avg_) * b * b;
                sp = avg_ * sp + (1.0 - avg_) * s * s;
                const double scale = sp > 1.0e-20 ? std::min(kMaxScale, std::sqrt(bp / sp)) : 1.0;
                x[i] = static_cast<float>(static_cast<double>(x[i]) + mix_ * scale * s);
            }
            // A non-finite input (contained by the caller) must not stick.
            bandPow_[ch] = std::isfinite(bp) ? bp : 0.0;
            satPow_[ch] = std::isfinite(sp) ? sp : 0.0;
        }
    }

  private:
    static constexpr double kMaxScale = 4.0;
    BiquadCascade bandHp_, satHp_;
    std::vector<double> band_, sat_, bandPow_, satPow_;
    double mix_ = 0.3, drive_ = 4.0, even_ = 0.35, avg_ = 0.0;
    int numChannels_ = 1;
};

/// A stand-in for `spec`, or nullptr (with `note` empty) when the offline
/// stage would pass the signal unchanged; `note` says how it differs.
std::unique_ptr<RealtimeChain::LiveStage> makeLiveStage(const ChainStageSpec& spec, const Params& p,
                                                        double sampleRate, int numChannels, int maxBlock,
                                                        std::string& note)
{
    const auto& e = spec.effect;
    const double nyq = sampleRate * 0.5;
    if (e == "Limiter") {
        note = "previewed with a sample-peak limiter at the same ceiling; the render limits true peak";
        return std::make_unique<LimiterLive>(spec, sampleRate, numChannels);
    }
    if (e == "BassMonoMaker") {
        if (numChannels < 2) return nullptr; // the render passes mono unchanged
        note = "previewed with a minimum-phase crossover at the same frequency; the render's is zero-phase";
        return std::make_unique<BassMonoLive>(p, sampleRate);
    }
    if (e == "TapeSaturation") {
        note = "previewed with the same curve without oversampling; the render oversamples 4x";
        return std::make_unique<SaturationLive>(p, sampleRate);
    }
    if (e == "DynamicEQ") {
        const int shape = static_cast<int>(std::lround(at(p, "shape", 0.0)));
        const double f = at(p, "frequency_hz", 3000.0);
        RealtimeEqBand::Dynamics dyn;
        dyn.on = true;
        dyn.thresholdDb = static_cast<float>(at(p, "threshold_db", -30.0));
        dyn.ratio = static_cast<float>(at(p, "ratio", 4.0));
        dyn.rangeDb = static_cast<float>(at(p, "max_reduction_db", 24.0));
        dyn.attackMs = static_cast<float>(at(p, "attack_ms", 5.0));
        dyn.releaseMs = static_cast<float>(at(p, "release_ms", 120.0));
        RealtimeEqBand::Type type = RealtimeEqBand::Type::Bell;
        float q = 0.70710678f;
        if (shape == 1)
            type = RealtimeEqBand::Type::LowShelf;
        else if (shape == 2)
            type = RealtimeEqBand::Type::HighShelf;
        else {
            // The render's band: 1/q octaves around f (it passes the input
            // unchanged when the band reaches below 10 Hz).
            const double octaves = 1.0 / std::max(0.3, at(p, "q", 2.0));
            const double k = std::pow(2.0, octaves / 2.0);
            if (f / k < 10.0 || f / k >= std::min(nyq * 0.95, f * k)) return nullptr;
            q = qForOctaves(octaves);
        }
        note = at(p, "use_key", 0.0) >= 0.5
                   ? "previewed with a causal dynamic band keyed on itself; the render keys on the key signal"
                   : "previewed with a causal dynamic band; the render's band is zero-phase";
        return std::make_unique<DynamicBandLive>(sampleRate, numChannels, type, static_cast<float>(f), q, dyn);
    }
    if (e == "DeEsser") {
        const double f = at(p, "frequency_hz", 6500.0);
        const double lo = std::max(2000.0, f / 1.6);
        const double hi = std::min(nyq * 0.95, f * 1.6);
        if (lo >= hi) return nullptr; // the render passes the input unchanged
        RealtimeEqBand::Dynamics dyn;
        dyn.on = true;
        dyn.thresholdDb = static_cast<float>(at(p, "threshold_db", -30.0));
        dyn.ratio = static_cast<float>(at(p, "ratio", 4.0));
        dyn.rangeDb = 48.0f; // the render has no floor
        dyn.attackMs = 8.0f; // its one-pole envelope, both ways
        dyn.releaseMs = 8.0f;
        note = "previewed with a causal dynamic band over the same range; the render's band is zero-phase";
        return std::make_unique<DynamicBandLive>(sampleRate, numChannels, RealtimeEqBand::Type::Bell,
                                                 static_cast<float>(std::sqrt(lo * hi)),
                                                 qForOctaves(std::log2(hi / lo)), dyn);
    }
    if (e == "Exciter") {
        if (at(p, "mix", 0.3) <= 0.0 || at(p, "frequency_hz", 3000.0) >= nyq * 0.9) return nullptr;
        note = "previewed with minimum-phase filters, level set from the last half second; the render uses "
               "the whole track";
        return std::make_unique<ExciterLive>(p, sampleRate, numChannels, maxBlock);
    }
    return nullptr;
}

bool hasLiveStandIn(const std::string& effect)
{
    return effect == "Limiter" || effect == "BassMonoMaker" || effect == "TapeSaturation" || effect == "DynamicEQ" ||
           effect == "DeEsser" || effect == "Exciter";
}

} // namespace

bool RealtimeChain::prepare(const std::vector<ChainStageSpec>& chain, double sampleRate, int numChannels,
                            int maxBlockSize, std::string& error)
{
    plan_.clear();
    slots_.clear();
    latency_ = 0;
    predictedGainDb_ = 0.0;
    if (sampleRate <= 0.0 || numChannels <= 0 || maxBlockSize <= 0) {
        error = "Invalid audio format";
        return false;
    }
    sampleRate_ = sampleRate;
    numChannels_ = numChannels;
    maxBlock_ = maxBlockSize;

    for (const auto& spec : chain) {
        StagePlan entry;
        entry.effect = spec.effect;
        if (!isNativeChainEffect(spec.effect)) {
            entry.note = "needs the analysis engine - heard in the render only";
            plan_.push_back(std::move(entry));
            continue;
        }
        if (hasLiveStandIn(spec.effect)) {
            // STU-18: a causal stand-in, reading the parameters as the
            // offline stage reads them.
            std::string paramError;
            const auto params = resolveChainStageParams(spec, sampleRate, paramError);
            if (!params) {
                entry.note = paramError.empty() ? std::string("cannot run at this sample rate") : paramError;
                plan_.push_back(std::move(entry));
                continue;
            }
            std::string note;
            auto live = makeLiveStage(spec, *params, sampleRate, numChannels, maxBlockSize, note);
            if (!live) {
                // The render passes the signal unchanged here: so does the preview.
                entry.mode = StageMode::Exact;
                plan_.push_back(std::move(entry));
                continue;
            }
            latency_ += live->latencySamples();
            Slot slot;
            slot.live = std::move(live);
            slots_.push_back(std::move(slot));
            entry.mode = StageMode::CausalEquivalent;
            entry.note = std::move(note);
            plan_.push_back(std::move(entry));
            continue;
        }
        std::string stageError;
        auto stage = makeChainStage(spec, sampleRate, numChannels, stageError);
        if (!stage) {
            entry.note = stageError.empty() ? std::string("cannot run at this sample rate") : stageError;
            plan_.push_back(std::move(entry));
            continue;
        }
        if (!stage->isCausalInPlace()) {
            entry.note = "needs the whole track or a zero-phase pass - heard in the render only";
            plan_.push_back(std::move(entry));
            continue;
        }
        if (spec.effect == "Gain") predictedGainDb_ += paramOr(spec, "gain_db", 0.0, -36.0, 24.0);
        Slot slot;
        slot.stage = std::move(stage);
        slots_.push_back(std::move(slot));
        entry.mode = StageMode::Exact;
        plan_.push_back(std::move(entry));
    }

    scratch_.ch.assign(static_cast<size_t>(numChannels), {});
    for (auto& c : scratch_.ch) c.reserve(static_cast<size_t>(maxBlockSize));
    liveBuf_.assign(static_cast<size_t>(numChannels), std::vector<float>(static_cast<size_t>(maxBlockSize), 0.0f));
    livePtrs_.assign(static_cast<size_t>(numChannels), nullptr);
    for (size_t c = 0; c < liveBuf_.size(); ++c) livePtrs_[c] = liveBuf_[c].data();
    resetMatch();
    return true;
}

void RealtimeChain::resetMatch() noexcept
{
    inPower_ = outPower_ = 0.0;
    matchPrimed_ = false;
    gain_ = match_ ? dbToGain(-predictedGainDb_) : 1.0f;
}

void RealtimeChain::adoptMatchState(const RealtimeChain& previous) noexcept
{
    if (!match_ || !previous.match_) return;
    // Continue from the gain the previous chain reached, moved by the change
    // in static gain the two chains predict (a Gain knob turned by +3 dB
    // starts 3 dB lower), and keep its input average so it does not re-learn.
    const float carried = previous.gain_ * dbToGain(previous.predictedGainDb_ - predictedGainDb_);
    gain_ = std::clamp(carried, dbToGain(-kMaxMatchDb), dbToGain(kMaxMatchDb));
    if (previous.matchPrimed_) {
        inPower_ = previous.inPower_;
        outPower_ = inPower_ / std::max(1.0e-12, static_cast<double>(gain_) * gain_);
        matchPrimed_ = true;
    }
}

float RealtimeChain::matchGainDb() const noexcept
{
    return 20.0f * std::log10(std::max(gain_, 1.0e-6f));
}

void RealtimeChain::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (slots_.empty() || numSamples <= 0 || numChannels <= 0 || maxBlock_ <= 0) return;
    for (int offset = 0; offset < numSamples; offset += maxBlock_)
        processChunk(channels, numChannels, offset, std::min(maxBlock_, numSamples - offset));
}

void RealtimeChain::processChunk(float* const* channels, int numChannels, int offset, int n) noexcept
{
    const int used = std::min(numChannels, numChannels_);
    const auto un = static_cast<size_t>(n);
    double inSum = 0.0;
    for (int c = 0; c < numChannels_; ++c) {
        auto& dst = scratch_.ch[static_cast<size_t>(c)];
        dst.resize(un); // within the reserved capacity: no allocation
        // A mono output feeds every prepared channel from its one channel.
        const float* src = channels[std::min(c, used - 1)] + offset;
        for (size_t i = 0; i < un; ++i) dst[i] = static_cast<double>(src[i]);
    }
    for (int c = 0; c < used; ++c)
        for (double v : scratch_.ch[static_cast<size_t>(c)]) inSum += v * v;

    for (auto& slot : slots_) {
        if (slot.stage) {
            slot.stage->push(scratch_);
            continue;
        }
        for (int c = 0; c < numChannels_; ++c) {
            const auto& s = scratch_.ch[static_cast<size_t>(c)];
            float* f = livePtrs_[static_cast<size_t>(c)];
            for (size_t i = 0; i < un; ++i) f[i] = static_cast<float>(s[i]);
        }
        slot.live->process(livePtrs_.data(), numChannels_, n);
        for (int c = 0; c < numChannels_; ++c) {
            auto& s = scratch_.ch[static_cast<size_t>(c)];
            const float* f = livePtrs_[static_cast<size_t>(c)];
            for (size_t i = 0; i < un; ++i) s[i] = static_cast<double>(f[i]);
        }
    }

    float target = gain_;
    if (match_) {
        double outSum = 0.0;
        for (int c = 0; c < used; ++c)
            for (double v : scratch_.ch[static_cast<size_t>(c)]) outSum += v * v;
        const double denom = static_cast<double>(n) * used;
        const double inPow = inSum / denom, outPow = outSum / denom;
        if (inPow > kSilencePower) {
            const double a = std::exp(-static_cast<double>(n) / (kMatchSeconds * sampleRate_));
            if (!matchPrimed_) {
                // First signal: start the averages from the prediction.
                inPower_ = inPow;
                outPower_ = inPow / std::max(1.0e-12, static_cast<double>(gain_) * gain_);
                matchPrimed_ = true;
            }
            inPower_ = a * inPower_ + (1.0 - a) * inPow;
            outPower_ = a * outPower_ + (1.0 - a) * outPow;
            if (outPower_ > 1.0e-12)
                target = std::clamp(static_cast<float>(std::sqrt(inPower_ / outPower_)), dbToGain(-kMaxMatchDb),
                                    dbToGain(kMaxMatchDb));
        }
    }
    // Ramp to the new gain across the chunk (no zipper step).
    const float start = gain_;
    const float step = (target - start) / static_cast<float>(n);
    for (int c = 0; c < used; ++c) {
        const auto& s = scratch_.ch[static_cast<size_t>(c)];
        float* out = channels[c] + offset;
        if (!match_) {
            for (size_t i = 0; i < un; ++i) out[i] = static_cast<float>(s[i]);
            continue;
        }
        for (size_t i = 0; i < un; ++i)
            out[i] = static_cast<float>(s[i]) * (start + step * static_cast<float>(i + 1));
    }
    gain_ = target;
}

} // namespace khensu::dsp
