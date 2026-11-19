// desktop-app/Source/ui/MixMasterEngine/ProvenanceTimelinePanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <vector>

namespace khensu {

class IAppController;

/// "What changed and why" - reads the provenance event layer (GET /events)
/// for the active session and lists applied chains/renders, explanations, and
/// feedback in reverse-chronological order. Each entry links back to the
/// recommendation that triggered it.
class ProvenanceTimelinePanel final : public PanelCard {
  public:
    explicit ProvenanceTimelinePanel(IAppController& c);
    ~ProvenanceTimelinePanel() override;

    /// Reloads the timeline for the current session (async).
    void refresh();
    void resized() override;

  private:
    // Client-facing Revision Storyboard export: writes a standalone HTML
    // chronology of the session's client-visible changes (via
    // POST /report/storyboard) to a user-chosen path.
    juce::TextButton exportStoryboardButton_{TRANS("Export Storyboard...")};
    std::unique_ptr<juce::FileChooser> storyboardChooser_;
    void exportStoryboard();

  public:
  private:
    struct Row {
        juce::String when; // HH:MM
        juce::String type; // human label
        juce::String summary;
        juce::String recId;
        juce::Colour accent;
    };
    void rebuild(std::vector<Row> rows);

    juce::Viewport viewport_;
    juce::Component content_;
    juce::Label emptyLabel_;

    struct RowComp;
    std::vector<std::unique_ptr<RowComp>> rows_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProvenanceTimelinePanel)
};

} // namespace khensu
