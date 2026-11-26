#pragma once
#include "../shared/LiveSpectrogramView.h"
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
#include "PresetBar.h"
#include "SectionTabs.h"
#include "../shared/MeterBridge.h"
#include "HumanizePanel.h"
#include "../Dashboard/CardGroup.h"
#include "StemRebalancePanel.h"
#include <tuple>
#include "WhatChangedPanel.h"
#include "../shared/ListeningRoomPanel.h"
#include <khensu/ui/EqCurveComponent.h>
#include "../shared/SeverityLegendComponent.h"
#include "../shared/LivePreviewToggle.h"
#include "../../services/EqBandChain.h"

namespace khensu {

class IAppController;
enum class ReportMode;

// UX2-2: one focal area. The signal chain (the plugin's DSP rack, with the
// preset bar and rack A/B on top) is the hero; a right rail holds the large
// meters and the three most severe open recommendations; everything else -
// loudness (gain staging over the streaming audition), the EQ editor, versions (what changed,
// render tree, snapshots, timeline), dynamics (the gain X-ray) and presets &
// tools (preset library, humanize, stem rebalance, listening room) - is a row
// of five sentence-case tabs under the rack.
// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class MixMasterScreen final : public juce::Component {
  public:
    explicit MixMasterScreen(IAppController& controller);
    ~MixMasterScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Keyboard-driven "next issue": forwards to SuggestionListPanel. Returns
    /// false if there's nothing unresolved to jump to.
    bool focusNextIssue();

    /// The legacy card index (IShell::showMixMasterCard): 0 streaming, 1 EQ
    /// editor, 2 timeline, 3 snapshots, 4 signal rack, 5 X-ray, 6 presets,
    /// 7 humanize, 8 gain staging - each opens the view that holds it.
    void jumpToCard(int tabIdx);

    /// The secondary views, in tab order.
    enum View { ViewLoudness = 0, ViewEq = 1, ViewVersions = 2, ViewDynamics = 3, ViewTools = 4, ViewLive = 5, kViewCount = 6 };
    void showView(int view);
    int currentView() const noexcept { return view_; }

  private:
    void pushDataToPanels(const AnalysisResult& r);
    void updateState();
    void applyReportMode(ReportMode mode);
    void layoutView(juce::Rectangle<int> area);

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
    // Live copy of eqCurvePanel's bands, kept in sync via onBandChanged, so
    // "Apply EQ" renders the user's actual edits, not just the suggestions.
    std::vector<EqBand> currentEqBands_;
    /// STU-9: hear the EQ curve while dragging it; Apply EQ still renders.
    LivePreviewToggle eqPreview_{controller, TRANS("EQ curve"), [this] { return eqBandsToChain(currentEqBands_); }};
    StreamingImpactPanel streamingPanel_;
    ProvenanceTimelinePanel timelinePanel_;
    SnapshotHistoryPanel snapshotPanel_;
    SignalFlowRackPanel signalFlowRackPanel_;
    CompressionXrayPanel compressionXrayPanel_;
    RenderTreePanel renderTreePanel_;
    PresetLibraryPanel presetLibraryPanel_;
    HumanizePanel humanizePanel_;       // §5.2 Artifacts/Humanize card
    StemRebalancePanel stemRebalancePanel_{controller}; // FEAT-4
    PresetBar presetBar_{controller};                                 ///< UI-9: name, arrows, menu, rack A/B
    MeterBridge meterCard_{controller, MeterBridge::Layout::Large}; ///< UI-5: the large meter view
    LiveSpectrogramView liveSpectrogram_{controller}; ///< VIZ-4a: the Live view
    /// PERF-5: (session, result version, render-tree size, current version)
    /// at the last screen entry that refreshed the cards.
    std::tuple<std::string, uint64_t, size_t, int> lastEntryKey_{std::string{}, 0, 0, -2}; // -2: never seen
    WhatChangedPanel whatChangedPanel_; // §9.3 before->after diff card
    ListeningRoomPanel listeningRoomPanel_{controller}; // CE-COL-3 blind LAN A/B room
    /// CON-5: one Versions view - what changed, the render tree, snapshots
    /// and the provenance timeline as tabs.
    CardGroup versionsGroup_{"versions", TRANS("Versions")};
    /// UX2-2: the preset library and the occasional tools, one view.
    CardGroup toolsGroup_{"tools", TRANS("Presets & tools")};

    SectionTabs viewTabs_;
    int view_ = ViewLoudness;
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
