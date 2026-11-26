#include <khensu/ui/ChartKitPlots.h>
#include <khensu/ui/KhensuPalette.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace khensu::ui::chart {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

juce::Path dashedOf(const juce::Path& p, float width, float on, float off)
{
    juce::Path dashed;
    const float dashes[] = {on, off};
    juce::PathStrokeType(width).createDashedStroke(dashed, p, dashes, 2);
    return dashed; // a filled outline - fill it, never stroke it
}

/// "min to max unit" of a series' finite values, written with the axis.
juce::String rangeText(const std::vector<double>& ys, const Axis& a)
{
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    for (const double v : ys)
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    if (lo > hi) return "no data";
    if (std::abs(hi - lo) < 1e-9) return a.label(lo, true);
    return "from " + a.label(lo, false) + " to " + a.label(hi, true);
}
} // namespace

// ---- LineChart --------------------------------------------------------------------

void LineChart::setSeries(std::vector<Series> series)
{
    series_ = std::move(series);
    rebuildLegend();
    repaint();
}

void LineChart::setBands(std::vector<Band> bands)
{
    bands_ = std::move(bands);
    rebuildLegend();
    repaint();
}

void LineChart::setRefLines(std::vector<RefLine> lines)
{
    lines_ = std::move(lines);
    repaint();
}

void LineChart::setSpans(std::vector<Span> spans)
{
    spans_ = std::move(spans);
    repaint();
}

void LineChart::clear()
{
    series_.clear();
    bands_.clear();
    lines_.clear();
    spans_.clear();
    rebuildLegend();
    repaint();
}

void LineChart::setAutoLegend(bool on)
{
    autoLegend_ = on;
    rebuildLegend();
}

void LineChart::rebuildLegend()
{
    if (!autoLegend_) return;
    std::vector<LegendItem> items;
    for (const auto& b : bands_)
        if (b.name.isNotEmpty()) items.push_back({b.name, b.colour.withAlpha(1.0f), LegendItem::Kind::Band});
    for (const auto& s : series_) {
        if (!s.inLegend || s.name.isEmpty()) continue;
        const auto kind = s.kind == Series::Kind::Dashed ? LegendItem::Kind::Dashed
                          : s.kind == Series::Kind::Dots ? LegendItem::Kind::Dot
                          : s.kind == Series::Kind::Area ? LegendItem::Kind::Band
                                                         : LegendItem::Kind::Line;
        items.push_back({s.name, s.colour, kind});
    }
    // One series needs no key: the chart's title names it.
    if (items.size() == 1 && bands_.empty()) items.clear();
    setLegend(std::move(items));
}

std::optional<double> LineChart::valueAt(const Series& s, double xv)
{
    const size_t n = std::min(s.xs.size(), s.ys.size());
    if (n == 0) return std::nullopt;
    if (s.kind == Series::Kind::Dots) {
        size_t best = n;
        double bestD = std::numeric_limits<double>::max();
        for (size_t i = 0; i < n; ++i)
            if (std::isfinite(s.ys[i]) && std::abs(s.xs[i] - xv) < bestD) {
                bestD = std::abs(s.xs[i] - xv);
                best = i;
            }
        return best < n ? std::optional<double>(s.ys[best]) : std::nullopt;
    }
    if (xv < s.xs.front() || xv > s.xs[n - 1]) {
        if (s.kind != Series::Kind::Steps || xv < s.xs.front()) return std::nullopt;
    }
    const auto it = std::upper_bound(s.xs.begin(), s.xs.begin() + static_cast<std::ptrdiff_t>(n), xv);
    const size_t hi = static_cast<size_t>(std::distance(s.xs.begin(), it));
    const size_t lo = hi == 0 ? 0 : hi - 1;
    if (s.kind == Series::Kind::Steps || hi >= n) {
        return std::isfinite(s.ys[lo]) ? std::optional<double>(s.ys[lo]) : std::nullopt;
    }
    const double a = s.ys[lo], b = s.ys[hi];
    if (!std::isfinite(a) || !std::isfinite(b)) return std::nullopt;
    const double span = s.xs[hi] - s.xs[lo];
    const double t = span > 0.0 ? (xv - s.xs[lo]) / span : 0.0;
    return a + (b - a) * t;
}

void LineChart::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    for (const auto& sp : spans_) {
        g.setColour(sp.colour);
        if (sp.vertical) {
            const float x0 = px(sp.lo), x1 = px(sp.hi);
            g.fillRect(juce::Rectangle<float>(std::min(x0, x1), plot.getY(), std::max(1.0f, std::abs(x1 - x0)),
                                              plot.getHeight()));
        } else {
            const float y0 = py(sp.lo), y1 = py(sp.hi);
            g.fillRect(juce::Rectangle<float>(plot.getX(), std::min(y0, y1), plot.getWidth(),
                                              std::max(1.0f, std::abs(y1 - y0))));
        }
    }
    for (const auto& b : bands_) {
        g.setColour(b.colour);
        g.fillPath(bandPath(b.xs, b.lo, b.hi));
    }
    for (const auto& l : lines_) {
        juce::Path p;
        if (l.vertical) {
            p.startNewSubPath(px(l.value), plot.getY());
            p.lineTo(px(l.value), plot.getBottom());
        } else {
            p.startNewSubPath(plot.getX(), py(l.value));
            p.lineTo(plot.getRight(), py(l.value));
        }
        g.setColour(l.colour);
        if (l.dashed)
            g.fillPath(dashedOf(p, 1.2f, 4.0f, 3.0f));
        else
            g.strokePath(p, juce::PathStrokeType(1.2f));
    }
    paintUnder(g, plot);
    for (const auto& s : series_) {
        const size_t n = std::min(s.xs.size(), s.ys.size());
        g.setColour(s.colour);
        if (s.kind == Series::Kind::Dots) {
            const float r = std::max(2.0f, s.width + 1.0f);
            for (size_t i = 0; i < n; ++i)
                if (std::isfinite(s.ys[i])) g.fillEllipse(px(s.xs[i]) - r, yFor(s, s.ys[i]) - r, 2.0f * r, 2.0f * r);
            continue;
        }
        juce::Path p;
        bool pen = false;
        float lastY = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(s.xs[i]) || !std::isfinite(s.ys[i])) {
                pen = false;
                continue;
            }
            const float X = px(s.xs[i]), Y = yFor(s, s.ys[i]);
            if (!pen)
                p.startNewSubPath(X, Y);
            else if (s.kind == Series::Kind::Steps) {
                p.lineTo(X, lastY);
                p.lineTo(X, Y);
            } else
                p.lineTo(X, Y);
            lastY = Y;
            pen = true;
        }
        if (s.kind == Series::Kind::Steps && pen && n > 0) p.lineTo(plot.getRight(), lastY);
        switch (s.kind) {
        case Series::Kind::Dashed:
            g.fillPath(dashedOf(p, s.width, 5.0f, 4.0f));
            break;
        case Series::Kind::Area: {
            juce::Path fill;
            bool open = false;
            float firstX = 0.0f, lastX = 0.0f;
            const float base = plot.getBottom();
            for (size_t i = 0; i < n; ++i) {
                if (!std::isfinite(s.xs[i]) || !std::isfinite(s.ys[i])) {
                    if (open) {
                        fill.lineTo(lastX, base);
                        fill.lineTo(firstX, base);
                        fill.closeSubPath();
                    }
                    open = false;
                    continue;
                }
                const float X = px(s.xs[i]), Y = yFor(s, s.ys[i]);
                if (!open) {
                    fill.startNewSubPath(X, Y);
                    firstX = X;
                    open = true;
                } else
                    fill.lineTo(X, Y);
                lastX = X;
            }
            if (open) {
                fill.lineTo(lastX, base);
                fill.lineTo(firstX, base);
                fill.closeSubPath();
            }
            g.setColour(s.colour.withMultipliedAlpha(0.22f));
            g.fillPath(fill);
            g.setColour(s.colour);
            g.strokePath(p, juce::PathStrokeType(s.width, juce::PathStrokeType::curved));
            break;
        }
        case Series::Kind::Line:
        case Series::Kind::Steps:
        case Series::Kind::Dots:
            g.strokePath(p, juce::PathStrokeType(s.width, juce::PathStrokeType::curved));
            break;
        }
    }
    paintOver(g, plot);
    // Span and reference labels, on top.
    g.setFont(font(0.92f));
    for (const auto& sp : spans_)
        if (sp.label.isNotEmpty() && sp.vertical) {
            const float x0 = std::min(px(sp.lo), px(sp.hi)), x1 = std::max(px(sp.lo), px(sp.hi));
            if (x1 - x0 < 24.0f) continue;
            g.setColour(style().text.withAlpha(0.85f));
            g.drawText(sp.label, juce::Rectangle<float>(x0 + 3.0f, plot.getY() + 2.0f, x1 - x0 - 6.0f, 13.0f),
                       juce::Justification::centredLeft, true);
        }
    for (const auto& l : lines_)
        if (l.label.isNotEmpty()) {
            g.setColour(l.colour.withAlpha(1.0f));
            if (l.vertical)
                g.drawText(l.label, juce::Rectangle<float>(px(l.value) + 3.0f, plot.getY() + 2.0f, 120.0f, 13.0f),
                           juce::Justification::centredLeft, false);
            else
                g.drawText(
                    l.label,
                    juce::Rectangle<float>(plot.getX() + 4.0f, py(l.value) - 14.0f, plot.getWidth() * 0.6f, 13.0f),
                    juce::Justification::centredLeft, false);
        }
}

Readout LineChart::readoutAt(double xv, double)
{
    Readout r;
    r.title = x.label(xv, true);
    for (const auto& sp : spans_)
        if (sp.vertical && sp.label.isNotEmpty() && xv >= std::min(sp.lo, sp.hi) && xv <= std::max(sp.lo, sp.hi))
            r.lines.push_back({sp.label, sp.colour.withAlpha(1.0f)});
    for (const auto& s : series_) {
        if (!s.inReadout) continue;
        const auto v = valueAt(s, xv);
        const auto& axis = s.secondary ? y2 : y;
        const auto text = v ? axis.label(*v, true) : juce::String("-");
        r.lines.push_back({s.name.isNotEmpty() ? s.name + " " + text : text, s.colour});
    }
    for (const auto& b : bands_) {
        if (b.name.isEmpty()) continue;
        Series lo{b.name, b.colour, b.xs, b.lo};
        Series hi{b.name, b.colour, b.xs, b.hi};
        const auto a = valueAt(lo, xv), c = valueAt(hi, xv);
        if (a && c)
            r.lines.push_back({b.name + " " + y.label(*a) + " to " + y.label(*c, true), b.colour.withAlpha(1.0f)});
    }
    extendReadout(r, xv);
    return r;
}

juce::String LineChart::autoSummary() const
{
    juce::StringArray parts;
    for (const auto& s : series_) {
        if (!s.inReadout) continue;
        const auto& axis = s.secondary ? y2 : y;
        parts.add((s.name.isNotEmpty() ? s.name + " " : juce::String()) + rangeText(s.ys, axis));
    }
    for (const auto& sp : spans_)
        if (sp.label.isNotEmpty()) parts.add(sp.label);
    if (parts.isEmpty()) return getTitle().isNotEmpty() ? getTitle() + ": no data" : juce::String();
    return (getTitle().isNotEmpty() ? getTitle() + ": " : juce::String()) + parts.joinIntoString("; ");
}

// ---- BarChart ---------------------------------------------------------------------

BarChart::BarChart()
{
    onClickAt = [this](double xv, double yv) {
        if (!onBarClicked) return;
        const int b = barAt(xv, yv);
        if (b >= 0) onBarClicked(b);
    };
    layoutAxes();
}

void BarChart::setHorizontal(bool horizontal)
{
    if (horizontal == horizontal_) return;
    std::swap(x, y);
    horizontal_ = horizontal;
    layoutAxes();
    repaint();
}

void BarChart::setBaseline(std::optional<double> v)
{
    baseline_ = v;
    repaint();
}

void BarChart::setBars(std::vector<Bar> bars, std::vector<BarSeries> series)
{
    bars_ = std::move(bars);
    series_ = std::move(series);
    if (series_.empty()) series_.push_back({});
    std::vector<LegendItem> legend;
    if (series_.size() > 1)
        for (const auto& s : series_)
            legend.push_back({s.name, s.colour, LegendItem::Kind::Band});
    setLegend(std::move(legend));
    layoutAxes();
    repaint();
}

void BarChart::layoutAxes()
{
    auto& cat = horizontal_ ? y : x;
    cat.format = Format::Category;
    cat.scale = Scale::Linear;
    cat.categories.clear();
    for (const auto& b : bars_)
        cat.categories.push_back(b.label);
    cat.lo = -0.5;
    cat.hi = static_cast<double>(std::max<size_t>(1, bars_.size())) - 0.5;
    cat.inverted = horizontal_; // listed top-down
    auto& val = horizontal_ ? x : y;
    val.inverted = false;
    if (val.format == Format::Category) val.format = Format::Number;
    const auto fixed =
        fixedMargins_.value_or(horizontal_ ? juce::Rectangle<int>(36, 6, 44, 20) : juce::Rectangle<int>(44, 8, 10, 20));
    if (horizontal_) {
        float w = 0.0f;
        const auto f = font();
        for (const auto& b : bars_)
            w = std::max(w, juce::GlyphArrangement::getStringWidth(f, b.label));
        setMargins(juce::jlimit(fixed.getX(), 150, juce::roundToInt(w) + 12), fixed.getY(), fixed.getWidth(),
                   fixed.getHeight());
        setGridLines(true, false);
    } else {
        setMargins(fixed.getX(), fixed.getY(), fixed.getWidth(), fixed.getHeight());
        setGridLines(false, true);
    }
}

void BarChart::setFixedMargins(int left, int top, int right, int bottom)
{
    fixedMargins_ = juce::Rectangle<int>(left, top, right, bottom);
    layoutAxes();
    repaint();
}

int BarChart::barAt(double xv, double yv) const
{
    const double c = horizontal_ ? yv : xv;
    const int i = juce::roundToInt(c);
    return i >= 0 && i < static_cast<int>(bars_.size()) && std::abs(c - i) <= 0.45 ? i : -1;
}

juce::String BarChart::textFor(size_t bar, size_t s, double v) const
{
    if (valueText) return valueText(bar, s, v);
    return valueAxis().label(v, true);
}

void BarChart::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    juce::ignoreUnused(plot);
    const auto& val = valueAxis();
    const double base = baseline_.value_or(val.lo);
    const size_t k = std::max<size_t>(1, series_.size());
    const double slot = 0.72 / static_cast<double>(k);
    g.setFont(font(0.92f));
    for (size_t i = 0; i < bars_.size(); ++i) {
        const auto& bar = bars_[i];
        for (size_t s = 0; s < k; ++s) {
            const double v = s < bar.values.size() ? bar.values[s] : kNaN;
            const double c0 = static_cast<double>(i) - 0.36 + slot * static_cast<double>(s);
            const double c1 = c0 + slot;
            juce::Rectangle<float> r;
            const bool has = std::isfinite(v);
            const double end = has ? v : base;
            if (horizontal_) {
                const float a = px(base), b = px(end), t0 = py(c0), t1 = py(c1);
                r = juce::Rectangle<float>::leftTopRightBottom(std::min(a, b), std::min(t0, t1), std::max(a, b),
                                                               std::max(t0, t1));
            } else {
                const float a = py(base), b = py(end), t0 = px(c0), t1 = px(c1);
                r = juce::Rectangle<float>::leftTopRightBottom(std::min(t0, t1), std::min(a, b), std::max(t0, t1),
                                                               std::max(a, b));
            }
            const auto colour = (bar.colour.isOpaque() || bar.colour.getAlpha() > 0 ? bar.colour : series_[s].colour)
                                    .withMultipliedAlpha(bar.alpha);
            if (has) {
                g.setColour(colour);
                g.fillRoundedRectangle(r, 2.0f);
            }
            if (!showValues_) continue;
            const auto text = bar.note.isNotEmpty() ? bar.note : has ? textFor(i, s, v) : juce::String("-");
            g.setColour(has && bar.note.isEmpty() ? style().text : style().muted);
            const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 6.0f;
            if (horizontal_) {
                const bool positive = end >= base;
                auto tr = positive ? juce::Rectangle<float>(r.getRight() + 3.0f, r.getY(), tw, r.getHeight())
                                   : juce::Rectangle<float>(r.getX() - tw - 3.0f, r.getY(), tw, r.getHeight());
                const auto bounds = getLocalBounds().toFloat();
                if (tr.getRight() > bounds.getRight()) tr.setX(r.getRight() - tw - 3.0f);
                if (tr.getHeight() < 11.0f) tr = tr.withSizeKeepingCentre(tr.getWidth(), 11.0f);
                g.drawText(text, tr, positive ? juce::Justification::centredLeft : juce::Justification::centredRight,
                           false);
            } else {
                auto tr = juce::Rectangle<float>(r.getCentreX() - tw * 0.5f, r.getY() - 14.0f, tw, 13.0f);
                if (tr.getY() < plot.getY()) tr.setY(r.getY() + 2.0f);
                g.drawText(text, tr, juce::Justification::centred, false);
            }
        }
        if (bar.target) { // the target: a tick across the bar's slot
            const double c0 = static_cast<double>(i) - 0.42, c1 = static_cast<double>(i) + 0.42;
            g.setColour(style().text.withAlpha(0.9f));
            if (horizontal_)
                g.fillRect(juce::Rectangle<float>(px(*bar.target) - 1.0f, std::min(py(c0), py(c1)), 2.0f,
                                                  std::abs(py(c1) - py(c0))));
            else
                g.fillRect(juce::Rectangle<float>(std::min(px(c0), px(c1)), py(*bar.target) - 1.0f,
                                                  std::abs(px(c1) - px(c0)), 2.0f));
        }
    }
}

void BarChart::paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float, float)
{
    if (!hover()) return;
    const int i = barAt(hover()->first, hover()->second);
    if (i < 0) return;
    const double c0 = i - 0.5, c1 = i + 0.5;
    g.setColour(style().crosshair.withMultipliedAlpha(0.25f));
    if (horizontal_)
        g.fillRect(
            juce::Rectangle<float>(plot.getX(), std::min(py(c0), py(c1)), plot.getWidth(), std::abs(py(c1) - py(c0))));
    else
        g.fillRect(
            juce::Rectangle<float>(std::min(px(c0), px(c1)), plot.getY(), std::abs(px(c1) - px(c0)), plot.getHeight()));
}

Readout BarChart::readoutAt(double xv, double yv)
{
    Readout r;
    const int i = barAt(xv, yv);
    if (i < 0) return r;
    const auto& bar = bars_[static_cast<size_t>(i)];
    r.title = bar.label;
    for (size_t s = 0; s < series_.size(); ++s) {
        const double v = s < bar.values.size() ? bar.values[s] : kNaN;
        auto text = bar.note.isNotEmpty() ? bar.note
                    : std::isfinite(v)    ? textFor(static_cast<size_t>(i), s, v)
                                          : juce::String("-");
        if (series_[s].name.isNotEmpty()) text = series_[s].name + " " + text;
        const auto colour = bar.colour.getAlpha() > 0 ? bar.colour : series_[s].colour;
        r.lines.push_back({text, colour.withAlpha(1.0f)});
    }
    if (bar.target) r.lines.push_back({"Target " + valueAxis().label(*bar.target, true), style().text});
    return r;
}

juce::String BarChart::autoSummary() const
{
    juce::StringArray parts;
    for (size_t i = 0; i < bars_.size(); ++i) {
        const auto& bar = bars_[i];
        juce::StringArray vals;
        for (size_t s = 0; s < series_.size(); ++s) {
            const double v = s < bar.values.size() ? bar.values[s] : kNaN;
            auto t = bar.note.isNotEmpty() ? bar.note
                     : std::isfinite(v)    ? textFor(i, s, v)
                                           : juce::String("not measured");
            if (series_.size() > 1 && series_[s].name.isNotEmpty()) t = series_[s].name + " " + t;
            vals.add(t);
        }
        auto line = bar.label + " " + vals.joinIntoString(", ");
        if (bar.target) line << " (target " << valueAxis().label(*bar.target, true) << ")";
        parts.add(line);
    }
    if (parts.isEmpty()) return getTitle().isNotEmpty() ? getTitle() + ": no data" : juce::String();
    return (getTitle().isNotEmpty() ? getTitle() + ": " : juce::String()) + parts.joinIntoString("; ");
}

// ---- StackedAreaChart -------------------------------------------------------------

void StackedAreaChart::setLayers(std::vector<double> xs, std::vector<Layer> layers, bool normalise)
{
    xs_ = std::move(xs);
    layers_ = std::move(layers);
    normalise_ = normalise;
    std::vector<LegendItem> legend;
    for (const auto& l : layers_)
        legend.push_back({l.name, l.colour, LegendItem::Kind::Band});
    setLegend(std::move(legend));
    if (normalise_) {
        y.lo = 0.0;
        y.hi = 1.0;
        y.format = Format::Percent;
    } else {
        double top = 0.0;
        for (size_t i = 0; i < xs_.size(); ++i) {
            double sum = 0.0;
            for (size_t l = 0; l < layers_.size(); ++l)
                sum += valueOf(l, i);
            top = std::max(top, sum);
        }
        y.lo = 0.0;
        y.hi = top > 0.0 ? top * 1.05 : 1.0;
    }
    repaint();
}

double StackedAreaChart::valueOf(size_t layer, size_t i) const
{
    const auto& ys = layers_[layer].ys;
    const double v = i < ys.size() && std::isfinite(ys[i]) ? std::max(0.0, ys[i]) : 0.0;
    if (!normalise_) return v;
    double sum = 0.0;
    for (const auto& l : layers_)
        sum += i < l.ys.size() && std::isfinite(l.ys[i]) ? std::max(0.0, l.ys[i]) : 0.0;
    return sum > 0.0 ? v / sum : 0.0;
}

size_t StackedAreaChart::indexAt(double xv) const
{
    if (xs_.empty()) return 0;
    size_t best = 0;
    for (size_t i = 1; i < xs_.size(); ++i)
        if (std::abs(xs_[i] - xv) < std::abs(xs_[best] - xv)) best = i;
    return best;
}

int StackedAreaChart::layerAt(double xv, double yv) const
{
    if (xs_.empty()) return -1;
    const size_t i = indexAt(xv);
    double acc = 0.0;
    for (size_t l = 0; l < layers_.size(); ++l) {
        acc += valueOf(l, i);
        if (yv <= acc) return static_cast<int>(l);
    }
    return -1;
}

void StackedAreaChart::paintData(juce::Graphics& g, juce::Rectangle<float>)
{
    if (xs_.size() < 2) return;
    std::vector<double> lower(xs_.size(), 0.0);
    for (size_t l = 0; l < layers_.size(); ++l) {
        std::vector<double> upper(lower);
        for (size_t i = 0; i < xs_.size(); ++i)
            upper[i] += valueOf(l, i);
        g.setColour(layers_[l].colour.withMultipliedAlpha(0.78f));
        g.fillPath(bandPath(xs_, lower, upper));
        g.setColour(layers_[l].colour);
        g.strokePath(seriesPath(xs_, upper), juce::PathStrokeType(1.0f));
        lower = std::move(upper);
    }
}

Readout StackedAreaChart::readoutAt(double xv, double yv)
{
    Readout r;
    if (xs_.empty()) return r;
    const size_t i = indexAt(xv);
    r.title = x.label(xs_[i], true);
    const int under = layerAt(xv, yv);
    for (size_t l = 0; l < layers_.size(); ++l) {
        auto text = layers_[l].name + " " + y.label(valueOf(l, i), true);
        if (static_cast<int>(l) == under) text = "> " + text;
        r.lines.push_back({text, layers_[l].colour});
    }
    return r;
}

juce::String StackedAreaChart::autoSummary() const
{
    juce::StringArray parts;
    for (size_t l = 0; l < layers_.size() && !xs_.empty(); ++l) {
        double sum = 0.0;
        for (size_t i = 0; i < xs_.size(); ++i)
            sum += valueOf(l, i);
        parts.add(layers_[l].name + " averages " + y.label(sum / static_cast<double>(xs_.size()), true));
    }
    if (parts.isEmpty()) return getTitle().isNotEmpty() ? getTitle() + ": no data" : juce::String();
    return (getTitle().isNotEmpty() ? getTitle() + ": " : juce::String()) + parts.joinIntoString("; ");
}

// ---- RadarChart -------------------------------------------------------------------

RadarChart::RadarChart()
    : style_(Style::fromPalette()), paletteVersion_(Palette::version()), seriesColour_(Palette::accentTeal),
      referenceColour_(Palette::textMuted)
{
    setOpaque(false);
}

void RadarChart::setStyle(Style s)
{
    style_ = s;
    paletteStyle_ = false;
    repaint();
}

void RadarChart::syncPaletteStyle() const
{
    if (paletteVersion_ == Palette::version()) return;
    paletteVersion_ = Palette::version();
    if (paletteStyle_) style_ = Style::fromPalette();
    if (paletteColours_) {
        seriesColour_ = Palette::accentTeal;
        referenceColour_ = Palette::textMuted;
    }
}

void RadarChart::setSpokes(std::vector<Spoke> spokes)
{
    spokes_ = std::move(spokes);
    hovered_ = -1;
    repaint();
}

void RadarChart::setReference(std::vector<double> values, juce::String name)
{
    reference_ = std::move(values);
    referenceName_ = std::move(name);
    repaint();
}

void RadarChart::setColours(juce::Colour series, juce::Colour reference)
{
    seriesColour_ = series;
    referenceColour_ = reference;
    paletteColours_ = false;
    repaint();
}

void RadarChart::setSeriesName(juce::String name) { seriesName_ = std::move(name); }
void RadarChart::setEmptyText(juce::String text) { emptyText_ = std::move(text); }

void RadarChart::setSummary(juce::String text)
{
    summary_ = std::move(text);
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

juce::String RadarChart::summary() const
{
    if (summary_.isNotEmpty()) return summary_;
    juce::StringArray parts;
    for (size_t i = 0; i < spokes_.size(); ++i) {
        const auto& s = spokes_[i];
        auto t = s.label + " " +
                 (s.valueText.isNotEmpty() ? s.valueText : juce::String(juce::roundToInt(s.value * 100.0)) + " %");
        if (i < reference_.size())
            t << " (" << (referenceName_.isNotEmpty() ? referenceName_ : juce::String("reference")) << " "
              << juce::roundToInt(reference_[i] * 100.0) << " %)";
        parts.add(t);
    }
    if (parts.isEmpty()) return getTitle().isNotEmpty() ? getTitle() + ": no data" : juce::String();
    return (getTitle().isNotEmpty() ? getTitle() + ": " : juce::String()) + parts.joinIntoString("; ");
}

std::unique_ptr<juce::AccessibilityHandler> RadarChart::createAccessibilityHandler()
{
    return makeChartAccessibilityHandler(*this, [this] { return summary(); });
}

juce::Point<float> RadarChart::centre() const noexcept
{
    const auto b = getLocalBounds().toFloat();
    return {b.getCentreX(), b.getCentreY() + (referenceName_.isNotEmpty() ? 6.0f : 0.0f)};
}

float RadarChart::radius() const noexcept
{
    const auto b = getLocalBounds().toFloat();
    // Room for the spoke labels (two text lines) around the web.
    return std::min(b.getWidth() * 0.5f - 46.0f,
                    b.getHeight() * 0.5f - 26.0f - (referenceName_.isNotEmpty() ? 8.0f : 0.0f));
}

juce::Point<float> RadarChart::pointFor(size_t i, double norm) const noexcept
{
    const auto n = std::max<size_t>(1, spokes_.size());
    const float a = juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(n) -
                    juce::MathConstants<float>::halfPi;
    const float r = radius() * static_cast<float>(juce::jlimit(0.0, 1.0, norm));
    const auto c = centre();
    return {c.x + r * std::cos(a), c.y + r * std::sin(a)};
}

int RadarChart::spokeAt(juce::Point<float> pos) const
{
    if (spokes_.size() < 3 || radius() < 1.0f) return -1;
    const auto c = centre();
    const float d = pos.getDistanceFrom(c);
    if (d > radius() + 30.0f) return -1;
    float a = std::atan2(pos.y - c.y, pos.x - c.x) + juce::MathConstants<float>::halfPi;
    while (a < 0.0f)
        a += juce::MathConstants<float>::twoPi;
    const float step = juce::MathConstants<float>::twoPi / static_cast<float>(spokes_.size());
    return static_cast<int>(std::lround(a / step)) % static_cast<int>(spokes_.size());
}

Readout RadarChart::readoutFor(int spoke) const
{
    Readout r;
    if (spoke < 0 || spoke >= static_cast<int>(spokes_.size())) return r;
    const auto& s = spokes_[static_cast<size_t>(spoke)];
    r.title = s.label;
    const auto value = s.valueText.isNotEmpty() ? s.valueText : juce::String(juce::roundToInt(s.value * 100.0)) + " %";
    r.lines.push_back({(seriesName_.isNotEmpty() ? seriesName_ + " " : juce::String()) + value, seriesColour_});
    if (static_cast<size_t>(spoke) < reference_.size())
        r.lines.push_back({(referenceName_.isNotEmpty() ? referenceName_ : juce::String("Reference")) + " " +
                               juce::String(juce::roundToInt(reference_[static_cast<size_t>(spoke)] * 100.0)) + " %",
                           referenceColour_});
    return r;
}

void RadarChart::paint(juce::Graphics& g)
{
    syncPaletteStyle();
    const auto bounds = getLocalBounds().toFloat();
    const bool any = std::any_of(spokes_.begin(), spokes_.end(), [](const Spoke& s) { return s.value > 0.01; });
    if (spokes_.size() < 3 || !any) {
        g.setColour(style_.muted);
        g.setFont(Palette::uiFont(style_.fontSize));
        g.drawText(emptyText_, bounds, juce::Justification::centred, true);
        return;
    }
    if (radius() < 12.0f) return; // too small for a useful web
    const size_t n = spokes_.size();
    const auto c = centre();
    auto polygon = [&](auto valueOf) {
        juce::Path p;
        for (size_t i = 0; i < n; ++i) {
            const auto pt = pointFor(i, valueOf(i));
            if (i == 0)
                p.startNewSubPath(pt);
            else
                p.lineTo(pt);
        }
        p.closeSubPath();
        return p;
    };
    // Rings at 25 / 50 / 75 / 100 %, the 50 and 100 % ones labelled.
    for (const double f : {0.25, 0.5, 0.75, 1.0}) {
        g.setColour(f == 1.0 ? style_.axis : style_.grid);
        g.strokePath(polygon([f](size_t) { return f; }), juce::PathStrokeType(1.0f));
    }
    g.setColour(style_.grid);
    for (size_t i = 0; i < n; ++i)
        g.drawLine({c, pointFor(i, 1.0)}, 1.0f);
    g.setFont(Palette::uiFont(style_.fontSize * 0.85f));
    g.setColour(style_.muted);
    for (const double f : {0.5, 1.0}) {
        const auto pt = pointFor(0, f);
        g.drawText(juce::String(juce::roundToInt(f * 100.0)) + " %",
                   juce::Rectangle<float>(pt.x + 3.0f, pt.y - 1.0f, 40.0f, 11.0f), juce::Justification::centredLeft,
                   false);
    }

    if (reference_.size() >= n) {
        const auto ref = polygon([this](size_t i) { return reference_[i]; });
        g.setColour(referenceColour_);
        g.fillPath(dashedOf(ref, 1.3f, 4.0f, 3.0f));
    }
    const auto data = polygon([this](size_t i) { return spokes_[i].value; });
    g.setColour(seriesColour_.withAlpha(0.22f));
    g.fillPath(data);
    g.setColour(seriesColour_);
    g.strokePath(data, juce::PathStrokeType(1.8f));
    for (size_t i = 0; i < n; ++i) {
        const auto pt = pointFor(i, spokes_[i].value);
        const float r = static_cast<int>(i) == hovered_ ? 4.0f : 2.5f;
        g.fillEllipse(pt.x - r, pt.y - r, 2.0f * r, 2.0f * r);
    }

    // Spoke labels: the name, and the value beneath it.
    const auto f = Palette::uiFont(style_.fontSize);
    for (size_t i = 0; i < n; ++i) {
        const auto tip = pointFor(i, 1.0);
        const auto dir = (tip - c) / std::max(1.0f, radius());
        const auto at = tip + dir * 16.0f;
        auto box = juce::Rectangle<float>(84.0f, 26.0f).withCentre(at);
        if (dir.x > 0.35f)
            box.setX(at.x - 8.0f);
        else if (dir.x < -0.35f)
            box.setRight(at.x + 8.0f);
        box = box.constrainedWithin(bounds);
        const bool hov = static_cast<int>(i) == hovered_;
        const auto just = dir.x > 0.35f    ? juce::Justification::left
                          : dir.x < -0.35f ? juce::Justification::right
                                           : juce::Justification::centred;
        g.setFont(hov ? f.boldened() : f);
        g.setColour(hov ? style_.text : style_.muted);
        g.drawText(spokes_[i].label, box.removeFromTop(13.0f), just, false);
        g.setFont(Palette::uiFont(style_.fontSize * 0.9f));
        g.setColour(style_.text);
        g.drawText(spokes_[i].valueText.isNotEmpty() ? spokes_[i].valueText
                                                     : juce::String(juce::roundToInt(spokes_[i].value * 100.0)) + " %",
                   box, just, false);
    }

    if (referenceName_.isNotEmpty()) { // the key, top-left
        auto key = bounds.withHeight(14.0f).withTrimmedLeft(2.0f);
        g.setFont(Palette::uiFont(style_.fontSize * 0.9f));
        g.setColour(seriesColour_);
        g.fillRect(key.removeFromLeft(14.0f).withSizeKeepingCentre(14.0f, 2.0f));
        g.setColour(style_.muted);
        const auto sn = seriesName_.isNotEmpty() ? seriesName_ : juce::String("Track");
        const float sw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), sn) + 12.0f;
        g.drawText(sn, key.removeFromLeft(sw).withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
        auto sw2 = key.removeFromLeft(14.0f).withSizeKeepingCentre(14.0f, 2.0f);
        g.setColour(referenceColour_);
        g.fillRect(sw2.withWidth(5.0f));
        g.fillRect(sw2.withTrimmedLeft(8.0f).withWidth(5.0f));
        g.setColour(style_.muted);
        g.drawText(referenceName_, key.withTrimmedLeft(4.0f), juce::Justification::centredLeft, false);
    }

    if (hovered_ >= 0) ChartView::paintReadout(g, readoutFor(hovered_), hoverPos_, getLocalBounds().toFloat(), style_);
}

void RadarChart::mouseMove(const juce::MouseEvent& e)
{
    hoverPos_ = e.position;
    const int hit = spokeAt(e.position);
    if (hit != hovered_ || hit >= 0) {
        hovered_ = hit;
        repaint();
    }
}

void RadarChart::mouseExit(const juce::MouseEvent&)
{
    if (hovered_ < 0) return;
    hovered_ = -1;
    repaint();
}

} // namespace khensu::ui::chart
