// desktop-app/Source/ui/Dashboard/LoudnessPanel.cpp
#include "LoudnessPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/RecommendationExplainer.h"
#include "../shared/KhensuTheme.h"
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace {
const juce::String kLoudnessExplainerBase =
    "Integrated LUFS is overall perceived loudness; streaming platforms target around "
    "-14 LUFS. LRA is loudness range (macro-dynamics). PLR is peak-to-loudness headroom. "
    "True peak above -1 dBTP risks inter-sample clipping on lossy codecs. "
    "The section arc shows per-section loudness; amber bars exceed the mix average by 1.5 LU.";

// Blue (quiet) -> teal -> amber -> red (hot/over reference), interpolated
// across t in [0,1] - used for both the loudness and dynamics heat strips.
juce::Colour heatColourFor(float t)
{
    t = juce::jlimit(0.0f, 1.0f, t);
    if (t < 0.33f) return KhensuTheme::accentBlue.interpolatedWith(KhensuTheme::accentTeal, t / 0.33f);
    if (t < 0.66f) return KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentAmber, (t - 0.33f) / 0.33f);
    return KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentRed, (t - 0.66f) / 0.34f);
}
} // anonymous namespace

void SectionLoudnessBar::paint(juce::Graphics& g)
{
    if (values_.empty()) return;

    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(TRANS("SECTION LOUDNESS (3D)"), getLocalBounds().withHeight(12), juce::Justification::centredLeft,
               false);

    // Reserve room at top-right for the isometric depth offset so bars don't
    // clip into the label row.
    constexpr float kDepth = 5.0f;
    auto b = getLocalBounds().withTrimmedTop(13).toFloat();
    b.removeFromRight(kDepth);
    b.removeFromTop(kDepth);
    if (b.getHeight() < 4.0f) return;

    const int n = (int)values_.size();
    const float spacing = 3.0f;
    const float barW = (b.getWidth() - spacing * (n - 1)) / (float)n;
    const float lo = refLufs_ - 12.0f;
    const float hi = refLufs_ + 4.0f;
    const float range = hi - lo;

    for (int i = 0; i < n; ++i) {
        const float t = range > 0.001f ? juce::jlimit(0.0f, 1.0f, (values_[i] - lo) / range) : 0.5f;
        const float bx = b.getX() + (float)i * (barW + spacing);
        const float bh = b.getHeight() * (0.15f + 0.85f * t);
        const float by = b.getBottom() - bh;

        KhensuLookAndFeel::draw3DHeatBar(g, {bx, by, barW, bh}, heatColourFor(t), kDepth);
    }

    // Dashed reference line at the mix's integrated LUFS
    const float refT = range > 0.001f ? juce::jlimit(0.0f, 1.0f, (refLufs_ - lo) / range) : 0.5f;
    const float refY = b.getBottom() - b.getHeight() * (0.15f + 0.85f * refT);
    g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.35f));
    float dashes[] = {3.0f, 2.0f};
    g.drawDashedLine({b.getX(), refY, b.getRight() + kDepth, refY}, dashes, 2, 1.0f);
}

LoudnessPanel::LoudnessPanel(IAppController& c) : PanelCard(c, "Loudness & Levels")
{
    setExplainer(TRANS("Loudness"), kLoudnessExplainerBase, "integrated_loudness");
    lufsMeter_.setRange(-24.0f, 0.0f);
    lufsMeter_.setTargetZone(-16.0f, -9.0f);
    lufsMeter_.addZoneLabel(0.0f, "QUIET");
    lufsMeter_.addZoneLabel(0.33f, "COMMERCIAL");
    lufsMeter_.addZoneLabel(0.66f, "COMPETITIVE");
    lufsMeter_.addZoneLabel(1.0f, "VERY LOUD");

    lraMeter_.setRange(0.0f, 20.0f);
    lraMeter_.setTargetZone(4.0f, 10.0f);
    lraMeter_.addZoneLabel(0.0f, "FLAT");
    lraMeter_.addZoneLabel(0.33f, "NATURAL");
    lraMeter_.addZoneLabel(0.66f, "DYNAMIC");
    lraMeter_.addZoneLabel(1.0f, "WIDE");

    addAndMakeVisible(lufsMeter_);
    addAndMakeVisible(lraMeter_);
    addAndMakeVisible(plrBadge_);
    addAndMakeVisible(truePeakBadge_);
    addAndMakeVisible(sectionArc_);
    sectionArc_.setVisible(false);

    addAndMakeVisible(liveMeter_);

    addAndMakeVisible(compareChip_);
    compareChip_.setVisible(false);

    emptyState_.setMessage(TRANS("Awaiting loudness analysis"),
                           TRANS("Drop a track to see integrated LUFS, LRA, true peak, and section loudness."));
    addChildComponent(skeleton_);
    addChildComponent(emptyState_);
    updateStateVisibility();

    realTimeMeterListener_ = listen(
        controller_.playback(), this, &IPlayback::addRealTimeMeterListener, &IPlayback::removeRealTimeMeterListener,
        [this](float /*lp*/, float /*rp*/, float mom, float st, float /*corr*/, float /*w*/) {
            // Live DAW metering shows in its own row - it never overwrites the
            // integrated-LUFS analysis meter. Momentary/short-term are an
            // RMS-EMA (not K-weighted) estimate from the plugin - labelled as
            // such by LiveLufsMeterComponent (DSK-15).
            liveMeter_.pushReading(mom, st);
        });
}

LoudnessPanel::~LoudnessPanel() = default;

void LoudnessPanel::setData(const AnalysisResult& r)
{
    // DSK-09: LUFS, LRA, PLR and true peak are the local C++ (BS.1770)
    // measurements - gate on a result existing, not on the Python deep pass.
    // Local-first: with the service down this panel showed a skeleton
    // forever while the zone-1 tiles showed the very same numbers. Only the
    // section arc below needs Python data (and checks for it).
    hasAnalysis_ = controller_.hasAnalysis() && !r.is_partial && !r.analysis_failed;
    updateStateVisibility();
    if (!hasAnalysis_) return;

    // Genre-aware target zone (§10.3): the session genre's LUFS window, the
    // generic -16..-9 without one.
    float lo = -16.0f, hi = -9.0f;
    if (const auto* s = controller_.getActiveSession())
        if (auto g = controller_.genreDatabase().findById(s->target.primary_genre)) {
            lo = g->lufs_range_min;
            hi = g->lufs_range_max;
        }
    lufsMeter_.setTargetZone(lo, hi);

    liveMeter_.reset(); // new analysis = new session/track; don't carry over the last one's live distribution
    lufsMeter_.setValue(r.loudness.lufs_integrated,
                        TRANS("Integrated:") + " " + juce::String(r.loudness.lufs_integrated, 1) + " LUFS");
    if (r.loudness.lra_measured)
        lraMeter_.setValue(r.loudness.lra, "LRA: " + juce::String(r.loudness.lra, 1) + " LU");
    else
        lraMeter_.setValue(0.0f, TRANS("LRA: n/a (needs > 3 s of audio)"));
    plrBadge_.setBadge("PLR " + juce::String(r.loudness.plr, 1) + " dB",
                       r.loudness.plr < 8.0f ? BadgeStatus::Warn : BadgeStatus::Pass);
    truePeakBadge_.setBadge(TRANS("True Peak") + " " + juce::String(r.loudness.true_peak_dbtp, 1) + " dBTP",
                            r.loudness.true_peak_dbtp > -1.0f ? BadgeStatus::Fail : BadgeStatus::Pass);

    if (r.section_analysis && !r.section_analysis->sections.empty()) {
        std::vector<float> lufsVals;
        for (const auto& sec : r.section_analysis->sections)
            if (sec.lufs_integrated < -0.1f && sec.lufs_integrated > -70.0f) lufsVals.push_back(sec.lufs_integrated);
        if (!lufsVals.empty()) {
            sectionArc_.setSections(lufsVals, r.loudness.lufs_integrated);
            sectionArc_.setVisible(true);
        } else {
            sectionArc_.setVisible(false);
        }
    } else {
        sectionArc_.setVisible(false);
    }

    const auto* rec =
        explainer::findTopRecommendation(r.recommendations, {"lufs_too_loud", "lufs_too_quiet", "lra_too_compressed",
                                                             "lra_too_dynamic", "true_peak_over"});
    setExplainer(TRANS("Loudness"), explainer::appendReasoningTrace(kLoudnessExplainerBase, rec), "integrated_loudness");

    lastRefDeltas_ = r.reference_deltas;
    updateCompareChip();

    repaint();
}

void LoudnessPanel::setCompareModeEnabled(bool enabled)
{
    compareEnabled_ = enabled;
    updateCompareChip();
}

void LoudnessPanel::updateCompareChip()
{
    if (compareEnabled_ && lastRefDeltas_.has_value()) {
        if (const auto* d = CompareDeltaChip::find(*lastRefDeltas_, "lufs")) {
            compareChip_.setDelta(d->delta, " dB");
            resized();
            return;
        }
    }
    compareChip_.clear();
    resized();
}

void LoudnessPanel::updateStateVisibility()
{
    const bool active = controller_.hasActiveSession();
    skeleton_.setVisible(active && !hasAnalysis_);
    emptyState_.setVisible(!active);

    const bool contentVisible = hasAnalysis_;
    lufsMeter_.setVisible(contentVisible);
    lraMeter_.setVisible(contentVisible);
    plrBadge_.setVisible(contentVisible);
    truePeakBadge_.setVisible(contentVisible);
    liveMeter_.setVisible(contentVisible);
}

void LoudnessPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    skeleton_.setBounds(b);
    emptyState_.setBounds(b);
    if (!hasAnalysis_) return;

    lufsMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(6);
    lraMeter_.setBounds(b.removeFromTop(34));
    b.removeFromTop(8);
    auto row = b.removeFromTop(26);
    plrBadge_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 6));
    row.removeFromLeft(12);
    truePeakBadge_.setBounds(row);

    b.removeFromTop(4);
    liveMeter_.setBounds(b.removeFromTop(40));

    if (compareChip_.isVisible()) {
        b.removeFromTop(4);
        compareChip_.setBounds(b.removeFromTop(16));
    }

    if (sectionArc_.isVisible()) {
        b.removeFromTop(6);
        sectionArc_.setBounds(b.removeFromTop(48));
    } else {
        sectionArc_.setBounds({});
    }
}

} // namespace khensu
