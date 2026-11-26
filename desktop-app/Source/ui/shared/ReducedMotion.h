// desktop-app/Source/ui/shared/ReducedMotion.h
// VIS-7: the reduced-motion setting. When it is on, nothing on screen moves
// on its own: ValueAnimator and motion::fade jump to the end value, the 3D
// orbit stops auto-rotating, the hero spectrum's morph and the spectrogram
// zoom are instant, and notices / toasts appear without sliding in.
//
// The default follows Windows' "Show animations in Windows" setting
// (SPI_GETCLIENTAREAANIMATION); the user's choice in Settings overrides it and
// is kept in settings.json as ui.reduced_motion ("on" / "off"; "" = follow
// Windows).
//
//   if (ui::motion::reduced()) jumpTo(target); else animateTo(target);
//
// THREAD_CONTEXT(Any) - reads are lock-free; setters persist on the caller's thread.
#pragma once
#include <JuceHeader.h>

namespace khensu::ui::motion {

/// True when animations should be skipped (the user's choice, else Windows').
bool reduced();

/// The user's explicit choice, saved to settings.json (ui.reduced_motion).
void setReduced(bool reducedMotion);

/// Drops the explicit choice and follows Windows again (saved as "").
void followSystem();

/// True while no explicit choice is stored.
bool followsSystem();

/// Windows' "Show animations" turned off (false where it cannot be read).
bool systemPrefersReduced();

/// Where settings.json lives (default: AppPaths::dataDirectory()). Tests point
/// it at a scratch folder; the stored choice is re-read from there.
void setConfigDirectory(const juce::File& configDir);

/// Session-only override that is never saved (tests, the screenshot pass).
/// std::nullopt-like: pass -1 to clear, 0 / 1 to force.
void setOverrideForTesting(int forced);

} // namespace khensu::ui::motion
