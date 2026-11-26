// desktop-app/Source/ui/Dashboard/MidSideSpectrogramPanel.cpp
#include <functional>
#include <limits>
#include <cmath>
#include "MidSideSpectrogramPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include "../shared/VizMaps.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

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

static juce::Colour magColour(float v)
{
    // VIS-2: the one spectrogram map (magma) for magnitude 0..1.
    return ui::viz::spectrogram(std::isfinite(v) ? v : 0.0f);
}

// Every cached palette colour on this component, in one place (UI-01).
void MidSideSpectrogramPanel::applyThemeColours()
{
}

MidSideSpectrogramPanel::MidSideSpectrogramPanel(IAppController& c) : PanelCard(c, "Mid/Side Spectrogram")
{
    applyThemeColours();
    setExplainer(TRANS("Mid/Side Spectrogram"), TRANS("Two time x frequency views: the center (MID = L+R) and the sides "
                                         "(SIDE = L-R). The mastering staple for spotting a lopsided image - "
                                         "where the stereo width lives across frequency and time, and whether the "
                                         "center is carrying the low end. Both share one brightness scale, so a "
                                         "near-silent channel stays dark."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void MidSideSpectrogramPanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    fetchThenPublish(
        *this, controller_, run_, TRANS("Computing mid/side..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("file_path", path);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/mid_side_spectrogram", juce::var(body.get()));
        },
        [](MidSideSpectrogramPanel& self, const juce::var& v) -> juce::String {
            auto toGrid = [](const juce::var& arr) {
                std::vector<std::vector<float>> g;
                if (auto* rows = arr.getArray())
                    for (const auto& row : *rows) {
                        std::vector<float> r;
                        if (auto* cells = row.getArray())
                            for (const auto& c : *cells)
                                r.push_back((float)(double)c);
                        g.push_back(std::move(r));
                    }
                return g;
            };
            self.edgesHz_ = floatsOf(v[af::MidSideSpectrogramResponse::band_edges_hz]);
            self.midImg_ = buildGridImageOnAxis(toGrid(v[af::MidSideSpectrogramResponse::mid_frames]), self.edgesHz_, magColour);
            self.sideImg_ = buildGridImageOnAxis(toGrid(v[af::MidSideSpectrogramResponse::side_frames]), self.edgesHz_, magColour);
            self.grid_ = gridOf(v[af::MidSideSpectrogramResponse::mid_frames]);
            self.grid2_ = gridOf(v[af::MidSideSpectrogramResponse::side_frames]);
            self.durationSec_ = (float)(double)v[af::MidSideSpectrogramResponse::duration_sec];
            return TRANS("Mid (center) vs Side (width).");
        },
        std::move(options));
}

void MidSideSpectrogramPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No mid/side view yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    // VIS-2 / VIS-3: both views on the one frequency axis, with one colour bar.
    const int labelW = 30;
    auto legend = b.removeFromRight(static_cast<int>(ui::viz::kColourBarW)).toFloat();
    b.removeFromLeft(labelW);
    const int gap = 14;
    const int half = (b.getHeight() - gap) / 2;

    auto drawOne = [&](juce::Rectangle<int> area, const juce::Image& img, const juce::String& label) {
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
        g.fillRect(area);
        if (img.isValid()) g.drawImage(img, area.toFloat(), juce::RectanglePlacement::stretchToFit);
        ui::viz::paintFrequencyTicks(g, area.toFloat(), static_cast<float>(labelW), false);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel).boldened());
        g.drawText(label, area.getX() + 4, area.getY() + 2, 60, 13, juce::Justification::centredLeft, false);
    };
    gridArea_ = b.withHeight(half).toFloat();
    drawOne(b.removeFromTop(half), midImg_, TRANS("Mid"));
    b.removeFromTop(gap);
    grid2Area_ = b.toFloat();
    drawOne(b, sideImg_, TRANS("Side"));
    ui::viz::paintSpectrogramBar(g, legend.withTrimmedLeft(6.0f).withTrimmedTop(4.0f).withTrimmedBottom(4.0f),
                                 TRANS("loud"), TRANS("quiet"));
}

void MidSideSpectrogramPanel::mouseMove(const juce::MouseEvent& e)
{
    const bool side = grid2Area_.contains(e.position);
    setTooltip(gridReadoutOnAxis(side ? grid2_ : grid_, edgesHz_, durationSec_, side ? grid2Area_ : gridArea_, e.position,
                           [side](float v) {
                               return (side ? TRANS("side") : TRANS("mid")) + " " + juce::String(juce::roundToInt(v * 100.0f))
                                      + " % " + TRANS("of the loudest");
                           }));
}

void MidSideSpectrogramPanel::resized()
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
