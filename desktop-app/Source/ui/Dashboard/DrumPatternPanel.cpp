// desktop-app/Source/ui/Dashboard/DrumPatternPanel.cpp
#include "DrumPatternPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include "../shared/RecommendationExplainer.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {
static const juce::String kDrumGrooveBase = "Per-hit kick/snare timeline from the drum analysis pass. Marker size "
                                            "reflects hit loudness (body_db); colour reflects transient sharpness "
                                            "(attack_ms) -- brighter markers are punchier hits, dimmer markers are "
                                            "softer/rounder ones.";
}

DrumPatternPanel::DrumPatternPanel(IAppController& c) : PanelCard(c, "Drum Groove")
{
    setExplainer(TRANS("Drum Groove"), kDrumGrooveBase, "analysis_dashboard");
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    setAnalysisState(PanelAnalysisState::NotAnalyzed);
}

void DrumPatternPanel::setData(const AnalysisResult& r)
{
    if (!r.drums.has_value() || (r.drums->kick_events.empty() && r.drums->snare_events.empty())) {
        kickEvents_.clear();
        snareEvents_.clear();
        setAnalysisState(r.drums.has_value() ? PanelAnalysisState::Analyzed : PanelAnalysisState::NotAnalyzed);
        repaint();
        return;
    }

    kickEvents_ = r.drums->kick_events;
    snareEvents_ = r.drums->snare_events;
    kickAvgBpm_ = r.drums->kick_avg_bpm;
    fromStem_ = false; // full-mix pass until the stem re-run replaces it
    rebuildSpan();
    setAnalysisState(PanelAnalysisState::Analyzed);
    repaint();
}

void DrumPatternPanel::setDrumData(const AnalysisResult::DrumData& d, bool fromStem)
{
    kickEvents_ = d.kick_events;
    snareEvents_ = d.snare_events;
    kickAvgBpm_ = d.kick_avg_bpm;
    fromStem_ = fromStem;
    rebuildSpan();
    setAnalysisState(PanelAnalysisState::Analyzed);
    repaint();
}

void DrumPatternPanel::rebuildSpan()
{
    span_ = controller_.playback().getPlaybackDuration() > 0.0 ? static_cast<float>(controller_.playback().getPlaybackDuration()) : 0.0f;
    for (const auto& e : kickEvents_)
        span_ = std::max(span_, e.onset_sec + 1.0f);
    for (const auto& e : snareEvents_)
        span_ = std::max(span_, e.onset_sec + 1.0f);

    setExplainer(TRANS("Drum Groove"),
                 explainer::buildMetricsBlock(
                     {{"Kick hits", juce::String((int)kickEvents_.size()), "", ""},
                      {"Snare hits", juce::String((int)snareEvents_.size()), "", ""},
                      // THY-11: the service value is the kick hit RATE (60 / mean
                      // inter-onset interval), not the tempo - kicks on 1 and 3 at
                      // 120 BPM read 60.
                      {"Kick rate", kickAvgBpm_ > 0.0f ? juce::String(kickAvgBpm_, 1) : "-", "kicks/min", ""},
                      {"Source", fromStem_ ? juce::String("drum stem") : juce::String("full mix"), "", ""}}) +
                     kDrumGrooveBase,
                 "analysis_dashboard");
}

void DrumPatternPanel::resized() { PanelCard::resized(); }

float DrumPatternPanel::xFor(float sec) const
{
    if (span_ <= 0.0f) return (float)kLabelW;
    const auto body = bodyArea();
    const float w = (float)body.getWidth() - (float)kLabelW;
    return (float)body.getX() + (float)kLabelW + juce::jlimit(0.0f, 1.0f, sec / span_) * w;
}

juce::Rectangle<float> DrumPatternPanel::axisArea() const
{
    const auto body = bodyArea();
    return {(float)body.getX(), (float)body.getY(), (float)body.getWidth(), (float)kAxisH};
}

juce::Rectangle<float> DrumPatternPanel::laneArea(int index) const
{
    const auto body = bodyArea();
    const float top = (float)body.getY() + (float)kAxisH;
    const float laneH = ((float)body.getHeight() - (float)kAxisH) * 0.5f;
    return {(float)body.getX(), top + laneH * (float)index, (float)body.getWidth(), laneH};
}

void DrumPatternPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);

    if (kickEvents_.empty() && snareEvents_.empty()) {
        drawEmptyState(g, "No drum hits detected", "No percussive onsets were found in this source.");
        return;
    }

    auto axis = axisArea();
    g.setColour(KhensuTheme::bgSurface.darker(0.1f));
    g.fillRect(axis);

    static constexpr float kSteps[] = {1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f};
    float step = kSteps[std::size(kSteps) - 1];
    for (float s : kSteps) {
        if (span_ / s <= 10.0f) {
            step = s;
            break;
        }
    }

    g.setColour(KhensuTheme::textSecondary.withAlpha(0.6f));
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
    for (float t = 0.0f; t <= span_; t += step) {
        const float x = xFor(t);
        g.drawVerticalLine(juce::roundToInt(x), axis.getBottom() - 5.0f, axis.getBottom());
        const int min = (int)t / 60, sec = (int)t % 60;
        g.drawText(juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2), (int)x - 20, (int)axis.getY(), 40,
                   kAxisH - 5, juce::Justification::centred, false);
    }

    if (kickAvgBpm_ > 0.0f) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.7f));
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(juce::String(juce::roundToInt(kickAvgBpm_)) + " " + TRANS("kicks/min"), axis.withTrimmedLeft(axis.getWidth() - 80.0f),
                   juce::Justification::centredRight, false);
    }

    struct LaneSpec {
        const std::vector<AnalysisResult::DrumHitEvent>* events;
        const char* label;
        juce::Colour colour;
    };
    const LaneSpec lanes[2] = {
        {&kickEvents_, "KICK", KhensuTheme::accentAmber},
        {&snareEvents_, "SNARE", KhensuTheme::accentBlue},
    };

    for (int i = 0; i < 2; ++i) {
        auto lane = laneArea(i);
        g.setColour((i % 2 == 0) ? KhensuTheme::bgSurface : KhensuTheme::bgSurface.brighter(0.04f));
        g.fillRect(lane.withTrimmedLeft((float)kLabelW));

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(lanes[(size_t)i].label, lane.withWidth((float)kLabelW).toNearestInt(),
                   juce::Justification::centredLeft, false);

        const float midY = lane.getY() + lane.getHeight() * 0.5f;
        const float maxLaneRadius = juce::jmin(lane.getHeight() * 0.42f, 10.0f);

        for (const auto& hit : *lanes[(size_t)i].events) {
            const float mx = xFor(hit.onset_sec);
            // body_db is typically negative dBFS-ish; normalize a plausible
            // -40..0 dB range into a 0..1 loudness fraction for marker size.
            const float loudness = juce::jlimit(0.0f, 1.0f, (hit.body_db + 40.0f) / 40.0f);
            const float radius = juce::jmap(loudness, 3.0f, maxLaneRadius);

            // Sharper (lower) attack_ms -> brighter marker.
            const float sharpness = juce::jlimit(0.0f, 1.0f, 1.0f - (hit.attack_ms / 30.0f));
            const juce::Colour col = lanes[(size_t)i].colour.withAlpha(0.35f + 0.6f * sharpness);

            g.setColour(col);
            g.fillEllipse(mx - radius, midY - radius, radius * 2.0f, radius * 2.0f);
        }

        g.setColour(KhensuTheme::textSecondary.withAlpha(0.2f));
        g.drawHorizontalLine(juce::roundToInt(lane.getBottom()), lane.getX(), lane.getRight());
    }

    if (hoverTimeSec_ >= 0.0f) {
        const float hx = xFor(hoverTimeSec_);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.3f));
        g.drawVerticalLine(juce::roundToInt(hx), axis.getBottom(), (float)bodyArea().getBottom());
    }
}

void DrumPatternPanel::mouseMove(const juce::MouseEvent& e)
{
    if (span_ <= 0.0f) return;
    const auto body = bodyArea();
    const float w = (float)body.getWidth() - (float)kLabelW;
    if (w <= 0.0f) return;
    hoverTimeSec_ = juce::jlimit(0.0f, span_, (((float)e.x - (float)body.getX() - (float)kLabelW) / w) * span_);
    repaint();
}

void DrumPatternPanel::mouseExit(const juce::MouseEvent&)
{
    hoverTimeSec_ = -1.0f;
    repaint();
}

} // namespace khensu
