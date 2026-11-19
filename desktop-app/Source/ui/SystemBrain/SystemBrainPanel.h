// desktop-app/Source/ui/SystemBrain/SystemBrainPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/common/ThreadContext.h>
#include "FeatureCardComponent.h"
#include "FeatureDetailPanel.h"
#include <memory>
#include <vector>

namespace khensu {

class IAppController;

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class SystemBrainPanel final : public juce::Component, private juce::Timer {
  public:
    explicit SystemBrainPanel(IAppController& controller);
    ~SystemBrainPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void selectFeature(const std::string& id);
    void applySessionValue(const std::string& id); // feeds detail panel's session section
    void updateState();

  private:
    /// Search debounce (audit 11.3): every keystroke used to rebuild every
    /// feature card; the list now refreshes once typing pauses.
    void timerCallback() override;
    static constexpr int kSearchDebounceMs = 200;
    void rebuildCategoryChips();
    void refreshFeatureList(); // filters by searchText_ and activeCategory_, repopulates list
    void layoutCards();

    IAppController& controller;

    ScreenHeader titleLabel_; // UX-3: title = the sidebar label
    // Live service diagnostics from GET /analyze/metadata - versions of the
    // Python service, its analysis libraries, and the analysis schema, so
    // "what is the brain actually running" is answerable from inside the app.
    juce::Label serviceInfoLabel_;
    void fetchServiceInfo();

    juce::TextEditor searchInput_;
    juce::String searchText_;
    juce::String activeCategory_; // "" means All

    static const char* kCategories[];

    juce::OwnedArray<juce::TextButton> categoryChips_; // [0] = "All"

    juce::Viewport featureListViewport_;
    juce::Component listContent_;
    std::vector<std::unique_ptr<FeatureCardComponent>> cards_;

    // §6: the detail column scrolls - long feature entries (measured, fully
    // wrapped text) grow past the window instead of overlapping sections.
    juce::Viewport detailViewport_;
    FeatureDetailPanel detailPanel_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SystemBrainPanel)
};

} // namespace khensu
