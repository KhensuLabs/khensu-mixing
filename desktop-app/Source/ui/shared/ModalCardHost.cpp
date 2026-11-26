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
    // Modal backdrop scrim routed through the theme token.
    g.fillAll(KhensuTheme::scrim);
    if (!drawCardBackground_) return;

    const auto cb = cardBounds();
    // LG-12: layered ambient + contact shadow for true floating glass depth.
    juce::DropShadow(KhensuTheme::shadowColour.withMultipliedAlpha(1.1f), 32, {0, 12}).drawForRectangle(g, cb);
    juce::DropShadow(KhensuTheme::shadowColour.withMultipliedAlpha(0.85f), 10, {0, 3}).drawForRectangle(g, cb);

    // Route modal card through Liquid Glass Raised surface + live smoke refraction.
    auto* root = getTopLevelComponent();
    if (root != nullptr && root != this) {
        KhensuLookAndFeel::drawGlassPanel(g, cb, GlassPanelVariant::Raised,
                                          KhensuLookAndFeel::glassBackdrop(root->getLocalBounds(), GlassPanelVariant::Raised),
                                          root->getLocalArea(this, cb));
    } else {
        KhensuLookAndFeel::drawGlassPanel(g, cb, GlassPanelVariant::Raised);
    }
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
