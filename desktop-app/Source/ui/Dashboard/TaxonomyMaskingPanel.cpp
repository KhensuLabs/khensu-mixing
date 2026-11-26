// desktop-app/Source/ui/Dashboard/TaxonomyMaskingPanel.cpp
#include "TaxonomyMaskingPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void TaxonomyMaskingPanel::applyThemeColours()
{
    noteLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    fixLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
}

TaxonomyMaskingPanel::TaxonomyMaskingPanel(IAppController& c) : PanelCard(c, "Masking & Frequency Collision")
{
    applyThemeColours();
    setExplainer(TRANS("Masking & Frequency Collision"),
                 TRANS("The hottest frequency collisions between the canonical stem buckets "
                 "(Vocals, Bass, Synth, Kick, Snare, Percussion, Drums, Atmosphere/FX). "
                 "Each pair's score (0-100%) is the overlap of their band-energy "
                 "profiles: two buckets fighting for the same bands mask each other, "
                 "so the louder one wins and the other turns to mud.\n\n"
                 "Kick/snare/percussion and synth/atmosphere are derived splits of the "
                 "drum and 'other' stems - treat their pairs as estimates. "
                 "Fix the top pair first: EQ a pocket for the more important bucket, "
                 "or sidechain the other out of its way."),
                 "masking_collision");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (auto-runs once they exist)."));

    noteLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(noteLabel_);

    fixLabel_.setFont(KhensuTheme::labelFont(type::caption));
    fixLabel_.setJustificationType(juce::Justification::topLeft);
    fixLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(fixLabel_);
}

juce::String TaxonomyMaskingPanel::suggestFix(const juce::String& a, const juce::String& b, float score)
{
    juce::StringArray p{a.toLowerCase(), b.toLowerCase()};
    p.sort(false);
    const juce::String key = p.joinIntoString("+");

    juce::String fix;
    // Vocals - protect intelligibility.
    if (key == "drums+vocals" || key == "snare+vocals")
        fix = "Sidechain the drums/snare 2-6 kHz under the vocal, or a 2-3 dB dip there in vocal sections.";
    else if (key == "bass+vocals")
        fix = "High-pass the bass ~100-150 Hz so it stops crowding the vocal's low-mids.";
    else if (key == "synth+vocals" || key == "other+vocals" || key == "atmosphere_fx+vocals")
        fix = "Carve a narrow dynamic notch in the synth/pad around the vocal's 200 Hz-3 kHz core.";
    else if (key == "kick+vocals" || key == "percussion+vocals")
        fix = "Usually benign; if muddy, trim shared low-mids (200-400 Hz) on the less important element.";
    // Low end - kick vs bass is the classic clash.
    else if (key == "bass+kick" || key == "bass+drums")
        fix = "Sidechain the bass from the kick, or split at ~60-80 Hz (kick owns sub, bass owns above).";
    // Synth / atmosphere vs rhythm.
    else if (key == "drums+synth" || key == "snare+synth" || key == "kick+synth")
        fix = "EQ-cut the synth at the drum transient bands (150-250 Hz body, 2-5 kHz crack).";
    else if (key == "bass+synth")
        fix = "Low-pass the bass' upper harmonics and high-pass the synth ~200 Hz to un-stack the low end.";
    else if (key == "atmosphere_fx+synth" || key == "other+synth")
        fix = "Pan or width-separate them, or dip whichever is less important where the other peaks.";
    else if (p[0].contains("percussion") || p[1].contains("percussion"))
        fix = "Percussion collisions are usually high-band; a gentle 4-8 kHz dip on the busier element helps.";
    else
        fix = "Complementary EQ: cut one bucket where the other is strongest, or separate them in the stereo field.";

    const juce::String sev = score >= 0.65f ? "High" : score >= 0.40f ? "Moderate" : "Low";
    return "Top fix (" + sev + " " + juce::String((int)std::round(score * 100.0f)) + "%): " + fix;
}

void TaxonomyMaskingPanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Stems not available yet - separation runs at intake."));
        return;
    }
    const juce::String vocals = controller_.getStemFile("vocals").getFullPathName();
    const juce::String drums = controller_.getStemFile("drums").getFullPathName();
    const juce::String bass = controller_.getStemFile("bass").getFullPathName();
    const juce::String other = controller_.getStemFile("other").getFullPathName();

    FetchOptions options;
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Scoring collisions..."),
        [vocals, drums, bass, other] {
            return ipc::HttpServiceBridge{}.analyzeStemTaxonomy(vocals.toStdString(), drums.toStdString(),
                                                                bass.toStdString(), other.toStdString());
        },
        [](TaxonomyMaskingPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("taxonomy_masking", v); // WF-14
            self.pairs_.clear();
            if (auto* arr = v[af::StemTaxonomyResponse::collisions].getArray()) {
                for (const auto& cv : *arr) {
                    if (self.pairs_.size() >= 8) break; // display cap
                    Pair p;
                    p.a = cv[af::StemTaxonomyCollision::a].toString();
                    p.b = cv[af::StemTaxonomyCollision::b].toString();
                    p.score = (float)(double)cv[af::StemTaxonomyCollision::score];
                    self.pairs_.push_back(std::move(p));
                }
            }
            self.noteLabel_.setText(v[af::StemTaxonomyResponse::taxonomy_note].toString(), juce::dontSendNotification);
            // Actionable remedy for the hottest pair (spec §3).
            if (!self.pairs_.empty() && self.pairs_.front().score >= 0.40f)
                self.fixLabel_.setText(
                    suggestFix(self.pairs_.front().a, self.pairs_.front().b, self.pairs_.front().score),
                    juce::dontSendNotification);
            else
                self.fixLabel_.setText({}, juce::dontSendNotification);
            return self.pairs_.empty() ? TRANS("No significant collisions.") : TRANS("Hottest pairs first.");
        },
        std::move(options));
}

void TaxonomyMaskingPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (run_.hasError() || pairs_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);                                          // controls row
    b.removeFromBottom(22);                                       // note label
    if (fixLabel_.getText().isNotEmpty()) b.removeFromBottom(28); // suggested-fix line (two rows tall)

    const int rowH = juce::jmax(16, b.getHeight() / (int)pairs_.size());
    const int labelW = 150;

    auto riskColour = [](float s) {
        return s >= 0.65f ? KhensuTheme::accentRed : s >= 0.40f ? KhensuTheme::accentAmber : KhensuTheme::accentTeal;
    };
    auto pretty = [](const juce::String& id) {
        return id == "atmosphere_fx" ? juce::String("Atmos/FX") : id.substring(0, 1).toUpperCase() + id.substring(1);
    };

    for (const auto& p : pairs_) {
        auto row = b.removeFromTop(rowH);
        const auto col = riskColour(p.score);

        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(pretty(p.a) + " x " + pretty(p.b), row.removeFromLeft(labelW).reduced(0, 1),
                   juce::Justification::centredLeft, false);

        auto barArea = row.reduced(2, rowH > 22 ? 5 : 2);
        g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
        g.fillRoundedRectangle(barArea.toFloat(), 3.0f);
        g.setColour(col.withAlpha(0.8f));
        g.fillRoundedRectangle(
            barArea.toFloat().withWidth(barArea.toFloat().getWidth() * juce::jlimit(0.0f, 1.0f, p.score)), 3.0f);

        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(juce::String((int)std::round(p.score * 100.0f)) + "%", barArea.reduced(6, 0),
                   juce::Justification::centredRight, false);
    }
}

void TaxonomyMaskingPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(160));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    noteLabel_.setBounds(b.removeFromBottom(20));
    fixLabel_.setBounds(b.removeFromBottom(28));

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
