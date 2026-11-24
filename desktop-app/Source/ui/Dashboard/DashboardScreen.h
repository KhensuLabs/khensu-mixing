// desktop-app/Source/ui/Dashboard/DashboardScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/ListenerHandle.h"
#include <khensu/common/ThreadContext.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/session/SessionManager.h>
#include "SummaryPanel.h"
#include "LoudnessPanel.h"
#include "DynamicsPanel.h"
#include "SpectralPanel.h"
#include "StereoFieldPanel.h"
#include "PhasePanel.h"
#include "ArtifactPanel.h"
#include "TranslationPanel.h"
#include "MixNotesPanel.h"
#include "SonicFingerprintPanel.h"
#include "HookDashboard.h"
#include "MemorabilityPanel.h"
#include "StructuralFlowPanel.h"
#include "HeavinessPanel.h"
#include "ReferenceComparePanel.h"
#include "ChordsPanel.h"
#include "LoudnessArcPanel.h"
#include "SongInfoPanel.h"
#include "VocalInfoPanel.h"
#include "EmotionalArcPanel.h"
#include "BenchmarkPanel.h"
#include "GenreDistributionPanel.h"
#include "GenreFitPanel.h"
#include "CardGroup.h"
#include "SpectrogramPanel.h"
#include "WaveformEditorPanel.h"
#include "../shared/PanelDockManager.h"
#include "EqCurvePanel.h"
#include "SpatialAudioPanel.h"
#include "DrumPatternPanel.h"
#include "LoudnessCurvePanel.h"
#include "TonalBalancePanel.h"
#include "SourceTriagePanel.h"
#include "SignalIntegrityPanel.h"
#include "TaxonomyEnergyPanel.h"
#include "TaxonomyMaskingPanel.h"
#include "AiArtifactsPanel.h"
#include "SubBassPhasePanel.h"
#include "StreamingNormalizationPanel.h"
#include "SourceForensicsPanel.h"
#include "TuningReferencePanel.h"
#include "MonoCompatPanel.h"
#include "MidSideSpectrogramPanel.h"
#include "PhaseHeatmapPanel.h"
#include "StemEnergyTimelinePanel.h"
#include "ReferenceSpectralDeltaPanel.h"
#include "RecreateSoundPanel.h"
#include "KhensuVerdictCard.h"
#include "../shared/HeroSpectrumView.h"
#include "SongTimelineCard.h"
#include "../shared/ServiceHealthWidget.h"
#include "ArrangementPanel.h"
#include "DepthLadderPanel.h"
#include "PerformanceAnalysisPanels.h"
#include "PanBalancePanel.h"
#include "SectionReadinessPanel.h"
#include "MixCraftPanel.h"
#include "IssueHeatmapPanel.h"
#include "WorkspaceLayout.h"
#include "DashboardLayoutPanel.h"
#include "../shared/MetricTileComponent.h"
#include "../shared/ShimmerComponent.h"
#include "../shared/PipelineProgressBar.h"
#include "../shared/MetricDiffOverlay.h"
#include "../shared/SessionScoped.h"
#include "../../IAppController.h" // ScopedSessionRegistration
#include <vector>
#include <memory>
#include <map>
#include <string>
#include "../shared/KhensuTheme.h"
#include "../shared/SegmentedControl.h"

namespace khensu {

class IAppController;

/// Zone 3's scrollable grid, upgraded to a juce::DragAndDropTarget so
/// PanelCard::setDraggable() cards can be reordered by mouse drag instead of
/// only the DashboardLayoutPanel modal's Up/Down buttons. Drop handling is
/// centralized here (rather than on each of the 13 panel subclasses) since
/// the drop target/index only depends on which panel's bounds the pointer is
/// over, not on anything panel-type-specific.
class Zone3Grid final : public juce::Component, public juce::DragAndDropTarget {
  public:
    std::function<bool(const SourceDetails&)> isInterested;
    std::function<void(const SourceDetails&)> onDragMove;
    std::function<void(const SourceDetails&)> onDrop;
    std::function<void()> onDragExit;

    std::function<juce::Rectangle<int>()> hoverHighlightBounds;

    bool isInterestedInDragSource(const SourceDetails& d) override { return isInterested && isInterested(d); }
    void itemDragMove(const SourceDetails& d) override
    {
        if (onDragMove) onDragMove(d);
    }
    void itemDropped(const SourceDetails& d) override
    {
        if (onDrop) onDrop(d);
    }
    void itemDragExit(const SourceDetails&) override
    {
        if (onDragExit) onDragExit();
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (!hoverHighlightBounds) return;
        auto r = hoverHighlightBounds();
        if (r.isEmpty()) return;
        g.setColour(KhensuTheme::accentTeal);
        g.drawRoundedRectangle(r.toFloat().expanded(3.0f), 6.0f, 2.5f);
    }
};

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class DashboardScreen final : public juce::Component, public juce::DragAndDropContainer, private SessionScoped {
  public:
    explicit DashboardScreen(IAppController& controller);
    ~DashboardScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    /// S4 session-reset fan-out: every panel forgets the previous session,
    /// auto-scan bookkeeping and snapshots reset (DSK-02/-03/-04).
    void resetForSession() override;
    void runMixLevelAutoScans(bool countAsFollowUps);
    void loadPreferencesAsync();
    void restorePoppedOutPanels();
    /// Writes a user preference off the message thread (DSK-20); at shutdown
    /// (pools already drained) it writes synchronously.
    void writePreferenceAsync(const std::string& key, const std::string& value);
    std::string currentWeightProfileId() const;
    /// UI-10: keyboard legend, completeness chip and per-stage bar are
    /// shown only in Engineer Detail.
    bool engineerDetail() const;
    /// OVH-1: the verdict from the derived data and the issue queue.
    void refreshVerdict();
    bool prefsLoaded_ = false;
    void onAnalysisUpdate(const AnalysisResult& r);
    void pushDataToPanels(const AnalysisResult& r);
    /// (had a result, its version) at the last push - PERF-5.
    std::pair<bool, uint64_t> lastPushed_{false, ~uint64_t{0}};
    void refreshDrumsFromStem(); ///< §2.1: re-run groove on the drum stem
    void layoutZone3();
    void saveDockState();
    static constexpr const char* kDockStatePrefKey = "dashboard_dock_state";
    std::unique_ptr<PanelDockManager> dock_;
    bool dockRestored_ = false;
    void updateHeader();
    void updateCompletenessBadge();
    juce::String getSpinnerText(const juce::String& base) const;

    IAppController& controller;

    ScreenHeader titleLabel; // UX-3: title = the sidebar label
    juce::Label statusLabel; // "awaiting analysis" / "running deep..." / "complete"
    juce::Label shortcutLegendLabel_;
    juce::Label warningLabel;    // "no active session"
    juce::TextButton loadTrackButton_; // UX-5: the empty state's one action
    juce::Label conflictBanner_; // cross-panel conflict warning banner
    PipelineProgressBar pipelineProgress_;
    juce::Viewport viewport;
    Zone3Grid grid; // holds all panels, lives inside viewport

    // Zone 3 category tabs (Gemini audit): filter the grid to one workspace
    // (Loudness / Spectrum / Spatial / Song & Structure / Stems / Quality)
    // instead of scrolling all 30+ cards. "All" keeps the full grid. The tab
    // strip sits above the grid cards and reuses the existing order/visibility/
    // drag-reorder machinery - it only narrows which panels are active.
    SegmentedControl zone3Tabs_; ///< UX-17: the one themed segment control
    // UX-1: "Essentials" (the default) shows about eight cards picked from the
    // result's findings (EssentialsSelection.h); "All" shows every card.
    juce::String activeZone3Category_{"Essentials"};
    std::vector<std::string> essentialIds_;
    juce::String categoryForPanel(const std::string& id) const;

    juce::String dragHoverId_; // Zone 3 panel id currently under a drag, or empty
    bool findDropTarget(juce::Point<int> pos, std::string& outTargetId, bool& outBefore) const;
    void reorderZone3Panel(const std::string& draggedId, const std::string& targetId, bool before);

    // "Timeline" tab: cross-domain issue heatmap, alternate to the Zone 3 grid.
    juce::TextButton timelineToggleButton_{TRANS("Timeline View")};
    IssueHeatmapPanel issueHeatmap_{controller};
    bool showTimeline_ = false;

    // Confidence & Completeness Overlay: dims/hatches not-yet-analyzed Zone 3
    // panels and shows a "X% complete" badge based on their analysis state.
    // No standalone button - toggled from overflowButton_'s popup menu.
    juce::Label completenessBadge_;
    bool confidenceOverlayEnabled_ = false;
    void toggleConfidenceOverlay(bool enabled);
    // OVH-7 (agent D): "Show advanced cards" in the overflow menu - the
    // Advanced category's cards join "All" (dashboard_show_advanced pref).
    bool showAdvancedCards_ = false;

    // Overflow menu: Customize Layout / Tune Weights / Confidence Overlay -
    // consolidated here instead of each being its own button, since the
    // status row previously packed 5 sub-Fitts-sized controls into 18px.
    juce::TextButton overflowButton_{"..."};
    juce::ComboBox weightProfileCombo_;

    // Genre-Tuned Readiness/Scorecard Weighting Profiles editor.
    std::unique_ptr<juce::Component> weightEditorOverlay_;
    juce::String loadedSessionId_;

    // Custom Dashboard layouts / User Profiles / Workspace Modes.
    std::unique_ptr<DashboardLayoutPanel> layoutEditorOverlay_;
    std::vector<std::string> zone3PanelIds_; // parallel to zone3Panels_, stable ids
    std::vector<std::string> zone3Order_;    // current display order (subset of zone3PanelIds_)
    std::map<std::string, bool> zone3Visibility_;
    std::map<juce::String, WorkspaceLayout> builtInLayoutPresets() const;
    juce::Component* panelById(const std::string& id) const;
    juce::String panelLabel(const std::string& id) const;
    void applyWorkspaceLayout(const WorkspaceLayout& layout, bool persist = true);
    void openWeightEditor();
    void openLayoutEditor();
    void openLayoutEditorWith(const std::vector<juce::String>& savedNames);
    void showCompareToLastSession(const std::vector<SessionManager::AnalysisSummary>& summaries, bool loadFailed);
    void setupTileContextMenus();

    SummaryPanel summary_{controller};
    LoudnessPanel loudness_{controller};
    DynamicsPanel dynamics_{controller};
    SpectralPanel spectral_{controller};
    StereoFieldPanel stereo_{controller};
    PhasePanel phase_{controller};
    ArtifactPanel artifact_{controller};
    TranslationPanel translation_{controller};
    SonicFingerprintPanel fingerprint_{controller};
    HookDashboard hooks_{controller};
    MemorabilityPanel memorability_{controller};
    StructuralFlowPanel structural_{controller};
    HeavinessPanel heaviness_{controller};
    MixNotesPanel mixNotes_{controller};
    ReferenceComparePanel reference_{controller};
    ChordsPanel chords_{controller};
    LoudnessArcPanel loudnessArc_{controller};
    SongInfoPanel songInfo_{controller};
    VocalInfoPanel vocalInfo_{controller};
    EmotionalArcPanel emotionalArc_{controller};
    BenchmarkPanel benchmark_{controller};
    GenreDistributionPanel genreDist_{controller};
    GenreFitPanel genreFit_{controller}; // NEW-3
    SpectrogramPanel spectrogram_{controller};
    WaveformEditorPanel waveformEditor_{controller};
    EqCurvePanel eqCurvePanel_{controller};
    SpatialAudioPanel spatialAudio_{controller};
    DrumPatternPanel drumPattern_{controller};
    LoudnessCurvePanel loudnessCurve_{controller};
    TonalBalancePanel tonalBalance_{controller};
    SourceTriagePanel sourceTriage_{controller};
    SignalIntegrityPanel signalIntegrity_{controller};
    ArrangementPanel arrangement_{controller};
    DepthLadderPanel depthLadder_{controller};
    PanBalancePanel panBalance_{controller};
    SectionReadinessPanel sectionReadiness_{controller};
    TaxonomyEnergyPanel taxonomyEnergy_{controller};
    TaxonomyMaskingPanel taxonomyMasking_{controller};
    AiArtifactsPanel aiArtifacts_{controller};
    SubBassPhasePanel subBassPhase_{controller};
    StreamingNormalizationPanel streamingNorm_{controller};
    SourceForensicsPanel sourceForensics_{controller};
    TuningReferencePanel tuningRef_{controller};
    MonoCompatPanel monoCompat_{controller};
    MidSideSpectrogramPanel midSideSpec_{controller};
    PhaseHeatmapPanel phaseHeatmap_{controller};
    StemEnergyTimelinePanel stemEnergyTl_{controller};
    ReferenceSpectralDeltaPanel refSpecDelta_{controller};
    RecreateSoundPanel recreateSound_{controller};
    // On-demand cards for the formerly orphan performance routes (§9.2).
    GrooveTimingPanel grooveTiming_{controller};
    HarmonyTimelinePanel harmonyTimeline_{controller};
    ReverbTailPanel reverbTail_{controller};
    StemPhasePanel stemPhase_{controller};

    // Zone 0: hero scorecards, above the critical metrics strip
    KhensuVerdictCard verdictCard_; // kimi P1: headline readiness verdict
    // UX2-1: the Issues screen's hero spectrum under the verdict - the
    // findings drawn where they live; a marker opens it on the Issues screen.
    HeroSpectrumView hero_;
    bool referenceRequested_ = false; ///< VIZ-N1: measuring the reference once
    SongTimelineCard songTimeline_{controller}; ///< VIZ-2 where in the song
    void updateHero();
    ServiceHealthWidget serviceHealthWidget_;
    MixCraftPanel mixCraft_;
    // UX2-1: quiet captions over the detail sections, and the waveform /
    // spectrogram editor as a collapsible section at the bottom.
    juce::Label detailCaption_, measurementsCaption_;
    juce::TextButton waveformToggle_;
    bool waveformOpen_ = false;
    void setWaveformOpen(bool open, bool persist);

    int spinnerTick_ = 0; ///< Advances on each partial update to animate the status spinner

    // Intake auto-run bookkeeping (spec §1.2): the mix-level scans fire once
    // per analysis RUN (AppController::analysisRunToken - DSK-03: keyed on
    // the path, a re-analysis of the same file never re-ran them), the
    // stem-level ones once per source file when real stems land.
    int lastAutoScanRunToken_ = -1;     ///< mix-level scans (triage/integrity/pan)
    juce::String lastStemAutoScanPath_; ///< stem-level scans (arrangement/depth)

    // Zone 3 panels (inside viewport/grid - laid out by layoutZone3())
    // Technical Quality and Creative/Song Structure panels are all shown
    // together - no tab control gating their visibility.
    std::vector<juce::Component*> zone3Panels_;

    // CON-2..CON-8: cards merged into one card with tabs. zone3Panels_ /
    // zone3PanelIds_ still list every card (session reset, completeness,
    // confidence overlay reach all of them); the layout works in "units" -
    // a group, or a card in no group - so zone3Order_ / zone3Visibility_ /
    // the layout presets hold unit ids, and a member's id resolves to its
    // group (canonicalId).
    std::vector<std::unique_ptr<CardGroup>> cardGroups_;
  public:
    /// --screenshot-audio: expands Zone 3 on "All", lays it out and returns
    /// the group cards (each cycled through its tabs by the caller).
    std::vector<std::pair<juce::String, juce::Component*>> groupCardsForDiagnostics();
  private:
    std::map<std::string, CardGroup*> groupOfMember_;
    std::vector<std::string> zone3UnitIds_; ///< layout units in default order
    void buildCardGroups();
    std::string canonicalId(const std::string& id) const;
    CardGroup* groupById(const std::string& id) const;
    /// Unit ids from any mix of member and unit ids, first occurrence kept.
    std::vector<std::string> canonicalIds(const std::vector<std::string>& ids) const;

    // Zone 3 shimmer overlays - one per AI insight card
    ShimmerComponent shimmerHooks_, shimmerStructural_, shimmerTranslation_, shimmerMixNotes_;

    // Zone 1 - Critical Metrics Strip
    MetricTileComponent tileIntLufs_;
    MetricTileComponent tileTruePeak_;
    MetricTileComponent tileDynamicRange_;
    MetricTileComponent tilePhase_;
    MetricTileComponent tileClipping_;
    // OVH-1: the "MASTER READY" tile and its penalty bar are gone - they
    // repeated the headline with a second number and other thresholds.

    // App-wide Compare toggle: generalizes ReferenceComparePanel's per-dimension
    // delta pattern into Loudness/Dynamics/Spectral/Stereo panel bodies.
    juce::TextButton compareToggleButton_{TRANS("Compare")};
    // Shown only when the current analysis is C++-only (Python deep pass didn't
    // run) - a one-click manual re-run for the service-up-but-deep-failed case.
    juce::TextButton btnRerunDeep_{TRANS("Re-run Deep Analysis")};
    // UX-9: one Snapshots menu (save as A / B, compare) instead of three buttons.
    juce::TextButton snapshotsButton_{TRANS("Snapshots")};
    void showSnapshotsMenu();
    void compareSnapshots();
    std::optional<AnalysisResult> snapA_;
    std::optional<AnalysisResult> snapB_;
    std::unique_ptr<juce::Component> compareOverlay_;

    // Guided Fix Wizard overlay (kimi audit P2): step-through top recommendations.

    // Reference-track ritual helper: forwards match-EQ to the compare panel.
  public:
    void applyReferenceMatchEq();

  private:
    /// Re-applies every cached palette colour on the dashboard chrome. Called
    /// at construction and on each theme change.
    void applyThemeColours();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle pipelineProgressListener_;
    ListenerHandle reportModeListener_; ///< UX-1: Client mode collapses to the verdict
    ListenerHandle serviceRecoveredListener_;
    ListenerHandle vocalAnalysisListener_;
    ListenerHandle issuesListener_; ///< OVH-1: "Start here" follows the issue queue

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};
    // After the theme listener: deregistered first (S4).
    ScopedSessionRegistration sessionRegistration_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DashboardScreen)
};

} // namespace khensu
