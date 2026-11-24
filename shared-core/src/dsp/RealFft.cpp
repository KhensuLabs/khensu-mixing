// FN2-2a: double-precision radix-2 FFT - see RealFft.h.
#include <khensu/dsp/RealFft.h>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace khensu::dsp {

RealFft::RealFft(int size) : n_(size)
{
    if (size < 2 || (size & (size - 1)) != 0) throw std::invalid_argument("RealFft size must be a power of two");
    int bits = 0;
    while ((1 << bits) < size) ++bits;
    bitrev_.resize(static_cast<size_t>(size));
    for (int i = 0; i < size; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if ((i >> b) & 1) r |= 1 << (bits - 1 - b);
        bitrev_[static_cast<size_t>(i)] = r;
    }
    constexpr double kPi = 3.14159265358979323846;
    twiddle_.resize(static_cast<size_t>(size / 2));
    for (int k = 0; k < size / 2; ++k)
        twiddle_[static_cast<size_t>(k)] =
            std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(size));
    work_.resize(static_cast<size_t>(size));
}

void RealFft::transform(std::vector<std::complex<double>>& a, bool inverse) const
{
    const auto n = static_cast<size_t>(n_);
    for (size_t i = 0; i < n; ++i) {
        const auto j = static_cast<size_t>(bitrev_[i]);
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, step = n / len;
        for (size_t start = 0; start < n; start += len)
            for (size_t k = 0; k < half; ++k) {
                const auto w = inverse ? std::conj(twiddle_[k * step]) : twiddle_[k * step];
                const auto u = a[start + k];
                const auto v = a[start + k + half] * w;
                a[start + k] = u + v;
                a[start + k + half] = u - v;
            }
    }
}

void RealFft::forward(const double* in, std::vector<std::complex<double>>& out)
{
    const auto n = static_cast<size_t>(n_);
    for (size_t i = 0; i < n; ++i) work_[i] = {in[i], 0.0};
    transform(work_, false);
    out.assign(work_.begin(), work_.begin() + static_cast<std::ptrdiff_t>(n / 2 + 1));
}

void RealFft::inverse(const std::vector<std::complex<double>>& spec, double* out)
{
    const auto n = static_cast<size_t>(n_);
    const size_t bins = n / 2 + 1;
    work_[0] = {spec[0].real(), 0.0};
    work_[n / 2] = {spec[n / 2].real(), 0.0};
    for (size_t k = 1; k < bins - 1; ++k) {
        work_[k] = spec[k];
        work_[n - k] = std::conj(spec[k]);
    }
    transform(work_, true);
    const double scale = 1.0 / static_cast<double>(n);
    for (size_t i = 0; i < n; ++i) out[i] = work_[i].real() * scale;
}

} // namespace khensu::dsp
