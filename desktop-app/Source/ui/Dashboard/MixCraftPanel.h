// desktop-app/Source/ui/Dashboard/MixCraftPanel.h
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/MixScorecard.h>

namespace khensu {

/// "Mix Craft" category scorecard: overall weighted score + a row per
/// category (weight%, score/100, Strongest/Improve tag), with Technical
/// Integrity's Clipping/Noise & Artefacts sub-scores shown indented.
/// Categories the backing analysis hasn't computed yet (FX & Processing,
/// Frequency Masking need /analyze/extended) show "Not yet analyzed" instead
/// of a fabricated number.
class MixCraftPanel final : public juce::Component {
  public:
    MixCraftPanel() = default;
    ~MixCraftPanel() override = default;

    void setScorecard(const MixScorecard& scorecard);
    void clear();

    int preferredHeight() const;

    void paint(juce::Graphics& g) override;

  private:
    MixScorecard scorecard_;
    bool hasData_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixCraftPanel)
};

} // namespace khensu
