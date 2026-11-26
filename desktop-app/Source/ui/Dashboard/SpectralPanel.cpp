// desktop-app/Source/ui/Dashboard/SpectralPanel.cpp
#include "SpectralPanel.h"
#include <khensu/session/Session.h>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/RecommendationExplainer.h"
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace {
const juce::String kSpectralExplainerBase =
    "Energy across ten octave bands (31 Hz - 16 kHz). The low/mid/high split summarizes "
    "tonal balance; spectral centroid is the perceived 'brightness' centre of mass. "
    "The teal needle marks the genre centroid target. Orange pins are resonance hotspots.";
}

// Every cached palette colour on this component, in one place (UI-01).
void SpectralPanel::applyThemeColours()
{
    balance_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

SpectralPanel::SpectralPanel(IAppController& c) : PanelCard(c, "Spectral Balance")
{
    applyThemeColours();
    setExplainer(TRANS("Spectral"), kSpectralExplainerBase, "spectral_balance");
    addAndMakeVisible(spectrum_);
    balance_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    addAndMakeVisible(balance_);
    addAndMakeVisible(sectionSpectral_);
    sectionSpectral_.setVisible(false);
    addAndMakeVisible(compareChip_);
    compareChip_.setVisible(false);
}

void SpectralPanel::setData(const AnalysisResult& r)
{
    spectrum_.setBands(r.spectral.fft_mid_magnitudes);
    spectrum_.setMidSideBands(r.spectral.fft_mid_magnitudes, r.spectral.fft_side_magnitudes);
    if (r.sample_rate > 0.0) spectrum_.setSampleRate(r.sample_rate);
    balance_.setText(TRANS("Low ") + juce::String((int)(r.spectral.balance_low * 100)) +
                         "%   "
                         "Mid " +
                         juce::String((int)(r.spectral.balance_mid * 100)) +
                         "%   "
                         "High " +
                         juce::String((int)(r.spectral.balance_high * 100)) +
                         "%   |   "
                         "Centroid " +
                         juce::String((int)r.spectral.centroid_hz) + " Hz",
                     juce::dontSendNotification);

    // Resonance pins
    if (r.resonance) {
        std::vector<SpectrumPin> pins;
        for (const auto& hp : r.resonance->hotspots)
            pins.push_back({hp.hz, hp.prominence});
        spectrum_.setResonancePins(std::move(pins));
    } else {
        spectrum_.setResonancePins({});
    }

    // Genre centroid + low-end weight reference
    std::optional<GenreEntry> genre;
    if (auto* session = controller_.getActiveSession())
        genre = controller_.genreDatabase().findById(session->target.primary_genre);

    if (genre) {
        spectrum_.setGenreReference(genre->spectral_centroid_hz, genre->low_end_weight);
        spectrum_.setTargetTilt(genre->spectral_tilt_target_db_per_octave, true);

        // Tilt/crest vs genre-target readout on the balance strip.
        const float tiltDelta = r.spectral.tilt_db_per_octave - genre->spectral_tilt_target_db_per_octave;
        const float crestDelta = r.dynamics.crest_factor_db - genre->crest_factor_target_db;
        balance_.setText(balance_.getText() + "   |   Tilt " + juce::String(r.spectral.tilt_db_per_octave, 1) +
                             " (tgt " + juce::String(genre->spectral_tilt_target_db_per_octave, 1) + ", " +
                             (tiltDelta >= 0 ? "+" : "") + juce::String(tiltDelta, 1) + ")" + "   Crest " +
                             juce::String(r.dynamics.crest_factor_db, 1) + " dB (tgt " +
                             juce::String(genre->crest_factor_target_db, 1) + ", " + (crestDelta >= 0 ? "+" : "") +
                             juce::String(crestDelta, 1) + ")",
                         juce::dontSendNotification);
    } else {
        spectrum_.setGenreReference(0.0f, 0.0f);
        spectrum_.setTargetTilt(0.0f, false);
    }

    // Section harshness evolution
    if (r.section_analysis.has_value() && !r.section_analysis->sections.empty()) {
        std::vector<float> harshnessVals;
        for (const auto& sec : r.section_analysis->sections) {
            // Use harshness_score if computed; fallback to 0.0f if not
            harshnessVals.push_back(sec.harshness_score >= 0.0f ? sec.harshness_score : 0.0f);
        }
        sectionSpectral_.setSections(harshnessVals);
        sectionSpectral_.setVisible(true);
    } else {
        sectionSpectral_.setVisible(false);
    }

    // EQ suggestion overlay: extract spectral recommendations with EQ parameters
    {
        std::vector<HeatSpectrumComponent::EqPoint> eqPts;
        for (const auto& rec : r.recommendations) {
            auto itFreq = rec.suggested_parameters.find("eq_freq_hz");
            auto itGain = rec.suggested_parameters.find("eq_gain_db");
            auto itQ = rec.suggested_parameters.find("eq_q");
            if (itFreq != rec.suggested_parameters.end() && itGain != rec.suggested_parameters.end()) {
                HeatSpectrumComponent::EqPoint pt;
                pt.freq_hz = itFreq->second;
                pt.gain_db = itGain->second;
                pt.q = itQ != rec.suggested_parameters.end() ? itQ->second : 0.707f;
                pt.severity = rec.severity;
                eqPts.push_back(pt);
            }
        }
        spectrum_.setEqSuggestions(std::move(eqPts));
    }

    const auto* rec = explainer::findTopRecommendation(
        r.recommendations, {"centroid_too_bright", "centroid_too_dark", "low_end_heavy", "low_end_light"});
    setExplainer(TRANS("Spectral"), explainer::appendReasoningTrace(kSpectralExplainerBase, rec), "spectral_balance");

    lastRefDeltas_ = r.reference_deltas;
    updateCompareChip();

    repaint();
}

void SpectralPanel::setCompareModeEnabled(bool enabled)
{
    compareEnabled_ = enabled;
    updateCompareChip();
}

void SpectralPanel::updateCompareChip()
{
    if (compareEnabled_ && lastRefDeltas_.has_value()) {
        if (const auto* d = CompareDeltaChip::find(*lastRefDeltas_, "centroid_hz")) {
            compareChip_.setDelta(d->delta, " Hz");
            resized();
            return;
        }
    }
    compareChip_.clear();
    resized();
}

void SpectralPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    balance_.setBounds(b.removeFromBottom(18));
    b.removeFromBottom(4);
    if (compareChip_.isVisible()) {
        compareChip_.setBounds(b.removeFromBottom(16));
        b.removeFromBottom(4);
    }
    sectionSpectral_.setBounds(b.removeFromBottom(32));
    b.removeFromBottom(6);
    spectrum_.setBounds(b);
}

} // namespace khensu
