// desktop-app/Source/ui/Dashboard/EqCurvePanel.cpp
#include "EqCurvePanel.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

EqCurvePanel::EqCurvePanel(IAppController& c) : PanelCard(c, "Dynamic EQ Suggestions")
{
    setExplainer(TRANS("EQ Curve"),
                 TRANS("Suggested corrective EQ matching your track to the target genre's reference curve. "
                 "Drag node handles to adjust parametric EQ bands. Triangle markers indicate resonance "
                 "hotspots. Circled ! chips flag recommendations that concern a frequency range "
                 "(red severe, amber moderate, blue minor) - hover a chip to read the note."),
                 "eq_curve_gui");
    addAndMakeVisible(eqCurve_);
}

void EqCurvePanel::setData(const AnalysisResult& r)
{
    // Extract bands from recommendations or build demo bands if empty
    std::vector<EqBand> bands;

    // Check if recommendations contain EQ suggestions
    for (const auto& rec : r.recommendations) {
        auto itFreq = rec.suggested_parameters.find("eq_freq_hz");
        auto itGain = rec.suggested_parameters.find("eq_gain_db");
        auto itQ = rec.suggested_parameters.find("eq_q");
        auto itType = rec.suggested_parameters.find("eq_type");
        if (itFreq != rec.suggested_parameters.end() && itGain != rec.suggested_parameters.end()) {
            EqBand b;
            b.hz = itFreq->second;
            b.gain_db = itGain->second;
            b.q = itQ != rec.suggested_parameters.end() ? itQ->second : 1.0f;
            // Keep the recommended SHAPE (AUDIT-008): a high-shelf suggestion
            // used to be drawn - and auditioned - as a bell.
            b.type = itType != rec.suggested_parameters.end() ? eqBandTypeFromCode(itType->second) : EqBandType::Peak;
            b.is_suggested = true;
            b.enabled = true;
            bands.push_back(b);
        }
    }

    // No recommended EQ moves = a flat curve. (This used to invent five demo
    // bands, which read as real recommendations for the user's track.)
    // Bands are evaluated as digital biquads at the session's rate.
    if (r.sample_rate > 0.0) eqCurve_.setSpectrumSampleRate(r.sample_rate);
    eqCurve_.setBands(std::move(bands));

    // Underlayer spectrum
    if (!r.spectral.fft_mid_magnitudes.empty()) {
        eqCurve_.setSpectrumSampleRate(r.sample_rate);
        eqCurve_.setSpectrumUnderlayer(r.spectral.fft_mid_magnitudes);
    } else {
        // No measured spectrum: show none rather than a fabricated one.
        eqCurve_.setSpectrumUnderlayer({});
    }

    // Genre reference centroids
    std::optional<GenreEntry> genre;
    if (auto* session = controller_.getActiveSession())
        genre = controller_.genreDatabase().findById(session->target.primary_genre);

    if (genre) {
        eqCurve_.setGenreReferenceCurve(genre->spectral_centroid_hz, genre->low_end_weight);
    } else {
        eqCurve_.setGenreReferenceCurve(2500.0f, 0.5f); // default demo target
    }

    // Resonance markers
    if (r.resonance) {
        eqCurve_.setResonanceMarkers(r.resonance->hotspots);
    } else {
        // No resonance scan (it needs the deep pass): show no markers. Two
        // invented "demo" peaks used to be drawn here, indistinguishable from
        // real resonances on the user's track.
        eqCurve_.setResonanceMarkers({});
    }

    // In-panel recommendation annotations (Gemini audit): overlay every
    // frequency-scoped recommendation directly on the curve so "what to fix"
    // reads on the graph, not only in a side list. Only real recommendations
    // with a valid freq_range and meaningful severity are shown - never demo
    // fillers, which would fabricate warnings.
    std::vector<EqAnnotation> annotations;
    for (const auto& rec : r.recommendations) {
        if (!rec.freq_range.has_value()) continue;
        const auto& fr = *rec.freq_range;
        if (!fr.is_valid() || fr.high_hz <= 0.0f) continue;
        if (rec.severity < 20.0f) continue;
        EqAnnotation a;
        a.low_hz = fr.low_hz;
        a.high_hz = fr.high_hz;
        a.severity = rec.severity;
        a.label = juce::String(rec.what.empty() ? rec.suggested_action : rec.what);
        annotations.push_back(std::move(a));
    }
    eqCurve_.setRecommendationAnnotations(std::move(annotations));

    repaint();
}

void EqCurvePanel::resized()
{
    PanelCard::resized();
    eqCurve_.setBounds(bodyArea());
}

} // namespace khensu
