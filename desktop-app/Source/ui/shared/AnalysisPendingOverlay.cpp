// desktop-app/Source/ui/shared/AnalysisPendingOverlay.cpp
#include "AnalysisPendingOverlay.h"
#include "KhensuTheme.h"

namespace khensu {

AnalysisPendingOverlay::AnalysisPendingOverlay()
{
    setInterceptsMouseClicks(true, true); // nothing underneath is reachable
    setOpaque(true);
    setWantsKeyboardFocus(false);
    setTitle(TRANS("Analysis in progress"));

    cancelButton_.setButtonText(TRANS("Cancel analysis"));
    cancelButton_.setTooltip(TRANS("Stop the analysis. Nothing is shown until an analysis finishes."));
    cancelButton_.onClick = [this] {
        if (onCancel) onCancel();
    };
    addAndMakeVisible(cancelButton_);

    rerunButton_.setButtonText(TRANS("Analyze again"));
    rerunButton_.setTooltip(TRANS("Run the full analysis of this session again."));
    rerunButton_.onClick = [this] {
        if (onRerun) onRerun();
    };
    addChildComponent(rerunButton_);
    setMode(Mode::Analyzing);
}

void AnalysisPendingOverlay::setMode(Mode mode, const juce::String& detail)
{
    if (mode == Mode::Analyzing && mode_ != Mode::Analyzing) {
        fraction_ = 0.0f;
        stageText_.clear();
        provisionalText_.clear();
    }
    mode_ = mode;
    detail_ = detail;
    const bool running = mode == Mode::Analyzing;
    cancelButton_.setVisible(running);
    rerunButton_.setVisible(!running);
    resized();
    repaint();
}

juce::String AnalysisPendingOverlay::stageLabel(const std::string& stage)
{
    auto starts = [&](const char* p) { return stage.rfind(p, 0) == 0; };
    if (starts("fast")) return TRANS("Measuring loudness, dynamics and spectrum");
    if (starts("deep_analysis:failed")) return TRANS("Deep analysis unavailable - continuing");
    if (starts("deep_analysis")) {
        // "deep_analysis:i:n" carries the service job's own sub-progress.
        const auto rest = stage.size() > 14 ? stage.substr(14) : std::string{};
        const auto colon = rest.find(':');
        if (colon != std::string::npos)
            return TRANS("Deep analysis") + " (" + juce::String(rest.substr(0, colon)) + "/" +
                   juce::String(rest.substr(colon + 1)) + ")";
        return TRANS("Deep analysis");
    }
    if (starts("sections")) return TRANS("Finding sections");
    if (starts("drums")) return TRANS("Analyzing drums and groove");
    if (starts("reference")) return TRANS("Comparing with the reference");
    if (starts("chords")) return TRANS("Reading chords and key");
    return TRANS("Analyzing");
}

float AnalysisPendingOverlay::overallFraction(const std::string& stage, int idx, int total)
{
    // The main pipeline reports stages 1..5 of a 6-slot scale (slot 6 is stem
    // separation, which runs separately and is not waited for); scale to the
    // five the gate waits on, short of 100% until the result is shown. The
    // follow-up scans are not waited for (PERF-1).
    if (total <= 0) return 0.0f;
    float f = juce::jlimit(0.0f, 1.0f, static_cast<float>(idx) / 5.0f);
    if (stage.rfind("deep_analysis:", 0) == 0) {
        const auto rest = stage.substr(14);
        const auto colon = rest.find(':');
        if (colon != std::string::npos) {
            const float sub = juce::String(rest.substr(0, colon)).getFloatValue();
            const float n = juce::String(rest.substr(colon + 1)).getFloatValue();
            if (n > 0.0f) f = juce::jlimit(0.0f, 1.0f, (static_cast<float>(idx) - 1.0f + sub / n) / 5.0f);
        }
    }
    return f * 0.95f;
}

void AnalysisPendingOverlay::updateStage(const std::string& stage, int idx, int total)
{
    if (stage.rfind("stems", 0) == 0) return; // separation is not part of the gate
    stageText_ = stageLabel(stage);
    fraction_ = juce::jmax(fraction_, overallFraction(stage, idx, total)); // never runs backwards
    repaint();
}

juce::String AnalysisPendingOverlay::provisionalSummary(float lufsIntegrated, float truePeakDbtp, bool hasClipping)
{
    return TRANS("Loudness") + " " + juce::String(lufsIntegrated, 1) + " LUFS   " + TRANS("True peak") + " " +
           juce::String(truePeakDbtp, 1) + " dBTP   " +
           (hasClipping ? TRANS("Clipping found") : TRANS("No clipping"));
}

void AnalysisPendingOverlay::setProvisional(float lufsIntegrated, float truePeakDbtp, bool hasClipping)
{
    provisionalText_ = provisionalSummary(lufsIntegrated, truePeakDbtp, hasClipping);
    resized();
    repaint();
}

void AnalysisPendingOverlay::resized()
{
    auto card = getLocalBounds().withSizeKeepingCentre(juce::jmin(520, getWidth() - 32), cardHeight());
    auto buttons = card.removeFromBottom(64).withSizeKeepingCentre(200, 36);
    cancelButton_.setBounds(buttons);
    rerunButton_.setBounds(buttons);
}

void AnalysisPendingOverlay::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgVoid); // opaque: nothing of the withheld results shows through

    const auto card =
        getLocalBounds().withSizeKeepingCentre(juce::jmin(520, getWidth() - 32), cardHeight()).toFloat();
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(card, 10.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(card.reduced(0.5f), 10.0f, 1.0f);

    auto b = card.reduced(28.0f, 22.0f).toNearestInt();
    juce::String title, body;
    switch (mode_) {
    case Mode::Analyzing:
        title = TRANS("Analyzing your mix");
        body = TRANS("Results and grades appear when every stage has finished, so nothing changes while you read.");
        break;
    case Mode::Cancelled:
        title = TRANS("Analysis cancelled");
        body = TRANS("Nothing is shown for a partial analysis. Run it again to see the results.");
        break;
    case Mode::Failed:
        title = TRANS("Analysis failed");
        body = detail_.isNotEmpty() ? detail_ : TRANS("The analysis could not finish.");
        break;
    }

    g.setColour(KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::uiFont(20.0f, juce::Font::bold));
    g.drawText(title, b.removeFromTop(30), juce::Justification::centredLeft, true);
    b.removeFromTop(6);
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::bodyFont(14.0f));
    g.drawFittedText(body, b.removeFromTop(40), juce::Justification::topLeft, 2);

    if (mode_ == Mode::Analyzing) {
        b.removeFromTop(10);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(12.0f));
        auto line = b.removeFromTop(18);
        g.drawText(juce::String(juce::roundToInt(fraction_ * 100.0f)) + "%", line.removeFromRight(48),
                   juce::Justification::centredRight, false);
        g.drawText(stageText_.isNotEmpty() ? stageText_ : TRANS("Starting"), line, juce::Justification::centredLeft,
                   true);
        b.removeFromTop(6);
        const auto bar = b.removeFromTop(6).toFloat();
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(bar, 3.0f);
        g.setColour(KhensuTheme::accentTeal);
        g.fillRoundedRectangle(bar.withWidth(juce::jmax(6.0f, bar.getWidth() * fraction_)), 3.0f);

        if (provisionalText_.isNotEmpty()) {
            b.removeFromTop(14);
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::labelFont(12.0f));
            g.drawText(TRANS("PROVISIONAL - FAST PASS"), b.removeFromTop(16), juce::Justification::centredLeft, true);
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::bodyFont(14.0f));
            g.drawText(provisionalText_, b.removeFromTop(22), juce::Justification::centredLeft, true);
        }
    }
}

int AnalysisPendingOverlay::cardHeight() const noexcept
{
    return mode_ == Mode::Analyzing && provisionalText_.isNotEmpty() ? 300 : 240;
}

} // namespace khensu
