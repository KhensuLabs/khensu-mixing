// desktop-app/Source/ui/Dashboard/SourceTriagePanel.cpp
#include "SourceTriagePanel.h"
#include "../shared/SessionEpoch.h"
#include <khensu/session/Session.h>
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RecommendationExplainer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
static const juce::String kSourceTriageBase =
    "A good vocal sound is ~80% recording, ~20% mixing. This scan looks for capture "
    "defects that no amount of mixing fully repairs and classifies each finding:\n\n"
    "FIXABLE - a standard DSP move fully repairs it (DC offset, hum notches).\n"
    "MITIGATE - tools reduce it but leave artifacts (declip, denoise, EQ).\n"
    "RE-RECORD - baked into the capture (heavy clipping, early reflections/comb "
    "filtering). Re-tracking will beat any plugin.\n\n"
    "Run it on raw takes before you start mixing - every minute here saves an hour "
    "of mixing around a flawed source.";

juce::Colour categoryColour(const juce::String& cat)
{
    if (cat == "rerecord") return KhensuTheme::accentRed;
    if (cat == "mitigable") return KhensuTheme::accentAmber;
    if (cat == "fixable") return KhensuTheme::accentTeal;
    return KhensuTheme::textTertiary;
}

juce::String categoryLabel(const juce::String& cat)
{
    if (cat == "rerecord") return "RE-RECORD";
    if (cat == "mitigable") return "MITIGATE";
    if (cat == "fixable") return "FIXABLE";
    return "INFO";
}
} // anonymous namespace

// Every cached palette colour on this component, in one place (UI-01).
void SourceTriagePanel::applyThemeColours()
{
}

SourceTriagePanel::SourceTriagePanel(IAppController& c) : PanelCard(c, "Fix-at-Source Triage")
{
    applyThemeColours();
    setExplainer(TRANS("Fix-at-Source Triage"), kSourceTriageBase, "recommendation_engine");

    scanButton_.onClick = [this] { runTriage(); };
    addAndMakeVisible(scanButton_);
    run_.onRetry([this] { runTriage(); });
    run_.setIdle(TRANS("Scan the source file for capture defects."));

    verdictLabel_.setFont(KhensuTheme::labelFont(11.0f).boldened());
    addAndMakeVisible(verdictLabel_);

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
}

void SourceTriagePanel::runTriage()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    const juce::String target(s->files[0].path);

    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Scanning..."),
        // The session's mix, graded as the intake grades it (CON-7): the
        // intake's cached triage comes back instead of a second scan.
        [target] { return ipc::HttpServiceBridge{}.analyzeSourceTriage(target.toStdString(), "mix"); },
        [](SourceTriagePanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("source_triage", v); // WF-14
            self.rows_.clear();

            const juce::String verdict = v[af::SourceTriageResponse::verdict].toString();
            juce::Colour vcol = verdict == "clean"                ? KhensuTheme::accentTeal
                                : verdict.startsWith("re-record") ? KhensuTheme::accentRed
                                                                  : KhensuTheme::accentAmber;
            self.verdictLabel_.setColour(juce::Label::textColourId, vcol);
            self.verdictLabel_.setText(TRANS("Verdict: ") + verdict.toUpperCase(), juce::dontSendNotification);

            int count = 0;
            if (auto* arr = v[af::SourceTriageResponse::findings].getArray()) {
                for (const auto& f : *arr) {
                    auto row = std::make_unique<FindingRow>();
                    row->title = f[af::SourceTriageFinding::title].toString();
                    row->detail = f[af::SourceTriageFinding::detail].toString();
                    row->category = f[af::SourceTriageFinding::category].toString();
                    row->severity = (float)(double)f[af::SourceTriageFinding::severity];
                    self.content_.addAndMakeVisible(*row);
                    self.rows_.push_back(std::move(row));
                    ++count;
                }
            }

            // Update the explainer popup with the scan's live results
            self.setExplainer(TRANS("Fix-at-Source Triage"),
                              explainer::buildMetricsBlock({{"Verdict", verdict.toUpperCase(), "", ""},
                                                            {"Total findings", juce::String(count), "", ""}}) +
                                  kSourceTriageBase,
                              "recommendation_engine");

            return count == 0 ? TRANS("No capture defects detected.") : juce::String(count) + " " + TRANS("finding(s).");
        },
        std::move(options));
}

void SourceTriagePanel::FindingRow::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(2);
    const auto col = categoryColour(category);

    g.setColour(col.withAlpha(0.08f));
    g.fillRoundedRectangle(b.toFloat(), 4.0f);
    g.setColour(col.withAlpha(0.9f));
    g.fillRect(b.getX(), b.getY(), 3, b.getHeight());

    auto inner = b.reduced(8, 4);
    auto top = inner.removeFromTop(14);
    g.setFont(KhensuTheme::bodyFont(11.0f).boldened());
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(title, top.removeFromLeft(top.getWidth() - 74), juce::Justification::centredLeft, true);
    g.setFont(KhensuTheme::labelFont(9.0f).boldened());
    g.setColour(col);
    g.drawText(categoryLabel(category), top, juce::Justification::centredRight, false);

    g.setFont(KhensuTheme::labelFont(9.5f));
    g.setColour(KhensuTheme::textSecondary);
    g.drawFittedText(detail, inner, juce::Justification::topLeft, 3);
}

void SourceTriagePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto controls = b.removeFromTop(24);
    scanButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(8);
    verdictLabel_.setBounds(controls);
    b.removeFromTop(2);
    run_.label().setBounds(b.removeFromTop(14));
    b.removeFromTop(4);
    // The error state takes the findings list's place while it shows.
    viewport_.setVisible(!run_.hasError());
    viewport_.setBounds(b);
    run_.errorState().setBounds(b);

    const int rowH = 64;
    content_.setSize(juce::jmax(50, viewport_.getWidth() - 10), juce::jmax(1, (int)rows_.size() * (rowH + 4)));
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, content_.getWidth(), rowH);
        y += rowH + 4;
    }
}

} // namespace khensu
