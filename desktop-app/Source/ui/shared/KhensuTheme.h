// desktop-app/Source/ui/shared/KhensuTheme.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include "BadgeComponent.h" // for BadgeStatus enum

namespace khensu {

enum class GlassPanelVariant {
    Default = 0,   // bgSurface 85%, borderSubtle
    Raised = 1,    // bgRaised, borderActive, stronger shadow
    Void = 2,      // bgBase solid, no border - sidebar, transport
    GlowAccent = 3 // Default + radial accent glow behind content
};

struct KhensuTheme {
    // ── Background layers ────────────────────────────────────────────
    // Roles only - the literal values are per-theme and live in setTheme().
    // (Don't restate hex here: three themes reassign these, so any single
    // value copied into a comment is wrong for the other two.)
    static juce::Colour bgVoid;    // window base
    static juce::Colour bgBase;    // sidebar
    static juce::Colour bgSurface; // panel bodies
    static juce::Colour bgRaised;  // elevated cards
    static juce::Colour bgOverlay; // chips, dialogs

    // ── Borders ──────────────────────────────────────────────────────
    static juce::Colour borderSubtle; // hairline panel edge
    static juce::Colour borderActive; // focused/hovered panel edge

    // ── Accents ──────────────────────────────────────────────────────
    static juce::Colour accentTeal;
    static juce::Colour accentTealDim; // 0x3300d4aa - 20% alpha
    static juce::Colour accentAmber;
    static juce::Colour accentAmberDim;
    static juce::Colour accentRed;
    static juce::Colour accentRedDim;
    // DEPRECATED (kimi audit 2026-07-11): purple is retired as a primary UI
    // accent. Keep the token for source compatibility with third-party plugins
    // and saved user content, but do not use it for new UI chrome. Existing
    // panel call sites are being migrated to teal/blue/neutral.
    static juce::Colour accentPurple;
    static juce::Colour accentPurpleDim;
    // Aurora Glass co-accent (2026-07-16): magenta is the second brand accent,
    // paired with teal for the aurora background glow and gradient chrome. It is
    // DECORATIVE ONLY - it must never carry severity meaning (the red/amber/blue/
    // green severity contract in colourForStatus() is unchanged). Use it for the
    // aurora orbs, active-nav glow, and teal->magenta gradients, not for data.
    static juce::Colour accentMagenta;
    static juce::Colour accentMagentaDim;
    // Mix-note severity scheme (spec §4): blue = minor, green = strength.
    // (Red = severe and amber = moderate reuse the accents above.)
    static juce::Colour accentBlue;
    static juce::Colour accentBlueDim;
    static juce::Colour accentGreen;
    static juce::Colour accentGreenDim;

    // ── Glassmorphism token contract (kimi audit P0) ─────────────────
    // Named tokens so elevation/overlay/focus are consistent instead of
    // ad-hoc per call site.
    static juce::Colour scrim;        ///< dim behind modals/overlays
    static juce::Colour shadowColour; ///< drop-shadow under raised surfaces
    static juce::Colour highlight;    ///< top-edge sheen on glass panels
    static juce::Colour focusRing;    ///< keyboard-focus ring (accessibility)

    // ── Text ─────────────────────────────────────────────────────────
    // Roles only, as above - values are per-theme, set in setTheme().
    static juce::Colour textPrimary;   // body / headings
    static juce::Colour textSecondary; // supporting copy
    static juce::Colour textTertiary;  // captions, disabled
    static juce::Colour textAccent;    // links, active values

    // ── Data visualisation (VIZ-F2) ──────────────────────────────────
    // One job per colour: categorical = identity (fixed slot order, never
    // cycled), sequential = magnitude, diverging = polarity. The values come
    // from khensu::ui::viz (shared-core/include/khensu/ui/VizPalette.h), the
    // steps for the current theme - colour-blind checked in VizPaletteTest.
    // Severity keeps accentRed / Amber / Blue / Green; never use a viz colour
    // for severity or a severity colour for a series.
    static juce::Colour vizCategory(int slot);   ///< slot 0..7
    static juce::Colour vizSequential(float t);  ///< t 0..1 (magnitude)
    static juce::Colour vizDiverging(float t);   ///< t -1..1 (polarity)
    static juce::Colour vizSpreadOuter;  ///< released tracks, p10-p90 band
    static juce::Colour vizSpreadInner;  ///< released tracks, p25-p75 band
    static juce::Colour vizMedian;       ///< released tracks' median line
    static juce::Colour vizTrack;        ///< "this track" (the measured line / dot)
    static juce::Colour vizReference;    ///< a reference track (categorical slot 6, violet)
    static juce::Colour vizGrid;         ///< chart gridline
    static juce::Colour vizAxis;         ///< chart baseline / axis

    // ── Spacing (px) ─────────────────────────────────────────────────
    static constexpr int sp1 = 4;
    static constexpr int sp2 = 8;
    static constexpr int sp3 = 12;
    static constexpr int sp4 = 16;
    static constexpr int sp5 = 24;
    static constexpr int sp6 = 32;
    static constexpr int sp7 = 48;
    static constexpr int panelPadding = 24;
    static constexpr int cardGap = 16;
    static constexpr int sectionGap = 32;
    static constexpr int navItemH = 44;
    static constexpr int sidebarW = 200;
    static constexpr int transportH = 56;
    static constexpr int headerH = 40;
    // UX-4: the three button heights. Button text has a 12 px floor
    // (KhensuLookAndFeel::getTextButtonFont), so nothing shorter than
    // buttonHSmall should be laid out.
    static constexpr int buttonHSmall = 28;
    static constexpr int buttonH = 32;
    static constexpr int buttonHLarge = 40;

    // ── Typography ────────────────────────────────────────────────────
    // Single deliberate UI typeface for the whole app: the platform's real UI
    // sans (Segoe UI / San Francisco / system), NOT the generic "Sans-Serif"
    // string (which resolves to an arbitrary default). Every font in the app
    // routes through uiFont()/monoFont() or the named helpers below.
    static const juce::String& uiTypefaceName();
    /// UX-16: the smallest text any font call returns (body text minimum).
    static constexpr float kMinTextPx = 11.0f;
    static juce::Font uiFont(float size, int style = juce::Font::plain);

    // Named type scale (pt sizes) - use these instead of ad-hoc literals so
    // the hierarchy stays consistent. UI-4: four steps - 28 metric, 17 title,
    // 13 body, 11 label - so a measured value is the largest text on its card.
    static constexpr float kFontMetric = 28.0f; // hero numeric readouts
    static constexpr float kFontH1 = 17.0f;     // screen / card titles
    static constexpr float kFontH2 = 13.0f;     // sub-headers (body size, semibold)
    static constexpr float kFontBody = 13.0f;   // body copy
    static constexpr float kFontLabel = 11.0f;  // labels / axes
    static constexpr float kFontCaption = 11.0f; // captions (the 11 px floor)

    static juce::Font bodyFont(float size = kFontBody);
    static juce::Font labelFont(float size = 13.0f);
    static juce::Font metricFont(float size = kFontMetric); // tabular figures
    /// Every number on screen (UI-4): a face whose digits are all one width,
    /// so a changing value never jitters and columns of figures line up.
    static juce::Font numericFont(float size = kFontBody, int style = juce::Font::plain);
    static juce::Font monoFont(float size = kFontLabel);
    static juce::Font sectionHeaderFont(); // kFontLabel; sentence/Title case (ALL CAPS retired)

    // ── Status helpers ────────────────────────────────────────────────
    static juce::Colour colourForStatus(BadgeStatus s);
    static juce::Colour dimColourForStatus(BadgeStatus s);

    // ── Runtime theming ───────────────────────────────────────────────
    // The palette above is intentionally mutable: setTheme() reassigns every
    // colour (and KhensuLookAndFeel's parallel palette) in one place, so the
    // whole UI re-tints on the next repaint without touching call sites that
    // read these statics live in paint().
    //
    // Surfaces that CACHE a palette colour instead of reading it live - a
    // Label's setColour(), a JUCE colour ID installed on a LookAndFeel - would
    // otherwise keep the old theme's colour until the component is rebuilt.
    // Those call sites register an onThemeChanged handler (below) and re-apply
    // their colours from there, so a theme switch is complete rather than
    // partial. Prefer reading the statics live in paint() where you can; use a
    // listener only when the colour genuinely has to be cached.
    enum class Theme { Dark, HighContrast, Light };
    static void setTheme(Theme t);
    static Theme currentTheme() noexcept;
    /// Dark -> HighContrast -> Light -> Dark. Returns the newly-applied theme.
    static Theme cycleTheme();
    static const char* themeName(Theme t) noexcept;

    /// Registers `handler`, called after setTheme() has reassigned the whole
    /// palette. Returns a token to pass to removeThemeListener(); callers MUST
    /// deregister in their destructor or the handler will outlive them.
    /// Handlers run on the calling (UI) thread, in registration order.
    static int addThemeListener(std::function<void()> handler);
    static void removeThemeListener(int token);
};

/// RAII registration for a theme-change handler.
///
/// Declare one as the LAST member of a component that caches palette colours:
///
/// ```cpp
///   private:
///     void applyThemeColours();   // re-applies every cached setColour()
///     ThemeListener themeListener_{[this] { applyThemeColours(); repaint(); }};
/// ```
///
/// Member order matters: the handler captures `this` and touches the other
/// members, so it must be declared after everything it uses - it is then
/// destroyed (deregistered) first. Using this instead of a raw
/// addThemeListener/removeThemeListener pair makes the deregistration
/// impossible to forget, which would otherwise leave a handler holding a
/// dangling `this`.
class ThemeListener final {
  public:
    explicit ThemeListener(std::function<void()> handler)
        : token_(KhensuTheme::addThemeListener(std::move(handler)))
    {
    }
    ~ThemeListener() { KhensuTheme::removeThemeListener(token_); }

    // Neither copyable nor movable: the registry holds a handler bound to one
    // specific object, so duplicating or relocating the token would either
    // double-deregister or strand the handler.
    ThemeListener(const ThemeListener&) = delete;
    ThemeListener& operator=(const ThemeListener&) = delete;
    ThemeListener(ThemeListener&&) = delete;
    ThemeListener& operator=(ThemeListener&&) = delete;

  private:
    int token_ = 0;
};

} // namespace khensu
