// MEAS-5: the one true-peak interpolator - see TruePeakFilter.h.
// THREAD_CONTEXT(Any)
#include <khensu/analysis/TruePeakFilter.h>

#include <algorithm>
#include <cmath>

namespace khensu::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTaps = 64 * kTruePeakOversample + 1; // 257
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

TruePeakPhases build() noexcept
{
    TruePeakPhases f{};
    // scipy.signal.firwin(257, 0.25, window=("kaiser", 9.0)), scale=True.
    double h[kTaps];
    double sum = 0.0;
    const double m0 = (kTaps - 1) * 0.5;
    const double i0b = besselI0(kKaiserBeta);
    for (int i = 0; i < kTaps; ++i) {
        const double m = static_cast<double>(i) - m0;
        const double xs = 0.25 * m;
        const double sinc = std::abs(xs) < 1e-15 ? 1.0 : std::sin(kPi * xs) / (kPi * xs);
        const double r = 2.0 * static_cast<double>(i) / static_cast<double>(kTaps - 1) - 1.0;
        const double win = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[i] = 0.25 * sinc * win;
        sum += h[i];
    }
    for (double& v : h) v = v / sum * kTruePeakOversample; // resample_poly's h *= up
    // resample_poly alignment: y[m] = sum_k h[k] * xup[m + 128 - k].
    for (int p = 0; p < kTruePeakOversample; ++p)
        for (int j = 0; j < kTruePeakPhaseTaps; ++j) {
            const int k = kTruePeakOversample * (kTruePeakPhaseTaps - 1 - j) + p;
            f.w[p][j] = k < kTaps ? h[k] : 0.0;
        }
    return f;
}

} // namespace

const TruePeakPhases& truePeakPhases() noexcept
{
    static const TruePeakPhases f = build();
    return f;
}

} // namespace khensu::analysis
