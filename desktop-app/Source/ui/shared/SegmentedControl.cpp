// desktop-app/Source/ui/shared/SegmentedControl.cpp
#include "SegmentedControl.h"
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"

namespace khensu {

SegmentedControl::SegmentedControl()
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void SegmentedControl::addSegment(const juce::String& label)
{
    segments_.push_back(label);
    repaint();
}

void SegmentedControl::setSelected(int index)
{
    if (segments_.empty()) return;
    selected_ = juce::jlimit(0, (int)segments_.size() - 1, index);
    repaint();
}

int SegmentedControl::segmentIndexAt(int x) const
{
    if (segments_.empty()) return -1;
    float segW = (float)getWidth() / (float)segments_.size();
    return juce::jlimit(0, (int)segments_.size() - 1, (int)(x / segW));
}

void SegmentedControl::mouseDown(const juce::MouseEvent& e)
{
    // Left button only, and only when enabled - a right- or middle-click used
    // to change the selected mode (kimi audit P0 #5).
    if (!isEnabled() || !e.mods.isLeftButtonDown()) return;

    int idx = segmentIndexAt(e.x);
    if (idx != selected_ && idx >= 0) {
        selected_ = idx;
        grabKeyboardFocus();
        repaint();
        if (onChange) onChange(selected_);
    }
}

void SegmentedControl::selectRelative(int delta)
{
    if (segments_.empty()) return;
    const int idx = juce::jlimit(0, (int)segments_.size() - 1, selected_ + delta);
    if (idx == selected_) return;
    selected_ = idx;
    repaint();
    if (onChange) onChange(selected_);
}

bool SegmentedControl::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled()) return false;
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey) {
        selectRelative(-1);
        return true;
    }
    if (key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey) {
        selectRelative(1);
        return true;
    }
    return false;
}

void SegmentedControl::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    constexpr float radius = 5.0f;

    // Container background
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Raised);

    if (segments_.empty()) return;
    float segW = b.getWidth() / (float)segments_.size();

    for (int i = 0; i < (int)segments_.size(); ++i) {
        auto segRect = b.withX(b.getX() + i * segW).withWidth(segW).reduced(1.0f);
        bool active = (i == selected_);

        // Active segment fill - stronger tinted pill + brighter rim so the
        // selected mode is unmistakable, not just a faint dim.
        if (active) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.24f));
            g.fillRoundedRectangle(segRect, radius);
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.7f));
            g.drawRoundedRectangle(segRect, radius, 1.5f);
        }

        // Divider between segments (skip around the active pill's edges).
        if (i > 0 && i != selected_ && i != selected_ + 1) {
            g.setColour(KhensuTheme::borderSubtle);
            g.drawVerticalLine(juce::roundToInt(b.getX() + i * segW), b.getY() + 4.0f, b.getBottom() - 4.0f);
        }

        // Label - bold when selected.
        g.setFont(active ? KhensuTheme::labelFont(11.0f).boldened() : KhensuTheme::labelFont(11.0f));
        const juce::Colour labelColour = active ? KhensuTheme::accentTeal : KhensuTheme::textSecondary;
        g.setColour(isEnabled() ? labelColour : KhensuTheme::textTertiary);
        g.drawText(segments_[i], segRect.toNearestInt(), juce::Justification::centred, true);
    }

    // Keyboard-focus ring around the whole control (the selected pill already
    // shows which segment the arrow keys will move from).
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.9f));
        g.drawRoundedRectangle(b.reduced(0.5f), radius, 1.6f);
    }
}

} // namespace khensu
