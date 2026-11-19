#include "KhensuLookAndFeel.h"
#include "ButtonRole.h"
#include "KhensuTheme.h"
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

// Bilinear resize of an ARGB image to w x h (pixel-centre aligned, edges
// clamped), 8-bit fixed-point weights. Separable and direct because it runs
// once per window size over every window pixel: each source row is expanded
// horizontally once, then each output row is a straight blend of two
// expanded rows (a contiguous loop the compiler vectorises). Graphics::
// drawImage's general transformed-image path cost several times as much.
juce::Image upscaleBilinear(const juce::Image& src, int w, int h)
{
    const int sw = src.getWidth(), sh = src.getHeight();
    juce::Image dst(juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
    const juce::Image::BitmapData in(src, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData out(dst, juce::Image::BitmapData::writeOnly);
    jassert(in.pixelStride == 4 && out.pixelStride == 4);

    auto sample = [](int i, int dstSize, int srcSize, int& i0, int& i1, int& wt) {
        float f = ((float)i + 0.5f) * (float)srcSize / (float)dstSize - 0.5f;
        f = juce::jlimit(0.0f, (float)(srcSize - 1), f);
        i0 = (int)f;
        i1 = juce::jmin(i0 + 1, srcSize - 1);
        wt = (int)((f - (float)i0) * 256.0f);
    };

    // Horizontally expanded source rows, channels scaled by 256 (uint16).
    const size_t rowLen = static_cast<size_t>(w) * 4;
    std::vector<juce::uint16> expanded(static_cast<size_t>(sh) * rowLen);
    {
        std::vector<int> x0(static_cast<size_t>(w)), x1(static_cast<size_t>(w)), wx(static_cast<size_t>(w));
        for (int x = 0; x < w; ++x)
            sample(x, w, sw, x0[(size_t)x], x1[(size_t)x], wx[(size_t)x]);
        for (int sy = 0; sy < sh; ++sy) {
            const juce::uint8* row = in.getLinePointer(sy);
            juce::uint16* e = expanded.data() + static_cast<size_t>(sy) * rowLen;
            for (int x = 0; x < w; ++x) {
                const juce::uint8* a = row + x0[(size_t)x] * 4;
                const juce::uint8* b = row + x1[(size_t)x] * 4;
                const int fx = wx[(size_t)x];
                for (int k = 0; k < 4; ++k)
                    e[x * 4 + k] = static_cast<juce::uint16>(a[k] * (256 - fx) + b[k] * fx);
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        int y0, y1, wy;
        sample(y, h, sh, y0, y1, wy);
        const juce::uint16* top = expanded.data() + static_cast<size_t>(y0) * rowLen;
        const juce::uint16* bottom = expanded.data() + static_cast<size_t>(y1) * rowLen;
        juce::uint8* o = out.getLinePointer(y);
        const juce::uint32 wb = static_cast<juce::uint32>(wy), wt = 256u - wb;
        for (size_t i = 0; i < rowLen; ++i)
            o[i] = static_cast<juce::uint8>((top[i] * wt + bottom[i] * wb + 32768u) >> 16);
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
    static const juce::String uiFace = khensu::KhensuTheme::uiFont(13.0f).getTypefaceName();
    static const juce::String monoFace = khensu::KhensuTheme::monoFont(13.0f).getTypefaceName();
    const auto face = font.getTypefaceName();
    if (face == uiFace || face == monoFace)
        return font.withHeight(std::max(font.getHeight(), khensu::KhensuTheme::kMinTextPx));
    return khensu::KhensuTheme::uiFont(13.0f, juce::Font::plain);
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
    auto cornerSize = 4.0f;

    // A disabled button must read as unavailable BEFORE it is hovered - it
    // previously drew identically to an enabled one, so dead controls looked
    // clickable (kimi audit P0 #2). Disabled also suppresses hover/down/focus
    // affordances below, since none of them can actually fire.
    const bool enabled = button.isEnabled();

    // UX-4 hierarchy: a primary (button_role::markPrimary) is filled; every
    // other button is a light plate whose border carries it, so a card's one
    // real action stands out from its eight helpers. A toggled-on button
    // reads as selected, so it keeps the stronger fill.
    const bool primary = button_role::isPrimary(button);
    const bool strong = primary || button.getToggleState();
    juce::Colour baseColour = primary ? KhensuTheme::accentTeal.withAlpha(0.92f)
                                       : backgroundColour.withAlpha(strong ? 0.60f : 0.22f);

    if (!enabled)
        baseColour = baseColour.withMultipliedSaturation(0.25f).withAlpha(0.28f);
    else if (shouldDrawButtonAsDown)
        baseColour = baseColour.darker(0.3f).withAlpha(0.80f);
    else if (shouldDrawButtonAsHighlighted)
        baseColour = baseColour.brighter(0.08f).withAlpha(0.75f);

    g.setColour(baseColour);
    g.fillRoundedRectangle(bounds, cornerSize);

    // Specular border: bright edge at the top-left falling to a subtle one at
    // the bottom-right. Sourced from the border tokens rather than literal
    // white -> near-black, which inverted correctly only in the dark themes and
    // left a white-on-white rim plus a black gash in Light (kimi audit P1 #16).
    const juce::Colour edgeBright = (enabled && shouldDrawButtonAsHighlighted)
                                        ? KhensuTheme::borderActive.withMultipliedAlpha(1.6f)
                                        : KhensuTheme::borderActive;
    juce::ColourGradient borderGrad(enabled ? edgeBright : KhensuTheme::borderSubtle, bounds.getTopLeft(),
                                    KhensuTheme::borderSubtle, bounds.getBottomRight(), false);
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, cornerSize, 1.0f);

    // Hover feedback: a restrained brighter border, not a neon glow
    // (kimi audit P0 - one accent, used sparingly).
    if (enabled && shouldDrawButtonAsHighlighted) {
        g.setColour(KhensuTheme::borderActive.withMultipliedAlpha(1.8f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize - 0.5f, 1.0f);
    }

    // Keyboard-focus ring (accessibility): a clear accent outline when the
    // button holds keyboard focus, so tab-navigation is visible - the audit's
    // focusRing token, applied.
    if (enabled && button.hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.9f));
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

void KhensuLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                         float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                         juce::Slider& slider)
{
    auto outline = slider.findColour(juce::Slider::rotarySliderOutlineColourId);
    auto fill = slider.findColour(juce::Slider::rotarySliderFillColourId);

    auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(6.0f);
    auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto toX = bounds.getCentreX();
    auto toY = bounds.getCentreY();
    auto lineW = 3.5f;
    auto arcRadius = radius - lineW * 1.5f;

    // Draw background track arc. bgRaised (not a fixed dark blue-grey) so the
    // unfilled part of the track stays visible against a light surface
    // (kimi audit P1 #16).
    juce::Path backgroundArc;
    backgroundArc.addCentredArc(toX, toY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(KhensuTheme::bgRaised);
    g.strokePath(backgroundArc,
                 juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (slider.isEnabled()) {
        // Draw active fill arc (neon cyan)
        auto valArcAngle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        juce::Path valueArc;
        valueArc.addCentredArc(toX, toY, arcRadius, arcRadius, 0.0f, rotaryStartAngle, valArcAngle, true);
        g.setColour(fill);
        g.strokePath(valueArc,
                     juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Draw pointer needle as a modern glowing cyan line from cap edge to track edge
        juce::Path needle;
        auto needleStart = radius * 0.40f;
        auto needleLength = radius * 0.85f;
        needle.startNewSubPath(toX + needleStart * std::sin(valArcAngle), toY - needleStart * std::cos(valArcAngle));
        needle.lineTo(toX + needleLength * std::sin(valArcAngle), toY - needleLength * std::cos(valArcAngle));
        g.setColour(KhensuTheme::accentTeal);
        g.strokePath(needle, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Draw center cap (sleek dark glass circular dial)
    auto capRadius = radius * 0.38f;
    g.setColour(KhensuTheme::bgVoid.brighter(0.04f));
    g.fillEllipse(toX - capRadius, toY - capRadius, capRadius * 2.0f, capRadius * 2.0f);

    // Draw spec border on cap - theme border tokens, so the rim inverts with
    // the palette instead of being a fixed white->near-black sweep.
    juce::ColourGradient capBorder(KhensuTheme::borderActive, toX - capRadius, toY - capRadius,
                                   KhensuTheme::borderSubtle, toX + capRadius, toY + capRadius, false);
    g.setGradientFill(capBorder);
    g.drawEllipse(toX - capRadius, toY - capRadius, capRadius * 2.0f, capRadius * 2.0f, 1.0f);
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

        // Draw playhead cursor line
        if (playbackProgress >= 0.0 && playbackProgress <= 1.0) {
            float playheadX = startX + (float)playbackProgress * w;

            // Draw a glowing vertical playhead bar (single accent: teal)
            juce::ColourGradient cg(KhensuTheme::accentTeal.withAlpha(0.20f), playheadX, 0.0f,
                                    juce::Colours::transparentBlack, playheadX + 15.0f, 0.0f, false);
            cg.addColour(0.5, KhensuTheme::accentTeal.withAlpha(0.4f));
            cg.addColour(0.8, KhensuTheme::accentTeal.withAlpha(0.20f));

            g.setGradientFill(cg);
            g.fillRect(playheadX - 1.5f, 0.0f, 3.0f, (float)bounds.getHeight());
        }
    }
}

namespace {

juce::Colour glassFill(GlassPanelVariant variant)
{
    return (variant == GlassPanelVariant::Raised) ? KhensuTheme::bgRaised.withAlpha(0.92f)
                                                  : KhensuTheme::bgSurface.withAlpha(0.62f);
}

/// Shared body of both drawGlassPanel() overloads; `drawBackdrop` paints the
/// blurred backdrop (clipped to the panel's rounded shape) or nothing.
/// `tintedCover` (local coordinates, may be empty) is the part of the panel
/// that `drawBackdrop` covers with an opaque backdrop that already includes
/// the glass fill: the fill is not drawn again there, and the shadow - which
/// that opaque backdrop hides completely - is not drawn under its interior.
template <typename DrawBackdrop>
void drawGlassPanelImpl(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                        juce::Rectangle<int> tintedCover, DrawBackdrop&& drawBackdrop)
{
    auto b = bounds.toFloat().reduced(0.5f);
    constexpr float radius = 6.0f;
    constexpr int inset = 1 + static_cast<int>(radius); // pixels fully inside the rounded shape
    tintedCover = tintedCover.getIntersection(bounds);

    // ── Shadow ──────────────────────────────────────────────────────
    // Depth comes from the shared shadowColour token (kimi audit) so every
    // raised surface casts a consistent shadow instead of ad-hoc alphas.
    {
        float shadowScale = (variant == GlassPanelVariant::Raised) ? 1.3f : 1.0f;
        float shadowOffset = (variant == GlassPanelVariant::Raised) ? 8.0f : 4.0f;
        auto shadow = b.translated(0.0f, shadowOffset).expanded(shadowOffset * 0.5f);
        juce::ColourGradient sg(KhensuTheme::shadowColour.withMultipliedAlpha(shadowScale), shadow.getCentreX(),
                                shadow.getY(), juce::Colours::transparentBlack, shadow.getCentreX(), shadow.getBottom(),
                                false);
        g.setGradientFill(sg);
        g.saveState();
        if (!tintedCover.isEmpty()) {
            // Only the shadow's rim outside the panel can show: skip the
            // interior, which the opaque backdrop paints over anyway.
            g.excludeClipRegion(bounds.reduced(inset, 1).getIntersection(tintedCover));
            g.excludeClipRegion(bounds.reduced(1, inset).getIntersection(tintedCover));
        }
        g.fillRoundedRectangle(shadow, radius + 2.0f);
        g.restoreState();
    }

    // GlowAccent's radial accent glow was retired (kimi audit P0 - no
    // per-panel colour halos). The variant is kept for source compatibility
    // but now renders identically to Default; a highlighted panel instead
    // gets a slightly stronger border below.

    // ── Fill ────────────────────────────────────────────────────────
    // The transparent tint lets whatever's behind the panel read through -
    // real-time backdrop blur (renderBlurredBackdrop(), cached by the caller)
    // when supplied, otherwise the app's background glow reads through sharp
    // as before. Either way, transparency + gradient sheen + a rim-light
    // border still do most of the work selling "glass".
    if (variant == GlassPanelVariant::Void) {
        g.setColour(KhensuTheme::bgBase);
        g.fillRoundedRectangle(b, radius);
    } else {
        drawBackdrop(b, radius);

        g.setColour(glassFill(variant));
        g.saveState();
        if (!tintedCover.isEmpty()) g.excludeClipRegion(tintedCover); // fill already in the backdrop
        g.fillRoundedRectangle(b, radius);
        g.restoreState();

        // Soft top-to-bottom sheen from the shared highlight token: light
        // catching the front face of glass, fading by mid-panel.
        juce::ColourGradient sheen(KhensuTheme::highlight, b.getCentreX(), b.getY(),
                                   KhensuTheme::highlight.withAlpha(0.0f), b.getCentreX(),
                                   b.getY() + b.getHeight() * 0.45f, false);
        g.setGradientFill(sheen);
        // The sheen is fully transparent below 45%: filling only the top part
        // gives the same pixels for under half the fill cost.
        g.saveState();
        g.reduceClipRegion(bounds.withHeight((int)std::ceil(b.getHeight() * 0.45f) + 2));
        g.fillRoundedRectangle(b, radius);
        g.restoreState();
    }

    // ── Border ──────────────────────────────────────────────────────
    // A gradient rim instead of a flat colour - lighter at the top-left
    // (where the implied light source hits) fading toward the bottom-right,
    // which reads as a refractive glass edge rather than a plain outline.
    if (variant != GlassPanelVariant::Void) {
        juce::Colour borderBase =
            (variant == GlassPanelVariant::Raised) ? KhensuTheme::borderActive : KhensuTheme::borderSubtle;
        juce::ColourGradient rim(borderBase.brighter(0.5f), b.getX(), b.getY(), borderBase.darker(0.2f), b.getRight(),
                                 b.getBottom(), false);
        g.setGradientFill(rim);
        g.drawRoundedRectangle(b, radius, 1.0f);
    }

    // ── Top-left inner highlight (glass specular) ───────────────────
    // The `highlight` token exists for exactly this edge; hardcoded white drew
    // an invisible sheen on the Light theme's white surfaces (kimi audit P1
    // #16). Doubled to keep the same visual weight the literal 10% white had.
    if (variant != GlassPanelVariant::Void) {
        g.setColour(KhensuTheme::highlight.withMultipliedAlpha(2.0f));
        g.drawLine(b.getX() + radius, b.getY() + 0.5f, b.getRight() - radius, b.getY() + 0.5f, 1.0f);
        g.drawLine(b.getX() + 0.5f, b.getY() + radius, b.getX() + 0.5f, b.getBottom() - radius, 1.0f);
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

} // namespace

void KhensuLookAndFeel::drawGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds, GlassPanelVariant variant,
                                       juce::Colour accentColour, const juce::Image& blurredBackdrop)
{
    // accentColour is retained in the signature for source compatibility but
    // no longer drives a per-panel glow (kimi audit P0).
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
    // The panel area the backdrop actually covers (all of it, unless the panel
    // extends past the app bounds), in local coordinates.
    const auto src = appBackdrop.isValid() ? backdropSource.getIntersection(appBackdrop.getBounds())
                                           : juce::Rectangle<int>();
    const auto cover = src.translated(bounds.getX() - backdropSource.getX(), bounds.getY() - backdropSource.getY());
    drawGlassPanelImpl(g, bounds, variant, cover, [&](juce::Rectangle<float> b, float radius) {
        if (src.isEmpty()) return;
        g.saveState();
        clipToPanel(g, b, radius);
        // Same-size source and destination: a plain translated copy, no
        // resampling (the stretched small image cost ~1.7 ms per card).
        g.drawImage(appBackdrop, cover.getX(), cover.getY(), src.getWidth(), src.getHeight(), src.getX(),
                    src.getY(), src.getWidth(), src.getHeight());
        g.restoreState();
    });
}

juce::Image KhensuLookAndFeel::glassBackdrop(juce::Rectangle<int> appBounds, GlassPanelVariant variant)
{
    if (variant == GlassPanelVariant::Void) return {};
    return blurredAppBackdrop(appBounds, glassFill(variant));
}

juce::Image KhensuLookAndFeel::blurredAppBackdrop(juce::Rectangle<int> appBounds, juce::Colour tint)
{
    if (appBounds.isEmpty()) return {};

    // Four entries: the main window plus one other top-level surface (a
    // dialog), each with up to two glass tints, can alternate without
    // rebuilding each other's backdrop every paint.
    struct Entry {
        juce::Rectangle<int> bounds;
        juce::uint64 palette = 0;
        juce::uint32 tint = 0;
        juce::Image image;
    };
    static Entry entries[4];
    static int nextSlot = 0;

    const auto palette = backdropPaletteKey();
    for (const auto& e : entries)
        if (e.image.isValid() && e.bounds == appBounds && e.palette == palette && e.tint == tint.getARGB())
            return e.image;

    // The same downsample + box blur a per-panel backdrop used, done once for
    // the whole app area, tinted (a constant blend, so tinting before the
    // bilinear upscale equals tinting after, to rounding), then scaled up
    // once to full size.
    auto small = renderBlurredBackdropSoftware(appBounds, appBounds);
    if (!tint.isTransparent()) {
        juce::Graphics g(small);
        g.fillAll(tint); // the same blend the per-paint fill did
    }
    auto native = juce::NativeImageType().convert(upscaleBilinear(small, appBounds.getWidth(), appBounds.getHeight()));
    entries[nextSlot] = {appBounds, palette, tint.getARGB(), native};
    nextSlot = (nextSlot + 1) % 4;
    return native;
}

void KhensuLookAndFeel::drawAppBackground(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    auto b = bounds.toFloat();
    const float w = b.getWidth(), h = b.getHeight();
    if (w <= 0.0f || h <= 0.0f) return;

    // Base wash: a lifted teal-black at the top-left settling into the window
    // void - the "radial-gradient(... #0b1416 0%, #050608 55%)" from the design.
    {
        juce::ColourGradient base(KhensuTheme::bgBase, w * 0.22f, -h * 0.10f, KhensuTheme::bgVoid, w * 0.55f, h * 0.60f,
                                  true);
        g.setGradientFill(base);
        g.fillRect(b);
    }

    // Aurora orbs: subtle and STATIC (no animation - the animated neon orbs were
    // retired in the kimi audit; these are low-alpha depth cues, not the old
    // glow). Teal top-left, magenta top-right, a fainter teal low-centre. In the
    // Light theme they're softer still so they don't muddy white surfaces.
    const bool light = KhensuTheme::currentTheme() == KhensuTheme::Theme::Light;
    const float orbAlpha = light ? 0.05f : 0.11f;
    const float span = juce::jmax(w, h);

    auto orb = [&g](juce::Colour c, float cx, float cy, float r, float a) {
        if (r <= 0.0f) return;
        juce::ColourGradient gg(c.withAlpha(a), cx, cy, c.withAlpha(0.0f), cx, cy + r, true);
        gg.addColour(0.55, c.withAlpha(a * 0.35f));
        g.setGradientFill(gg);
        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
    };

    orb(KhensuTheme::accentTeal, w * 0.02f, -h * 0.06f, span * 0.42f, orbAlpha);
    orb(KhensuTheme::accentMagenta, w * 0.99f, -h * 0.02f, span * 0.40f, orbAlpha * 0.9f);
    orb(KhensuTheme::accentTeal, w * 0.55f, h * 1.06f, span * 0.46f, orbAlpha * 0.7f);
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
