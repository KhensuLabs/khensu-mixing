// desktop-app/Source/ui/Copilot/CopilotScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ScreenHeader.h"
#include "../shared/SegmentedControl.h"
#include "../shared/FeatureHomeLink.h"
#include "ChatConsolePanel.h"
#include "MixerChainPanel.h"
#include "SectionIntensityPanel.h"
#include "TasteProfilePanel.h"
#include "FeedbackDecoderPanel.h"
#include "HostedChainPanel.h"
#include "AutoMixPanel.h"

namespace khensu {

class IAppController;

/// UXR-3: the Copilot screen's views under ONE segmented switch (it used to
/// be two stacked tab rows: Assist / Master / Chain, then each group's own
/// views). No switch when there is only one view.
class CopilotGroup final : public juce::Component {
  public:
    CopilotGroup()
    {
        switcher_.setTitle(TRANS("Copilot view"));
        switcher_.onChange = [this](int i) { show(i); };
    }

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
    SegmentedControl& switcher() noexcept { return switcher_; }

    void resized() override
    {
        auto b = getLocalBounds();
        if (views_.size() > 1) {
            // About 110 px a view, so seven labels fit without ellipsis.
            auto row = b.removeFromTop(KhensuTheme::buttonHLarge).reduced(KhensuTheme::panelPadding, 4);
            switcher_.setBounds(row.withWidth(juce::jmin(row.getWidth(), 110 * (int)views_.size())));
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

/// ENH-8: the mastering agent's one home is Export > Finish (the Master
/// Assistant). The Copilot's Mastering view links there and keeps what only
/// it has: the per-section intensity processing (CUT-8).
class MasteringView final : public juce::Component {
  public:
    MasteringView(FeatureHomeLink& link, juce::Component& sections) : link_(link), sections_(sections)
    {
        addAndMakeVisible(link_);
        addAndMakeVisible(sections_);
    }
    void resized() override
    {
        auto b = getLocalBounds().reduced(KhensuTheme::panelPadding, 0);
        link_.setBounds(b.removeFromTop(link_.preferredHeight(b.getWidth())));
        b.removeFromTop(KhensuTheme::cardGap);
        sections_.setBounds(b);
    }

  private:
    FeatureHomeLink& link_;
    juce::Component& sections_;
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
    // UXR-3: one tab level - every Copilot view in one segmented switch.
    CopilotGroup views_;

    // Interactive copilot chat (/copilot/chat + /copilot/apply_chain), the
    // client revision decoder and what Khensu learned about your taste.
    ChatConsolePanel chatConsole_;
    FeedbackDecoderPanel feedbackDecoderPanel_;
    TasteProfilePanel tasteProfilePanel_;

    // Mastering: a link to its home, Export > Finish (ENH-8), over the
    // per-section intensity processing (CUT-8); the verified auto-mix agent
    // (CE-AG-1, /agent/mix).
    FeatureHomeLink masteringLink_;
    SectionIntensityPanel sectionIntensityPanel_;
    AutoMixPanel autoMixPanel_;

    // The generated chain and the hosted third-party chain.
    MixerChainPanel mixerChainPanel_;
    HostedChainPanel hostedChainPanel_;

    MasteringView masteringView_{masteringLink_, sectionIntensityPanel_};

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
