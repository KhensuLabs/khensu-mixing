// tests/shared-core/TestNoise.h
// Deterministic coloured-noise generator for analyzer tests: seeded Gaussian
// white noise shaped in the frequency domain to a power spectral density
// proportional to 1/f^exponent (0 = white, 1 = pink, 2 = brown). The DC bin
// is zeroed. Output is normalised to `rms`.
#pragma once
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <complex>
#include <random>
#include <vector>

namespace khensu::test {

inline std::vector<float> colouredNoise(int fftOrder, double exponent, unsigned seed, double rms = 0.1)
{
    const int n = 1 << fftOrder;
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, 1.0f);

    std::vector<juce::dsp::Complex<float>> time(static_cast<size_t>(n)), freq(static_cast<size_t>(n));
    for (auto& v : time) v = {gauss(rng), 0.0f};

    juce::dsp::FFT fft(fftOrder);
    fft.perform(time.data(), freq.data(), false);
    freq[0] = {0.0f, 0.0f};
    for (int k = 1; k <= n / 2; ++k) {
        // Amplitude ~ f^(-exponent/2) so POWER ~ f^-exponent.
        const float g = static_cast<float>(std::pow(static_cast<double>(k), -exponent * 0.5));
        freq[static_cast<size_t>(k)] *= g;
        if (k != n / 2) freq[static_cast<size_t>(n - k)] *= g;
    }
    fft.perform(freq.data(), time.data(), true);

    std::vector<float> out(static_cast<size_t>(n));
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = time[static_cast<size_t>(i)].real();
        sumSq += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
    }
    const double scale = sumSq > 0.0 ? rms / std::sqrt(sumSq / n) : 0.0;
    for (auto& v : out) v = static_cast<float>(v * scale);
    return out;
}

} // namespace khensu::test
