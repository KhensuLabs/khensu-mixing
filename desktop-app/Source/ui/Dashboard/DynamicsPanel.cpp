// desktop-app/Source/ui/Dashboard/DynamicsPanel.cpp
#include "DynamicsPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RecommendationExplainer.h"

namespace khensu {

namespace {
const juce::String kDynamicsExplainerBase =
    "Dynamic range (EBU DR) and crest factor describe how much the loud moments stand "
    "above the average. Very low values indicate heavy compression/limiting; very high "
    "values can feel unpolished for dense genres. "
    "Kick/snare regularity (0-1) measures timing consistency from deep analysis.";

} // anonymous namespace

// VIS-1 / VIS-4: both strips are ChartKit bar charts - crest in dB on the
// value axis, one data colour, the values written; low crest (squashed) is
// read from the number and the axis, not from a red-to-blue ramp.
SectionDynamicsHeatStrip::SectionDynamicsHeatStrip()
{
    setTitle(TRANS("Crest factor per section"));
    y.lo = 0.0;
    y.hi = 24.0;
    y.unit = "dB";
    setBaseline(0.0);
    setFixedMargins(34, 14, 6, 16);
    valueText = [](size_t, size_t, double v) { return juce::String(v, 1); };
}

void SectionDynamicsHeatStrip::setSections(const std::vector<float>& crestDbValues, const std::vector<juce::String>& labels)
{
    values_ = crestDbValues;
    std::vector<ui::chart::Bar> bars;
    juce::StringArray parts;
    for (size_t i = 0; i < values_.size(); ++i) {
        ui::chart::Bar b;
        b.label = i < labels.size() && labels[i].isNotEmpty() ? labels[i] : juce::String(static_cast<int>(i) + 1);
        b.values = {values_[i]};
        bars.push_back(b);
        parts.add(b.label + " " + juce::String(values_[i], 1) + " dB");
    }
    setBars(std::move(bars), {{TRANS("Crest"), KhensuTheme::vizCategory(0)}});
    setSummary(TRANS("Crest factor per section") + ": " + parts.joinIntoString(", ") + ".");
}

void SectionDynamicsHeatStrip::paintOverlay(juce::Graphics& g, juce::Rectangle<float> plot)
{
    g.setFont(font(0.9f));
    g.setColour(style().muted);
    g.drawText(TRANS("Crest factor per section"), juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(getWidth()), plot.getY()),
               juce::Justification::centredLeft, false);
}

CrestHistogramStrip::CrestHistogramStrip()
{
    setTitle(TRANS("Crest factor distribution"));
    setBaseline(0.0);
    setFixedMargins(34, 14, 6, 16);
    y.unit = "";
    y.decimals = 0;
    valueText = [](size_t, size_t, double v) { return juce::String(juce::roundToInt(v)); };
}

void CrestHistogramStrip::setSections(const std::vector<float>& crestDbValues)
{
    counts_.fill(0);
    maxCount_ = 0;
    hasData_ = !crestDbValues.empty();

    constexpr float binWidth = (kMaxDb - kMinDb) / (float)kNumBins;
    for (float v : crestDbValues) {
        if (v <= 0.001f) continue; // 0 = not computed for this section
        int bin = (int)((juce::jlimit(kMinDb, kMaxDb - 0.01f, v) - kMinDb) / binWidth);
        bin = juce::jlimit(0, kNumBins - 1, bin);
        ++counts_[(size_t)bin];
    }
    for (int c : counts_)
        maxCount_ = juce::jmax(maxCount_, c);

    std::vector<ui::chart::Bar> bars;
    juce::StringArray parts;
    for (int i = 0; i < kNumBins; ++i) {
        const int lo = juce::roundToInt(kMinDb + binWidth * (float)i), hi = juce::roundToInt(kMinDb + binWidth * (float)(i + 1));
        ui::chart::Bar b;
        b.label = juce::String(lo) + "-" + juce::String(hi);
        b.values = {static_cast<double>(counts_[(size_t)i])};
        bars.push_back(b);
        if (counts_[(size_t)i] > 0) parts.add(b.label + " dB: " + juce::String(counts_[(size_t)i]));
    }
    y.lo = 0.0;
    y.hi = juce::jmax(1.0, static_cast<double>(maxCount_));
    setBars(std::move(bars), {{TRANS("Sections"), KhensuTheme::vizCategory(0)}});
    setSummary(TRANS("Sections per crest-factor range") + ": " + parts.joinIntoString(", ") + ".");
}

void CrestHistogramStrip::paintOverlay(juce::Graphics& g, juce::Rectangle<float> plot)
{
    g.setFont(font(0.9f));
    g.setColour(style().muted);
    g.drawText(TRANS("Sections per crest range (dB)"), juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(getWidth()), plot.getY()),
               juce::Justification::centredLeft, false);
}

// Every cached palette colour on this component, in one place (UI-01).
void DynamicsPanel::applyThemeColours()
{
    peakToRms_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

DynamicsPanel::DynamicsPanel(IAppController& c) : PanelCard(c, "Dynamics & Compression")
{
    applyThemeColours();
    setExplainer(TRANS("Dynamics"), kDynamicsExplainerBase, "dynamics_range");
    drMeter_.setRange(0.0f, 20.0f);
    drMeter_.setTargetZone(6.0f, 12.0f);
    crestMeter_.setRange(0.0f, 24.0f);
    crestMeter_.setTargetZone(8.0f, 16.0f);
    kickRegMeter_.setRange(0.0f, 1.0f);
    kickRegMeter_.setTargetZone(0.7f, 1.0f);
    snareRegMeter_.setRange(0.0f, 1.0f);
    snareRegMeter_.setTargetZone(0.7f, 1.0f);

    addAndMakeVisible(drMeter_);
    addAndMakeVisible(crestMeter_);
    addAndMakeVisible(kickRegMeter_);
    addAndMakeVisible(snareRegMeter_);
    kickRegMeter_.setVisible(false);
    snareRegMeter_.setVisible(false);

    peakToRms_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::plain));
    addAndMakeVisible(peakToRms_);

    addAndMakeVisible(dynamicsHeatStrip_);
    dynamicsHeatStrip_.setVisible(false);

    addAndMakeVisible(crestHistogram_);
    crestHistogram_.setVisible(false);

    addAndMakeVisible(compareChip_);
    compareChip_.setVisible(false);
}

void DynamicsPanel::setData(const AnalysisResult& r)
{
    drMeter_.setValue(r.dynamics.dynamic_range_db,
                      "Dynamic Range: " + juce::String(r.dynamics.dynamic_range_db, 1) + " DR");
    crestMeter_.setValue(r.dynamics.crest_factor_db,
                         "Crest Factor: " + juce::String(r.dynamics.crest_factor_db, 1) + " dB");
    peakToRms_.setText(TRANS("Peak-to-RMS: ") + juce::String(r.dynamics.peak_to_rms_db, 1) + " dB",
                       juce::dontSendNotification);

    if (r.drums) {
        kickRegMeter_.setValue(r.drums->kick_timing_regularity,
                               "Kick Regularity: " + juce::String(r.drums->kick_timing_regularity, 2) +
                                   (r.drums->kick_bass_clash ? "  (!) bass clash" : ""));
        snareRegMeter_.setValue(r.drums->snare_timing_regularity,
                                "Snare Regularity: " + juce::String(r.drums->snare_timing_regularity, 2));
        kickRegMeter_.setVisible(true);
        snareRegMeter_.setVisible(true);
    } else {
        kickRegMeter_.setVisible(false);
        snareRegMeter_.setVisible(false);
    }

    if (r.section_analysis && !r.section_analysis->sections.empty()) {
        std::vector<float> crestVals;
        std::vector<juce::String> crestLabels;
        for (const auto& sec : r.section_analysis->sections)
            if (sec.crest_db > 0.001f) {
                crestVals.push_back(sec.crest_db);
                crestLabels.push_back(juce::String(sec.label));
            }
        if (!crestVals.empty()) {
            dynamicsHeatStrip_.setSections(crestVals, crestLabels);
            dynamicsHeatStrip_.setVisible(true);
            crestHistogram_.setSections(crestVals);
            crestHistogram_.setVisible(true);
        } else {
            dynamicsHeatStrip_.setVisible(false);
            crestHistogram_.setVisible(false);
        }
    } else {
        dynamicsHeatStrip_.setVisible(false);
        crestHistogram_.setVisible(false);
    }

    const auto* rec = explainer::findTopRecommendation(r.recommendations, {"over_compressed", "under_compressed"});
    setExplainer(TRANS("Dynamics"), explainer::appendReasoningTrace(kDynamicsExplainerBase, rec), "dynamics_range");

    lastRefDeltas_ = r.reference_deltas;
    updateCompareChip();

    resized();
    repaint();
}

void DynamicsPanel::setCompareModeEnabled(bool enabled)
{
    compareEnabled_ = enabled;
    updateCompareChip();
}

void DynamicsPanel::updateCompareChip()
{
    if (compareEnabled_ && lastRefDeltas_.has_value()) {
        if (const auto* d = CompareDeltaChip::find(*lastRefDeltas_, "crest_db")) {
            compareChip_.setDelta(d->delta, " dB");
            resized();
            return;
        }
    }
    compareChip_.clear();
    resized();
}

void DynamicsPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    drMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    crestMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    peakToRms_.setBounds(b.removeFromTop(18));

    if (compareChip_.isVisible()) {
        b.removeFromTop(4);
        compareChip_.setBounds(b.removeFromTop(16));
    }

    if (kickRegMeter_.isVisible()) {
        b.removeFromTop(8);
        kickRegMeter_.setBounds(b.removeFromTop(34));
        b.removeFromTop(6);
        snareRegMeter_.setBounds(b.removeFromTop(34));
    }

    if (dynamicsHeatStrip_.isVisible()) {
        b.removeFromTop(8);
        dynamicsHeatStrip_.setBounds(b.removeFromTop(72));
    } else {
        dynamicsHeatStrip_.setBounds({});
    }

    if (crestHistogram_.isVisible()) {
        b.removeFromTop(6);
        crestHistogram_.setBounds(b.removeFromTop(64));
    } else {
        crestHistogram_.setBounds({});
    }
}

} // namespace khensu
