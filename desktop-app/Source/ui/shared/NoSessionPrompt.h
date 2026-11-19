// desktop-app/Source/ui/shared/NoSessionPrompt.h
// The empty state of a screen that needs a session (UX-5): one sentence and
// a "Load a track..." button that opens the file browser, instead of a
// sentence pointing at another screen.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "ButtonRole.h"
#include "KhensuTheme.h"
#include "../../services/IShell.h"

namespace khensu::no_session {

inline juce::String message()
{
    return TRANS("No track loaded yet.");
}

/// Sets up the screen's "Load a track..." button (primary: it is the only
/// thing to do on an empty screen). Add it with addChildComponent and show it
/// with the screen's empty-state label.
inline void configureLoadButton(juce::TextButton& button, IShell& shell)
{
    button.setButtonText(TRANS("Load a track..."));
    button.setTooltip(TRANS("Open an audio file to start a session (Ctrl+O)"));
    button_role::markPrimary(button);
    button.onClick = [&shell] { shell.browseForAudioFile(); };
}

/// Centred just below the empty-state sentence drawn in `labelArea`.
inline juce::Rectangle<int> loadButtonBounds(juce::Rectangle<int> labelArea)
{
    return labelArea.withSizeKeepingCentre(180, KhensuTheme::buttonH).translated(0, 40);
}

} // namespace khensu::no_session
