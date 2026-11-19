// desktop-app/Source/ui/shared/MixNotesTimeline.cpp
#include "MixNotesTimeline.h"
#include "KhensuTheme.h"
#include "../../IAppController.h"

namespace khensu {

namespace {
constexpr int kMarkerR = 5; // marker radius / half-size

// The mix-note colour system (spec §4), matching RecommendationCard:
// Strength -> green, >=66 -> red (severe), >=33 -> amber (moderate), else blue.
juce::Colour severityColour(float severity, bool isStrength)
{
    if (isStrength) return KhensuTheme::accentGreen;
    if (severity >= 66.0f) return KhensuTheme::accentRed;
    if (severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

// High-Contrast severity shapes so the level reads without colour.
enum class Shape { Triangle, Square, Circle, Diamond };
Shape severityShape(float severity, bool isStrength)
{
    if (isStrength) return Shape::Diamond;
    if (severity >= 66.0f) return Shape::Triangle; // severe
    if (severity >= 33.0f) return Shape::Square;   // moderate
    return Shape::Circle;                          // minor
}

void drawShape(juce::Graphics& g, Shape shape, float cx, float cy, float r)
{
    switch (shape) {
    case Shape::Circle:
        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
        break;
    case Shape::Square:
        g.fillRect(cx - r, cy - r, r * 2.0f, r * 2.0f);
        break;
    case Shape::Triangle: {
        juce::Path p;
        p.addTriangle(cx, cy - r, cx - r, cy + r, cx + r, cy + r);
        g.fillPath(p);
        break;
    }
    case Shape::Diamond: {
        juce::Path p;
        p.addQuadrilateral(cx, cy - r, cx + r, cy, cx, cy + r, cx - r, cy);
        g.fillPath(p);
        break;
    }
    }
}
} // namespace

MixNotesTimeline::MixNotesTimeline(IAppController& c) : controller_(c)
{
    setInterceptsMouseClicks(true, false);
    startTimerHz(15);
}

MixNotesTimeline::~MixNotesTimeline() { stopTimer(); }

void MixNotesTimeline::setNotes(const std::vector<Recommendation>& recs)
{
    notes_.clear();
    for (const auto& r : recs) {
        if (!r.time_range.has_value()) continue;
        const auto& tr = *r.time_range;
        if (!tr.is_valid()) continue;
        notes_.push_back({tr.start_sec, r.severity, r.type == RecommendationType::Strength, juce::String(r.what)});
    }
    repaint();
}

float MixNotesTimeline::xForTime(float sec) const
{
    auto b = getLocalBounds().reduced(kMarkerR + 2, 0);
    if (durationSec_ <= 0.0) return (float)b.getX();
    const float frac = juce::jlimit(0.0f, 1.0f, sec / (float)durationSec_);
    return (float)b.getX() + frac * (float)b.getWidth();
}

int MixNotesTimeline::markerAt(juce::Point<int> p) const
{
    const float cy = (float)getHeight() * 0.5f;
    int best = -1;
    float bestDist = (float)(kMarkerR + 3);
    for (size_t i = 0; i < notes_.size(); ++i) {
        const float cx = xForTime(notes_[i].startSec);
        const float d = std::abs((float)p.x - cx);
        if (d <= bestDist && std::abs((float)p.y - cy) <= kMarkerR + 4) {
            bestDist = d;
            best = (int)i;
        }
    }
    return best;
}

void MixNotesTimeline::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    const float cy = b.getCentreY();

    // Lane baseline.
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(juce::roundToInt(cy), b.getX() + 2.0f, b.getRight() - 2.0f);

    if (durationSec_ <= 0.0 || notes_.empty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.drawText("No time-anchored mix notes", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const bool highContrast = KhensuTheme::currentTheme() == KhensuTheme::Theme::HighContrast;

    for (size_t i = 0; i < notes_.size(); ++i) {
        const auto& m = notes_[i];
        const float cx = xForTime(m.startSec);
        const auto col = severityColour(m.severity, m.isStrength);
        const bool hot = ((int)i == hoverIndex_);
        const float r = hot ? (float)kMarkerR + 1.5f : (float)kMarkerR;

        g.setColour(col);
        if (highContrast) {
            drawShape(g, severityShape(m.severity, m.isStrength), cx, cy, r);
            // A thin outline lifts shapes off the dark high-contrast background.
            g.setColour(KhensuTheme::textPrimary.withAlpha(0.6f));
        } else {
            drawShape(g, Shape::Circle, cx, cy, r);
        }
        if (hot) {
            g.setColour(col.withAlpha(0.9f));
            g.drawEllipse(cx - r - 2.0f, cy - r - 2.0f, (r + 2.0f) * 2.0f, (r + 2.0f) * 2.0f, 1.2f);
        }
    }

    // Playhead.
    if (playheadSec_ >= 0.0f) {
        const float px = xForTime(playheadSec_);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(px), 2.0f, b.getBottom() - 2.0f);
    }
}

void MixNotesTimeline::mouseMove(const juce::MouseEvent& e)
{
    const int idx = markerAt(e.getPosition());
    if (idx != hoverIndex_) {
        hoverIndex_ = idx;
        setMouseCursor(idx >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void MixNotesTimeline::mouseExit(const juce::MouseEvent&)
{
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        repaint();
    }
}

void MixNotesTimeline::mouseDown(const juce::MouseEvent& e)
{
    const int idx = markerAt(e.getPosition());
    if (idx >= 0 && idx < (int)notes_.size())
        controller_.playback().setPlaybackPosition(notes_[(size_t)idx].startSec); // seek to the note
}

juce::String MixNotesTimeline::getTooltip()
{
    if (hoverIndex_ >= 0 && hoverIndex_ < (int)notes_.size()) return notes_[(size_t)hoverIndex_].text;
    return {};
}

void MixNotesTimeline::timerCallback()
{
    const double dur = controller_.playback().getPlaybackDuration();
    const float pos = (float)controller_.playback().getPlaybackPosition();
    bool dirty = false;
    if (std::abs(dur - durationSec_) > 0.01) {
        durationSec_ = dur;
        dirty = true;
    }
    if (std::abs(pos - playheadSec_) > 0.02f) {
        playheadSec_ = pos;
        dirty = true;
    }
    if (dirty) repaint();
}

} // namespace khensu
