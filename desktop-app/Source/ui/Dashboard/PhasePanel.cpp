// desktop-app/Source/ui/Dashboard/PhasePanel.cpp
#include "PhasePanel.h"
#include "../shared/PanelRunState.h"
#include <future>
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/ServiceErrorHint.h"
#include <cmath>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

void CorrelationStripComponent::setBands(std::vector<std::pair<juce::String, float>> bands)
{
    bands_ = std::move(bands);
    repaint();
}

void CorrelationStripComponent::paint(juce::Graphics& g)
{
    if (bands_.empty()) {
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(TRANS("Correlation by band: needs deep analysis."), getLocalBounds(),
                   juce::Justification::centredLeft, true);
        return;
    }

    auto bounds = getLocalBounds().toFloat();
    const float zeroY = bounds.getCentreY();               // correlation == 0
    const float halfH = bounds.getHeight() / 2.0f - 14.0f; // room for band labels below
    const int n = (int)bands_.size();
    const float slotW = bounds.getWidth() / (float)n;
    const float barW = juce::jmin(28.0f, slotW * 0.5f);

    g.setColour(KhensuTheme::textSecondary.withAlpha(0.3f));
    g.drawHorizontalLine(juce::roundToInt(zeroY), bounds.getX(), bounds.getRight());

    for (int i = 0; i < n; ++i) {
        const auto& [band, corr] = bands_[(size_t)i];
        const float v = juce::jlimit(-1.0f, 1.0f, corr);
        const float barH = std::abs(v) * halfH;
        const float x = bounds.getX() + slotW * i + (slotW - barW) / 2.0f;
        const juce::Colour colour = v < 0.0f   ? KhensuTheme::accentRed
                                    : v < 0.3f ? KhensuTheme::accentAmber
                                               : KhensuTheme::accentTeal;
        g.setColour(colour);
        if (v >= 0.0f)
            g.fillRoundedRectangle(x, zeroY - barH, barW, barH, 2.0f);
        else
            g.fillRoundedRectangle(x, zeroY, barW, barH, 2.0f);

        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(band, juce::Rectangle<float>(bounds.getX() + slotW * i, bounds.getBottom() - 12.0f, slotW, 12.0f),
                   juce::Justification::centred, true);
    }
}

// Every cached palette colour on this component, in one place (UI-01).
void PhasePanel::applyThemeColours()
{
    harshnessPeakLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    culpritLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    for (auto* l : {&monoCompatLabel_, &msImbalanceLabel_, &symmetryLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

PhasePanel::PhasePanel(IAppController& c) : PanelCard(c, "Phase")
{
    applyThemeColours();
    setExplainer(TRANS("Phase"),
                 TRANS("Overall phase correlation across the mix. Polarity issues and comb filtering cause "
                 "thin or hollow sound and poor mono compatibility. "
                 "Harshness (extended analysis) flags upper-frequency fatigue."),
                 "phase_correlation");

    corrMeter_.setRange(-1.0f, 1.0f);
    corrMeter_.setTargetZone(0.3f, 1.0f);
    addAndMakeVisible(corrMeter_);
    addAndMakeVisible(polarityBadge_);
    addAndMakeVisible(combBadge_);

    harshnessMeter_.setRange(0.0f, 1.0f);
    harshnessMeter_.setTargetZone(0.0f, 0.3f);
    harshnessMeter_.setVisible(false);
    addChildComponent(harshnessMeter_);

    harshnessPeakLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    harshnessPeakLabel_.setVisible(false);
    addChildComponent(harshnessPeakLabel_);

    for (auto* l : {&monoCompatLabel_, &msImbalanceLabel_, &symmetryLabel_}) {
        l->setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        l->setVisible(false);
        addAndMakeVisible(*l);
    }
    addAndMakeVisible(correlationStrip_);

    findCulpritButton_.setTooltip(TRANS("Requires separated stems: analyzes each stem's per-band side "
                                  "energy to name WHICH stem carries the anti-phase/wide content "
                                  "behind a failing correlation band."));
    findCulpritButton_.onClick = [this] { findCulprits(); };
    addAndMakeVisible(findCulpritButton_);

    culpritLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    culpritLabel_.setJustificationType(juce::Justification::topLeft);
    culpritLabel_.setVisible(false);
    addChildComponent(culpritLabel_);

    addChildComponent(errorState_);
    errorState_.onRetry([this] { findCulprits(); });
}

void PhasePanel::findCulprits()
{
    if (!controller_.stems().hasStemsAvailable()) {
        culpritLabel_.setText(TRANS("Requires separated stems - run Isolate Stems first."), juce::dontSendNotification);
        culpritLabel_.setVisible(true);
        resized();
        return;
    }
    findCulpritButton_.setEnabled(false);
    errorState_.setVisible(false);
    culpritLabel_.setText(TRANS("Analyzing stems..."), juce::dontSendNotification);
    culpritLabel_.setVisible(true);
    resized();

    std::map<std::string, std::string> stems;
    for (const char* name : {"vocals", "drums", "bass", "other"})
        stems[name] = controller_.getStemFile(name).getFullPathName().toStdString();

    juce::Component::SafePointer<PhasePanel> self(this);
    controller_.runInBackground([self, &work = controller_, stems]() {
        // Per-stem per-band width from the pan-balance analyzer: the stem
        // with the most side-energy share in a band is the mono-compat
        // culprit for that band.
        struct StemWidth {
            std::string stem;
            juce::String band;
            float width;     ///< 2S/(M+S) - saturates at 1
            float sideShare; ///< S/(M+S) - the honest "% side energy"
        };
        std::vector<StemWidth> widths;
        std::optional<ipc::BridgeError> lastError;
        // PERF-6: the four stems in parallel (one bridge each - a bridge is
        // not shared across threads), all joined before this job returns;
        // they used to wait on each other one after another.
        std::vector<std::pair<std::string, std::future<khensu::Result<juce::var, ipc::BridgeError>>>> pending;
        for (const auto& [name, path] : stems)
            pending.emplace_back(name, std::async(std::launch::async, [p = path] {
                                     return ipc::HttpServiceBridge{}.analyzePanBalance(p);
                                 }));
        for (auto& [name, future] : pending) {
            auto result = future.get();
            if (result.is_err()) {
                lastError = result.error();
                continue;
            }
            if (auto* arr = result.value()[af::PanBalanceResponse::band_summary].getArray())
                for (const auto& bv : *arr) {
                    const float width = (float)(double)bv[af::PanBandSummary::width];
                    const auto ss = bv[af::PanBandSummary::side_share];
                    // Older services lack side_share; width/2 is the same S/(M+S).
                    const float share = (ss.isDouble() || ss.isInt() || ss.isInt64()) ? (float)(double)ss
                                                                                      : width * 0.5f;
                    widths.push_back({name, bv[af::PanBandSummary::band].toString(), width, share});
                }
        }

        postIfCurrent(self, work, [widths, lastError](PhasePanel& panel) {
            if (widths.empty()) {
                const auto hint = lastError ? classifyServiceError(*lastError)
                                            : ServiceErrorHint{TRANS("Stem analysis returned no bands."),
                                                               TRANS("Retry; re-run Isolate Stems if it persists.")};
                panel.culpritLabel_.setVisible(false);
                panel.errorState_.setError(TRANS("Mono culprit analysis failed"), hint.message + "  " + hint.remedy);
                panel.errorState_.setVisible(true);
                panel.resized();
                return;
            }
            // Report the widest stem per band, worst first, skipping bands
            // where nothing is meaningfully wide.
            std::map<juce::String, StemWidth> worstPerBand;
            for (const auto& w : widths) {
                auto it = worstPerBand.find(w.band);
                if (it == worstPerBand.end() || w.width > it->second.width) worstPerBand[w.band] = w;
            }
            std::vector<StemWidth> ranked;
            for (const auto& [band, w] : worstPerBand)
                // width is the shared 2S/(M+S) metric (0.4 = 20% side energy).
                if (w.width > 0.40f) ranked.push_back(w);
            std::sort(ranked.begin(), ranked.end(),
                      [](const StemWidth& a, const StemWidth& b) { return a.width > b.width; });

            juce::StringArray lines;
            for (size_t i = 0; i < ranked.size() && i < 3; ++i)
                lines.add(ranked[i].band + ": " + TRANS("widest stem is") + " '" + juce::String(ranked[i].stem) +
                          "' (" + juce::String(juce::roundToInt(ranked[i].sideShare * 100.0f)) + "% " +
                          TRANS("side energy") + ")");
            panel.culpritLabel_.setColour(juce::Label::textColourId,
                                          lines.isEmpty() ? KhensuTheme::textSecondary : KhensuTheme::accentAmber);
            panel.culpritLabel_.setText(
                lines.isEmpty() ? TRANS("No stem carries significant side energy - mono fold-down is safe.")
                                : lines.joinIntoString("\n"),
                juce::dontSendNotification);
            panel.resized();
        }, [](PhasePanel& panel) { panel.findCulpritButton_.setEnabled(true); });
    });
}

void PhasePanel::setData(const AnalysisResult& r)
{
    // Demo values only before any analysis exists, and always labelled.
    // A real correlation of 0.0 is a legitimate reading (fully decorrelated),
    // so it is shown as-is - it used to be swapped for a fake 0.74.
    if (r.has_fast_analysis()) {
        corrMeter_.setValue(r.phase.overall_correlation,
                            TRANS("Correlation:") + " " + juce::String(r.phase.overall_correlation, 2));
        polarityBadge_.setBadge(r.phase.has_polarity_issue ? TRANS("Polarity issue") : TRANS("Polarity OK"),
                                r.phase.has_polarity_issue ? BadgeStatus::Fail : BadgeStatus::Pass);
        // CORE-09: the flag is an anti-phase-energy test, not comb filtering
        // (has_comb_filtering is now a deprecated alias of the same flag).
        combBadge_.setBadge(r.phase.has_anti_phase_energy ? TRANS("Anti-phase energy") : TRANS("No anti-phase energy"),
                            r.phase.has_anti_phase_energy ? BadgeStatus::Warn : BadgeStatus::Pass);
    } else {
        corrMeter_.setValue(0.74f, TRANS("Correlation: 0.74 (Demo)"));
        polarityBadge_.setBadge(TRANS("Polarity -"), BadgeStatus::Neutral);
        combBadge_.setBadge(TRANS("Anti-phase energy -"), BadgeStatus::Neutral);
    }

    if (r.harshness) {
        harshnessMeter_.setValue(r.harshness->score, TRANS("Harshness:") + " " + juce::String(r.harshness->score, 2));
        harshnessMeter_.setVisible(true);
        if (r.harshness->peak_hz) {
            harshnessPeakLabel_.setText(TRANS("Peak: ") + juce::String((int)*r.harshness->peak_hz) + " Hz",
                                        juce::dontSendNotification);
            harshnessPeakLabel_.setVisible(true);
        } else {
            harshnessPeakLabel_.setVisible(false);
        }
    } else if (!r.has_fast_analysis()) {
        // WIRE-05: a demo preview ONLY before any analysis, and labelled as
        // such - after a real analysis with no harshness data (deep pass
        // failed / service offline) invented numbers must not look real.
        harshnessMeter_.setValue(0.18f, TRANS("Harshness: 0.18 (Demo)"));
        harshnessMeter_.setVisible(true);
        harshnessPeakLabel_.setText(TRANS("Peak: 4200 Hz (Demo)"), juce::dontSendNotification);
        harshnessPeakLabel_.setVisible(true);
    } else {
        harshnessMeter_.setVisible(false);
        harshnessPeakLabel_.setText(TRANS("Harshness: needs the deep analysis (service)."), juce::dontSendNotification);
        harshnessPeakLabel_.setVisible(true);
    }

    // Mono compatibility: stereo share of the low end + worst per-band correlation.
    // FE-01: only fabricate a preview in the pre-analysis (demo) state, and label
    // it "(Demo)" like the sibling panels — never pass invented correlations off
    // as real. A real analysis with no mono-compat data (e.g. a mono source)
    // falls through to the honest hidden state below, not fake numbers.
    auto monoCompatOpt = r.mono_compat;
    const bool monoDemo = !monoCompatOpt.has_value() && !r.has_fast_analysis();
    if (monoDemo) {
        AnalysisResult::MonoCompatData mock;
        mock.low_end_stereo_pct = 8.0f;
        mock.per_band_correlation["Sub"] = 0.95f;
        mock.per_band_correlation["Bass"] = 0.88f;
        mock.per_band_correlation["Midrange"] = 0.75f;
        mock.per_band_correlation["Upper Mid"] = 0.65f;
        mock.per_band_correlation["Air"] = 0.42f;
        monoCompatOpt = mock;
    }

    if (monoCompatOpt) {
        float worstCorr = 1.0f;
        std::string worstBand;
        for (const auto& [band, corr] : monoCompatOpt->per_band_correlation) {
            if (corr < worstCorr) {
                worstCorr = corr;
                worstBand = band;
            }
        }
        juce::String text = juce::String(monoDemo ? "(Demo) " : "") + "Mono-compat: "
                            + juce::String(juce::roundToInt(monoCompatOpt->low_end_stereo_pct)) + "% stereo low-end";
        if (!worstBand.empty())
            text += "  |  weakest band: " + juce::String(worstBand) + " (" + juce::String(worstCorr, 2) + ")";
        monoCompatLabel_.setText(text, juce::dontSendNotification);
        monoCompatLabel_.setColour(juce::Label::textColourId,
                                   (monoCompatOpt->low_end_stereo_pct > 20.0f || worstCorr < 0.0f)
                                       ? KhensuTheme::accentAmber
                                       : KhensuTheme::textSecondary);
        monoCompatLabel_.setVisible(true);

        using MC = AnalysisResult::MonoCompatData;
        std::vector<std::pair<juce::String, float>> plotted;
        for (const char* band : {MC::kBandSub, MC::kBandBass, MC::kBandMidrange, MC::kBandUpperMid, MC::kBandAir}) {
            auto it = monoCompatOpt->per_band_correlation.find(band);
            if (it != monoCompatOpt->per_band_correlation.end()) {
                plotted.emplace_back(juce::String(band).replace("_", " "), it->second);
            } else {
                auto literalIt =
                    monoCompatOpt->per_band_correlation.find(juce::String(band).replace("_", " ").toStdString());
                if (literalIt != monoCompatOpt->per_band_correlation.end())
                    plotted.emplace_back(juce::String(band).replace("_", " "), literalIt->second);
            }
        }
        correlationStrip_.setBands(std::move(plotted));
    } else {
        monoCompatLabel_.setVisible(false);
        correlationStrip_.setBands({});
    }

    // Mid/side imbalance warning from extended analysis.
    if (r.mid_side_imbalance && !r.mid_side_imbalance->warning.empty()) {
        msImbalanceLabel_.setText(TRANS("M/S: ") + juce::String(r.mid_side_imbalance->warning), juce::dontSendNotification);
        msImbalanceLabel_.setColour(juce::Label::textColourId, r.mid_side_imbalance->imbalance_severity > 0.5f
                                                                   ? KhensuTheme::accentAmber
                                                                   : KhensuTheme::textSecondary);
        msImbalanceLabel_.setVisible(true);
    } else {
        msImbalanceLabel_.setVisible(false);
    }

    // Waveform peak symmetry & phase rotator recommendation
    if (r.has_python_analysis()) {
        juce::String symText = "Symmetry: " + juce::String(r.python_data.peak_asymmetry_ratio, 2) +
                               "x (skew: " + juce::String(r.python_data.skewness, 2) + ")";
        if (r.python_data.is_asymmetric) {
            symText += "  |  Recommend Phase Rotator (reclaim headroom)";
            symmetryLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
        } else {
            symmetryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        }
        symmetryLabel_.setText(symText, juce::dontSendNotification);
        symmetryLabel_.setVisible(true);
    } else {
        symmetryLabel_.setVisible(false);
    }

    resized();
    repaint();
}

void PhasePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    corrMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    auto row = b.removeFromTop(26);
    polarityBadge_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 6));
    row.removeFromLeft(12);
    combBadge_.setBounds(row);
    if (harshnessMeter_.isVisible()) {
        b.removeFromTop(10);
        harshnessMeter_.setBounds(b.removeFromTop(34));
    }
    if (harshnessPeakLabel_.isVisible()) {
        b.removeFromTop(4);
        harshnessPeakLabel_.setBounds(b.removeFromTop(18));
    }
    if (monoCompatLabel_.isVisible()) {
        b.removeFromTop(4);
        monoCompatLabel_.setBounds(b.removeFromTop(16));
    }
    if (msImbalanceLabel_.isVisible()) {
        b.removeFromTop(2);
        msImbalanceLabel_.setBounds(b.removeFromTop(16));
    }
    if (symmetryLabel_.isVisible()) {
        b.removeFromTop(2);
        symmetryLabel_.setBounds(b.removeFromTop(16));
    }
    b.removeFromTop(6);
    auto culpritRow = b.removeFromTop(22);
    findCulpritButton_.setBounds(culpritRow.removeFromLeft(140));
    if (culpritLabel_.isVisible()) {
        b.removeFromTop(2);
        culpritLabel_.setBounds(b.removeFromTop(44));
    }
    b.removeFromTop(6);
    if (errorState_.isVisible()) {
        errorState_.setBounds(b.withHeight(juce::jlimit(0, 130, b.getHeight())));
        b.removeFromTop(errorState_.getHeight() + 4);
    }
    correlationStrip_.setBounds(b);
}

} // namespace khensu
