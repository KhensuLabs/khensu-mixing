#pragma once
// FN2-2a: a double-precision FFT for the native chain renderer's spectral
// stages (SpectralDenoise). numpy's rfft / irfft run in float64; the denoiser
// is compared with the service's within a few hundredths of a dB, so this is
// a plain iterative radix-2 transform in double rather than juce::dsp::FFT
// (float only, and KhensuDsp compiles no JUCE sources).
// THREAD_CONTEXT(Any) - one instance per user; not shared across threads.
#include <complex>
#include <vector>

namespace khensu::dsp {

class RealFft {
  public:
    /// `size` must be a power of two (>= 2).
    explicit RealFft(int size);
    int size() const noexcept { return n_; }
    /// numpy.fft.rfft: `in` has size() samples; `out` receives size()/2 + 1 bins.
    void forward(const double* in, std::vector<std::complex<double>>& out);
    /// numpy.fft.irfft(spec, n=size()): `spec` has size()/2 + 1 bins (the
    /// imaginary parts of the DC and Nyquist bins are ignored, as numpy does);
    /// `out` receives size() samples.
    void inverse(const std::vector<std::complex<double>>& spec, double* out);

  private:
    void transform(std::vector<std::complex<double>>& a, bool inverse) const;
    int n_;
    std::vector<int> bitrev_;
    std::vector<std::complex<double>> twiddle_; // exp(-2 pi i k / n), k < n/2
    std::vector<std::complex<double>> work_;
};

} // namespace khensu::dsp
