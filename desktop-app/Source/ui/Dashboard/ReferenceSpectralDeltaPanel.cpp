// desktop-app/Source/ui/Dashboard/ReferenceSpectralDeltaPanel.cpp
#include <functional>
#include <limits>
#include "ReferenceSpectralDeltaPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace {
/// VIZ-U3: the value under `pos` in a [frame][band] grid drawn over `area`
/// (band 0 at the bottom), with its time and frequency.
juce::String gridReadout(const std::vector<std::vector<float>>& grid, const std::vector<float>& edgesHz, float durationSec,
                         juce::Rectangle<float> area, juce::Point<float> pos, const std::function<juce::String(float)>& value)
{
    if (grid.empty() || grid[0].empty() || !area.contains(pos)) return {};
    const auto f = (size_t)juce::jlimit(0, (int)grid.size() - 1, (int)((pos.x - area.getX()) / area.getWidth() * (float)grid.size()));
    const int bands = (int)grid[f].size();
    if (bands == 0) return {};
    const auto b = (size_t)juce::jlimit(0, bands - 1, (int)((area.getBottom() - pos.y) / area.getHeight() * (float)bands));
    juce::String where;
    if (durationSec > 0.0f) {
        const int t = juce::roundToInt(durationSec * ((float)f + 0.5f) / (float)grid.size());
        where << t / 60 << ":" << juce::String(t % 60).paddedLeft('0', 2);
    }
    if (b + 1 < edgesHz.size()) {
        const float hz = std::sqrt(edgesHz[b] * edgesHz[b + 1]);
        where << (where.isEmpty() ? "" : ", ")
              << (hz >= 1000.0f ? juce::String(hz / 1000.0f, 1) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz");
    }
    const float v = grid[f][b];
    return where + ": " + (std::isfinite(v) ? value(v) : TRANS("no data"));
}

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
    // Diverging around 0 dB: amber = target is HOT vs ref (+), blue = SHY (-).
    const float v = juce::jlimit(-12.0f, 12.0f, db) / 12.0f;
    if (v > 0.0f) return KhensuTheme::bgVoid.interpolatedWith(KhensuTheme::accentAmber, v);
    return KhensuTheme::bgVoid.interpolatedWith(KhensuTheme::accentBlue, -v);
}

// Every cached palette colour on this component, in one place (UI-01).
void ReferenceSpectralDeltaPanel::applyThemeColours()
{
}

ReferenceSpectralDeltaPanel::ReferenceSpectralDeltaPanel(IAppController& c) : PanelCard(c, "Reference Spectral Delta")
{
    applyThemeColours();
    setExplainer(TRANS("Reference Spectral Delta"), TRANS("How your mix's tonal balance deviates from a reference across frequency "
                                             "AND time - amber where your mix is hotter than the reference, blue "
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
            self.img_ = buildGridImage(grid, deltaColour);
            self.grid_ = grid;
            self.edgesHz_ = floatsOf(v[af::ReferenceSpectralDeltaResponse::band_edges_hz]);
            self.durationSec_ = (float)(double)v[af::ReferenceSpectralDeltaResponse::duration_sec];
            return TRANS("Amber = hot vs ref, blue = shy.");
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
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(b);
    g.drawImage(img_, b.toFloat(), juce::RectanglePlacement::stretchToFit);
    gridArea_ = b.toFloat();
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.drawText("20k", b.getX() + 2, b.getY() + 1, 30, 10, juce::Justification::centredLeft, false);
    g.drawText("20", b.getX() + 2, b.getBottom() - 11, 30, 10, juce::Justification::centredLeft, false);
}

void ReferenceSpectralDeltaPanel::mouseMove(const juce::MouseEvent& e)
{
    setTooltip(gridReadout(grid_, edgesHz_, durationSec_, gridArea_, e.position, [](float v) {
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
