// desktop-app/Source/AppController_Reasoning.cpp
// Metric reasoning: the detected / cause / fix / trade-off / verify card for a
// metric, the copy behind each health metric, and the metric diff overlay.
// Split out of AppController.cpp (REF-8).
#include "AppController.h"
#include "session/KeyedListeners.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
#include "diagnostics/AppDiagnostics.h"
#include "midi/MacroControlRouter.h"
#include "ui/Settings/MacroMappingPanel.h"
#include "ui/shared/AccessibilityCoverage.h"
#include "BackgroundReadKey.h"
#include "ui/shared/KhensuTheme.h"
#include "ui/shared/KhensuLookAndFeel.h"
#include <cmath>
#include <thread>
#include <khensu/common/AppPaths.h>
#include <khensu/common/FileAcl.h>
#include <khensu/common/SecureRandom.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <juce_core/juce_core.h>
#include "ui/SessionIntake/IntakeScreen.h"
#include "ui/Dashboard/DashboardScreen.h"
#include "ui/VocalWorkspace/VocalWorkspaceScreen.h"
#include "ui/MixMasterEngine/MixMasterScreen.h"
#include "ui/SystemBrain/SystemBrainPanel.h"
#include "ui/ExportDelivery/ExportScreen.h"
#include "ui/Copilot/CopilotScreen.h"
#include "ui/Issues/IssuesScreen.h"
#include "ui/ReferenceLibrary/ReferenceLibraryScreen.h"
#include "session/WorkspaceRecoveryState.h"
#include "ui/Settings/SettingsScreen.h"
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "midi/Midi2DeviceManager.h"
#include "ui/shared/RenderPaths.h"
#include "CrashReporter.h"
#include "ui/shared/JobPoller.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

void AppController::showMetricReasoning(const juce::String& detected, const juce::String& cause,
                                        const juce::String& fix, const juce::String& tradeOff,
                                        const juce::String& verify)
{
    Recommendation rec;
    rec.what = detected.toStdString();
    rec.most_likely_cause = cause.toStdString();
    rec.suggested_action = fix.toStdString();
    rec.trade_off = tradeOff.toStdString();
    rec.how_to_verify = verify.toStdString();
    showChainReasoning(rec);
}

void AppController::showHealthMetricReasoning(int metricIndex)
{
    if (!hasAnalysis()) return;
    const auto& r = currentAnalysis();

    switch (metricIndex) {
    case 0: { // Loudness
        const float lufs = r.loudness.lufs_integrated;
        showMetricReasoning("Integrated loudness measures " + juce::String(lufs, 1) + " LUFS.",
                            "This is the whole-track average loudness after K-weighting, the "
                            "same measure streaming platforms normalize to. Most services land "
                            "tracks around -14 LUFS and turn louder masters down.",
                            lufs > -9.0f ? "You're pushing hot - a limiter-heavy master. Ease the output "
                                           "ceiling or pull master gain back if dynamics are suffering."
                            : lufs < -16.0f
                                ? "You're conservative - headroom to spare. If the genre asks "
                                  "for it, a touch more master gain won't hurt."
                                : "You're in the streaming-friendly window; no change needed for loudness alone.",
                            "Louder isn't better once a platform normalizes - chasing LUFS past "
                            "the target just trades dynamics for nothing.",
                            "A/B against a reference at matched loudness (the blind A/B ritual "
                            "gain-matches for you) - if the mix only 'wins' when louder, it isn't actually better.");
        break;
    }
    case 1: { // Dynamics
        const float crest = r.dynamics.crest_factor_db;
        showMetricReasoning("Crest factor is " + juce::String(crest, 1) + " dB (peak above RMS).",
                            "Crest factor is how much room the peaks have above the average "
                            "level - a proxy for how dynamic vs. squashed the master is.",
                            crest < 6.0f    ? "Low crest = heavily compressed/limited. Back off the limiter or "
                                              "bus compression to let transients breathe."
                            : crest > 18.0f ? "Very dynamic - great for some genres, but quiet sections may "
                                              "get lost on small speakers. Gentle compression can even it out."
                                            : "Healthy dynamic range for most program material.",
                            "More dynamics can mean lower perceived loudness; it's a balance with "
                            "the loudness target, not a free win.",
                            "Watch the Compression X-Ray while playing the loudest section - if the "
                            "gain curve flatlines, you're over-compressing.");
        break;
    }
    case 2: { // Spectral
        const float mid = r.spectral.balance_mid;
        showMetricReasoning("Midrange holds " + juce::String(juce::roundToInt(mid * 100.0f)) + "% of the spectral energy.",
                            "Tonal balance across low/mid/high bands. The midrange carries most "
                            "instruments and vocals, so its share shapes how full or scooped the mix reads.",
                            mid < 0.25f   ? "Mid-scooped - can sound impressive solo but thin in a playlist. "
                                            "Check a broad cut around 300-800 Hz isn't over-applied."
                            : mid > 0.55f ? "Mid-forward - punchy but can feel boxy. A gentle dip in the "
                                            "low-mids opens it up."
                                          : "Balanced midrange presence.",
                            "Tonal moves interact - boosting one band changes the perceived level "
                            "of its neighbours; adjust in small steps.",
                            "Reference-match the tonal curve (the diverging strip shows the worst "
                            "band) and check on both monitors and earbuds.");
        break;
    }
    case 3: { // Stereo
        const float corr = r.stereo.correlation;
        showMetricReasoning("Stereo correlation is " + juce::String(corr, 2) + " (+1 mono ... -1 out-of-phase).",
                            "Correlation measures how much the left and right channels agree. "
                            "Near +1 is essentially mono; near or below 0 risks cancellation when "
                            "the mix is summed to mono (club systems, phone speakers).",
                            corr < 0.0f   ? "Negative correlation - parts of the mix will cancel in mono. "
                                            "Narrow the widest elements or check for a polarity/phase issue."
                            : corr > 0.9f ? "Quite narrow - safe in mono but not using the stereo field. "
                                            "Widen reverbs/pads if the genre wants space."
                                          : "Good balance of width and mono-compatibility.",
                            "Width and mono-compatibility trade off - wider imaging always erodes "
                            "some mono robustness.",
                            "Hit the mono button and listen for anything that thins out or "
                            "disappears - that's what's phase-cancelling.");
        break;
    }
    default: { // Artifacts
        const int clips = r.artifacts.clipping_frame_count;
        showMetricReasoning(clips == 0 ? juce::String("No clipped frames detected.")
                                       : juce::String(clips) + " clipped frame(s) detected.",
                            "Clipping is where the signal exceeds full scale and the waveform is "
                            "flattened, adding harsh distortion - usually from too much gain into "
                            "the master or a hot limiter ceiling.",
                            clips == 0 ? "Clean. Keep the true-peak ceiling at or below -1 dBTP for lossy "
                                         "codec headroom."
                                       : "Lower the limiter output ceiling (try -1 dBTP) or trim gain into "
                                         "the master until the clip count hits zero.",
                            "Chasing zero clips at a very low ceiling can cost loudness - target "
                            "-1 dBTP, not -6.",
                            "Re-run analysis after lowering the ceiling and confirm the clipped-"
                            "frame count is zero.");
        break;
    }
    }
}

void AppController::showMetricExplanation(int gauge)
{
    if (!hasAnalysis()) return;
    const auto& r = currentAnalysis();
    switch (gauge) {
    case 0: showHealthMetricReasoning(0); return; // loudness
    case 3: showHealthMetricReasoning(1); return; // crest
    case 4: showHealthMetricReasoning(3); return; // width / correlation
    case 1: {
        const float tp = r.loudness.true_peak_dbtp;
        showMetricReasoning(tp > -98.0f ? "True peak measures " + juce::String(tp, 1) + " dBTP."
                                        : juce::String("True peak was not measured on this file."),
                            "True peak is the highest level of the waveform between samples (4x "
                            "oversampled), the level a converter or a lossy codec actually reaches.",
                            tp > -1.0f ? "Above -1 dBTP a lossy encode can clip. Lower the limiter ceiling to -1 dBTP."
                                       : "Below -1 dBTP: safe for lossy encodes.",
                            "A lower ceiling costs a little loudness; -1 dBTP is the usual delivery value.",
                            "Re-run the analysis after changing the ceiling and check the true-peak tile.");
        return;
    }
    case 2: {
        showMetricReasoning(r.loudness.lra_measured ? "Loudness range is " + juce::String(r.loudness.lra, 1) + " LU."
                                                    : juce::String("Loudness range needs a longer file to measure."),
                            "Loudness range (EBU R128 LRA) is how far the short-term loudness moves "
                            "between the quiet and loud parts of the track (10th to 95th percentile).",
                            "Compare it with the dots: each one is a released track of the genre. Far "
                            "below them is over-levelled; far above them sounds uneven on small speakers.",
                            "Compression narrows the range; automation or a gentler limiter widens it.",
                            "Re-run the analysis after the change and compare the tile's position in the dots.");
        return;
    }
    default: return;
    }
}

void AppController::showMetricDiff(const juce::String& title, std::vector<MetricDiffTable::Row> rows,
                                   const juce::String& labelA, const juce::String& labelB)
{
    activeMetricDiffOverlay_ = std::make_unique<MetricDiffOverlay>(title, std::move(rows), labelA, labelB);
    activeMetricDiffOverlay_->onDismiss = [this]() { hideMetricDiff(); };
    addAndMakeVisible(activeMetricDiffOverlay_.get());
    activeMetricDiffOverlay_->setBounds(getLocalBounds());
}

void AppController::hideMetricDiff()
{
    if (activeMetricDiffOverlay_ != nullptr) {
        removeChildComponent(activeMetricDiffOverlay_.get());
        activeMetricDiffOverlay_.reset();
    }
}

} // namespace khensu
