// desktop-app/Source/ui/shared/ExplainMomentView.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <functional>
#include <vector>

namespace khensu {

class IAppController;

/// CE-AI-6 "Explain this moment": the content of the call-out opened from a
/// waveform selection. Measures the region against the whole track (and the
/// session genre's norm, and the separated stems when present) through
/// /explain/moment and shows the ranked, plain-English deviations. "Reword"
/// asks the configured LLM to rephrase the measured facts only (rejected by
/// the service if it adds numbers); "Find similar passages" runs the CE-AI-1
/// in-track section search and offers each hit as a jump target.
/// THREAD_CONTEXT(UIThread)
class ExplainMomentView final : public juce::Component {
  public:
    ExplainMomentView(IAppController& controller, juce::String filePath, double startS, double endS);

    /// Called with (start, end) when the user picks a similar passage.
    std::function<void(double, double)> onJumpTo;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    struct Deviation {
        juce::String label, value;
        double score = 0.0;
        bool notable = false;
    };
    struct Passage {
        double start = 0.0, end = 0.0, similarity = 0.0;
    };

    void request(bool rephrase);
    void findSimilarPassages();

    IAppController& controller_;
    juce::String filePath_;
    double startS_, endS_;

    juce::Label titleLabel_;
    juce::TextEditor text_;
    juce::Label noteLabel_;
    juce::TextButton rewordButton_{TRANS("Reword")};
    juce::TextButton similarButton_{TRANS("Find similar passages")};
    juce::OwnedArray<juce::TextButton> passageButtons_;
    std::vector<Deviation> deviations_;
    std::vector<Passage> passages_;

    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplainMomentView)
};

} // namespace khensu
