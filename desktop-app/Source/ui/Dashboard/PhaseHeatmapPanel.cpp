// desktop-app/Source/ui/Dashboard/PhaseHeatmapPanel.cpp
#include <functional>
#include <limits>
#include "PhaseHeatmapPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include "../shared/VizMaps.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <cmath>
#include <limits>

namespace khensu {

namespace {
std::vector<std::vector<float>> gridOf(const juce::var& rows)
{
    std::vector<std::vector<float>> grid;
    if (auto* arr = rows.getArray())
        for (const auto& row : *arr) {
            std::vector<float> r;
            if (auto* cells = row.getArray())
                for (const auto& c : *cells)
                    r.push_back(c.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float)(double)c);
            grid.push_back(std::move(r));
        }
    return grid;
}

std::vector<float> floatsOf(const juce::var& v)
{
    std::vector<float> out;
    if (auto* arr = v.getArray())
        for (const auto& x : *arr)
            out.push_back((float)(double)x);
    return out;
}
} // namespace


namespace af = api::fields; // generated service field names (ApiSchema.h)

static juce::Colour corrColour(float v)
{
    // A cell with no data (JSON null -> NaN: silence, no energy in the band)
    // is a neutral grey, NOT 0.0 - zero correlation is a real, decorrelated
    // reading and would read as "dark = decorrelated" here.
    if (!std::isfinite(v)) return KhensuTheme::textTertiary.withAlpha(0.25f);
    // VIS-2: the one diverging map - blue (anti-phase, -1) -> neutral (0)
    // -> orange (correlated, +1).
    return ui::viz::themedDiverging(juce::jlimit(-1.0f, 1.0f, v));
}

// Every cached palette colour on this component, in one place (UI-01).
void PhaseHeatmapPanel::applyThemeColours()
{
}

PhaseHeatmapPanel::PhaseHeatmapPanel(IAppController& c) : PanelCard(c, "Phase Correlation Heatmap")
{
    applyThemeColours();
    setExplainer(TRANS("Phase Correlation Heatmap"),
                 TRANS("L/R correlation across the whole spectrum over time: orange = mono-safe "
                 "(+1), blue = anti-phase and will cancel when summed to mono (-1), grey = "
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
            self.edgesHz_ = floatsOf(v[af::PhaseHeatmapResponse::band_edges_hz]);
            self.img_ = buildGridImageOnAxis(grid, self.edgesHz_, corrColour);
            self.grid_ = grid;
            self.durationSec_ = (float)(double)v[af::PhaseHeatmapResponse::duration_sec];
            return self.stereo_ ? TRANS("Orange = mono-safe, blue = anti-phase, faint = no data.")
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
    if (!stereo_ || !img_.isValid()) {
        drawEmptyState(g, TRANS("Mono source - no phase relationship to show."), {}, 28); // UXR-6
        return;
    }
    // VIS-2 / VIS-3: the standard frequency axis on the left, the colour bar on the right.
    const int labelW = 30;
    auto legend = b.removeFromRight(static_cast<int>(ui::viz::kColourBarW)).toFloat();
    b.removeFromLeft(labelW);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(b);
    g.drawImage(img_, b.toFloat(), juce::RectanglePlacement::stretchToFit);
    gridArea_ = b.toFloat();
    ui::viz::paintFrequencyTicks(g, gridArea_, static_cast<float>(labelW), true);
    ui::viz::paintDivergingBar(g, legend.withTrimmedLeft(6.0f).withTrimmedTop(4.0f).withTrimmedBottom(4.0f), "+1", "0",
                               "-1");
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

void PhaseHeatmapPanel::mouseMove(const juce::MouseEvent& e)
{
    setTooltip(gridReadoutOnAxis(grid_, edgesHz_, durationSec_, gridArea_, e.position, [](float v) {
        return TRANS("correlation") + " " + juce::String(v, 2)
               + (v < 0.0f ? " - " + TRANS("cancels in mono") : v > 0.7f ? " - " + TRANS("mono-safe") : juce::String());
    }));
}

} // namespace khensu
