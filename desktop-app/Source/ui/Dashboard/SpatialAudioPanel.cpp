// desktop-app/Source/ui/Dashboard/SpatialAudioPanel.cpp
#include "SpatialAudioPanel.h"
#include "../shared/KhensuTheme.h"
#include <algorithm>

namespace khensu {

SpatialAudioPanel::SpatialAudioPanel(IAppController& c) : PanelCard(c, "Spatial Audio / Atmos Readiness")
{
    setExplainer(TRANS("Spatial Audio Readiness"),
                 TRANS("A stereo mix carries no real per-object spatial data, so this isn't an Atmos "
                 "certification -- it's an estimate of how well this stereo mix would translate "
                 "into an object-based spatial upmix (Dolby Atmos, Sony 360 Reality Audio, Apple "
                 "Spatial Audio), from stereo width, bass/LFE centring, phase safety, and dynamic "
                 "headroom. The 3D view is an illustrative reference of where content would "
                 "plausibly sit in an immersive placement, not a literal render of your mix. "
                 "Drag to rotate, scroll to zoom."));

    camera_.pitch = 0.35f;
    camera_.yaw = 0.3f;
    camera_.zoom = 3.4f;
}

void SpatialAudioPanel::setData(const AnalysisResult& r)
{
    assessment_ = assessSpatialAudioReadiness(r);
    hasData_ = true;
    repaint();
}

void SpatialAudioPanel::resized()
{
    // 3D scene fills the remainder; nothing else to lay out (score column is
    // hand-painted, not child components).
}

void SpatialAudioPanel::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    lastDragPos_ = e.position;
}

void SpatialAudioPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_) return;
    const auto delta = e.position - lastDragPos_;
    camera_.yaw += delta.x * 0.01f;
    camera_.pitch = juce::jlimit(-1.4f, 1.4f, camera_.pitch + delta.y * 0.01f);
    lastDragPos_ = e.position;
    repaint();
}

void SpatialAudioPanel::mouseUp(const juce::MouseEvent&) { dragging_ = false; }

void SpatialAudioPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    camera_.zoom = juce::jlimit(1.5f, 8.0f, camera_.zoom - wheel.deltaY * 1.5f);
    repaint();
}

namespace {
juce::Colour bandColour(SpatialAudioAssessment::Band band)
{
    switch (band) {
    case SpatialAudioAssessment::Band::StrongCandidate:
        return KhensuTheme::accentTeal;
    case SpatialAudioAssessment::Band::Workable:
        return KhensuTheme::accentAmber;
    case SpatialAudioAssessment::Band::NotRecommended:
        return KhensuTheme::accentRed;
    }
    return KhensuTheme::textTertiary;
}

void drawScoreBar(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, float score)
{
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(label, area.removeFromTop(12), juce::Justification::centredLeft, false);

    auto track = area.removeFromTop(6).toFloat();
    g.setColour(KhensuTheme::bgRaised);
    g.fillRoundedRectangle(track, 3.0f);

    const float t = juce::jlimit(0.0f, 1.0f, score / 100.0f);
    auto fill = track.withWidth(track.getWidth() * t);
    const juce::Colour c = score >= 70.0f   ? KhensuTheme::accentTeal
                           : score >= 45.0f ? KhensuTheme::accentAmber
                                            : KhensuTheme::accentRed;
    g.setColour(c);
    g.fillRoundedRectangle(fill, 3.0f);
}
} // namespace

void SpatialAudioPanel::drawScoreColumn(juce::Graphics& g, juce::Rectangle<int> area)
{
    if (!hasData_) {
        drawEmptyState(g, "No spatial-audio data yet", "Run a full analysis to see spatial-audio readiness.");
        return;
    }

    g.setColour(bandColour(assessment_.band));
    g.setFont(KhensuTheme::labelFont(type::heading));
    g.drawText(juce::String((int)std::round(assessment_.overallScore)), area.removeFromTop(30),
               juce::Justification::centredLeft, false);

    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawText(toString(assessment_.band), area.removeFromTop(18), juce::Justification::centredLeft, false);
    area.removeFromTop(8);

    hover_.clear();
    auto bar = [&](const char* name, float score) {
        const auto r = area.removeFromTop(24);
        hover_.add(r.toFloat(), juce::translate(name) + ": " + juce::String(juce::roundToInt(score)) + " / 100");
        drawScoreBar(g, r, name, score);
        area.removeFromTop(6);
    };
    bar(NEEDS_TRANS("Stereo width"), assessment_.stereoWidthScore);
    bar(NEEDS_TRANS("Bass/LFE centring"), assessment_.monoCompatScore);
    bar(NEEDS_TRANS("Phase safety"), assessment_.phaseSafetyScore);
    bar(NEEDS_TRANS("Dynamic headroom"), assessment_.dynamicHeadroomScore);
    area.removeFromTop(10);

    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::bodyFont(type::caption));
    g.drawFittedText(assessment_.summary, area, juce::Justification::topLeft, 6);
}

void SpatialAudioPanel::drawScene3D(juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.5f));
    g.fillRect(area);

    const float w = (float)area.getWidth();
    const float h = (float)area.getHeight();
    if (w <= 0.0f || h <= 0.0f) return;

    auto project = [&](juce::Vector3D<float> p) {
        auto pt = camera_.project(p, w, h);
        return juce::Point<float>(pt.x + area.getX(), pt.y + area.getY());
    };

    // Dome-ish room: floor square + a few "height" rings suggesting a ceiling
    // layer, rather than the flat 4-wall room StereoFieldVizComponent draws
    // - this panel is specifically about the height/overhead axis Atmos adds.
    g.setColour(KhensuTheme::borderSubtle.withAlpha(0.25f));
    const float zFloor = -0.85f, zCeil = 0.85f;
    for (float g0 = -1.0f; g0 <= 1.001f; g0 += 0.5f) {
        auto a1 = project({g0, -1.0f, zFloor});
        auto a2 = project({g0, 1.0f, zFloor});
        g.drawLine(a1.x, a1.y, a2.x, a2.y, 0.8f);
        auto b1 = project({-1.0f, g0, zFloor});
        auto b2 = project({1.0f, g0, zFloor});
        g.drawLine(b1.x, b1.y, b2.x, b2.y, 0.8f);
    }
    g.setColour(KhensuTheme::borderSubtle.withAlpha(0.15f));
    for (float ring = 0.35f; ring <= 0.86f; ring += 0.25f) {
        auto c1 = project({-1.0f, -1.0f, zFloor + (zCeil - zFloor) * ring});
        auto c2 = project({1.0f, -1.0f, zFloor + (zCeil - zFloor) * ring});
        auto c3 = project({1.0f, 1.0f, zFloor + (zCeil - zFloor) * ring});
        auto c4 = project({-1.0f, 1.0f, zFloor + (zCeil - zFloor) * ring});
        juce::Path ringPath;
        ringPath.startNewSubPath(c1);
        ringPath.lineTo(c2);
        ringPath.lineTo(c3);
        ringPath.lineTo(c4);
        ringPath.closeSubPath();
        g.strokePath(ringPath, juce::PathStrokeType(0.8f));
    }

    if (!hasData_) return;

    // Listener marker at the room centre, ear height.
    auto listener = project({0.0f, 0.0f, -0.1f});
    g.setColour(KhensuTheme::textTertiary);
    g.drawEllipse(listener.x - 5.0f, listener.y - 5.0f, 10.0f, 10.0f, 1.2f);

    auto getDepth = [&](juce::Vector3D<float> p) {
        Matrix4x4 view = camera_.getViewMatrix();
        return p.x * view.m[2] + p.y * view.m[6] + p.z * view.m[10] + view.m[14];
    };

    auto objects = assessment_.virtualObjects;
    std::sort(objects.begin(), objects.end(),
              [&](const auto& a, const auto& b) { return getDepth({a.x, a.y, a.z}) > getDepth({b.x, b.y, b.z}); });

    static const juce::Colour kObjectColours[] = {
        KhensuTheme::accentAmber, KhensuTheme::accentTeal, KhensuTheme::accentBlue,
        KhensuTheme::accentTeal,  KhensuTheme::accentRed,
    };

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        const juce::Vector3D<float> pos{obj.x, obj.y, obj.z};
        auto pt = project(pos);

        auto floorPt = project({obj.x, obj.y, zFloor});
        g.setColour(khensu::KhensuTheme::shadowColour.withAlpha(0.3f));
        g.fillEllipse(floorPt.x - 5.0f, floorPt.y - 2.0f, 10.0f, 4.0f);
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.2f));
        g.drawLine(floorPt.x, floorPt.y, pt.x, pt.y, 0.7f);

        const juce::Colour c = kObjectColours[i % (sizeof(kObjectColours) / sizeof(kObjectColours[0]))];
        g.setColour(c);
        g.fillEllipse(pt.x - 7.0f, pt.y - 7.0f, 14.0f, 14.0f);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.6f));
        g.drawEllipse(pt.x - 7.0f, pt.y - 7.0f, 14.0f, 14.0f, 1.0f);

        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(juce::String(obj.label), (int)pt.x - 45, (int)pt.y + 8, 90, 12, juce::Justification::centred);
    }
}

void SpatialAudioPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea();

    auto scoreArea = b.removeFromLeft(kScoreColW);
    b.removeFromLeft(10);
    drawScoreColumn(g, scoreArea);
    drawScene3D(g, b);
}

} // namespace khensu
