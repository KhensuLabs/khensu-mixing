// desktop-app/Source/ui/shared/ButtonRole.h
// Button hierarchy (UX-4): KhensuLookAndFeel draws every TextButton as a
// light, outlined secondary unless it is marked primary - one filled
// primary per card, the action the card exists for.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>

namespace khensu::button_role {

inline const juce::Identifier kPrimary{"khensu.primaryButton"};

inline void markPrimary(juce::Button& button, bool primary = true)
{
    button.getProperties().set(kPrimary, primary);
    button.repaint();
}

inline bool isPrimary(const juce::Button& button)
{
    return (bool)button.getProperties().getWithDefault(kPrimary, false);
}

} // namespace khensu::button_role
