// desktop-app/Source/ui/Dashboard/PanBalancePanel.cpp
#include "PanBalancePanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RecommendationExplainer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
/// Static portion of the Pan Energy Balance explainer. Live measured values
/// (worst band + bias magnitude) are prepended at analysis time via
/// explainer::buildMetricsBlock().
static const juce::String kPanBalanceBase =
    "Per-band left/right energy bias across the song: each lane is a frequency band, "
    "the line drifts left/right with the stereo energy. A steady centered line is a "
    "balanced band; a line living on one side means an instrument leans that way "
    "- deliberate panning or an accident, now you know which.\n\n"
    "Rules of thumb it checks: the sub band should be mono (wide low end breaks "
    "mono playback and wastes club/vinyl headroom), and no band should average more "
    "than ~15% to either side.";
} // anonymous namespace

// Every cached palette colour on this component, in one place (UI-01).
void PanBalancePanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

PanBalancePanel::PanBalancePanel(IAppController& c) : PanelCard(c, "Pan Energy Balance")
{
    applyThemeColours();
    setExplainer(TRANS("Pan Energy Balance"), kPanBalanceBase);

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Scan the stereo field per band."));

    warningsLabel_.setFont(KhensuTheme::labelFont(9.5f));
    warningsLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(warningsLabel_);
}

void PanBalancePanel::runAnalysis()
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
        *this, controller_, run_, TRANS("Analyzing..."),
        [target] { return ipc::HttpServiceBridge{}.analyzePanBalance(target.toStdString()); },
        [](PanBalancePanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("pan_balance", v); // WF-14
            self.bands_.clear();
            if (auto* arr = v[af::PanBalanceResponse::band_summary].getArray()) {
                for (const auto& bv : *arr) {
                    Band band;
                    band.name = bv[af::PanBandSummary::band].toString();
                    band.bias = (float)(double)bv[af::PanBandSummary::bias];
                    band.width = (float)(double)bv[af::PanBandSummary::width];
                    const auto ss = bv[af::PanBandSummary::side_share];
                    band.sideShare = (ss.isDouble() || ss.isInt() || ss.isInt64()) ? (float)(double)ss
                                                                                    : band.width * 0.5f;
                    const juce::var curves = v[af::PanBalanceResponse::bias_curves];
                    if (auto* curve = curves[juce::Identifier(band.name)].getArray())
                        for (const auto& e : *curve)
                            band.biasCurve.push_back((float)(double)e);
                    self.bands_.push_back(band);
                }
            }
            juce::StringArray lines;
            if (auto* arr = v[af::PanBalanceResponse::warnings].getArray())
                for (size_t i = 0; i < arr->size() && i < 2; ++i)
                    lines.add(arr->getReference((int)i).toString());
            self.warningsLabel_.setColour(juce::Label::textColourId,
                                          lines.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::accentAmber);
            self.warningsLabel_.setText(lines.isEmpty() ? juce::String("Stereo energy balanced in every band.")
                                                        : lines.joinIntoString("\n"),
                                        juce::dontSendNotification);

            // §9.5 - update the '?' explainer with the worst-offending band so the
            // popup shows live numbers alongside the general description.
            {
                float maxBias = 0.0f;
                juce::String maxBand;
                for (const auto& band : self.bands_) {
                    if (std::abs(band.bias) > std::abs(maxBias)) {
                        maxBias = band.bias;
                        maxBand = band.name;
                    }
                }
                self.setExplainer(
                    TRANS("Pan Energy Balance"),
                    explainer::buildMetricsBlock(
                        {{"Bands analyzed", juce::String((int)self.bands_.size()), "", ""},
                         {"Worst band (by bias)", maxBand.isEmpty() ? "-" : maxBand.toUpperCase(), "", ""},
                         {"Bias magnitude", maxBand.isEmpty() ? "-" : juce::String(juce::roundToInt(std::abs(maxBias) * 100.0f)),
                          maxBand.isEmpty() ? "" : "%", "threshold: > 15% = flagged"}}) +
                        kPanBalanceBase);
            }

            return juce::String((int)self.bands_.size()) + " bands analyzed.";
        },
        std::move(options));
}

void PanBalancePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || bands_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromBottom(40);
    auto lanes = b.toFloat();
    const float laneH = lanes.getHeight() / (float)bands_.size();

    for (size_t bi = 0; bi < bands_.size(); ++bi) {
        const auto& band = bands_[bi];
        juce::Rectangle<float> lane(lanes.getX(), lanes.getY() + bi * laneH, lanes.getWidth(), laneH);
        auto label = lane.removeFromLeft(64.0f);

        g.setFont(KhensuTheme::labelFont(8.5f));
        const bool warn = std::abs(band.bias) > 0.15f || (band.name == "sub" && band.width > 0.5f);
        g.setColour(warn ? KhensuTheme::accentAmber : KhensuTheme::textSecondary);
        auto nameArea = label.removeFromTop(label.getHeight() * 0.55f);
        g.drawText(band.name.toUpperCase(), nameArea.toNearestInt(), juce::Justification::bottomLeft, false);
        g.setFont(KhensuTheme::labelFont(7.5f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("side") + " " + juce::String(juce::roundToInt(band.sideShare * 100.0f)) + "%",
                   label.toNearestInt(), juce::Justification::topLeft, false);

        // Center (mono) line.
        const float cy = lane.getCentreY();
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.25f));
        g.drawHorizontalLine(juce::roundToInt(cy), lane.getX(), lane.getRight());

        // Bias curve: above center = right, below = left.
        const int n = (int)band.biasCurve.size();
        if (n >= 2) {
            juce::Path p;
            for (int i = 0; i < n; ++i) {
                const float x = lane.getX() + lane.getWidth() * (float)i / (float)(n - 1);
                const float y = cy - band.biasCurve[(size_t)i] * (laneH * 0.42f);
                if (i == 0)
                    p.startNewSubPath(x, y);
                else
                    p.lineTo(x, y);
            }
            g.setColour((warn ? KhensuTheme::accentAmber : KhensuTheme::accentTeal).withAlpha(0.85f));
            g.strokePath(p, juce::PathStrokeType(1.2f));
        }
    }
    // L/R legend.
    g.setFont(KhensuTheme::labelFont(7.5f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("R", (int)lanes.getRight() - 12, (int)lanes.getY(), 10, 10, juce::Justification::right);
    g.drawText("L", (int)lanes.getRight() - 12, (int)lanes.getBottom() - 10, 10, 10, juce::Justification::right);
}

void PanBalancePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(160));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    warningsLabel_.setBounds(b.removeFromBottom(40));

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
