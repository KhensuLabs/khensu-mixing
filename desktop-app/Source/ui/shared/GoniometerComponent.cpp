// desktop-app/Source/ui/shared/GoniometerComponent.cpp
#include "GoniometerComponent.h"
#include "KhensuLookAndFeel.h"
#include <algorithm>
#include "KhensuTheme.h"

namespace khensu {

void GoniometerComponent::setValues(float width, float correlation)
{
    width_ = width;
    corr_ = correlation;

    // 1. Decay existing points
    for (auto& p : points_) {
        p.alpha -= 0.08f;
    }

    // Remove dead points
    points_.erase(
        std::remove_if(points_.begin(), points_.end(), [](const GoniometerPoint& p) { return p.alpha <= 0.0f; }),
        points_.end());

    // 2. Generate new point cloud representing real-time audio sample vector scatter
    float wVal = juce::jlimit(0.04f, 1.0f, width_);
    // Correlation near +1 is narrow/mono (vertical), correlation near -1 is out of phase (horizontal)
    float cVal = juce::jlimit(-1.0f, 1.0f, corr_);

    // Mid gain (mono sum) and Side gain (stereo difference)
    float midG = std::sqrt(juce::jmax(0.0f, 1.0f + cVal)) * 0.75f;
    float sideG = std::sqrt(juce::jmax(0.0f, 1.0f - cVal)) * wVal * 0.75f;

    juce::Random r;
    // Generate 120 points per analysis update to build a dense, organic cloud
    for (int i = 0; i < 120; ++i) {
        // Generate random mid/side values with a Gaussian-like distribution
        float mid = (r.nextFloat() + r.nextFloat() + r.nextFloat() - 1.5f) * 0.66f;
        float side = (r.nextFloat() + r.nextFloat() + r.nextFloat() - 1.5f) * 0.66f;

        mid *= midG;
        side *= sideG;

        // Convert back to Left / Right channels
        float left = (mid + side);
        float right = (mid - side);

        // Convert Left/Right channels to scope X and Y
        // X = L - R (difference / width spread)
        // Y = -(L + R) (sum / vertical amplitude)
        float x = (left - right) * 0.7071f;
        float y = -(left + right) * 0.7071f;

        points_.push_back({x, y, r.nextFloat() * 0.4f + 0.6f});
    }

    repaint();
}

void GoniometerComponent::setSectionTrail(const std::vector<SectionData>& sections, float globalWidth)
{
    trail_.clear();
    if (sections.empty()) {
        repaint();
        return;
    }

    const float wVal = juce::jlimit(0.04f, 1.0f, globalWidth);
    const int n = (int)sections.size();

    for (int i = 0; i < n; ++i) {
        const auto& s = sections[(size_t)i];
        const float cVal = juce::jlimit(-1.0f, 1.0f, s.stereo_correlation);

        // Same mid/side projection setValues() uses for the live cloud, but
        // one deterministic point per section instead of a random scatter.
        const float midG = std::sqrt(juce::jmax(0.0f, 1.0f + cVal)) * 0.75f;
        const float sideG = std::sqrt(juce::jmax(0.0f, 1.0f - cVal)) * wVal * 0.75f;
        const float left = midG;
        const float right = midG - 2.0f * sideG; // deterministic offset, not random L/R split
        const float x = (left - right) * 0.7071f;
        const float y = -(left + right) * 0.7071f;

        // Earliest section = cool teal, latest = warm amber, interpolated.
        const float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        const juce::Colour colour = KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentAmber, t);

        trail_.push_back({x, y, colour, juce::String(s.label)});
    }
    repaint();
}

void GoniometerComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced(4.0f);
    float cx = bounds.getCentreX();
    float cy = bounds.getCentreY();
    float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.45f;

    // Paint dark glass backing plate
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.40f));
    g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

    // Draw spec outline on circle
    g.setColour(KhensuTheme::bgOverlay);
    g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    // Draw reference polar grid rings (representing -3dB, -6dB, -12dB energy levels)
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.08f));
    g.drawEllipse(cx - radius * 0.7f, cy - radius * 0.7f, radius * 1.4f, radius * 1.4f, 1.0f);
    g.drawEllipse(cx - radius * 0.4f, cy - radius * 0.4f, radius * 0.8f, radius * 0.8f, 1.0f);

    // Draw Left and Right reference axis lines at -45° and +45°
    constexpr float s45 = 0.7071f;
    float axLen = radius * 0.95f;
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.20f));
    g.drawLine(cx, cy, cx - axLen * s45, cy - axLen * s45, 1.0f); // Left axis
    g.drawLine(cx, cy, cx + axLen * s45, cy - axLen * s45, 1.0f); // Right axis

    // Draw Center axis (mono indicator / vertical line)
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.12f));
    g.drawVerticalLine(juce::roundToInt(cx), cy - axLen, cy);

    // Draw labels: 'L' and 'R'
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.6f));
    g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
    g.drawText("L", (int)(cx - axLen * s45 - 12), (int)(cy - axLen * s45 - 12), 16, 16, juce::Justification::centred);
    g.drawText("R", (int)(cx + axLen * s45 - 4), (int)(cy - axLen * s45 - 12), 16, 16, juce::Justification::centred);
    g.drawText("M", (int)(cx - 8), (int)(cy - axLen - 14), 16, 16, juce::Justification::centred);

    // Render the scatter cloud points
    for (const auto& p : points_) {
        // Map normalized -1..1 point to scope radius
        float px = cx + p.x * radius;
        float py = cy + p.y * radius;

        // Draw particle with glowing alpha decay
        g.setColour(KhensuTheme::accentTeal.withAlpha(p.alpha * 0.55f));
        g.fillEllipse(px - 1.2f, py - 1.2f, 2.4f, 2.4f);

        // Subtly brighter core for glowing neon effect
        if (p.alpha > 0.4f) {
            g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(p.alpha * 0.4f));
            g.fillEllipse(px - 0.6f, py - 0.6f, 1.2f, 1.2f);
        }
    }

    // Per-section history trail: connecting line + colored markers, earliest
    // (teal) to latest (amber) section.
    if (trail_.size() >= 2) {
        juce::Path path;
        for (size_t i = 0; i < trail_.size(); ++i) {
            const float px = cx + trail_[i].x * radius;
            const float py = cy + trail_[i].y * radius;
            if (i == 0)
                path.startNewSubPath(px, py);
            else
                path.lineTo(px, py);
        }
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.15f));
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }
    for (const auto& t : trail_) {
        const float px = cx + t.x * radius;
        const float py = cy + t.y * radius;
        g.setColour(t.colour);
        g.fillEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f);
        g.setColour(khensu::KhensuTheme::shadowColour.withAlpha(0.6f));
        g.drawEllipse(px - 3.0f, py - 3.0f, 6.0f, 6.0f, 1.0f);
    }
}

} // namespace khensu
