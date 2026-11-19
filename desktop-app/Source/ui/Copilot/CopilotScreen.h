// desktop-app/Source/ui/Copilot/CopilotScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ScreenHeader.h"
#include "../shared/SegmentedControl.h"
#include "ChatConsolePanel.h"
#include "MixerChainPanel.h"
#include "MasteringAgentPanel.h"
#include "SectionIntensityPanel.h"
#include "TasteProfilePanel.h"
#include "FeedbackDecoderPanel.h"
#include "HostedChainPanel.h"
#include "AutoMixPanel.h"

namespace khensu {

class IAppController;

/// UX-13: one tab of the Copilot screen holding several related views, with
/// a segmented switch between them (no switch when there is only one).
class CopilotGroup final : public juce::Component {
  public:
    CopilotGroup() { switcher_.onChange = [this](int i) { show(i); }; }

    void addView(const juce::String& name, juce::Component& view)
    {
        views_.push_back(&view);
        switcher_.addSegment(name);
        addChildComponent(view);
        if (views_.size() == 2) addAndMakeVisible(switcher_);
        show(active_);
    }
    void show(int index)
    {
        if (index < 0 || index >= (int)views_.size()) return;
        active_ = index;
        switcher_.setSelected(index);
        for (int i = 0; i < (int)views_.size(); ++i)
            views_[(size_t)i]->setVisible(i == index);
        resized();
    }
    int activeView() const noexcept { return active_; }
    int viewCount() const noexcept { return (int)views_.size(); }

    void resized() override
    {
        auto b = getLocalBounds();
        if (views_.size() > 1) {
            switcher_.setBounds(b.removeFromTop(KhensuTheme::buttonH).reduced(KhensuTheme::panelPadding, 2));
            b.removeFromTop(4);
        }
        for (auto* v : views_)
            v->setBounds(b);
    }

  private:
    SegmentedControl switcher_;
    std::vector<juce::Component*> views_;
    int active_ = 0;
};

/// CUT-8: the mastering agent with section intensity as an option - a
/// toggle shows the per-section intensity controls under the agent instead
/// of a tab of their own.
class MasteringView final : public juce::Component {
  public:
    MasteringView(juce::Component& agent, juce::Component& sections) : agent_(agent), sections_(sections)
    {
        addAndMakeVisible(agent_);
        sectionsToggle_.setTooltip(TRANS("Shape the processing per song section (verse quieter, chorus wider, ...)"));
        sectionsToggle_.onClick = [this] {
            sections_.setVisible(sectionsToggle_.getToggleState());
            resized();
        };
        addAndMakeVisible(sectionsToggle_);
        addChildComponent(sections_);
    }
    void resized() override
    {
        auto b = getLocalBounds();
        sectionsToggle_.setBounds(b.removeFromBottom(KhensuTheme::buttonH).reduced(KhensuTheme::panelPadding, 2));
        if (sections_.isVisible()) sections_.setBounds(b.removeFromBottom(b.getHeight() * 45 / 100));
        agent_.setBounds(b);
    }
    juce::ToggleButton& sectionsToggle() noexcept { return sectionsToggle_; }

  private:
    juce::Component& agent_;
    juce::Component& sections_;
    juce::ToggleButton sectionsToggle_{TRANS("Per-section intensity")};
};

// THREAD_CONTEXT(UIThread)
class CopilotScreen final : public juce::Component {
  public:
    explicit CopilotScreen(IAppController& c);
    ~CopilotScreen() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;


  private:
    IAppController& controller_;

    ScreenHeader header_; // UX-3: the shared title row
    // UX-17: the one themed segment control, as every other screen uses.
    SegmentedControl sections_;
    void showSection(int index);

    // Assist: interactive copilot chat (/copilot/chat + /copilot/apply_chain),
    // the client revision decoder and what Khensu learned about your taste.
    ChatConsolePanel chatConsole_;
    FeedbackDecoderPanel feedbackDecoderPanel_;
    TasteProfilePanel tasteProfilePanel_;

    // Master: the iterative mastering agent (section intensity as an option,
    // CUT-8) and the verified auto-mix agent (CE-AG-1, /agent/mix).
    MasteringAgentPanel masteringAgentPanel_;
    SectionIntensityPanel sectionIntensityPanel_;
    AutoMixPanel autoMixPanel_;

    // Chain: the generated chain and the hosted third-party chain.
    MixerChainPanel mixerChainPanel_;
    HostedChainPanel hostedChainPanel_;

    MasteringView masteringView_{masteringAgentPanel_, sectionIntensityPanel_};
    CopilotGroup assist_, master_, chain_;

    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotScreen)
};

} // namespace khensu
