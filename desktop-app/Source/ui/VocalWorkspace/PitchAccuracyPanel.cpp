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

            // No notes: the service's 100% in_tune_pct is a placeholder (and
            // the short-audio return carries no tuning grid) - show the verdict.
            if (self.notes_.empty()) return v[af::PitchAccuracyResponse::verdict].toString();
            return v[af::PitchAccuracyResponse::verdict].toString() + "  -  " + juce::String(juce::roundToInt(self.inTunePct_)) +
                   TRANS("% in tune") + "  -  " + self.tuning_.gridText();
        });
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
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.setColour(KhensuTheme::accentAmber);
        g.drawFittedText(warning, b.removeFromTop(14), juce::Justification::centredLeft, 1, 0.9f);
    } else {
        b.removeFromTop(14);
    }

    const int rowH = juce::jmax(12, juce::jmin(18, b.getHeight() / juce::jmax(1, (int)notes_.size())));
    int maxRows = b.getHeight() / rowH;
    // Say when the list is truncated instead of silently dropping notes (UIX-24).
    const int total = (int)notes_.size();
    const bool truncated = total > maxRows && maxRows > 0;
    if (truncated) --maxRows; // the last row carries "+N more"

    int i = 0;
    for (const auto& n : notes_) {
        if (i >= maxRows) break;
        auto row = b.removeFromTop(rowH).reduced(0, 1);

        g.setFont(KhensuTheme::monoFont(9.5f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(n.name, row.removeFromLeft(36), juce::Justification::centredLeft, false);

        // Diverging bar centred at 0 cents; +/-50 cents spans the full width.
        auto barArea = row.removeFromLeft(row.getWidth() - 60);
        const float centreX = (float)barArea.getCentreX();
        g.setColour(KhensuTheme::borderSubtle);
        g.fillRect((int)centreX, barArea.getY(), 1, barArea.getHeight());

        const float norm = juce::jlimit(-1.0f, 1.0f, n.centsDeviation / 50.0f);
        const float halfW = (float)barArea.getWidth() * 0.5f;
        const auto colour = std::abs(n.centsDeviation) <= 15.0f   ? KhensuTheme::accentTeal
                            : std::abs(n.centsDeviation) <= 30.0f ? KhensuTheme::accentAmber
                                                                  : KhensuTheme::accentRed;
        g.setColour(colour.withAlpha(0.75f));
        const float barW = std::abs(norm) * halfW;
        g.fillRect(norm >= 0.0f ? centreX : centreX - barW, (float)barArea.getY() + 1.0f, barW,
                   (float)barArea.getHeight() - 2.0f);

        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(colour);
        g.drawText((n.centsDeviation >= 0 ? "+" : "") + juce::String(juce::roundToInt(n.centsDeviation)) + juce::String(juce::CharPointer_UTF8("\xc2\xa2")), row,
                   juce::Justification::centredRight, false);
        ++i;
    }
    if (truncated) {
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("+%n more").replace("%n", juce::String(total - maxRows)), b.removeFromTop(rowH),
                   juce::Justification::centredLeft, false);
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
}

} // namespace khensu
