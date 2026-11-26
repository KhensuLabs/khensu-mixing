#include "KhensuLookAndFeel.h"
#include "ButtonRole.h"
#include "KhensuTheme.h"
#include "LiquidSmokeBackdrop.h"
#include <cmath>
#include <vector>

namespace {

// One separable box-blur pass (horizontal or vertical) on ARGB images,
// through BitmapData: the premultiplied channels are averaged directly (the
// correct blur where alpha varies; identical to the old per-Colour average
// for the opaque backgrounds this runs on). A plain double-buffer, since each
// pass must read its UNMODIFIED neighbours, not partially-blurred ones.
// getPixelAt()/setPixelAt() used to do this; on a native (Direct2D) image
// each call maps the bitmap, which made a whole-window backdrop cost ~150 ms
// at 4K.
juce::Image boxBlurPass(const juce::Image& src, bool horizontal, int radius)
{
    const int w = src.getWidth(), h = src.getHeight();
    juce::Image dst(juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
    const juce::Image::BitmapData in(src, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData out(dst, juce::Image::BitmapData::writeOnly);
    const int count = 2 * radius + 1;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int acc[4] = {0, 0, 0, 0};
            for (int d = -radius; d <= radius; ++d) {
                const int sx = horizontal ? juce::jlimit(0, w - 1, x + d) : x;
                const int sy = horizontal ? y : juce::jlimit(0, h - 1, y + d);
                const juce::uint8* px = in.getPixelPointer(sx, sy);
                for (int c = 0; c < 4; ++c) acc[c] += px[c];
            }
            juce::uint8* q = out.getPixelPointer(x, y);
            for (int c = 0; c < 4; ++c) q[c] = static_cast<juce::uint8>(acc[c] / count);
        }
    }
    return dst;
}

} // namespace

namespace khensu {

KhensuLookAndFeel::KhensuLookAndFeel()
{
    applyThemeColours();
}

KhensuLookAndFeel::~KhensuLookAndFeel() = default;

void KhensuLookAndFeel::applyThemeColours()
{
    setColour(juce::ResizableWindow::backgroundColourId, KhensuTheme::bgVoid);
    setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    setColour(juce::TextButton::buttonColourId, KhensuTheme::bgSurface);
    setColour(juce::TextButton::buttonOnColourId, KhensuTheme::accentTealDim);
    setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    setColour(juce::TextButton::textColourOnId, KhensuTheme::accentTeal);
    setColour(juce::ComboBox::backgroundColourId, KhensuTheme::bgRaised);
    setColour(juce::ComboBox::textColourId, KhensuTheme::textPrimary);
    setColour(juce::ComboBox::outlineColourId, KhensuTheme::borderSubtle);
    setColour(juce::ComboBox::arrowColourId, KhensuTheme::textTertiary);
    setColour(juce::Slider::thumbColourId, KhensuTheme::accentTeal);
    setColour(juce::Slider::trackColourId, KhensuTheme::bgRaised);
    setColour(juce::Slider::backgroundColourId, KhensuTheme::bgSurface);
    setColour(juce::Slider::rotarySliderFillColourId, KhensuTheme::accentTeal);
    setColour(juce::Slider::rotarySliderOutlineColourId, KhensuTheme::borderSubtle);
    // Value boxes beside sliders: JUCE's default was white text, invisible
    // on the Light theme's surfaces (the Issues screen's Fix amount).
    setColour(juce::Slider::textBoxTextColourId, KhensuTheme::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, KhensuTheme::bgRaised);
    setColour(juce::Slider::textBoxOutlineColourId, KhensuTheme::borderSubtle);
    setColour(juce::Slider::textBoxHighlightColourId, KhensuTheme::accentTealDim);
}

juce::Font KhensuLookAndFeel::getLabelFont(juce::Label& label)
{
    // A label that set one of the theme's fonts (bundled Space Grotesk or
    // JetBrains Mono) is drawn in it. This override used to return 13 px
    // plain for every label, so each label.setFont() in the app - section
    // headings, small notes, mono readouts - was silently ignored (found with
    // UX-15). Any other font (JUCE's default, never set) keeps the 13 px UI
    // font: the default face can render UTF-8 as mojibake.
    const auto font = label.getFont();
    static const juce::String uiFace = khensu::KhensuTheme::uiFont(khensu::type::body).getTypefaceName();
    static const juce::String monoFace = khensu::KhensuTheme::monoFont(khensu::type::body).getTypefaceName();
    const auto face = font.getTypefaceName();
    static const juce::String numericFace = khensu::KhensuTheme::numericFont(khensu::type::body).getTypefaceName(); // UI-4
    if (face == uiFace || face == monoFace || face == numericFace)
        return font.withHeight(std::max(font.getHeight(), khensu::KhensuTheme::kMinTextPx));
    return khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::plain);
}

juce::Font KhensuLookAndFeel::getTextButtonFont(juce::TextButton& /*button*/, int buttonHeight)
{
    // UX-4: a 12 px floor - min(14, h * 0.44) gave a 20 px button 8.8 px text.
    // Capped at 80% of the height so a button a layout squeezed below ~15 px
    // (a layout bug of its own) shrinks its label instead of spilling it.
    const float h = (float)buttonHeight;
    return khensu::KhensuTheme::uiFont(juce::jmin(juce::jlimit(12.0f, 14.0f, h * 0.5f), h * 0.8f), juce::Font::bold);
}

void KhensuLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted,
                                             bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    // LG-8: softer liquid-glass geometry (8 px, clamped for small icon buttons).
    const float cornerSize = juce::jmin(8.0f, bounds.getHeight() * 0.35f);

    const bool enabled = button.isEnabled();
    const bool primary = button_role::isPrimary(button);
    const bool toggled = button.getToggleState();
    const bool strong = primary || toggled;
    const bool light = KhensuTheme::currentTheme() == KhensuTheme::Theme::Light;

    // LG-13: soft teal ambient glow underneath enabled primary buttons.
    if (primary && enabled && !shouldDrawButtonAsDown) {
        const float glowAlpha = shouldDrawButtonAsHighlighted ? 0.30f : 0.20f;
        auto glowRect = bounds.translated(0.0f, 2.0f).expanded(1.5f, 1.0f);
        g.setColour(KhensuTheme::accentTeal.withAlpha(glowAlpha));
        g.fillRoundedRectangle(glowRect, cornerSize + 1.5f);
    }

    juce::Colour baseColour = primary ? KhensuTheme::accentTeal.withAlpha(0.95f)
                                      : backgroundColour.withAlpha(strong ? 0.62f : 0.26f);

    if (!enabled)
        baseColour = baseColour.withMultipliedSaturation(0.25f).withMultipliedAlpha(0.45f);
    else if (shouldDrawButtonAsDown)
        baseColour = baseColour.darker(0.24f).withAlpha(primary ? 0.92f : 0.80f);
    else if (shouldDrawButtonAsHighlighted)
        baseColour = baseColour.brighter(0.10f).withAlpha(primary ? 0.98f : 0.72f);

    if (primary && enabled) {
        // LG-13: vertical teal luminance gradient for flagship primary CTA.
        juce::ColourGradient grad(baseColour.brighter(0.14f), bounds.getCentreX(), bounds.getY(),
                                  baseColour.darker(0.12f), bounds.getCentreX(), bounds.getBottom(), false);
        g.setGradientFill(grad);
    } else {
        g.setColour(baseColour);
    }
    g.fillRoundedRectangle(bounds, cornerSize);

    // Soft glass top sheen on enabled buttons.
    if (enabled) {
        const auto sheenTop = primary ? juce::Colours::white.withAlpha(0.24f)
                                      : KhensuTheme::highlight.withMultipliedAlpha(light ? 1.0f : 1.8f);
        juce::ColourGradient sheen(sheenTop, bounds.getCentreX(), bounds.getY(), sheenTop.withAlpha(0.0f),
                                   bounds.getCentreX(), bounds.getY() + bounds.getHeight() * 0.52f, false);
        g.setGradientFill(sheen);
        g.fillRoundedRectangle(bounds, cornerSize);
    }

    // LG-9: interactive cursor-tracking specular sheen on hover.
    if (enabled && shouldDrawButtonAsHighlighted) {
        drawCursorSheen(g, bounds, cornerSize, button.getMouseXYRelative().toFloat(), primary ? 0.9f : 1.1f);
    }

    // LG-5: curved Fresnel specular border (bright top-left, subtle bounce bottom-right).
    const juce::Colour edgeBright =
        primary && enabled ? juce::Colours::white.withAlpha(0.42f)
        : (enabled && shouldDrawButtonAsHighlighted) ? KhensuTheme::borderActive.withMultipliedAlpha(1.8f)
                                                     : KhensuTheme::borderActive;
    const juce::Colour edgeQuiet =
        enabled ? KhensuTheme::borderSubtle : KhensuTheme::borderSubtle.withMultipliedAlpha(0.6f);
    juce::ColourGradient borderGrad(enabled ? edgeBright : edgeQuiet, bounds.getTopLeft(), edgeQuiet,
                                    bounds.getBottomRight(), false);
    if (enabled && toggled && !primary)
        borderGrad.addColour(0.5, KhensuTheme::accentTeal.withAlpha(0.45f));
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, cornerSize, 1.0f);

    // Crisp top inner specular highlight on primary buttons (LG-13).
    if (primary && enabled && bounds.getWidth() > cornerSize * 2.5f) {
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.drawHorizontalLine(juce::roundToInt(bounds.getY() + 1.0f), bounds.getX() + cornerSize,
                             bounds.getRight() - cornerSize);
    }

    // Keyboard-focus ring (accessibility).
    if (enabled && button.hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.92f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.6f);
    }
}

void KhensuLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                       bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/)
{
    auto font = getTextButtonFont(button, button.getHeight());
    g.setFont(font);
    // Disabled text drops to the tertiary tone so the label matches the dimmed
    // background drawn above - previously it stayed at full strength, leaving a
    // crisp label on a faded plate (kimi audit P0 #3).
    juce::Colour text = button.findColour(button.isMouseButtonDown() ? juce::TextButton::textColourOnId
                                                                     : juce::TextButton::textColourOffId);
    if (button_role::isPrimary(button)) {
        // On a filled primary the accent text colour would vanish into the
        // fill: take whichever of the two text tokens contrasts more.
        const auto fill = KhensuTheme::accentTeal;
        const auto contrast = [&fill](juce::Colour c) {
            return std::abs(c.getPerceivedBrightness() - fill.getPerceivedBrightness());
        };
        text = contrast(KhensuTheme::bgBase) > contrast(KhensuTheme::textPrimary) ? KhensuTheme::bgBase
                                                                                  : KhensuTheme::textPrimary;
    }
    g.setColour(!button.isEnabled() ? KhensuTheme::textTertiary : text);

    auto yIndent = juce::jmin(4, button.getHeight() / 8);
    auto leftIndent = juce::jmin(8, button.getWidth() / 12);

    // Fitted: with the 12 px floor a long label in a narrow button narrows a
    // little before it ellipsizes.
    g.drawFittedText(button.getButtonText(), leftIndent, yIndent, button.getWidth() - (leftIndent * 2),
                     button.getHeight() - (yIndent * 2), juce::Justification::centred, 1, 0.85f);
}

void KhensuLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                         juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical) {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style,
                                               slider);
        return;
    }
    const bool horizontal = style == juce::Slider::LinearHorizontal;
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float track = 5.0f;
    const auto centre = bounds.getCentre();
    const auto start = horizontal ? juce::Point<float>(bounds.getX(), centre.y) : juce::Point<float>(centre.x, bounds.getBottom());
    const auto end = horizontal ? juce::Point<float>(bounds.getRight(), centre.y) : juce::Point<float>(centre.x, bounds.getY());
    const auto value = horizontal ? juce::Point<float>(sliderPos, centre.y) : juce::Point<float>(centre.x, sliderPos);
    const float alpha = slider.isEnabled() ? 1.0f : 0.4f;

    // LG-13: recessed translucent glass channel.
    juce::Path back;
    back.startNewSubPath(start);
    back.lineTo(end);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.55f * alpha));
    g.strokePath(back, juce::PathStrokeType(track + 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(KhensuTheme::borderActive.withMultipliedAlpha(0.75f * alpha));
    g.strokePath(back, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Luminous active fill (soft halo + crisp core).
    juce::Path fill;
    fill.startNewSubPath(start);
    fill.lineTo(value);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.24f * alpha));
    g.strokePath(fill, juce::PathStrokeType(track + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(KhensuTheme::accentTeal.withMultipliedAlpha(alpha));
    g.strokePath(fill, juce::PathStrokeType(track, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // LG-13: frosted glass thumb pill with specular rim and subtle shadow.
    const float thumb = juce::jmin(15.0f, (horizontal ? bounds.getHeight() : bounds.getWidth()) - 2.0f);
    const auto thumbRect = juce::Rectangle<float>(thumb, thumb).withCentre(value);
    g.setColour(KhensuTheme::shadowColour.withMultipliedAlpha(0.65f * alpha));
    g.fillEllipse(thumbRect.translated(0.0f, 1.5f));
    juce::ColourGradient thumbGrad(KhensuTheme::textPrimary.withMultipliedAlpha(0.95f * alpha), thumbRect.getTopLeft(),
                                   KhensuTheme::bgOverlay.brighter(0.35f).withMultipliedAlpha(0.92f * alpha),
                                   thumbRect.getBottomRight(), false);
    g.setGradientFill(thumbGrad);
    g.fillEllipse(thumbRect);
    g.setColour(juce::Colours::white.withAlpha(0.55f * alpha));
    g.drawEllipse(thumbRect.reduced(0.5f), 1.0f);

    if (slider.hasKeyboardFocus(true)) {
        g.setColour(KhensuTheme::focusRing);
        g.drawEllipse(juce::Rectangle<float>(thumb + 4.0f, thumb + 4.0f).withCentre(value), 1.5f);
    }
}

void KhensuLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& slider)
{
    auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);

    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(6.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto toX = bounds.getCentreX();
    auto toY = bounds.getCentreY();
    auto lineW = 3.5f;
    auto arcRadius = radius - lineW * 1.5f;

    // Recessed background track arc.
    juce::Path backgroundArc;
    backgroundArc.addCentredArc(toX, toY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.45f));
    g.strokePath(backgroundArc,
                 juce::PathStrokeType(lineW + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(KhensuTheme::bgRaised);
    g.strokePath(backgroundArc,
                 juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto valArcAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    if (slider.isEnabled()) {
        // LG-13: two-pass glowing value arc (wide low-alpha halo + crisp core).
        juce::Path valueArc;
        valueArc.addCentredArc(toX, toY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, valArcAngle, true);
        g.setColour(fill.withAlpha(0.24f));
        g.strokePath(valueArc,
                     juce::PathStrokeType(lineW + 4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(fill);
        g.strokePath(valueArc,
                     juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // LG-13: beveled glass center cap with drop shadow and radial specular sheen.
    const float capRadius = radius * 0.48f;
    const auto capRect = juce::Rectangle<float>(toX - capRadius, toY - capRadius, capRadius * 2.0f, capRadius * 2.0f);
    g.setColour(KhensuTheme::shadowColour.withAlpha(0.5f));
    g.fillEllipse(capRect.translated(0.0f, 2.0f));

    juce::ColourGradient capGrad(KhensuTheme::bgOverlay.brighter(0.18f), toX - capRadius * 0.45f,
                                 toY - capRadius * 0.45f, KhensuTheme::bgVoid.brighter(0.03f), toX + capRadius,
                                 toY + capRadius, true);
    g.setGradientFill(capGrad);
    g.fillEllipse(capRect);

    juce::ColourGradient capBorder(KhensuTheme::borderActive.brighter(0.4f), capRect.getTopLeft(),
                                   KhensuTheme::borderSubtle, capRect.getBottomRight(), false);
    g.setGradientFill(capBorder);
    g.drawEllipse(capRect.reduced(0.5f), 1.0f);

    if (slider.isEnabled()) {
        // Luminous indicator pip + pointer needle.
        const float sinA = std::sin(valArcAngle);
        const float cosA = std::cos(valArcAngle);
        const float needleStart = capRadius * 0.45f;
        const float needleEnd = radius * 0.86f;
        juce::Path needle;
        needle.startNewSubPath(toX + needleStart * sinA, toY - needleStart * cosA);
        needle.lineTo(toX + needleEnd * sinA, toY - needleEnd * cosA);
        g.setColour(fill.withAlpha(0.32f));
        g.strokePath(needle, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(fill.brighter(0.25f));
        g.strokePath(needle, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float pipX = toX + (capRadius * 0.68f) * sinA;
        const float pipY = toY - (capRadius * 0.68f) * cosA;
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.fillEllipse(pipX - 1.8f, pipY - 1.8f, 3.6f, 3.6f);
    }
}

void KhensuLookAndFeel::drawGlassmorphicBackground(juce::Graphics& g, juce::Rectangle<int> bounds,
                                                   const std::vector<float>& waveformPeaks, double playbackProgress)
{
    // Calm, near-black background (kimi audit P0: retire the glowing neon
    // teal/purple orbs - they read as generic AI-SaaS, not a pro audio tool).
    // A whisper of vertical gradient gives depth without colour; the palette's
    // own bgVoid drives it so themes stay consistent.
    const juce::Colour top = KhensuTheme::bgVoid;
    const juce::Colour bottom = KhensuTheme::bgVoid.brighter(0.015f);
    juce::ColourGradient bgGrad(top, 0.0f, 0.0f, bottom, 0.0f, (float)bounds.getHeight(), false);
    g.setGradientFill(bgGrad);
    g.fillAll();

    // Draw background downsampled playhead waveform backdrop if peaks exist
    if (!waveformPeaks.empty()) {
        juce::Path p;
        float w = (float)bounds.getWidth() - 200.0f;        // leave room for sidebar
        float startX = 170.0f;                              // start after sidebar
        float h = 120.0f;                                   // waveform height
        float centerY = (float)bounds.getBottom() - 100.0f; // position near bottom

        float step = w / (float)waveformPeaks.size();
        for (size_t i = 0; i < waveformPeaks.size(); ++i) {
            float x = startX + i * step;
            float val = waveformPeaks[i] * (h * 0.5f);
            if (i == 0)
                p.startNewSubPath(x, centerY - val);
            else
                p.lineTo(x, centerY - val);
        }
        for (int i = (int)waveformPeaks.size() - 1; i >= 0; --i) {
            float x = startX + i * step;
            float val = waveformPeaks[(size_t)i] * (h * 0.5f);
            p.lineTo(x, centerY + val);
        }
        p.closeSubPath();

        // Draw waveform path with low contrast ambient color
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.04f));
        g.fillPath(p);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.06f));
        g.strokePath(p, juce::PathStrokeType(1.0f));

        // UI-7: no full-height playhead line here. It swept across every
        // card while playing - motion that explained nothing. The playback
        // position shows on the transport and the waveform only.
        juce::ignoreUnused(playbackProgress);
    }
}

namespace {

juce::Colour glassFill(GlassPanelVariant variant)
{
    return (variant == GlassPanelVariant::Raised) ? KhensuTheme::bgRaised.withAlpha(0.84f)
                                                  : KhensuTheme::bgSurface.withAlpha(0.56f);
}

/// Shared body of both drawGlassPanel() overloads; `drawBackdrop` paints the
/// blurred backdrop (clipped to the panel's rounded shape) or nothing.
template <typename DrawBackdrop>
void drawGlassPanelImpl(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                        juce::Rectangle<int> tintedCover, DrawBackdrop&& drawBackdrop)
{
    auto b = bounds.toFloat().reduced(0.5f);
    // LG-8: softer liquid-glass geometry (12 px, clamped for small panels).
    const float radius = juce::jmin(12.0f, juce::jmin(b.getWidth(), b.getHeight()) * 0.35f);
    const int inset = 1 + static_cast<int>(radius);
    tintedCover = tintedCover.getIntersection(bounds);
    const bool light = KhensuTheme::currentTheme() == KhensuTheme::Theme::Light;

    // ── Ambient + Contact Shadow ────────────────────────────────────
    {
        const float shadowScale = (variant == GlassPanelVariant::Raised) ? 1.35f : 0.95f;
        const float shadowOffset = (variant == GlassPanelVariant::Raised) ? 10.0f : 5.0f;
        auto shadow = b.translated(0.0f, shadowOffset).expanded(shadowOffset * 0.45f);
        juce::ColourGradient sg(KhensuTheme::shadowColour.withMultipliedAlpha(shadowScale), shadow.getCentreX(),
                                shadow.getY(), juce::Colours::transparentBlack, shadow.getCentreX(), shadow.getBottom(),
                                false);
        g.setGradientFill(sg);
        g.saveState();
        if (!tintedCover.isEmpty()) {
            g.excludeClipRegion(bounds.reduced(inset, 1).getIntersection(tintedCover));
            g.excludeClipRegion(bounds.reduced(1, inset).getIntersection(tintedCover));
        }
        g.fillRoundedRectangle(shadow, radius + 2.0f);
        g.restoreState();
    }

    // ── Fill + Live Refracted Backdrop + Micro-Grain ────────────────
    if (variant == GlassPanelVariant::Void) {
        g.setColour(KhensuTheme::bgBase);
        g.fillRoundedRectangle(b, radius);
    } else {
        drawBackdrop(b, radius);

        g.setColour(glassFill(variant));
        g.saveState();
        if (!tintedCover.isEmpty()) g.excludeClipRegion(tintedCover);
        g.fillRoundedRectangle(b, radius);
        g.restoreState();

        // LG-7: etched-crystal micro-grain texture (tileable 128x128 blue-noise).
        if (!light) {
            const auto& grain = LiquidSmokeBackdrop::microGrainTexture();
            if (grain.isValid()) {
                g.saveState();
                juce::Path clip;
                clip.addRoundedRectangle(b, radius);
                g.reduceClipRegion(clip);
                g.setFillType(juce::FillType(grain, juce::AffineTransform()));
                g.fillRect(b);
                g.restoreState();
            }
        }

        // Soft curved liquid-glass top sheen catching ambient light.
        const auto sheenTop = light ? juce::Colours::white.withAlpha(0.40f)
                                    : juce::Colours::white.withAlpha(variant == GlassPanelVariant::Raised ? 0.08f : 0.055f);
        juce::ColourGradient sheen(sheenTop, b.getCentreX(), b.getY(), sheenTop.withAlpha(0.0f), b.getCentreX(),
                                   b.getY() + b.getHeight() * 0.42f, false);
        g.setGradientFill(sheen);
        g.saveState();
        g.reduceClipRegion(bounds.withHeight(static_cast<int>(std::ceil(b.getHeight() * 0.44f)) + 2));
        g.fillRoundedRectangle(b, radius);
        g.restoreState();

        // Subtle inner edge-lensing glow along the bottom-right rim (LG-6).
        if (!light) {
            juce::ColourGradient bounce(KhensuTheme::accentTeal.withAlpha(0.0f), b.getCentreX(),
                                        b.getBottom() - juce::jmin(28.0f, b.getHeight() * 0.3f),
                                        KhensuTheme::accentTeal.withAlpha(0.035f), b.getRight(), b.getBottom(), false);
            g.setGradientFill(bounce);
            g.fillRoundedRectangle(b, radius);
        }
    }

    // ── LG-5: Layered Curved Fresnel Specular Rim ───────────────────
    if (variant != GlassPanelVariant::Void) {
        // Outer 1 px dark separation hairline (gives physical glass thickness).
        if (!light) {
            g.setColour(KhensuTheme::shadowColour.withAlpha(0.45f));
            g.drawRoundedRectangle(b.expanded(0.5f), radius + 0.5f, 1.0f);
        }

        // Curved Fresnel rim: bright top-left specular (slightly teal-tinted),
        // falling to transparent mid-edge, rising softly at bottom-right bounce.
        const auto rimTopLeft =
            light ? juce::Colours::white.withAlpha(0.85f)
                  : juce::Colours::white.interpolatedWith(KhensuTheme::accentTeal, 0.16f)
                        .withAlpha(variant == GlassPanelVariant::Raised ? 0.28f : 0.21f);
        const auto rimMid = KhensuTheme::borderSubtle.withMultipliedAlpha(0.85f);
        const auto rimBounce =
            light ? KhensuTheme::borderSubtle
                  : KhensuTheme::accentTeal.interpolatedWith(juce::Colours::white, 0.35f).withAlpha(0.08f);

        juce::ColourGradient rim(rimTopLeft, b.getX(), b.getY(), rimBounce, b.getRight(), b.getBottom(), false);
        rim.addColour(0.45, rimMid);
        g.setGradientFill(rim);
        g.drawRoundedRectangle(b, radius, 1.0f);

        // Crisp inner top specular lip following the rounded top edge.
        if (b.getWidth() > radius * 2.5f && b.getHeight() > 16.0f) {
            const auto lip = light ? juce::Colours::white.withAlpha(0.65f) : juce::Colours::white.withAlpha(0.14f);
            juce::ColourGradient topLip(lip.withAlpha(0.0f), b.getX() + radius * 0.5f, b.getY() + 1.0f,
                                        lip.withAlpha(0.0f), b.getRight() - radius * 0.5f, b.getY() + 1.0f, false);
            topLip.addColour(0.28, lip);
            topLip.addColour(0.72, lip.withMultipliedAlpha(0.65f));
            g.setGradientFill(topLip);
            g.drawLine(b.getX() + radius * 0.7f, b.getY() + 1.0f, b.getRight() - radius * 0.7f, b.getY() + 1.0f, 1.0f);
        }
    }
}

void clipToPanel(juce::Graphics& g, juce::Rectangle<float> b, float radius)
{
    juce::Path clip;
    clip.addRoundedRectangle(b, radius);
    g.reduceClipRegion(clip);
}

/// Identifies everything drawAppBackground() reads, so a cached backdrop is
/// rebuilt when the theme (or any of its background tokens) changes.
juce::uint64 backdropPaletteKey()
{
    juce::uint64 key = static_cast<juce::uint64>(KhensuTheme::currentTheme());
    for (auto c : {KhensuTheme::bgBase, KhensuTheme::bgVoid, KhensuTheme::accentTeal, KhensuTheme::accentMagenta})
        key = key * 1000003u ^ c.getARGB();
    return key;
}

struct BackdropCacheEntry {
    juce::Rectangle<int> bounds;
    juce::uint64 palette = 0;
    juce::uint64 smokeGen = 0;
    juce::uint32 tint = 0;
    juce::Image image; // 160x90 native blurred+tinted image for fast GPU sampling
};
BackdropCacheEntry& backdropCacheSlot(int idx)
{
    static BackdropCacheEntry entries[4];
    return entries[idx & 3];
}
juce::Rectangle<int>& lastBackdropAppBounds()
{
    static juce::Rectangle<int> b;
    return b;
}

} // namespace

void KhensuLookAndFeel::drawGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                                       juce::Colour accentColour, const juce::Image& blurredBackdrop)
{
    juce::ignoreUnused(accentColour);
    drawGlassPanelImpl(g, bounds, variant, {}, [&](juce::Rectangle<float> b, float radius) {
        if (!blurredBackdrop.isValid()) return;
        g.saveState();
        clipToPanel(g, b, radius);
        g.drawImage(blurredBackdrop, b, juce::RectanglePlacement::stretchToFit);
        g.restoreState();
    });
}

void KhensuLookAndFeel::drawGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                                       const juce::Image& appBackdrop, juce::Rectangle<int> backdropSource)
{
    if (!appBackdrop.isValid() || backdropSource.isEmpty()) {
        drawGlassPanelImpl(g, bounds, variant, {}, [](juce::Rectangle<float>, float) {});
        return;
    }

    const auto appB = lastBackdropAppBounds();
    const bool isLowResSmoke = (appBackdrop.getWidth() == LiquidSmokeBackdrop::kBufW &&
                                appBackdrop.getHeight() == LiquidSmokeBackdrop::kBufH && !appB.isEmpty());

    if (isLowResSmoke) {
        // LG-2 + LG-6: map panel rect into the 160x90 blurred+tinted smoke texture
        // with a subtle 1.04x optical-refraction zoom around the panel centre.
        const float sx = static_cast<float>(appBackdrop.getWidth()) / static_cast<float>(appB.getWidth());
        const float sy = static_cast<float>(appBackdrop.getHeight()) / static_cast<float>(appB.getHeight());
        const float cx = (static_cast<float>(backdropSource.getCentreX()) - static_cast<float>(appB.getX())) * sx;
        const float cy = (static_cast<float>(backdropSource.getCentreY()) - static_cast<float>(appB.getY())) * sy;
        constexpr float kRefractZoom = 1.04f;
        const float sw = juce::jmax(1.0f, static_cast<float>(backdropSource.getWidth()) * sx / kRefractZoom);
        const float sh = juce::jmax(1.0f, static_cast<float>(backdropSource.getHeight()) * sy / kRefractZoom);
        const int rx = juce::jlimit(0, appBackdrop.getWidth() - 1, juce::roundToInt(cx - sw * 0.5f));
        const int ry = juce::jlimit(0, appBackdrop.getHeight() - 1, juce::roundToInt(cy - sh * 0.5f));
        const int rw = juce::jlimit(1, appBackdrop.getWidth() - rx, juce::roundToInt(sw));
        const int rh = juce::jlimit(1, appBackdrop.getHeight() - ry, juce::roundToInt(sh));

        drawGlassPanelImpl(g, bounds, variant, bounds, [&](juce::Rectangle<float> b, float radius) {
            g.saveState();
            clipToPanel(g, b, radius);
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(appBackdrop, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), rx, ry, rw,
                        rh);
            g.restoreState();
        });
        return;
    }

    const auto src = backdropSource.getIntersection(appBackdrop.getBounds());
    const auto cover = src.translated(bounds.getX() - backdropSource.getX(), bounds.getY() - backdropSource.getY());
    drawGlassPanelImpl(g, bounds, variant, cover, [&](juce::Rectangle<float> b, float radius) {
        if (src.isEmpty()) return;
        g.saveState();
        clipToPanel(g, b, radius);
        g.drawImage(appBackdrop, cover.getX(), cover.getY(), src.getWidth(), src.getHeight(), src.getX(), src.getY(),
                    src.getWidth(), src.getHeight());
        g.restoreState();
    });
}

void KhensuLookAndFeel::drawCursorSheen(juce::Graphics& g, juce::Rectangle<float> bounds, float cornerRadius,
                                        juce::Point<float> cursorPos, float strength)
{
    if (bounds.isEmpty() || strength <= 0.0f) return;
    const bool light = KhensuTheme::currentTheme() == KhensuTheme::Theme::Light;
    const float r = juce::jlimit(48.0f, 160.0f, juce::jmax(bounds.getWidth(), bounds.getHeight()) * 0.42f);
    const auto tint = light ? KhensuTheme::accentTeal : juce::Colours::white.interpolatedWith(KhensuTheme::accentTeal, 0.25f);

    g.saveState();
    juce::Path clip;
    clip.addRoundedRectangle(bounds, cornerRadius);
    g.reduceClipRegion(clip);
    juce::ColourGradient glow(tint.withAlpha((light ? 0.08f : 0.065f) * strength), cursorPos.x, cursorPos.y,
                              tint.withAlpha(0.0f), cursorPos.x + r, cursorPos.y, true);
    g.setGradientFill(glow);
    g.fillRect(bounds);
    g.restoreState();

    // Localized rim brightening near the cursor.
    juce::ColourGradient rimGlow(tint.withAlpha((light ? 0.26f : 0.24f) * strength), cursorPos.x, cursorPos.y,
                                 tint.withAlpha(0.0f), cursorPos.x + r * 0.75f, cursorPos.y, true);
    g.setGradientFill(rimGlow);
    g.drawRoundedRectangle(bounds, cornerRadius, 1.0f);
}

void KhensuLookAndFeel::drawFrostedRail(juce::Graphics& g, juce::Rectangle<int> bounds, juce::Rectangle<int> appBounds,
                                        juce::Rectangle<int> regionInAppSpace, bool topEdgeHighlight,
                                        bool rightEdgeHighlight)
{
    if (bounds.isEmpty()) return;
    const bool light = KhensuTheme::currentTheme() == KhensuTheme::Theme::Light;

    if (!appBounds.isEmpty() && !regionInAppSpace.isEmpty()) {
        auto backdrop = blurredAppBackdrop(appBounds, KhensuTheme::bgBase.withAlpha(light ? 0.84f : 0.76f));
        if (backdrop.isValid()) {
            const float sx = static_cast<float>(backdrop.getWidth()) / static_cast<float>(appBounds.getWidth());
            const float sy = static_cast<float>(backdrop.getHeight()) / static_cast<float>(appBounds.getHeight());
            const int rx = juce::jlimit(0, backdrop.getWidth() - 1,
                                        juce::roundToInt((regionInAppSpace.getX() - appBounds.getX()) * sx));
            const int ry = juce::jlimit(0, backdrop.getHeight() - 1,
                                        juce::roundToInt((regionInAppSpace.getY() - appBounds.getY()) * sy));
            const int rw = juce::jlimit(1, backdrop.getWidth() - rx, juce::roundToInt(regionInAppSpace.getWidth() * sx));
            const int rh =
                juce::jlimit(1, backdrop.getHeight() - ry, juce::roundToInt(regionInAppSpace.getHeight() * sy));
            g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
            g.drawImage(backdrop, bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), rx, ry, rw, rh);
        } else {
            g.fillAll(KhensuTheme::bgBase);
        }
    } else {
        g.fillAll(KhensuTheme::bgBase);
    }

    // Subtle etched micro-grain on dark rails.
    if (!light) {
        const auto& grain = LiquidSmokeBackdrop::microGrainTexture();
        if (grain.isValid()) {
            g.setFillType(juce::FillType(grain, juce::AffineTransform()));
            g.fillRect(bounds);
        }
    }

    if (topEdgeHighlight) {
        const auto rim = light ? KhensuTheme::borderActive : juce::Colours::white.withAlpha(0.14f);
        juce::ColourGradient topGrad(rim.withMultipliedAlpha(0.4f), 0.0f, 0.0f, rim.withMultipliedAlpha(0.4f),
                                     static_cast<float>(bounds.getWidth()), 0.0f, false);
        topGrad.addColour(0.35, rim);
        topGrad.addColour(0.65, KhensuTheme::accentTeal.withAlpha(light ? 0.25f : 0.18f));
        g.setGradientFill(topGrad);
        g.fillRect(bounds.getX(), bounds.getY(), bounds.getWidth(), 1);
    }

    if (rightEdgeHighlight) {
        const auto rim = light ? KhensuTheme::borderSubtle : juce::Colours::white.withAlpha(0.10f);
        juce::ColourGradient rightGrad(rim, static_cast<float>(bounds.getRight() - 1), 0.0f,
                                       KhensuTheme::borderSubtle, static_cast<float>(bounds.getRight() - 1),
                                       static_cast<float>(bounds.getHeight()), false);
        g.setGradientFill(rightGrad);
        g.fillRect(bounds.getRight() - 1, bounds.getY(), 1, bounds.getHeight());
    }
}

juce::Image KhensuLookAndFeel::glassBackdrop(juce::Rectangle<int> appBounds, GlassPanelVariant variant)
{
    if (variant == GlassPanelVariant::Void) return {};
    return blurredAppBackdrop(appBounds, glassFill(variant));
}

juce::Image KhensuLookAndFeel::blurredAppBackdrop(juce::Rectangle<int> appBounds, juce::Colour tint)
{
    if (appBounds.isEmpty()) return {};
    lastBackdropAppBounds() = appBounds;

    static int nextSlot = 0;
    const auto palette = backdropPaletteKey();
    const auto smokeGen = LiquidSmokeBackdrop::instance().generation();
    for (int i = 0; i < 4; ++i) {
        const auto& e = backdropCacheSlot(i);
        if (e.image.isValid() && e.bounds == appBounds && e.palette == palette && e.smokeGen == smokeGen &&
            e.tint == tint.getARGB())
            return e.image;
    }

    // LG-2: take the live 160x90 smoke frame, apply 1 fast separable box blur +
    // glass tint, and upload as a tiny native texture for GPU bilinear refraction.
    auto small = boxBlurPass(LiquidSmokeBackdrop::instance().smokeSoftwareImage(), true, 2);
    small = boxBlurPass(small, false, 2);
    if (!tint.isTransparent()) {
        juce::Graphics g(small);
        g.fillAll(tint);
    }
    auto native = juce::NativeImageType().convert(small);
    backdropCacheSlot(nextSlot) = {appBounds, palette, smokeGen, tint.getARGB(), native};
    nextSlot = (nextSlot + 1) % 4;
    return native;
}

void KhensuLookAndFeel::drawAppBackground(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty()) return;
    // LG-1: hardware bilinear upscale of the live 160x90 liquid smoke field.
    const auto& smoke = LiquidSmokeBackdrop::instance().smokeImage();
    if (smoke.isValid()) {
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImage(smoke, bounds.toFloat(), juce::RectanglePlacement::stretchToFit);
        return;
    }

    // Fallback wash if smoke image is unavailable.
    auto b = bounds.toFloat();
    juce::ColourGradient base(KhensuTheme::bgBase, b.getWidth() * 0.22f, -b.getHeight() * 0.10f, KhensuTheme::bgVoid,
                              b.getWidth() * 0.55f, b.getHeight() * 0.60f, true);
    g.setGradientFill(base);
    g.fillRect(b);
}

juce::Image KhensuLookAndFeel::renderBlurredBackdrop(juce::Rectangle<int> regionInAppSpace,
                                                     juce::Rectangle<int> appBounds)
{
    // The blur works on software pixels; hand callers a native image, which
    // is what they draw every paint.
    return juce::NativeImageType().convert(renderBlurredBackdropSoftware(regionInAppSpace, appBounds));
}

juce::Image KhensuLookAndFeel::renderBlurredBackdropSoftware(juce::Rectangle<int> regionInAppSpace,
                                                             juce::Rectangle<int> appBounds)
{
    if (regionInAppSpace.isEmpty() || appBounds.isEmpty()) return {};

    // Downsampling most of the way there is what actually buys the perf
    // headroom a per-panel, per-scroll-frame "manual blur" needs to be
    // viable; the box-blur pass below just smooths the residual block
    // structure left by stretching a very small image back to panel size.
    constexpr float kDownscale = 0.12f;
    const int smallW = juce::jmax(1, (int)std::lround(regionInAppSpace.getWidth() * kDownscale));
    const int smallH = juce::jmax(1, (int)std::lround(regionInAppSpace.getHeight() * kDownscale));

    juce::Image img(juce::Image::ARGB, smallW, smallH, true, juce::SoftwareImageType());
    {
        juce::Graphics g(img);
        // Maps an app-space point to this small image's pixel space:
        // dst = (appPoint - region.origin) * kDownscale. drawAppBackground()
        // itself is unaware of the crop - the transform does all the work,
        // so there's no duplicated background-drawing logic to keep in sync.
        g.addTransform(juce::AffineTransform(kDownscale, 0.0f, -kDownscale * (float)regionInAppSpace.getX(), 0.0f,
                                             kDownscale, -kDownscale * (float)regionInAppSpace.getY()));
        drawAppBackground(g, appBounds);
    }

    constexpr int kBoxRadius = 2;
    constexpr int kPasses = 2;
    for (int i = 0; i < kPasses; ++i) {
        img = boxBlurPass(img, true, kBoxRadius);
        img = boxBlurPass(img, false, kBoxRadius);
    }
    return img;
}

void KhensuLookAndFeel::draw3DHeatBar(juce::Graphics& g, juce::Rectangle<float> frontBounds, juce::Colour heatColour,
                                      float depth)
{
    // Fakes an extruded/isometric bar using three flat faces so 2D data reads
    // as a small 3D relief chart without pulling in an OpenGL pipeline:
    // a lit top face (depth foreshortened upward-right), a shaded right face,
    // then the front face on top with a vertical light->dark gradient.
    if (frontBounds.getWidth() <= 0.0f || frontBounds.getHeight() <= 0.0f) return;

    juce::Path top;
    top.startNewSubPath(frontBounds.getTopLeft());
    top.lineTo(frontBounds.getTopLeft().translated(depth, -depth));
    top.lineTo(frontBounds.getTopRight().translated(depth, -depth));
    top.lineTo(frontBounds.getTopRight());
    top.closeSubPath();
    g.setColour(heatColour.brighter(0.35f));
    g.fillPath(top);

    juce::Path side;
    side.startNewSubPath(frontBounds.getTopRight());
    side.lineTo(frontBounds.getTopRight().translated(depth, -depth));
    side.lineTo(frontBounds.getBottomRight().translated(depth, -depth));
    side.lineTo(frontBounds.getBottomRight());
    side.closeSubPath();
    g.setColour(heatColour.darker(0.45f));
    g.fillPath(side);

    juce::ColourGradient frontGrad(heatColour.brighter(0.1f), frontBounds.getX(), frontBounds.getY(),
                                   heatColour.darker(0.2f), frontBounds.getX(), frontBounds.getBottom(), false);
    g.setGradientFill(frontGrad);
    g.fillRect(frontBounds);
}

} // namespace khensu
