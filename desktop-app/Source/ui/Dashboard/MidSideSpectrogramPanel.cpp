// desktop-app/Source/ui/Dashboard/MidSideSpectrogramPanel.cpp
#include "MidSideSpectrogramPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include "../shared/HeatmapImage.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

static juce::Colour magColour(float v)
{
    // Dark -> teal -> white ramp for magnitude 0..1.
    v = juce::jlimit(0.0f, 1.0f, v);
    return v < 0.7f ? KhensuTheme::bgVoid.interpolatedWith(KhensuTheme::accentTeal, v / 0.7f)
                    : KhensuTheme::accentTeal.interpolatedWith(juce::Colours::white, (v - 0.7f) / 0.3f); // theme-exempt(perceptual heat ramp - peak magnitude is white in every theme)
}

// Every cached palette colour on this component, in one place (UI-01).
void MidSideSpectrogramPanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
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
            self.midImg_ = buildGridImage(toGrid(v[af::MidSideSpectrogramResponse::mid_frames]), magColour);
            self.sideImg_ = buildGridImage(toGrid(v[af::MidSideSpectrogramResponse::side_frames]), magColour);
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
    const int gap = 14;
    const int half = (b.getHeight() - gap) / 2;

    auto drawOne = [&](juce::Rectangle<int> area, const juce::Image& img, const juce::String& label) {
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
        g.fillRect(area);
        if (img.isValid()) g.drawImage(img, area.toFloat(), juce::RectanglePlacement::stretchToFit);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(9.0f).boldened());
        g.drawText(label, area.getX() + 3, area.getY() + 2, 60, 12, juce::Justification::centredLeft, false);
    };
    drawOne(b.removeFromTop(half), midImg_, "MID");
    b.removeFromTop(gap);
    drawOne(b, sideImg_, "SIDE");
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
