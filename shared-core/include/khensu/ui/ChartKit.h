// shared-core/include/khensu/ui/ChartKit.h
// VIZ-F1: one chart toolkit for the desktop app and the plugin. Every chart
// gets the same axes (linear, log-frequency, time), recessive grid, legend,
// hover crosshair with the exact value and unit (VIZ-U3), wheel zoom / drag
// pan on the zoomable axis and double-click to reset. A chart derives from
// ChartView, sets its axes and draws its data in paintData() through px()/py()
// - it never computes its own tick positions or tooltip.
// Colours come from a Style: Style::fromPalette() (the current ui::Palette
// theme, followed until setStyle() - STU-6);
// the desktop passes its live KhensuTheme tokens (ui/shared/ChartStyle.h) so a
// theme change re-tints every chart.
// VIS-1 / VIS-8: every chart is an accessible image whose description is a
// text summary built from its data (summary()); the units are written on the
// axes (the y unit beside the top tick, the x unit on the last tick); a time
// chart can draw a playhead (setPlayhead) and seek on click (onClick). The
// standard axes (one frequency axis, the dB / LUFS ranges) are in
// ChartScales.h; bar, stacked-area and radar charts in ChartKitPlots.h.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace khensu::ui::chart {

enum class Scale { Linear, Log };
enum class Format { Number, Hz, Time, Percent, Category };

/// One axis: its visible range, scale and how a value is written.
struct Axis {
    double lo = 0.0, hi = 1.0;
    Scale scale = Scale::Linear;
    Format format = Format::Number;
    juce::String unit; ///< "dB", "LUFS", ... (Hz / s / % come from the format)
    int decimals = 1;
    /// Format::Category: the name of each integer position (0, 1, 2 ...).
    std::vector<juce::String> categories;
    /// Runs the other way (a category axis listed top-down).
    bool inverted = false;

    /// 0..1 across the visible range (log for Scale::Log; values <= 0 clamp).
    double norm(double v) const noexcept;
    double denorm(double t) const noexcept;
    /// "Nice" tick values inside the range, at most about `maxTicks`.
    std::vector<double> ticks(int maxTicks = 6) const;
    /// A tick label ("1k", "2:30", "-12") or, withUnit, a readout ("1.2 kHz",
    /// "2:30.4", "-12.3 dB").
    juce::String label(double v, bool withUnit = false) const;
};

struct Style {
    juce::Colour grid, axis, text, muted, crosshair, tooltipBg, tooltipText, tooltipBorder;
    juce::Colour playhead; ///< the transport position on a time chart
    float fontSize = 11.0f;
    /// The current khensu::ui::Palette theme's values (STU-6). A chart that
    /// never had setStyle() called re-reads them when the palette changes.
    static Style fromPalette();
};

struct LegendItem {
    juce::String name;
    juce::Colour colour;
    enum class Kind { Line, Dashed, Band, Dot } kind = Kind::Line;
};

/// What the hover tooltip shows: a title (the x value) and one line per
/// series, each beside its colour.
struct Readout {
    juce::String title;
    std::vector<std::pair<juce::String, juce::Colour>> lines;
};

/// VIS-8: an accessibility handler that presents `component` as an image
/// whose description is `describe()` (falling back to its description).
std::unique_ptr<juce::AccessibilityHandler> makeChartAccessibilityHandler(juce::Component& component,
                                                                          std::function<juce::String()> describe);

/// VIS-8: for a chart painted by a component that is not a ChartView (a
/// panel that draws its own plot): a transparent, click-through child laid
/// over the plot that screen readers announce as an image with the summary.
class ChartSummary final : public juce::Component {
  public:
    ChartSummary();
    std::function<juce::String()> describe;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
};

/// "a, b and c" - for summaries.
juce::String joinWords(const juce::StringArray& items);

class ChartView : public juce::Component, public juce::SettableTooltipClient {
  public:
    ChartView();
    ~ChartView() override;

    Axis x, y;

    /// Fixes the style; until called, the chart follows the shared Palette.
    void setStyle(Style s);
    const Style& style() const
    {
        syncPaletteStyle();
        return style_;
    }
    void setLegend(std::vector<LegendItem> items);
    /// The full x range zoom returns to; set with the data. Zoom is on x only.
    void setXRange(double lo, double hi);
    void setZoomable(bool zoomable) noexcept { zoomable_ = zoomable; }
    void resetZoom();
    bool isZoomed() const noexcept;

    void setMargins(int left, int top, int right, int bottom);
    void setShowAxes(bool xAxis, bool yAxis) noexcept { showX_ = xAxis; showY_ = yAxis; }
    void setShowGrid(bool show) noexcept { showGrid_ = show; gridX_ = gridY_ = show; }
    /// Gridlines on the x ticks and / or the y ticks only (bar charts keep
    /// the value lines and drop the category lines).
    void setGridLines(bool xLines, bool yLines) noexcept { gridX_ = xLines; gridY_ = yLines; showGrid_ = xLines || yLines; }

    /// A second y scale on the right (a curve with its own unit). Off by default.
    Axis y2;
    void setSecondaryAxis(bool show) noexcept { showY2_ = show; }
    float py2(double v) const noexcept;

    /// VIS-8: the text a screen reader reads for this chart. Set it with the
    /// data; when empty, autoSummary() (built from the data) is used.
    void setSummary(juce::String text);
    juce::String summary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /// A time chart's transport position (x units); < 0 hides it. Repaints
    /// only the strips it leaves and enters.
    void setPlayhead(double xValue);
    double playhead() const noexcept { return playhead_; }

    juce::Rectangle<float> plotArea() const noexcept;
    float px(double v) const noexcept;
    float py(double v) const noexcept;
    double xAt(float pixel) const noexcept;
    double yAt(float pixel) const noexcept;

    /// A click (not a drag) in the plot, with the x value under it.
    std::function<void(double x)> onClick;
    /// The same click with both values (charts whose categories run on y).
    std::function<void(double x, double y)> onClickAt;

    /// The hover position (x, y values), when the mouse is over the plot.
    std::optional<std::pair<double, double>> hover() const noexcept { return hover_; }
    /// For tests and for charts that drive the crosshair themselves.
    void setHover(std::optional<std::pair<double, double>> h);
    Readout currentReadout() { return hover_ ? readoutAt(hover_->first, hover_->second) : Readout{}; }

    /// A line through (xs[i], ys[i]); non-finite values break the line.
    juce::Path seriesPath(const std::vector<double>& xs, const std::vector<double>& ys) const;
    juce::Path seriesPath(const std::vector<float>& xs, const std::vector<float>& ys) const;
    /// A closed band between lo[i] and hi[i] over xs.
    juce::Path bandPath(const std::vector<double>& xs, const std::vector<double>& lo,
                        const std::vector<double>& hi) const;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    /// Draws a readout box near `anchor`, kept inside `bounds`.
    static void paintReadout(juce::Graphics& g, const Readout& r, juce::Point<float> anchor,
                             juce::Rectangle<float> bounds, const Style& s);

  protected:
    /// Draw the data inside `plot` (already clipped).
    virtual void paintData(juce::Graphics& g, juce::Rectangle<float> plot) = 0;
    /// Drawn above the data and axes, under the crosshair (markers, labels).
    virtual void paintOverlay(juce::Graphics&, juce::Rectangle<float>) {}
    /// The tooltip at (x, y); the default writes both values with units.
    virtual Readout readoutAt(double xValue, double yValue);
    /// Called when the hover moves (charts that link a playhead use it).
    virtual void hoverChanged() {}
    /// The hover mark under the readout; the default is a crosshair.
    virtual void paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float px, float py);
    /// VIS-8: the summary built from the data (used when none was set).
    virtual juce::String autoSummary() const { return {}; }

    juce::Font font(float scale = 1.0f) const;

  private:
    void paintGrid(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintAxes(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintLegend(juce::Graphics& g, juce::Rectangle<float> plot) const;
    void paintCrosshair(juce::Graphics& g, juce::Rectangle<float> plot);
    /// STU-6: re-reads Style::fromPalette() when the palette's theme changed
    /// and no setStyle() fixed the style.
    void syncPaletteStyle() const;

    mutable Style style_;
    mutable std::uint32_t paletteVersion_ = 0;
    bool paletteStyle_ = true;
    std::vector<LegendItem> legend_;
    juce::String summary_;
    double playhead_ = -1.0;
    bool showY2_ = false, gridX_ = true, gridY_ = true;
    double fullLo_ = 0.0, fullHi_ = 1.0;
    bool zoomable_ = false;
    int ml_ = 40, mt_ = 8, mr_ = 10, mb_ = 20;
    bool showX_ = true, showY_ = true, showGrid_ = true;
    std::optional<std::pair<double, double>> hover_;
    juce::Point<float> downPos_;
    double dragLo_ = 0.0, dragHi_ = 1.0;
    bool dragged_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChartView)
};

} // namespace khensu::ui::chart
