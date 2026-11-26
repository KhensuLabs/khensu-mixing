// desktop-app/Source/ui/shared/ChartKitDesktop.h
// VIS-1: the chart toolkit's desktop side - any ChartKit chart type that
// follows the theme (Themed<T>, ThemedRadar), and the time-chart links to
// the transport: a click plays from that moment (the seek path every time
// chart uses: set the position, start playback if stopped) and the playhead
// follows playback at display rate.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/ChartKitPlots.h>
#include <khensu/ui/ChartScales.h>
#include <memory>
#include "../../services/IPlayback.h"
#include "ChartStyle.h"

namespace khensu {

/// A ChartKit chart that follows the desktop theme.
template <class Chart>
class Themed : public Chart {
  public:
    Themed() { this->setStyle(desktopChartStyle()); }

  private:
    ThemeListener themeListener_{[this] { this->setStyle(desktopChartStyle()); }};
};

using ThemedLineChart = Themed<ui::chart::LineChart>;
using ThemedBarChart = Themed<ui::chart::BarChart>;
using ThemedStackedAreaChart = Themed<ui::chart::StackedAreaChart>;

/// The radar chart, themed: the series in the track colour, the reference
/// (the genre's profile) in the median grey.
class ThemedRadar final : public ui::chart::RadarChart {
  public:
    ThemedRadar() { apply(); }

  private:
    void apply()
    {
        setStyle(desktopChartStyle());
        setColours(KhensuTheme::vizCategory(0), KhensuTheme::vizMedian);
    }
    ThemeListener themeListener_{[this] { apply(); }};
};

namespace chart_transport {

/// The seek path of every time chart: move the transport there and play.
inline void playFrom(IPlayback& playback, double seconds)
{
    playback.setPlaybackPosition(std::max(0.0, seconds));
    if (!playback.isPlaying()) playback.playAudio();
}

/// Click-to-play on a time chart: a click (not a drag) plays from there.
inline void attachClickToPlay(ui::chart::ChartView& chart, IPlayback& playback)
{
    chart.onClick = [&playback](double seconds) { playFrom(playback, seconds); };
    chart.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    if (chart.getTooltip().isEmpty()) chart.setTooltip(TRANS("Click to play from this moment"));
}

/// Keeps a time chart's playhead on the transport at display rate. Own one
/// per chart, declared after the chart. `active` (optional) says whether the
/// chart shows what is playing; while it is false the playhead is hidden.
class PlayheadFollower final {
  public:
    PlayheadFollower(ui::chart::ChartView& chart, IPlayback& playback, std::function<bool()> active = {})
        : chart_(chart), playback_(playback), active_(std::move(active)),
          vblank_(&chart, [this] { update(); })
    {
    }

  private:
    void update()
    {
        if (!chart_.isShowing()) return;
        const bool on = !active_ || active_();
        chart_.setPlayhead(on ? playback_.getPlaybackPosition() : -1.0);
    }
    ui::chart::ChartView& chart_;
    IPlayback& playback_;
    std::function<bool()> active_;
    juce::VBlankAttachment vblank_;
};

} // namespace chart_transport

} // namespace khensu
