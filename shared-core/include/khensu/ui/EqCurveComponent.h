// shared-core/include/khensu/ui/EqCurveComponent.h
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <khensu/analysis/AnalysisResult.h>
#include <functional>
#include <vector>

namespace khensu {

/// Filter shape of an EqBand. Drawn and rendered as the matching RBJ
/// "Audio EQ Cookbook" biquad (AUDIT-008: shelves used to be drawn and
/// rendered as peaking bells, so a "-3 dB high shelf at 5 kHz" left 10-20 kHz
/// untouched).
enum class EqBandType { Peak, LowShelf, HighShelf, HighPass, LowPass };

/// Maps a recommendation's suggested_parameters["eq_type"] code
/// (SpectralRecommender: 0 bell, 1 high shelf, 2 low shelf, 3 high-pass,
/// 4 low-pass) to a band type. Unknown codes fall back to a bell.
inline EqBandType eqBandTypeFromCode(float code) noexcept
{
    switch (static_cast<int>(code + 0.5f)) {
        case 1: return EqBandType::HighShelf;
        case 2: return EqBandType::LowShelf;
        case 3: return EqBandType::HighPass;
        case 4: return EqBandType::LowPass;
        default: return EqBandType::Peak;
    }
}

/// The local service's render-chain effect name for a band type (the
/// pedalboard filter classes registered in copilot/dsp.py EFFECT_MAP).
inline const char* renderEffectName(EqBandType type) noexcept
{
    switch (type) {
        case EqBandType::LowShelf: return "LowShelfFilter";
        case EqBandType::HighShelf: return "HighShelfFilter";
        case EqBandType::HighPass: return "HighpassFilter";
        case EqBandType::LowPass: return "LowpassFilter";
        case EqBandType::Peak: break;
    }
    return "PeakFilter";
}

/// True for shapes whose gain parameter does something (HPF/LPF ignore it).
inline bool eqBandTypeHasGain(EqBandType type) noexcept
{
    return type != EqBandType::HighPass && type != EqBandType::LowPass;
}

/// A single parametric EQ band descriptor shown on the curve.
struct EqBand {
    float hz = 1000.0f;        ///< Centre / corner frequency (Hz)
    float gain_db = 0.0f;      ///< Gain (dB), positive = boost, negative = cut (unused by HPF/LPF)
    float q = 0.707f;          ///< Q factor (bandwidth / shelf slope / filter resonance)
    bool is_suggested = false; ///< true = grey (suggested), false = user-set (teal)
    bool enabled = true;       ///< greys out the band when false
    /// Last, so existing positional {hz, gain, q, suggested, enabled}
    /// initialisers (the plugin's three peaking bands) stay valid.
    EqBandType type = EqBandType::Peak;
};

/// A recommendation warning overlaid directly on the curve at the frequency
/// range it concerns. Fed from AnalysisResult::recommendations that carry a
/// freq_range, so the "what to fix" reads on the graph, not just a side list.
struct EqAnnotation {
    float low_hz = 0.0f;   ///< Range start (Hz); clamped to axis
    float high_hz = 0.0f;  ///< Range end (Hz)
    float severity = 0.0f; ///< 0-100 (red >=66, amber 33-66, blue <33)
    juce::String label;    ///< Plain-English "what" - shown on hover
};

/// An interactive EQ curve display + editing component.
///
/// Shows:
///   - The aggregate magnitude response curve computed from the EqBand list
///   - Draggable handles for each band (left/right = frequency, up/down = gain)
///   - Resonance hazard markers from AnalysisResult (amber triangles)
///   - Genre target reference curve (dashed teal) when set
///   - Octave-band spectrum underlay (light grey fill) when band data is provided
///
/// Callback: onBandChanged(index, EqBand) fires on every drag update.
///
/// THREAD_CONTEXT(UIThread)
class EqCurveComponent final : public juce::Component {
  public:
    using BandChangedCallback = std::function<void(int index, const EqBand& band)>;

    EqCurveComponent();

    // ---- Data setters ----
    void setBands(std::vector<EqBand> bands);
    void setSpectrumUnderlayer(const std::vector<float>& rms_per_octave_db);
    /// Sample rate of the analysed audio, used to place FFT-bin underlays
    /// (default 44.1 kHz; 48/96 kHz material was drawn 9-54% too low) and to
    /// evaluate each band as the digital biquad the render will apply.
    void setSpectrumSampleRate(double sampleRate) noexcept
    {
        if (sampleRate > 0.0) spectrumSampleRate_ = sampleRate;
    }

    /// Magnitude (dB) of one analog peaking-EQ band at `hz`. Public for tests.
    static float peakingMagnitudeDb(float fc, float gain_db, float Q, float hz) noexcept;
    /// |H(e^jw)| in dB of `band` as an RBJ digital biquad at `sampleRate`,
    /// evaluated at `hz` (the curve the render's pedalboard filter has).
    /// Public for tests.
    static float bandMagnitudeDb(const EqBand& band, float hz, double sampleRate) noexcept;
    void setResonanceMarkers(const std::vector<AnalysisResult::ResonancePeak>& peaks);
    void setGenreReferenceCurve(float centroid_hz, float low_end_weight);
    void setRecommendationAnnotations(std::vector<EqAnnotation> annotations);
    void onBandChanged(BandChangedCallback cb) { onBandChanged_ = std::move(cb); }
    /// Fires once when a band drag begins / ends (with the band's state at that
    /// moment), so hosts can bracket parameter gestures and persist one record
    /// per drag instead of one per mouse event.
    void onBandDragStarted(BandChangedCallback cb) { onBandDragStarted_ = std::move(cb); }
    void onBandDragEnded(BandChangedCallback cb) { onBandDragEnded_ = std::move(cb); }

    // ---- JUCE Component overrides ----
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    // Coordinate helpers
    float xForHz(float hz) const noexcept;
    float yForDb(float db) const noexcept;
    float hzForX(float x) const noexcept;
    float dbForY(float y) const noexcept;

    // Curve computation
    float computeMagnitudeDb(float hz) const noexcept;
    juce::Path buildCurvePath() const;
    juce::Path buildSpectrumPath() const;
    juce::Path buildReferencePath() const;

    // State
    std::vector<EqBand> bands_;
    std::vector<float> spectrumDb_; // 10-band octave RMS
    double spectrumSampleRate_ = 44100.0;
    std::vector<AnalysisResult::ResonancePeak> resonancePeaks_;
    std::vector<EqAnnotation> annotations_;
    float genreCentroid_ = 0.0f;
    float genreLowEnd_ = 0.0f;
    int dragBandIndex_ = -1;
    int hoveredBand_ = -1;
    int hoveredAnnotation_ = -1;
    juce::Point<float> dragStartPos_;
    float dragStartGain_ = 0.0f;
    float dragStartHz_ = 0.0f;
    BandChangedCallback onBandChanged_;
    BandChangedCallback onBandDragStarted_;
    BandChangedCallback onBandDragEnded_;

    // Layout
    static constexpr float kMinHz = 20.0f;
    static constexpr float kMaxHz = 20000.0f;
    static constexpr float kMinDb = -18.0f;
    static constexpr float kMaxDb = +18.0f;
    static constexpr float kHandleRadius = 7.0f;

    juce::Rectangle<float> plotArea() const noexcept;
    int hitTestBand(juce::Point<float> pos) const noexcept;
    int hitTestAnnotation(juce::Point<float> pos) const noexcept;
    juce::Rectangle<float> annotationChipBounds(const EqAnnotation& a) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqCurveComponent)
};

} // namespace khensu
