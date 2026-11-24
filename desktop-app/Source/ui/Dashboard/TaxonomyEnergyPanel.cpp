// desktop-app/Source/ui/Dashboard/TaxonomyEnergyPanel.cpp
#include "TaxonomyEnergyPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void TaxonomyEnergyPanel::applyThemeColours()
{
}

TaxonomyEnergyPanel::TaxonomyEnergyPanel(IAppController& c) : PanelCard(c, "Stem Energy Balance")
{
    applyThemeColours();
    setExplainer(TRANS("Stem Energy Balance"),
                 TRANS("How the mix's energy is distributed across the canonical stem "
                 "taxonomy: Vocals, Bass, Synth, Kick, Snare, Percussion, Drums "
                 "(kick+snare+percussion together), Atmosphere/FX, and Other.\n\n"
                 "Vocals/bass/drums come straight from stem separation. Kick, snare "
                 "and percussion are band-splits of the drum stem; synth and "
                 "atmosphere/FX are the tonal vs. diffuse components of the 'other' "
                 "stem - those bars are dimmed to their confidence so estimates never "
                 "read as measurements. Greyed rows mean the bucket wasn't detected "
                 "in this song at all."),
                 "stem_taxonomy");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (auto-runs once they exist)."));
}

void TaxonomyEnergyPanel::setProvisionalEstimate(float low, float mid, float high)
{
    // Don't clobber a real (stem-based) result if one already landed.
    if (!buckets_.empty() && !provisional_) return;
    const float total = juce::jmax(0.001f, low + mid + high);
    provisional_ = true;
    buckets_.clear();
    // Map the three broad bands onto the buckets they roughly correspond to,
    // so the panel reads as "estimated" without fabricating per-stem detail.
    auto add = [this](const char* id, const char* label, float share) {
        Bucket b;
        b.id = id;
        b.label = label;
        b.sharePct = share * 100.0f;
        b.confidence = 0.3f;
        b.present = share > 0.01f;
        b.rollup = false;
        b.targetPct = -1.0f;
        buckets_.push_back(std::move(b));
    };
    add("bass", "Low (bass/kick)", low / total);
    add("drums", "Low-mid/body", mid * 0.5f / total);
    add("vocals", "Mid (vocal/synth)", mid * 0.5f / total);
    add("atmosphere_fx", "High (air/cymbals)", high / total);
    // Status text only: a scan may be in flight, so the run state stays as is.
    run_.label().setText(TRANS("Estimated from full mix - refining when stems separate..."), juce::dontSendNotification);
    repaint();
}

void TaxonomyEnergyPanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        if (buckets_.empty())
            run_.setIdle(TRANS("Stems not available yet - separation runs at intake."));
        return;
    }
    provisional_ = false; // a real stem pass supersedes the estimate
    const juce::String vocals = controller_.getStemFile("vocals").getFullPathName();
    const juce::String drums = controller_.getStemFile("drums").getFullPathName();
    const juce::String bass = controller_.getStemFile("bass").getFullPathName();
    const juce::String other = controller_.getStemFile("other").getFullPathName();
    // §8: pass the session's genre so the response carries per-bucket target
    // shares to overlay against the measured energy.
    juce::String genreId;
    if (const auto* s = controller_.getActiveSession()) genreId = juce::String(s->target.primary_genre);

    FetchOptions options;
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Classifying stems..."),
        [vocals, drums, bass, other, genreId] {
            return ipc::HttpServiceBridge{}.analyzeStemTaxonomy(vocals.toStdString(), drums.toStdString(),
                                                                bass.toStdString(), other.toStdString(),
                                                                genreId.toStdString());
        },
        [](TaxonomyEnergyPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("taxonomy_energy", v); // WF-14
            self.buckets_.clear();
            if (auto* arr = v[af::StemTaxonomyResponse::buckets].getArray()) {
                for (const auto& bv : *arr) {
                    Bucket b;
                    b.id = bv[af::StemTaxonomyBucket::id].toString();
                    b.label = bv[af::StemTaxonomyBucket::label].toString();
                    b.sharePct = (float)(double)bv[af::StemTaxonomyBucket::energy_share_pct];
                    b.confidence = (float)(double)bv[af::StemTaxonomyBucket::confidence];
                    b.present = (bool)bv[af::StemTaxonomyBucket::present];
                    b.rollup = b.id == "drums";
                    b.targetPct = bv[af::StemTaxonomyBucket::target_share_pct].isVoid() ? -1.0f : (float)(double)bv[af::StemTaxonomyBucket::target_share_pct];
                    self.buckets_.push_back(std::move(b));
                }
            }
            return TRANS("Energy balance mapped.");
        },
        std::move(options));
}

void TaxonomyEnergyPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (run_.hasError() || buckets_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28); // controls row

    const int rowH = juce::jmax(14, b.getHeight() / (int)buckets_.size());
    const int labelW = 96;
    float maxShare = 1.0f;
    bool haveTargets = false;
    for (const auto& bk : buckets_) {
        maxShare = juce::jmax(maxShare, bk.sharePct);
        if (bk.targetPct >= 0.0f) {
            maxShare = juce::jmax(maxShare, bk.targetPct);
            haveTargets = true;
        }
    }

    for (const auto& bk : buckets_) {
        auto row = b.removeFromTop(rowH);
        const juce::Colour base = bk.rollup ? KhensuTheme::accentBlue : KhensuTheme::accentTeal;
        const bool grey = !bk.present;
        const float alpha = grey ? 0.25f : (0.45f + 0.55f * bk.confidence);

        g.setFont(KhensuTheme::labelFont(9.5f));
        g.setColour(grey ? KhensuTheme::textTertiary : KhensuTheme::textSecondary);
        g.drawText(bk.label, row.removeFromLeft(labelW).reduced(0, 1), juce::Justification::centredLeft, false);

        auto barArea = row.reduced(2, rowH > 20 ? 4 : 2);
        g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
        g.fillRoundedRectangle(barArea.toFloat(), 3.0f);

        if (!grey) {
            const float frac = juce::jlimit(0.0f, 1.0f, bk.sharePct / maxShare);
            auto fill = barArea.toFloat().withWidth(barArea.toFloat().getWidth() * frac);
            g.setColour(base.withAlpha(alpha));
            g.fillRoundedRectangle(fill, 3.0f);
        }

        // §8: genre target as a vertical tick on the bar; the bar over/under
        // the tick shows whether this bucket is hot or light for the genre.
        if (bk.targetPct >= 0.0f) {
            const float tx = barArea.getX() + juce::jlimit(0.0f, 1.0f, bk.targetPct / maxShare) * barArea.getWidth();
            g.setColour(KhensuTheme::accentAmber.withAlpha(0.9f));
            g.drawLine(tx, (float)barArea.getY() - 1.0f, tx, (float)barArea.getBottom() + 1.0f, 1.6f);
        }

        g.setFont(KhensuTheme::monoFont(9.0f));
        g.setColour(grey ? KhensuTheme::textTertiary.withAlpha(0.6f) : KhensuTheme::textPrimary);
        g.drawText(grey ? juce::String("not detected") : juce::String(bk.sharePct, 1) + "%", barArea.reduced(6, 0),
                   juce::Justification::centredRight, false);
    }

    if (haveTargets) {
        g.setFont(KhensuTheme::labelFont(8.0f));
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.85f));
        g.drawText("| = genre target", bodyArea().removeFromBottom(12).reduced(4, 0), juce::Justification::centredRight,
                   false);
    }
}

void TaxonomyEnergyPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(170));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
