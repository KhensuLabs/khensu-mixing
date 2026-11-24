// desktop-app/Source/ui/shared/NavIcons.h
// UX2-4: the sidebar's line icons, drawn as vectors. Every icon is an open
// outline on a 24 x 24 grid (2 px margin), stroked - never filled - at one
// width (kStroke, 1.5 px at any size), with round joins and caps, so the set
// reads as one family next to the labels. Nothing here holds a colour: the
// caller strokes the path in the row's text or accent colour, so the icons
// follow the theme for free.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <utility>

namespace khensu::nav_icons {

/// The design grid every icon is drawn on.
constexpr float kGrid = 24.0f;
/// The stroke width in pixels, independent of the drawn size.
constexpr float kStroke = 1.5f;

enum class Icon {
    None,
    Intake,     ///< a tray with an arrow coming in
    Issues,     ///< a checklist
    Analysis,   ///< a spectrum: bars under a curve
    MixMaster,  ///< three faders
    Vocal,      ///< a microphone
    Export,     ///< a box with an arrow going out
    Copilot,    ///< a speech bubble with a spark
    Provenance, ///< a shield with a check
    References, ///< a record
    Help,       ///< a question mark in a circle
    Settings,   ///< a cog
};

namespace detail {
inline void line(juce::Path& p, float x1, float y1, float x2, float y2)
{
    p.startNewSubPath(x1, y1);
    p.lineTo(x2, y2);
}
} // namespace detail

/// The icon's outline on the 24 x 24 grid (empty for Icon::None).
inline juce::Path path(Icon icon)
{
    using detail::line;
    juce::Path p;
    switch (icon) {
    case Icon::None: break;
    case Icon::Intake:
        // Tray: open top, a lip at each side.
        p.startNewSubPath(3.0f, 13.0f);
        p.lineTo(3.0f, 19.0f);
        p.quadraticTo(3.0f, 21.0f, 5.0f, 21.0f);
        p.lineTo(19.0f, 21.0f);
        p.quadraticTo(21.0f, 21.0f, 21.0f, 19.0f);
        p.lineTo(21.0f, 13.0f);
        // Arrow down into it.
        line(p, 12.0f, 3.0f, 12.0f, 15.0f);
        p.startNewSubPath(7.5f, 10.5f);
        p.lineTo(12.0f, 15.0f);
        p.lineTo(16.5f, 10.5f);
        break;
    case Icon::Issues:
        // Three rows: a tick, a tick, an open box; a line beside each.
        p.startNewSubPath(3.5f, 6.0f);
        p.lineTo(5.0f, 7.5f);
        p.lineTo(8.0f, 4.5f);
        line(p, 11.0f, 6.0f, 21.0f, 6.0f);
        p.startNewSubPath(3.5f, 12.0f);
        p.lineTo(5.0f, 13.5f);
        p.lineTo(8.0f, 10.5f);
        line(p, 11.0f, 12.0f, 21.0f, 12.0f);
        p.addRoundedRectangle(3.5f, 16.5f, 4.0f, 4.0f, 1.0f);
        line(p, 11.0f, 18.5f, 21.0f, 18.5f);
        break;
    case Icon::Analysis:
        // Baseline, four bars and the curve riding over them.
        line(p, 3.0f, 21.0f, 21.0f, 21.0f);
        line(p, 6.0f, 21.0f, 6.0f, 14.0f);
        line(p, 10.0f, 21.0f, 10.0f, 9.0f);
        line(p, 14.0f, 21.0f, 14.0f, 12.0f);
        line(p, 18.0f, 21.0f, 18.0f, 16.0f);
        p.startNewSubPath(3.0f, 12.0f);
        p.cubicTo(6.0f, 4.0f, 9.0f, 3.0f, 12.0f, 6.0f);
        p.cubicTo(15.0f, 9.0f, 17.0f, 10.0f, 21.0f, 8.0f);
        break;
    case Icon::MixMaster:
        // Three fader slots with their caps at different heights.
        for (const auto [x, capY] : {std::pair{6.0f, 14.0f}, std::pair{12.0f, 8.0f}, std::pair{18.0f, 16.0f}}) {
            line(p, x, 3.0f, x, capY - 2.0f);
            line(p, x, capY + 2.0f, x, 21.0f);
            p.addRoundedRectangle(x - 2.5f, capY - 2.0f, 5.0f, 4.0f, 1.0f);
        }
        break;
    case Icon::Vocal:
        // Capsule, cradle, stand.
        p.addRoundedRectangle(9.0f, 2.5f, 6.0f, 11.0f, 3.0f);
        p.startNewSubPath(5.5f, 10.5f);
        p.quadraticTo(5.5f, 17.0f, 12.0f, 17.0f);
        p.quadraticTo(18.5f, 17.0f, 18.5f, 10.5f);
        line(p, 12.0f, 17.0f, 12.0f, 21.0f);
        line(p, 8.5f, 21.0f, 15.5f, 21.0f);
        break;
    case Icon::Export:
        // Box open at the top, arrow out of it.
        p.startNewSubPath(8.0f, 9.0f);
        p.lineTo(5.0f, 9.0f);
        p.quadraticTo(3.5f, 9.0f, 3.5f, 10.5f);
        p.lineTo(3.5f, 19.5f);
        p.quadraticTo(3.5f, 21.0f, 5.0f, 21.0f);
        p.lineTo(19.0f, 21.0f);
        p.quadraticTo(20.5f, 21.0f, 20.5f, 19.5f);
        p.lineTo(20.5f, 10.5f);
        p.quadraticTo(20.5f, 9.0f, 19.0f, 9.0f);
        p.lineTo(16.0f, 9.0f);
        line(p, 12.0f, 15.0f, 12.0f, 2.5f);
        p.startNewSubPath(8.0f, 6.5f);
        p.lineTo(12.0f, 2.5f);
        p.lineTo(16.0f, 6.5f);
        break;
    case Icon::Copilot:
        // Speech bubble with a tail, a four-point spark inside.
        p.startNewSubPath(7.0f, 18.0f);
        p.lineTo(5.5f, 18.0f);
        p.quadraticTo(3.0f, 18.0f, 3.0f, 15.5f);
        p.lineTo(3.0f, 6.0f);
        p.quadraticTo(3.0f, 3.5f, 5.5f, 3.5f);
        p.lineTo(18.5f, 3.5f);
        p.quadraticTo(21.0f, 3.5f, 21.0f, 6.0f);
        p.lineTo(21.0f, 15.5f);
        p.quadraticTo(21.0f, 18.0f, 18.5f, 18.0f);
        p.lineTo(12.0f, 18.0f);
        p.lineTo(7.0f, 21.5f);
        p.closeSubPath();
        p.startNewSubPath(12.0f, 6.5f);
        p.quadraticTo(12.5f, 10.0f, 15.5f, 10.75f);
        p.quadraticTo(12.5f, 11.5f, 12.0f, 15.0f);
        p.quadraticTo(11.5f, 11.5f, 8.5f, 10.75f);
        p.quadraticTo(11.5f, 10.0f, 12.0f, 6.5f);
        p.closeSubPath();
        break;
    case Icon::Provenance:
        // Shield, a tick inside.
        p.startNewSubPath(12.0f, 2.5f);
        p.lineTo(19.5f, 5.5f);
        p.lineTo(19.5f, 11.0f);
        p.quadraticTo(19.5f, 18.0f, 12.0f, 21.5f);
        p.quadraticTo(4.5f, 18.0f, 4.5f, 11.0f);
        p.lineTo(4.5f, 5.5f);
        p.closeSubPath();
        p.startNewSubPath(8.5f, 12.0f);
        p.lineTo(11.0f, 14.5f);
        p.lineTo(15.5f, 9.5f);
        break;
    case Icon::References:
        // A record: rim, a groove arc, the label.
        p.addEllipse(2.75f, 2.75f, 18.5f, 18.5f);
        p.addCentredArc(12.0f, 12.0f, 6.0f, 6.0f, 0.0f, -1.9f, -0.2f, true);
        p.addEllipse(10.0f, 10.0f, 4.0f, 4.0f);
        break;
    case Icon::Help:
        p.addEllipse(2.75f, 2.75f, 18.5f, 18.5f);
        p.startNewSubPath(9.25f, 9.5f);
        p.quadraticTo(9.25f, 6.75f, 12.0f, 6.75f);
        p.quadraticTo(14.75f, 6.75f, 14.75f, 9.25f);
        p.quadraticTo(14.75f, 11.0f, 12.0f, 12.25f);
        p.lineTo(12.0f, 13.75f);
        line(p, 12.0f, 16.75f, 12.0f, 17.25f);
        break;
    case Icon::Settings: {
        // A cog: eight flat-topped teeth around a ring, the hub inside.
        constexpr int kTeeth = 8;
        constexpr float rOuter = 9.5f, rInner = 7.0f;
        const float step = juce::MathConstants<float>::twoPi / static_cast<float>(kTeeth);
        const float half = step * 0.22f; // half a tooth's top
        const float flank = step * 0.32f; // half a tooth's root
        for (int i = 0; i < kTeeth; ++i) {
            const float a = static_cast<float>(i) * step;
            auto pt = [](float r, float ang) {
                return juce::Point<float>(12.0f + r * std::sin(ang), 12.0f - r * std::cos(ang));
            };
            const auto p1 = pt(rInner, a - flank), p2 = pt(rOuter, a - half), p3 = pt(rOuter, a + half),
                       p4 = pt(rInner, a + flank);
            if (i == 0)
                p.startNewSubPath(p1);
            else
                p.lineTo(p1);
            p.lineTo(p2);
            p.lineTo(p3);
            p.lineTo(p4);
        }
        p.closeSubPath();
        p.addEllipse(9.0f, 9.0f, 6.0f, 6.0f);
        break;
    }
    }
    return p;
}

/// Strokes `icon` into `area` (centred, square, scaled from the 24 grid)
/// at kStroke pixels whatever the size.
inline void draw(juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour)
{
    if (icon == Icon::None || area.isEmpty()) return;
    auto p = path(icon);
    const float side = juce::jmin(area.getWidth(), area.getHeight());
    const auto box = area.withSizeKeepingCentre(side, side);
    p.applyTransform(juce::AffineTransform::scale(side / kGrid).translated(box.getX(), box.getY()));
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(kStroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

} // namespace khensu::nav_icons
