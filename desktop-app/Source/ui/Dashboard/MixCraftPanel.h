// desktop-app/Source/ui/Dashboard/MixCraftPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/targets/GenreSpread.h>
#include <map>

namespace khensu {

/// "Mix Craft" category scorecard - the Readiness breakdown (UX2-1): a row
/// per category (weight %, a bar to 100, the score), with Technical
/// Integrity's Clipping/Noise & Artefacts sub-scores shown indented.
/// Categories the backing analysis hasn't computed yet (FX & Processing,
/// Frequency Masking need /analyze/extended) show "Not yet analyzed" instead
/// of a fabricated number.
class MixCraftPanel final : public juce::Component, public juce::SettableTooltipClient {
  public:
    MixCraftPanel() = default;
    ~MixCraftPanel() override = default;

    void setScorecard(const MixScorecard& scorecard);
    void clear();
    /// VIZ-1c: where released tracks of the genre score per category (their
    /// middle 50 % / 80 % behind each row); nullptr = no measured spread.
    void setSpread(const GenreSpread::Entry* entry, const juce::String& genreName);
    void mouseMove(const juce::MouseEvent& e) override;
    /// The row tooltip for a category (tests).
    juce::String rowReadout(const ScorecardCategory& cat) const;

    int preferredHeight() const;
    /// Where the host puts the weighting choice, in this panel's own
    /// coordinates (the header's right end, UX2-1).
    juce::Rectangle<int> weightControlArea() const;

    void paint(juce::Graphics& g) override;

  private:
    MixScorecard scorecard_;
    bool hasData_ = false;
    std::map<std::string, GenreSpread::Percentiles> spread_;
    juce::String spreadName_;
    int spreadTracks_ = 0;
    std::vector<std::pair<juce::Rectangle<int>, std::string>> rowAreas_; ///< last paint, for hover

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixCraftPanel)
};

} // namespace khensu
