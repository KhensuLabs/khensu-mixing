// desktop-app/Source/ui/Dashboard/StereoFieldPanel.cpp
#include "StereoFieldPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RecommendationExplainer.h"
#include "../../IAppController.h"

namespace khensu {

namespace {
const juce::String kStereoExplainerBase =
    "Width is the spread of the stereo image (0 = mono, 1 = very wide). Correlation near "
    "+1 is mono-safe; negative values warn of phase cancellation in mono playback. "
    "The XY circle shows width (X) and depth (Y) plotted against the genre target.";
}

void StereoFieldTimelineStrip::paint(juce::Graphics& g)
{
    g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::plain));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("STEREO FIELD OVER TIME", getLocalBounds().withHeight(11), juce::Justification::centredLeft, false);

    if (points_.empty()) return;

    auto b = getLocalBounds().toFloat().withTrimmedTop(12.0f);
    if (b.getHeight() < 6.0f || b.getWidth() < 4.0f) return;
    hover_.clear();
    for (size_t i = 0; i < points_.size(); ++i) {
        const float x = b.getX() + points_[i].progress_fraction * b.getWidth();
        const float next = i + 1 < points_.size() ? b.getX() + points_[i + 1].progress_fraction * b.getWidth() : b.getRight();
        hover_.addColumn(x, next, b,
                         juce::String(juce::roundToInt(points_[i].progress_fraction * 100.0f)) + " % " + TRANS("through")
                             + ": " + TRANS("width") + " " + juce::String(juce::roundToInt(points_[i].width * 100.0f)) + " %, "
                             + TRANS("correlation") + " " + juce::String(points_[i].correlation, 2) + ", "
                             + TRANS("sub-bass mono") + " " + juce::String(juce::roundToInt(points_[i].sub_bass_mono_pct)) + " %");
    }

    auto plot = [&](auto valueOf, juce::Colour colour, float rangeLo, float rangeHi) {
        juce::Path path;
        for (size_t i = 0; i < points_.size(); ++i) {
            const float t = (rangeHi > rangeLo)
                                ? juce::jlimit(0.0f, 1.0f, (valueOf(points_[i]) - rangeLo) / (rangeHi - rangeLo))
                                : 0.5f;
            const float x = b.getX() + points_[i].progress_fraction * b.getWidth();
            const float y = b.getBottom() - t * b.getHeight();
            if (i == 0)
                path.startNewSubPath(x, y);
            else
                path.lineTo(x, y);
        }
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(1.5f));
    };

    plot([](const AnalysisResult::StereoFieldPoint& p) { return p.correlation; }, KhensuTheme::accentTeal, -1.0f, 1.0f);
    plot([](const AnalysisResult::StereoFieldPoint& p) { return p.width; }, KhensuTheme::accentBlue, 0.0f, 1.0f);

    g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
    auto legend = getLocalBounds().withHeight(11).toFloat();
    g.setColour(KhensuTheme::accentTeal);
    g.drawText("- Correlation", legend, juce::Justification::topRight, false);
    legend.translate(0.0f, 0.0f);
    g.setColour(KhensuTheme::accentBlue);
    g.drawText("- Width", legend.withTrimmedRight(76.0f), juce::Justification::topRight, false);
}

StereoFieldPanel::StereoFieldPanel(IAppController& c) : PanelCard(c, "Stereo Field"), fieldViz_(c)
{
    setExplainer(TRANS("Stereo"), kStereoExplainerBase, "stereo_field");

    correlationMeter_.setRange(-1.0f, 1.0f);
    correlationMeter_.setTargetZone(0.3f, 0.95f);
    correlationMeter_.addZoneLabel(0.0f, "CANCEL");
    correlationMeter_.addZoneLabel(0.25f, "MONO");
    correlationMeter_.addZoneLabel(0.65f, "CORRELATED");
    correlationMeter_.addZoneLabel(1.0f, "WIDE");

    addAndMakeVisible(fieldViz_);
    addAndMakeVisible(correlationMeter_);
    addAndMakeVisible(goniometer_);
    addAndMakeVisible(timelineStrip_);
    addAndMakeVisible(compareChip_);
    compareChip_.setVisible(false);

    realTimeMeterListener_ = listen(controller_.playback(), this, &IPlayback::addRealTimeMeterListener,
                                    &IPlayback::removeRealTimeMeterListener,
                                    [this](float /*lp*/, float /*rp*/, float /*mom*/, float /*st*/, float corr,
                                           float w) { updateRealTime(w, corr); });
}

StereoFieldPanel::~StereoFieldPanel() = default;

void StereoFieldPanel::setData(const AnalysisResult& r)
{
    updateRealTime(r.stereo.width, r.stereo.correlation);
    goniometer_.setAnalysisData(r);

    timelineStrip_.setTimeline(r.stereo_field_timeline ? r.stereo_field_timeline->points
                                                       : std::vector<AnalysisResult::StereoFieldPoint>{});
    timelineStrip_.setVisible(timelineStrip_.hasData());
    resized();

    const auto* rec =
        explainer::findTopRecommendation(r.recommendations, {"stereo_too_wide", "stereo_too_narrow", "phase_issue"});
    setExplainer(TRANS("Stereo"), explainer::appendReasoningTrace(kStereoExplainerBase, rec), "stereo_field");

    lastRefDeltas_ = r.reference_deltas;
    updateCompareChip();
}

void StereoFieldPanel::setCompareModeEnabled(bool enabled)
{
    compareEnabled_ = enabled;
    updateCompareChip();
}

void StereoFieldPanel::updateCompareChip()
{
    if (compareEnabled_ && lastRefDeltas_.has_value()) {
        if (const auto* d = CompareDeltaChip::find(*lastRefDeltas_, "stereo_correlation")) {
            compareChip_.setDelta(d->delta, "");
            resized();
            return;
        }
    }
    compareChip_.clear();
    resized();
}

void StereoFieldPanel::updateRealTime(float width, float correlation)
{
    fieldViz_.setCorrelation(correlation);
    fieldViz_.setWidthPercent(width * 100.0f);

    // Depth: high correlation (mono) = no perceived depth; anti-phase = maximum depth illusion.
    const float depth = (1.0f - correlation) * 50.0f;
    fieldViz_.setDepthPercent(juce::jlimit(0.0f, 100.0f, depth));

    correlationMeter_.setValue(correlation, "Correlation: " + juce::String(correlation, 3));

    goniometer_.updateRealTime(width, correlation);
}

void StereoFieldPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    correlationMeter_.setBounds(b.removeFromBottom(48));
    b.removeFromBottom(KhensuTheme::sp2);
    if (timelineStrip_.isVisible()) {
        timelineStrip_.setBounds(b.removeFromBottom(40));
        b.removeFromBottom(4);
    }
    if (compareChip_.isVisible()) {
        compareChip_.setBounds(b.removeFromBottom(16));
        b.removeFromBottom(4);
    }

    // Split remaining space into 2 columns: left (field target), right (goniometer)
    auto leftCol = b.removeFromLeft(b.getWidth() / 2 - 6);
    b.removeFromLeft(12);
    fieldViz_.setBounds(leftCol);
    goniometer_.setBounds(b);
}

} // namespace khensu
