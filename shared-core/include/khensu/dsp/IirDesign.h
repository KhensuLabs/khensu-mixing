#pragma once
// OVH-9: the IIR designs the native chain renderer uses, each written to
// match the Python service's render (copilot/dsp*.py) exactly:
//   * Butterworth high/low/band-pass = scipy.signal.butter(..., output="sos")
//     (analogue prototype -> lp2hp / lp2lp / lp2bp -> bilinear with the
//     tan pre-warp, fs = 2), so the magnitude response is scipy's;
//   * peak / low shelf / high shelf = juce::dsp::IIR::Coefficients'
//     makePeakFilter / makeLowShelf / makeHighShelf, which is what
//     pedalboard's PeakFilter / LowShelfFilter / HighShelfFilter run (RBJ
//     cookbook, gain factor 10^(dB/20), A = sqrt(gain factor)).
// THREAD_CONTEXT(Any) - pure functions and plain value types.
#include <vector>

namespace khensu::dsp {

/// One second-order section, a0 normalised to 1:
/// y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2. A first-order section has
/// b2 = a2 = 0.
struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

/// A cascade of sections with per-channel state (transposed direct form II,
/// double precision - scipy's sosfilt).
class BiquadCascade {
  public:
    void setSections(std::vector<BiquadCoefficients> sections, int numChannels);
    const std::vector<BiquadCoefficients>& sections() const noexcept { return sections_; }
    void reset();
    /// Filters `x` in place, carrying the channel's state across calls.
    void process(int channel, double* x, int numSamples) noexcept;
    /// |H(e^jw)| of the whole cascade at `frequencyHz`.
    double magnitudeAt(double frequencyHz, double sampleRate) const noexcept;

  private:
    std::vector<BiquadCoefficients> sections_;
    std::vector<std::vector<double>> state_; // [channel][2 * section]
};

enum class ButterworthType { Lowpass, Highpass, Bandpass };

/// scipy.signal.butter(order, cutoff(s), btype, fs=sampleRate, output="sos"),
/// up to rounding. `lowHz` is the cutoff (or the band's lower edge), `highHz`
/// the band's upper edge (band-pass only). The caller keeps the cutoffs
/// below Nyquist.
std::vector<BiquadCoefficients> butterworth(ButterworthType type, int order, double sampleRate, double lowHz,
                                            double highHz = 0.0);

/// juce::dsp::IIR::Coefficients::makePeakFilter / makeLowShelf / makeHighShelf.
BiquadCoefficients peakFilter(double sampleRate, double frequencyHz, double q, double gainDb) noexcept;
BiquadCoefficients lowShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept;
BiquadCoefficients highShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept;

} // namespace khensu::dsp
