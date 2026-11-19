// desktop-app/Source/ui/Dashboard/TonalBalancePanel.h
// Tonal Balance Fingerprint: long-term octave spectrum vs. the genre's
// target tilt curve with a tolerance band - the standard pro "does the
// balance match the genre" sanity check as a shape comparison, not a
// single centroid/tilt scalar.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

class IAppController;

/// Draws AnalysisResult::SpectralData::rms_per_octave_db (10 octave bands,
/// 31 Hz-16 kHz) as a mean-normalized curve against the genre's expected
/// tilt line (GenreEntry::spectral_tilt_target_db_per_octave) with a ±3 dB
/// tolerance band. Bands outside tolerance are flagged, and the footer names
/// the worst deviation ("+4.2 dB at 8 kHz - brighter than genre norm").
class TonalBalancePanel final : public PanelCard {
  public:
    explicit TonalBalancePanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;

  private:
    static constexpr float kToleranceDb = 3.0f;

    std::vector<float> measuredDb_; // mean-normalized, one per octave band
    std::vector<float> targetDb_;   // mean-normalized genre tilt line, same length
    juce::String genreName_;
    juce::String verdict_;
    juce::Colour verdictColour_;
    bool hasData_ = false;

    void rebuildVerdict();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TonalBalancePanel)
};

} // namespace khensu
