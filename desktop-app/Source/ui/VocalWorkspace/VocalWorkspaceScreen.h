// desktop-app/Source/ui/VocalWorkspace/VocalWorkspaceScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../Dashboard/CardGroup.h"
#include "../shared/ListenerHandle.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/common/ThreadContext.h>
#include <khensu/analysis/AnalysisResult.h>
#include "VocalAnalysisPanel.h"
#include "VocalStrategyPanel.h"
#include "VocalMacroPanel.h"
#include "VocalHookPanel.h"
#include "VocalQuickStatsPanel.h"
#include "StemMaskingPanel.h"
#include "StemEnergyPanel.h"
#include "LyricIntelligibilityPanel.h"
#include "VocalRenderPanel.h"
#include "GainRiderPanel.h"
#include "PreflightPanel.h"
#include "CompingPanel.h"
#include "PitchAccuracyPanel.h"
#include "VocalHarmonyPanel.h"
#include "PitchNoteGridEditor.h"
#include "WordClipGainEditor.h"
#include "TakeAlignmentPanel.h"
#include "MixMapCanvas.h"
#include "../shared/PanelDockManager.h"
#include "../shared/SegmentedControl.h"

namespace khensu {

class IAppController;

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class VocalWorkspaceScreen final : public juce::Component {
  public:
    explicit VocalWorkspaceScreen(IAppController& controller);
    ~VocalWorkspaceScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    void pushDataToPanels(const AnalysisResult& r);
    void updateState();
    void updateStemPanel();
    void updateSoloButtonStates();

    enum class StemState { None, Running, Ready };
    StemState stemState_ = StemState::None;
    juce::StringArray stemFiles_; // paths to separated stems when Ready

    /// Intake auto-run bookkeeping (spec §1.2): stem-gated vocal analyses
    /// (masking, pitch accuracy, gain ride) fire once per source file.
    juce::String lastStemAutoScanPath_;

    // Per-stem solo row (visible once stems are Ready). "Full Mix" un-solos.
    juce::TextButton soloVocalsButton_{TRANS("Vocals")};
    juce::TextButton soloDrumsButton_{TRANS("Drums")};
    juce::TextButton soloBassButton_{TRANS("Bass")};
    juce::TextButton soloOtherButton_{TRANS("Other")};
    juce::TextButton soloFullMixButton_{TRANS("Full Mix")};

    void layoutContent();
    /// The current-vocal row (WF-4): shown while an edit is the current vocal.
    void updateCurrentVocal();
    juce::Label currentVocalLabel_;
    juce::TextButton backToStemButton_{TRANS("Back to separated vocal")};

    // Dockable / resizable panels (same contract as the Dashboard's Zone 3).
    // Every card can pop out into its own window and be drag-resized; state
    // persists as the vocal_workspace_dock_state preference.
    void setUpDocking();
    void saveDockState();
    /// Lays `card` out at (x, y, w) with its user height (or `defaultH`) and
    /// returns the height used - 0 when the card is floating in its window.
    int placeCard(PanelCard& card, const std::string& id, int x, int y, int w, int defaultH);
    std::vector<std::pair<std::string, PanelCard*>> dockableCards();
    static constexpr const char* kDockStatePrefKey = "vocal_workspace_dock_state";
    std::unique_ptr<PanelDockManager> dock_;
    bool dockRestored_ = false;

    IAppController& controller;

    ScreenHeader titleLabel; // UX-3: title = the sidebar label
    juce::Label statusLabel;
    juce::Label warningLabel;
    juce::TextButton loadTrackButton_; // UX-5: the empty state's one action
    juce::TextButton separateButton;
    juce::ComboBox separationMethodBox;

    // Fixed panel budgets (masking 300 + lyrics 220 + a real bottom row)
    // added up to more than a typical window height, silently squeezing
    // the bottom row's percentage-of-remaining-space panels (Vocal
    // Strategy/Analysis/Macros) down to a sliver with just enough room for
    // their header and nothing else - the same "empty box" failure class
    // DashboardScreen hit before it moved to a scrolling Viewport. This
    // screen gets the same fix: everything below the fixed header/status
    // labels lives in a scrollable content_ with generous minimum heights
    // instead of fighting over whatever's left of a single screen height.
    juce::Viewport viewport_;
    // UX-14: three tabs instead of 19 panels on one page - Edit (pitch, word
    // gain, ride, comp, align, harmony), Diagnose (accuracy, intelligibility,
    // masking, energy, stats, hooks, preflight) and Tone (strategy, macros,
    // render). One container per tab; the viewport shows the active one, so a
    // card's own visibility (stem cards appear once stems exist) is untouched.
    SegmentedControl tabStrip_;
    juce::Component editContent_, diagnoseContent_, toneContent_;
    int activeTab_ = 0;
    void showTab(int index);
    juce::Component& tabContent(int index) noexcept;

    VocalAnalysisPanel analysisPanel;
    VocalStrategyPanel strategyPanel;
    VocalMacroPanel macroPanel;
    VocalRenderPanel renderPanel;
    GainRiderPanel gainRider_;
    PreflightPanel preflight_;
    CompingPanel comping_;
    PitchAccuracyPanel pitchAccuracy_;
    VocalHookPanel hookPanel;
    VocalQuickStatsPanel vocalStats_;
    StemMaskingPanel stemMasking_;
    StemEnergyPanel stemEnergy_;
    LyricIntelligibilityPanel lyricIntelligibility_;
    PitchNoteGridEditor pitchEditor_;
    WordClipGainEditor wordClipGain_;
    TakeAlignmentPanel takeAlignment_;
    VocalHarmonyPanel vocalHarmony_; ///< harmony-line MIDI export (/analyze/vocal_harmony)
    MixMapCanvas mixMap_;
    /// CON-6: the stem views - masking, energy, the mix map - share one card
    /// with tabs (shown once stems exist).
    CardGroup stemsGroup_{"stems", TRANS("Stems")};
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle analysisListener_;
    ListenerHandle stemAnalysisListener_;
    ListenerHandle vocalAnalysisListener_;
    ListenerHandle currentVocalListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalWorkspaceScreen)
};

} // namespace khensu
