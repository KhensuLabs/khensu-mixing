// desktop-app/Source/ui/ReferenceLibrary/GenreFitChart.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <cmath>
#include <functional>
#include <vector>

namespace khensu {

/// UXR-4: the comparative genre ranking as a chart - how close the loaded mix
/// sits to each genre's measured norms (integrated loudness, loudness range,
/// spectral tilt and crest factor), closest first, one labelled horizontal bar
/// per genre. A bar's length is the match: 100% at the genre's targets,
/// falling with the normalised distance (exp(-d / 2)). Clicking a bar picks
/// that genre. Paints everything live from the theme.
/// THREAD_CONTEXT(UIThread)
class GenreFitChart final : public juce::Component, public juce::SettableTooltipClient {
  public:
    struct Entry {
        juce::String genreId;
        juce::String name;
        float distance = 0.0f; ///< normalised Euclidean distance to the genre's norms
    };

    GenreFitChart();

    /// Closest first. Empty shows `emptyText`.
    void setEntries(std::vector<Entry> entries, const juce::String& emptyText = {});
    /// The genre picked on the screen, outlined when it is in the chart.
    void setSelectedGenre(const juce::String& genreId);
    std::function<void(const juce::String& genreId)> onGenreClicked;

    /// 0..1 match of a distance (exposed for tests).
    static float matchOf(float distance) { return std::exp(-juce::jmax(0.0f, distance) / 2.0f); }

    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    juce::Rectangle<int> barsArea() const;
    int rowAt(juce::Point<int> p) const;
    int visibleRows() const;

    std::vector<Entry> entries_;
    juce::String emptyText_;
    juce::String selected_;
    int hover_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenreFitChart)
};

} // namespace khensu
