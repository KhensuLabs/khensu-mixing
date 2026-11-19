// desktop-app/Source/ui/Dashboard/WeightProfileOverlay.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ModalCardHost.h"
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <memory>
#include <vector>

namespace khensu {

/// Modal editor for a genre's ScoreWeightProfile - adjustable Mastering
/// Readiness penalty caps and Mix Scorecard category weights, saved per
/// genre via FeedbackManager's preference store (see ScoreWeightProfile).
class WeightProfileOverlay final : public ModalCardHost {
  public:
    WeightProfileOverlay(const ScoreWeightProfile& initial, const juce::String& genreLabel);

    void resized() override;

    std::function<void(const ScoreWeightProfile&)> onSave;
    std::function<void()> onCancel;

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onCancel) onCancel();
    }

  private:
    struct Row {
        juce::Label label;
        juce::Slider slider;
        float ScoreWeightProfile::* field;
    };
    void addRow(const juce::String& label, float ScoreWeightProfile::* field, double minV, double maxV);

    ScoreWeightProfile current_;

    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton saveButton_{TRANS("Save")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton resetButton_{TRANS("Reset to Defaults")};
    juce::Viewport viewport_;
    juce::Component rowsContainer_;
    std::vector<std::unique_ptr<Row>> rows_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WeightProfileOverlay)
};

} // namespace khensu
