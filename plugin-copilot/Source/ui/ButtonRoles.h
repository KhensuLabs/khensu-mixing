// plugin-copilot/Source/ui/ButtonRoles.h
// UX-4 in the plugin: every TextButton draws as an outlined secondary unless
// marked primary - one filled (accent) primary per card. Read by
// PluginEditorLookAndFeel::drawButtonBackground.
#pragma once
#include <JuceHeader.h>

namespace khensu::copilot::button_role {

inline const juce::Identifier kPrimary{"khensuPrimary"};

inline void markPrimary(juce::Button& button, bool primary = true)
{
    button.getProperties().set(kPrimary, primary);
    button.repaint();
}

inline bool isPrimary(const juce::Button& button)
{
    return static_cast<bool>(button.getProperties().getWithDefault(kPrimary, false));
}

} // namespace khensu::copilot::button_role
