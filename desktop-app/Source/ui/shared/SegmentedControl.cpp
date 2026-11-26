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
    auto b = getLocalBounds().toFloat().reduced(0.5f);
    // LG-8: full pill capsule for segmented controls.
    const float radius = b.getHeight() * 0.5f;

    // Frosted capsule track.
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.62f));
    g.fillRoundedRectangle(b, radius);
    juce::ColourGradient trackRim(KhensuTheme::borderActive.brighter(0.2f), b.getTopLeft(), KhensuTheme::borderSubtle,
                                  b.getBottomRight(), false);
    g.setGradientFill(trackRim);
    g.drawRoundedRectangle(b, radius, 1.0f);

    if (segments_.empty()) return;
    float segW = b.getWidth() / (float)segments_.size();

    for (int i = 0; i < (int)segments_.size(); ++i) {
        auto segRect = b.withX(b.getX() + i * segW).withWidth(segW).reduced(2.0f);
        const float segR = segRect.getHeight() * 0.5f;
        bool active = (i == selected_);

        // Active liquid-glass pill + teal specular rim.
        if (active) {
            juce::ColourGradient pillFill(KhensuTheme::accentTeal.withAlpha(0.30f), segRect.getCentreX(),
                                          segRect.getY(), KhensuTheme::accentTeal.withAlpha(0.16f),
                                          segRect.getCentreX(), segRect.getBottom(), false);
            g.setGradientFill(pillFill);
            g.fillRoundedRectangle(segRect, segR);
            juce::ColourGradient pillRim(juce::Colours::white.interpolatedWith(KhensuTheme::accentTeal, 0.45f).withAlpha(0.75f), // theme-exempt(specular glint is white light in every theme)
                                         segRect.getTopLeft(), KhensuTheme::accentTeal.withAlpha(0.40f),
                                         segRect.getBottomRight(), false);
            g.setGradientFill(pillRim);
            g.drawRoundedRectangle(segRect, segR, 1.2f);
        }

        // Divider between segments (skip around the active pill's edges).
        if (i > 0 && i != selected_ && i != selected_ + 1) {
            g.setColour(KhensuTheme::borderSubtle);
            g.drawVerticalLine(juce::roundToInt(b.getX() + i * segW), b.getY() + 6.0f, b.getBottom() - 6.0f);
        }

        g.setFont(active ? KhensuTheme::labelFont(type::caption).boldened() : KhensuTheme::labelFont(type::caption));
        const juce::Colour labelColour = active ? KhensuTheme::textPrimary : KhensuTheme::textSecondary;
        g.setColour(isEnabled() ? labelColour : KhensuTheme::textTertiary);
        g.drawText(segments_[i], segRect.toNearestInt(), juce::Justification::centred, true);
    }

    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.9f));
        g.drawRoundedRectangle(b.reduced(0.5f), radius, 1.6f);
    }
}

} // namespace khensu
