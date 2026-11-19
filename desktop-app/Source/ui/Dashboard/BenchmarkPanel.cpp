// desktop-app/Source/ui/Dashboard/BenchmarkPanel.cpp
#include "BenchmarkPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
const juce::String kMetricLabels[] = {"Loudness", "Loudness Range", "Tonal Tilt", "Crest Factor"};
const char* const kMetricKeys[] = {"lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db"};

juce::String formatTime(float seconds)
{
    const int totalSeconds = juce::jmax(0, static_cast<int>(std::round(seconds)));
    return juce::String(totalSeconds / 60) + ":" + juce::String(totalSeconds % 60).paddedLeft('0', 2);
}

juce::String mostDivergentSectionSummary(const juce::var& response)
{
    const auto* sections = response[af::ReferenceMatchResponse::sections].getArray();
    if (sections == nullptr || sections->isEmpty()) return {};

    const juce::var* mostDivergent = nullptr;
    float greatestDistance = -1.0f;
    for (const auto& section : *sections) {
        const float distance = static_cast<float>(section[af::ReferenceMatchSection::nearest][af::ReferenceMatchEntry::distance]);
        if (distance > greatestDistance) {
            greatestDistance = distance;
            mostDivergent = &section;
        }
    }
    if (mostDivergent == nullptr) return {};

    const juce::String guidance = (*mostDivergent)[af::ReferenceMatchSection::guidance].toString();
    if (guidance.isEmpty()) return {};
    return "Section reference (most different) - "
        + (*mostDivergent)[af::ReferenceMatchSection::label].toString() + " ("
        + formatTime(static_cast<float>((*mostDivergent)[af::ReferenceMatchSection::start_sec])) + "-"
        + formatTime(static_cast<float>((*mostDivergent)[af::ReferenceMatchSection::end_sec])) + "): " + guidance;
}
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void BenchmarkPanel::applyThemeColours()
{
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

BenchmarkPanel::BenchmarkPanel(IAppController& c) : PanelCard(c, "Chart Benchmark")
{
    applyThemeColours();
    setExplainer(TRANS("Chart Benchmark"),
                 TRANS("Percentile rank of this track's measured loudness/dynamics/tonal-balance against a "
                 "reference-measurement library for the detected genre, instead of distance from a "
                 "single target number. Seeded with a ~139-genre bootstrap/synthetic library (sampled "
                 "around each genre's production norms, not measurements of real reference tracks), so "
                 "percentiles read as directional until real reference tracks are analyzed and swapped "
                 "in; empty only for a genre with no library entry. Also compares each detected "
                 "section against the same library and calls out the section that differs most."));

    summaryLabel_.setFont(KhensuTheme::bodyFont(11.0f));
    summaryLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(summaryLabel_);
}

void BenchmarkPanel::setData(const AnalysisResult& r)
{
    if (r.benchmark.has_value()) {
        benchmark_ = r.benchmark;
    } else if (!r.has_fast_analysis()) {
        // Pre-analysis only: a clearly-LABELED demo preview (FE-01) - not
        // fabricated percentiles passed off as real. is_bootstrap_data=true so
        // the panel's synthetic/directional caveat is shown, not suppressed.
        AnalysisResult::BenchmarkData demo;
        demo.genre_id = "synthwave (demo)";
        demo.sample_size = 85;
        demo.is_bootstrap_data = true;
        demo.competitive_summary = "(Demo) Competitive low-end and dynamics vs the reference library.";
        demo.percentile_by_metric["lufs_integrated"] = 72.0f;
        demo.percentile_by_metric["lra"] = 64.0f;
        demo.percentile_by_metric["spectral_tilt_db_per_octave"] = 55.0f;
        demo.percentile_by_metric["crest_factor_db"] = 78.0f;
        benchmark_ = demo;
    } else {
        // Real analysis, no reference-library entry for the genre -> honest
        // empty state (paint() draws "No benchmark yet"), never fake numbers.
        benchmark_.reset();
    }
    const juce::String base = benchmark_ ? juce::String(benchmark_->competitive_summary) : juce::String{};
    summaryLabel_.setText(base, juce::dontSendNotification);
    repaint();

    // Nearest-reference retrieval (pipeline #8): append the closest professional
    // exemplar's guidance ("2 LU louder, 3 dB less dynamic than your nearest
    // reference") to the summary — the concrete-target counterpart to the
    // percentile bars. Best-effort; appended into the existing label (no layout
    // change). Whole-library search (genre_id empty) so it works off any track.
    const juce::String target = controller_.activeSourcePath();
    if (target.isEmpty()) return;
    juce::Component::SafePointer<BenchmarkPanel> self(this);
    controller_.runInBackground([self, &work = controller_, target, base]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeReferenceMatch(target.toStdString(), "", 1, true);
        postIfCurrent(self, work, [result, target, base](BenchmarkPanel& panel) {
            if (result.is_err()) return;
            if (panel.controller_.activeSourcePath() != target)
                return;
            const juce::var v = result.value();
            const juce::String guidance = v[af::ReferenceMatchResponse::guidance].toString();
            if (guidance.isEmpty()) return;
            juce::String txt = base;
            if (txt.isNotEmpty()) txt += "\n\n";
            txt += juce::String(juce::CharPointer_UTF8("Nearest reference \xe2\x80\x94 ")) + guidance;
            const juce::String sectionSummary = mostDivergentSectionSummary(v);
            if (sectionSummary.isNotEmpty()) txt += "\n\n" + sectionSummary;
            if ((bool)v[af::ReferenceMatchResponse::is_bootstrap_data])
                txt += "  (directional: synthetic library)";
            panel.summaryLabel_.setText(txt, juce::dontSendNotification);
        });
    });
}

void BenchmarkPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea();

    if (!benchmark_) {
        drawEmptyState(g, "No benchmark yet", "No reference-library entry for the detected genre yet.");
        return;
    }

    g.setFont(KhensuTheme::labelFont(9.5f));
    g.setColour(KhensuTheme::textTertiary);
    juce::String header =
        "vs. " + juce::String(benchmark_->genre_id) + " (" + juce::String(benchmark_->sample_size) + " refs)";
    if (benchmark_->is_bootstrap_data) header += "  - bootstrap/synthetic data";
    g.drawText(header, b.removeFromTop(14), juce::Justification::centredLeft, false);
    b.removeFromTop(6);

    const int barsBottom = b.getHeight() - 44;
    auto barsArea = b.removeFromTop(juce::jmax(0, barsBottom));

    const int n = 4;
    const int rowH = juce::jmin(28, barsArea.getHeight() / n);
    for (int i = 0; i < n; ++i) {
        auto row = barsArea.removeFromTop(rowH);
        g.setFont(KhensuTheme::bodyFont(10.0f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(kMetricLabels[i], row.removeFromLeft(100), juce::Justification::centredLeft, false);

        auto it = benchmark_->percentile_by_metric.find(kMetricKeys[i]);
        const float pct = it != benchmark_->percentile_by_metric.end() ? it->second : 50.0f;

        auto barTrack = row.reduced(4, 6);
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(barTrack.toFloat(), 3.0f);

        const float t = juce::jlimit(0.0f, 1.0f, pct / 100.0f);
        auto fill = barTrack.withWidth((int)(barTrack.getWidth() * t));
        const juce::Colour fillColour =
            (pct < 20.0f || pct > 80.0f) ? KhensuTheme::accentAmber : KhensuTheme::accentTeal;
        g.setColour(fillColour);
        g.fillRoundedRectangle(fill.toFloat(), 3.0f);

        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(juce::String((int)std::round(pct)) + "th pct", barTrack, juce::Justification::centredRight, false);
    }
}

void BenchmarkPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    b.removeFromTop(20 + 6);                            // header row
    b.removeFromTop(juce::jmax(0, b.getHeight() - 44)); // bars area (paint-driven)
    summaryLabel_.setBounds(b);
}

} // namespace khensu
