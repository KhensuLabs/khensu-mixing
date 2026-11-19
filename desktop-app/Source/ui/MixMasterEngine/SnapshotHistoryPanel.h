// desktop-app/Source/ui/MixMasterEngine/SnapshotHistoryPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <vector>
#include <memory>

namespace khensu {

class IAppController;

/// Named-revision snapshot history, beyond the two-slot A/B compare -
/// backed by the existing provenance event layer (GET/POST /events,
/// event_type == "snapshot_created"). Each row offers Restore (reloads the
/// snapshot's rendered file via AppController::registerAppliedRender, which
/// already drives the undo/redo render history) and Compare-to-current
/// (shows a metric diff against the live analysis).
class SnapshotHistoryPanel final : public PanelCard {
  public:
    explicit SnapshotHistoryPanel(IAppController& c);
    ~SnapshotHistoryPanel() override;

    /// Reloads the snapshot list for the current session (async).
    void refresh();
    void resized() override;

  private:
    struct Row {
        juce::String when; // HH:MM
        juce::String label;
        juce::String renderedPath;
        bool hasMetrics = false;
        double lufsIntegrated = 0.0;
        double truePeakDbtp = 0.0;
        double dynamicRangeDb = 0.0;
        double stereoWidth = 0.0;
    };
    void rebuild(std::vector<Row> rows);
    /// Session the listed rows were fetched for - Restore registers into it
    /// only (AUDIT-042).
    juce::String rowsSessionId_;
    void compareToCurrent(const Row& row);

    juce::TextEditor labelEditor_;
    juce::TextButton saveButton_{TRANS("Save Snapshot")};

    juce::Viewport viewport_;
    juce::Component content_;

    /// Illustrative empty state (a simple drawn "camera" glyph + heading +
    /// subtext) shown before any snapshot has been saved for this session,
    /// instead of a bare line of muted text.
    struct EmptyStateComponent final : public juce::Component {
        void paint(juce::Graphics& g) override;
    };
    EmptyStateComponent emptyState_;

    struct RowComp;
    std::vector<std::unique_ptr<RowComp>> rows_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnapshotHistoryPanel)
};

} // namespace khensu
