// desktop-app/Source/ui/SessionIntake/TrendLinePanel.cpp
#include "TrendLinePanel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"

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

void TrendLinePanel::drawSparkline(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& label,
                                   juce::Colour colour, float minVal, float maxVal,
                                   std::function<float(const SessionManager::AnalysisSummary&)> extract) const
{
    g.setFont(KhensuTheme::labelFont(10.0f));
    g.setColour(colour);
    auto labelArea = bounds.removeFromLeft(90);
    g.drawText(label, labelArea, juce::Justification::centredLeft, true);

    if (summaries_.size() < 2) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("not enough history", bounds, juce::Justification::centredLeft, true);
        return;
    }

    // Inset vertically so the 3px endpoint dot and 2px stroke never bleed
    // past this row's own bounds into the row above/below.
    const float dotRadius = 3.0f;
    auto plot = bounds.toFloat().reduced(0.0f, dotRadius + 1.0f);

    // summaries_ is newest-first; walk oldest-to-newest so the line reads left->right.
    const int n = (int)summaries_.size();
    juce::Path path;
    const float range = juce::jmax(0.001f, maxVal - minVal);

    // Dashed reference line at this cohort's own historical average, so the
    // trend reads against "your normal" and not just an absolute min/max -
    // e.g. a Loudness line sitting flat above the dash means this cohort has
    // been trending louder than its own average lately.
    float sum = 0.0f;
    for (const auto& s : summaries_)
        sum += extract(s);
    const float avg = sum / (float)n;
    const float avgV = juce::jlimit(0.0f, 1.0f, (avg - minVal) / range);
    const float avgY = plot.getBottom() - avgV * plot.getHeight();
    {
        juce::Graphics::ScopedSaveState save(g);
        g.setColour(colour.withAlpha(0.35f));
        float dashLengths[] = {3.0f, 3.0f};
        juce::Path dashPath;
        dashPath.startNewSubPath(plot.getX(), avgY);
        dashPath.lineTo(plot.getRight(), avgY);
        juce::PathStrokeType(1.0f).createDashedStroke(dashPath, dashPath, dashLengths, 2);
        g.strokePath(dashPath, juce::PathStrokeType(1.0f));
    }

    for (int i = 0; i < n; ++i) {
        const auto& s = summaries_[(size_t)(n - 1 - i)];
        const float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        const float v = juce::jlimit(0.0f, 1.0f, (extract(s) - minVal) / range);
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
    const auto& latest = summaries_.front();
    const float v = juce::jlimit(0.0f, 1.0f, (extract(latest) - minVal) / range);
    const float x = plot.getRight() - 1.0f;
    const float y = plot.getBottom() - v * plot.getHeight();
    g.fillEllipse(x - dotRadius, y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
}

void TrendLinePanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds, 8.0f);

    juce::ColourGradient borderGrad(khensu::KhensuTheme::highlight.withAlpha(0.18f), bounds.getTopLeft(),
                                    KhensuTheme::bgVoid.withAlpha(0.60f), bounds.getBottomRight(), false);
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, 8.0f, 1.2f);

    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);

    g.setFont(khensu::KhensuTheme::uiFont(13.0f, juce::Font::bold));
    g.setColour(KhensuTheme::accentTeal);
    g.drawText("CROSS-SESSION TRENDS", b.removeFromTop(18), juce::Justification::centredLeft, true);
    b.removeFromTop(KhensuTheme::sp1);
    b.removeFromTop(22); // reserve the cohort-filter row laid out in resized()
    b.removeFromTop(KhensuTheme::sp1);

    if (summaries_.empty()) {
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(allSummaries_.empty() ? "Complete a few sessions to see trends here."
                                         : "No sessions match this filter.",
                   b, juce::Justification::centredLeft, true);
        return;
    }

    // Reserve gap space between rows up front so the per-row height floor
    // (needed for label legibility) can never push total content past b's
    // bottom edge - that overflow-then-clip was the "off balance/clipping"
    // bug: 6 rows floored to 20px each could exceed the space actually
    // available, drawing rows into (and getting cut off by) the panel below.
    constexpr int kRows = 6;
    constexpr int kRowGap = 4;
    // Floor is a legibility minimum, not a target - IntakeScreen sizes the
    // panel so the natural (height-based) row size is 26px, well above it.
    // If a host ever gives this panel less room, rows shrink together
    // instead of forcing a fixed size that would overflow b's bottom edge.
    const int rowH = juce::jmin(26, juce::jmax(12, (b.getHeight() - (kRows - 1) * kRowGap) / kRows));
    auto nextRow = [&b, kRowGap](int h) {
        auto r = b.removeFromTop(h);
        b.removeFromTop(kRowGap);
        return r;
    };

    drawSparkline(g, nextRow(rowH), "Loudness", KhensuTheme::accentTeal, -24.0f, -6.0f,
                  [](const SessionManager::AnalysisSummary& s) { return s.lufs_integrated; });
    drawSparkline(g, nextRow(rowH), "Energy", KhensuTheme::accentAmber, 0.0f, 100.0f,
                  [](const SessionManager::AnalysisSummary& s) { return s.energy_detected; });
    drawSparkline(g, nextRow(rowH), "Hook", KhensuTheme::accentPurple, 0.0f, 100.0f,
                  [](const SessionManager::AnalysisSummary& s) { return s.hook_strength; });
    drawSparkline(g, nextRow(rowH), "True Peak", KhensuTheme::accentRed, -3.0f, 0.0f,
                  [](const SessionManager::AnalysisSummary& s) { return s.true_peak_dbtp; });
    drawSparkline(g, nextRow(rowH), "Dyn Range", KhensuTheme::accentBlue, 0.0f, 20.0f,
                  [](const SessionManager::AnalysisSummary& s) { return s.dynamic_range_db; });
    drawSparkline(
        g, nextRow(rowH), "Readiness", KhensuTheme::accentGreen, 0.0f, 100.0f,
        [](const SessionManager::AnalysisSummary& s) { return s.readiness_score >= 0.0f ? s.readiness_score : 0.0f; });
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
}

} // namespace khensu
