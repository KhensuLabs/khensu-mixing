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
    chart_.setTitle(TRANS("Stem energy balance"));
    chart_.setHorizontal(true);
    chart_.setBaseline(0.0);
    addChildComponent(chart_);
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
    rebuildChart();
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
            self.rebuildChart();
            return TRANS("Energy balance mapped.");
        },
        std::move(options));
}

void TaxonomyEnergyPanel::rebuildChart()
{
    const bool show = !run_.hasError() && !buckets_.empty();
    chart_.setVisible(show);
    if (!show) return;
    // VIS-1 / VIS-4: one data colour; an estimate is dimmed by its
    // confidence and a missing bucket says so in words, the genre target is
    // a tick across the bar (named in the readout).
    std::vector<ui::chart::Bar> bars;
    double top = 0.0;
    bool haveTargets = false;
    juce::StringArray parts;
    for (const auto& bk : buckets_) {
        ui::chart::Bar b;
        b.label = bk.rollup ? bk.label + " (" + TRANS("sum") + ")" : bk.label;
        b.values = {bk.present ? static_cast<double>(bk.sharePct) / 100.0 : std::nan("")};
        if (!bk.present) b.note = TRANS("not detected");
        b.alpha = bk.present ? 0.45f + 0.55f * bk.confidence : 1.0f;
        if (bk.rollup) b.colour = KhensuTheme::vizCategory(1);
        if (bk.targetPct >= 0.0f) {
            b.target = static_cast<double>(bk.targetPct) / 100.0;
            haveTargets = true;
            top = std::max(top, *b.target);
        }
        top = std::max(top, static_cast<double>(bk.sharePct) / 100.0);
        bars.push_back(b);
        parts.add(bk.label + " " + (bk.present ? juce::String(bk.sharePct, 1) + " %" : TRANS("not detected")) +
                  (bk.targetPct >= 0.0f ? " (" + TRANS("target") + " " + juce::String(bk.targetPct, 1) + " %)" : juce::String()));
    }
    chart_.x = ui::chart::scales::percent(std::min(1.0, std::ceil((top + 0.05) * 10.0) / 10.0));
    chart_.setXRange(chart_.x.lo, chart_.x.hi);
    chart_.setBars(std::move(bars), {{TRANS("Energy share"), KhensuTheme::vizCategory(0)}});
    chart_.setLegend(haveTargets ? std::vector<ui::chart::LegendItem>{{TRANS("Genre target"), KhensuTheme::textPrimary,
                                                                       ui::chart::LegendItem::Kind::Line}}
                                 : std::vector<ui::chart::LegendItem>{});
    chart_.setSummary((provisional_ ? TRANS("Energy balance, estimated from the full mix") : TRANS("Stem energy balance")) +
                      ": " + parts.joinIntoString(", ") + ".");
}

void TaxonomyEnergyPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

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
    chart_.setBounds(errArea);
    chart_.setVisible(!run_.hasError() && !buckets_.empty());
}

} // namespace khensu
