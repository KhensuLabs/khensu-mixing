// desktop-app/Source/ui/shared/HeroSpectrumView.cpp
#include "HeroSpectrumView.h"
#include "ValueAnimator.h" // motion::easeOutCubic (UI-7)
#include "ChartStyle.h"       // VIZ-U3 readout
#include <khensu/ui/VizPalette.h>
#include <algorithm>
#include <cmath>

namespace khensu {

using hero_spectrum::Finding;
using hero_spectrum::Shape;

namespace {
constexpr float kMinHz = 20.0f;
constexpr float kMaxHz = 20000.0f;
constexpr float kHeaderH = 44.0f;
constexpr float kAxisH = 18.0f;
constexpr float kLeftW = 40.0f;
constexpr float kRightW = 12.0f;
constexpr float kFindingsLabelW = 96.0f;

juce::String hzLabel(float hz)
{
    if (hz >= 1000.0f) return juce::String(juce::roundToInt(hz / 1000.0f)) + "k";
    return juce::String(juce::roundToInt(hz));
}

juce::String severityWord(float severity)
{
    if (severity >= 66.0f) return TRANS("Severe");
    if (severity >= 33.0f) return TRANS("Moderate");
    return TRANS("Minor");
}
} // namespace

HeroSpectrumView::HeroSpectrumView()
{
    setTitle(TRANS("Spectrum and findings"));
    setDescription(TRANS("The track's spectrum against the genre target, with each finding drawn where it lives"));
    setTooltip(TRANS("The track's spectrum against the genre target and its tolerance. Click a finding to select "
                     "its issue."));
    setWantsKeyboardFocus(false);
    setOpaque(false);
    vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onVBlank(); });
}

void HeroSpectrumView::onVBlank()
{
    if (originalProbe_ && isShowing()) {
        const float target = originalProbe_() ? 0.0f : 1.0f;
        if (target != abTo_) {
            abFrom_ = abWeight();
            abTo_ = target;
            abStartMs_ = juce::Time::getMillisecondCounterHiRes();
        }
    }
    if (juce::Time::getMillisecondCounterHiRes() - abStartMs_ <= kAnimMs + 20.0) repaint();
}

float HeroSpectrumView::abWeight() const noexcept
{
    const double t = juce::jlimit(0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - abStartMs_) / kAnimMs);
    return abFrom_ + (abTo_ - abFrom_) * motion::easeOutCubic(static_cast<float>(t));
}

HeroSpectrumView::~HeroSpectrumView() { stopTimer(); }

// ---- Data ------------------------------------------------------------------------

void HeroSpectrumView::setModel(hero_spectrum::Model model, std::vector<float> afterResponse, bool animate)
{
    const bool sameGrid = model.spectrum.size() == model_.spectrum.size() && !model_.spectrum.empty();
    // What is on screen now becomes the start of the morph.
    std::vector<float> nowAfter(model.spectrum.size(), 0.0f);
    if (sameGrid)
        for (size_t i = 0; i < nowAfter.size(); ++i)
            nowAfter[i] = shownAfter(i);
    std::map<std::string, float> nowRemaining;
    for (const auto& f : model.findings) {
        const auto key = f.key();
        nowRemaining[key] = remainingTo_.count(key) != 0 ? shownRemaining(key) : f.remaining;
    }
    if (afterResponse.size() != model.spectrum.size()) afterResponse.assign(model.spectrum.size(), 0.0f);

    model_ = std::move(model);
    afterTo_ = std::move(afterResponse);
    remainingTo_.clear();
    for (const auto& f : model_.findings)
        remainingTo_[f.key()] = f.remaining;
    if (preview_.size() != model_.spectrum.size()) preview_.clear();

    const bool changed = nowAfter != afterTo_ || nowRemaining != remainingTo_;
    if (animate && sameGrid && changed && isShowing()) {
        afterFrom_ = std::move(nowAfter);
        remainingFrom_ = std::move(nowRemaining);
        animStartMs_ = juce::Time::getMillisecondCounterHiRes();
        startTimerHz(60);
    } else {
        stopTimer();
        afterFrom_ = afterTo_;
        remainingFrom_ = remainingTo_;
    }
    updateDbRange();
    repaint();
}

void HeroSpectrumView::setPreviewResponse(std::vector<float> response)
{
    preview_ = response.size() == model_.spectrum.size() ? std::move(response) : std::vector<float>{};
    repaint();
}

void HeroSpectrumView::setSelected(const std::string& key)
{
    if (key == selected_) return;
    selected_ = key;
    repaint();
}

void HeroSpectrumView::updateDbRange()
{
    if (!model_.hasSpectrum()) {
        dbTop_ = 12.0f;
        dbBottom_ = -36.0f;
        return;
    }
    float lo = 1e9f, hi = -1e9f;
    for (const auto& p : model_.spectrum) {
        if (p.hz < 30.0f || p.hz > 17000.0f) continue;
        lo = std::min(lo, p.db);
        hi = std::max(hi, p.db);
    }
    if (lo > hi) {
        lo = -24.0f;
        hi = 6.0f;
    }
    dbTop_ = std::ceil((hi + 6.0f) / 6.0f) * 6.0f;
    dbBottom_ = std::min(dbTop_ - 42.0f, std::floor((lo - 6.0f) / 6.0f) * 6.0f);
}

// ---- Animation -------------------------------------------------------------------

void HeroSpectrumView::timerCallback()
{
    if (juce::Time::getMillisecondCounterHiRes() - animStartMs_ >= kAnimMs) {
        stopTimer();
        afterFrom_ = afterTo_;
        remainingFrom_ = remainingTo_;
    }
    repaint();
}

float HeroSpectrumView::eased() const noexcept
{
    if (!isTimerRunning()) return 1.0f;
    const double t = juce::jlimit(0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - animStartMs_) / kAnimMs);
    return motion::easeOutCubic(static_cast<float>(t)); // the app's one curve (UI-7)
}

float HeroSpectrumView::shownAfter(size_t i) const noexcept
{
    const float to = i < afterTo_.size() ? afterTo_[i] : 0.0f;
    const float from = i < afterFrom_.size() ? afterFrom_[i] : to;
    return (from + (to - from) * eased()) * abWeight(); // VIZ-3c: A glides to the original
}

float HeroSpectrumView::shownRemaining(const std::string& key) const noexcept
{
    const auto to = remainingTo_.find(key);
    if (to == remainingTo_.end()) return 1.0f;
    const auto from = remainingFrom_.find(key);
    const float a = from != remainingFrom_.end() ? from->second : to->second;
    return a + (to->second - a) * eased();
}

// ---- Layout ----------------------------------------------------------------------

juce::Rectangle<float> HeroSpectrumView::plotArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    b.removeFromTop(kHeaderH);
    b.removeFromBottom(static_cast<float>(kStripH) + 10.0f);
    b.removeFromBottom(kAxisH);
    b.removeFromLeft(kLeftW);
    b.removeFromRight(kRightW);
    return b.getHeight() > 10.0f && b.getWidth() > 10.0f ? b : juce::Rectangle<float>();
}

juce::Rectangle<float> HeroSpectrumView::stripArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    return b.removeFromBottom(static_cast<float>(kStripH));
}

juce::Rectangle<float> HeroSpectrumView::tilesArea() const noexcept
{
    return stripArea().removeFromTop(static_cast<float>(kTilesH)).reduced(0.0f, 2.0f);
}

juce::Rectangle<float> HeroSpectrumView::findingsArea() const noexcept
{
    return stripArea().removeFromBottom(static_cast<float>(kFindingsH));
}

float HeroSpectrumView::xForHz(float hz) const noexcept
{
    const auto p = plotArea();
    const float t = std::log(juce::jlimit(kMinHz, kMaxHz, hz) / kMinHz) / std::log(kMaxHz / kMinHz);
    return p.getX() + t * p.getWidth();
}

float HeroSpectrumView::yForDb(float db) const noexcept
{
    const auto p = plotArea();
    const float t = (dbTop_ - db) / std::max(1.0f, dbTop_ - dbBottom_);
    return p.getY() + juce::jlimit(0.0f, 1.0f, t) * p.getHeight();
}

std::vector<juce::Rectangle<float>> HeroSpectrumView::tileCells() const
{
    std::vector<juce::Rectangle<float>> out;
    const auto area = tilesArea();
    const float gap = 8.0f;
    const int n = hero_spectrum::kGaugeCount;
    const float w = (area.getWidth() - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
        out.push_back({area.getX() + static_cast<float>(i) * (w + gap), area.getY(), w, area.getHeight()});
    return out;
}

int HeroSpectrumView::tileAt(juce::Point<float> pos) const
{
    const auto cells = tileCells();
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].contains(pos)) return static_cast<int>(i);
    return -1;
}

std::vector<HeroSpectrumView::ChipCell> HeroSpectrumView::chipCells() const
{
    std::vector<ChipCell> cells;
    auto row = findingsArea();
    row.removeFromLeft(kFindingsLabelW);
    std::vector<int> chips;
    for (size_t i = 0; i < model_.findings.size(); ++i)
        if (model_.findings[i].shape == Shape::Chip) chips.push_back(static_cast<int>(i));
    // Open first, worst first.
    std::stable_sort(chips.begin(), chips.end(), [this](int a, int b) {
        const auto& fa = model_.findings[static_cast<size_t>(a)];
        const auto& fb = model_.findings[static_cast<size_t>(b)];
        if (fa.settled != fb.settled) return !fa.settled;
        return fa.severity > fb.severity;
    });
    const auto font = KhensuTheme::bodyFont(12.0f);
    const float gap = 6.0f, overflowW = 44.0f;
    float x = row.getX();
    for (size_t k = 0; k < chips.size(); ++k) {
        const auto& f = model_.findings[static_cast<size_t>(chips[k])];
        const float w = juce::GlyphArrangement::getStringWidth(font, f.chipLabel) + 34.0f;
        const bool last = k + 1 == chips.size();
        if (x + w > row.getRight() - (last ? 0.0f : overflowW + gap)) {
            cells.push_back({{x, row.getY() + 3.0f, overflowW, row.getHeight() - 6.0f}, -1,
                             static_cast<int>(chips.size() - k)});
            break;
        }
        cells.push_back({{x, row.getY() + 3.0f, w, row.getHeight() - 6.0f}, chips[k], 0});
        x += w + gap;
    }
    return cells;
}

// ---- Hit testing -----------------------------------------------------------------

std::string HeroSpectrumView::findingAt(juce::Point<float> pos) const
{
    // The "Top findings" row: a chip selects its finding.
    for (const auto& c : chipCells())
        if (c.bounds.contains(pos))
            return c.finding >= 0 ? model_.findings[static_cast<size_t>(c.finding)].key() : std::string();
    const auto plot = plotArea();
    if (!plot.expanded(0.0f, 6.0f).contains(pos)) return {};
    // Markers first (the smallest targets), then the tilt line, then the
    // narrowest band under the pointer.
    for (const auto& f : model_.findings)
        if (f.shape == Shape::Marker && std::abs(xForHz(f.hz) - pos.x) <= 8.0f) return f.key();
    for (const auto& f : model_.findings)
        if (f.shape == Shape::Tilt && model_.hasSpectrum()) {
            const float hz = kMinHz * std::pow(kMaxHz / kMinHz, (pos.x - plot.getX()) / plot.getWidth());
            const float lineDb = model_.tiltMeasured * std::log2(hz / 1000.0f);
            if (std::abs(yForDb(lineDb) - pos.y) <= 7.0f) return f.key();
        }
    const Finding* best = nullptr;
    for (const auto& f : model_.findings) {
        if (f.shape != Shape::Band) continue;
        if (pos.x < xForHz(f.lowHz) || pos.x > xForHz(f.highHz)) continue;
        if (best == nullptr || f.highHz / f.lowHz < best->highHz / best->lowHz) best = &f;
    }
    return best != nullptr ? best->key() : std::string();
}

void HeroSpectrumView::mouseMove(const juce::MouseEvent& e)
{
    hoverPos_ = e.position;
    const auto key = findingAt(e.position);
    const bool inPlot = plotArea().contains(e.position) && model_.hasSpectrum();
    setMouseCursor(!key.empty() || tileAt(e.position) >= 0 ? juce::MouseCursor::PointingHandCursor
                                                          : juce::MouseCursor::NormalCursor);
    if (key != hovered_ || !key.empty() || inPlot || inPlot != hoverInPlot_) {
        hovered_ = key;
        hoverInPlot_ = inPlot;
        repaint();
    }
}

void HeroSpectrumView::mouseExit(const juce::MouseEvent&)
{
    if (hovered_.empty() && !hoverInPlot_) return;
    hovered_.clear();
    hoverInPlot_ = false;
    repaint();
}

void HeroSpectrumView::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown()) return;
    if (const int tile = tileAt(e.position); tile >= 0) {
        if (onMetricClicked) onMetricClicked(tile);
        return;
    }
    const auto key = findingAt(e.position);
    if (key.empty()) return;
    setSelected(key);
    if (onFindingClicked) onFindingClicked(key);
}

// ---- Painting --------------------------------------------------------------------

juce::Colour HeroSpectrumView::colourFor(const Finding& f) const
{
    if (f.settled) return KhensuTheme::textTertiary;
    if (f.severity >= 66.0f) return KhensuTheme::accentRed;
    if (f.severity >= 33.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentBlue;
}

juce::Path HeroSpectrumView::curvePath(juce::Rectangle<float> plot, const std::vector<float>& offsets,
                                       float amount) const
{
    juce::Path path;
    const auto& pts = model_.spectrum;
    bool first = true;
    for (size_t i = 0; i < pts.size(); ++i) {
        const float off = i < offsets.size() ? offsets[i] * amount : 0.0f;
        const float x = xForHz(pts[i].hz);
        const float y = yForDb(pts[i].db + off);
        if (first) {
            path.startNewSubPath(x, y);
            first = false;
        } else {
            // A gentle quadratic through midpoints keeps the 1/6-octave
            // steps from reading as a polyline.
            const float px = xForHz(pts[i - 1].hz);
            const float poff = (i - 1) < offsets.size() ? offsets[i - 1] * amount : 0.0f;
            const float py = yForDb(pts[i - 1].db + poff);
            path.quadraticTo(px, py, (px + x) * 0.5f, (py + y) * 0.5f);
        }
    }
    if (!pts.empty()) {
        const float off = offsets.size() == pts.size() ? offsets.back() * amount : 0.0f;
        path.lineTo(xForHz(pts.back().hz), yForDb(pts.back().db + off));
    }
    juce::ignoreUnused(plot);
    return path;
}

void HeroSpectrumView::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    // The display well: a recessed surface, like a hardware screen.
    auto well = bounds.withTrimmedBottom(static_cast<float>(kStripH) + 10.0f);
    juce::ignoreUnused(bounds);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.55f));
    g.fillRoundedRectangle(well, 8.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(well.reduced(0.5f), 8.0f, 1.0f);

    paintHeader(g);
    const auto plot = plotArea();
    if (!plot.isEmpty()) {
        paintGrid(g, plot);
        if (model_.hasSpectrum()) {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(plot.toNearestInt().expanded(0, 2));
            paintSpread(g, plot);
            paintFindings(g, plot);
            paintTarget(g, plot);
            paintReference(g, plot);
            paintSpectrum(g, plot);
        } else {
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::bodyFont(13.0f));
            g.drawText(TRANS("The spectrum appears when the analysis finishes."), plot,
                       juce::Justification::centred, true);
        }
    }
    paintTiles(g);
    paintStrip(g);
    paintCrosshair(g);
    paintHover(g);
}

void HeroSpectrumView::paintHeader(juce::Graphics& g) const
{
    auto h = getLocalBounds().toFloat().removeFromTop(kHeaderH).reduced(14.0f, 6.0f);
    // Left: the measured tilt (largest) and the target it is held to.
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(11.0f));
    auto left = h.removeFromLeft(std::min(h.getWidth() * 0.55f, 420.0f));
    g.drawText(TRANS("Tonal balance"), left.removeFromTop(14.0f), juce::Justification::centredLeft, false);
    if (model_.hasSpectrum()) {
        const juce::String tilt = juce::String(model_.tiltMeasured, 1);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::metricFont(17.0f).boldened());
        const auto tiltFont = KhensuTheme::metricFont(17.0f).boldened();
        const float tw = juce::GlyphArrangement::getStringWidth(tiltFont, tilt);
        g.drawText(tilt, left.removeFromLeft(tw + 2.0f), juce::Justification::centredLeft, false);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(11.0f));
        juce::String rest = " " + TRANS("dB/oct tilt");
        if (model_.tiltTarget)
            rest << "   " << TRANS("target") << " " << juce::String(*model_.tiltTarget, 1) << " +/- "
                 << juce::String(model_.tiltTolerance, 1)
                 << (model_.genreName.isNotEmpty() ? " (" + model_.genreName + ")" : juce::String());
        g.drawText(rest, left, juce::Justification::centredLeft, true);
    }

    // Right: a legend, only for what is drawn.
    struct Key {
        juce::String text;
        juce::Colour colour;
        bool dashed;
    };
    std::vector<Key> keys;
    bool anyAfter = false;
    for (size_t i = 0; i < afterTo_.size(); ++i)
        anyAfter = anyAfter || std::abs(afterTo_[i]) > 0.05f;
    if (model_.hasSpectrum())
        keys.push_back({anyAfter ? (showingOriginal() ? TRANS("Playing the original (A)") : TRANS("Current version (expected)"))
                                 : TRANS("Track"),
                        anyAfter ? KhensuTheme::accentTeal : KhensuTheme::textPrimary, false});
    if (anyAfter) keys.push_back({TRANS("Original"), KhensuTheme::textTertiary, true});
    if (model_.hasSpread())
        keys.push_back({TRANS("Released") + " " + model_.spreadName + " (" + juce::String(model_.spreadTracks) + ")",
                        KhensuTheme::vizSpreadInner.withAlpha(0.9f), false});
    if (!model_.reference.empty()) keys.push_back({model_.referenceName, KhensuTheme::vizReference, false});
    if (model_.tiltTarget) keys.push_back({TRANS("Genre target"), KhensuTheme::accentTeal.withAlpha(0.7f), true});
    if (!preview_.empty()) keys.push_back({TRANS("Fix preview"), KhensuTheme::accentTeal, true});
    auto right = h;
    g.setFont(KhensuTheme::labelFont(11.0f));
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        const float w = juce::GlyphArrangement::getStringWidth(KhensuTheme::labelFont(11.0f), it->text) + 28.0f;
        auto cell = right.removeFromRight(w).withSizeKeepingCentre(w, 16.0f);
        right.removeFromRight(10.0f);
        auto sw = cell.removeFromLeft(18.0f).withSizeKeepingCentre(16.0f, 2.0f);
        g.setColour(it->colour);
        if (it->dashed) {
            g.fillRect(sw.withWidth(5.0f));
            g.fillRect(sw.withTrimmedLeft(8.0f).withWidth(5.0f));
        } else {
            g.fillRect(sw);
        }
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(it->text, cell.withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
    }
}

void HeroSpectrumView::paintGrid(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    g.setFont(KhensuTheme::labelFont(11.0f));
    for (const float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        const float x = xForHz(hz);
        g.setColour(KhensuTheme::borderSubtle.withMultipliedAlpha(0.6f));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(hzLabel(hz), juce::Rectangle<float>(x - 20.0f, plot.getBottom() + 2.0f, 40.0f, kAxisH - 2.0f),
                   juce::Justification::centredTop, false);
    }
    for (float db = dbTop_; db >= dbBottom_ - 0.01f; db -= 12.0f) {
        const float y = yForDb(db);
        g.setColour(KhensuTheme::borderSubtle.withMultipliedAlpha(0.6f));
        g.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(juce::String(juce::roundToInt(db)),
                   juce::Rectangle<float>(plot.getX() - kLeftW, y - 7.0f, kLeftW - 6.0f, 14.0f),
                   juce::Justification::centredRight, false);
    }
}

void HeroSpectrumView::paintTarget(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (!model_.tiltTarget) return;
    const float target = *model_.tiltTarget;
    const float tol = model_.tiltTolerance;
    auto lineAt = [&](float slope, float hz) { return yForDb(slope * std::log2(hz / 1000.0f)); };
    // The tolerance: a fan around the target line, pivoting at 1 kHz.
    juce::Path fan;
    const int steps = 48;
    for (int i = 0; i <= steps; ++i) {
        const float hz = kMinHz * std::pow(kMaxHz / kMinHz, static_cast<float>(i) / static_cast<float>(steps));
        const float x = xForHz(hz);
        const float y = lineAt(target + (hz >= 1000.0f ? tol : -tol), hz);
        if (i == 0) fan.startNewSubPath(x, y);
        else fan.lineTo(x, y);
    }
    for (int i = steps; i >= 0; --i) {
        const float hz = kMinHz * std::pow(kMaxHz / kMinHz, static_cast<float>(i) / static_cast<float>(steps));
        fan.lineTo(xForHz(hz), lineAt(target + (hz >= 1000.0f ? -tol : tol), hz));
    }
    fan.closeSubPath();
    if (!model_.hasSpread()) { // the measured spread says more than a fan
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.07f));
        g.fillPath(fan);
    }

    juce::Path line;
    line.startNewSubPath(plot.getX(), lineAt(target, kMinHz));
    line.lineTo(plot.getRight(), lineAt(target, kMaxHz));
    juce::Path dashed;
    const float dashes[] = {6.0f, 5.0f};
    juce::PathStrokeType(1.4f).createDashedStroke(dashed, line, dashes, 2);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.7f));
    g.fillPath(dashed); // createDashedStroke returns a filled outline
}

void HeroSpectrumView::paintSpectrum(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    std::vector<float> after(model_.spectrum.size(), 0.0f);
    bool anyAfter = false;
    for (size_t i = 0; i < after.size(); ++i) {
        after[i] = shownAfter(i);
        anyAfter = anyAfter || std::abs(after[i]) > 0.05f || (i < afterTo_.size() && std::abs(afterTo_[i]) > 0.05f);
    }
    const auto main = curvePath(plot, after, 1.0f);

    // The fill under the curve (a whisper of the curve's colour).
    juce::Path fill(main);
    fill.lineTo(xForHz(model_.spectrum.back().hz), plot.getBottom());
    fill.lineTo(xForHz(model_.spectrum.front().hz), plot.getBottom());
    fill.closeSubPath();
    const auto curveColour = anyAfter ? KhensuTheme::accentTeal : KhensuTheme::textPrimary;
    g.setGradientFill(juce::ColourGradient(curveColour.withAlpha(0.16f), 0.0f, plot.getY(),
                                           curveColour.withAlpha(0.0f), 0.0f, plot.getBottom(), false));
    g.fillPath(fill);

    if (anyAfter) { // the original, as a ghost under the current version
        const auto original = curvePath(plot, {}, 0.0f);
        juce::Path dashed;
        const float dashes[] = {3.0f, 3.0f};
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, original, dashes, 2);
        g.setColour(KhensuTheme::textTertiary);
        g.fillPath(dashed);
    }
    g.setColour(curveColour.withAlpha(0.92f));
    g.strokePath(main, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (model_.hasSpread() && !anyAfter) {
        // Where the track leaves the middle 50 % of released tracks, the line
        // takes the diverging colour: red = more energy than released tracks,
        // blue = less; full strength at the edge of the middle 80 %.
        const auto& pts = model_.spectrum;
        for (size_t i = 1; i < pts.size(); ++i) {
            const float hz = std::sqrt(pts[i - 1].hz * pts[i].hz);
            const float stray = hero_spectrum::strayAt(model_, hz, 0.5f * (pts[i - 1].db + pts[i].db));
            if (std::abs(stray) < 0.05f) continue;
            const auto pole = juce::Colour(stray > 0.0f ? ui::viz::kDivergingWarm : ui::viz::kDivergingCool);
            g.setColour(curveColour.interpolatedWith(pole.brighter(0.25f), std::abs(stray)));
            g.drawLine(xForHz(pts[i - 1].hz), yForDb(pts[i - 1].db), xForHz(pts[i].hz), yForDb(pts[i].db),
                       2.4f);
        }
    }

    if (!preview_.empty()) { // the selected fix at the chosen amount, before it renders
        std::vector<float> combined(after);
        for (size_t i = 0; i < combined.size() && i < preview_.size(); ++i)
            combined[i] += preview_[i];
        const auto pv = curvePath(plot, combined, 1.0f);
        juce::Path dashed;
        const float dashes[] = {5.0f, 3.0f};
        juce::PathStrokeType(1.6f).createDashedStroke(dashed, pv, dashes, 2);
        g.setColour(KhensuTheme::accentTeal);
        g.fillPath(dashed);
    }
}

void HeroSpectrumView::paintFindings(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    // Settled first so open findings draw over them; the selection last.
    std::vector<const Finding*> order;
    for (const auto& f : model_.findings)
        if (f.shape != Shape::Chip) order.push_back(&f);
    std::stable_sort(order.begin(), order.end(), [this](const Finding* a, const Finding* b) {
        const bool sa = a->key() == selected_, sb = b->key() == selected_;
        if (sa != sb) return sb;
        if (a->settled != b->settled) return a->settled;
        return a->severity < b->severity;
    });
    for (const auto* f : order) {
        const bool sel = f->key() == selected_;
        const bool hov = f->key() == hovered_;
        const float remain = juce::jlimit(0.0f, 1.0f, shownRemaining(f->key()));
        // A resolved mark stays as a faint outline: where the problem was.
        const float presence = 0.25f + 0.75f * remain;
        const auto c = colourFor(*f);
        switch (f->shape) {
        case Shape::Band: {
            const float x0 = xForHz(f->lowHz), x1 = xForHz(f->highHz);
            auto col = juce::Rectangle<float>(x0, plot.getY(), std::max(2.0f, x1 - x0), plot.getHeight());
            g.setColour(c.withAlpha((sel ? 0.20f : hov ? 0.15f : 0.09f) * presence));
            g.fillRect(col);
            // The cap: a bar whose thickness follows what is left of the finding.
            const float capH = (sel ? 4.0f : 3.0f) * (0.4f + 0.6f * remain);
            g.setColour(c.withAlpha(0.95f * presence));
            g.fillRect(col.withHeight(capH));
            if (sel || hov) {
                g.setColour(c.withAlpha(0.6f));
                g.drawVerticalLine(juce::roundToInt(x0), plot.getY(), plot.getBottom());
                g.drawVerticalLine(juce::roundToInt(x1), plot.getY(), plot.getBottom());
            }
            break;
        }
        case Shape::Marker: {
            const float x = xForHz(f->hz);
            float db = 0.0f;
            for (size_t i = 0; i + 1 < model_.spectrum.size(); ++i)
                if (model_.spectrum[i + 1].hz >= f->hz) {
                    db = model_.spectrum[i].db;
                    break;
                }
            const float y = yForDb(db) - 10.0f;
            const float r = (sel ? 6.5f : 5.0f) * (0.5f + 0.5f * remain);
            g.setColour(c.withAlpha(0.35f * presence));
            g.drawVerticalLine(juce::roundToInt(x), y, plot.getBottom());
            juce::Path tri;
            tri.addTriangle(x, y + r, x - r, y - r, x + r, y - r);
            g.setColour(c.withAlpha(presence));
            g.fillPath(tri);
            if (sel) {
                g.setColour(KhensuTheme::textPrimary);
                g.strokePath(tri, juce::PathStrokeType(1.0f));
            }
            break;
        }
        case Shape::Tilt: {
            // The measured slope, through the same 1 kHz pivot as the target.
            juce::Path line;
            line.startNewSubPath(plot.getX(), yForDb(model_.tiltMeasured * std::log2(kMinHz / 1000.0f)));
            line.lineTo(plot.getRight(), yForDb(model_.tiltMeasured * std::log2(kMaxHz / 1000.0f)));
            g.setColour(c.withAlpha(0.9f * presence));
            g.strokePath(line, juce::PathStrokeType((sel || hov ? 2.6f : 1.8f) * (0.5f + 0.5f * remain)));
            break;
        }
        case Shape::Chip:
            break;
        }
    }
    // The selected finding names itself on the graph.
    for (const auto* f : order)
        if (f->key() == selected_ && f->shape != Shape::Chip) {
            const float x = f->shape == Shape::Band ? (xForHz(f->lowHz) + xForHz(f->highHz)) * 0.5f
                            : f->shape == Shape::Marker ? xForHz(f->hz)
                                                        : plot.getRight() - 140.0f;
            const auto font = KhensuTheme::labelFont(11.0f);
            const juce::String text = severityWord(f->severity) + "  " + f->title;
            const float w = std::min(320.0f, juce::GlyphArrangement::getStringWidth(font, text) + 16.0f);
            auto tag = juce::Rectangle<float>(juce::jlimit(plot.getX(), plot.getRight() - w, x - w * 0.5f),
                                              plot.getY() + 8.0f, w, 20.0f);
            g.setColour(KhensuTheme::bgOverlay.withAlpha(0.92f));
            g.fillRoundedRectangle(tag, 4.0f);
            g.setColour(colourFor(*f));
            g.fillRect(tag.removeFromLeft(3.0f).reduced(0.0f, 4.0f));
            g.setColour(KhensuTheme::textPrimary);
            g.setFont(font);
            g.drawText(text, tag.reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);
        }
}

void HeroSpectrumView::paintSpread(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (!model_.hasSpread()) return;
    juce::ignoreUnused(plot);
    auto band = [this](int lo, int hi) {
        juce::Path p;
        const auto& sp = model_.spread;
        p.startNewSubPath(xForHz(sp.front().hz), yForDb(sp.front().p[(size_t)hi]));
        for (size_t i = 1; i < sp.size(); ++i)
            p.lineTo(xForHz(sp[i].hz), yForDb(sp[i].p[(size_t)hi]));
        for (size_t i = sp.size(); i-- > 0;)
            p.lineTo(xForHz(sp[i].hz), yForDb(sp[i].p[(size_t)lo]));
        p.closeSubPath();
        return p;
    };
    g.setColour(KhensuTheme::vizSpreadOuter);
    g.fillPath(band(GenreSpread::kP10, GenreSpread::kP90));
    g.setColour(KhensuTheme::vizSpreadInner);
    g.fillPath(band(GenreSpread::kP25, GenreSpread::kP75));
    juce::Path median;
    for (size_t i = 0; i < model_.spread.size(); ++i) {
        const float x = xForHz(model_.spread[i].hz), y = yForDb(model_.spread[i].p[GenreSpread::kP50]);
        if (i == 0) median.startNewSubPath(x, y);
        else median.lineTo(x, y);
    }
    juce::Path dashed;
    const float dashes[] = {2.0f, 4.0f};
    juce::PathStrokeType(1.0f).createDashedStroke(dashed, median, dashes, 2);
    g.setColour(KhensuTheme::vizMedian);
    g.fillPath(dashed); // createDashedStroke returns a filled outline
}

void HeroSpectrumView::paintReference(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (model_.reference.size() < 4) return;
    juce::ignoreUnused(plot);
    juce::Path p;
    for (size_t i = 0; i < model_.reference.size(); ++i) {
        const float x = xForHz(model_.reference[i].hz), y = yForDb(model_.reference[i].db);
        if (i == 0) p.startNewSubPath(x, y);
        else p.lineTo(x, y);
    }
    g.setColour(KhensuTheme::vizReference.withAlpha(0.9f));
    g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HeroSpectrumView::paintTiles(juce::Graphics& g) const
{
    // VIZ-P2: measurements only - neutral tiles; findings have their own row.
    const auto cells = tileCells();
    for (size_t gi = 0; gi < cells.size() && gi < model_.gauges.size(); ++gi) {
        const auto& gauge = model_.gauges[gi];
        auto b = cells[gi];
        g.setColour(KhensuTheme::bgRaised.withAlpha(0.6f));
        g.fillRoundedRectangle(b, 6.0f);
        auto inner = b.reduced(12.0f, 7.0f);
        auto top = inner.removeFromTop(14.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(11.0f));
        g.drawText(gauge.label, top, juce::Justification::centredLeft, false);
        if (gauge.windowText.isNotEmpty() && inner.getWidth() > 150.0f)
            g.drawText(gauge.windowText, top, juce::Justification::centredRight, false);
        // VIZ-P6: undefined is "-" in neutral, never a coloured value.
        g.setColour(gauge.defined ? KhensuTheme::textPrimary : KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::metricFont(17.0f));
        g.drawText(gauge.defined ? gauge.value : juce::String("-"), inner.removeFromTop(22.0f),
                   juce::Justification::centredLeft, false);

        // VIZ-1b: a strip of dots, one per released track, this track marked.
        auto strip = inner.removeFromTop(16.0f);
        float lo = gauge.axisLo, hi = gauge.axisHi;
        if (!gauge.released.empty()) {
            const auto& v = gauge.released;
            lo = v[(size_t)(0.02 * (double)(v.size() - 1))];
            hi = v[(size_t)(0.98 * (double)(v.size() - 1))];
            if (gauge.defined) {
                lo = std::min(lo, gauge.measured);
                hi = std::max(hi, gauge.measured);
            }
            const float pad = std::max(1e-3f, (hi - lo) * 0.06f);
            lo -= pad;
            hi += pad;
        }
        auto at = [&](float v) {
            const float t = (v - lo) / std::max(1e-6f, hi - lo);
            return strip.getX() + juce::jlimit(0.0f, 1.0f, t) * strip.getWidth();
        };
        if (gauge.windowLo && gauge.windowHi) { // the genre window, as an underlay
            const float x0 = at(*gauge.windowLo), x1 = at(*gauge.windowHi);
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.14f));
            g.fillRoundedRectangle({x0, strip.getY() + 1.0f, std::max(2.0f, x1 - x0), strip.getHeight() - 2.0f}, 3.0f);
        }
        if (!gauge.released.empty()) {
            g.setColour(KhensuTheme::textSecondary.withAlpha(0.55f));
            juce::Random jitter(17 + (juce::int64)gi); // stable: the same dots every repaint
            // At most ~200 dots (evenly through the sorted values): the whole
            // corpus is 1 400+ tracks, which smears into a solid bar.
            const size_t stride = std::max<size_t>(1, gauge.released.size() / 200);
            for (size_t k = 0; k < gauge.released.size(); k += stride) {
                const float y = strip.getY() + 3.0f + jitter.nextFloat() * (strip.getHeight() - 6.0f);
                g.fillEllipse(at(gauge.released[k]) - 1.3f, y - 1.3f, 2.6f, 2.6f);
            }
        } else {
            g.setColour(KhensuTheme::borderSubtle);
            g.fillRoundedRectangle(strip.withSizeKeepingCentre(strip.getWidth(), 3.0f), 1.5f);
        }
        if (gauge.reference) { // VIZ-N1: the reference, a violet diamond
            const float x = at(*gauge.reference);
            juce::Path d;
            d.addTriangle(x, strip.getY() - 1.0f, x - 4.0f, strip.getY() - 7.0f, x + 4.0f, strip.getY() - 7.0f);
            g.setColour(KhensuTheme::vizReference);
            g.fillPath(d);
        }
        if (gauge.defined) {
            const float x = at(gauge.measured);
            g.setColour(KhensuTheme::vizTrack);
            g.fillRoundedRectangle({x - 1.5f, strip.getY() - 1.0f, 3.0f, strip.getHeight() + 2.0f}, 1.5f);
        }
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(11.0f));
        const juce::String caption = !gauge.defined ? TRANS("Not measured on this file")
                                     : gauge.caption.isNotEmpty() ? gauge.caption
                                     : gauge.windowText.isNotEmpty() ? TRANS("Target") + " " + gauge.windowText
                                                                     : juce::String();
        g.drawText(caption, inner, juce::Justification::centredLeft, true);
    }
}

void HeroSpectrumView::paintStrip(juce::Graphics& g) const
{
    // VIZ-U1: the "Top findings" row - every finding that has no frequency.
    auto row = findingsArea();
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.drawText(TRANS("Top findings"), row.removeFromLeft(kFindingsLabelW), juce::Justification::centredLeft, false);
    const auto cells = chipCells();
    if (cells.empty()) {
        g.drawText(model_.hasSpectrum() ? TRANS("None outside the spectrum") : juce::String("-"), row,
                   juce::Justification::centredLeft, false);
        return;
    }
    for (const auto& cell : cells) {
        auto b = cell.bounds;
        if (cell.hiddenCount > 0) {
            g.setColour(KhensuTheme::bgRaised.withAlpha(0.6f));
            g.fillRoundedRectangle(b, 6.0f);
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::bodyFont(12.0f));
            g.drawText("+" + juce::String(cell.hiddenCount), b, juce::Justification::centred, false);
            continue;
        }
        const auto& f = model_.findings[static_cast<size_t>(cell.finding)];
        const bool sel = f.key() == selected_;
        const bool hov = f.key() == hovered_;
        g.setColour(KhensuTheme::bgRaised.withAlpha(sel ? 0.95f : hov ? 0.8f : 0.6f));
        g.fillRoundedRectangle(b, 6.0f);
        if (sel) {
            g.setColour(KhensuTheme::borderActive);
            g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
        }
        const float remain = juce::jlimit(0.0f, 1.0f, shownRemaining(f.key()));
        g.setColour(colourFor(f).withAlpha(0.3f + 0.7f * remain));
        g.fillEllipse(b.removeFromLeft(22.0f).withSizeKeepingCentre(7.0f, 7.0f));
        g.setColour(f.settled ? KhensuTheme::textSecondary : KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.drawText(f.chipLabel, b.withTrimmedRight(8.0f), juce::Justification::centredLeft, false);
    }
}

void HeroSpectrumView::paintCrosshair(juce::Graphics& g) const
{
    // VIZ-U3: off a finding, the plot reads out the frequency, the track's
    // level and where released tracks sit there.
    if (!hoverInPlot_ || !hovered_.empty() || !model_.hasSpectrum()) return;
    const auto plot = plotArea();
    if (!plot.contains(hoverPos_)) return;
    const float hz = kMinHz * std::pow(kMaxHz / kMinHz, (hoverPos_.x - plot.getX()) / plot.getWidth());
    const float db = band_power::at(model_.spectrum, hz);
    const float y = yForDb(db);
    g.setColour(KhensuTheme::textPrimary.withAlpha(0.35f));
    g.drawVerticalLine(juce::roundToInt(hoverPos_.x), plot.getY(), plot.getBottom());
    g.setColour(KhensuTheme::vizTrack);
    g.fillEllipse(hoverPos_.x - 3.5f, y - 3.5f, 7.0f, 7.0f);

    ui::chart::Axis fx;
    fx.format = ui::chart::Format::Hz;
    ui::chart::Readout r;
    r.title = fx.label(hz, true);
    r.lines.push_back({TRANS("Track") + " " + juce::String(db, 1) + " dB", KhensuTheme::vizTrack});
    if (const auto sp = hero_spectrum::spreadAt(model_, hz)) {
        r.lines.push_back({TRANS("Released median") + " " + juce::String((*sp)[GenreSpread::kP50], 1) + " dB",
                           KhensuTheme::vizMedian});
        r.lines.push_back({TRANS("Middle 80 %") + " " + juce::String((*sp)[GenreSpread::kP10], 1) + " "
                               + TRANS("to") + " " + juce::String((*sp)[GenreSpread::kP90], 1) + " dB",
                           KhensuTheme::vizSpreadInner.withAlpha(1.0f)});
        const float stray = hero_spectrum::strayAt(model_, hz, db);
        if (std::abs(stray) > 0.05f)
            r.lines.push_back({stray > 0.0f ? TRANS("More energy than most released tracks")
                                            : TRANS("Less energy than most released tracks"),
                               juce::Colour(stray > 0.0f ? ui::viz::kDivergingWarm : ui::viz::kDivergingCool)});
    }
    if (model_.reference.size() >= 4)
        r.lines.push_back({model_.referenceName + " " + juce::String(band_power::at(model_.reference, hz), 1) + " dB",
                           KhensuTheme::vizReference});
    ui::chart::ChartView::paintReadout(g, r, {hoverPos_.x, y}, getLocalBounds().toFloat(), desktopChartStyle());
}

void HeroSpectrumView::paintHover(juce::Graphics& g) const
{
    if (hovered_.empty()) return;
    const Finding* f = nullptr;
    for (const auto& x : model_.findings)
        if (x.key() == hovered_) f = &x;
    if (f == nullptr) return;
    const auto font = KhensuTheme::bodyFont(13.0f);
    juce::String text = f->title;
    const float w = std::min(360.0f, juce::GlyphArrangement::getStringWidth(font, text) + 24.0f);
    juce::AttributedString as;
    as.append(text, font, KhensuTheme::textPrimary);
    as.setWordWrap(juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout(as, w - 24.0f);
    const float h = layout.getHeight() + 30.0f;
    auto bubble = juce::Rectangle<float>(hoverPos_.x + 14.0f, hoverPos_.y + 14.0f, w, h);
    const auto bounds = getLocalBounds().toFloat();
    if (bubble.getRight() > bounds.getRight()) bubble.setX(hoverPos_.x - 14.0f - w);
    if (bubble.getBottom() > bounds.getBottom()) bubble.setY(hoverPos_.y - 14.0f - h);
    g.setColour(KhensuTheme::shadowColour);
    g.fillRoundedRectangle(bubble.translated(0.0f, 2.0f), 6.0f);
    g.setColour(KhensuTheme::bgOverlay);
    g.fillRoundedRectangle(bubble, 6.0f);
    g.setColour(KhensuTheme::borderActive);
    g.drawRoundedRectangle(bubble.reduced(0.5f), 6.0f, 1.0f);
    auto inner = bubble.reduced(12.0f, 8.0f);
    g.setColour(colourFor(*f));
    g.setFont(KhensuTheme::labelFont(11.0f));
    g.drawText(f->settled ? TRANS("Settled") : severityWord(f->severity) + " - " + TRANS("click to select"),
               inner.removeFromTop(14.0f), juce::Justification::centredLeft, false);
    layout.draw(g, inner);
}

} // namespace khensu
