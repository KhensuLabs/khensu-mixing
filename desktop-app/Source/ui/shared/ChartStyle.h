// desktop-app/Source/ui/shared/ChartStyle.h
// VIZ-F1: the chart toolkit's style from the live KhensuTheme tokens. Charts
// call desktopChartStyle() in paint() (or from a ThemeListener), so a theme
// change re-tints axes, grid and tooltips with everything else.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <khensu/ui/ChartKit.h>
#include "KhensuTheme.h"

namespace khensu {

inline ui::chart::Style desktopChartStyle()
{
    ui::chart::Style s;
    s.grid = KhensuTheme::vizGrid;
    s.axis = KhensuTheme::vizAxis;
    s.text = KhensuTheme::textPrimary;
    s.muted = KhensuTheme::textSecondary;
    s.crosshair = KhensuTheme::textPrimary.withAlpha(0.45f);
    s.tooltipBg = KhensuTheme::bgOverlay.withAlpha(0.97f);
    s.tooltipText = KhensuTheme::textPrimary;
    s.tooltipBorder = KhensuTheme::borderActive;
    s.fontSize = KhensuTheme::kFontLabel;
    return s;
}

/// A ChartView that follows the desktop theme.
class ThemedChart : public ui::chart::ChartView {
  public:
    ThemedChart() { setStyle(desktopChartStyle()); }

  private:
    ThemeListener themeListener_{[this] { setStyle(desktopChartStyle()); }};
};

} // namespace khensu
