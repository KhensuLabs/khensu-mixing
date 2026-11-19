// desktop-app/Source/ui/Dashboard/SignalIntegrityPanel.cpp
#include "SignalIntegrityPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RecommendationExplainer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
/// Static portion of the Signal Integrity explainer - the general description
/// that explains what each sub-check means. Live measured values are prepended
/// at scan time via explainer::buildMetricsBlock().
static const juce::String kSignalIntegrityBase =
    "Three checks that peak/LUFS metering alone misses:\n\n"
    "TRUE PEAK - the reconstructed analog waveform can overshoot a full-scale "
    "sample peak between samples. A track reading -1 dBFS on a normal meter can "
    "still clip after MP3/AAC encoding if its true (inter-sample) peak is hot. "
    "Keep it at or below -1 dBTP.\n\n"
    "MONO BASS - low end that isn't correlated between L/R below ~120 Hz will "
    "partially cancel on club systems, vinyl, and phone speakers that sum to mono.\n\n"
    "NOISE FLOOR - 50/60 Hz mains hum or an elevated hiss floor, measured in the "
    "quietest stretch of the track so program material doesn't mask it.";
} // anonymous namespace

// Every cached palette colour on this component, in one place (UI-01).
void SignalIntegrityPanel::applyThemeColours()
{
    scanButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    scanButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

SignalIntegrityPanel::SignalIntegrityPanel(IAppController& c) : PanelCard(c, "Signal Integrity")
{
    applyThemeColours();
    setExplainer(TRANS("Signal Integrity"), kSignalIntegrityBase);

    scanButton_.onClick = [this] { runScan(); };
    addAndMakeVisible(scanButton_);
    run_.onRetry([this] { runScan(); });
    run_.setIdle(TRANS("Scan the source file for peak, mono, and noise-floor issues."));

    cards_[0].title = "TRUE PEAK";
    cards_[1].title = "MONO BASS";
    cards_[2].title = "NOISE FLOOR";
}

void SignalIntegrityPanel::runScan()
{
    const juce::String target = controller_.activeSourcePath();
    if (target.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Scanning..."),
        [target] { return ipc::HttpServiceBridge{}.analyzeSignalIntegrity(target.toStdString()); },
        [](SignalIntegrityPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("signal_integrity", v); // WF-14
            auto colourForRisk = [](bool risky, bool moderate) {
                return risky ? KhensuTheme::accentRed : moderate ? KhensuTheme::accentAmber : KhensuTheme::accentTeal;
            };

            const juce::var tp = v[af::SignalIntegrityResponse::true_peak];
            self.cards_[0].metric = juce::String((double)tp[af::TruePeakIntegrity::true_peak_dbtp], 2) + " dBTP";
            self.cards_[0].verdict = tp[af::TruePeakIntegrity::verdict].toString();
            self.cards_[0].colour = colourForRisk((bool)tp[af::TruePeakIntegrity::over_minus_1dbtp] && (double)tp[af::TruePeakIntegrity::true_peak_dbtp] > -0.3,
                                                  (bool)tp[af::TruePeakIntegrity::over_minus_1dbtp]);
            self.cards_[0].hasData = true;

            const juce::var mb = v[af::SignalIntegrityResponse::mono_bass];
            const double corr = (double)mb[af::MonoBassIntegrity::correlation_below_cutoff];
            self.cards_[1].metric = "corr " + juce::String(corr, 2);
            self.cards_[1].verdict = mb[af::MonoBassIntegrity::verdict].toString();
            self.cards_[1].colour = colourForRisk(corr < 0.3, corr < 0.7);
            self.cards_[1].hasData = true;

            const juce::var nf = v[af::SignalIntegrityResponse::noise_floor];
            const bool hum = (bool)nf[af::NoiseFloorIntegrity::hum_detected];
            self.cards_[2].metric = hum ? (juce::String((int)(double)nf[af::NoiseFloorIntegrity::hum_hz]) + " Hz hum")
                                        : (juce::String(juce::roundToInt((double)nf[af::NoiseFloorIntegrity::hiss_headroom_db])) + " dB headroom");
            self.cards_[2].verdict = nf[af::NoiseFloorIntegrity::verdict].toString();
            self.cards_[2].colour = colourForRisk(hum, (double)nf[af::NoiseFloorIntegrity::hiss_headroom_db] < 40.0);
            self.cards_[2].hasData = true;

            // §9.5 - update the '?' explainer with the actual measured values so
            // the popup shows live numbers alongside the general description.
            self.setExplainer(
                TRANS("Signal Integrity"),
                explainer::buildMetricsBlock(
                    {{"True Peak", juce::String((double)tp[af::TruePeakIntegrity::true_peak_dbtp], 2), "dBTP", "threshold: -1.0 dBTP"},
                     {"Mono Bass corr", juce::String((double)mb[af::MonoBassIntegrity::correlation_below_cutoff], 2), "",
                      "threshold: > 0.6 for safe mono"},
                     {"Noise Floor (hiss headroom)", juce::String((double)nf[af::NoiseFloorIntegrity::hiss_headroom_db], 1), "dB",
                      "threshold: > 40 dB headroom"}}) +
                    kSignalIntegrityBase);

            return TRANS("Scan complete.");
        },
        std::move(options));
}

void SignalIntegrityPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    const int cardW = b.getWidth() / 3;

    for (int i = 0; i < 3; ++i) {
        auto cardArea = b.removeFromLeft(cardW).reduced(4);
        const auto& card = cards_[(size_t)i];

        g.setColour(card.colour.withAlpha(0.10f));
        g.fillRoundedRectangle(cardArea.toFloat(), 6.0f);
        g.setColour(card.colour.withAlpha(0.4f));
        g.drawRoundedRectangle(cardArea.toFloat().reduced(0.5f), 6.0f, 1.0f);

        auto inner = cardArea.reduced(10, 8);
        g.setFont(KhensuTheme::labelFont(9.0f).boldened());
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(card.title, inner.removeFromTop(14), juce::Justification::centredLeft, false);

        g.setFont(KhensuTheme::metricFont(18.0f));
        g.setColour(card.colour);
        g.drawText(card.metric, inner.removeFromTop(26), juce::Justification::centredLeft, false);

        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawFittedText(card.verdict, inner, juce::Justification::topLeft, 3);
    }
}

void SignalIntegrityPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    scanButton_.setBounds(controls.removeFromLeft(170));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
