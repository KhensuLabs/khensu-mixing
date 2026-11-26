// desktop-app/Source/ui/Dashboard/ReadinessBreakdownBar.cpp
#include "ReadinessBreakdownBar.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

void ReadinessBreakdownBar::mouseMove(const juce::MouseEvent& e)
{
    if (!hasData_ || terms_.empty()) return;
    float total = 0.0f;
    for (const auto& t : terms_)
        total += std::max(0.0f, t.points);
    if (total <= 0.0f) {
        hoveredIndex_ = -1;
        repaint();
        return;
    }

    const float x = static_cast<float>(e.position.x);
    float cursorX = 0.0f;
    int idx = -1;
    for (int i = 0; i < static_cast<int>(terms_.size()); ++i) {
        const float w =
            (std::max(0.0f, terms_[static_cast<size_t>(i)].points) / total) * static_cast<float>(getWidth());
        if (x >= cursorX && x < cursorX + w) {
            idx = i;
            break;
        }
        cursorX += w;
    }
    if (idx != hoveredIndex_) {
        hoveredIndex_ = idx;
        repaint();
    }
}

void ReadinessBreakdownBar::paint(juce::Graphics& g)
{
    auto full = getLocalBounds();
    auto labelArea = full.removeFromBottom(14);
    auto b = full;

    if (!hasData_ || terms_.empty()) {
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(b.toFloat(), 3.0f);
        return;
    }

    float total = 0.0f;
    for (const auto& t : terms_)
        total += std::max(0.0f, t.points);

    if (total <= 0.01f) {
        // No penalties at all - draw a solid "clean" bar.
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.5f));
        g.fillRoundedRectangle(b.toFloat(), 3.0f);
        return;
    }

    // Colour ramp by which term it is - not by magnitude, so the same term
    // is always recognisable at a glance across sessions.
    static const juce::Colour kColours[] = {
        KhensuTheme::accentRed,  KhensuTheme::accentAmber, KhensuTheme::accentBlue,
        KhensuTheme::accentTeal, juce::Colour(0xff5b8def), juce::Colour(0xffe67e22), // theme-exempt(categorical palette extension: distinct hues beyond the token set)
    };

    float x = static_cast<float>(b.getX());
    const float y = static_cast<float>(b.getY());
    const float h = static_cast<float>(b.getHeight());

    for (int i = 0; i < static_cast<int>(terms_.size()); ++i) {
        const float pts = std::max(0.0f, terms_[static_cast<size_t>(i)].points);
        const float w = (pts / total) * static_cast<float>(b.getWidth());
        if (w > 0.5f) {
            const juce::Colour col = kColours[static_cast<size_t>(i) % 6];
            g.setColour(i == hoveredIndex_ ? col : col.withAlpha(0.65f));
            g.fillRect(juce::Rectangle<float>(x, y, w, h));
        }
        x += w;
    }

    if (hoveredIndex_ >= 0 && hoveredIndex_ < static_cast<int>(terms_.size())) {
        const auto& t = terms_[static_cast<size_t>(hoveredIndex_)];
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawText(juce::String(t.name) + ": -" + juce::String(t.points, 1) + " pts", labelArea,
                   juce::Justification::centredLeft, true);
    } else {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawText("Hover to see what's dragging the score down", labelArea, juce::Justification::centredLeft, true);
    }
}

void ReadinessBreakdownBar::mouseDown(const juce::MouseEvent& e)
{
    if (!hasData_ || terms_.empty()) return;
    float total = 0.0f;
    for (const auto& t : terms_)
        total += std::max(0.0f, t.points);
    if (total <= 0.001f) return;

    const float x = static_cast<float>(e.position.x);
    float cursorX = 0.0f;
    int idx = -1;
    for (int i = 0; i < static_cast<int>(terms_.size()); ++i) {
        const float w =
            (std::max(0.0f, terms_[static_cast<size_t>(i)].points) / total) * static_cast<float>(getWidth());
        if (x >= cursorX && x < cursorX + w) {
            idx = i;
            break;
        }
        cursorX += w;
    }

    if (idx >= 0 && idx < static_cast<int>(terms_.size())) {
        if (onSegmentClicked) {
            onSegmentClicked(terms_[static_cast<size_t>(idx)].name);
        }
    }
}

} // namespace khensu
