// plugin-copilot/Source/ui/PluginSubTabs.h
// UX-18: one of the editor's five tabs holding two related views, with a
// small radio row to switch between them (the plugin cannot use the desktop's
// SegmentedControl). A tab with one view shows no row.
#pragma once
#include <JuceHeader.h>
#include "PluginLookAndFeel.h"

namespace khensu {

class PluginSubTabs final : public juce::Component {
  public:
    void addView(const juce::String& name, juce::Component& view)
    {
        auto button = std::make_unique<juce::TextButton>(name);
        button->setClickingTogglesState(true);
        button->setRadioGroupId(1);
        // The selected view's button is filled, like the selected tab above it.
        button->setColour(juce::TextButton::buttonOnColourId, PluginLookAndFeel::accentTeal.withAlpha(0.35f));
        button->setColour(juce::TextButton::textColourOnId, PluginLookAndFeel::accentTeal);
        button->setTitle(name);
        button->setTooltip(TRANS("Show") + " " + name);
        const int index = (int)views_.size();
        button->onClick = [this, index] { show(index); };
        addAndMakeVisible(*button);
        buttons_.push_back(std::move(button));
        views_.push_back(&view);
        addChildComponent(view);
        show(active_);
    }

    void show(int index)
    {
        if (index < 0 || index >= (int)views_.size()) return;
        active_ = index;
        for (int i = 0; i < (int)views_.size(); ++i) {
            views_[(size_t)i]->setVisible(i == index);
            buttons_[(size_t)i]->setToggleState(i == index, juce::dontSendNotification);
        }
        resized();
    }
    int activeView() const noexcept { return active_; }
    int viewCount() const noexcept { return (int)views_.size(); }

    void resized() override
    {
        auto b = getLocalBounds();
        const bool row = views_.size() > 1;
        for (auto& button : buttons_)
            button->setVisible(row);
        if (row) {
            auto strip = b.removeFromTop(26).reduced(8, 2);
            const int w = juce::jmin(160, strip.getWidth() / juce::jmax(1, (int)buttons_.size()));
            for (auto& button : buttons_)
                button->setBounds(strip.removeFromLeft(w).reduced(2, 0));
            b.removeFromTop(2);
        }
        for (auto* v : views_)
            v->setBounds(b);
    }

  private:
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    std::vector<juce::Component*> views_;
    int active_ = 0;
};

} // namespace khensu
