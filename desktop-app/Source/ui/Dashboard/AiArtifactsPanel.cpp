// desktop-app/Source/ui/Dashboard/AiArtifactsPanel.cpp
#include "AiArtifactsPanel.h"
#include "../AiDetection/AiVerdictText.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void AiArtifactsPanel::applyThemeColours()
{
    scanButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    scanButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    humanizeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.2f));
    humanizeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

AiArtifactsPanel::AiArtifactsPanel(IAppController& c) : PanelCard(c, "AI Artifacts")
{
    applyThemeColours();
    setExplainer(TRANS("AI Artifacts"),
                 TRANS("Scans the source file for AI-generation evidence in three classes. "
                 "Provenance: container metadata naming a generator, a decoded "
                 "watermark, a validated model - only this can read 'likely "
                 "AI-generated'. Heuristic artifacts: codec-reconstruction residue "
                 "and similar AI-typical signatures - at most 'AI-typical artifacts "
                 "found'. Production-style observations (lossy low-pass, constant "
                 "noise floor, flat dynamics, looped sections) are shown as context "
                 "with no weight - human records do all of these.\n\n"
                 "The number is an uncalibrated heuristic signal score, not a "
                 "probability. A checklist row reads 'resembles X' unless provenance "
                 "names the service. Detectors that did not run are listed. Clipping "
                 "events are counted with their timestamps."),
                 "ai_artifact_detection");

    scanButton_.onClick = [this] { runScan(); };
    addAndMakeVisible(scanButton_);

    humanizeButton_.onClick = [this] {
        controller_.shell().showMixMasterCard(7); // TabHumanize
    };
    addAndMakeVisible(humanizeButton_);

    run_.onRetry([this] { runScan(); });
    run_.setIdle(TRANS("Runs automatically at intake; re-run any time."));

    summaryLabel_.setFont(KhensuTheme::labelFont(9.5f));
    summaryLabel_.setJustificationType(juce::Justification::topLeft);
    summaryLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(summaryLabel_);
}

void AiArtifactsPanel::runScan()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    const juce::String target(s->files[0].path);

    // If Demucs stems already exist, pass them so the service runs the deep
    // tier (per-stem realism) too (spec §5.1 heavy pass).
    std::map<std::string, std::string> stems;
    if (controller_.stems().hasStemsAvailable()) {
        for (const char* name : {"drums", "vocals", "bass", "other"}) {
            const auto f = controller_.getStemFile(name);
            if (f.existsAsFile()) stems[name] = f.getFullPathName().toStdString();
        }
    }

    FetchOptions options;
    options.longPool = true; // scans run up to 5 min
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_,
        stems.empty() ? juce::String("Scanning metadata + spectral signatures...")
                      : juce::String("Deep scan: metadata + spectral + per-stem..."),
        [target, stems] { return ipc::HttpServiceBridge{}.analyzeArtifactsScan(target.toStdString(), stems); },
        [](AiArtifactsPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("ai_artifacts", v); // WF-14
            self.scan_ = v;
            self.notRunLine_ = ai_verdict_text::notRunLine(v, 3);
            self.findingCount_ = v[af::ArtifactScanResponse::findings].getArray() != nullptr ? v[af::ArtifactScanResponse::findings].getArray()->size() : 0;
            self.services_.clear();
            if (auto* arr = v[af::ArtifactScanResponse::service_checklist].getArray()) {
                for (const auto& row : *arr) {
                    ServiceRow r;
                    r.line = ai_verdict_text::checklistLine(row);
                    r.likelihood = (float)(double)row[af::ArtifactServiceCheck::likelihood];
                    r.fromProvenance = ai_verdict_text::checklistFromProvenance(row);
                    self.services_.push_back(std::move(r));
                }
            }
            const juce::var clip = v[af::ArtifactScanResponse::clipping];
            self.clipCount_ = (int)clip[af::ArtifactClippingCensus::event_count];
            self.clipPositions_.clear();
            if (auto* pos = clip[af::ArtifactClippingCensus::event_positions_sec].getArray()) {
                juce::StringArray stamps;
                constexpr int kMaxStamps = 8;
                for (const auto& p : *pos) {
                    if (stamps.size() >= kMaxStamps) break;
                    const int sec = (int)(double)p;
                    stamps.add(juce::String(sec / 60) + ":" + juce::String(sec % 60).paddedLeft('0', 2));
                }
                self.clipPositions_ = stamps.joinIntoString(", ");
                // Say how many were left out instead of truncating silently (UIX-24).
                if (const auto more = ai_verdict_text::moreSuffix(pos->size(), stamps.size()); more.isNotEmpty())
                    self.clipPositions_ << " " << more;
            }
            self.summaryLabel_.setText(v[af::ArtifactScanResponse::summary].toString(), juce::dontSendNotification);
            return TRANS("Scan complete - ") + juce::String(self.findingCount_) + " finding(s).";
        },
        std::move(options));
}

void AiArtifactsPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) return;

    auto b = bodyArea();
    b.removeFromTop(28); // controls row

    // ── Verdict header ───────────────────────────────────────────────────
    // AID-05 / UIX-09: "likely AI-generated" only on provenance evidence; the
    // number is an uncalibrated heuristic signal score.
    auto header = b.removeFromTop(30);
    const auto head = ai_verdict_text::headline(scan_);
    const juce::Colour vc = head.tone == ai_verdict_text::Tone::Provenance  ? KhensuTheme::accentRed
                            : head.tone == ai_verdict_text::Tone::Heuristic ? KhensuTheme::accentAmber
                                                                            : KhensuTheme::accentBlue; // not a pass
    g.setColour(vc.withAlpha(0.15f));
    g.fillRoundedRectangle(header.toFloat().reduced(0, 2), 4.0f);
    g.setColour(vc);
    g.setFont(KhensuTheme::labelFont(11.0f).boldened());
    g.drawText(head.text, header.reduced(10, 0), juce::Justification::centredLeft, true);
    g.setFont(KhensuTheme::monoFont(9.0f));
    g.drawText(ai_verdict_text::scoreText(scan_), header.reduced(10, 0),
               juce::Justification::centredRight, false);
    b.removeFromTop(4);

    // ── Service checklist (two columns) ─────────────────────────────────
    const int rows = ((int)services_.size() + 1) / 2;
    const int rowH = 16;
    auto grid = b.removeFromTop(rows * rowH);
    const int colW = grid.getWidth() / 2;
    for (size_t i = 0; i < services_.size(); ++i) {
        const auto& sr = services_[i];
        juce::Rectangle<int> cell(grid.getX() + colW * (int)(i / (size_t)rows),
                                  grid.getY() + rowH * (int)(i % (size_t)rows), colW, rowH);
        const juce::Colour rc = sr.fromProvenance        ? KhensuTheme::accentRed
                                : sr.likelihood >= 0.05f ? KhensuTheme::accentAmber
                                                         : KhensuTheme::textTertiary;
        g.setColour(rc);
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.drawText(sr.line, cell.reduced(2, 0), juce::Justification::centredLeft, true);
    }
    b.removeFromTop(4);

    // Detectors that did not run (gated models, errors) - AID-05.
    auto notRunRow = b.removeFromTop(14);
    if (notRunLine_.isNotEmpty()) {
        g.setFont(KhensuTheme::labelFont(8.5f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(notRunLine_, notRunRow, juce::Justification::centredLeft, true);
    }

    // ── Clipping census line ─────────────────────────────────────────────
    auto clipRow = b.removeFromTop(14);
    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(clipCount_ > 0 ? KhensuTheme::accentRed : KhensuTheme::textTertiary);
    g.drawText(clipCount_ > 0 ? "Clipping: " + juce::String(clipCount_) + " event(s) at " + clipPositions_
                              : "Clipping: none detected",
               clipRow, juce::Justification::centredLeft, false);
}

void AiArtifactsPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    scanButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(6);
    humanizeButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(6);
    run_.label().setBounds(controls);

    if (run_.showsResult()) {
        // Header + checklist + clipping rows are painted; the summary label
        // takes whatever remains below them.
        const int rows = ((int)services_.size() + 1) / 2;
        const int consumed = 4 + 30 + 4 + rows * 16 + 4 + 14 + 14 + 4;
        b.removeFromTop(consumed);
        summaryLabel_.setBounds(b);
    } else {
        summaryLabel_.setBounds({});
    }

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
