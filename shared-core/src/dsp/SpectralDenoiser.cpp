// FN2-2a: SpectralDenoise as a native chain stage - see SpectralDenoiser.h.
// Mirrors dsp_dynamics._apply_spectral_denoise; change the two together
// (tests/shared-core/test_offline_chain_renderer.cpp pins them through
// fixtures written by scripts/gen_render_parity_fixtures.py).
#include <khensu/dsp/SpectralDenoiser.h>
#include <khensu/dsp/RealFft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace khensu::dsp {

using namespace denoise;

// ---- PercentileProfile ---------------------------------------------------------

PercentileProfile::PercentileProfile(int columns, int exactRowLimit)
    : columns_(columns), limit_(std::max(1, exactRowLimit)),
      buckets_(static_cast<int>(std::lround((kMaxDb - kMinDb) / kStepDb)))
{
}

void PercentileProfile::addToHistogram(int column, double value)
{
    int bucket = 0;
    if (value > 0.0) {
        const double db = 10.0 * std::log10(value);
        bucket = std::clamp(static_cast<int>(std::floor((db - kMinDb) / kStepDb)), 0, buckets_ - 1);
    }
    ++hist_[static_cast<size_t>(column) * static_cast<size_t>(buckets_) + static_cast<size_t>(bucket)];
}

void PercentileProfile::toHistogram()
{
    hist_.assign(static_cast<size_t>(columns_) * static_cast<size_t>(buckets_), 0u);
    const auto cols = static_cast<size_t>(columns_);
    for (size_t i = 0; i < values_.size(); ++i)
        addToHistogram(static_cast<int>(i % cols), static_cast<double>(values_[i]));
    values_.clear();
    values_.shrink_to_fit();
}

void PercentileProfile::add(const double* row)
{
    if (exact() && rows_ >= limit_) toHistogram();
    if (exact())
        for (int c = 0; c < columns_; ++c) values_.push_back(static_cast<float>(row[c]));
    else
        for (int c = 0; c < columns_; ++c) addToHistogram(c, row[c]);
    ++rows_;
}

std::vector<double> PercentileProfile::percentile(double p) const
{
    std::vector<double> out(static_cast<size_t>(columns_), 0.0);
    if (rows_ == 0) return out;
    const double rank = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(rows_ - 1);
    const auto cols = static_cast<size_t>(columns_);
    if (exact()) {
        const auto lo = static_cast<std::int64_t>(std::floor(rank));
        const std::int64_t hi = std::min(lo + 1, rows_ - 1);
        const double frac = rank - static_cast<double>(lo);
        std::vector<double> col(static_cast<size_t>(rows_));
        for (size_t c = 0; c < cols; ++c) {
            for (size_t r = 0; r < col.size(); ++r) col[r] = static_cast<double>(values_[r * cols + c]);
            const auto loIt = col.begin() + static_cast<std::ptrdiff_t>(lo);
            std::nth_element(col.begin(), loIt, col.end());
            const double a = *loIt;
            const double b = hi == lo ? a : *std::min_element(loIt + 1, col.end());
            out[c] = a + (b - a) * frac;
        }
        return out;
    }
    for (size_t c = 0; c < cols; ++c) {
        const auto* h = hist_.data() + c * static_cast<size_t>(buckets_);
        double before = 0.0;
        for (int b = 0; b < buckets_; ++b) {
            const auto count = static_cast<double>(h[b]);
            if (count > 0.0 && before + count > rank) {
                if (b == 0) break; // silence (or below -200 dB): 0
                const double within = std::clamp((rank - before + 0.5) / count, 0.0, 1.0);
                out[c] = std::pow(10.0, (kMinDb + kStepDb * (static_cast<double>(b) + within)) / 10.0);
                break;
            }
            before += count;
        }
    }
    return out;
}

// ---- the stage -----------------------------------------------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;

/// One channel's padded stream, cut into the STFT's frames. Frame f covers
/// padded samples [f kHop, f kHop + kFftSize); samples before the next
/// frame's start are dropped once its predecessor has been handed out.
class Framer {
  public:
    Framer() { appendZeros(kFftSize); }
    void append(const double* x, size_t n) { buf_.insert(buf_.end(), x, x + n); }
    void appendZeros(size_t n) { buf_.insert(buf_.end(), n, 0.0); }
    /// Calls onFrame(frameIndex, samples) for every complete frame.
    template <typename F> void frames(F&& onFrame)
    {
        while (next_ * kHop + kFftSize <= start_ + static_cast<std::int64_t>(buf_.size())) {
            onFrame(next_, buf_.data() + static_cast<std::ptrdiff_t>(next_ * kHop - start_));
            ++next_;
        }
        const std::int64_t keep = next_ * kHop; // the next frame's first sample
        if (keep - start_ >= 16 * kHop) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(keep - start_));
            start_ = keep;
        }
    }

  private:
    std::vector<double> buf_;
    std::int64_t start_ = 0; ///< padded index of buf_[0]
    std::int64_t next_ = 0;  ///< the next frame to hand out
};

class SpectralDenoiseStage final : public ChainStage {
  public:
    SpectralDenoiseStage(double reductionDb, double sensitivity, int numChannels, int exactProfileFrames)
        : numChannels_(numChannels), sensitivity_(sensitivity),
          floorPow_(std::pow(10.0, -std::abs(reductionDb) / 10.0)), exactFrames_(exactProfileFrames), fft_(kFftSize)
    {
        window_.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) // np.hanning(n + 1)[:-1]: periodic
            window_[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kFftSize);
        frame_.resize(kFftSize);
        out_.resize(kFftSize);
        for (int c = 0; c < numChannels; ++c) {
            profileFramers_.emplace_back();
            profiles_.emplace_back(kBins, exactFrames_);
        }
    }

    bool needsProfile() const override { return true; }

    void profile(const PlanarBlock& in) override
    {
        if (in.numSamples() == 0) return;
        profiledSamples_ += in.numSamples();
        for (size_t c = 0; c < profileFramers_.size() && c < in.ch.size(); ++c) {
            profileFramers_[c].append(in.ch[c].data(), in.ch[c].size());
            profileFrames(c);
        }
    }

    void endProfile() override
    {
        for (size_t c = 0; c < profileFramers_.size(); ++c) {
            profileFramers_[c].appendZeros(kFftSize + kHop);
            profileFrames(c);
        }
        const double scale = -std::log1p(-kProfilePercentile / 100.0);
        noise_.clear();
        for (auto& prof : profiles_) {
            auto n = prof.percentile(kProfilePercentile);
            for (auto& v : n) v /= scale;
            noise_.push_back(std::move(n));
        }
        profiles_.clear();
        profileFramers_.clear();
        profiled_ = true;
        startRender();
    }

    void adoptProfile(const ChainStage& other) override
    {
        const auto* o = dynamic_cast<const SpectralDenoiseStage*>(&other);
        if (o == nullptr || !o->profiled_) return;
        noise_ = o->noise_;
        profiledSamples_ = o->profiledSamples_;
        profiles_.clear();
        profileFramers_.clear();
        profiled_ = true;
        startRender();
    }

    void push(PlanarBlock& io) override
    {
        if (bypass()) return;
        const int n = io.numSamples();
        PlanarBlock out;
        out.ch.assign(static_cast<size_t>(numChannels_), {});
        for (size_t c = 0; c < chans_.size() && c < io.ch.size(); ++c) {
            chans_[c].framer.append(io.ch[c].data(), static_cast<size_t>(n));
            renderFrames(c, out.ch[c], false);
        }
        io = std::move(out);
    }

    void finish(PlanarBlock& tail) override
    {
        tail.ch.assign(static_cast<size_t>(numChannels_), {});
        if (bypass()) return;
        for (size_t c = 0; c < chans_.size(); ++c) {
            chans_[c].framer.appendZeros(kFftSize + kHop);
            renderFrames(c, tail.ch[c], true);
        }
    }

  private:
    struct Channel {
        Framer framer;
        std::vector<double> acc, norm; ///< overlap-add from padded index accStart
        std::int64_t accStart = 0;
        std::vector<double> prev; ///< the previous frame's smoothed gain
    };

    /// No profile (a caller that skipped the pass) or a track shorter than
    /// one frame: the service returns those unchanged.
    bool bypass() const noexcept { return !profiled_ || profiledSamples_ < kFftSize; }

    void startRender()
    {
        chans_.clear();
        chans_.resize(static_cast<size_t>(numChannels_));
        for (auto& ch : chans_) ch.prev.assign(kBins, 1.0);
    }

    void profileFrames(size_t c)
    {
        profileFramers_[c].frames([this, c](std::int64_t f, const double* x) {
            if (f % kProfileStride != 0) return;
            for (int i = 0; i < kFftSize; ++i) frame_[static_cast<size_t>(i)] = x[i] * window_[static_cast<size_t>(i)];
            fft_.forward(frame_.data(), spec_);
            power_.resize(kBins);
            for (int k = 0; k < kBins; ++k) {
                const double mag = std::abs(spec_[static_cast<size_t>(k)]);
                power_[static_cast<size_t>(k)] = mag * mag;
            }
            profiles_[c].add(power_.data());
        });
    }

    /// Processes every complete frame of channel `c` and appends the samples
    /// no later frame can change (all that are left when `final`).
    void renderFrames(size_t c, std::vector<double>& dst, bool final)
    {
        auto& ch = chans_[c];
        const auto& noise = noise_[c];
        const std::int64_t signalEnd = kFftSize + profiledSamples_; // padded index past the last signal sample
        std::int64_t lastFrame = -1;
        ch.framer.frames([&](std::int64_t f, const double* x) {
            lastFrame = f;
            for (int i = 0; i < kFftSize; ++i) frame_[static_cast<size_t>(i)] = x[i] * window_[static_cast<size_t>(i)];
            fft_.forward(frame_.data(), spec_);
            gain_.resize(kBins);
            for (int k = 0; k < kBins; ++k) {
                const auto& z = spec_[static_cast<size_t>(k)];
                const double power = z.real() * z.real() + z.imag() * z.imag();
                gain_[static_cast<size_t>(k)] =
                    std::max(1.0 - sensitivity_ * noise[static_cast<size_t>(k)] / std::max(power, 1e-30), floorPow_);
            }
            // np.convolve(gain, [0.25, 0.5, 0.25], mode="same"), then the release.
            smooth_.resize(kBins);
            for (int k = 0; k < kBins; ++k) {
                const auto i = static_cast<size_t>(k);
                const double left = k > 0 ? gain_[i - 1] : 0.0;
                const double right = k + 1 < kBins ? gain_[i + 1] : 0.0;
                double g = 0.25 * left + 0.5 * gain_[i] + 0.25 * right;
                if (g < ch.prev[i]) g = kRelease * ch.prev[i] + (1.0 - kRelease) * g;
                smooth_[i] = g;
            }
            ch.prev = smooth_;
            for (int k = 0; k < kBins; ++k) spec_[static_cast<size_t>(k)] *= std::sqrt(smooth_[static_cast<size_t>(k)]);
            fft_.inverse(spec_, out_.data());
            const std::int64_t s0 = f * kHop;
            const auto need = static_cast<size_t>(s0 + kFftSize - ch.accStart);
            if (ch.acc.size() < need) {
                ch.acc.resize(need, 0.0);
                ch.norm.resize(need, 0.0);
            }
            const auto off = static_cast<size_t>(s0 - ch.accStart);
            for (int i = 0; i < kFftSize; ++i) {
                const auto w = window_[static_cast<size_t>(i)];
                ch.acc[off + static_cast<size_t>(i)] += out_[static_cast<size_t>(i)] * w;
                ch.norm[off + static_cast<size_t>(i)] += w * w;
            }
        });
        // Final up to the next frame's start (or everything, at the end).
        const std::int64_t finalEnd = final ? ch.accStart + static_cast<std::int64_t>(ch.acc.size())
                                            : (lastFrame < 0 ? ch.accStart : (lastFrame + 1) * kHop);
        if (finalEnd <= ch.accStart) return;
        const auto count = static_cast<size_t>(finalEnd - ch.accStart);
        for (size_t i = 0; i < count; ++i) {
            const std::int64_t p = ch.accStart + static_cast<std::int64_t>(i);
            if (p >= kFftSize && p < signalEnd) dst.push_back(ch.acc[i] / std::max(ch.norm[i], 1e-9));
        }
        ch.acc.erase(ch.acc.begin(), ch.acc.begin() + static_cast<std::ptrdiff_t>(count));
        ch.norm.erase(ch.norm.begin(), ch.norm.begin() + static_cast<std::ptrdiff_t>(count));
        ch.accStart = finalEnd;
    }

    int numChannels_;
    double sensitivity_, floorPow_;
    int exactFrames_;
    RealFft fft_;
    std::vector<double> window_, frame_, out_, power_, gain_, smooth_;
    std::vector<std::complex<double>> spec_;
    // Profile pass.
    std::vector<Framer> profileFramers_;
    std::vector<PercentileProfile> profiles_;
    std::int64_t profiledSamples_ = 0;
    bool profiled_ = false;
    // Render pass.
    std::vector<std::vector<double>> noise_; ///< per channel, per bin: mean noise power
    std::vector<Channel> chans_;
};

} // namespace

std::unique_ptr<ChainStage> makeSpectralDenoiseStage(double reductionDb, double sensitivity, int numChannels,
                                                     int exactProfileFrames)
{
    return std::make_unique<SpectralDenoiseStage>(reductionDb, sensitivity, numChannels, exactProfileFrames);
}

} // namespace khensu::dsp
