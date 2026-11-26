// desktop-app/Source/ui/Dashboard/TuningReferencePanel.cpp
#include "TuningReferencePanel.h"
#include "../VocalWorkspace/MixTuningReference.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void TuningReferencePanel::applyThemeColours()
{
}

TuningReferencePanel::TuningReferencePanel(IAppController& c) : PanelCard(c, "Tuning Reference")
{
    applyThemeColours();
    setExplainer(TRANS("Tuning Reference"), TRANS("The track's concert-pitch reference: A=440 (standard), A=432, or N cents "
                                     "off, plus how much the tuning drifts across the song. A large drift is a "
                                     "red flag for a stitched-together or varispeed source. The strip plots the "
                                     "per-section tuning deviation over time."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void TuningReferencePanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Detection failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Estimating tuning..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("file_path", path);
            auto result = ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/tuning_reference", juce::var(body.get()));
            // Share the measurement with the intonation panels (AUDIT-016) so
            // they don't re-run it for this session.
            if (result.is_ok()) MixTuningReference::instance().store(path, session_epoch::current(), result.value());
            return result;
        },
        [](TuningReferencePanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("tuning_reference", v); // WF-14
            self.refHz_ = (float)(double)v[af::TuningReferenceResponse::reference_hz];
            self.cents_ = (float)(double)v[af::TuningReferenceResponse::cents_from_440];
            self.driftRange_ = (float)(double)v[af::TuningReferenceResponse::drift_range_cents];
            self.drifting_ = (bool)v[af::TuningReferenceResponse::drifting];
            self.verdict_ = v[af::TuningReferenceResponse::verdict].toString();
            self.drift_.clear();
            if (auto* arr = v[af::TuningReferenceResponse::drift].getArray())
                for (const auto& d : *arr)
                    self.drift_.push_back({(float)(double)d[af::TuningDriftPoint::time_sec], (float)(double)d[af::TuningDriftPoint::cents]});
            if (self.onReferenceDetected && self.refHz_ > 0.0f) self.onReferenceDetected(self.refHz_);
            return TRANS("A=") + juce::String(self.refHz_, 1) + " Hz";
        },
        std::move(options));
}

void TuningReferencePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No tuning data yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);

    g.setFont(KhensuTheme::metricFont(type::heading));
    g.setColour(drifting_ ? KhensuTheme::accentAmber : KhensuTheme::accentTeal);
    g.drawText("A=" + juce::String(refHz_, 1) + " Hz", b.removeFromTop(26), juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::bodyFont(type::caption));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(verdict_, b.removeFromTop(16), juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(drifting_ ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
    g.drawText("Drift: " + juce::String(juce::roundToInt(driftRange_)) + " cents" +
                   (drifting_ ? juce::String(juce::CharPointer_UTF8("  \xe2\x9a\xa0 unstable")) : juce::String()),
               b.removeFromTop(14), juce::Justification::centredLeft, false);
    b.removeFromTop(4);

    // Drift strip: cents (-50..50) over time.
    if (drift_.size() >= 2) {
        auto strip = b;
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.4f));
        g.fillRoundedRectangle(strip.toFloat(), 4.0f);
        const float midY = static_cast<float>(strip.getCentreY());
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.3f));
        g.drawHorizontalLine(juce::roundToInt(midY), (float)strip.getX(), (float)strip.getRight());
        const float tMax = drift_.back().x > 0 ? drift_.back().x : 1.0f;
        juce::Path p;
        hover_.clear();
        for (size_t i = 0; i < drift_.size(); ++i) {
            const float x = strip.getX() + juce::jlimit(0.0f, 1.0f, drift_[i].x / tMax) * strip.getWidth();
            {
                const float w = strip.getWidth() / (float)std::max<size_t>(1, drift_.size() - 1);
                hover_.addColumn(x - w * 0.5f, x + w * 0.5f, strip.toFloat(),
                                 juce::String(drift_[i].x, 1) + " s: " + (drift_[i].y >= 0.0f ? "+" : "")
                                     + juce::String(drift_[i].y, 1) + " " + TRANS("cents from") + " "
                                     + juce::String(refHz_, 1) + " Hz");
            }
            const float y = midY - juce::jlimit(-50.0f, 50.0f, drift_[i].y) / 50.0f * (strip.getHeight() * 0.5f);
            if (i == 0)
                p.startNewSubPath(x, y);
            else
                p.lineTo(x, y);
        }
        g.setColour(KhensuTheme::accentTeal);
        g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
    }
}

void TuningReferencePanel::resized()
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
