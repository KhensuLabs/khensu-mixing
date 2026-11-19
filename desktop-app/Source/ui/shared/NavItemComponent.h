// desktop-app/Source/ui/shared/NavItemComponent.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include "KhensuTheme.h"

namespace khensu {

class NavItemComponent final : public juce::Component {
  public:
    NavItemComponent();

    void setLabel(const juce::String& label);
    void setActive(bool active);
    bool isActive() const { return active_; }

    std::function<void()> onClick;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    /// Return/Space activate the focused item, so the sidebar is reachable
    /// without a mouse (kimi audit P0 #4).
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    void mouseEnter(const juce::MouseEvent&) override
    {
        hovered_ = true;
        repaint();
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        hovered_ = false;
        repaint();
    }

  private:
    juce::String label_;
    bool active_ = false;
    bool hovered_ = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NavItemComponent)
};

} // namespace khensu
