#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/ListenerHandle.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <functional>
#include <vector>
#include <khensu/common/ThreadContext.h>
#include <khensu/analysis/AnalysisResult.h>
#include "SuggestionListPanel.h"
#include "GainStagingPanel.h"
#include "StreamingImpactPanel.h"
#include "ProvenanceTimelinePanel.h"
#include "SnapshotHistoryPanel.h"
#include "SignalFlowRackPanel.h"
#include "CompressionXrayPanel.h"
#include "RenderTreePanel.h"
#include "PresetLibraryPanel.h"
#include "HumanizePanel.h"
#include "../Dashboard/CardGroup.h"
#include "StemRebalancePanel.h"
#include <tuple>
#include "WhatChangedPanel.h"
#include "../shared/ListeningRoomPanel.h"
#include <khensu/ui/EqCurveComponent.h>
#include "../shared/SegmentedControl.h"
#include "../shared/SeverityLegendComponent.h"

namespace khensu {

class IAppController;
enum class ReportMode;

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class MixMasterScreen final : public juce::Component,
                              public juce::DragAndDropContainer,
                              public juce::DragAndDropTarget {
  public:
    explicit MixMasterScreen(IAppController& controller);
    ~MixMasterScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Keyboard-driven "next issue": forwards to SuggestionListPanel. Returns
    /// false if there's nothing unresolved to jump to.
    bool focusNextIssue();

    void jumpToCard(int tabIdx);

    // juce::DragAndDropTarget - right-stack card reordering.
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;

  private:
    void pushDataToPanels(const AnalysisResult& r);
    void updateState();
    void applyReportMode(ReportMode mode);

    IAppController& controller;

    ScreenHeader titleLabel; // UX-3: title = the sidebar label
    juce::Label statusLabel;
    juce::Label warningLabel;
    juce::TextButton loadTrackButton_; // UX-5: the empty state's one action
    SeverityLegendComponent severityLegend_; ///< mix-note colour key (spec §4)

    SuggestionListPanel suggestionPanel;
    GainStagingPanel gainStagingPanel;
    EqCurveComponent eqCurvePanel;
    juce::TextButton applyEqButton_{TRANS("Apply EQ")};
    // Live copy of eqCurvePanel's bands, kept in sync via onBandChanged -
    // dragging a handle previously only logged a FeedbackManager override
    // for preference learning; this is what lets "Apply EQ" write the
    // user's actual edits back to a real render instead of just the
    // recommendation-suggested starting points.
    std::vector<EqBand> currentEqBands_;
    StreamingImpactPanel streamingPanel_;
    ProvenanceTimelinePanel timelinePanel_;
    SnapshotHistoryPanel snapshotPanel_;
    SignalFlowRackPanel signalFlowRackPanel_;
    CompressionXrayPanel compressionXrayPanel_;
    RenderTreePanel renderTreePanel_; // shares the SNAPSHOTS tab
    PresetLibraryPanel presetLibraryPanel_;
    HumanizePanel humanizePanel_;       // §5.2 Artifacts/Humanize card
    StemRebalancePanel stemRebalancePanel_{controller}; // FEAT-4
    /// PERF-5: (session, result version, render-tree size, current version)
    /// at the last screen entry that refreshed the cards.
    std::tuple<std::string, uint64_t, size_t, int> lastEntryKey_{std::string{}, 0, 0, -2}; // -2: never seen
    WhatChangedPanel whatChangedPanel_; // §9.3 before->after diff card
    ListeningRoomPanel listeningRoomPanel_{controller}; // CE-COL-3 blind LAN A/B room
    /// CON-5: one Versions card - what changed, the render tree, snapshots
    /// and the provenance timeline as tabs (they were four boxes).
    CardGroup versionsGroup_{"versions", TRANS("Versions")};
    /// CON-2: gain staging and the streaming impact, one card with tabs.
    CardGroup loudnessGroup_{"loudness", TRANS("Loudness & streaming")};

    // §7: the right column is a scrollable stack of individual cards -
    // Variant Compare, EQ Curve, Timeline, Snapshots, Signal Rack, X-Ray and
    // Presets are all visible at once instead of tab-gated one-at-a-time.
    // The segmented control survives as jump-to-card navigation.
    juce::Viewport rightViewport_;
    juce::Component rightContent_;
    void layoutRightStack();

    // Data-driven ordered section list (drag reorder, shipped). Each
    // section owns its own layout closure so both simple single-card rows and
    // the two compound rows (EQ curve + Apply button; Snapshots/Render-Tree
    // side-by-side pair) share one reorderable list instead of the previous
    // hand-rolled sequence of `place()` calls. `card` is non-null only for
    // draggable, collapsible single-card sections; compound sections have a
    // null card and are not drag sources (no single header to arm the drag
    // from) but can still be reordered around by dragging the cards next to
    // them.
    struct RightStackSection {
        juce::String id;
        PanelCard* card = nullptr;
        std::function<int()> heightFor;
        std::function<void(juce::Rectangle<int>)> layout;
        bool draggable = true;
    };
    std::vector<RightStackSection> sections_;
    void buildSections();
    void loadSectionOrder();
    void saveSectionOrder();
    int sectionIndexById(const juce::String& id) const;
    void reorderSection(const juce::String& draggedId, int insertBeforeIndex);

    // Recorded by layoutRightStack() each pass; used both to resolve a drop's
    // target index and to draw the insertion-line indicator while dragging.
    std::vector<juce::Rectangle<int>> sectionRects_;
    int dragInsertBeforeIndex_ = -1; ///< -1 = not currently dragging over the stack

    SegmentedControl bottomTabControl_;
    enum BottomTab {
        TabVariant = 0, ///< CUT-2: the variant card is gone; this jumps to the top (Streaming)
        TabEqCurve = 1,
        TabTimeline = 2,
        TabSnapshots = 3,
        TabSignalRack = 4,
        TabXray = 5,
        TabPresets = 6,
        TabHumanize = 7
    };
    int bottomTab_ = TabVariant;
    int cardY_[8] = {}; ///< scroll anchors, filled by layoutRightStack()
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle analysisListener_;
    ListenerHandle abRenderAppliedListener_;
    ListenerHandle reportModeListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixMasterScreen)
};

} // namespace khensu
