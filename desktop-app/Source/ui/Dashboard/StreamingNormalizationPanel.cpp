// desktop-app/Source/ui/Dashboard/StreamingNormalizationPanel.cpp
#include "StreamingNormalizationPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void StreamingNormalizationPanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

StreamingNormalizationPanel::StreamingNormalizationPanel(IAppController& c) : PanelCard(c, "Streaming Loudness Preview")
{
    applyThemeColours();
    setExplainer(TRANS("Streaming Loudness Preview"),
                 TRANS("How each streaming platform will actually play this master. Every service "
                 "normalizes to its own target loudness, turning loud masters DOWN (and, on "
                 "some, quiet ones UP through a limiter), so the master's own LUFS is not how "
                 "it will sound. Each row shows the gain the platform applies, the resulting "
                 "loudness, and the true-peak headroom left below its ceiling - a red "
                 "headroom means it will clip after their processing."),
                 "translation_preview");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void StreamingNormalizationPanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Preview failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Computing platform playback..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("file_path", path);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/streaming_normalization", juce::var(body.get()));
        },
        [](StreamingNormalizationPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("streaming_normalization", v); // WF-14
            // `measurable` is absent from older services: treat absent as true.
            const juce::var measurable = v[af::StreamingNormalizationResponse::measurable];
            self.measurable_ = measurable.isVoid() || (bool)measurable;
            self.note_ = v[af::StreamingNormalizationResponse::note].toString();
            self.integratedLufs_ = (float)(double)v[af::StreamingNormalizationResponse::integrated_lufs];
            self.truePeak_ = (float)(double)v[af::StreamingNormalizationResponse::true_peak_dbtp];
            self.rows_.clear();
            if (auto* arr = v[af::StreamingNormalizationResponse::platforms].getArray())
                for (const auto& p : *arr) {
                    Row r;
                    r.name = p[af::StreamingPlatformResult::name].toString();
                    r.gain = (float)(double)p[af::StreamingPlatformResult::applied_gain_db];
                    r.lufs = (float)(double)p[af::StreamingPlatformResult::resulting_lufs];
                    r.headroom = (float)(double)p[af::StreamingPlatformResult::headroom_db];
                    r.limiter = (bool)p[af::StreamingPlatformResult::limiter_engaged];
                    self.rows_.push_back(std::move(r));
                }
            if (!self.measurable_) {
                // No per-platform gains for an unmeasurable file: the note says why.
                self.rows_.clear();
                return self.note_.isNotEmpty() ? self.note_ : TRANS("Loudness could not be measured.");
            }
            return TRANS("Measured") + " " + juce::String(self.integratedLufs_, 1) + " LUFS / " +
                   juce::String(self.truePeak_, 1) + " dBTP." +
                   (self.note_.isNotEmpty() ? " " + self.note_ : juce::String());
        },
        std::move(options));
}

void StreamingNormalizationPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No platform preview yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    if (!measurable_) {
        drawEmptyState(g, TRANS("No platform gains"),
                       note_.isNotEmpty() ? note_ : TRANS("Loudness could not be measured."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);

    auto header = b.removeFromTop(16);
    g.setFont(KhensuTheme::labelFont(9.0f).boldened());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("PLATFORM", header.removeFromLeft(120), juce::Justification::centredLeft, false);
    g.drawText("GAIN", header.removeFromLeft(64), juce::Justification::centredRight, false);
    g.drawText("LUFS", header.removeFromLeft(58), juce::Justification::centredRight, false);
    g.drawText("HEADROOM", header, juce::Justification::centredRight, false);
    b.removeFromTop(2);

    const int rowH = juce::jmax(15, b.getHeight() / (int)juce::jmax((size_t)1, rows_.size()));
    for (const auto& r : rows_) {
        auto row = b.removeFromTop(rowH);
        g.setFont(KhensuTheme::labelFont(10.0f));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(r.name, row.removeFromLeft(120), juce::Justification::centredLeft, false);
        g.setFont(KhensuTheme::monoFont(9.5f));
        g.setColour(r.gain < -0.05f  ? KhensuTheme::accentAmber
                    : r.gain > 0.05f ? KhensuTheme::accentTeal
                                     : KhensuTheme::textTertiary);
        g.drawText((r.gain >= 0 ? "+" : "") + juce::String(r.gain, 1), row.removeFromLeft(64),
                   juce::Justification::centredRight, false);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::String(r.lufs, 1), row.removeFromLeft(58), juce::Justification::centredRight, false);
        const bool bad = r.headroom < 0.0f || r.limiter;
        g.setColour(bad ? KhensuTheme::accentRed : KhensuTheme::accentGreen);
        g.drawText((r.limiter ? juce::String("LIM ") : juce::String()) + (r.headroom >= 0 ? "+" : "") +
                       juce::String(r.headroom, 1) + " dB",
                   row, juce::Justification::centredRight, false);
    }
}

void StreamingNormalizationPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(160));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
