// desktop-app/Source/ui/Dashboard/PhaseHeatmapPanel.cpp
#include "PhaseHeatmapPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <cmath>
#include <limits>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

static juce::Colour corrColour(float v)
{
    // A cell with no data (JSON null -> NaN: silence, no energy in the band)
    // is a neutral grey, NOT 0.0 - zero correlation is a real, decorrelated
    // reading and would read as "dark = decorrelated" here.
    if (!std::isfinite(v)) return KhensuTheme::textTertiary.withAlpha(0.25f);
    // Diverging: red (anti-phase, -1) -> dark (0) -> teal (correlated, +1).
    v = juce::jlimit(-1.0f, 1.0f, v);
    if (v < 0.0f) return KhensuTheme::bgVoid.interpolatedWith(KhensuTheme::accentRed, -v);
    return KhensuTheme::bgVoid.interpolatedWith(KhensuTheme::accentTeal, v);
}

// Every cached palette colour on this component, in one place (UI-01).
void PhaseHeatmapPanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

PhaseHeatmapPanel::PhaseHeatmapPanel(IAppController& c) : PanelCard(c, "Phase Correlation Heatmap")
{
    applyThemeColours();
    setExplainer(TRANS("Phase Correlation Heatmap"),
                 TRANS("L/R correlation across the whole spectrum over time: teal = mono-safe "
                 "(+1), red = anti-phase and will cancel when summed to mono (-1), dark = "
                 "decorrelated (0). Shows exactly where (frequency) and when (time) the mix "
                 "goes out of phase - the mono-compatibility problems a single "
                 "correlation number hides."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void PhaseHeatmapPanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    fetchThenPublish(
        *this, controller_, run_, TRANS("Computing phase map..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("file_path", path);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/phase_heatmap", juce::var(body.get()));
        },
        [](PhaseHeatmapPanel& self, const juce::var& v) -> juce::String {
            self.stereo_ = (bool)v[af::PhaseHeatmapResponse::is_stereo];
            std::vector<std::vector<float>> grid;
            if (auto* rows = v[af::PhaseHeatmapResponse::grid].getArray())
                for (const auto& row : *rows) {
                    std::vector<float> r;
                    if (auto* cells = row.getArray())
                        for (const auto& c : *cells)
                            r.push_back(c.isVoid() ? std::numeric_limits<float>::quiet_NaN()
                                                   : (float)(double)c);
                    grid.push_back(std::move(r));
                }
            self.img_ = buildGridImage(grid, corrColour);
            return self.stereo_ ? TRANS("Teal = mono-safe, red = anti-phase, grey = no data.")
                                : TRANS("Mono file - perfectly correlated.");
        },
        std::move(options));
}

void PhaseHeatmapPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No phase map yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(b);
    if (!stereo_ || !img_.isValid()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.drawText("Mono source - no phase relationship to show.", b, juce::Justification::centred, false);
        return;
    }
    g.drawImage(img_, b.toFloat(), juce::RectanglePlacement::stretchToFit);
    // Freq axis hint: low at bottom, high at top.
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.drawText("20k", b.getX() + 2, b.getY() + 1, 30, 10, juce::Justification::centredLeft, false);
    g.drawText("20", b.getX() + 2, b.getBottom() - 11, 30, 10, juce::Justification::centredLeft, false);
}

void PhaseHeatmapPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
