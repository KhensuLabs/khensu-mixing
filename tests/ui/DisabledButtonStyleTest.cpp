// UXR-3: a disabled button's plate is lighter than the enabled one's (it used
// to be set to a fixed 28% alpha, heavier than a 22% secondary plate).
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "ui/shared/KhensuLookAndFeel.h"
#include "ui/shared/KhensuTheme.h"

namespace {
/// The plate's alpha at the button's centre, drawn on a transparent image.
float plateAlphaAtCentre(khensu::KhensuLookAndFeel& lnf, juce::TextButton& button)
{
    juce::Image image(juce::Image::ARGB, button.getWidth(), button.getHeight(), true);
    {
        juce::Graphics g(image); // flushed when it goes out of scope
        lnf.drawButtonBackground(g, button, button.findColour(juce::TextButton::buttonColourId), false, false);
    }
    return image.getPixelAt(button.getWidth() / 2, button.getHeight() / 2).getFloatAlpha();
}
} // namespace

TEST_CASE("A disabled button is lighter than an enabled one (UXR-3)", "[lookandfeel][uxr3]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::KhensuLookAndFeel lnf;
    juce::TextButton button("Apply");
    button.setLookAndFeel(&lnf);
    button.setSize(120, 32);

    // A tinted secondary, as the Copilot's Apply / Audition / Pin are.
    button.setColour(juce::TextButton::buttonColourId, khensu::KhensuTheme::accentBlue.withAlpha(0.25f));
    const float enabled = plateAlphaAtCentre(lnf, button);
    button.setEnabled(false);
    const float disabled = plateAlphaAtCentre(lnf, button);
    CHECK(enabled > 0.0f);
    CHECK(disabled < enabled);

    // The default plate too.
    button.removeColour(juce::TextButton::buttonColourId);
    button.setEnabled(true);
    const float plainEnabled = plateAlphaAtCentre(lnf, button);
    button.setEnabled(false);
    CHECK(plateAlphaAtCentre(lnf, button) < plainEnabled);
    button.setLookAndFeel(nullptr);
}
