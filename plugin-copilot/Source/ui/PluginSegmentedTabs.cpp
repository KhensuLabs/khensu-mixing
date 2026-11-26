#include "PluginSegmentedTabs.h"
#include "PluginLookAndFeel.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

namespace {
constexpr float kRadius = 5.0f;
constexpr int kBarMaxSegmentWidth = 150;
} // namespace

/// One segment: a button that paints itself as part of the track.
class PluginSegmentedTabs::Segment final : public juce::Button {
  public:
    Segment(PluginSegmentedTabs& owner, int index, const juce::String& name)
        : juce::Button(name), owner_(owner), index_(index)
    {
        setButtonText(name);
        setTitle(name);
        setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setTriggeredOnMouseDown(true);
        onClick = [this] { owner_.setCurrentTabIndex(index_); };
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const int n = owner_.getNumTabs();
        int next = -1;
        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey)
            next = juce::jmax(0, owner_.getCurrentTabIndex() - 1);
        else if (key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey)
            next = juce::jmin(n - 1, owner_.getCurrentTabIndex() + 1);
        if (next < 0) return juce::Button::keyPressed(key);
        owner_.setCurrentTabIndex(next);
        if (auto* b = owner_.getSegmentButton(next)) b->grabKeyboardFocus();
        return true;
    }

    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override
    {
        using P = PluginLookAndFeel;
        const auto r = getLocalBounds().toFloat().reduced(1.0f, 2.0f);
        const bool active = getToggleState();
        if (active) {
            g.setColour(P::accentTeal.withAlpha(0.24f));
            g.fillRoundedRectangle(r, kRadius);
            g.setColour(P::accentTeal.withAlpha(0.7f));
            g.drawRoundedRectangle(r, kRadius, 1.5f);
        } else if (isMouseOver) {
            g.setColour(P::textPrimary.withAlpha(0.05f));
            g.fillRoundedRectangle(r, kRadius);
        }
        g.setFont(ui::Palette::uiFont(type::body, active ? juce::Font::bold : juce::Font::plain));
        g.setColour(active ? P::accentTeal : (isMouseOver ? P::textPrimary : P::textMuted));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(4, 0), juce::Justification::centred, 1, 0.85f);
        if (hasKeyboardFocus(false)) {
            g.setColour(P::accentTeal.withAlpha(0.9f));
            g.drawRoundedRectangle(r.reduced(1.0f), kRadius, 1.4f);
        }
    }

  private:
    PluginSegmentedTabs& owner_;
    int index_;
};

PluginSegmentedTabs::PluginSegmentedTabs()
{
    // focusContainer (not keyboardFocusContainer): the editor's Tab order
    // descends into the segments and the page.
    setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
}

PluginSegmentedTabs::~PluginSegmentedTabs() = default;

void PluginSegmentedTabs::addTab(const juce::String& name, juce::Component& page)
{
    const int index = getNumTabs();
    auto segment = std::make_unique<Segment>(*this, index, name);
    segment->setDescription(TRANS("Show the") + " " + name + " " + TRANS("section") + " (Ctrl+" +
                            juce::String(index + 1) + ").");
    segment->setTooltip(name + " (Ctrl+" + juce::String(index + 1) + ")");
    segment->setExplicitFocusOrder(index + 1);
    segment->setToggleState(index == current_, juce::dontSendNotification);
    addAndMakeVisible(*segment);
    segments_.push_back(std::move(segment));
    pages_.push_back(&page);
    // Pages follow the segments in the Tab order.
    page.setExplicitFocusOrder(1000);
    addChildComponent(page);
    page.setVisible(index == current_);
    resized();
}

juce::StringArray PluginSegmentedTabs::getTabNames() const
{
    juce::StringArray names;
    for (const auto& s : segments_)
        names.add(s->getButtonText());
    return names;
}

juce::Button* PluginSegmentedTabs::getSegmentButton(int index) const
{
    return index >= 0 && index < getNumTabs() ? segments_[static_cast<size_t>(index)].get() : nullptr;
}

void PluginSegmentedTabs::setCurrentTabIndex(int index)
{
    if (index < 0 || index >= getNumTabs()) return;
    const bool changed = index != current_;
    current_ = index;
    for (int i = 0; i < getNumTabs(); ++i) {
        segments_[static_cast<size_t>(i)]->setToggleState(i == index, juce::dontSendNotification);
        pages_[static_cast<size_t>(i)]->setVisible(i == index);
    }
    repaint();
    if (changed && onTabChanged) onTabChanged(index);
}

juce::Rectangle<int> PluginSegmentedTabs::barBounds() const
{
    auto bar = getLocalBounds().removeFromTop(kBarHeight).reduced(4, 3);
    const int wanted = kBarMaxSegmentWidth * juce::jmax(1, getNumTabs());
    return bar.withWidth(juce::jmin(bar.getWidth(), wanted));
}

void PluginSegmentedTabs::paint(juce::Graphics& g)
{
    using P = PluginLookAndFeel;
    const auto bar = barBounds().toFloat();
    // The track: the desktop's raised panel, one hairline rim.
    g.setColour(P::bgPanel.brighter(0.06f));
    g.fillRoundedRectangle(bar, kRadius);
    g.setColour(P::textPrimary.withAlpha(0.10f));
    g.drawRoundedRectangle(bar.reduced(0.5f), kRadius, 1.0f);
    // Dividers between segments, skipped around the selected pill.
    for (int i = 1; i < getNumTabs(); ++i) {
        if (i == current_ || i == current_ + 1) continue;
        const auto x = static_cast<float>(segments_[static_cast<size_t>(i)]->getX());
        g.setColour(P::textPrimary.withAlpha(0.08f));
        g.drawVerticalLine(juce::roundToInt(x), bar.getY() + 6.0f, bar.getBottom() - 6.0f);
    }
}

void PluginSegmentedTabs::resized()
{
    auto bar = barBounds();
    const int n = juce::jmax(1, getNumTabs());
    const int w = bar.getWidth() / n;
    for (auto& s : segments_)
        s->setBounds(bar.removeFromLeft(w));
    auto content = getLocalBounds();
    content.removeFromTop(kBarHeight + 2);
    for (auto* page : pages_)
        page->setBounds(content);
}

} // namespace khensu
