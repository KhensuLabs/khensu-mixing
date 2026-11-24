// FN2-2a: TapeSaturation as a native chain stage - see TapeSaturator.h.
// Mirrors dsp_color._apply_tape_saturation (and scipy's resample_poly /
// firwin); change them together (tests/shared-core/test_offline_chain_renderer.cpp
// pins them through fixtures written by scripts/gen_render_parity_fixtures.py).
#include <khensu/dsp/TapeSaturator.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace khensu::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 500; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

class TapeSaturationStage final : public ChainStage {
  public:
    static constexpr int kOver = 4;
    static constexpr int kHalf = 10 * kOver; ///< resample_poly: half_len = 10 x max_rate

    TapeSaturationStage(double driveDb, double warmth, double mix, double trimDb, double sampleRate, int numChannels)
        : drive_(std::pow(10.0, driveDb / 20.0)), warmth_(std::clamp(warmth, 0.0, 1.0)), mix_(std::clamp(mix, 0.0, 1.0)),
          trim_(std::pow(10.0, trimDb / 20.0)), dcR_(std::exp(-2.0 * kPi * 5.0 / (kOver * sampleRate))),
          biasTanh_(std::tanh(kBias)), h_(firwinKaiser(2 * kHalf + 1, 1.0 / kOver, 5.0)),
          chans_(static_cast<size_t>(numChannels))
    {
    }

    void push(PlanarBlock& io) override
    {
        const int n = io.numSamples();
        for (size_t c = 0; c < chans_.size() && c < io.ch.size(); ++c)
            chans_[c].x.insert(chans_[c].x.end(), io.ch[c].begin(), io.ch[c].end());
        received_ += n;
        PlanarBlock out;
        out.ch.assign(chans_.size(), {});
        produce(out);
        io = std::move(out);
    }

    void finish(PlanarBlock& tail) override
    {
        tail.ch.assign(chans_.size(), {});
        finished_ = true;
        produce(tail);
    }

  private:
    static constexpr double kBias = 0.25;

    struct Channel {
        std::vector<double> x; ///< input from xStart
        std::int64_t xStart = 0;
        std::vector<double> s; ///< shaped (oversampled) from sStart
        std::int64_t sStart = 0;
        double dcIn = 0.0, dcOut = 0.0;
    };

    double xAt(const Channel& c, std::int64_t j) const
    {
        if (j < 0 || j >= received_) return 0.0;
        return c.x[static_cast<size_t>(j - c.xStart)];
    }
    double sAt(const Channel& c, std::int64_t m) const
    {
        if (m < 0 || m >= kOver * received_) return 0.0; // resample_poly's output is exactly 4n long
        return c.s[static_cast<size_t>(m - c.sStart)];
    }

    void produce(PlanarBlock& out)
    {
        // up[m] reads x up to (m + kHalf) / 4: known once that is received
        // (at the end the rest is zero padding).
        const std::int64_t mLimit = finished_ ? kOver * received_ : kOver * received_ - kHalf;
        for (; mNext_ < mLimit; ++mNext_) {
            const std::int64_t jlo = -floorDiv(-(mNext_ - kHalf), kOver); // ceil
            const std::int64_t jhi = floorDiv(mNext_ + kHalf, kOver);
            for (auto& c : chans_) {
                double up = 0.0;
                for (std::int64_t j = jlo; j <= jhi; ++j)
                    up += xAt(c, j) * (kOver * h_[static_cast<size_t>(mNext_ - kOver * j + kHalf)]);
                const double driven = up * drive_;
                const double sym = std::tanh(driven);
                const double asym = std::tanh(driven + kBias) - biasTanh_;
                const double shaped = (1.0 - warmth_) * sym + warmth_ * asym;
                const double y = shaped - c.dcIn + dcR_ * c.dcOut;
                c.dcIn = shaped;
                c.dcOut = y;
                c.s.push_back(y);
            }
        }
        // down[k] reads shaped up to 4k + kHalf.
        const std::int64_t kLimit = finished_ ? received_ : (mNext_ > kHalf ? floorDiv(mNext_ - kHalf - 1, kOver) + 1 : 0);
        for (; kNext_ < kLimit; ++kNext_) {
            for (size_t ci = 0; ci < chans_.size(); ++ci) {
                const auto& c = chans_[ci];
                double down = 0.0;
                for (std::int64_t m = kOver * kNext_ - kHalf; m <= kOver * kNext_ + kHalf; ++m)
                    down += sAt(c, m) * h_[static_cast<size_t>(kOver * kNext_ - m + kHalf)];
                const double dry = xAt(c, kNext_);
                out.ch[ci].push_back(dry * (1.0 - mix_) + down * trim_ * mix_);
            }
        }
        // Drop what no later sample reads.
        const std::int64_t xKeep = std::min(kNext_, floorDiv(mNext_ - kHalf, kOver) - 1);
        const std::int64_t sKeep = kOver * kNext_ - kHalf;
        for (auto& c : chans_) {
            if (xKeep - c.xStart > 4096) {
                c.x.erase(c.x.begin(), c.x.begin() + static_cast<std::ptrdiff_t>(xKeep - c.xStart));
                c.xStart = xKeep;
            }
            if (sKeep - c.sStart > 4 * 4096) {
                c.s.erase(c.s.begin(), c.s.begin() + static_cast<std::ptrdiff_t>(sKeep - c.sStart));
                c.sStart = sKeep;
            }
        }
    }

    double drive_, warmth_, mix_, trim_, dcR_, biasTanh_;
    std::vector<double> h_;
    std::vector<Channel> chans_;
    std::int64_t received_ = 0, mNext_ = 0, kNext_ = 0;
    bool finished_ = false;
};

} // namespace

std::vector<double> firwinKaiser(int numTaps, double cutoff, double beta)
{
    std::vector<double> h(static_cast<size_t>(numTaps));
    const double alpha = 0.5 * (numTaps - 1);
    const double i0Beta = besselI0(beta);
    double sum = 0.0;
    for (int i = 0; i < numTaps; ++i) {
        const double m = i - alpha;
        const double x = cutoff * m;
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double r = alpha > 0.0 ? m / alpha : 0.0;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
        h[static_cast<size_t>(i)] = cutoff * sinc * w;
        sum += h[static_cast<size_t>(i)];
    }
    for (auto& v : h) v /= sum;
    return h;
}

std::unique_ptr<ChainStage> makeTapeSaturationStage(double driveDb, double warmth, double mix, double outputTrimDb,
                                                    double sampleRate, int numChannels)
{
    return std::make_unique<TapeSaturationStage>(driveDb, warmth, mix, outputTrimDb, sampleRate, numChannels);
}

} // namespace khensu::dsp
