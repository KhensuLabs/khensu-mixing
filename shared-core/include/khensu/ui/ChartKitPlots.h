// shared-core/include/khensu/ui/ChartKitPlots.h
// VIS-1: the chart types built on ChartView, so a panel sets data instead of
// drawing its own axes, ticks and tooltip:
//   LineChart        lines, dots, steps and areas over x, with bands (a
//                    tolerance, a spread), reference lines and shaded spans;
//                    an optional second y scale on the right;
//   BarChart         categories against a value, vertical or horizontal,
//                    grouped series, a target tick per bar;
//   StackedAreaChart layers stacked over x (shares or absolute);
//   RadarChart       values on spokes (polar), with a reference polygon.
// Each one writes its hover readout (the value and unit under the pointer)
// and builds its screen-reader summary from the data (VIS-8).
// THREAD_CONTEXT(UIThread)
#pragma once
#include "ChartKit.h"
#include <optional>
#include <vector>

namespace khensu::ui::chart {

// ---- LineChart --------------------------------------------------------------------

struct Series {
    enum class Kind { Line, Dashed, Dots, Steps, Area };
    juce::String name;
    juce::Colour colour;
    std::vector<double> xs, ys; ///< non-finite ys break the line
    Kind kind = Kind::Line;
    float width = 1.6f;
    bool secondary = false; ///< drawn against y2 (the right-hand scale)
    bool inReadout = true;
    bool inLegend = true;
};

struct Band {
    juce::String name;
    juce::Colour colour; ///< filled as is (set the alpha)
    std::vector<double> xs, lo, hi;
};

/// A reference line: horizontal at y = value, or vertical at x = value.
struct RefLine {
    double value = 0.0;
    juce::String label;
    juce::Colour colour;
    bool vertical = false;
    bool dashed = true;
};

/// A shaded range: x from lo to hi (vertical = true), or y from lo to hi.
struct Span {
    double lo = 0.0, hi = 0.0;
    juce::Colour colour; ///< filled as is (set the alpha)
    juce::String label;  ///< written at the top of the span, and in the readout
    bool vertical = true;
};

class LineChart : public ChartView {
  public:
    void setSeries(std::vector<Series> series);
    void setBands(std::vector<Band> bands);
    void setRefLines(std::vector<RefLine> lines);
    void setSpans(std::vector<Span> spans);
    void clear();
    /// Builds the legend from the series' names (on by default).
    void setAutoLegend(bool on);
    const std::vector<Series>& series() const noexcept { return series_; }

    /// The series' value at x: interpolated for lines, the step's value for
    /// steps, the nearest point for dots; empty outside the data or in a gap.
    static std::optional<double> valueAt(const Series& s, double x);

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    /// Hooks around the series (lanes, markers, highlights).
    virtual void paintUnder(juce::Graphics&, juce::Rectangle<float>) {}
    virtual void paintOver(juce::Graphics&, juce::Rectangle<float>) {}
    Readout readoutAt(double x, double y) override;
    /// Extra lines for the readout at x (a section name, a finding).
    virtual void extendReadout(Readout&, double) {}
    juce::String autoSummary() const override;
    float yFor(const Series& s, double v) const noexcept { return s.secondary ? py2(v) : py(v); }

  private:
    void rebuildLegend();
    std::vector<Series> series_;
    std::vector<Band> bands_;
    std::vector<RefLine> lines_;
    std::vector<Span> spans_;
    bool autoLegend_ = true;
};

// ---- BarChart ---------------------------------------------------------------------

struct Bar {
    juce::String label;
    std::vector<double> values;   ///< one per series; NaN = not measured
    std::optional<double> target; ///< a tick across the bar
    juce::Colour colour;          ///< overrides the series colour when opaque
    float alpha = 1.0f;           ///< dims an estimate
    juce::String note;            ///< replaces the value text ("not detected")
};

struct BarSeries {
    juce::String name;
    juce::Colour colour;
};

class BarChart : public ChartView {
  public:
    BarChart();
    /// Categories along y (listed top-down) instead of along x.
    void setHorizontal(bool horizontal);
    bool isHorizontal() const noexcept { return horizontal_; }
    /// Bars grow from this value (default: the value axis' low end).
    void setBaseline(std::optional<double> v);
    /// The value written at the end of each bar.
    void setShowValues(bool show) noexcept { showValues_ = show; }
    /// Margins that survive setBars() (which otherwise lays them out); a
    /// horizontal chart still widens the left one to fit its labels.
    void setFixedMargins(int left, int top, int right, int bottom);
    void setBars(std::vector<Bar> bars, std::vector<BarSeries> series);
    const std::vector<Bar>& bars() const noexcept { return bars_; }
    /// The value axis (x when horizontal, else y).
    Axis& valueAxis() noexcept { return horizontal_ ? x : y; }
    const Axis& valueAxis() const noexcept { return horizontal_ ? x : y; }
    /// A bar was clicked (its index).
    std::function<void(int bar)> onBarClicked;
    /// The bar under a value pair (x, y), or -1.
    int barAt(double xValue, double yValue) const;
    /// A bar's value text (default: the value with the axis unit).
    std::function<juce::String(size_t bar, size_t series, double value)> valueText;

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    void paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float px, float py) override;
    Readout readoutAt(double x, double y) override;
    juce::String autoSummary() const override;
    juce::String textFor(size_t bar, size_t series, double value) const;

  private:
    void layoutAxes();
    std::vector<Bar> bars_;
    std::vector<BarSeries> series_;
    bool horizontal_ = false, showValues_ = true;
    std::optional<double> baseline_;
    std::optional<juce::Rectangle<int>> fixedMargins_; ///< left, top, right, bottom as x, y, w, h
};

// ---- StackedAreaChart -------------------------------------------------------------

struct Layer {
    juce::String name;
    juce::Colour colour;
    std::vector<double> ys; ///< one per x; non-finite counts as 0
};

class StackedAreaChart : public ChartView {
  public:
    /// `normalise`: each x sums to 1 (shares, Percent axis) - else absolute.
    void setLayers(std::vector<double> xs, std::vector<Layer> layers, bool normalise);
    const std::vector<Layer>& layers() const noexcept { return layers_; }
    /// The layer under a point, or -1.
    int layerAt(double xValue, double yValue) const;

  protected:
    void paintData(juce::Graphics& g, juce::Rectangle<float> plot) override;
    Readout readoutAt(double x, double y) override;
    juce::String autoSummary() const override;

  private:
    size_t indexAt(double xValue) const;
    double valueOf(size_t layer, size_t i) const;
    std::vector<double> xs_;
    std::vector<Layer> layers_;
    bool normalise_ = true;
};

// ---- RadarChart -------------------------------------------------------------------

struct Spoke {
    juce::String label;
    double value = 0.0;     ///< 0..1 along the spoke
    juce::String valueText; ///< the readout ("0.62", "-14 LUFS"); default: a percent
};

class RadarChart : public juce::Component, public juce::SettableTooltipClient {
  public:
    RadarChart();
    /// Fixes the style; until called, the chart follows the shared Palette.
    void setStyle(Style s);
    const Style& style() const
    {
        syncPaletteStyle();
        return style_;
    }
    void setSpokes(std::vector<Spoke> spokes);
    /// A second polygon (the genre's profile), one value per spoke.
    void setReference(std::vector<double> values, juce::String name);
    void setColours(juce::Colour series, juce::Colour reference);
    void setSeriesName(juce::String name);
    /// Shown when no spoke has a value.
    void setEmptyText(juce::String text);
    const std::vector<Spoke>& spokes() const noexcept { return spokes_; }

    void setSummary(juce::String text);
    juce::String summary() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /// The spoke nearest a point (by angle), or -1 outside the web.
    int spokeAt(juce::Point<float> pos) const;
    Readout readoutFor(int spoke) const;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    juce::Point<float> centre() const noexcept;
    float radius() const noexcept;
    juce::Point<float> pointFor(size_t i, double norm) const noexcept;
    /// STU-6: follows the palette until setStyle() / setColours().
    void syncPaletteStyle() const;

    mutable Style style_;
    mutable std::uint32_t paletteVersion_ = 0;
    bool paletteStyle_ = true, paletteColours_ = true;
    std::vector<Spoke> spokes_;
    std::vector<double> reference_;
    juce::String referenceName_, seriesName_, emptyText_, summary_;
    mutable juce::Colour seriesColour_, referenceColour_;
    int hovered_ = -1;
    juce::Point<float> hoverPos_;
};

} // namespace khensu::ui::chart
