#pragma once
// UI-5: the one knob (and matching linear slider) for the plugin and the
// desktop. A juce::Slider, so juce::SliderParameterAttachment /
// APVTS::SliderAttachment work unchanged (host gestures included).
//
//   KhensuKnob knob("Amount");                     // rotary, label under it
//   knob.setRange(0.0, 1.0); knob.setDefaultValue(1.0);
//   KhensuKnob fader("Gain", KhensuKnob::Style::LinearHorizontal);
//   knob.setBipolar(true);                         // arc grows from the centre
//
//   knob.setUnit(KhensuKnob::Unit::Hz);              // "80 Hz", "1.2 kHz"; parses "1.2k"
//
// Behaviour (UX2-6a): drag up/right to raise (Shift-drag = fine, 1/8 the
// speed, and switching Shift mid-drag never jumps), the scroll wheel steps it
// (Shift = fine), double-click - or Enter while focused - opens an inline
// editor to type a value (Return / click away commits, Escape cancels), and
// Alt/Option-click resets to the default. The value is drawn under the dial
// as plain tabular text with its unit (no box), the label under that; a
// popup bubble only appears when the knob is too small to draw its value.
// Every edit is one host gesture. Drawn by KhensuKnobLookAndFeel (shared,
// vector-only): a neutral body, the value arc in the accent.
// THREAD_CONTEXT(UIThread)

#include <juce_gui_basics/juce_gui_basics.h>

namespace khensu::ui {

class KhensuKnobLookAndFeel final : public juce::LookAndFeel_V4 {
  public:
    KhensuKnobLookAndFeel();
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle,
                          float endAngle, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getSliderPopupFont(juce::Slider&) override;
    int getSliderPopupPlacement(juce::Slider&) override;
    void drawBubble(juce::Graphics&, juce::BubbleComponent&, const juce::Point<float>& tip,
                    const juce::Rectangle<float>& body) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;
};

class KhensuKnob : public juce::Slider {
  public:
    enum class Style { Rotary, LinearHorizontal, LinearVertical };
    /// What the value means, for the compact readout and for parsing typed
    /// text. Percent is a 0..1 fraction shown as 0..100 %.
    enum class Unit { None, Hz, Db, Ms, Ratio, Percent, Q };

    /// Compact readout: "80 Hz", "1.2 kHz", "-18.0 dB", "+3.0 dB", "5.0 ms",
    /// "4.0:1", "70 %", "1.41".
    static juce::String formatValue(double value, Unit unit);
    /// Reads typed text in that unit ("1.2k", "1.2 kHz", "-3", "70%", "4:1",
    /// "1.5 s"); `fallback` when there is no number in it.
    static double parseValue(const juce::String& text, Unit unit, double fallback);
    /// The interaction help every knob's tooltip ends with.
    static juce::String usageHint();

    /// Where the dial, the value and the label go inside `area` (the value
    /// and label rows are empty when there is no room for them).
    struct Layout {
        juce::Rectangle<float> dial, value, label;
    };
    static Layout layoutFor(juce::Rectangle<float> area, bool hasLabel);

    explicit KhensuKnob(const juce::String& label = {}, Style style = Style::Rotary);
    ~KhensuKnob() override;

    /// Name drawn under the knob (and its accessible title when none is set).
    void setLabel(const juce::String& label);
    const juce::String& getLabel() const noexcept { return label_; }
    /// The value Alt/Option-click returns to.
    void setDefaultValue(double value);
    /// Sets the readout / typed-text conversion (call after a
    /// SliderParameterAttachment, which installs the parameter's own).
    void setUnit(Unit unit);
    Unit getUnit() const noexcept { return unit_; }
    /// The inline value editor (double-click / Enter).
    void showValueEditor();
    /// Closes it; `commit` sets the typed value as one gesture.
    void hideValueEditor(bool commit);
    bool isValueEditorShowing() const noexcept { return valueEditor_ != nullptr && valueEditor_->isVisible(); }
    juce::TextEditor* valueEditorForTest() noexcept { return valueEditor_.get(); }
    /// Arc drawn from the centre of the range (gain, pan) instead of the start.
    void setBipolar(bool bipolar);
    bool isBipolar() const noexcept { return bipolar_; }
    /// How many times slower a Shift-drag moves (default 8).
    void setFineFactor(double factor) { fineFactor_ = juce::jmax(1.0, factor); }
    /// Pixels of vertical travel for the full range in a normal drag.
    void setDragPixels(double pixels) { dragPixels_ = juce::jmax(20.0, pixels); }

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void parentHierarchyChanged() override;
    void resized() override;

    /// Test seam: a drag of (dx, dy) pixels from the current value, with or
    /// without Shift; returns the new value.
    double simulateDragForTest(float dx, float dy, bool fine);

  private:
    void applyDragDelta(float dx, float dy, bool fine);
    void updatePopupMode();
    juce::Rectangle<int> valueEditorBounds() const;
    juce::SharedResourcePointer<KhensuKnobLookAndFeel> lookAndFeel_;
    juce::String label_;
    bool bipolar_ = false;
    double fineFactor_ = 8.0;
    double dragPixels_ = 220.0;
    juce::Point<float> lastDragPos_;
    double dragProportion_ = 0.0;
    bool customDrag_ = false;
    bool resetClick_ = false; ///< this press was an Alt-click reset: no drag follows
    Unit unit_ = Unit::None;
    std::unique_ptr<juce::TextEditor> valueEditor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuKnob)
};

} // namespace khensu::ui
