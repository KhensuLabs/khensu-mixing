// desktop-app/Source/ui/MixMasterEngine/WhatChangedPanel.h
// "What Changed" (spec §9.3): per-metric before -> after deltas between the
// session's original audio and the current A/B "B" render, computed from
// the server-cached /analyze/quick_stats + /analyze/artifacts_scan results
// (no new analyzers - a pure diff over two cached result sets). Refreshes
// automatically every time a render is applied.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ui/ChartKit.h>
#include <vector>

namespace khensu {

class IAppController;

class WhatChangedPanel final : public PanelCard {
  public:
    explicit WhatChangedPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Recompute the diff for the current A/B pair. Called on render apply
    /// and on screen entry; self-guards when there is no processed render.
    void refresh();

  private:
    struct Row {
        juce::String label, before, after;
        float delta = 0.0f; // after - before, native units
        int direction = 0;  // +1 improved, -1 worse (written as words), 0 neutral
        juce::String unit;
    };

    /// A resolved/new mix-note diff line, from the two artifact scans' finding
    /// summaries already fetched for the metric diff (no extra network call).
    struct NoteRow {
        juce::String text;
        bool resolved = false; // true = green "resolved", false = amber "new"
    };

    juce::String getSelectedPath(juce::ComboBox& combo) const;
    void populateCombos();

    juce::Label statusLabel_;
    juce::ComboBox beforeCombo_;
    juce::ComboBox afterCombo_;
    std::vector<Row> rows_;
    std::vector<NoteRow> noteRows_;
    ui::chart::ChartSummary summary_; ///< VIS-8
    juce::String diffedPair_; ///< "before|after" guard against re-fetch
    bool fetchInFlight_ = false;
    bool afterPickedByUser_ = false;
    bool populatingCombos_ = false;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WhatChangedPanel)
};

} // namespace khensu
