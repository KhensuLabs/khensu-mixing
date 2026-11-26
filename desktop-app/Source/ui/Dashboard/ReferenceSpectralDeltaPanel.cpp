// desktop-app/Source/ui/Dashboard/ReferenceSpectralDeltaPanel.cpp
#include <functional>
#include <limits>
#include <cmath>
#include "ReferenceSpectralDeltaPanel.h"
#include "../../IAppController.h"
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

static juce::Colour deltaColour(float db)
{
    // VIS-2: the one diverging map around 0 dB - orange = the mix is hotter
    // than the reference (+), blue = shy (-); +-12 dB is the full colour.
    if (!std::isfinite(db)) return juce::Colours::transparentBlack;
    return ui::viz::themedDiverging(juce::jlimit(-12.0f, 12.0f, db) / 12.0f);
}

// Every cached palette colour on this component, in one place (UI-01).
void ReferenceSpectralDeltaPanel::applyThemeColours()
{
}

ReferenceSpectralDeltaPanel::ReferenceSpectralDeltaPanel(IAppController& c) : PanelCard(c, "Reference Spectral Delta")
{
    applyThemeColours();
    setExplainer(TRANS("Reference Spectral Delta"), TRANS("How your mix's tonal balance deviates from a reference across frequency "
                                             "AND time - orange where your mix is hotter than the reference, blue "
                                             "where it's shy, at each moment. A dynamic view of the match, beyond the "
                                             "static match-EQ callout. Load (or reuse) a reference track to compare."));

    loadButton_.onClick = [this] {
        // Reuse an already-loaded reference if there is one; else pick a file.
        const juce::String existing = controller_.analysis().getReferenceTrack();
        if (existing.isNotEmpty() && juce::File(existing).existsAsFile()) {
            refPath_ = existing;
            runAnalysis();
            return;
        }
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Select a reference track", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav,*.aiff,*.flac,*.mp3,*.aac,*.ogg");
        fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      auto f = fc.getResult();
                                      if (f.existsAsFile()) {
                                          refPath_ = f.getFullPathName();
                                          controller_.analysis().setReferenceTrack(refPath_);
                                          runAnalysis();
                                      }
                                  });
    };
    addAndMakeVisible(loadButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Load a reference to compare over time."));
}

void ReferenceSpectralDeltaPanel::runAnalysis()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    if (refPath_.isEmpty()) refPath_ = controller_.analysis().getReferenceTrack();
    if (refPath_.isEmpty() || !juce::File(refPath_).existsAsFile()) {
        run_.setIdle(TRANS("Load a reference to compare."));
        return;
    }
    const juce::String target(s->files[0].path), ref(refPath_);
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Comparison failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Comparing to reference..."),
        [target, ref] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("target_path", target);
            body->setProperty("reference_path", ref);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/reference_spectral_delta", juce::var(body.get()));
        },
        [](ReferenceSpectralDeltaPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("reference_spectral_delta", v); // WF-14
            std::vector<std::vector<float>> grid;
            if (auto* rows = v[af::ReferenceSpectralDeltaResponse::grid].getArray())
                for (const auto& row : *rows) {
                    std::vector<float> r;
                    if (auto* cells = row.getArray())
                        for (const auto& c : *cells)
                            r.push_back((float)(double)c);
                    grid.push_back(std::move(r));
                }
            self.edgesHz_ = floatsOf(v[af::ReferenceSpectralDeltaResponse::band_edges_hz]);
            self.img_ = buildGridImageOnAxis(grid, self.edgesHz_, deltaColour);
            self.grid_ = grid;
            self.durationSec_ = (float)(double)v[af::ReferenceSpectralDeltaResponse::duration_sec];
            return TRANS("Orange = hot vs ref, blue = shy.");
        },
        std::move(options));
}

void ReferenceSpectralDeltaPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || !img_.isValid()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No reference comparison yet"),
                           TRANS("Load (or reuse) a reference track to compare over time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    // VIS-2 / VIS-3: the standard frequency axis on the left, the colour bar on the right.
    const int labelW = 30;
    auto legend = b.removeFromRight(static_cast<int>(ui::viz::kColourBarW)).toFloat();
    b.removeFromLeft(labelW);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(b);
    g.drawImage(img_, b.toFloat(), juce::RectanglePlacement::stretchToFit);
    gridArea_ = b.toFloat();
    ui::viz::paintFrequencyTicks(g, gridArea_, static_cast<float>(labelW), true);
    ui::viz::paintDivergingBar(g, legend.withTrimmedLeft(6.0f).withTrimmedTop(4.0f).withTrimmedBottom(4.0f),
                               "+12 dB", "0", "-12");
}

void ReferenceSpectralDeltaPanel::mouseMove(const juce::MouseEvent& e)
{
    setTooltip(gridReadoutOnAxis(grid_, edgesHz_, durationSec_, gridArea_, e.position, [](float v) {
        return (v >= 0.0f ? "+" : "") + juce::String(v, 1) + " dB " + TRANS("vs the reference");
    }));
}

void ReferenceSpectralDeltaPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    loadButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
