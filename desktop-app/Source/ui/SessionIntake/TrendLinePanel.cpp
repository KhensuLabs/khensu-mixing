// desktop-app/Source/ui/SessionIntake/TrendLinePanel.cpp
#include "TrendLinePanel.h"
#include "../shared/KhensuEmptyState.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ChartStyle.h"
#include <khensu/ui/KhensuPalette.h>

namespace khensu {

TrendLinePanel::TrendLinePanel()
{
    filterCombo_.addItem(TRANS("All Sessions"), 1);
    filterCombo_.addItem(TRANS("This Genre"), 2);
    filterCombo_.addItem(TRANS("This Client"), 3);
    filterCombo_.addItem(TRANS("Custom Date Range"), 4);
    filterCombo_.setSelectedId(1, juce::dontSendNotification);
    filterCombo_.onChange = [this] {
        applyFilter();
        resized();
    };
    addAndMakeVisible(filterCombo_);

    clientQueryEditor_.setTextToShowWhenEmpty(TRANS("Session name contains..."), KhensuTheme::textSecondary);
    clientQueryEditor_.onTextChange = [this] { applyFilter(); };
    addChildComponent(clientQueryEditor_);

    rangeStartEditor_.setTextToShowWhenEmpty(TRANS("Start YYYY-MM-DD"), KhensuTheme::textSecondary);
    rangeStartEditor_.onTextChange = [this] { applyFilter(); };
    addChildComponent(rangeStartEditor_);

    rangeEndEditor_.setTextToShowWhenEmpty(TRANS("End YYYY-MM-DD"), KhensuTheme::textSecondary);
    rangeEndEditor_.onTextChange = [this] { applyFilter(); };
    addChildComponent(rangeEndEditor_);

    // VIS-8: the sparklines read as one image with a text summary.
    summaryOverlay_.setTitle(TRANS("Cross-session trends"));
    summaryOverlay_.describe = [this] { return summary(); };
    addAndMakeVisible(summaryOverlay_);
}

void TrendLinePanel::setSummaries(std::vector<SessionManager::AnalysisSummary> summaries)
{
    allSummaries_ = std::move(summaries);
    applyFilter();
}

void TrendLinePanel::setCurrentGenre(const juce::String& genre)
{
    currentGenre_ = genre;
    if (filterCombo_.getSelectedId() == 2) applyFilter();
}

void TrendLinePanel::applyFilter()
{
    const auto filter = static_cast<CohortFilter>(juce::jmax(1, filterCombo_.getSelectedId()) - 1);

    clientQueryEditor_.setVisible(filter == CohortFilter::ThisClient);
    rangeStartEditor_.setVisible(filter == CohortFilter::CustomRange);
    rangeEndEditor_.setVisible(filter == CohortFilter::CustomRange);

    switch (filter) {
    case CohortFilter::All:
        summaries_ = allSummaries_;
        break;

    case CohortFilter::ThisGenre: {
        summaries_.clear();
        for (const auto& s : allSummaries_)
            if (currentGenre_.isNotEmpty() && juce::String(s.genre_detected).equalsIgnoreCase(currentGenre_))
                summaries_.push_back(s);
        break;
    }

    case CohortFilter::ThisClient: {
        const auto query = clientQueryEditor_.getText().trim();
        summaries_.clear();
        for (const auto& s : allSummaries_)
            if (query.isEmpty() || juce::String(s.session_name).containsIgnoreCase(query)) summaries_.push_back(s);
        break;
    }

    case CohortFilter::CustomRange: {
        const auto start = rangeStartEditor_.getText().trim();
        const auto end = rangeEndEditor_.getText().trim();
        summaries_.clear();
        for (const auto& s : allSummaries_) {
            // computed_at is ISO-8601 ("YYYY-MM-DDTHH:MM:SS..."), so plain
            // string comparison against a YYYY-MM-DD bound sorts correctly.
            const juce::String computedAt(s.computed_at);
            if (start.isNotEmpty() && computedAt < start) continue;
            if (end.isNotEmpty() && computedAt.substring(0, 10) > end) continue;
            summaries_.push_back(s);
        }
        break;
    }
    }
    repaint();
}

const std::vector<TrendLinePanel::Metric>& TrendLinePanel::metrics()
{
    // VIS-4: each metric is a series - a categorical colour (identity),
    // never a severity colour. The unit is written beside the latest value.
    using S = SessionManager::AnalysisSummary;
    static const std::vector<Metric> m{
        {NEEDS_TRANS("Loudness"), "LUFS", 0, -24.0f, -6.0f, [](const S& s) { return s.lufs_integrated; }},
        {NEEDS_TRANS("Energy"), "/100", 1, 0.0f, 100.0f, [](const S& s) { return s.energy_detected; }},
        {NEEDS_TRANS("Hook"), "/100", 2, 0.0f, 100.0f, [](const S& s) { return s.hook_strength; }},
        {NEEDS_TRANS("True Peak"), "dBTP", 3, -3.0f, 0.0f, [](const S& s) { return s.true_peak_dbtp; }},
        {NEEDS_TRANS("Dyn Range"), "dB", 4, 0.0f, 20.0f, [](const S& s) { return s.dynamic_range_db; }},
        {NEEDS_TRANS("Readiness"), "/100", 5, 0.0f, 100.0f,
         [](const S& s) { return s.readiness_score >= 0.0f ? s.readiness_score : 0.0f; }},
    };
    return m;
}

void TrendLinePanel::drawSparkline(juce::Graphics& g, juce::Rectangle<int> bounds, const Metric& metric) const
{
    const auto colour = KhensuTheme::vizCategory(metric.slot);
    const auto style = desktopChartStyle();
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textSecondary);
    auto labelArea = bounds.removeFromLeft(90);
    // The key: the series colour beside its name (the name carries it).
    g.setColour(colour);
    g.fillRect(labelArea.removeFromLeft(10).toFloat().withSizeKeepingCentre(8.0f, 2.0f));
    labelArea.removeFromLeft(4);
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(juce::translate(metric.label), labelArea, juce::Justification::centredLeft, true);

    if (summaries_.size() < 2) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("not enough history"), bounds, juce::Justification::centredLeft, true);
        return;
    }

    // The latest value with its unit, at the right.
    const auto& latest = summaries_.front();
    auto valueArea = bounds.removeFromRight(76);
    g.setColour(KhensuTheme::textPrimary);
    g.setFont(ui::Palette::uiFont(style.fontSize));
    g.drawText(juce::String(metric.extract(latest), 1) + " " + metric.unit, valueArea, juce::Justification::centredRight, false);
    bounds.removeFromRight(6);

    // Inset vertically so the 3px endpoint dot and 2px stroke never bleed
    // past this row's own bounds into the row above/below.
    const float dotRadius = 3.0f;
    auto plot = bounds.toFloat().reduced(0.0f, dotRadius + 1.0f);
    plots_.push_back(plot);

    // summaries_ is newest-first; walk oldest-to-newest so the line reads left->right.
    const int n = (int)summaries_.size();
    juce::Path path;
    const float range = juce::jmax(0.001f, metric.max - metric.min);

    // Dashed reference line at this cohort's own historical average, so the
    // trend reads against "your normal" and not just an absolute min/max.
    float sum = 0.0f;
    for (const auto& s : summaries_)
        sum += metric.extract(s);
    const float avg = sum / (float)n;
    const float avgV = juce::jlimit(0.0f, 1.0f, (avg - metric.min) / range);
    const float avgY = plot.getBottom() - avgV * plot.getHeight();
    {
        g.setColour(colour.withAlpha(0.45f));
        float dashLengths[] = {3.0f, 3.0f};
        juce::Path line, dashed;
        line.startNewSubPath(plot.getX(), avgY);
        line.lineTo(plot.getRight(), avgY);
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dashLengths, 2);
        g.fillPath(dashed); // createDashedStroke returns a filled outline
    }

    for (int i = 0; i < n; ++i) {
        const auto& s = summaries_[(size_t)(n - 1 - i)];
        const float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        const float v = juce::jlimit(0.0f, 1.0f, (metric.extract(s) - metric.min) / range);
        const float x = plot.getX() + t * plot.getWidth();
        const float y = plot.getBottom() - v * plot.getHeight();
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    // Endpoint dot to make the latest value legible at a glance.
    const float v = juce::jlimit(0.0f, 1.0f, (metric.extract(latest) - metric.min) / range);
    const float x = plot.getRight() - 1.0f;
    const float y = plot.getBottom() - v * plot.getHeight();
    g.fillEllipse(x - dotRadius, y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
}

juce::String TrendLinePanel::summary() const
{
    // VIS-8: each metric's latest value against the cohort's average.
    if (summaries_.size() < 2) return TRANS("Cross-session trends: not enough history yet.");
    juce::StringArray parts;
    for (const auto& m : metrics()) {
        float sum = 0.0f;
        for (const auto& s : summaries_)
            sum += m.extract(s);
        const float avg = sum / static_cast<float>(summaries_.size());
        const float latest = m.extract(summaries_.front());
        parts.add(juce::translate(m.label) + " " + juce::String(latest, 1) + " " + m.unit + " (" + TRANS("average") + " " +
                  juce::String(avg, 1) + ")");
    }
    return TRANS("Cross-session trends over") + " " + juce::String(static_cast<int>(summaries_.size())) + " " +
           TRANS("sessions, latest first") + ": " + parts.joinIntoString("; ") + ".";
}

void TrendLinePanel::mouseMove(const juce::MouseEvent& e)
{
    hoverPos_ = e.position;
    repaint();
}

void TrendLinePanel::mouseExit(const juce::MouseEvent&)
{
    hoverPos_ = {-1.0f, -1.0f};
    repaint();
}

void TrendLinePanel::paint(juce::Graphics& g)
{
    plots_.clear();
    auto bounds = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds, 8.0f);

    juce::ColourGradient borderGrad(khensu::KhensuTheme::highlight.withAlpha(0.18f), bounds.getTopLeft(),
                                    KhensuTheme::bgVoid.withAlpha(0.60f), bounds.getBottomRight(), false);
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, 8.0f, 1.2f);

    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);

    g.setFont(khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::bold));
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(TRANS("Cross-session trends"), b.removeFromTop(18), juce::Justification::centredLeft, true);
    b.removeFromTop(KhensuTheme::sp1);
    b.removeFromTop(22); // reserve the cohort-filter row laid out in resized()
    b.removeFromTop(KhensuTheme::sp1);

    if (summaries_.empty()) {
        KhensuEmptyState::paintInto(g, b,
                                    allSummaries_.empty() ? TRANS("Complete a few sessions to see trends here.")
                                                          : TRANS("No sessions match this filter."),
                                    {}); // UXR-6
        return;
    }

    // Reserve gap space between rows up front so the per-row height floor
    // (needed for label legibility) can never push total content past b's
    // bottom edge.
    const int kRows = static_cast<int>(metrics().size());
    constexpr int kRowGap = 4;
    const int rowH = juce::jmin(26, juce::jmax(12, (b.getHeight() - (kRows - 1) * kRowGap) / kRows));
    for (const auto& m : metrics()) {
        drawSparkline(g, b.removeFromTop(rowH), m);
        b.removeFromTop(kRowGap);
    }

    // VIS-1: the ChartKit readout for the session under the pointer.
    const int n = static_cast<int>(summaries_.size());
    for (size_t row = 0; row < plots_.size() && row < metrics().size() && n >= 2; ++row) {
        const auto plot = plots_[row];
        if (!plot.expanded(0.0f, 3.0f).contains(hoverPos_)) continue;
        const int i = juce::jlimit(0, n - 1, juce::roundToInt((hoverPos_.x - plot.getX()) / plot.getWidth() * (float)(n - 1)));
        const auto& s = summaries_[(size_t)(n - 1 - i)]; // oldest at the left
        const auto& m = metrics()[row];
        const float x = plot.getX() + (n > 1 ? (float)i / (float)(n - 1) : 0.0f) * plot.getWidth();
        g.setColour(KhensuTheme::textPrimary.withAlpha(0.35f));
        g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        ui::chart::Readout r;
        r.title = juce::String(s.session_name);
        r.lines.push_back({juce::translate(m.label) + " " + juce::String(m.extract(s), 1) + " " + m.unit,
                           KhensuTheme::vizCategory(m.slot)});
        r.lines.push_back({juce::String(s.computed_at).substring(0, 10), KhensuTheme::textTertiary});
        ui::chart::ChartView::paintReadout(g, r, {x, plot.getCentreY()}, getLocalBounds().toFloat(), desktopChartStyle());
        break;
    }
}

void TrendLinePanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);
    b.removeFromTop(18 + KhensuTheme::sp1);

    auto filterRow = b.removeFromTop(22);
    filterCombo_.setBounds(filterRow.removeFromLeft(150));
    filterRow.removeFromLeft(6);
    if (clientQueryEditor_.isVisible()) {
        clientQueryEditor_.setBounds(filterRow);
    } else if (rangeStartEditor_.isVisible()) {
        rangeStartEditor_.setBounds(filterRow.removeFromLeft(filterRow.getWidth() / 2 - 3));
        filterRow.removeFromLeft(6);
        rangeEndEditor_.setBounds(filterRow);
    }
    b.removeFromTop(KhensuTheme::sp1);
    summaryOverlay_.setBounds(b);
}

} // namespace khensu
