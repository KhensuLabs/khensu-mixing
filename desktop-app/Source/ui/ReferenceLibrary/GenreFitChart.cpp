// desktop-app/Source/ui/ReferenceLibrary/GenreFitChart.cpp
#include "GenreFitChart.h"

namespace khensu {

namespace {
constexpr int kHeaderH = 52;
constexpr int kRowH = 22;
constexpr int kMaxRows = 10;
} // namespace

GenreFitChart::GenreFitChart()
{
    setTitle(TRANS("Closest genre fit"));
    setTooltip(TRANS("How close this mix is to each genre's measured norms. Click a bar to pick that genre."));
}

void GenreFitChart::setEntries(std::vector<Entry> entries, const juce::String& emptyText)
{
    entries_ = std::move(entries);
    emptyText_ = emptyText;
    hover_ = -1;
    repaint();
}

void GenreFitChart::setSelectedGenre(const juce::String& genreId)
{
    if (selected_ == genreId) return;
    selected_ = genreId;
    repaint();
}

int GenreFitChart::visibleRows() const
{
    const int fit = juce::jmax(0, (getHeight() - kHeaderH - 4) / kRowH);
    return juce::jmin(kMaxRows, fit, (int)entries_.size());
}

juce::Rectangle<int> GenreFitChart::barsArea() const
{
    auto b = getLocalBounds().reduced(10, 0);
    b.removeFromTop(kHeaderH);
    return b.removeFromTop(visibleRows() * kRowH);
}

int GenreFitChart::rowAt(juce::Point<int> p) const
{
    const auto area = barsArea();
    if (!area.contains(p)) return -1;
    const int r = (p.y - area.getY()) / kRowH;
    return r < visibleRows() ? r : -1;
}

void GenreFitChart::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(b.toFloat(), 6.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 6.0f, 1.0f);

    auto inner = b.reduced(10, 0);
    auto header = inner.removeFromTop(kHeaderH);
    header.removeFromTop(8);
    g.setColour(KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::labelFont(type::body));
    g.drawText(TRANS("Closest genre fit"), header.removeFromTop(18), juce::Justification::centredLeft, true);
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::bodyFont(type::caption));
    g.drawFittedText(TRANS("How close this mix is to each genre's measured norms (loudness, range, tilt, crest)."),
                     header, juce::Justification::topLeft, 2, 1.0f);

    if (entries_.empty()) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.drawFittedText(emptyText_, inner.reduced(0, 6), juce::Justification::centredTop, 3, 1.0f);
        return;
    }

    const auto area = barsArea();
    const int rows = visibleRows();
    const int nameW = juce::jlimit(70, 140, area.getWidth() * 38 / 100);
    const int pctW = 40;
    for (int i = 0; i < rows; ++i) {
        const auto& e = entries_[(size_t)i];
        auto row = juce::Rectangle<int>(area.getX(), area.getY() + i * kRowH, area.getWidth(), kRowH);
        if (i == hover_) {
            g.setColour(KhensuTheme::bgRaised);
            g.fillRect(row);
        }
        auto nameR = row.removeFromLeft(nameW);
        auto pctR = row.removeFromRight(pctW);
        const bool closest = i == 0;
        const bool picked = e.genreId == selected_;
        g.setColour(closest || picked ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawText(e.name, nameR.reduced(2, 0), juce::Justification::centredLeft, true);

        const float match = matchOf(e.distance);
        auto track = row.reduced(4, 6).toFloat();
        g.setColour(KhensuTheme::vizGrid);
        g.fillRoundedRectangle(track, 2.0f);
        auto bar = track.withWidth(juce::jmax(2.0f, track.getWidth() * match));
        g.setColour(closest ? KhensuTheme::vizTrack : KhensuTheme::accentBlue.withAlpha(0.65f));
        g.fillRoundedRectangle(bar, 2.0f);
        if (picked) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRoundedRectangle(track.expanded(1.5f), 3.0f, 1.2f);
        }
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::metricFont(type::caption));
        g.drawText(juce::String(juce::roundToInt(match * 100.0f)) + "%", pctR, juce::Justification::centredRight);
    }

    // Axis caption under the bars.
    auto caption = getLocalBounds().reduced(10, 0).withTop(area.getBottom() + 2).withHeight(16);
    if (caption.getBottom() <= getHeight()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawText(TRANS("match to the genre's targets, closest first"), caption, juce::Justification::centredLeft,
                   true);
    }
}

void GenreFitChart::mouseUp(const juce::MouseEvent& e)
{
    const int r = rowAt(e.getPosition());
    if (r >= 0 && onGenreClicked) onGenreClicked(entries_[(size_t)r].genreId);
}

void GenreFitChart::mouseMove(const juce::MouseEvent& e)
{
    const int r = rowAt(e.getPosition());
    if (r != hover_) {
        hover_ = r;
        setMouseCursor(r >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void GenreFitChart::mouseExit(const juce::MouseEvent&)
{
    if (hover_ != -1) {
        hover_ = -1;
        repaint();
    }
}

} // namespace khensu
