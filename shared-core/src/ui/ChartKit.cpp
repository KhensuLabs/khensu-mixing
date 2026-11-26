#include <khensu/ui/ChartKit.h>
#include <khensu/ui/KhensuPalette.h>
#include <algorithm>
#include <cmath>

namespace khensu::ui::chart {

namespace {
double safeLog(double v) noexcept { return std::log10(std::max(v, 1e-9)); }

juce::String trimNumber(double v, int decimals)
{
    if (decimals <= 0) return juce::String(juce::roundToInt(v));
    auto s = juce::String(v, decimals);
    if (s.containsChar('.')) {
        while (s.endsWithChar('0')) s = s.dropLastCharacters(1);
        if (s.endsWithChar('.')) s = s.dropLastCharacters(1);
    }
    return s == "-0" ? juce::String("0") : s;
}

/// The unit an axis carries on its labels ("dB", "Hz", "%"); time is m:ss.
juce::String unitOf(const Axis& a)
{
    switch (a.format) {
    case Format::Hz: return "Hz";
    case Format::Percent: return "%";
    case Format::Time:
    case Format::Category: return {};
    case Format::Number: break;
    }
    return a.unit;
}

/// How many ticks fit along `length` px: a category label needs a text line
/// (vertical) or ~56 px (horizontal); a number a 34 / 70 px gap.
int tickCount(const Axis& a, float length, bool vertical)
{
    const float gap = a.format == Format::Category ? (vertical ? 13.0f : 56.0f) : (vertical ? 34.0f : 70.0f);
    return std::max(2, static_cast<int>(length / gap));
}
} // namespace

double Axis::norm(double v) const noexcept
{
    double t = 0.0;
    if (scale == Scale::Log) {
        const double a = safeLog(lo), b = safeLog(hi);
        t = b > a ? (safeLog(v) - a) / (b - a) : 0.0;
    } else {
        t = hi > lo ? (v - lo) / (hi - lo) : 0.0;
    }
    return inverted ? 1.0 - t : t;
}

double Axis::denorm(double t) const noexcept
{
    if (inverted) t = 1.0 - t;
    if (scale == Scale::Log) {
        const double a = safeLog(lo), b = safeLog(hi);
        return std::pow(10.0, a + t * (b - a));
    }
    return lo + t * (hi - lo);
}

std::vector<double> Axis::ticks(int maxTicks) const
{
    std::vector<double> out;
    if (!(hi > lo) || maxTicks < 2) return out;
    if (format == Format::Category) { // every position, thinned evenly to fit
        const int first = static_cast<int>(std::ceil(lo - 1e-9)), last = static_cast<int>(std::floor(hi + 1e-9));
        const int count = last - first + 1;
        const int stride = std::max(1, (count + maxTicks - 1) / std::max(1, maxTicks));
        for (int i = first; i <= last; i += stride)
            out.push_back(static_cast<double>(i));
        return out;
    }
    if (scale == Scale::Log) {
        // 1-2-5 per decade, thinned to fit.
        std::vector<double> all;
        for (double d = std::pow(10.0, std::floor(safeLog(lo))); d <= hi * 1.0001; d *= 10.0)
            for (const double m : {1.0, 2.0, 5.0})
                if (const double v = d * m; v >= lo * 0.9999 && v <= hi * 1.0001) all.push_back(v);
        if (static_cast<int>(all.size()) <= maxTicks) return all;
        for (const double v : all) { // decades and the 2s / 5s that fit
            const double m = v / std::pow(10.0, std::floor(safeLog(v) + 1e-9));
            if (std::abs(m - 1.0) < 1e-6 || (static_cast<int>(all.size()) <= maxTicks * 2 && std::abs(m - 5.0) < 1e-6))
                out.push_back(v);
        }
        return out;
    }
    const double span = hi - lo;
    double step = std::pow(10.0, std::floor(std::log10(span / maxTicks)));
    if (format == Format::Time && span > 20.0) { // seconds: 5 / 10 / 15 / 30 / 60 ...
        for (const double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0})
            if (span / s <= maxTicks) { step = s; break; }
    } else {
        for (const double m : {1.0, 2.0, 2.5, 5.0, 10.0})
            if (span / (step * m) <= maxTicks) { step *= m; break; }
    }
    for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-6; v += step)
        out.push_back(std::abs(v) < step * 1e-9 ? 0.0 : v);
    return out;
}

juce::String Axis::label(double v, bool withUnit) const
{
    switch (format) {
    case Format::Hz:
        if (v >= 1000.0) {
            const auto k = trimNumber(v / 1000.0, withUnit ? 2 : 1);
            return withUnit ? k + " kHz" : k + "k";
        }
        return trimNumber(v, withUnit && v < 100.0 ? 1 : 0) + (withUnit ? " Hz" : "");
    case Format::Time: {
        const double t = std::max(0.0, v);
        const int m = static_cast<int>(t / 60.0);
        const double s = t - m * 60.0;
        const auto secs = withUnit ? juce::String(s, 1).paddedLeft('0', 4) : juce::String(static_cast<int>(s)).paddedLeft('0', 2);
        return juce::String(m) + ":" + secs;
    }
    case Format::Percent:
        return trimNumber(v * 100.0, withUnit ? decimals : 0) + (withUnit ? " %" : "");
    case Format::Category: {
        const auto i = juce::roundToInt(v);
        return i >= 0 && i < static_cast<int>(categories.size()) ? categories[static_cast<size_t>(i)] : juce::String();
    }
    case Format::Number: break;
    }
    const auto n = trimNumber(v, withUnit ? decimals : std::max(0, decimals - 1));
    return withUnit && unit.isNotEmpty() ? n + " " + unit : n;
}

Style Style::fromPalette()
{
    Style s;
    s.grid = Palette::textPrimary.withAlpha(0.07f);
    s.axis = Palette::textPrimary.withAlpha(0.18f);
    s.text = Palette::textPrimary;
    s.muted = Palette::textMuted;
    s.crosshair = Palette::textPrimary.withAlpha(0.45f);
    s.tooltipBg = Palette::bgBase.withAlpha(0.96f);
    s.tooltipText = Palette::textPrimary;
    s.tooltipBorder = Palette::borderActive;
    s.playhead = Palette::textPrimary.withAlpha(0.85f);
    return s;
}

// ---- Accessibility (VIS-8) --------------------------------------------------------

namespace {
class ChartAccessibilityHandler final : public juce::AccessibilityHandler {
  public:
    ChartAccessibilityHandler(juce::Component& c, std::function<juce::String()> describe)
        : juce::AccessibilityHandler(c, juce::AccessibilityRole::image), describe_(std::move(describe))
    {
    }
    juce::String getDescription() const override
    {
        const auto d = describe_ ? describe_() : juce::String();
        return d.isNotEmpty() ? d : getComponent().getDescription();
    }

  private:
    std::function<juce::String()> describe_;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> makeChartAccessibilityHandler(juce::Component& component,
                                                                          std::function<juce::String()> describe)
{
    return std::make_unique<ChartAccessibilityHandler>(component, std::move(describe));
}

ChartSummary::ChartSummary()
{
    setInterceptsMouseClicks(false, false);
    setWantsKeyboardFocus(false);
}

std::unique_ptr<juce::AccessibilityHandler> ChartSummary::createAccessibilityHandler()
{
    return makeChartAccessibilityHandler(*this, [this] { return describe ? describe() : juce::String(); });
}

juce::String joinWords(const juce::StringArray& items)
{
    if (items.size() <= 1) return items.joinIntoString({});
    juce::StringArray head(items);
    const auto last = head[head.size() - 1];
    head.remove(head.size() - 1);
    return head.joinIntoString(", ") + " and " + last;
}

// ---- ChartView --------------------------------------------------------------------

ChartView::ChartView() : style_(Style::fromPalette()), paletteVersion_(Palette::version()) {}
ChartView::~ChartView() = default;

void ChartView::setStyle(Style s)
{
    style_ = s;
    paletteStyle_ = false;
    repaint();
}

void ChartView::syncPaletteStyle() const
{
    if (!paletteStyle_ || paletteVersion_ == Palette::version()) return;
    style_ = Style::fromPalette();
    paletteVersion_ = Palette::version();
}

void ChartView::setLegend(std::vector<LegendItem> items)
{
    legend_ = std::move(items);
    repaint();
}

void ChartView::setXRange(double lo, double hi)
{
    fullLo_ = lo;
    fullHi_ = hi;
    x.lo = lo;
    x.hi = hi;
    repaint();
}

void ChartView::resetZoom()
{
    x.lo = fullLo_;
    x.hi = fullHi_;
    repaint();
}

bool ChartView::isZoomed() const noexcept
{
    return std::abs(x.lo - fullLo_) > 1e-9 || std::abs(x.hi - fullHi_) > 1e-9;
}

void ChartView::setMargins(int left, int top, int right, int bottom)
{
    ml_ = left;
    mt_ = top;
    mr_ = right;
    mb_ = bottom;
    repaint();
}

juce::Rectangle<float> ChartView::plotArea() const noexcept
{
    auto r = getLocalBounds().toFloat();
    r.removeFromLeft(static_cast<float>(showY_ ? ml_ : 2));
    r.removeFromTop(static_cast<float>(mt_));
    r.removeFromRight(static_cast<float>(mr_));
    r.removeFromBottom(static_cast<float>(showX_ ? mb_ : 2));
    return r.getWidth() > 1.0f && r.getHeight() > 1.0f ? r : juce::Rectangle<float>();
}

float ChartView::px(double v) const noexcept
{
    const auto p = plotArea();
    return p.getX() + static_cast<float>(x.norm(v)) * p.getWidth();
}

float ChartView::py(double v) const noexcept
{
    const auto p = plotArea();
    return p.getBottom() - static_cast<float>(y.norm(v)) * p.getHeight();
}

float ChartView::py2(double v) const noexcept
{
    const auto p = plotArea();
    return p.getBottom() - static_cast<float>(y2.norm(v)) * p.getHeight();
}

double ChartView::xAt(float pixel) const noexcept
{
    const auto p = plotArea();
    return p.getWidth() > 0.0f ? x.denorm((pixel - p.getX()) / p.getWidth()) : x.lo;
}

double ChartView::yAt(float pixel) const noexcept
{
    const auto p = plotArea();
    return p.getHeight() > 0.0f ? y.denorm((p.getBottom() - pixel) / p.getHeight()) : y.lo;
}

void ChartView::setSummary(juce::String text)
{
    if (text == summary_) return;
    summary_ = std::move(text);
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

juce::String ChartView::summary() const { return summary_.isNotEmpty() ? summary_ : autoSummary(); }

std::unique_ptr<juce::AccessibilityHandler> ChartView::createAccessibilityHandler()
{
    return makeChartAccessibilityHandler(*this, [this] { return summary(); });
}

void ChartView::setPlayhead(double xValue)
{
    const double next = xValue >= 0.0 ? xValue : -1.0;
    if (std::abs(next - playhead_) < 1e-9) return;
    const auto plot = plotArea();
    auto strip = [this, &plot](double v) {
        return juce::Rectangle<float>(px(v) - 3.0f, plot.getY(), 6.0f, plot.getHeight()).toNearestInt();
    };
    const bool wasShown = playhead_ >= 0.0 && x.lo <= playhead_ && playhead_ <= x.hi;
    const bool isShown = next >= 0.0 && x.lo <= next && next <= x.hi;
    if (wasShown && isShown && juce::roundToInt(px(next)) == juce::roundToInt(px(playhead_))) {
        playhead_ = next;
        return;
    }
    if (wasShown) repaint(strip(playhead_));
    playhead_ = next;
    if (isShown) repaint(strip(playhead_));
}

void ChartView::setHover(std::optional<std::pair<double, double>> h)
{
    hover_ = h;
    hoverChanged();
    repaint();
}

juce::Path ChartView::seriesPath(const std::vector<double>& xs, const std::vector<double>& ys) const
{
    juce::Path p;
    bool pen = false;
    const size_t n = std::min(xs.size(), ys.size());
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) {
            pen = false;
            continue;
        }
        const float X = px(xs[i]), Y = py(ys[i]);
        if (!pen) p.startNewSubPath(X, Y);
        else p.lineTo(X, Y);
        pen = true;
    }
    return p;
}

juce::Path ChartView::seriesPath(const std::vector<float>& xs, const std::vector<float>& ys) const
{
    return seriesPath(std::vector<double>(xs.begin(), xs.end()), std::vector<double>(ys.begin(), ys.end()));
}

juce::Path ChartView::bandPath(const std::vector<double>& xs, const std::vector<double>& lo,
                               const std::vector<double>& hi) const
{
    juce::Path p;
    const size_t n = std::min({xs.size(), lo.size(), hi.size()});
    std::vector<size_t> ok;
    for (size_t i = 0; i < n; ++i)
        if (std::isfinite(xs[i]) && std::isfinite(lo[i]) && std::isfinite(hi[i])) ok.push_back(i);
    if (ok.size() < 2) return p;
    p.startNewSubPath(px(xs[ok[0]]), py(hi[ok[0]]));
    for (size_t k = 1; k < ok.size(); ++k)
        p.lineTo(px(xs[ok[k]]), py(hi[ok[k]]));
    for (size_t k = ok.size(); k-- > 0;)
        p.lineTo(px(xs[ok[k]]), py(lo[ok[k]]));
    p.closeSubPath();
    return p;
}

juce::Font ChartView::font(float scale) const
{
    return Palette::uiFont(style_.fontSize * scale);
}

void ChartView::paintGrid(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (!showGrid_) return;
    g.setColour(style_.grid);
    if (gridX_)
        for (const double v : x.ticks(tickCount(x, plot.getWidth(), false)))
            g.drawVerticalLine(juce::roundToInt(px(v)), plot.getY(), plot.getBottom());
    if (gridY_)
        for (const double v : y.ticks(tickCount(y, plot.getHeight(), true)))
            g.drawHorizontalLine(juce::roundToInt(py(v)), plot.getX(), plot.getRight());
}

void ChartView::paintAxes(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    g.setFont(font());
    const auto bounds = getLocalBounds().toFloat();
    if (showX_) {
        g.setColour(style_.axis);
        g.drawHorizontalLine(juce::roundToInt(plot.getBottom()), plot.getX(), plot.getRight());
        g.setColour(style_.muted);
        const auto ticks = x.ticks(tickCount(x, plot.getWidth(), false));
        const auto xUnit = unitOf(x);
        for (size_t i = 0; i < ticks.size(); ++i) {
            const double v = ticks[i];
            const float X = px(v);
            // VIS-1: the last label carries the unit ("20 kHz", "0 LUFS").
            const bool last = i + 1 == ticks.size() && xUnit.isNotEmpty();
            const auto text = !last                   ? x.label(v)
                              : x.format == Format::Hz ? x.label(v, true)
                                                       : x.label(v) + (x.format == Format::Percent ? "" : " ") + xUnit;
            const float w = last ? juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 6.0f : 60.0f;
            auto r = juce::Rectangle<float>(X - w * 0.5f, plot.getBottom() + 3.0f, w, 14.0f);
            if (r.getRight() > bounds.getRight()) r.setX(bounds.getRight() - w);
            g.drawText(text, r, juce::Justification::centredTop, false);
        }
    }
    auto paintY = [&](const Axis& a, bool right) {
        g.setColour(style_.muted);
        const auto ticks = a.ticks(tickCount(a, plot.getHeight(), true));
        const auto unit = unitOf(a);
        auto yOf = [&](double v) { return right ? py2(v) : py(v); };
        // VIS-1: the unit beside the top tick ("0 dB"), or above the axis
        // when the margin is too narrow for it.
        size_t top = ticks.size();
        for (size_t i = 0; i < ticks.size(); ++i)
            if (top == ticks.size() || yOf(ticks[i]) < yOf(ticks[top])) top = i;
        const float room = right ? bounds.getRight() - plot.getRight() - 5.0f : plot.getX() - 5.0f;
        bool unitDrawn = unit.isEmpty();
        for (size_t i = 0; i < ticks.size(); ++i) {
            const float Y = yOf(ticks[i]);
            auto text = a.label(ticks[i]);
            if (i == top && !unitDrawn) {
                const auto withUnit = text + (a.format == Format::Percent ? "" : " ") + unit;
                if (juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), withUnit) <= room) {
                    text = withUnit;
                    unitDrawn = true;
                }
            }
            const auto r = right ? juce::Rectangle<float>(plot.getRight() + 5.0f, Y - 7.0f, room, 14.0f)
                                 : juce::Rectangle<float>(0.0f, Y - 7.0f, room, 14.0f);
            g.drawText(text, r, right ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
        }
        if (!unitDrawn) {
            const float top0 = std::max(0.0f, plot.getY() - 13.0f);
            const auto r = right ? juce::Rectangle<float>(plot.getRight() - 40.0f, top0, bounds.getRight() - plot.getRight() + 40.0f, 12.0f)
                                 : juce::Rectangle<float>(0.0f, top0, plot.getX() + 40.0f, 12.0f);
            g.drawText(unit, r, right ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
        }
    };
    if (showY_) paintY(y, false);
    if (showY2_) paintY(y2, true);
}

void ChartView::paintLegend(juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (legend_.empty()) return;
    g.setFont(font());
    float xr = plot.getRight() - 4.0f;
    const float yTop = plot.getY() + 4.0f;
    for (auto it = legend_.rbegin(); it != legend_.rend(); ++it) {
        const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), it->name);
        const float w = tw + 22.0f;
        juce::Rectangle<float> cell(xr - w, yTop, w, 14.0f);
        auto swatch = cell.removeFromLeft(16.0f).reduced(1.0f, 0.0f);
        g.setColour(it->colour);
        switch (it->kind) {
        case LegendItem::Kind::Line: g.fillRect(swatch.withSizeKeepingCentre(swatch.getWidth(), 2.0f)); break;
        case LegendItem::Kind::Dashed:
            for (float sx = swatch.getX(); sx < swatch.getRight(); sx += 5.0f)
                g.fillRect(juce::Rectangle<float>(sx, swatch.getCentreY() - 1.0f, 3.0f, 2.0f));
            break;
        case LegendItem::Kind::Band: g.setColour(it->colour.withMultipliedAlpha(0.45f)); g.fillRect(swatch.reduced(0.0f, 3.0f)); break;
        case LegendItem::Kind::Dot: g.fillEllipse(swatch.withSizeKeepingCentre(7.0f, 7.0f)); break;
        }
        g.setColour(style_.muted);
        g.drawText(it->name, cell.withTrimmedLeft(3.0f), juce::Justification::centredLeft, false);
        xr -= w + 10.0f;
    }
}

Readout ChartView::readoutAt(double xValue, double yValue)
{
    Readout r;
    r.title = x.label(xValue, true);
    r.lines.push_back({y.label(yValue, true), style_.text});
    return r;
}

void ChartView::paintReadout(juce::Graphics& g, const Readout& r, juce::Point<float> anchor,
                             juce::Rectangle<float> bounds, const Style& s)
{
    if (r.title.isEmpty() && r.lines.empty()) return;
    const auto f = Palette::uiFont(s.fontSize);
    float w = juce::GlyphArrangement::getStringWidth(f.boldened(), r.title);
    for (const auto& [text, colour] : r.lines)
        w = std::max(w, juce::GlyphArrangement::getStringWidth(f, text) + 12.0f);
    w += 14.0f;
    const float h = 8.0f + 15.0f * static_cast<float>((r.title.isNotEmpty() ? 1 : 0) + r.lines.size());
    juce::Rectangle<float> box(anchor.x + 12.0f, anchor.y - h - 8.0f, w, h);
    if (box.getRight() > bounds.getRight()) box.setX(anchor.x - 12.0f - w);
    if (box.getY() < bounds.getY()) box.setY(anchor.y + 12.0f);
    box = box.constrainedWithin(bounds);
    g.setColour(s.tooltipBg);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(s.tooltipBorder);
    g.drawRoundedRectangle(box, 4.0f, 1.0f);
    auto row = box.reduced(7.0f, 4.0f);
    if (r.title.isNotEmpty()) {
        g.setColour(s.tooltipText);
        g.setFont(f.boldened());
        g.drawText(r.title, row.removeFromTop(15.0f), juce::Justification::centredLeft, false);
    }
    g.setFont(f);
    for (const auto& [text, colour] : r.lines) {
        auto line = row.removeFromTop(15.0f);
        g.setColour(colour);
        g.fillEllipse(line.removeFromLeft(12.0f).withSizeKeepingCentre(6.0f, 6.0f));
        g.setColour(s.tooltipText);
        g.drawText(text, line, juce::Justification::centredLeft, false);
    }
}

void ChartView::paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float X, float Y)
{
    g.setColour(style_.crosshair);
    g.drawVerticalLine(juce::roundToInt(X), plot.getY(), plot.getBottom());
    g.setColour(style_.crosshair.withMultipliedAlpha(0.4f));
    g.drawHorizontalLine(juce::roundToInt(Y), plot.getX(), plot.getRight());
}

void ChartView::paintCrosshair(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (!hover_) return;
    const float X = px(hover_->first), Y = py(hover_->second);
    if (!plot.expanded(1.0f).contains(X, Y)) return;
    paintHoverMark(g, plot, X, Y);
    paintReadout(g, readoutAt(hover_->first, hover_->second), {X, Y}, getLocalBounds().toFloat(), style_);
}

void ChartView::paint(juce::Graphics& g)
{
    syncPaletteStyle();
    const auto plot = plotArea();
    if (plot.isEmpty()) return;
    paintGrid(g, plot);
    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(plot.toNearestIntEdges().expanded(1));
        paintData(g, plot);
    }
    paintAxes(g, plot);
    paintOverlay(g, plot);
    if (playhead_ >= 0.0 && playhead_ >= x.lo && playhead_ <= x.hi) {
        g.setColour(style_.playhead);
        g.fillRect(juce::Rectangle<float>(px(playhead_) - 0.75f, plot.getY(), 1.5f, plot.getHeight()));
    }
    paintLegend(g, plot);
    paintCrosshair(g, plot);
}

void ChartView::mouseMove(const juce::MouseEvent& e)
{
    const auto plot = plotArea();
    if (plot.contains(e.position)) setHover(std::make_pair(xAt(e.position.x), yAt(e.position.y)));
    else if (hover_) setHover(std::nullopt);
}

void ChartView::mouseExit(const juce::MouseEvent&)
{
    if (hover_) setHover(std::nullopt);
}

void ChartView::mouseDown(const juce::MouseEvent& e)
{
    downPos_ = e.position;
    dragLo_ = x.lo;
    dragHi_ = x.hi;
    dragged_ = false;
}

void ChartView::mouseDrag(const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 3) dragged_ = true;
    if (!zoomable_ || !isZoomed() || !dragged_) return;
    const auto plot = plotArea();
    if (plot.getWidth() <= 0.0f) return;
    const double dt = -(e.position.x - downPos_.x) / plot.getWidth();
    Axis a = x;
    a.lo = dragLo_;
    a.hi = dragHi_;
    double lo = a.denorm(dt), hi = a.denorm(1.0 + dt);
    if (lo < fullLo_) { hi = a.scale == Scale::Log ? hi * fullLo_ / lo : hi + (fullLo_ - lo); lo = fullLo_; }
    if (hi > fullHi_) { lo = a.scale == Scale::Log ? lo * fullHi_ / hi : lo - (hi - fullHi_); hi = fullHi_; }
    x.lo = std::max(lo, fullLo_);
    x.hi = std::min(hi, fullHi_);
    mouseMove(e);
    repaint();
}

void ChartView::mouseUp(const juce::MouseEvent& e)
{
    if (dragged_ || !plotArea().contains(e.position)) return;
    if (onClick) onClick(xAt(e.position.x));
    if (onClickAt) onClickAt(xAt(e.position.x), yAt(e.position.y));
}

void ChartView::mouseDoubleClick(const juce::MouseEvent&)
{
    if (zoomable_) resetZoom();
}

void ChartView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (!zoomable_ || w.deltaY == 0.0f) {
        Component::mouseWheelMove(e, w);
        return;
    }
    const auto plot = plotArea();
    if (!plot.contains(e.position)) return;
    const double t = (e.position.x - plot.getX()) / plot.getWidth();
    const double factor = w.deltaY > 0 ? 0.8 : 1.25;
    Axis full = x;
    full.lo = fullLo_;
    full.hi = fullHi_;
    const double nLo = full.norm(x.lo), nHi = full.norm(x.hi);
    const double c = nLo + t * (nHi - nLo);
    double span = std::clamp((nHi - nLo) * factor, 0.02, 1.0);
    double lo = c - t * span, hi = lo + span;
    if (lo < 0.0) { hi -= lo; lo = 0.0; }
    if (hi > 1.0) { lo -= hi - 1.0; hi = 1.0; }
    x.lo = full.denorm(std::max(0.0, lo));
    x.hi = full.denorm(std::min(1.0, hi));
    mouseMove(e);
    repaint();
}

} // namespace khensu::ui::chart
