// desktop-app/Source/ui/shared/ModalCardHost.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

/// Shared base for centered-card modal overlays (dim backdrop + rounded
/// card + ESC/click-outside dismiss). Consolidates the boilerplate that
/// used to be hand-duplicated across ExplainerOverlay, ChainReasoningOverlay,
/// MetricDiffOverlay, WeightProfileOverlay, DashboardLayoutPanel, and
/// TargetOverrideEditor. Derived classes own their card component(s), add
/// them as children, and size them in their own resized() override; they
/// report the card's bounds via cardBounds() and their dismiss action via
/// dismiss(). Appearance knobs (backdropAlpha_, cardColour_, etc.) are
/// protected so a derived class can tweak them in its constructor before
/// the first paint().
class ModalCardHost : public juce::Component {
  public:
    ~ModalCardHost() override = default;

    void paint(juce::Graphics& g) final;
    void mouseDown(const juce::MouseEvent& event) final;
    bool keyPressed(const juce::KeyPress& key) final;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

  protected:
    virtual juce::Rectangle<int> cardBounds() const = 0;
    virtual void dismiss() = 0;

    /// Call at the end of the derived constructor, after card children are
    /// added, to match the original per-class construction order.
    void grabModalFocus();

    /// DEPRECATED: the modal backdrop now uses the theme-adaptive
    /// KhensuTheme::scrim token (not this per-instance black alpha), so all
    /// modals dim consistently. Retained for source compatibility only.
    float backdropAlpha_ = 0.6f;
    bool drawCardBackground_ = true;
    juce::Colour cardColour_; // set to KhensuTheme::bgSurface by default in the .cpp
    juce::Colour borderColour_{0xff333333};
    float borderThickness_ = 1.0f;
    float cornerRadius_ = 8.0f;

    ModalCardHost();

  private:
    void takePendingFocus();
    bool focusPending_ = false;
};

} // namespace khensu
