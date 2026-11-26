// desktop-app/Source/ui/ProvenanceSource/AiDetectionScreen.cpp
#include "AiDetectionScreen.h"
#include "../shared/PanelRunState.h"
#include "../shared/ScreenHeader.h"
#include "AiVerdictText.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/AnalysisFeatureText.h"
#include "../shared/ServiceErrorHint.h"
#include "../../settings/OnlineSettings.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/common/AppPaths.h>
#include <cmath>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {

struct AiDetectionSectionLayout {
    juce::Rectangle<int> whatDetected;
    juce::Rectangle<int> timeline;
    juce::Rectangle<int> identity;
    juce::Rectangle<int> cleanup;
};

AiDetectionSectionLayout layoutSections(juce::Rectangle<int> area)
{
    AiDetectionSectionLayout layout;
    layout.whatDetected = area.removeFromTop(juce::jmax(160, area.getHeight() - 300));
    layout.timeline = area.removeFromTop(84);
    layout.identity = area.removeFromTop(112); // + the fingerprint-compare line
    layout.cleanup = area;
    return layout;
}

} // namespace

// Severity of a finding's *effective* contribution: production-style
// observations (no weight) stay neutral, never red (AID-05).
static juce::Colour severityColour(float effectiveWeight, const juce::String& evidenceClass)
{
    if (evidenceClass == "observation" || effectiveWeight <= 0.0f) return KhensuTheme::textTertiary;
    if (evidenceClass == "provenance") return KhensuTheme::accentRed;
    return effectiveWeight >= 0.33f ? KhensuTheme::accentAmber : KhensuTheme::accentBlue;
}

static juce::Colour toneColour(ai_verdict_text::Tone tone)
{
    switch (tone) {
    case ai_verdict_text::Tone::Provenance: return KhensuTheme::accentRed;
    case ai_verdict_text::Tone::Heuristic: return KhensuTheme::accentAmber;
    case ai_verdict_text::Tone::Clear: break;
    }
    // Informational, not a pass: a clean scan does not show the music is human-made.
    return KhensuTheme::accentBlue;
}

// Every cached palette colour on this component, in one place (UI-01).
void AiDetectionScreen::applyThemeColours()
{
    scanButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    scanButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    lookupButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    lookupButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    compareButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    compareButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    applyLeak_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    applyLeak_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    openRecreate_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    openHumanize_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
}

AiDetectionScreen::AiDetectionScreen(IAppController& c, std::string baseUrl)
    : controller_(c), baseUrl_(std::move(baseUrl)), sessionRegistration_(c, *this)
{
    applyThemeColours();
    scanButton_.onClick = [this] { runScan(); };
    updateScanButton();
    addAndMakeVisible(scanButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(TRANS("Scans the mix for AI-generation + forensic artifacts."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    lookupButton_.onClick = [this] { runFingerprint(); };
    addAndMakeVisible(lookupButton_);

    compareButton_.setTooltip(TRANS("Fingerprint this mix and another file locally and say whether they are the same "
                                    "recording, another version of the song, or different."));
    compareButton_.onClick = [this] { chooseCompareFile(); };
    addAndMakeVisible(compareButton_);

    const char* presets[] = {"Default", "Vocal", "Instrument", "Master", "AI Ghost", "AI Heavy", "Aggressive"};
    for (int i = 0; i < 7; ++i)
        leakPreset_.addItem(presets[i], i + 1);
    leakPreset_.setSelectedId(5, juce::dontSendNotification); // "AI Ghost" default
    // A preset list with no label beside it: name it explicitly (VER-6).
    leakPreset_.setTitle(TRANS("LeakGate preset"));
    leakPreset_.setTooltip(TRANS("How strongly LeakGate cleans the mix. Apply LeakGate renders with this preset."));
    addAndMakeVisible(leakPreset_);

    applyLeak_.onClick = [this] {
        const int idx = juce::jlimit(0, 6, leakPreset_.getSelectedItemIndex());
        cleanupStatus_ = "Applying LeakGate...";
        applyLeak_.setEnabled(false);
        repaint();
        juce::Component::SafePointer<AiDetectionScreen> self(this);
        controller_.renders().applyCleanupChain("LeakGate", idx, [self](bool ok, juce::String pathOrErr) {
            if (self == nullptr) return;
            self->applyLeak_.setEnabled(true);
            self->cleanupStatus_ =
                ok ? "Cleaned - " + juce::File(pathOrErr).getFileName() : "Cleanup failed: " + pathOrErr;
            self->repaint();
        });
    };
    addAndMakeVisible(applyLeak_);

    openRecreate_.onClick = [this] { controller_.shell().showScreen(ScreenType::Dashboard); };
    addAndMakeVisible(openRecreate_);

    openHumanize_.onClick = [this] {
        controller_.shell().showMixMasterCard(7); // TabHumanize
    };
    addAndMakeVisible(openHumanize_);

    errorState_.onRetry([this] { runScan(); });
    addChildComponent(errorState_);

    // CON-8: AI detection | Source.
    viewStrip_.addSegment(TRANS("AI detection"));
    viewStrip_.addSegment(TRANS("Source"));
    viewStrip_.setTitle(TRANS("Provenance view"));
    viewStrip_.onChange = [this](int i) {
        sourceView_.setVisible(i == 1);
        if (i == 1) sourceView_.toFront(false);
    };
    addAndMakeVisible(viewStrip_);
    addChildComponent(sourceView_);

    // AUTO-1: a published analysis is followed by the source scan. Registered
    // in the body, not the initializer list (ENG-2). A short delay keeps it
    // behind the Dashboard's own follow-up scans, which it then reuses.
    analysisListener_ = listen(controller_.analysis(), this, &IAnalysisHub::addAnalysisListener,
                               &IAnalysisHub::removeAnalysisListener, [this](const AnalysisResult&) {
                                   juce::Component::SafePointer<AiDetectionScreen> self(this);
                                   juce::Timer::callAfterDelay(1500, [self] {
                                       if (self != nullptr) self->maybeAutoScan();
                                   });
                               });
}

void AiDetectionScreen::updateScanButton()
{
    // Before a result: the deep scan is the only way in. After: run it again.
    scanButton_.setButtonText(hasData_ ? TRANS("Re-scan") : TRANS("Scan for AI / Artifacts"));
    scanButton_.setTooltip(TRANS("Runs the deep scan: metadata, spectral signatures and the learned watermark "
                                 "models (AudioSeal, WavMark). It can take a few minutes."));
}

void AiDetectionScreen::resetForSession()
{
    // The previous track's verdict must not stay on screen for the next one.
    hasData_ = false;
    deepShown_ = false;
    autoRetries_ = 0;
    scannedPath_.clear();
    scan_ = juce::var();
    findings_.clear();
    timeline_.clear();
    services_.clear();
    clipPositions_.clear();
    notRunLine_.clear();
    durationSec_ = 0.0f;
    detectorsRun_ = 0;
    mixQualityFetched_ = false;
    identityLine_.clear();
    identityStatus_.clear();
    fpMethod_.clear();
    fpDigest_.clear();
    compareLine_.clear();
    cleanupStatus_.clear();
    errorState_.setVisible(false);
    statusLabel_.setText(TRANS("Scans the mix for AI-generation + forensic artifacts."), juce::dontSendNotification);
    updateScanButton();
    resized();
    repaint();
}

void AiDetectionScreen::visibilityChanged()
{
    if (isShowing()) maybeAutoScan();
}

void AiDetectionScreen::maybeAutoScan()
{
    if (!isShowing() || scanning_ || fastScanning_) return;
    const auto path = controller_.activeSourcePath();
    if (path.isEmpty()) return;                   // nothing open: the empty state says so
    if (hasData_ && scannedPath_ == path) return; // already showing this track
    const auto retryLater = [this](const juce::String& why) {
        statusLabel_.setText(why, juce::dontSendNotification);
        if (autoRetryQueued_) return;
        autoRetryQueued_ = true;
        juce::Component::SafePointer<AiDetectionScreen> self(this);
        juce::Timer::callAfterDelay(3000, [self] {
            if (self == nullptr) return;
            self->autoRetryQueued_ = false;
            self->maybeAutoScan();
        });
    };
    // Low priority: never alongside the analysis it follows, never at a
    // starting or stopped engine.
    if (controller_.analysis().resultsPending() || !controller_.hasAnalysis()) {
        retryLater(TRANS("The scan runs once the track's analysis is ready."));
        return;
    }
    // "Starting" can last while the engine already answers (the analysis it
    // follows came from it); only a stopped engine waits.
    if (const auto st = controller_.lifecycle().getPythonServiceStatus();
        st == PythonStatus::Stopped || st == PythonStatus::GaveUp) {
        retryLater(TRANS("Waiting for the analysis engine to start the scan..."));
        return;
    }
    runFastScan();
}

void AiDetectionScreen::runFastScan()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) return;
    const juce::String target(s->files[0].path);
    // The same request as the Dashboard's AI Artifacts card (no learned
    // tier, the same stems), so the service's cache answers it.
    std::map<std::string, std::string> stems;
    if (controller_.stems().hasStemsAvailable()) {
        for (const char* name : {"drums", "vocals", "bass", "other"}) {
            const auto f = controller_.getStemFile(name);
            if (f.existsAsFile()) stems[name] = f.getFullPathName().toStdString();
        }
    }
    fastScanning_ = true;
    statusLabel_.setText(TRANS("Reading the scan that ran with the analysis..."), juce::dontSendNotification);
    repaint();
    juce::Component::SafePointer<AiDetectionScreen> self(this);
    controller_.runLongInBackground([self, &work = controller_, target, stems, baseUrl = baseUrl_]() {
        auto result = ipc::HttpServiceBridge(baseUrl).analyzeArtifactsScan(target.toStdString(), stems);
        postIfCurrent(
            self, work,
            [result, target](AiDetectionScreen& panel) {
                if (panel.scanning_ || (panel.deepShown_ && panel.scannedPath_ == target))
                    return; // the deep scan is running or already shown: it says more
                if (result.is_err()) {
                    // Quiet: the button still offers the deep scan, and the
                    // reason is one line, not the full error card.
                    panel.statusLabel_.setText(TRANS("The quick scan did not answer:") + " " +
                                                   serviceErrorLine(result.error()) + " " +
                                                   TRANS("Press Scan to run it now."),
                                               juce::dontSendNotification);
                    // The engine may still be coming up: try again a few times.
                    if (panel.autoRetries_++ < 5) {
                        juce::Component::SafePointer<AiDetectionScreen> again(&panel);
                        juce::Timer::callAfterDelay(5000, [again] {
                            if (again != nullptr) again->maybeAutoScan();
                        });
                    }
                    return;
                }
                panel.scannedPath_ = target;
                panel.autoRetries_ = 0;
                panel.applyScan(result.value(), false);
            },
            [](AiDetectionScreen& panel) {
                panel.fastScanning_ = false;
                panel.repaint();
            });
    });
}

void AiDetectionScreen::runScan()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    const juce::String target(s->files[0].path);
    std::map<std::string, std::string> stems;
    if (controller_.stems().hasStemsAvailable()) {
        for (const char* name : {"drums", "vocals", "bass", "other"}) {
            const auto f = controller_.getStemFile(name);
            if (f.existsAsFile()) stems[name] = f.getFullPathName().toStdString();
        }
    }
    scanButton_.setEnabled(false);
    beginScanProgress();

    juce::Component::SafePointer<AiDetectionScreen> self(this);
    controller_.runLongInBackground([self, &work = controller_, target, stems, baseUrl = baseUrl_]() { // scans run up to 5 min
        ipc::HttpServiceBridge bridge(baseUrl);
        // The explicit deep scan is the only caller that asks for the learned
        // watermark/model tiers (AID-08).
        auto result = bridge.analyzeArtifactsScan(target.toStdString(), stems, /*includeLearned=*/true);
        // Mix-quality model (pipeline #9) runs alongside — opt-in/gated, so this
        // usually reports unavailable. Best-effort; never blocks the scan.
        auto mq = bridge.analyzeMixQuality(target.toStdString());
        postIfCurrent(self, work, [mq](AiDetectionScreen& panel) {
            panel.mixQualityFetched_ = true;
            panel.mixQualityAvailable_ = false;
            panel.mixQualityProb_ = -1.0f;
            panel.mixQualityBand_.clear();
            panel.mixQualityNote_.clear();
            panel.mixQualityReliability_.clear();
            panel.mixQualityEce_ = -1.0f;
            panel.mixQualityEceLimit_ = -1.0f;
            if (mq.is_ok()) {
                const juce::var v = mq.value();
                panel.mixQualityAvailable_ = (bool)v[af::MixQualityResponse::available];
                if (panel.mixQualityAvailable_) {
                    panel.mixQualityBand_ = v[af::MixQualityResponse::quality_band].toString();
                    if (v[af::MixQualityResponse::quality_probability].isDouble())  // else null == withheld
                        panel.mixQualityProb_ = (float)(double)v[af::MixQualityResponse::quality_probability];
                    if (v[af::MixQualityResponse::calibration_ece].isDouble())
                        panel.mixQualityEce_ = (float)(double)v[af::MixQualityResponse::calibration_ece];
                    if (v[af::MixQualityResponse::calibration_ece_limit].isDouble())
                        panel.mixQualityEceLimit_ = (float)(double)v[af::MixQualityResponse::calibration_ece_limit];
                    if (auto* bins = v[af::MixQualityResponse::reliability_diagram].getArray())
                        for (const auto& bin : *bins) {
                            if (!bin[af::MixQualityReliabilityBin::mean_confidence].isDouble() ||
                                !bin[af::MixQualityReliabilityBin::observed_accuracy].isDouble() || !bin[af::MixQualityReliabilityBin::count].isInt())
                                continue;
                            const float confidence = (float)(double)bin[af::MixQualityReliabilityBin::mean_confidence];
                            const float accuracy = (float)(double)bin[af::MixQualityReliabilityBin::observed_accuracy];
                            const int count = (int)bin[af::MixQualityReliabilityBin::count];
                            if (count > 0 && std::isfinite(confidence) && std::isfinite(accuracy) &&
                                juce::isPositiveAndBelow(confidence, 1.001f) &&
                                juce::isPositiveAndBelow(accuracy, 1.001f))
                                panel.mixQualityReliability_.push_back({confidence, accuracy, count});
                        }
                } else {
                    panel.mixQualityNote_ = v[af::MixQualityResponse::reason].toString();
                }
            } else {
                panel.mixQualityNote_ = "service unavailable";
            }
        });
        postIfCurrent(self, work, [result, target](AiDetectionScreen& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Scan failed."), juce::dontSendNotification);
                const auto hint = classifyServiceError(result.error()); // the real cause, not always "unreachable"
                panel.errorState_.setError(hint.message, hint.remedy);
                panel.errorState_.setVisible(true);
                panel.resized();
                panel.repaint();
                return;
            }
            panel.scannedPath_ = target;
            panel.applyScan(result.value(), true);
            panel.statusLabel_.setText(TRANS("Scan complete in ") + panel.elapsedString() + " - " +
                                           juce::String((int)panel.findings_.size()) + " finding(s) from " +
                                           juce::String(panel.detectorsRun_) + " detector(s).",
                                       juce::dontSendNotification);
        }, [](AiDetectionScreen& panel) {
            panel.scanButton_.setEnabled(true);
            panel.endScanProgress();
        });
    });
}

void AiDetectionScreen::applyScan(const juce::var& v, bool deep)
{
    AiDetectionScreen& panel = *this;
    panel.errorState_.setVisible(false);
    panel.deepShown_ = deep;
    panel.scan_ = v;
    panel.notRunLine_ = ai_verdict_text::notRunLine(v);
    panel.durationSec_ = (float)(double)v[af::ArtifactScanResponse::duration_sec];
    panel.findings_.clear();
    if (auto* arr = v[af::ArtifactScanResponse::findings].getArray())
        for (const auto& f : *arr) {
            Finding fd;
            fd.kind = f[af::ArtifactFindingOut::kind].toString();
            fd.summary = f[af::ArtifactFindingOut::summary].toString();
            fd.evidence = f[af::ArtifactFindingOut::evidence].toString();
            fd.evidenceClass = f[af::ArtifactFindingOut::evidence_class].toString();
            fd.weight = (float)(double)f[af::ArtifactFindingOut::weight];
            const juce::var eff = f[af::ArtifactFindingOut::effective_weight];
            fd.effectiveWeight = eff.isVoid() ? fd.weight : (float)(double)eff;
            if (auto* tr = f[af::ArtifactFindingOut::time_range_sec].getArray(); tr != nullptr && tr->size() >= 2) {
                fd.t0 = (float)(double)(*tr)[0];
                fd.t1 = (float)(double)(*tr)[1];
            }
            panel.findings_.push_back(std::move(fd));
        }
    panel.timeline_.clear();
    if (auto* arr = v[af::ArtifactScanResponse::timeline].getArray())
        for (const auto& w : *arr)
            panel.timeline_.push_back({(float)(double)w[af::ArtifactTimelineWindow::start_sec],
                                       (float)(double)w[af::ArtifactTimelineWindow::end_sec],
                                       (float)(double)w[af::ArtifactTimelineWindow::confidence]});
    panel.services_.clear();
    if (auto* arr = v[af::ArtifactScanResponse::service_checklist].getArray())
        for (const auto& row : *arr) {
            ServiceRow r;
            r.line = ai_verdict_text::checklistLine(row);
            r.likelihood = (float)(double)row[af::ArtifactServiceCheck::likelihood];
            r.fromProvenance = ai_verdict_text::checklistFromProvenance(row);
            panel.services_.push_back(std::move(r));
        }
    panel.clipPositions_.clear();
    if (auto* pos = v[af::ArtifactScanResponse::clipping][af::ArtifactClippingCensus::event_positions_sec].getArray())
        for (const auto& p : *pos)
            panel.clipPositions_.push_back((float)(double)p);
    panel.detectorsRun_ = v[af::ArtifactScanResponse::detectors_run].getArray() != nullptr ? v[af::ArtifactScanResponse::detectors_run].getArray()->size() : 0;
    panel.hasData_ = true;
    if (!deep)
        panel.statusLabel_.setText(
            TRANS("Quick scan from the analysis:") + " " + juce::String((int)panel.findings_.size()) +
                " finding(s) from " + juce::String(panel.detectorsRun_) + " detector(s). " +
                TRANS("Re-scan adds the learned watermark models."),
            juce::dontSendNotification);
    panel.updateScanButton();
    panel.resized();
    panel.repaint();
}

void AiDetectionScreen::runFingerprint()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) return;
    const auto settings = OnlineSettings::load(juce::File(juce::String(AppPaths::dataDirectory())));
    const bool onlineOk = settings.enabled && settings.acoustidLookup;
    lookupButton_.setEnabled(false);
    identityStatus_ =
        onlineOk ? juce::String("Looking up...") : juce::String("Online lookup is off - enable it in Settings.");
    repaint();

    juce::Component::SafePointer<AiDetectionScreen> self(this);
    controller_.runInBackground([self, &work = controller_, path, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty("file_path", path);
        body->setProperty("online", true);
        auto result = bridge.postAnalyzeJson("/analyze/fingerprint", juce::var(body.get()));
        postIfCurrent(self, work, [result](AiDetectionScreen& panel) {
            if (result.is_err()) {
                panel.identityStatus_ = "Fingerprint failed.";
                panel.repaint();
                return;
            }
            const juce::var v = result.value();
            panel.fpMethod_ = v[af::FingerprintResponse::method].toString();
            panel.fpDigest_ = v[af::FingerprintResponse::fingerprint].toString().substring(0, 16);
            const juce::var ol = v[af::FingerprintResponse::online_lookup];
            const juce::String st = ol[af::FingerprintOnlineLookup::status].toString();
            panel.identityLine_.clear();
            if (st == "ok") {
                if (auto* arr = ol[af::FingerprintOnlineLookup::results].getArray(); arr != nullptr && arr->size() > 0) {
                    const auto& r = (*arr)[0];
                    panel.identityLine_ = r[af::AcoustIdMatch::artist].toString() + " - " + r[af::AcoustIdMatch::title].toString() + "  (" +
                                          juce::String((double)r[af::AcoustIdMatch::score], 2) + ")";
                    panel.identityStatus_ = "Matched";
                } else
                    panel.identityStatus_ = "No catalog match.";
            } else if (st == "no_match")
                panel.identityStatus_ = "No catalog match.";
            else if (st == "no_key")
                panel.identityStatus_ = "Add an AcoustID key in Settings.";
            else if (st == "no_chromaprint")
                panel.identityStatus_ = "Needs Chromaprint - put fpcalc on PATH or set FPCALC, then retry.";
            else if (st == "disabled")
                panel.identityStatus_ = "Online lookup is off - enable in Settings.";
            else
                panel.identityStatus_ = "Lookup unavailable.";
            panel.repaint();
        }, [](AiDetectionScreen& panel) { panel.lookupButton_.setEnabled(true); });
    });
}

void AiDetectionScreen::chooseCompareFile()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        compareLine_ = TRANS("No active session.");
        repaint();
        return;
    }
    compareChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Compare with another file"), juce::File(path).getParentDirectory(),
        "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg;*.m4a");
    juce::Component::SafePointer<AiDetectionScreen> self(this);
    const auto epoch = controller_.sessionEpoch();
    compareChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [self, path, epoch](const juce::FileChooser& fc) {
                                     if (self == nullptr) return;
                                     if (self->controller_.sessionEpoch() != epoch) return; // session changed
                                     const auto file = fc.getResult();
                                     if (!file.existsAsFile()) return;
                                     self->runCompare(path, file);
                                 });
}

void AiDetectionScreen::runCompare(const juce::String& pathA, const juce::File& other)
{
    compareButton_.setEnabled(false);
    compareIsMatch_ = false;
    compareLine_ = TRANS("Comparing fingerprints with") + " " + other.getFileName() + "...";
    repaint();
    juce::Component::SafePointer<AiDetectionScreen> self(this);
    const juce::String pathB = other.getFullPathName();
    controller_.runInBackground([self, &work = controller_, pathA, pathB, baseUrl = baseUrl_]() {
        auto result = ipc::HttpServiceBridge(baseUrl).compareFingerprints(pathA, pathB);
        postIfCurrent(self, work, [result, pathB](AiDetectionScreen& panel) {
            if (result.is_err()) {
                panel.compareLine_ = TRANS("Compare failed:") + " " + serviceErrorLine(result.error());
            } else {
                const auto& r = result.value();
                panel.compareIsMatch_ = r.verdict == "same_recording";
                panel.compareLine_ = feature_text::fingerprintSummary(r, juce::File(pathB).getFileName());
            }
            panel.repaint();
        }, [](AiDetectionScreen& panel) { panel.compareButton_.setEnabled(true); }); // busy state first
    });
}

// ── deep-scan progress affordance ────────────────────────────────────────────

void AiDetectionScreen::beginScanProgress()
{
    scanning_ = true;
    scanStartMs_ = (juce::int64)juce::Time::getMillisecondCounter();
    spinnerPhase_ = 0;
    statusLabel_.setText(TRANS("Scanning... deep pass (learned watermark models) - this can take a few minutes."),
                         juce::dontSendNotification);
    startTimerHz(8); // spinner + elapsed tick; cheap on a mostly-static screen
    repaint();
}

void AiDetectionScreen::endScanProgress()
{
    scanning_ = false;
    stopTimer();
    // scanStartMs_ is deliberately left intact so the completion/failure line
    // can still report the total elapsed time via elapsedString().
    repaint();
}

juce::String AiDetectionScreen::elapsedString() const
{
    const auto now = (juce::int64)juce::Time::getMillisecondCounter();
    const int secs = (int)juce::jmax((juce::int64)0, (now - scanStartMs_) / 1000);
    return juce::String(secs / 60) + ":" + juce::String(secs % 60).paddedLeft('0', 2);
}

void AiDetectionScreen::timerCallback()
{
    if (!scanning_) {
        stopTimer();
        return;
    }
    // ASCII spinner only - JUCE renders multibyte UTF-8 per-byte (mojibake).
    static const char kSpin[] = {'|', '/', '-', '\\'};
    const char frame = kSpin[spinnerPhase_ % 4];
    ++spinnerPhase_;
    statusLabel_.setText(juce::String("Scanning ") + frame + "  " + elapsedString() +
                             " - learned watermark pass, can take a few minutes.",
                         juce::dontSendNotification);
    repaint(); // animate the centre spinner arc
}

// ── painting ────────────────────────────────────────────────────────────────

void AiDetectionScreen::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgBase);
    auto b = getLocalBounds().reduced(24);

    // Title + status row (controls are laid out in resized()).
    screen_header::paint(g, b.removeFromTop(30), screen_header::title(ScreenType::AiDetection),
                         screen_header::purpose(ScreenType::AiDetection)); // UX-3
    b.removeFromTop(30); // controls row (scan button/status painted as widgets)

    if (errorState_.isVisible()) return;
    if (!hasData_) {
        if (scanning_) {
            // Rotating arc spinner above a two-line "this takes a while" note, so
            // the multi-minute learned pass reads as working, not hung.
            const float cx = (float)b.getCentreX();
            const float cy = (float)b.getCentreY() - 34.0f;
            const float r = 15.0f;
            const float start = (float)spinnerPhase_ * 0.4f;
            juce::Path arc;
            arc.addCentredArc(cx, cy, r, r, 0.0f, start, start + juce::MathConstants<float>::pi * 1.5f, true);
            g.setColour(KhensuTheme::accentTeal);
            g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const int cyc = b.getCentreY();
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::bodyFont(type::body));
            g.drawText("Scanning for AI-generation and forensic artifacts... " + elapsedString(),
                       juce::Rectangle<int>(b.getX(), cyc - 2, b.getWidth(), 20), juce::Justification::centred, false);
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::bodyFont(type::caption));
            g.drawText("This deep pass runs the learned watermark models (AudioSeal, WavMark) and can take a few minutes.",
                       juce::Rectangle<int>(b.getX(), cyc + 20, b.getWidth(), 16), juce::Justification::centred, false);
            return;
        }
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        // AUTO-1: the scan follows the analysis by itself; say which step it waits for.
        const juce::String text = !controller_.hasActiveSession()
                                      ? TRANS("Open a track: its AI and artifact scan runs after the analysis.")
                                  : fastScanning_ ? TRANS("Reading the scan that ran with the analysis...")
                                                  : TRANS("The scan runs after the analysis; Scan runs the deep pass now.");
        g.drawText(text, b, juce::Justification::centred, false);
        return;
    }

    const auto layout = layoutSections(b);
    paintWhatDetected(g, layout.whatDetected);
    paintTimeline(g, layout.timeline);
    paintIdentity(g, layout.identity);
    paintCleanup(g, layout.cleanup);
}

void AiDetectionScreen::paintWhatDetected(juce::Graphics& g, juce::Rectangle<int> area)
{
    // Verdict header (AID-05 / UIX-09): "likely AI-generated" only on
    // provenance evidence; the number is labelled as an uncalibrated
    // heuristic signal score.
    auto header = area.removeFromTop(30);
    const auto head = ai_verdict_text::headline(scan_);
    const juce::Colour vc = toneColour(head.tone);
    g.setColour(vc.withAlpha(0.15f));
    g.fillRoundedRectangle(header.toFloat().reduced(0, 2), 4.0f);
    g.setColour(vc);
    g.setFont(KhensuTheme::labelFont(type::secondary).boldened());
    g.drawText(head.text, header.reduced(10, 0), juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.drawText(ai_verdict_text::scoreText(scan_), header.reduced(10, 0),
               juce::Justification::centredRight, false);
    area.removeFromTop(6);

    // Mix-quality readout (pipeline #9), compact single line — contained within
    // this section's fixed area, so it never shifts the timeline/identity/cleanup
    // sections below or their widgets.
    if (mixQualityFetched_) {
        auto mqRow = area.removeFromTop(16);
        g.setFont(KhensuTheme::labelFont(type::caption));
        if (!mixQualityAvailable_) {
            g.setColour(KhensuTheme::textTertiary);
            g.drawText("Mix quality: not available (" + mixQualityNote_ + ")", mqRow,
                       juce::Justification::centredLeft, true);
        } else {
            const juce::Colour bc = mixQualityBand_ == "clean"       ? KhensuTheme::accentGreen
                                    : mixQualityBand_ == "borderline" ? KhensuTheme::accentAmber
                                                                      : KhensuTheme::accentRed;
            g.setColour(bc);
            juce::String txt = "Mix quality: " + mixQualityBand_;
            if (mixQualityProb_ >= 0.0f)
                txt += "  (" + juce::String((int)std::round(mixQualityProb_ * 100.0f)) + "%)";
            else
                txt += "  (score withheld - calibration gated)";
            g.drawText(txt, mqRow, juce::Justification::centredLeft, true);
        }
        area.removeFromTop(4);
    }

    if (!mixQualityReliability_.empty() && area.getHeight() >= 84) {
        paintReliabilityDiagram(g, area.removeFromTop(66));
        area.removeFromTop(4);
    }

    // Service checklist (two columns).
    const int rows = ((int)services_.size() + 1) / 2;
    if (rows > 0) {
        auto grid = area.removeFromTop(rows * 16);
        const int colW = grid.getWidth() / 2;
        for (size_t i = 0; i < services_.size(); ++i) {
            const auto& sr = services_[i];
            juce::Rectangle<int> cell(grid.getX() + colW * (int)(i / (size_t)rows),
                                      grid.getY() + 16 * (int)(i % (size_t)rows), colW, 16);
            // "resembles X (score)" unless provenance names the service.
            const juce::Colour rc = sr.fromProvenance        ? KhensuTheme::accentRed
                                    : sr.likelihood >= 0.05f ? KhensuTheme::accentAmber
                                                             : KhensuTheme::textTertiary;
            g.setColour(rc);
            g.setFont(KhensuTheme::labelFont(type::caption));
            g.drawText(sr.line, cell.reduced(2, 0), juce::Justification::centredLeft, true);
        }
    }
    // Detectors that did not run / errored, so "nothing found" is never read
    // as "everything checked" (AID-05, AID-12).
    if (notRunLine_.isNotEmpty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(notRunLine_, area.removeFromTop(14), juce::Justification::centredLeft, true);
    }
    // NEW-9: the CLAP probe's reading - evidence beside the verdict, not part of it.
    if (const auto probe = scan_[af::ArtifactScanResponse::clap_probe]; probe.isObject()) {
        const double p = (double)probe[af::ArtifactClapProbe::probability];
        const juce::var fpr = probe[af::ArtifactClapProbe::unseen_source_false_positive_rate];
        juce::String line = TRANS("Learned CLAP probe (experimental, not in the verdict): AI likelihood %p.")
                                .replace("%p", juce::String(p, 2));
        if (!fpr.isVoid())
            line << " " << TRANS("On human-made music from a source it never saw, %f scored as AI.")
                                .replace("%f", juce::String(100.0 * (double)fpr, 1) + "%");
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(line, area.removeFromTop(14), juce::Justification::centredLeft, true);
    }
    area.removeFromTop(8);

    // Findings list (summary + evidence), most severe first.
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(TRANS("Findings"), area.removeFromTop(18), juce::Justification::centredLeft, false);
    // Whole-file rows only; localized copies of a tell are drawn on the
    // timeline. Truncation says "+N more" (UIX-24).
    int listed = 0, shown = 0;
    for (const auto& f : findings_) {
        if (f.t0 >= 0.0f) continue;
        ++listed;
        if (area.getHeight() < 28) continue;
        ++shown;
        auto row = area.removeFromTop(26);
        g.setColour(severityColour(f.effectiveWeight, f.evidenceClass));
        g.fillRoundedRectangle(row.removeFromLeft(4).toFloat().reduced(0, 3), 1.5f);
        row.removeFromLeft(6);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(f.summary + "  [" + ai_verdict_text::classTag(f.evidenceClass) + "]",
                   row.removeFromTop(13), juce::Justification::centredLeft, true);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.drawText(f.evidence, row, juce::Justification::centredLeft, true);
    }
    if (const auto more = ai_verdict_text::moreSuffix(listed, shown); more.isNotEmpty() && area.getHeight() >= 12) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(more, area.removeFromTop(12), juce::Justification::centredLeft, false);
    }
}

void AiDetectionScreen::paintReliabilityDiagram(juce::Graphics& g, juce::Rectangle<int> area)
{
    const bool hasGate = mixQualityEce_ >= 0.0f && mixQualityEceLimit_ > 0.0f;
    const bool passesGate = hasGate && mixQualityEce_ <= mixQualityEceLimit_;
    g.setColour(passesGate ? KhensuTheme::accentGreen : KhensuTheme::accentAmber);
    g.setFont(KhensuTheme::labelFont(type::caption));
    juce::String title = "Held-out calibration: predicted vs observed";
    if (hasGate)
        title += "  ECE " + juce::String(mixQualityEce_, 3) + " / " +
                 juce::String(mixQualityEceLimit_, 3);
    g.drawText(title, area.removeFromTop(13), juce::Justification::centredLeft, true);

    auto plot = area.reduced(20, 1);
    if (plot.getWidth() < 8 || plot.getHeight() < 8) return;
    g.setColour(KhensuTheme::bgSurface.brighter(0.06f));
    g.fillRoundedRectangle(plot.toFloat(), 2.0f);
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
    g.drawLine((float)plot.getX(), (float)plot.getBottom(),
               (float)plot.getRight(), (float)plot.getY(), 1.0f);

    juce::Path curve;
    bool hasPoint = false;
    for (const auto& bin : mixQualityReliability_) {
        const float x = (float)plot.getX() + bin.confidence * (float)plot.getWidth();
        const float y = (float)plot.getBottom() - bin.observedAccuracy * (float)plot.getHeight();
        if (hasPoint) curve.lineTo(x, y); else curve.startNewSubPath(x, y);
        hasPoint = true;
    }
    g.setColour(KhensuTheme::accentTeal);
    g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
    for (const auto& bin : mixQualityReliability_) {
        const float x = (float)plot.getX() + bin.confidence * (float)plot.getWidth();
        const float y = (float)plot.getBottom() - bin.observedAccuracy * (float)plot.getHeight();
        g.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
    }
}

void AiDetectionScreen::paintTimeline(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText("Where in the song", area.removeFromTop(18), juce::Justification::centredLeft, false);
    auto track = area.removeFromTop(40);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(track.toFloat(), 3.0f);
    if (durationSec_ <= 0.0f) return;

    // Per-window heuristic signal from the service timeline (AID-11: localized
    // tells now carry their weight, so windows differ).
    const juce::Colour signal = ai_verdict_text::provenanceFound(scan_) ? KhensuTheme::accentRed
                                                                        : KhensuTheme::accentAmber;
    for (const auto& w : timeline_) {
        if (w.confidence <= 0.0f || w.end <= w.start) continue;
        const int x0 = track.getX() + (int)(track.getWidth() * (w.start / durationSec_));
        const int x1 = track.getX() + (int)(track.getWidth() * (juce::jmin(w.end, durationSec_) / durationSec_));
        g.setColour(signal.withAlpha(juce::jlimit(0.08f, 0.85f, w.confidence)));
        g.fillRect(track.withX(x0).withWidth(juce::jmax(1, x1 - x0)).reduced(0, 6));
    }
    // Localized findings as thin markers on top.
    for (const auto& f : findings_) {
        if (!(f.t0 >= 0.0f && f.t1 > f.t0)) continue;
        const int x0 = track.getX() + (int)(track.getWidth() * (f.t0 / durationSec_));
        const int x1 = track.getX() + (int)(track.getWidth() * (juce::jmin(f.t1, durationSec_) / durationSec_));
        g.setColour(severityColour(f.effectiveWeight, f.evidenceClass));
        g.fillRect(juce::Rectangle<int>(x0, track.getBottom() - 6, juce::jmax(2, x1 - x0), 3));
    }
    // Clip markers.
    g.setColour(KhensuTheme::accentRed);
    for (float p : clipPositions_) {
        const int x = track.getX() + (int)(track.getWidth() * (juce::jmin(p, durationSec_) / durationSec_));
        g.fillRect(x, track.getY(), 1, track.getHeight());
    }
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.drawText("0:00", area.removeFromTop(12).removeFromLeft(60), juce::Justification::centredLeft, false);
}

void AiDetectionScreen::paintIdentity(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText("Identity / Provenance", area.removeFromTop(18), juce::Justification::centredLeft, false);
    area.removeFromTop(28); // Look Up button row (widget)
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::monoFont(type::caption));
    if (fpMethod_.isNotEmpty())
        g.drawText("fingerprint: " + fpMethod_ + "  " + fpDigest_ + "...", area.removeFromTop(14),
                   juce::Justification::centredLeft, false);
    if (identityLine_.isNotEmpty()) {
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(identityLine_, area.removeFromTop(16), juce::Justification::centredLeft, false);
    }
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(identityStatus_, area.removeFromTop(14), juce::Justification::centredLeft, false);
    if (compareLine_.isNotEmpty()) {
        g.setColour(compareIsMatch_ ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(compareLine_, area.removeFromTop(16), juce::Justification::centredLeft, true);
    }
}

void AiDetectionScreen::paintCleanup(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText("Cleanup", area.removeFromTop(18), juce::Justification::centredLeft, false);
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText("Remove inter-stem bleed / AI ghosting - non-destructive, loudness-matched.", area.removeFromTop(14),
               juce::Justification::centredLeft, false);
    area.removeFromTop(30); // controls row (widgets)
    if (cleanupStatus_.isNotEmpty()) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.drawText(cleanupStatus_, area.removeFromTop(14), juce::Justification::centredLeft, false);
    }
}

void AiDetectionScreen::resized()
{
    auto b = getLocalBounds().reduced(24);
    // CON-8: the view strip at the right of the title row; the Source view
    // covers everything under it.
    viewStrip_.setBounds(b.removeFromTop(30).removeFromRight(260).reduced(0, 2));
    sourceView_.setBounds(b.withTrimmedTop(KhensuTheme::sp2));
    auto controls = b.removeFromTop(30);
    scanButton_.setBounds(controls.removeFromLeft(180).reduced(0, 2));
    controls.removeFromLeft(8);
    statusLabel_.setBounds(controls);

    if (errorState_.isVisible()) {
        errorState_.setBounds(b);
        return;
    }
    if (!hasData_) return;

    // Use the same section budget as paint() so a future layout change cannot
    // move painted content away from its controls.
    const auto layout = layoutSections(b);

    // §3 identity: Look Up button sits on the row under the section title.
    auto identity = layout.identity;
    identity.removeFromTop(18);
    auto identityRow = identity.removeFromTop(26);
    lookupButton_.setBounds(identityRow.removeFromLeft(180));
    identityRow.removeFromLeft(8);
    compareButton_.setBounds(identityRow.removeFromLeft(220));

    // §4 cleanup controls.
    auto cleanup = layout.cleanup;
    cleanup.removeFromTop(18 + 14);
    auto row = cleanup.removeFromTop(28);
    leakPreset_.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(8);
    applyLeak_.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(8);
    openRecreate_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(8);
    openHumanize_.setBounds(row.removeFromLeft(140));
}

} // namespace khensu
