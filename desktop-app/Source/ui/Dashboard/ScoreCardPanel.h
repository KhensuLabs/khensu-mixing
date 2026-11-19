// desktop-app/Source/ui/Dashboard/ScoreCardPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/recommendations/MixScorecard.h>
#include <array>

namespace khensu {

/// Hero readiness card: big 0-100 heuristic technical/genre-alignment number,
/// a bounded readiness-band label (NOT a quality/skill claim - 2026-07-15
/// safety audit P0), and a collapsible list of all six bands with the current
/// one highlighted ("<- you"). Purely a presentation layer over MixScorecard.
class ScoreCardPanel final : public juce::Component {
  public:
    ScoreCardPanel();
    ~ScoreCardPanel() override = default;

    void setScore(const MixScorecard& scorecard);
    void clear();

    /// Total height this panel needs given its current bands-visible state -
    /// callers must re-run their layout after toggling bands since this changes.
    int preferredHeight() const;

    /// Fired when the bands list is shown/hidden, since that changes
    /// preferredHeight() - the owning screen should re-run its layout.
    std::function<void()> onLayoutChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    struct Band {
        int lo, hi;
        juce::String label, audience;
    };
    static const std::array<Band, 6>& bands();

    float score_ = 0.0f;
    juce::String tierLabel_;
    juce::String tierAudience_;
    bool hasData_ = false;
    bool bandsVisible_ = true;

    juce::TextButton toggleBandsButton_{TRANS("Hide Score Bands")};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreCardPanel)
};

} // namespace khensu
