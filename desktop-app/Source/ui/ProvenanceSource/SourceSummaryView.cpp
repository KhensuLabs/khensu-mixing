// desktop-app/Source/ui/ProvenanceSource/SourceSummaryView.cpp
#include "SourceSummaryView.h"
#include "../shared/KhensuEmptyState.h"
#include "../../IAppController.h"
#include <cmath>

namespace khensu {

namespace {
constexpr int kMaxLines = 6;

struct ScanName {
    const char* key;
    const char* title;
};
constexpr ScanName kScans[] = {
    {"source_triage", NEEDS_TRANS("Source triage")},
    {"source_forensics", NEEDS_TRANS("Source forensics")},
    {"signal_integrity", NEEDS_TRANS("Signal integrity")},
    {"ai_artifacts", NEEDS_TRANS("AI evidence")},
};

/// "dc_offset_db" -> "Dc offset db": the report's wording rule.
juce::String humanise(const juce::String& key)
{
    auto s = key.replaceCharacter('_', ' ').trim();
    return s.isEmpty() ? s : s.substring(0, 1).toUpperCase() + s.substring(1);
}

juce::String valueText(const juce::var& v)
{
    if (v.isBool()) return (bool)v ? TRANS("yes") : TRANS("no");
    if (v.isInt() || v.isInt64()) return juce::String((juce::int64)v);
    if (v.isDouble()) return juce::String((double)v, std::abs((double)v) < 1.0 ? 3 : 1); // 0.004 stays 0.004
    if (v.isString()) return v.toString().substring(0, 90);
    return {};
}
} // namespace

SourceSummaryView::SourceSummaryView(IAppController& c) : controller_(c)
{
    setOpaque(true);
    detailsButton_.setTooltip(TRANS("The source cards in full, with their charts and re-runs."));
    detailsButton_.onClick = [this] { controller_.shell().showScreen(ScreenType::Dashboard); };
    addAndMakeVisible(detailsButton_);
}

SourceSummaryView::~SourceSummaryView() { stopTimer(); }

std::vector<SourceSummaryView::Section> SourceSummaryView::sectionsFor(const juce::var& scans)
{
    std::vector<Section> out;
    for (const auto& scan : kScans) {
        Section s;
        s.title = juce::translate(scan.title);
        if (const auto* obj = scans[scan.key].getDynamicObject())
            for (const auto& p : obj->getProperties()) {
                const auto text = valueText(p.value);
                if (text.isNotEmpty()) s.lines.add(humanise(p.name.toString()) + ": " + text);
                if (s.lines.size() >= kMaxLines) break;
            }
        out.push_back(std::move(s));
    }
    return out;
}

void SourceSummaryView::refresh()
{
    sections_ = sectionsFor(controller_.analysis().scanResults());
    repaint();
}

void SourceSummaryView::visibilityChanged()
{
    // The scans finish after the analysis, one by one: re-read while shown.
    if (isShowing()) {
        refresh();
        startTimer(1000);
    } else {
        stopTimer();
    }
}

void SourceSummaryView::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);
    auto b = getLocalBounds();
    b.removeFromTop(KhensuTheme::buttonHSmall + KhensuTheme::sp2); // the details button row
    if (!controller_.hasActiveSession()) {
        KhensuEmptyState::paintInto(g, b, TRANS("Open a track: its source scans run with the analysis."), {}); // UXR-6
        return;
    }
    // Two columns of sections.
    const int colW = (b.getWidth() - KhensuTheme::cardGap) / 2;
    for (size_t i = 0; i < sections_.size(); ++i) {
        const auto& s = sections_[i];
        const int col = (int)i % 2;
        const int row = (int)i / 2;
        juce::Rectangle<int> r(b.getX() + col * (colW + KhensuTheme::cardGap), b.getY() + row * 170, colW, 160);
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(r.toFloat(), 6.0f, 1.0f);
        auto inner = r.reduced(12, 10);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::body));
        g.drawText(s.title, inner.removeFromTop(20), juce::Justification::centredLeft);
        g.setFont(KhensuTheme::labelFont(type::caption));
        if (s.lines.isEmpty()) {
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(TRANS("Not run yet for this track - it runs after the analysis."), inner.removeFromTop(18),
                       juce::Justification::centredLeft);
            continue;
        }
        g.setColour(KhensuTheme::textSecondary);
        for (const auto& line : s.lines)
            g.drawText(line, inner.removeFromTop(18), juce::Justification::centredLeft, true);
    }
}

void SourceSummaryView::resized()
{
    detailsButton_.setBounds(getLocalBounds().removeFromTop(KhensuTheme::buttonHSmall).removeFromRight(260));
}

} // namespace khensu
