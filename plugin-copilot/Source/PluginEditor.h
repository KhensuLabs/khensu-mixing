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

    /// Resize limits (PLG-20): the editor is resizable; below the minimum the
    /// Chain tab's knobs stop being legible.
    static constexpr int kMinWidth = 720, kMinHeight = 540, kMaxWidth = 1800, kMaxHeight = 1300;
    static constexpr int kDefaultWidth = 800, kDefaultHeight = 600;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /// Selects tab `index` (0-based) as the Ctrl+<n> shortcut does. Returns
    /// false for an out-of-range index.
    bool selectTabByShortcut(int index);
    int getCurrentTabIndex() const { return tabs_.getCurrentTabIndex(); }

  private:
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

    /// "AI Suggestions" tab page: the timed Issues list (CE-DAW-3) above the
    /// suggestion feed, plus a footer deep-link pointing engineers to the full
    /// Chain Reasoning View, which only the desktop app has room to render
    /// (Detected -> Cause -> Fix -> Trade-off -> Alternative Strategies -> How
    /// to Verify). The issues live here rather than in a 9th tab (UX-18).
    struct SuggestionsTabPage final : public juce::Component {
        SuggestionsTabPage(IssueTimelinePanel& issues, SuggestionFeedPanel& feed, KhensuCopilotProcessor& processor)
            : issues_(issues), feed_(feed), processor_(processor)
        {
            addAndMakeVisible(issues_);
            addAndMakeVisible(feed_);
            desktopLinkButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            desktopLinkButton_.setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::accentTeal);
            desktopLinkButton_.setTooltip("Ask Khensu Mixing on the desktop to show the full reasoning for this session.");
            desktopLinkButton_.onClick = [this] {
                juce::String sessionId = processor_.getSessionId();
                if (sessionId.isNotEmpty()) {
                    processor_.runShortInBackground([sessionId]() {
                        auto bridge = copilot::ServiceClient::bridge();
                        bridge.postEvent(sessionId, "show_reasoning", "View full reasoning request from plugin");
                    });
                } else {
                    juce::NativeMessageBox::showMessageBoxAsync(
                        juce::MessageBoxIconType::InfoIcon, "Full Reasoning on Desktop",
                        "Open Khensu Mixing on the desktop and select this session's "
                        "Mix/Master screen to see the full Chain Reasoning View for each "
                        "recommendation (Detected -> Cause -> Fix -> Trade-off -> "
                        "Alternative Strategies -> How to Verify) - there's more room "
                        "for it there than the plugin window allows.");
                }
            };
            addAndMakeVisible(desktopLinkButton_);
        }
        void resized() override
        {
            auto b = getLocalBounds();
            desktopLinkButton_.setBounds(b.removeFromBottom(22));
            issues_.setBounds(b.removeFromTop(juce::jmax(200, b.getHeight() * 11 / 20)));
            b.removeFromTop(4);
            feed_.setBounds(b);
        }
        IssueTimelinePanel& issues_;
        SuggestionFeedPanel& feed_;
        KhensuCopilotProcessor& processor_;
        juce::TextButton desktopLinkButton_{juce::CharPointer_UTF8("View full reasoning on Desktop \xe2\x86\x92")};
    };

    KhensuCopilotProcessor& processor_;
    // Declared before every child so it outlives them (UX-4).
    PluginEditorLookAndFeel editorLookAndFeel_;

    juce::Label titleLabel_;
    /// VER-8: the ARA and CLAP builds are experimental (CE-DAW-1 / CE-DAW-2):
    /// built and tested, not yet checked in those hosts.
    khensu::ui::ExperimentalTag formatTag_;
    juce::TextButton loadButton_{"Load Session..."};
    /// FEAT-7: record the next bars (or the loop) and hand them to the
    /// desktop app as a new session.
    juce::TextButton captureButton_{"Capture..."};
    void onCaptureClicked();
    /// Timer: the capture's progress, and writing it once done.
    void updateCapture();
    juce::String captureLabel_;  ///< "8 bars" / "Loop", for the session name
    juce::String captureNote_;   ///< the last outcome, shown on the button for a while
    int captureNoteTicks_ = 0;
    /// Loads the bound (or newest) session - after the picker (WF-8).
    void startLoad();
    juce::TextButton bypassButton_{"Bypass"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment_;
    juce::Label statusLabel_;

    MeterSection meterSection_; // persistent top meter strip, spans all tabs
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

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    HooksBrainTabPage hooksBrainPage_{hookPanel_, systemBrainPanel_};
    SuggestionsTabPage suggestionsPage_{issuePanel_, suggestionPanel_, processor_};
    // UX-18: five tabs - Suggest (suggestions, hooks/brain), DSP (chain, EQ
    // curve), Plugins (mixer chain, hosted), Mix, Chat.
    PluginSubTabs suggestTab_, dspTab_, pluginsTab_;

    void timerCallback() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void configureKeyboardAccess();
    void onLoadClicked();
    void onAnalysisLoaded();

    juce::Label mappingHudLabel_;
    juce::Label serviceHealthLabel_;
    PluginComparisonStrip comparisonStrip_;
    // UX-10: what Apply did, inline - never a modal dialog inside the DAW.
    // Undo puts back every parameter the apply changed (as host gestures).
    juce::Label applyStripLabel_;
    juce::TextButton applyUndoButton_{"Undo"};
    juce::TextButton applyDismissButton_{"x"};
    std::vector<float> applyUndoSnapshot_; ///< normalised values before the last apply
    void showApplyStrip(const juce::String& summary, const juce::String& details, bool canUndo);
    void undoLastApply();
  public:
    /// Routes a recommendation's suggested parameters into the built-in DSP
    /// chain (the Suggestions tab's Apply); the rest is listed to dial in.
    void applySuggestion(const Recommendation& rec);
    /// UX-10 (tests): presses the strip's Undo.
    void undoLastApplyForTesting() { undoLastApply(); }
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
