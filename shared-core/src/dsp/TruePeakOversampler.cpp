// OVH-9: the N-times true-peak interpolator - see TruePeakOversampler.h.
#include <khensu/dsp/TruePeakOversampler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>

namespace khensu::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKaiserBeta = 9.0;

double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}

OversamplerPhases build(int oversample)
{
    // firwin(64 N + 1, 1/N, window=("kaiser", 9.0)), scale=True, then
    // resample_poly's h *= N; phase p, tap j as in TruePeakFilter.cpp.
    OversamplerPhases f;
    f.oversample = oversample;
    const int taps = 64 * oversample + 1;
    const double cutoff = 1.0 / static_cast<double>(oversample);
    std::vector<double> h(static_cast<size_t>(taps));
    double sum = 0.0;
    const double m0 = (taps - 1) * 0.5;
    const double i0b = besselI0(kKaiserBeta);
    for (int i = 0; i < taps; ++i) {
        const double m = static_cast<double>(i) - m0;
        const double xs = cutoff * m;
        const double sinc = std::abs(xs) < 1e-15 ? 1.0 : std::sin(kPi * xs) / (kPi * xs);
        const double r = 2.0 * static_cast<double>(i) / static_cast<double>(taps - 1) - 1.0;
        const double win = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[static_cast<size_t>(i)] = cutoff * sinc * win;
        sum += h[static_cast<size_t>(i)];
    }
    for (double& v : h) v = v / sum * static_cast<double>(oversample);
    f.w.assign(static_cast<size_t>(oversample * kOversamplePhaseTaps), 0.0);
    for (int p = 0; p < oversample; ++p)
        for (int j = 0; j < kOversamplePhaseTaps; ++j) {
            const int k = oversample * (kOversamplePhaseTaps - 1 - j) + p;
            f.w[static_cast<size_t>(p * kOversamplePhaseTaps + j)] = k < taps ? h[static_cast<size_t>(k)] : 0.0;
        }
    return f;
}

} // namespace

const OversamplerPhases& oversamplerPhases(int oversample)
{
    static std::mutex mutex;
    static std::array<std::unique_ptr<OversamplerPhases>, 17> cache;
    const int n = std::clamp(oversample, 2, 16);
    std::lock_guard<std::mutex> lock(mutex);
    auto& slot = cache[static_cast<size_t>(n)];
    if (!slot) slot = std::make_unique<OversamplerPhases>(build(n));
    return *slot;
}

void OversampledEnvelope::prepare(int numChannels, int oversample)
{
    phases_ = &oversamplerPhases(oversample);
    numChannels_ = std::max(1, numChannels);
    history_.assign(static_cast<size_t>(numChannels_ * 2 * kOversamplePhaseTaps), 0.0);
    zeros_.assign(static_cast<size_t>(numChannels_), 0.0);
    pos_ = 0;
    pushed_ = 0;
}

double OversampledEnvelope::evaluate() const noexcept
{
    // The 65 newest samples, oldest first, start at pos_ in each channel's
    // doubled ring; window[j] = x[n - 32 + j].
    double peak = 0.0;
    const int os = phases_->oversample;
    for (int c = 0; c < numChannels_; ++c) {
        const double* win = history_.data() + static_cast<size_t>(c * 2 * kOversamplePhaseTaps + pos_);
        for (int p = 0; p < os; ++p) {
            const double* w = phases_->w.data() + static_cast<size_t>(p * kOversamplePhaseTaps);
            double acc = 0.0;
            for (int j = 0; j < kOversamplePhaseTaps; ++j) acc += w[j] * win[j];
            peak = std::max(peak, std::abs(acc));
        }
    }
    return peak;
}

bool OversampledEnvelope::push(const double* frame, double& value) noexcept
{
    for (int c = 0; c < numChannels_; ++c) {
        double* ring = history_.data() + static_cast<size_t>(c * 2 * kOversamplePhaseTaps);
        ring[pos_] = frame[c];
        ring[pos_ + kOversamplePhaseTaps] = frame[c];
    }
    pos_ = (pos_ + 1) % kOversamplePhaseTaps;
    ++pushed_;
    if (pushed_ <= kOversampleHalf) return false;
    value = evaluate();
    return true;
}

bool OversampledEnvelope::pushZero(double& value) noexcept
{
    return push(zeros_.data(), value);
}

void StreamingTruePeak::prepare(int numChannels, int oversample)
{
    env_.prepare(numChannels, oversample);
    frame_.assign(static_cast<size_t>(std::max(1, numChannels)), 0.0);
    peak_ = 0.0;
}

void StreamingTruePeak::push(const double* const* channels, int numChannels, int numSamples) noexcept
{
    double v = 0.0;
    const int nch = std::min(numChannels, static_cast<int>(frame_.size()));
    for (int i = 0; i < numSamples; ++i) {
        for (int c = 0; c < nch; ++c) {
            frame_[static_cast<size_t>(c)] = channels[c][i];
            peak_ = std::max(peak_, std::abs(channels[c][i]));
        }
        if (env_.push(frame_.data(), v)) peak_ = std::max(peak_, v);
    }
}

double StreamingTruePeak::finishLinear() noexcept
{
    double v = 0.0;
    for (int i = env_.tailFrames(); i > 0; --i)
        if (env_.pushZero(v)) peak_ = std::max(peak_, v);
    return peak_;
}

} // namespace khensu::dsp
