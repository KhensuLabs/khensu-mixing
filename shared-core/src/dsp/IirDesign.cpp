// OVH-9: IIR designs matched to the service's chain render - see IirDesign.h.
// THREAD_CONTEXT(Any)
#include <khensu/dsp/IirDesign.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace khensu::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
using Complex = std::complex<double>;

BiquadCoefficients normalised(double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

/// scipy's pre-warp with fs = 2: 2 * fs * tan(pi * Wn / fs), Wn = f / Nyquist.
double prewarp(double hz, double sampleRate) noexcept
{
    const double wn = hz / (sampleRate * 0.5);
    return 4.0 * std::tan(kPi * wn / 2.0);
}

} // namespace

void BiquadCascade::setSections(std::vector<BiquadCoefficients> sections, int numChannels)
{
    sections_ = std::move(sections);
    state_.assign(static_cast<size_t>(std::max(0, numChannels)), std::vector<double>(2 * sections_.size(), 0.0));
}

void BiquadCascade::reset()
{
    for (auto& s : state_) std::fill(s.begin(), s.end(), 0.0);
}

void BiquadCascade::process(int channel, double* x, int numSamples) noexcept
{
    if (channel < 0 || static_cast<size_t>(channel) >= state_.size()) return;
    auto& st = state_[static_cast<size_t>(channel)];
    for (size_t k = 0; k < sections_.size(); ++k) {
        const auto& c = sections_[k];
        double s1 = st[2 * k], s2 = st[2 * k + 1];
        for (int i = 0; i < numSamples; ++i) {
            const double in = x[i];
            const double y = c.b0 * in + s1;
            s1 = c.b1 * in - c.a1 * y + s2;
            s2 = c.b2 * in - c.a2 * y;
            x[i] = y;
        }
        st[2 * k] = s1;
        st[2 * k + 1] = s2;
    }
}

double BiquadCascade::magnitudeAt(double frequencyHz, double sampleRate) const noexcept
{
    const double w = 2.0 * kPi * frequencyHz / sampleRate;
    const Complex z1 = std::polar(1.0, -w);
    const Complex z2 = z1 * z1;
    double mag = 1.0;
    for (const auto& c : sections_) {
        const Complex num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const Complex den = 1.0 + c.a1 * z1 + c.a2 * z2;
        mag *= std::abs(num / den);
    }
    return mag;
}

std::vector<BiquadCoefficients> butterworth(ButterworthType type, int order, double sampleRate, double lowHz,
                                            double highHz)
{
    const int n = std::clamp(order, 1, 16);
    // buttap: p = -exp(1j * pi * m / (2N)), m = -N+1, -N+3, ..., N-1; k = 1.
    std::vector<Complex> proto;
    for (int m = -n + 1; m <= n - 1; m += 2)
        proto.push_back(-std::exp(Complex(0.0, kPi * static_cast<double>(m) / (2.0 * n))));

    std::vector<Complex> zeros, poles;
    double gain = 1.0;
    switch (type) {
    case ButterworthType::Lowpass: { // lp2lp_zpk
        const double wo = prewarp(lowHz, sampleRate);
        for (const auto& p : proto) poles.push_back(p * wo);
        gain = std::pow(wo, n);
        break;
    }
    case ButterworthType::Highpass: { // lp2hp_zpk
        const double wo = prewarp(lowHz, sampleRate);
        Complex prodNegP = 1.0;
        for (const auto& p : proto) {
            poles.push_back(wo / p);
            prodNegP *= -p;
        }
        zeros.assign(static_cast<size_t>(n), Complex(0.0, 0.0));
        gain = std::real(1.0 / prodNegP);
        break;
    }
    case ButterworthType::Bandpass: { // lp2bp_zpk
        const double w1 = prewarp(lowHz, sampleRate), w2 = prewarp(highHz, sampleRate);
        const double bw = w2 - w1, wo = std::sqrt(w1 * w2);
        for (const auto& p : proto) {
            const Complex pl = p * (bw / 2.0);
            const Complex root = std::sqrt(pl * pl - wo * wo);
            poles.push_back(pl + root);
            poles.push_back(pl - root);
        }
        zeros.assign(static_cast<size_t>(n), Complex(0.0, 0.0));
        gain = std::pow(bw, n);
        break;
    }
    }

    // bilinear_zpk with fs = 2 (fs2 = 4); missing zeros go to Nyquist (z = -1).
    constexpr double fs2 = 4.0;
    Complex num = 1.0, den = 1.0;
    for (const auto& z : zeros) num *= fs2 - z;
    for (const auto& p : poles) den *= fs2 - p;
    gain *= std::real(num / den);
    std::vector<double> zd; // every zero is real (+1 or -1)
    for (const auto& z : zeros) zd.push_back(std::real((fs2 + z) / (fs2 - z)));
    for (size_t i = zeros.size(); i < poles.size(); ++i) zd.push_back(-1.0);
    std::vector<Complex> pd;
    for (const auto& p : poles) pd.push_back((fs2 + p) / (fs2 - p));

    // Pair conjugate poles into sections; real poles two at a time (one
    // first-order section for an odd count). Zeros alternate +1 / -1 where
    // both exist so every band-pass section is [1, 0, -1].
    std::vector<Complex> upper;
    std::vector<double> reals;
    for (const auto& p : pd) {
        if (std::abs(p.imag()) < 1e-12)
            reals.push_back(p.real());
        else if (p.imag() > 0.0)
            upper.push_back(p);
    }
    std::vector<double> plus, minus;
    for (double z : zd) (z > 0.0 ? plus : minus).push_back(z);
    std::vector<double> zeroPool;
    while (!plus.empty() || !minus.empty()) {
        if (!plus.empty()) {
            zeroPool.push_back(plus.back());
            plus.pop_back();
        }
        if (!minus.empty()) {
            zeroPool.push_back(minus.back());
            minus.pop_back();
        }
    }
    size_t nextZero = 0;
    const auto takeZero = [&]() { return nextZero < zeroPool.size() ? zeroPool[nextZero++] : 0.0; };

    std::vector<BiquadCoefficients> sections;
    for (const auto& p : upper) {
        const double z1 = takeZero(), z2 = takeZero();
        sections.push_back({1.0, -(z1 + z2), z1 * z2, -2.0 * p.real(), std::norm(p)});
    }
    for (size_t i = 0; i + 1 < reals.size(); i += 2) {
        const double z1 = takeZero(), z2 = takeZero();
        sections.push_back({1.0, -(z1 + z2), z1 * z2, -(reals[i] + reals[i + 1]), reals[i] * reals[i + 1]});
    }
    if (reals.size() % 2 == 1) {
        const double z1 = takeZero();
        sections.push_back({1.0, -z1, 0.0, -reals.back(), 0.0});
    }
    if (!sections.empty()) {
        sections.front().b0 *= gain;
        sections.front().b1 *= gain;
        sections.front().b2 *= gain;
    }
    return sections;
}

BiquadCoefficients peakFilter(double sampleRate, double frequencyHz, double q, double gainDb) noexcept
{
    const double gainFactor = std::pow(10.0, gainDb / 20.0);
    const double A = std::sqrt(std::max(0.0, gainFactor));
    const double omega = 2.0 * kPi * std::max(frequencyHz, 2.0) / sampleRate;
    const double alpha = std::sin(omega) / (q * 2.0);
    const double c2 = -2.0 * std::cos(omega);
    const double alphaTimesA = alpha * A, alphaOverA = alpha / A;
    return normalised(1.0 + alphaTimesA, c2, 1.0 - alphaTimesA, 1.0 + alphaOverA, c2, 1.0 - alphaOverA);
}

BiquadCoefficients lowShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept
{
    const double A = std::sqrt(std::max(0.0, std::pow(10.0, gainDb / 20.0)));
    const double aminus1 = A - 1.0, aplus1 = A + 1.0;
    const double omega = 2.0 * kPi * std::max(frequencyHz, 2.0) / sampleRate;
    const double coso = std::cos(omega);
    const double beta = std::sin(omega) * std::sqrt(A) / q;
    const double aminus1TimesCoso = aminus1 * coso;
    return normalised(A * (aplus1 - aminus1TimesCoso + beta), A * 2.0 * (aminus1 - aplus1 * coso),
                      A * (aplus1 - aminus1TimesCoso - beta), aplus1 + aminus1TimesCoso + beta,
                      -2.0 * (aminus1 + aplus1 * coso), aplus1 + aminus1TimesCoso - beta);
}

BiquadCoefficients highShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept
{
    const double A = std::sqrt(std::max(0.0, std::pow(10.0, gainDb / 20.0)));
    const double aminus1 = A - 1.0, aplus1 = A + 1.0;
    const double omega = 2.0 * kPi * std::max(frequencyHz, 2.0) / sampleRate;
    const double coso = std::cos(omega);
    const double beta = std::sin(omega) * std::sqrt(A) / q;
    const double aminus1TimesCoso = aminus1 * coso;
    return normalised(A * (aplus1 + aminus1TimesCoso + beta), A * -2.0 * (aminus1 + aplus1 * coso),
                      A * (aplus1 + aminus1TimesCoso - beta), aplus1 - aminus1TimesCoso + beta,
                      2.0 * (aminus1 - aplus1 * coso), aplus1 - aminus1TimesCoso - beta);
}

} // namespace khensu::dsp
