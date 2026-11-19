// desktop-app/Source/ui/Dashboard/SongInfoPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/BadgeComponent.h"
#include "../shared/MeterBarComponent.h"
#include "../shared/TargetRingGauge.h"
#include "../shared/KhensuErrorState.h"
#include <khensu/analysis/AnalysisResult.h>
#include <array>

namespace khensu {

// Small pseudo-3D fingerprint strip: three heat bars (Energy / Hook /
// Memorability) for an at-a-glance visual read of the track's character.
class SongFingerprintStrip : public juce::Component {
  public:
    SongFingerprintStrip() = default;
    void setValues(float energy0to100, float hook0to100, float memorability0to100)
    {
        energy_ = energy0to100;
        hook_ = hook0to100;
        memorability_ = memorability0to100;
        hasData_ = true;
        repaint();
    }
    void paint(juce::Graphics& g) override;

  private:
    float energy_ = 0.0f, hook_ = 0.0f, memorability_ = 0.0f;
    bool hasData_ = false;
    JUCE_DECLARE_NON_COPYABLE(SongFingerprintStrip)
};

// Thin horizontal stacked bar showing top-N genre candidate rank weights.
// (see AnalysisResult::GenreBlendEntry) - an augmentation of the existing
// genre badge for tracks that sit between genres, not a replacement for it.
// Hidden (zero height wanted) when there's only one blend entry, since a
// single-genre track has nothing to visualize beyond the badge itself.
class GenreBlendStrip : public juce::Component, public juce::SettableTooltipClient {
  public:
    GenreBlendStrip() = default;
    void setBlend(std::vector<AnalysisResult::GenreBlendEntry> blend)
    {
        blend_ = std::move(blend);
        juce::String tip;
        for (const auto& e : blend_)
            tip << juce::String(e.genre) << " relative rank " << juce::roundToInt(e.rank_score * 100.0f) << "%  ";
        setTooltip(tip.trim());
        repaint();
    }
    bool hasMultipleGenres() const noexcept { return blend_.size() > 1; }
    void paint(juce::Graphics& g) override;

  private:
    std::vector<AnalysisResult::GenreBlendEntry> blend_;
    JUCE_DECLARE_NON_COPYABLE(GenreBlendStrip)
};

// General song info: genre/mood/energy, BPM/key, file specs, and duration -
// consolidated in one place instead of scattered across Intake and Summary.
class SongInfoPanel final : public PanelCard {
  public:
    explicit SongInfoPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void resized() override;
    /// Concert-pitch reference from TuningReferencePanel (A = <hz>); <= 0 clears it.
    void setTuningReference(float referenceHz);

    /// Pure formatter for the "Musical identity" line (unit-testable).
    /// Fields that are empty / unknown are omitted; a time signature whose
    /// meter_confidence is <= 0.2 is shown with a "?" because it is the
    /// service's 4/4 fallback rather than a detection.
    static juce::String formatMusicalIdentity(const juce::String& key, const juce::String& camelot,
                                              const juce::String& relativeKey, const juce::String& parallelKey,
                                              const juce::String& timeSignature, float meterConfidence, float bpm,
                                              float tuningHz);

  private:
    BadgeComponent genreBadge_, moodBadge_;
    juce::Label genreContextLabel_;
    /// "Confirm genre" (P0-1): marks the detected genre as the session's
    /// confirmed one, so genre-relative actions can run. Shown only while
    /// they can't and a confirmation would change that.
    juce::TextButton confirmGenreButton_{TRANS("Confirm genre")};
    void confirmDetectedGenre();
    MeterBarComponent energyMeter_;
    std::array<TargetRingGauge, 3> targetRings_;
    GenreBlendStrip genreBlend_;
    juce::Label fileSpecsLabel_;
    juce::Label musicLabel_; ///< "Musical identity": key, Camelot, relatives, meter, BPM, A = Hz
    struct MusicIdentity {
        juce::String key, camelot, relativeKey, parallelKey, timeSignature;
        float meterConfidence = -1.0f, bpm = 0.0f;
    } music_;
    float tuningHz_ = 0.0f;
    void refreshMusicLabel();
    KhensuErrorState errorState_; ///< measurement-trust fetch failure + Retry
    void fetchMeasurementTrust();
    juce::Label trustLabel_; ///< measurement trust map from /analyze/measurement_quality (#5)
    std::array<BadgeComponent, 5> trustBadges_; ///< loudness, true peak, dynamics, tempo, key
    SongFingerprintStrip fingerprint_;
    void setMeasurementTrust(const std::map<std::string, AnalysisResult::MeasurementTrust>& trust);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SongInfoPanel)
};

} // namespace khensu
