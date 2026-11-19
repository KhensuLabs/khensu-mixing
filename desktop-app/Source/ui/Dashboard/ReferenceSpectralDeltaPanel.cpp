// desktop-app/Source/ui/Dashboard/ReferenceSpectralDeltaPanel.cpp
#include "ReferenceSpectralDeltaPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

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
    loadButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    loadButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
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
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.drawText("20k", b.getX() + 2, b.getY() + 1, 30, 10, juce::Justification::centredLeft, false);
    g.drawText("20", b.getX() + 2, b.getBottom() - 11, 30, 10, juce::Justification::centredLeft, false);
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
