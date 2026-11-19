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

// Low crest factor (heavily compressed/limited) reads hot (red); high crest
// (dynamic, untouched) reads cool (blue) - inverse of the loudness heat map,
// since here "hot" means "aggressively squashed", not "loud".
juce::Colour dynamicsHeatColourFor(float crestDb)
{
    constexpr float lo = 4.0f, hi = 20.0f;                                // matches crestMeter_'s musical range
    const float t = juce::jlimit(0.0f, 1.0f, (crestDb - lo) / (hi - lo)); // 0=squashed, 1=dynamic
    if (t < 0.5f) return KhensuTheme::accentRed.interpolatedWith(KhensuTheme::accentAmber, t / 0.5f);
    return KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentBlue, (t - 0.5f) / 0.5f);
}
} // anonymous namespace

void SectionDynamicsHeatStrip::paint(juce::Graphics& g)
{
    if (values_.empty()) return;

    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("SECTION COMPRESSION (3D)", getLocalBounds().withHeight(12), juce::Justification::centredLeft, false);

    constexpr float kDepth = 5.0f;
    auto b = getLocalBounds().withTrimmedTop(13).toFloat();
    b.removeFromRight(kDepth);
    b.removeFromTop(kDepth);
    if (b.getHeight() < 4.0f) return;

    const int n = (int)values_.size();
    const float spacing = 3.0f;
    const float cellW = (b.getWidth() - spacing * (n - 1)) / (float)n;

    for (int i = 0; i < n; ++i) {
        constexpr float lo = 4.0f, hi = 20.0f;
        const float t = juce::jlimit(0.0f, 1.0f, (values_[i] - lo) / (hi - lo));
        const float cx = b.getX() + (float)i * (cellW + spacing);
        // Squashed sections read as short/heavy; dynamic sections stand tall.
        const float ch = b.getHeight() * (0.2f + 0.8f * t);
        const float cy = b.getBottom() - ch;

        KhensuLookAndFeel::draw3DHeatBar(g, {cx, cy, cellW, ch}, dynamicsHeatColourFor(values_[i]), kDepth);
    }
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
    repaint();
}

void CrestHistogramStrip::paint(juce::Graphics& g)
{
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("CREST FACTOR DISTRIBUTION", getLocalBounds().withHeight(12), juce::Justification::centredLeft, false);

    if (!hasData_ || maxCount_ == 0) return;

    auto b = getLocalBounds().withTrimmedTop(13).toFloat();
    if (b.getHeight() < 4.0f) return;

    constexpr float spacing = 2.0f;
    const float binW = (b.getWidth() - spacing * (kNumBins - 1)) / (float)kNumBins;
    constexpr float binWidthDb = (kMaxDb - kMinDb) / (float)kNumBins;

    for (int i = 0; i < kNumBins; ++i) {
        const float t = (float)counts_[(size_t)i] / (float)maxCount_;
        const float bx = b.getX() + (float)i * (binW + spacing);
        const float bh = b.getHeight() * juce::jmax(0.04f, t);
        const float by = b.getBottom() - bh;

        // Low crest (squashed) reads red, high crest (dynamic) reads blue -
        // same colour convention as SectionDynamicsHeatStrip above.
        const float binCentreDb = kMinDb + binWidthDb * ((float)i + 0.5f);
        const float hueT = juce::jlimit(0.0f, 1.0f, (binCentreDb - kMinDb) / (kMaxDb - kMinDb));
        const juce::Colour col = KhensuTheme::accentRed.interpolatedWith(KhensuTheme::accentBlue, hueT);

        g.setColour(counts_[(size_t)i] > 0 ? col : col.withAlpha(0.15f));
        g.fillRoundedRectangle(bx, by, binW, bh, 1.5f);
    }
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

    peakToRms_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::plain));
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
        for (const auto& sec : r.section_analysis->sections)
            if (sec.crest_db > 0.001f) crestVals.push_back(sec.crest_db);
        if (!crestVals.empty()) {
            dynamicsHeatStrip_.setSections(crestVals);
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
        dynamicsHeatStrip_.setBounds(b.removeFromTop(48));
    } else {
        dynamicsHeatStrip_.setBounds({});
    }

    if (crestHistogram_.isVisible()) {
        b.removeFromTop(6);
        crestHistogram_.setBounds(b.removeFromTop(36));
    } else {
        crestHistogram_.setBounds({});
    }
}

} // namespace khensu
