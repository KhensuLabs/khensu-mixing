// desktop-app/Source/ui/SystemBrain/FeatureDetailPanel.cpp
#include "FeatureDetailPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {

// Wrapped-text block: measures with juce::TextLayout, and draws with the
// same layout when a Graphics is supplied. Returns the wrapped height.
int textBlock(juce::Graphics* g, const juce::String& text, const juce::Font& font, juce::Colour colour, int x, int y,
              int width, int minHeight = 0)
{
    if (text.isEmpty() || width <= 0) return minHeight;
    juce::AttributedString as;
    as.append(text, font, colour);
    juce::TextLayout tl;
    tl.createLayout(as, (float)width);
    const int h = juce::jmax(minHeight, (int)std::ceil(tl.getHeight()));
    if (g != nullptr) tl.draw(*g, juce::Rectangle<float>((float)x, (float)y, (float)width, (float)h));
    return h;
}

} // namespace

FeatureDetailPanel::FeatureDetailPanel()
{
    addChildComponent(sessionMeter_); // hidden until hasSessionValue_ is true
}

void FeatureDetailPanel::setEntry(const FeatureEntry& e)
{
    hasFeature_ = true;
    hasSessionValue_ = false; // no live session data from FeatureEntry alone

    featureName_ = juce::String(e.name);
    featureCategory_ = juce::String(e.category);
    description_ = juce::String(e.short_description);
    measurementMethod_ = juce::String(e.detailed_description);
    logicAndMath_ = juce::String(e.logic_and_math);
    whyItMatters_ = juce::String(e.why_it_exists);

    // Session fields cleared until populated by a live session
    sessionValue_ = {};
    sessionUnit_ = {};
    genreTarget_ = {};

    // Inside the host's viewport we own our height: re-measure for the new
    // entry so the scroll range matches the wrapped content.
    if (getWidth() > 0) setSize(getWidth(), preferredHeight(getWidth()));
    repaint();
    resized();
}

void FeatureDetailPanel::setSessionValue(const juce::String& value, const juce::String& unit,
                                         const juce::String& genreTarget, float numeric, float rangeMin, float rangeMax,
                                         float zoneLo, float zoneHi)
{
    hasSessionValue_ = true;
    sessionHasMeter_ = true;
    sessionValue_ = value;
    sessionUnit_ = unit;
    genreTarget_ = genreTarget;
    sessionMeter_.setRange(rangeMin, rangeMax);
    sessionMeter_.setTargetZone(zoneLo, zoneHi);
    sessionMeter_.setValue(numeric);
    if (getWidth() > 0) setSize(getWidth(), preferredHeight(getWidth()));
    repaint();
    resized();
}

void FeatureDetailPanel::setSessionNote(const juce::String& value, const juce::String& note)
{
    hasSessionValue_ = true;
    sessionHasMeter_ = false;
    sessionValue_ = value;
    sessionUnit_ = {};
    genreTarget_ = note;
    if (getWidth() > 0) setSize(getWidth(), preferredHeight(getWidth()));
    repaint();
    resized();
}

void FeatureDetailPanel::clear()
{
    hasFeature_ = false;
    hasSessionValue_ = false;
    featureName_ = {};
    featureCategory_ = {};
    description_ = {};
    measurementMethod_ = {};
    sessionValue_ = {};
    sessionUnit_ = {};
    genreTarget_ = {};
    whyItMatters_ = {};
    repaint();
    resized();
}

int FeatureDetailPanel::walkSections(juce::Graphics* g, int width)
{
    const int pad = KhensuTheme::panelPadding;
    const int x = pad;
    const int w = juce::jmax(0, width - pad * 2);
    int y = pad;

    auto divider = [&](void) {
        if (g != nullptr) {
            g->setColour(KhensuTheme::borderSubtle);
            g->fillRect(x, y, w, 1);
        }
        y += 1 + KhensuTheme::sp3;
    };
    auto header = [&](const char* text) {
        if (g != nullptr) {
            g->setFont(KhensuTheme::sectionHeaderFont());
            g->setColour(KhensuTheme::textTertiary);
            g->drawText(text, x, y, w, 16, juce::Justification::centredLeft, false);
        }
        y += 16 + 4;
    };

    // ── Section 1: Feature name + category badge ──────────────────────
    if (g != nullptr) {
        g->setFont(KhensuTheme::labelFont(20.0f));
        g->setColour(KhensuTheme::textPrimary);
        g->drawText(featureName_, x, y, w, 28, juce::Justification::centredLeft, true);
    }
    y += 28;
    if (g != nullptr) {
        juce::Rectangle<int> badge(x, y + 2, 100, 20);
        g->setColour(KhensuTheme::bgOverlay);
        g->fillRoundedRectangle(badge.toFloat(), 4.0f);
        g->setFont(KhensuTheme::labelFont(10.0f));
        g->setColour(KhensuTheme::textTertiary);
        g->drawText(featureCategory_.toUpperCase(), badge, juce::Justification::centred, false);
    }
    y += 24 + KhensuTheme::sp3;
    divider();

    // ── Section 2: Description ────────────────────────────────────────
    y += textBlock(g, description_, KhensuTheme::bodyFont(13.0f), KhensuTheme::textSecondary, x, y, w) +
         KhensuTheme::sp3;

    // ── Section 3: HOW KHENSU MEASURES IT ────────────────────────────
    header("HOW KHENSU MEASURES IT");
    y += textBlock(g, measurementMethod_, KhensuTheme::bodyFont(12.0f), KhensuTheme::textSecondary, x, y, w) +
         KhensuTheme::sp3;

    // ── Section 3b: THE LOGIC & MATH (thresholds/formulas) ───────────
    if (logicAndMath_.isNotEmpty()) {
        header("THE LOGIC & MATH");
        // Monospace so formulas/thresholds line up and read as "the actual
        // rule", not prose.
        y += textBlock(g, logicAndMath_, KhensuTheme::monoFont(10.5f), KhensuTheme::textSecondary, x, y, w) +
             KhensuTheme::sp3;
    }

    // ── Section 4: YOUR SESSION VALUE (when session active) ───────────
    if (hasSessionValue_) {
        divider();
        header("YOUR SESSION VALUE");
        if (g != nullptr) {
            g->setFont(KhensuTheme::metricFont(24.0f));
            g->setColour(KhensuTheme::textPrimary);
            g->drawText(sessionValue_ + " " + sessionUnit_, x, y, w, 32, juce::Justification::centredLeft, false);
        }
        y += 32;
        if (g != nullptr) {
            g->setFont(KhensuTheme::labelFont(11.0f));
            g->setColour(KhensuTheme::textTertiary);
            g->drawText(sessionHasMeter_ ? "Genre target: " + genreTarget_ : genreTarget_, x, y, w, 16,
                        juce::Justification::centredLeft, false);
        }
        y += 16 + KhensuTheme::sp2;
        if (sessionHasMeter_) {
            meterY_ = y; // resized() places the meter component here
            y += 40 + KhensuTheme::sp3;
        }
    }

    // ── Section 5: WHY IT MATTERS ─────────────────────────────────────
    divider();
    header("WHY IT MATTERS");

    juce::String mainWhy = whyItMatters_;
    juce::String misconception;
    const int startIdx = whyItMatters_.indexOf("[Common Misconception:");
    if (startIdx >= 0) {
        const int endIdx = whyItMatters_.indexOf(startIdx, "]");
        if (endIdx > startIdx) {
            misconception = whyItMatters_.substring(startIdx + 22, endIdx).trim();
            mainWhy = whyItMatters_.substring(0, startIdx).trim();
        }
    }

    y += textBlock(g, mainWhy, KhensuTheme::bodyFont(12.0f), KhensuTheme::textSecondary, x, y, w) + 10;

    if (misconception.isNotEmpty()) {
        // bodyFont(), not FontOptions{"Sans-Serif", ...}: that placeholder token
        // resolves on some Windows setups to a byte-mode fallback that renders
        // UTF-8 as per-byte Latin-1 mojibake - the exact hazard KhensuTheme's
        // uiTypefaceName() comment warns about. The hardcoded #e74c3c also
        // ignored the theme, so this callout stayed the same red in all three
        // themes (kimi audit P0 #14).
        const juce::Font miscFont = KhensuTheme::bodyFont(9.5f);
        const int innerW = w - 16;
        const int textH = textBlock(nullptr, misconception, miscFont, KhensuTheme::textSecondary, 0, 0, innerW);
        const int boxH = 14 + 4 + textH + 12;
        if (g != nullptr) {
            juce::Rectangle<int> box(x, y, w, boxH);
            g->setColour(KhensuTheme::accentRed.withAlpha(0.08f));
            g->fillRoundedRectangle(box.toFloat(), 4.0f);
            g->setColour(KhensuTheme::accentRed.withAlpha(0.25f));
            g->drawRoundedRectangle(box.toFloat(), 4.0f, 1.0f);

            auto inside = box.reduced(8, 6);
            g->setFont(khensu::KhensuTheme::uiFont(9.5f, juce::Font::bold));
            g->setColour(KhensuTheme::accentRed);
            g->drawText("COMMON MISCONCEPTION DEBUNKED:", inside.removeFromTop(14), juce::Justification::topLeft, true);
            inside.removeFromTop(4);
            textBlock(g, misconception, miscFont, KhensuTheme::textSecondary, inside.getX(), inside.getY(),
                      inside.getWidth());
        }
        y += boxH;
    }

    return y + pad;
}

int FeatureDetailPanel::preferredHeight(int width)
{
    if (!hasFeature_) return 200;
    return walkSections(nullptr, width);
}

void FeatureDetailPanel::paint(juce::Graphics& g)
{
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default);

    if (!hasFeature_) {
        g.setFont(KhensuTheme::bodyFont(13.0f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("Select a feature to learn how Khensu analyses your mix",
                   getLocalBounds().reduced(KhensuTheme::panelPadding), juce::Justification::centred, true);
        return;
    }

    walkSections(&g, getWidth());
}

void FeatureDetailPanel::resized()
{
    sessionMeter_.setVisible(hasSessionValue_ && sessionHasMeter_);
    if (hasSessionValue_ && sessionHasMeter_) {
        // Measure pass records the meter's Y at the current width.
        walkSections(nullptr, getWidth());
        sessionMeter_.setBounds(KhensuTheme::panelPadding, meterY_, getWidth() - KhensuTheme::panelPadding * 2, 40);
    }
}

} // namespace khensu
