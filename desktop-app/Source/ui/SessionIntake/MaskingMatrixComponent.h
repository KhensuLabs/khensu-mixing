// desktop-app/Source/ui/SessionIntake/MaskingMatrixComponent.h
#pragma once
#include <JuceHeader.h>
#include "MultitrackModel.h"
#include <functional>
#include <utility>
#include <vector>

namespace khensu {

/// Track-vs-track masking risk grid. Rows and columns are the tracks in
/// session order (numbered across the top, named down the side); a cell's
/// colour follows the shared severity scale (red high, amber moderate, blue
/// minor) and its tooltip names the band ("Kick vs Bass at 20-60 Hz").
// THREAD_CONTEXT(UIThread)
class MaskingMatrixComponent final : public juce::Component, public juce::SettableTooltipClient {
  public:
    MaskingMatrixComponent();

    void setAnalysis(const multitrack::Analysis* analysis); // nullptr clears
    void setHighlightedTrack(int index);
    std::function<void(int row)> onTrackClicked;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    /// Cell under a point as (row, col), or (-1, -1). Public for tests.
    std::pair<int, int> cellAt(juce::Point<int> p) const;

  private:
    juce::Rectangle<int> gridArea() const;
    const multitrack::Pair* pairFor(int row, int col) const;

    std::vector<juce::String> names_;
    std::vector<std::vector<double>> matrix_;
    std::vector<multitrack::Pair> pairs_;
    int highlighted_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MaskingMatrixComponent)
};

} // namespace khensu
