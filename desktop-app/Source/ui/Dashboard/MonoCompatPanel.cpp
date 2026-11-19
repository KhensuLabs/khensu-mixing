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
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

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
            return self.stereo_ ? TRANS("Per-system fold-down score.") : TRANS("Mono file - fully compatible.");
        },
        std::move(options));
}

void MonoCompatPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No mono check yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    const int rowH = juce::jmax(24, b.getHeight() / (int)juce::jmax((size_t)1, systems_.size()));
    for (const auto& s : systems_) {
        auto row = b.removeFromTop(rowH);
        auto labelRow = row.removeFromTop(14);
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(s.name, labelRow.removeFromLeft(150), juce::Justification::centredLeft, false);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::monoFont(9.5f));
        g.drawText(juce::String(s.score) +
                       (s.score < 70 && s.worst.isNotEmpty() ? "  worst " + s.worst : juce::String()),
                   labelRow, juce::Justification::centredRight, false);
        // Score bar.
        auto bar = row.removeFromTop(8).reduced(0, 1);
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
        g.fillRoundedRectangle(bar.toFloat(), 2.0f);
        const juce::Colour c = s.score >= 80   ? KhensuTheme::accentGreen
                               : s.score >= 55 ? KhensuTheme::accentAmber
                                               : KhensuTheme::accentRed;
        g.setColour(c);
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * s.score / 100).toFloat(), 2.0f);
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
}

} // namespace khensu
