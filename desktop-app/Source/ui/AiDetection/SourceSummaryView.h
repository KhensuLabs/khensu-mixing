// desktop-app/Source/ui/AiDetection/SourceSummaryView.h
// CON-8: the Provenance & Source screen's Source view - where the audio came
// from and what state it is in, from the session's own follow-up scans
// (source triage, forensics, signal integrity, AI evidence; WF-14's scan
// store). The scans run with the analysis and their full cards are the
// Analysis screen's "Provenance & source" card; this reads their results in
// one place next to the AI-detection page.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener

namespace khensu {

class IAppController;

class SourceSummaryView final : public juce::Component, private juce::Timer {
  public:
    explicit SourceSummaryView(IAppController& c);
    ~SourceSummaryView() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Re-reads the scan store.
    void refresh();

    struct Section {
        juce::String title;
        juce::StringArray lines; ///< "Field: value"; empty = not run yet
    };
    /// The sections for a scan store (pure; tests read it).
    static std::vector<Section> sectionsFor(const juce::var& scans);

  private:
    void timerCallback() override { refresh(); }

    IAppController& controller_;
    std::vector<Section> sections_;
    juce::TextButton detailsButton_{TRANS("Full cards on the Analysis screen")};

    void applyThemeColours() {}
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] { repaint(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceSummaryView)
};

} // namespace khensu
