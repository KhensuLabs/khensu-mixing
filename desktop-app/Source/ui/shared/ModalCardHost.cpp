// desktop-app/Source/ui/shared/ModalCardHost.cpp
#include "ModalCardHost.h"
#include "KhensuLookAndFeel.h"

namespace khensu {

ModalCardHost::ModalCardHost() : cardColour_(KhensuTheme::bgSurface) {}

void ModalCardHost::grabModalFocus()
{
    setWantsKeyboardFocus(true);
    // Every overlay calls this from its constructor, before it is added to a
    // parent - grabKeyboardFocus() then silently did nothing (a jassert in
    // Debug), so ESC and Tab did not reach the modal until the user clicked
    // it. Take focus now if already on screen, otherwise once it is shown.
    focusPending_ = true;
    takePendingFocus();
}

void ModalCardHost::takePendingFocus()
{
    if (!focusPending_ || !isShowing()) return;
    focusPending_ = false;
    grabKeyboardFocus();
}

void ModalCardHost::parentHierarchyChanged() { takePendingFocus(); }

void ModalCardHost::visibilityChanged() { takePendingFocus(); }

void ModalCardHost::paint(juce::Graphics& g)
{
    // Modal backdrop scrim routed through the theme token (per-theme alpha -
    // opaque in high-contrast, subtle in light) instead of a hardcoded black
    // alpha, so every modal dims consistently (glassmorphism token contract).
    g.fillAll(KhensuTheme::scrim);
    if (!drawCardBackground_) return;

    auto bounds = cardBounds().toFloat();

    // Themed drop shadow under the raised card (KhensuTheme::shadowColour).
    juce::DropShadow(KhensuTheme::shadowColour, 24, {0, 6}).drawForRectangle(g, cardBounds());

    g.setColour(cardColour_);
    g.fillRoundedRectangle(bounds, cornerRadius_);
    g.setColour(borderColour_);
    g.drawRoundedRectangle(bounds, cornerRadius_, borderThickness_);
}

void ModalCardHost::mouseDown(const juce::MouseEvent& event)
{
    if (!cardBounds().contains(event.getPosition())) dismiss();
}

bool ModalCardHost::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) {
        dismiss();
        return true;
    }
    return false;
}

} // namespace khensu
