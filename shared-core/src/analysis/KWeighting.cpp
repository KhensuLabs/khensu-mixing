// ITU-R BS.1770-4 K-weighting coefficients: the one C++ definition (FIX-11).
#include <khensu/analysis/KWeighting.h>

#include <cmath>

namespace khensu::analysis {

namespace {
constexpr double kPi = 3.14159265358979323846;
} // namespace

// K-weighting in the De Man parameterisation of BS.1770 (CORE-12 parity):
// both stages are bilinear-transformed with prewarping, and at 48 kHz they
// reproduce the ITU-R BS.1770-4 Table 1 / Table 2 coefficients exactly. The
// old RBJ +4 dB / 1500 Hz shelf was only an approximation of the stage-1 curve
// (the Python service uses these same constants - keep them in lock-step).
KWeightingCoefficients kWeightingCoefficients(double fs) noexcept
{
    KWeightingCoefficients c{};
    if (fs == 48000.0) {
        // ITU-R BS.1770-4 Tables 1 and 2 verbatim, as the Python service's
        // loudness.py uses at 48 kHz (MEAS-5); the design below reproduces
        // them only to ~1e-12.
        c.preFilter = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
        c.rlbFilter = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
        return c;
    }
    {
        constexpr double G = 3.99984385397; // shelf gain, dB
        constexpr double Q = 0.7071752369554193;
        constexpr double fc = 1681.9744509555319;
        const double K = std::tan(kPi * fc / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.499666774155) /* loudness.py's exponent */;
        const double a0 = 1.0 + K / Q + K * K;
        c.preFilter = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                       2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    }
    {
        // RLB high-pass (BS.1770-4 stage 2). The numerator is exactly
        // [1, -2, 1] (unnormalised) - a passband-normalised numerator reads
        // ~0.04 LU low against the ITU reference.
        constexpr double Q = 0.5003270373253953;
        constexpr double fc = 38.13547087613982;
        const double K = std::tan(kPi * fc / fs);
        const double den = 1.0 + K / Q + K * K;
        c.rlbFilter = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / den, (1.0 - K / Q + K * K) / den};
    }
    return c;
}

} // namespace khensu::analysis
