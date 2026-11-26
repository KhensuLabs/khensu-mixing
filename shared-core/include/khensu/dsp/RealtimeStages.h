// shared-core/include/khensu/dsp/RealtimeStages.h
#pragma once
// STU-10: causal, zero-latency, block-wise versions of the chain stages the
// Copilot plugin's built-in chain lacked, for the audio thread. The desktop
// can run the same classes for a live preview later. Every class here:
//   * allocates only in prepare() (fixed members, at most kMaxChannels = 2);
//   * process() is noexcept, allocation-free and adds no latency;
//   * takes its parameters per control block from the caller (who smooths
//     them); a setter never allocates.
// Against the offline renderer (ChainProcessor / ExtendedChainStages), whose
// zero-phase stages need the whole file:
//   RealtimeEqBand   PeakFilter / LowShelfFilter / HighShelfFilter with the
//                    same RBJ designs (juce::dsp::IIR::ArrayCoefficients, which
//                    pedalboard runs too), plus a notch and a 12 dB/oct high-
//                    or low-pass; a channel mode (stereo / mid / side, as the
//                    MidSideProcessor's bells); and a dynamic mode - a causal,
//                    self-keyed DynamicEQ: the band's own signal (a band-pass
//                    at the bell, the shelf's side of a 2nd-order split) in a
//                    peak follower moves the band's gain down by up to `range`
//                    dB above `threshold` at kDynamicRatio:1 (Dynamics::ratio
//                    overrides it). The gain is
//                    re-designed every kControlSamples (the plugin's control
//                    block), so attack is no faster than that (~0.7 ms at
//                    48 kHz). A band in stereo, static mode runs exactly the
//                    filter the plugin's v4 bands ran (one juce::dsp IIR
//                    filter per channel, same coefficients), so an old
//                    session sounds bit-for-bit the same.
//   BassMono         BassMonoMaker, causally: the side's LR4 (2 x Butterworth)
//                    high band is kept, its low band dropped; the mid runs
//                    through the matching 2nd-order all-pass (LR4 low + high
//                    = that all-pass), so the image above the crossover keeps
//                    its phase.
//   StereoWidth      StereoWidener: side x width, ramped per sample.
//   Saturation       TapeSaturation's curve ((1 - warmth) tanh(d x) + warmth
//                    (tanh(d x + 0.25) - tanh(0.25))), its 5 Hz DC blocker,
//                    mix and output trim - without the 4x FIR oversampling,
//                    which adds latency: first-order antiderivative anti-
//                    aliasing instead (half a sample of delay on the wet path).
//   TransientShaper  TransientShaper: the same linked detector (mean |x|), a
//                    1 ms / 10 ms and an attack / release follower, gain =
//                    sustain + (attack - sustain) clip((fast - slow) / slow).
// THREAD_CONTEXT(AudioThread) for process(); prepare() on the message thread.
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace khensu::dsp {

inline constexpr int kRealtimeMaxChannels = 2;

/// One EQ band (see the header comment).
class RealtimeEqBand {
  public:
    enum class Type { Bell = 0, LowShelf = 1, HighShelf = 2, Notch = 3, HighPass = 4, LowPass = 5 };
    enum class Channels { Stereo = 0, Mid = 1, Side = 2 };
    struct Dynamics {
        bool on = false;
        float thresholdDb = -30.0f;
        float rangeDb = 6.0f;
        float attackMs = 5.0f;
        float releaseMs = 120.0f;
        /// STU-18: the compression ratio above the threshold (the plugin's
        /// bands keep kDynamicRatio; the desktop's preview of a DynamicEQ /
        /// DeEsser passes the stage's own ratio).
        float ratio = 4.0f;
    };

    static constexpr int kControlSamples = 32;
    static constexpr float kDynamicRatio = 4.0f;

    /// Allocates the filters' coefficient objects and state.
    void prepare(double sampleRate, int numChannels)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        numChannels_ = std::clamp(numChannels, 1, kRealtimeMaxChannels);
        const juce::dsp::ProcessSpec spec{sampleRate_, static_cast<juce::uint32>(kControlSamples), 1};
        const auto design = designFor(type_, freq_, q_, gainDb_);
        for (auto* f : {&filters_[0], &filters_[1], &detectors_[0], &detectors_[1]}) {
            // A 2nd-order object from the start: assigning 6 values later then
            // never changes the order (which would need a reset) nor grows
            // storage (assignImpl reserves 8 slots).
            f->coefficients = new juce::dsp::IIR::Coefficients<float>(design);
            f->prepare(spec);
        }
        applyDesign(design);
        updateDetector();
        updateBallistics();
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : filters_) f.reset();
        for (auto& d : detectors_) d.reset();
        env_ = 0.0f;
        reductionDb_ = 0.0f;
        appliedReductionDb_ = 0.0f;
    }

    /// The static band. Re-designs the coefficients when anything changed
    /// (or `force`); a channel-mode change also clears the state.
    void setBand(Type type, Channels channels, float freqHz, float q, float gainDb, bool force = false) noexcept
    {
        if (channels != channels_) {
            channels_ = channels;
            reset();
            force = true;
        }
        if (!force && type == type_ && freqHz == freq_ && q == q_ && gainDb == gainDb_) return;
        const bool detectorMoved = force || type != type_ || freqHz != freq_ || q != q_;
        type_ = type;
        freq_ = freqHz;
        q_ = q;
        gainDb_ = gainDb;
        applyDesign(designFor(type_, freq_, q_, gainDb_ - appliedReductionDb_));
        if (detectorMoved) updateDetector();
    }

    void setDynamics(const Dynamics& dynamics) noexcept
    {
        const bool timesMoved = dynamics.attackMs != dyn_.attackMs || dynamics.releaseMs != dyn_.releaseMs;
        const bool wasOn = dynamicActive();
        dyn_ = dynamics;
        if (timesMoved) updateBallistics();
        if (wasOn && !dynamicActive() && appliedReductionDb_ != 0.0f) {
            // Back to the static band at once.
            appliedReductionDb_ = 0.0f;
            reductionDb_ = 0.0f;
            applyDesign(designFor(type_, freq_, q_, gainDb_));
        }
        if (!wasOn && dynamicActive()) env_ = 0.0f;
    }

    /// True when the dynamic mode applies (a bell or a shelf with dynamics on).
    bool dynamicActive() const noexcept { return dyn_.on && type_ <= Type::HighShelf; }

    /// The gain reduction (dB, >= 0) the dynamic mode applied last.
    float currentReductionDb() const noexcept { return appliedReductionDb_; }

    /// Filters `numChannels` channels of `numSamples` in place. Mid / side
    /// modes need two channels; on one channel they act as stereo.
    void process(float* const* channels, int numChannels, int numSamples) noexcept
    {
        const int nch = std::min(numChannels, numChannels_);
        if (nch <= 0 || numSamples <= 0) return;
        const bool ms = channels_ != Channels::Stereo && nch >= 2;
        if (!dynamicActive() && !ms) {
            // The v4 path: one juce IIR filter per channel over the block.
            for (int c = 0; c < nch; ++c) runFilter(filters_[static_cast<size_t>(c)], channels[c], numSamples);
            return;
        }
        for (int start = 0; start < numSamples; start += kControlSamples) {
            const int n = std::min(kControlSamples, numSamples - start);
            if (ms) {
                float* l = channels[0] + start;
                float* r = channels[1] + start;
                auto& path = channels_ == Channels::Mid ? mid_ : side_;
                for (int i = 0; i < n; ++i) {
                    const auto k = static_cast<size_t>(i);
                    mid_[k] = (l[i] + r[i]) * 0.5f;
                    side_[k] = (l[i] - r[i]) * 0.5f;
                }
                if (dynamicActive()) {
                    const float* key = path.data();
                    updateDynamicGain(&key, 1, n);
                }
                runFilter(filters_[0], path.data(), n);
                for (int i = 0; i < n; ++i) {
                    const auto k = static_cast<size_t>(i);
                    l[i] = mid_[k] + side_[k];
                    r[i] = mid_[k] - side_[k];
                }
            } else {
                std::array<const float*, kRealtimeMaxChannels> key{};
                std::array<float*, kRealtimeMaxChannels> out{};
                for (int c = 0; c < nch; ++c) {
                    out[static_cast<size_t>(c)] = channels[c] + start;
                    key[static_cast<size_t>(c)] = out[static_cast<size_t>(c)];
                }
                updateDynamicGain(key.data(), nch, n);
                for (int c = 0; c < nch; ++c) runFilter(filters_[static_cast<size_t>(c)], out[static_cast<size_t>(c)], n);
            }
        }
    }

    /// RBJ coefficients of `type` ([b0, b1, b2, a0, a1, a2]); frequency
    /// clamped to [10 Hz, 0.45 x the rate], q to >= 0.05.
    std::array<float, 6> designFor(Type type, float freqHz, float q, float gainDb) const noexcept
    {
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        const float f = std::clamp(freqHz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
        const float qq = std::max(0.05f, q);
        const float gain = juce::Decibels::decibelsToGain(gainDb);
        switch (type) {
        case Type::LowShelf: return AC::makeLowShelf(sampleRate_, f, qq, gain);
        case Type::HighShelf: return AC::makeHighShelf(sampleRate_, f, qq, gain);
        case Type::Notch: return AC::makeNotch(sampleRate_, f, qq);
        case Type::HighPass: return AC::makeHighPass(sampleRate_, f, qq);
        case Type::LowPass: return AC::makeLowPass(sampleRate_, f, qq);
        case Type::Bell: break;
        }
        return AC::makePeakFilter(sampleRate_, f, qq, gain);
    }

  private:
    static void runFilter(juce::dsp::IIR::Filter<float>& filter, float* data, int n) noexcept
    {
        float* channels[] = {data};
        juce::dsp::AudioBlock<float> block(channels, 1, static_cast<size_t>(n));
        juce::dsp::ProcessContextReplacing<float> context(block);
        filter.process(context);
    }

    void applyDesign(const std::array<float, 6>& design) noexcept
    {
        for (auto& f : filters_) *f.coefficients = design;
    }

    void updateDetector() noexcept
    {
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        const float f = std::clamp(freq_, 10.0f, static_cast<float>(sampleRate_ * 0.45));
        std::array<float, 6> design{};
        if (type_ == Type::LowShelf)
            design = AC::makeLowPass(sampleRate_, f, 0.70710678f);
        else if (type_ == Type::HighShelf)
            design = AC::makeHighPass(sampleRate_, f, 0.70710678f);
        else
            design = AC::makeBandPass(sampleRate_, f, std::max(0.05f, q_));
        for (auto& d : detectors_) *d.coefficients = design;
    }

    void updateBallistics() noexcept
    {
        const auto coeff = [this](float ms) {
            const double seconds = std::max(0.01, static_cast<double>(ms)) * 0.001;
            return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate_)));
        };
        attack_ = coeff(dyn_.attackMs);
        release_ = coeff(dyn_.releaseMs);
    }

    /// Runs the detector over the key, follows its peak and re-designs the
    /// band at its reduced gain when the reduction moved.
    void updateDynamicGain(const float* const* key, int nch, int n) noexcept
    {
        for (int c = 0; c < nch; ++c) {
            auto& scratch = detectScratch_[static_cast<size_t>(c)];
            std::copy(key[c], key[c] + n, scratch.begin());
            runFilter(detectors_[static_cast<size_t>(c)], scratch.data(), n);
        }
        for (int i = 0; i < n; ++i) {
            float peak = 0.0f;
            for (int c = 0; c < nch; ++c)
                peak = std::max(peak, std::abs(detectScratch_[static_cast<size_t>(c)][static_cast<size_t>(i)]));
            env_ += (peak > env_ ? attack_ : release_) * (peak - env_);
        }
        if (!std::isfinite(env_)) env_ = 0.0f;
        const float levelDb = 20.0f * std::log10(env_ + 1.0e-9f);
        const float slope = 1.0f - 1.0f / std::max(1.0f, dyn_.ratio);
        reductionDb_ = std::clamp((levelDb - dyn_.thresholdDb) * slope, 0.0f, std::max(0.0f, dyn_.rangeDb));
        if (std::abs(reductionDb_ - appliedReductionDb_) > 0.01f) {
            appliedReductionDb_ = reductionDb_;
            applyDesign(designFor(type_, freq_, q_, gainDb_ - appliedReductionDb_));
        }
    }

    double sampleRate_ = 44100.0;
    int numChannels_ = 2;
    Type type_ = Type::Bell;
    Channels channels_ = Channels::Stereo;
    float freq_ = 1000.0f, q_ = 1.0f, gainDb_ = 0.0f;
    Dynamics dyn_;
    float attack_ = 1.0f, release_ = 1.0f, env_ = 0.0f;
    float reductionDb_ = 0.0f, appliedReductionDb_ = 0.0f;
    std::array<juce::dsp::IIR::Filter<float>, kRealtimeMaxChannels> filters_, detectors_;
    std::array<float, kControlSamples> mid_{}, side_{};
    std::array<std::array<float, kControlSamples>, kRealtimeMaxChannels> detectScratch_{};
};

/// Mono below the crossover (see the header comment). Two channels only.
class RealtimeBassMono {
  public:
    /// The BassMonoMaker's default crossover.
    static constexpr float kDefaultCrossoverHz = 120.0f;

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        const juce::dsp::ProcessSpec spec{sampleRate_, static_cast<juce::uint32>(RealtimeEqBand::kControlSamples), 1};
        for (auto* f : {&sideHp1_, &sideHp2_, &midAp_}) {
            f->coefficients = new juce::dsp::IIR::Coefficients<float>(
                juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass(sampleRate_, kDefaultCrossoverHz));
            f->prepare(spec);
        }
        freq_ = -1.0f;
        setCrossover(kDefaultCrossoverHz);
        reset();
    }

    void reset() noexcept
    {
        sideHp1_.reset();
        sideHp2_.reset();
        midAp_.reset();
    }

    void setCrossover(float hz) noexcept
    {
        if (hz == freq_) return;
        freq_ = hz;
        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        const float f = std::clamp(hz, 10.0f, static_cast<float>(sampleRate_ * 0.45));
        constexpr float kButterworthQ = 0.70710678f;
        const auto hp = AC::makeHighPass(sampleRate_, f, kButterworthQ);
        *sideHp1_.coefficients = hp;
        *sideHp2_.coefficients = hp;
        *midAp_.coefficients = AC::makeAllPass(sampleRate_, f, kButterworthQ);
    }

    void process(float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (numChannels < 2) return; // mono is already mono
        for (int start = 0; start < numSamples; start += RealtimeEqBand::kControlSamples) {
            const int n = std::min(RealtimeEqBand::kControlSamples, numSamples - start);
            float* l = channels[0] + start;
            float* r = channels[1] + start;
            for (int i = 0; i < n; ++i) {
                const auto k = static_cast<size_t>(i);
                mid_[k] = (l[i] + r[i]) * 0.5f;
                side_[k] = (l[i] - r[i]) * 0.5f;
            }
            run(midAp_, mid_.data(), n);
            run(sideHp1_, side_.data(), n);
            run(sideHp2_, side_.data(), n);
            for (int i = 0; i < n; ++i) {
                const auto k = static_cast<size_t>(i);
                l[i] = mid_[k] + side_[k];
                r[i] = mid_[k] - side_[k];
            }
        }
    }

  private:
    static void run(juce::dsp::IIR::Filter<float>& filter, float* data, int n) noexcept
    {
        float* channels[] = {data};
        juce::dsp::AudioBlock<float> block(channels, 1, static_cast<size_t>(n));
        juce::dsp::ProcessContextReplacing<float> context(block);
        filter.process(context);
    }

    double sampleRate_ = 44100.0;
    float freq_ = -1.0f;
    juce::dsp::IIR::Filter<float> sideHp1_, sideHp2_, midAp_;
    std::array<float, RealtimeEqBand::kControlSamples> mid_{}, side_{};
};

/// Side x width, ramped per sample from the last call's width. Two channels only.
class RealtimeStereoWidth {
  public:
    void reset(float width) noexcept { current_ = width; }

    void process(float* const* channels, int numChannels, int numSamples, float width) noexcept
    {
        if (numChannels < 2 || numSamples <= 0) {
            current_ = width;
            return;
        }
        const float step = (width - current_) / static_cast<float>(numSamples);
        float w = current_;
        float* l = channels[0];
        float* r = channels[1];
        for (int i = 0; i < numSamples; ++i) {
            w += step;
            const float mid = (l[i] + r[i]) * 0.5f;
            const float side = (l[i] - r[i]) * 0.5f * w;
            l[i] = mid + side;
            r[i] = mid - side;
        }
        current_ = width;
    }

  private:
    float current_ = 1.0f;
};

/// TapeSaturation without oversampling (see the header comment).
class RealtimeSaturation {
  public:
    struct Settings {
        float driveDb = 6.0f;
        float warmth = 0.5f;
        float mix = 1.0f;
        float trimDb = 0.0f;
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        dcPole_ = std::exp(-2.0 * juce::MathConstants<double>::pi * 5.0 / sampleRate_);
        reset();
    }

    void reset() noexcept
    {
        u1_.fill(0.0);
        dcIn1_.fill(0.0);
        dcOut1_.fill(0.0);
    }

    void process(float* const* channels, int numChannels, int numSamples, const Settings& s) noexcept
    {
        const int nch = std::min(numChannels, kRealtimeMaxChannels);
        const double drive = std::pow(10.0, static_cast<double>(s.driveDb) / 20.0);
        const double warmth = std::clamp(static_cast<double>(s.warmth), 0.0, 1.0);
        const double mix = std::clamp(static_cast<double>(s.mix), 0.0, 1.0);
        const double trim = std::pow(10.0, static_cast<double>(s.trimDb) / 20.0);
        for (int c = 0; c < nch; ++c) {
            const auto k = static_cast<size_t>(c);
            float* x = channels[c];
            double u1 = u1_[k], in1 = dcIn1_[k], out1 = dcOut1_[k];
            for (int i = 0; i < numSamples; ++i) {
                const double dry = static_cast<double>(x[i]);
                const double u = drive * dry;
                const double du = u - u1;
                const double shaped = std::abs(du) > 1.0e-6 ? (antiderivative(u, warmth) - antiderivative(u1, warmth)) / du
                                                            : curve(0.5 * (u + u1), warmth);
                u1 = u;
                const double dc = shaped - in1 + dcPole_ * out1;
                in1 = shaped;
                out1 = dc;
                x[i] = static_cast<float>(dry * (1.0 - mix) + dc * trim * mix);
            }
            // A non-finite input (contained by the caller) must not stick here.
            u1_[k] = std::isfinite(u1) ? u1 : 0.0;
            dcIn1_[k] = std::isfinite(in1) ? in1 : 0.0;
            dcOut1_[k] = std::isfinite(out1) ? out1 : 0.0;
        }
    }

    /// The static curve (TapeSaturator's shaper).
    static double curve(double u, double warmth) noexcept
    {
        return (1.0 - warmth) * std::tanh(u) + warmth * (std::tanh(u + kBias) - std::tanh(kBias));
    }

  private:
    static constexpr double kBias = 0.25;

    /// log(cosh(u)), stable for large |u|.
    static double logCosh(double u) noexcept
    {
        const double a = std::abs(u);
        return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994530942;
    }

    /// The curve's antiderivative.
    static double antiderivative(double u, double warmth) noexcept
    {
        return (1.0 - warmth) * logCosh(u) + warmth * (logCosh(u + kBias) - std::tanh(kBias) * u);
    }

    double sampleRate_ = 44100.0, dcPole_ = 0.0;
    std::array<double, kRealtimeMaxChannels> u1_{}, dcIn1_{}, dcOut1_{};
};

/// TransientShaper (see the header comment).
class RealtimeTransientShaper {
  public:
    struct Settings {
        float attackGainDb = 0.0f;
        float sustainGainDb = 0.0f;
        float attackMs = 3.0f;
        float releaseMs = 200.0f;
    };

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        fastAtk_ = std::exp(-1.0 / (0.001 * sampleRate_));
        fastRel_ = std::exp(-1.0 / (0.010 * sampleRate_));
        reset();
    }

    void reset() noexcept { fast_ = slow_ = 0.0; }

    void process(float* const* channels, int numChannels, int numSamples, const Settings& s) noexcept
    {
        const int nch = std::min(numChannels, kRealtimeMaxChannels);
        if (nch <= 0) return;
        const auto coeff = [this](float ms) {
            return std::exp(-1.0 / (std::max(1.0, static_cast<double>(ms)) * 0.001 * sampleRate_));
        };
        const double slowAtk = coeff(s.attackMs), slowRel = coeff(s.releaseMs);
        const double attack = std::pow(10.0, static_cast<double>(s.attackGainDb) / 20.0);
        const double sustain = std::pow(10.0, static_cast<double>(s.sustainGainDb) / 20.0);
        const double inv = 1.0 / static_cast<double>(nch);
        for (int i = 0; i < numSamples; ++i) {
            double level = 0.0;
            for (int c = 0; c < nch; ++c) level += std::abs(static_cast<double>(channels[c][i]));
            level *= inv;
            fast_ = level > fast_ ? fastAtk_ * fast_ + (1.0 - fastAtk_) * level : fastRel_ * fast_ + (1.0 - fastRel_) * level;
            slow_ = level > slow_ ? slowAtk * slow_ + (1.0 - slowAtk) * level : slowRel * slow_ + (1.0 - slowRel) * level;
            const double amount = std::clamp((fast_ - slow_) / std::max(slow_, 1e-6), 0.0, 1.0);
            const auto gain = static_cast<float>(sustain + (attack - sustain) * amount);
            for (int c = 0; c < nch; ++c) channels[c][i] *= gain;
        }
        if (!std::isfinite(fast_) || !std::isfinite(slow_)) reset();
    }

  private:
    double sampleRate_ = 44100.0, fastAtk_ = 0.0, fastRel_ = 0.0, fast_ = 0.0, slow_ = 0.0;
};

} // namespace khensu::dsp
