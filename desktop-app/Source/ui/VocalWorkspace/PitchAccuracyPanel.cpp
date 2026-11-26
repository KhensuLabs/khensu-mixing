// desktop-app/Source/ui/VocalWorkspace/PitchAccuracyPanel.cpp
#include "PitchAccuracyPanel.h"
#include "../shared/SessionAudioPaths.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "MixTuningReference.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void PitchAccuracyPanel::applyThemeColours()
{
    scanButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    scanButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

namespace {
/// VIS-4: the tuning verdict in words beside the number.
juce::String tuneWord(float cents)
{
    if (std::abs(cents) <= 15.0f) return TRANS("in tune");
    return cents > 0.0f ? TRANS("sharp") : TRANS("flat");
}
} // namespace

PitchAccuracyPanel::PitchAccuracyPanel(IAppController& c) : PanelCard(c, "Pitch Accuracy")
{
    applyThemeColours();
    setExplainer(TRANS("Pitch Accuracy"), "Tracks pitch (pYIN) across the vocal and segments it into notes, showing how many "
                                   "cents sharp or flat each one sits from the nearest in-tune pitch - the "
                                   "specific question a singer or engineer actually asks, not just a broad tuning "
                                   "score.\n\n"
                                   "Within 15 cents reads as in-tune to most listeners. STABILITY is the wobble within "
                                   "a note (0 = rock steady, higher = wavering) - a note that's confidently "
                                   "10 cents flat throughout and a note that wanders +/-"
                                   "30 cents are very "
                                   "different fixes.");

    scanButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(scanButton_);
    chart_.setTitle(TRANS("Pitch accuracy per note"));
    chart_.setHorizontal(true);
    chart_.x.lo = -50.0;
    chart_.x.hi = 50.0;
    chart_.x.unit = TRANS("cents");
    chart_.setXRange(-50.0, 50.0);
    chart_.setBaseline(0.0);
    chart_.valueText = [](size_t, size_t, double v) {
        const auto c = static_cast<float>(v);
        return (c >= 0.0f ? "+" : "") + juce::String(juce::roundToInt(c)) + " c " + tuneWord(c);
    };
    addChildComponent(chart_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Analyze the vocal for per-note tuning accuracy."));
}

void PitchAccuracyPanel::runAnalysis()
{
    const juce::String target = session_audio::vocalStemOrSourcePath(controller_);
    if (target.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    const juce::String mixPath = controller_.activeSourcePath();
    fetchThenPublish(
        *this, controller_, run_, TRANS("Tracking pitch..."),
        [target, mixPath] {
            ipc::HttpServiceBridge bridge;
            // AUDIT-016: judge the take against the mix's own concert A, not 440.
            const auto referenceHz = MixTuningReference::forJob(bridge, mixPath);
            return bridge.analyzePitchAccuracy(target.toStdString(), referenceHz);
        },
        [](PitchAccuracyPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("pitch_accuracy", v); // WF-14
            self.notes_.clear();
            if (auto* arr = v[af::PitchAccuracyResponse::notes].getArray()) {
                for (const auto& nv : *arr) {
                    NoteRow row;
                    row.name = nv[af::PitchAccuracyNote::note_name].toString();
                    row.centsDeviation = (float)(double)nv[af::PitchAccuracyNote::mean_cents_deviation];
                    row.stability = (float)(double)nv[af::PitchAccuracyNote::cents_stability];
                    self.notes_.push_back(row);
                }
            }
            self.meanAbsCents_ = (float)(double)v[af::PitchAccuracyResponse::mean_abs_cents];
            self.inTunePct_ = (float)(double)v[af::PitchAccuracyResponse::in_tune_pct];
            self.tuning_ = TuningReading::fromResponse(v);
            self.rebuildChart();

            // No notes: the service's 100% in_tune_pct is a placeholder (and
            // the short-audio return carries no tuning grid) - show the verdict.
            if (self.notes_.empty()) return v[af::PitchAccuracyResponse::verdict].toString();
            return v[af::PitchAccuracyResponse::verdict].toString() + "  -  " + juce::String(juce::roundToInt(self.inTunePct_)) +
                   TRANS("% in tune") + "  -  " + self.tuning_.gridText();
        });
}

void PitchAccuracyPanel::rebuildChart()
{
    // VIS-1: a ChartKit bar chart on a cents axis (-50 .. +50), one bar per
    // note; flat in the diverging palette's cool pole, sharp in its warm
    // pole, every bar labelled with its cents and the verdict in words.
    std::vector<ui::chart::Bar> bars;
    juce::StringArray parts;
    for (const auto& n : notes_) {
        ui::chart::Bar b;
        b.label = n.name;
        b.values = {n.centsDeviation};
        b.colour = KhensuTheme::vizDiverging(juce::jlimit(-1.0f, 1.0f, n.centsDeviation / 50.0f) * 0.6f +
                                             (n.centsDeviation >= 0.0f ? 0.4f : -0.4f));
        bars.push_back(b);
        parts.add(n.name + " " + (n.centsDeviation >= 0.0f ? "+" : "") + juce::String(juce::roundToInt(n.centsDeviation)) +
                  " " + TRANS("cents") + " (" + tuneWord(n.centsDeviation) + ")");
    }
    chart_.setBars(std::move(bars), {{TRANS("Mean deviation"), KhensuTheme::vizDiverging(0.8f)}});
    chart_.setSummary(notes_.empty() ? TRANS("Pitch accuracy: no notes measured.")
                                     : TRANS("Pitch accuracy per note") + ", " + juce::String(juce::roundToInt(inTunePct_)) +
                                           TRANS("% in tune") + ": " + parts.joinIntoString(", ") + ".");
}

void PitchAccuracyPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || notes_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    // Off-grid caveat (the service's reference_offset_warning): a take whose
    // own tuning sits away from the grid reads every note as "out".
    const auto warning = tuning_.warningText();
    if (warning.isNotEmpty()) {
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::accentAmber);
        g.drawFittedText(warning, b.removeFromTop(14), juce::Justification::centredLeft, 1, 0.9f);
    }
}

void PitchAccuracyPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    scanButton_.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
    chart_.setBounds(errArea.withTrimmedTop(16)); // under the off-grid caveat row
    chart_.setVisible(run_.showsResult() && !notes_.empty());
}

} // namespace khensu
