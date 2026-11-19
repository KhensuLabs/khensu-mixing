// desktop-app/Source/ui/shared/SegmentedControl.h
#pragma once
#include <JuceHeader.h>
#include <vector>
#include <functional>

namespace khensu {

class SegmentedControl final : public juce::Component {
  public:
    SegmentedControl();

    void addSegment(const juce::String& label);
    void setSelected(int index);
    int getSelected() const { return selected_; }

    std::function<void(int)> onChange;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    /// Left/Right (and Up/Down) step the selection, so the control is operable
    /// from the keyboard (kimi audit P0 #5).
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

  private:
    std::vector<juce::String> segments_;
    int selected_ = 0;

    int segmentIndexAt(int x) const;
    /// Moves the selection by `delta` segments, clamped, firing onChange only
    /// when the index actually moved.
    void selectRelative(int delta);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SegmentedControl)
};

} // namespace khensu
