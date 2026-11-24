// plugin-copilot/Source/PluginEditor.h
#pragma once
#include <khensu/ui/ExperimentalTag.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CopilotServiceClient.h"
#include "ui/MeterSection.h"
#include "ui/PluginComparisonStrip.h"
#include "ui/HookIndicatorPanel.h"
#include "ui/SuggestionFeedPanel.h"
#include "ui/SystemBrainMiniPanel.h"
#include "ui/MixerChainMiniPanel.h"
#include "ui/ChatConsoleMiniPanel.h"
#include "ui/DspControlGridPanel.h"
#include "ui/EqCurveMiniPanel.h"
#include "ui/HostedChainMiniPanel.h"
#include "ui/MixHubPanel.h"
#include "ui/IssueTimelinePanel.h"
#include "ui/PluginSubTabs.h"
#include "ui/PresetBar.h"
#include "ui/TopFixControl.h"

namespace khensu {

// THREAD_CONTEXT(UIThread)
class KhensuCopilotEditor final : public juce::AudioProcessorEditor,
#if JucePlugin_Enable_ARA
                                  public juce::AudioProcessorEditorARAExtension,
#endif
                                  public juce::Timer,
                                  public juce::FileDragAndDropTarget {
  public:
    explicit KhensuCopilotEditor(KhensuCopilotProcessor&);
    ~KhensuCopilotEditor() override;

    /// Full-view resize limits (PLG-20): below the minimum the Chain tab's
    /// knobs stop being legible. The minimum is the Small size.
    static constexpr int kMinWidth = 720, kMinHeight = 540, kMaxWidth = 1800, kMaxHeight = 1300;
    /// UI-8: the Small / Medium / Large full sizes and the Compact view, which
    /// fits a DAW insert slot.
    static constexpr int kSmallWidth = 720, kSmallHeight = 540;
    static constexpr int kMediumWidth = 960, kMediumHeight = 720;
    static constexpr int kLargeWidth = 1280, kLargeHeight = 960;
    static constexpr int kCompactWidth = 560, kCompactHeight = 380;
    static constexpr int kDefaultWidth = kMediumWidth, kDefaultHeight = kMediumHeight;
    static juce::Point<int> sizeFor(KhensuCopilotProcessor::ViewSize size) noexcept;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /// Selects tab `index` (0-based) as the Ctrl+<n> shortcut does. Returns
    /// false for an out-of-range index.
    bool selectTabByShortcut(int index);
    int getCurrentTabIndex() const { return tabs_.getCurrentTabIndex(); }

    /// UI-8: switches between Compact and a full size; saved per instance in
    /// the plugin state (with the current tab).
    void setView(bool compact, KhensuCopilotProcessor::ViewSize size);
    bool isCompactView() const noexcept { return compact_; }
    TopFixControl& topFixControl() noexcept { return suggestionsPage_.topFix_; }
    TopFixControl& compactTopFixControl() noexcept { return compactTopFix_; }
    TopFindingsList& compactFindings() noexcept { return compactFindings_; }
    PresetBar& presetBar() noexcept { return presetBar_; }

  private:
    /// The tab component, reporting tab changes so the tab is saved (UI-8).
    struct CopilotTabs final : public juce::TabbedComponent {
        CopilotTabs() : juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop) {}
        std::function<void(int)> onTabChanged;
        void currentTabChanged(int index, const juce::String&) override
        {
            if (onTabChanged) onTabChanged(index);
        }
    };

    /// "Hooks / Brain" tab page: lays out two existing panels side-by-side
    /// under one tab instead of adding a 6th top-level tab. Does not own
    /// hooks/brain - they're KhensuCopilotEditor members, laid out here.
    struct HooksBrainTabPage final : public juce::Component {
        HooksBrainTabPage(HookIndicatorPanel& hooks, SystemBrainMiniPanel& brain) : hooks_(hooks), brain_(brain)
        {
            addAndMakeVisible(hooks_);
            addAndMakeVisible(brain_);
        }
        void resized() override
        {
            auto b = getLocalBounds();
            hooks_.setBounds(b.removeFromTop(b.getHeight() / 2));
            b.removeFromTop(4);
            brain_.setBounds(b);
        }
        HookIndicatorPanel& hooks_;
        SystemBrainMiniPanel& brain_;
    };

    /// "AI Suggestions" tab page: Apply top fix (OVH-8) over the timed Issues
    /// list (CE-DAW-3) and the suggestion feed, plus a footer deep-link
    /// pointing engineers to the full Chain Reasoning View, which only the
    /// desktop app has room to render. The issues live here rather than in a
    /// 9th tab (UX-18).
    struct SuggestionsTabPage final : public juce::Component {
        SuggestionsTabPage(IssueTimelinePanel& issues, SuggestionFeedPanel& feed, KhensuCopilotProcessor& processor);
        void resized() override;
        IssueTimelinePanel& issues_;
        SuggestionFeedPanel& feed_;
        KhensuCopilotProcessor& processor_;
        TopFixControl topFix_;
        juce::TextButton desktopLinkButton_;
    };

    KhensuCopilotProcessor& processor_;
    // Declared before every child so it outlives them (UX-4).
    PluginEditorLookAndFeel editorLookAndFeel_;

    juce::Label titleLabel_;
    /// VER-8: the ARA and CLAP builds are experimental (CE-DAW-1 / CE-DAW-2):
    /// built and tested, not yet checked in those hosts.
    khensu::ui::ExperimentalTag formatTag_;
    juce::TextButton loadButton_;
    /// FEAT-7: record the next bars (or the loop) and hand them to the
    /// desktop app as a new session.
    juce::TextButton captureButton_;
    void onCaptureClicked();
    /// Timer: the capture's progress, and writing it once done.
    void updateCapture();
    juce::String captureLabel_;  ///< "8 bars" / "Loop", for the session name
    juce::String captureNote_;   ///< the last outcome, shown on the button for a while
    int captureNoteTicks_ = 0;
    /// Loads the bound (or newest) session - after the picker (WF-8).
    void startLoad();
    juce::TextButton bypassButton_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment_;
    /// UI-8: Compact / Small / Medium / Large.
    juce::TextButton viewButton_;
    void showViewMenu();
    PresetBar presetBar_;
    juce::Label statusLabel_;

    MeterSection meterSection_; // persistent meter strip, spans all tabs
    HookIndicatorPanel hookPanel_;
    SuggestionFeedPanel suggestionPanel_;
    SystemBrainMiniPanel systemBrainPanel_;
    MixerChainMiniPanel mixerChainPanel_;
    ChatConsoleMiniPanel chatPanel_;
    DspControlGridPanel dspGridPanel_;
    EqCurveMiniPanel eqCurvePanel_;
    HostedChainMiniPanel hostedChainPanel_;
    MixHubPanel mixHubPanel_;
    IssueTimelinePanel issuePanel_;

    CopilotTabs tabs_;
    HooksBrainTabPage hooksBrainPage_{hookPanel_, systemBrainPanel_};
    SuggestionsTabPage suggestionsPage_{issuePanel_, suggestionPanel_, processor_};
    // UX-18: five tabs - Suggest (suggestions, hooks/brain), DSP (chain, EQ
    // curve), Plugins (mixer chain, hosted), Mix, Chat.
    PluginSubTabs suggestTab_, dspTab_, pluginsTab_;

    // UI-8 Compact view: meters, top three findings, Apply top fix, A/B.
    bool compact_ = false;
    TopFindingsList compactFindings_;
    TopFixControl compactTopFix_;

    void timerCallback() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void configureKeyboardAccess();
    void onLoadClicked();
    void onAnalysisLoaded();
    /// Re-reads the findings into the top-fix controls and the compact list.
    void refreshFindings();
    void layoutFull(juce::Rectangle<int> b);
    void layoutCompact(juce::Rectangle<int> b);
    void layoutApplyStrip(juce::Rectangle<int>& b);

    juce::Label mappingHudLabel_;
    juce::Label serviceHealthLabel_;
    PluginComparisonStrip comparisonStrip_;
    // UX-10: what Apply did, inline - never a modal dialog inside the DAW.
    // Undo puts back every parameter the apply changed (as host gestures).
    juce::Label applyStripLabel_;
    juce::TextButton applyUndoButton_;
    juce::TextButton applyOverrideButton_; ///< OVH-8: "Apply anyway" when not safe to auto-apply
    juce::TextButton applyDismissButton_{"x"};
    std::vector<float> applyUndoSnapshot_; ///< normalised values before the last apply
    std::optional<Recommendation> pendingOverride_;
    void showApplyStrip(const juce::String& summary, const juce::String& details, bool canUndo, bool canOverride = false);
    void undoLastApply();
  public:
    /// Routes a recommendation's suggested parameters into the built-in DSP
    /// chain through the shared mapper (OVH-8) and keeps it live under the
    /// Amount knob; the rest is listed to dial in. With `requireSafe`, a
    /// finding not marked safe_to_auto_apply is not applied (the strip says
    /// why and offers Apply anyway). Returns true when anything was applied.
    bool applySuggestion(const Recommendation& rec, bool requireSafe = true);
    /// UX-10 (tests): presses the strip's Undo.
    void undoLastApplyForTesting() { undoLastApply(); }
    /// OVH-8 (tests): presses the strip's Apply anyway.
    void applyAnywayForTesting()
    {
        if (applyOverrideButton_.isVisible()) applyOverrideButton_.onClick();
    }
    /// UX-10 (tests): the inline apply strip's text, empty when hidden.
    juce::String applyStripTextForTesting() const
    {
        return applyStripLabel_.isVisible() ? applyStripLabel_.getText() : juce::String();
    }
  private:
    juce::TooltipWindow tooltipWindow_{this, 600};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuCopilotEditor)
};

} // namespace khensu
