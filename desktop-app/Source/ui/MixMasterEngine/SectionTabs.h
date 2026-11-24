// desktop-app/Source/ui/MixMasterEngine/SectionTabs.h
// UX2-2: Mix / Master's secondary views as a quiet row of sentence-case text
// tabs (the selected one underlined in the accent), left-aligned, each sized
// to its label - not a strip of equal-width UPPERCASE segments. Each tab is a
// button (keyboard focus, a spoken name); Left / Right move the selection.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <functional>
#include <memory>
#include <vector>

namespace khensu {

class SectionTabs final : public juce::Component {
  public:
    SectionTabs() { setTitle(TRANS("Views")); }

    void addTab(const juce::String& label, const juce::String& tooltip)
    {
        auto tab = std::make_unique<Tab>(*this, static_cast<int>(tabs_.size()), label);
        tab->setTooltip(tooltip);
        addAndMakeVisible(*tab);
        tabs_.push_back(std::move(tab));
        resized();
    }

    int selected() const noexcept { return selected_; }
    int count() const noexcept { return static_cast<int>(tabs_.size()); }

    /// Selects a tab; fires onChange only when `notify` and it changed.
    void setSelected(int index, bool notify = false)
    {
        if (index < 0 || index >= count()) return;
        const bool changed = index != selected_;
        selected_ = index;
        for (auto& t : tabs_)
            t->repaint();
        if (changed && notify && onChange) onChange(selected_);
    }

    std::function<void(int)> onChange;

    void paint(juce::Graphics& g) override
    {
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRect(0.0f, static_cast<float>(getHeight()) - 1.0f, static_cast<float>(getWidth()), 1.0f);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        for (auto& t : tabs_) {
            const int w = juce::GlyphArrangement::getStringWidthInt(font(), t->getButtonText()) + 2 * kPadX;
            t->setBounds(b.removeFromLeft(w));
            b.removeFromLeft(kGap);
        }
    }

    static juce::Font font() { return KhensuTheme::bodyFont(KhensuTheme::kFontBody); }

  private:
    static constexpr int kPadX = 12;
    static constexpr int kGap = 4;

    class Tab final : public juce::Button {
      public:
        Tab(SectionTabs& owner, int index, const juce::String& label) : juce::Button(label), owner_(owner), index_(index)
        {
            setTitle(label);
            setWantsKeyboardFocus(true);
            onClick = [this] { owner_.setSelected(index_, true); };
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            juce::ignoreUnused(down);
            const bool on = owner_.selected_ == index_;
            auto b = getLocalBounds();
            g.setFont(on ? SectionTabs::font().boldened() : SectionTabs::font());
            g.setColour(on ? KhensuTheme::textPrimary : (over ? KhensuTheme::textSecondary : KhensuTheme::textTertiary));
            g.drawText(getButtonText(), b.withTrimmedBottom(3), juce::Justification::centred, false);
            if (on) {
                g.setColour(KhensuTheme::accentTeal);
                g.fillRoundedRectangle(b.removeFromBottom(2).reduced(kPadX - 4, 0).toFloat(), 1.0f);
            }
            if (hasKeyboardFocus(false)) {
                g.setColour(KhensuTheme::focusRing);
                g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, 1.0f);
            }
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            const int step = key == juce::KeyPress::rightKey ? 1 : key == juce::KeyPress::leftKey ? -1 : 0;
            if (step == 0) return juce::Button::keyPressed(key);
            const int next = juce::jlimit(0, owner_.count() - 1, index_ + step);
            owner_.setSelected(next, true);
            owner_.tabs_[static_cast<size_t>(next)]->grabKeyboardFocus();
            return true;
        }

      private:
        SectionTabs& owner_;
        int index_;
    };

    std::vector<std::unique_ptr<Tab>> tabs_;
    int selected_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionTabs)
};

} // namespace khensu
