// desktop-app/Source/ui/Dashboard/MonoCompatPanel.cpp
#include "MonoCompatPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void MonoCompatPanel::applyThemeColours()
{
}

namespace {
/// VIS-4: the score's verdict in words - the bar is one data colour.
juce::String scoreWord(int score)
{
    return score >= 80 ? TRANS("good") : score >= 55 ? TRANS("fair") : TRANS("poor");
}
} // namespace

MonoCompatPanel::MonoCompatPanel(IAppController& c) : PanelCard(c, "Mono Compatibility")
{
    applyThemeColours();
    setExplainer(TRANS("Mono Compatibility"),
                 TRANS("How the mix survives on systems that sum to mono. A club PA and a vinyl "
                 "cutter fold the low end; a phone or Bluetooth speaker folds the whole "
                 "signal. Anti-phase content cancels when summed, so each system gets a 0-100 "
                 "score with the worst offending band - low scores mean the low end "
                 "(or more) disappears on that system."),
                 "mono_compatibility_analysis");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    chart_.setTitle(TRANS("Mono fold-down score"));
    chart_.setHorizontal(true);
    chart_.x.lo = 0.0;
    chart_.x.hi = 100.0;
    chart_.setXRange(0.0, 100.0);
    chart_.setBaseline(0.0);
    chart_.valueText = [this](size_t i, size_t, double v) {
        const int score = juce::roundToInt(v);
        auto text = juce::String(score) + " - " + scoreWord(score);
        if (i < systems_.size() && score < 70 && systems_[i].worst.isNotEmpty())
            text << ", " << TRANS("worst") << " " << systems_[i].worst;
        return text;
    };
    addChildComponent(chart_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void MonoCompatPanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Check failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Folding to mono..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty(af::MonoCompatRequest::file_path, path);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/mono_compat", juce::var(body.get()));
        },
        [](MonoCompatPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("mono_compat", v); // WF-14
            self.stereo_ = (bool)v[af::MonoCompatResponse::is_stereo];
            self.systems_.clear();
            if (auto* arr = v[af::MonoCompatResponse::systems].getArray())
                for (const auto& sy : *arr) {
                    Sys s2;
                    s2.name = sy["name"].toString();
                    s2.score = (int)sy["score"];
                    if (auto* wb = sy["worst_band_hz"].getArray())
                        if (wb->size() >= 2)
                            s2.worst =
                                juce::String((int)(double)(*wb)[0]) + "-" + juce::String((int)(double)(*wb)[1]) + " Hz";
                    self.systems_.push_back(std::move(s2));
                }
            self.rebuildChart();
            return self.stereo_ ? TRANS("Per-system fold-down score.") : TRANS("Mono file - fully compatible.");
        },
        std::move(options));
}

void MonoCompatPanel::rebuildChart()
{
    // Visibility follows the run state in resized() (called after every outcome).
    chart_.setVisible(run_.showsResult() && !systems_.empty());
    std::vector<ui::chart::Bar> bars;
    juce::StringArray parts;
    for (const auto& s : systems_) {
        ui::chart::Bar b;
        b.label = s.name;
        b.values = {static_cast<double>(s.score)};
        bars.push_back(b);
        parts.add(s.name + " " + juce::String(s.score) + " (" + scoreWord(s.score) +
                  (s.score < 70 && s.worst.isNotEmpty() ? ", " + TRANS("worst") + " " + s.worst : juce::String()) + ")");
    }
    chart_.setBars(std::move(bars), {{TRANS("Mono score"), KhensuTheme::vizCategory(0)}});
    chart_.setSummary(TRANS("Mono fold-down score per playback system, out of 100") + ": " + parts.joinIntoString(", ") + ".");
}

void MonoCompatPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No mono check yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    if (systems_.empty()) { // a mono file: nothing folds down
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont());
        g.drawText(TRANS("Mono file - fully compatible."), bodyArea().withTrimmedTop(28), juce::Justification::centred, true);
    }
}

void MonoCompatPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(130));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
    chart_.setBounds(errArea);
    chart_.setVisible(run_.showsResult() && !systems_.empty());
}

} // namespace khensu
