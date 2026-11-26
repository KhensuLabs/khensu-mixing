// desktop-app/Source/ui/SessionIntake/MaskingMatrixComponent.cpp
#include "MaskingMatrixComponent.h"
#include "../shared/KhensuEmptyState.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {

juce::Colour riskColour(double risk)
{
    if (risk >= 0.35) return KhensuTheme::accentRed;
    if (risk >= 0.15) return KhensuTheme::accentAmber;
    if (risk >= 0.05) return KhensuTheme::accentBlue.withAlpha(0.6f);
    return KhensuTheme::bgRaised;
}

constexpr int kNameColumnW = 120;
constexpr int kHeaderH = 18;

} // namespace

// ---------------------------------------------------------------------------
// MaskingMatrixComponent
// ---------------------------------------------------------------------------

MaskingMatrixComponent::MaskingMatrixComponent()
{
    setTitle(TRANS("Masking matrix"));
    setTooltip(TRANS("Track-vs-track masking risk. Hover a cell for the band; click to select the track."));
}

void MaskingMatrixComponent::setAnalysis(const multitrack::Analysis* analysis)
{
    names_.clear();
    matrix_.clear();
    pairs_.clear();
    if (analysis != nullptr) {
        names_ = analysis->matrixTracks;
        matrix_ = analysis->matrix;
        pairs_ = analysis->pairs;
    }
    repaint();
}

void MaskingMatrixComponent::setHighlightedTrack(int index)
{
    highlighted_ = index;
    repaint();
}

juce::Rectangle<int> MaskingMatrixComponent::gridArea() const
{
    auto b = getLocalBounds();
    b.removeFromLeft(kNameColumnW);
    b.removeFromTop(kHeaderH);
    const int n = juce::jmax(1, (int)names_.size());
    const int cell = juce::jmax(4, juce::jmin(b.getWidth() / n, b.getHeight() / n, 40));
    return {b.getX(), b.getY(), cell * (int)names_.size(), cell * (int)names_.size()};
}

std::pair<int, int> MaskingMatrixComponent::cellAt(juce::Point<int> p) const
{
    const auto g = gridArea();
    if (names_.empty() || !g.contains(p)) return {-1, -1};
    const int cell = g.getWidth() / (int)names_.size();
    if (cell <= 0) return {-1, -1};
    return {(p.y - g.getY()) / cell, (p.x - g.getX()) / cell};
}

const multitrack::Pair* MaskingMatrixComponent::pairFor(int row, int col) const
{
    if (row < 0 || col < 0 || row >= (int)names_.size() || col >= (int)names_.size()) return nullptr;
    const auto& a = names_[(size_t)row];
    const auto& b = names_[(size_t)col];
    for (const auto& p : pairs_)
        if ((p.trackA == a && p.trackB == b) || (p.trackA == b && p.trackB == a)) return &p;
    return nullptr;
}

void MaskingMatrixComponent::paint(juce::Graphics& g)
{
    if (names_.empty()) {
        KhensuEmptyState::paintInto(g, getLocalBounds(), TRANS("Masking appears here after the stems are analyzed."), {}); // UXR-6
        return;
    }
    const auto grid = gridArea();
    const int n = (int)names_.size();
    const int cell = grid.getWidth() / n;
    const auto font = KhensuTheme::monoFont(KhensuTheme::kFontCaption);
    g.setFont(font);

    for (int r = 0; r < n; ++r) {
        const auto rowBox = juce::Rectangle<int>(0, grid.getY() + r * cell, kNameColumnW - 4, cell);
        g.setColour(r == highlighted_ ? KhensuTheme::textAccent : KhensuTheme::textSecondary);
        g.drawText(juce::String(r + 1) + " " + names_[(size_t)r], rowBox, juce::Justification::centredRight, true);
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(r + 1), juce::Rectangle<int>(grid.getX() + r * cell, 0, cell, kHeaderH),
                   juce::Justification::centred, false);
        for (int c = 0; c < n; ++c) {
            auto box = juce::Rectangle<int>(grid.getX() + c * cell, grid.getY() + r * cell, cell, cell).reduced(1);
            if (r == c) {
                g.setColour(KhensuTheme::bgBase);
                g.fillRect(box);
                continue;
            }
            const double risk = ((size_t)r < matrix_.size() && (size_t)c < matrix_[(size_t)r].size())
                                    ? matrix_[(size_t)r][(size_t)c]
                                    : 0.0;
            g.setColour(riskColour(risk));
            g.fillRect(box);
            if (r == highlighted_ || c == highlighted_) {
                g.setColour(KhensuTheme::focusRing.withAlpha(0.5f));
                g.drawRect(box, 1);
            }
        }
    }
}

void MaskingMatrixComponent::mouseMove(const juce::MouseEvent& e)
{
    const auto [r, c] = cellAt(e.getPosition());
    if (const auto* p = pairFor(r, c); p != nullptr && r != c)
        setTooltip(p->summary + " - " + TRANS("risk") + " " + juce::String(p->risk, 2) + " (" + p->riskLevel + ")");
    else if (r >= 0 && r == c)
        setTooltip(names_[(size_t)r]);
    else
        setTooltip(TRANS("Track-vs-track masking risk. Hover a cell for the band; click to select the track."));
}

void MaskingMatrixComponent::mouseUp(const juce::MouseEvent& e)
{
    const auto [r, c] = cellAt(e.getPosition());
    juce::ignoreUnused(c);
    if (r >= 0 && onTrackClicked) onTrackClicked(r);
}

} // namespace khensu
