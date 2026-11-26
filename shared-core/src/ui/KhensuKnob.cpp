#include <khensu/ui/KhensuKnob.h>
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>

#include <cmath>

namespace khensu::ui {

namespace {
constexpr float kLabelRow = 14.0f;
constexpr float kValueRow = 16.0f;

const KhensuKnob* asKnob(const juce::Slider& s) { return dynamic_cast<const KhensuKnob*>(&s); }

/// `value` with `decimals` places, trailing zeros dropped down to `keep`.
juce::String trimmed(double value, int decimals, int keep)
{
    auto text = juce::String(value, decimals);
    if (!text.containsChar('.')) return text;
    const int dot = text.indexOfChar('.');
    while (text.length() > dot + 1 + keep && text.endsWithChar('0')) text = text.dropLastCharacters(1);
    if (text.endsWithChar('.')) text = text.dropLastCharacters(1);
    return text;
}

/// Avoids "-0.0": anything that rounds to zero at `decimals` places is 0.
double noNegativeZero(double value, int decimals)
{
    return std::abs(value) < 0.5 * std::pow(10.0, -decimals) ? 0.0 : value;
}
} // namespace

juce::String KhensuKnob::formatValue(double v, Unit unit)
{
    if (!std::isfinite(v)) return "-";
    switch (unit) {
    case Unit::Hz:
        if (std::abs(v) >= 10000.0) return trimmed(v / 1000.0, 1, 0) + " kHz";
        if (std::abs(v) >= 1000.0) return trimmed(v / 1000.0, 2, 0) + " kHz";
        if (std::abs(v) >= 100.0) return juce::String(juce::roundToInt(v)) + " Hz";
        return trimmed(v, 1, 0) + " Hz";
    case Unit::Db: {
        const double d = noNegativeZero(v, 1);
        return (d > 0.0 ? "+" : "") + juce::String(d, 1) + " dB";
    }
    case Unit::Ms:
        if (std::abs(v) >= 1000.0) return juce::String(v / 1000.0, 2) + " s";
        if (std::abs(v) >= 10.0) return juce::String(juce::roundToInt(v)) + " ms";
        return juce::String(noNegativeZero(v, 1), 1) + " ms";
    case Unit::Ratio: return juce::String(v, 1) + ":1";
    case Unit::Percent: return juce::String(juce::roundToInt(v * 100.0)) + " %";
    case Unit::Q: return juce::String(v, 2);
    case Unit::None:
    default: {
        const double a = std::abs(v);
        return trimmed(noNegativeZero(v, 2), a >= 100.0 ? 0 : (a >= 10.0 ? 1 : 2), 0);
    }
    }
}

double KhensuKnob::parseValue(const juce::String& rawText, Unit unit, double fallback)
{
    const auto text = rawText.trim().toLowerCase();
    // The number at the start ("-3.5", "+2", ".5").
    const auto numberPart = text.initialSectionContainingOnly("+-0123456789.,").replaceCharacter(',', '.');
    if (numberPart.isEmpty() || !numberPart.containsAnyOf("0123456789")) return fallback;
    double value = numberPart.getDoubleValue();
    const auto rest = text.substring(numberPart.length()).trim();
    switch (unit) {
    case Unit::Hz:
        if (rest.startsWithChar('k')) value *= 1000.0; // "1.2k", "1.2 kHz"
        break;
    case Unit::Ms:
        if (rest == "s" || rest.startsWith("sec")) value *= 1000.0; // "1.5 s"
        break;
    case Unit::Percent: value /= 100.0; break; // typed as 0..100 %
    case Unit::Db:
    case Unit::Ratio:
    case Unit::Q:
    case Unit::None:
    default: break;
    }
    return std::isfinite(value) ? value : fallback;
}

juce::String KhensuKnob::usageHint()
{
    return TRANS("Drag (Shift for fine) or scroll to change; double-click or Enter to type a value; "
                 "Alt/Option-click to reset.");
}

KhensuKnob::Layout KhensuKnob::layoutFor(juce::Rectangle<float> area, bool hasLabel)
{
    // Value under the dial, the label under that - when there is room.
    // The dial keeps at least kMinDial px; the label goes first when short.
    constexpr float kMinDial = 24.0f;
    Layout layout;
    const float h = area.getHeight();
    if (h >= kMinDial + kValueRow) {
        if (hasLabel && h >= kMinDial + 6.0f + kValueRow + kLabelRow) layout.label = area.removeFromBottom(kLabelRow);
        layout.value = area.removeFromBottom(kValueRow);
    }
    layout.dial = area;
    return layout;
}

//==============================================================================
KhensuKnobLookAndFeel::KhensuKnobLookAndFeel() { applyPalette(); }

void KhensuKnobLookAndFeel::applyPalette()
{
    setColour(juce::Slider::thumbColourId, Palette::textPrimary);
    setColour(juce::Slider::rotarySliderFillColourId, Palette::accentTeal);
    setColour(juce::Slider::rotarySliderOutlineColourId, studio::track());
    setColour(juce::Slider::trackColourId, Palette::accentTeal);
    setColour(juce::Slider::backgroundColourId, studio::track());
    setColour(juce::BubbleComponent::backgroundColourId, studio::surfaceRaised());
    setColour(juce::BubbleComponent::outlineColourId, studio::hairline());
    setColour(juce::TooltipWindow::textColourId, Palette::textPrimary);
}

void KhensuKnobLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float startAngle, float endAngle, juce::Slider& slider)
{
    const auto* knob = asKnob(slider);
    const juce::String label = knob != nullptr ? knob->getLabel() : juce::String();
    const auto rows =
        KhensuKnob::layoutFor(juce::Rectangle<int>(x, y, width, height).toFloat(), label.isNotEmpty());
    auto area = rows.dial;
    const auto valueRow = rows.value;
    const auto labelRow = rows.label;

    // With a readout, the dial sits right above it (spare height goes on
    // top) and stays a sensible size, so dial / value / name read as one.
    const float maxDial = valueRow.isEmpty() ? 1.0e6f : 84.0f;
    const float diameter = juce::jmin(juce::jmin(area.getWidth(), area.getHeight()) - 4.0f, maxDial);
    if (!valueRow.isEmpty()) area = area.removeFromBottom(diameter + 4.0f);
    if (diameter < 8.0f) return;
    const auto dial = area.withSizeKeepingCentre(diameter, diameter);
    const auto centre = dial.getCentre();
    const float radius = diameter * 0.5f;
    const float arcThickness = juce::jlimit(2.0f, 5.0f, radius * 0.12f);
    const float arcRadius = radius - arcThickness * 0.5f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const bool enabled = slider.isEnabled();

    // Track
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour(studio::track());
    g.strokePath(track, juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc from the start, or from the centre for a bipolar knob.
    const bool bipolar = knob != nullptr && knob->isBipolar();
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    if (std::abs(angle - from) > 0.001f) {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin(from, angle),
                            juce::jmax(from, angle), true);
        g.setColour(enabled ? Palette::accentTeal : Palette::textMuted);
        g.strokePath(value,
                     juce::PathStrokeType(arcThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Body: a neutral disc inside the arc, then the pointer.
    const float bodyRadius = arcRadius - arcThickness - 2.0f;
    if (bodyRadius > 3.0f) {
        const auto body = juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);
        g.setGradientFill(juce::ColourGradient(studio::surfaceRaised().brighter(0.08f), centre.x, body.getY(),
                                               studio::surface(), centre.x, body.getBottom(), false));
        g.fillEllipse(body);
        g.setColour(studio::hairline());
        g.drawEllipse(body, 1.0f);
        const auto tip = centre.getPointOnCircumference(bodyRadius * 0.85f, angle);
        const auto base = centre.getPointOnCircumference(bodyRadius * 0.35f, angle);
        g.setColour(enabled ? Palette::textPrimary : Palette::textMuted);
        g.drawLine({base, tip}, juce::jmax(1.5f, radius * 0.06f));
    }
    if (slider.hasKeyboardFocus(false)) {
        g.setColour(Palette::accentTeal.withAlpha(0.6f));
        g.drawEllipse(dial.expanded(1.0f), 1.0f);
    }

    // Plain tabular text, no box; hidden while the inline editor is up.
    if (!valueRow.isEmpty() && (knob == nullptr || !knob->isValueEditorShowing())) {
        g.setColour(enabled ? Palette::textPrimary : Palette::textMuted);
        g.setFont(studio::numberFont(studio::kBodySize));
        g.drawText(slider.getTextFromValue(slider.getValue()), valueRow, juce::Justification::centred, false);
    }
    if (!labelRow.isEmpty()) {
        g.setColour(Palette::textMuted);
        g.setFont(studio::labelFont());
        g.drawText(label, labelRow, juce::Justification::centred, true);
    }
}

void KhensuKnobLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                             float minSliderPos, float maxSliderPos,
                                             juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical
        && style != juce::Slider::LinearBar && style != juce::Slider::LinearBarVertical) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    const bool vertical = style == juce::Slider::LinearVertical || style == juce::Slider::LinearBarVertical;
    auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const auto* knob = asKnob(slider);
    const bool bipolar = knob != nullptr && knob->isBipolar();
    const float thickness = 3.0f;
    const auto trackRect = vertical ? area.withSizeKeepingCentre(thickness, area.getHeight())
                                    : area.withSizeKeepingCentre(area.getWidth(), thickness);
    g.setColour(studio::track());
    g.fillRoundedRectangle(trackRect, thickness * 0.5f);

    const float zeroPos = bipolar ? (vertical ? trackRect.getCentreY() : trackRect.getCentreX())
                                  : (vertical ? trackRect.getBottom() : trackRect.getX());
    g.setColour(slider.isEnabled() ? Palette::accentTeal : Palette::textMuted);
    if (vertical)
        g.fillRect(trackRect.getX(), juce::jmin(zeroPos, sliderPos), thickness, std::abs(zeroPos - sliderPos));
    else
        g.fillRect(juce::jmin(zeroPos, sliderPos), trackRect.getY(), std::abs(sliderPos - zeroPos), thickness);

    // Thumb: a short rounded bar across the track.
    const float thumbLong = 14.0f, thumbShort = 5.0f;
    const auto thumb = vertical ? juce::Rectangle<float>(thumbLong, thumbShort).withCentre({trackRect.getCentreX(), sliderPos})
                                : juce::Rectangle<float>(thumbShort, thumbLong).withCentre({sliderPos, trackRect.getCentreY()});
    g.setColour(Palette::textPrimary);
    g.fillRoundedRectangle(thumb, 2.0f);
    if (slider.hasKeyboardFocus(false)) {
        g.setColour(Palette::accentTeal.withAlpha(0.6f));
        g.drawRoundedRectangle(thumb.expanded(2.0f), 3.0f, 1.0f);
    }
}

juce::Font KhensuKnobLookAndFeel::getSliderPopupFont(juce::Slider&)
{
    return studio::numberFont(studio::kBodySize);
}

int KhensuKnobLookAndFeel::getSliderPopupPlacement(juce::Slider&)
{
    return juce::BubbleComponent::above | juce::BubbleComponent::below;
}

void KhensuKnobLookAndFeel::drawBubble(juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&,
                                       const juce::Rectangle<float>& body)
{
    g.setColour(studio::surfaceRaised());
    g.fillRoundedRectangle(body, 4.0f);
    g.setColour(studio::hairline());
    g.drawRoundedRectangle(body.reduced(0.5f), 4.0f, 1.0f);
}

juce::Label* KhensuKnobLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(studio::numberFont(studio::kBodySize));
    label->setColour(juce::Label::textColourId, Palette::textPrimary);
    label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return label;
}

//==============================================================================
KhensuKnob::KhensuKnob(const juce::String& label, Style style)
    : juce::Slider(style == Style::Rotary             ? juce::Slider::RotaryHorizontalVerticalDrag
                   : style == Style::LinearHorizontal ? juce::Slider::LinearHorizontal
                                                      : juce::Slider::LinearVertical,
                   juce::Slider::NoTextBox)
{
    setLookAndFeel(&lookAndFeel_.get());
    setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    setScrollWheelEnabled(true);
    setWantsKeyboardFocus(true);
    setLabel(label);
    setTooltip(usageHint());
}

KhensuKnob::~KhensuKnob()
{
    valueEditor_.reset();
    setLookAndFeel(nullptr); // the shared look-and-feel may go with the last knob
}

void KhensuKnob::setLabel(const juce::String& label)
{
    label_ = label;
    if (getTitle().isEmpty() && label.isNotEmpty()) setTitle(label);
    repaint();
}

void KhensuKnob::setDefaultValue(double value)
{
    // Alt/Option-click returns here (JUCE's single-click modifier); a
    // double-click opens the value editor instead (mouseDoubleClick).
    setDoubleClickReturnValue(true, value, juce::ModifierKeys::altModifier);
}

void KhensuKnob::setUnit(Unit unit)
{
    unit_ = unit;
    textFromValueFunction = [unit](double v) { return formatValue(v, unit); };
    valueFromTextFunction = [this, unit](const juce::String& text) { return parseValue(text, unit, getValue()); };
    updateText();
    repaint();
}

void KhensuKnob::setBipolar(bool bipolar)
{
    bipolar_ = bipolar;
    repaint();
}

void KhensuKnob::updatePopupMode()
{
    // The value is drawn under the dial when it fits; only a knob too small
    // for that (or a linear slider) gets the popup bubble. The popup lives
    // inside the top-level component (a plugin window must not open desktop
    // windows of its own).
    auto* top = getTopLevelComponent();
    const bool drawsValue = isRotary() && !layoutFor(getLocalBounds().toFloat(), label_.isNotEmpty()).value.isEmpty();
    setPopupDisplayEnabled(!drawsValue, !drawsValue, top != this ? top : nullptr, 1500);
}

void KhensuKnob::parentHierarchyChanged()
{
    juce::Slider::parentHierarchyChanged();
    updatePopupMode();
}

void KhensuKnob::resized()
{
    juce::Slider::resized();
    updatePopupMode();
    if (isValueEditorShowing()) valueEditor_->setBounds(valueEditorBounds());
}

juce::Rectangle<int> KhensuKnob::valueEditorBounds() const
{
    const auto local = getLocalBounds();
    const auto rows = layoutFor(local.toFloat(), label_.isNotEmpty());
    const int w = juce::jmax(56, local.getWidth());
    if (isRotary() && !rows.value.isEmpty())
        return rows.value.toNearestInt().withSizeKeepingCentre(
            w, juce::jmax(18, juce::roundToInt(rows.value.getHeight())));
    return local.withSizeKeepingCentre(w, 20);
}

void KhensuKnob::applyValueEditorColours()
{
    if (valueEditor_ == nullptr) return;
    auto& ed = *valueEditor_;
    ed.setColour(juce::TextEditor::backgroundColourId, studio::surfaceRaised());
    ed.setColour(juce::TextEditor::textColourId, Palette::textPrimary);
    ed.setColour(juce::TextEditor::highlightColourId, Palette::accentTeal.withAlpha(0.35f));
    ed.setColour(juce::TextEditor::outlineColourId, Palette::accentTeal.withAlpha(0.7f));
    ed.setColour(juce::TextEditor::focusedOutlineColourId, Palette::accentTeal);
    ed.setColour(juce::CaretComponent::caretColourId, Palette::textPrimary);
}

void KhensuKnob::showValueEditor()
{
    if (!isEnabled()) return;
    if (valueEditor_ == nullptr) {
        valueEditor_ = std::make_unique<juce::TextEditor>("value");
        auto& ed = *valueEditor_;
        ed.setFont(studio::numberFont(studio::kBodySize));
        ed.setJustification(juce::Justification::centred);
        ed.setSelectAllWhenFocused(true);
        ed.setIndents(2, 1);
        ed.onReturnKey = [this] { hideValueEditor(true); };
        ed.onEscapeKey = [this] { hideValueEditor(false); };
        ed.onFocusLost = [this] { hideValueEditor(true); };
        addChildComponent(ed);
    }
    applyValueEditorColours(); // the current palette, every time it opens (STU-6)
    auto& ed = *valueEditor_;
    ed.setTitle((getTitle().isNotEmpty() ? getTitle() : label_) + " " + TRANS("value"));
    ed.setBounds(valueEditorBounds());
    ed.setText(getTextFromValue(getValue()), juce::dontSendNotification);
    ed.setVisible(true);
    // Only a knob on screen can take the focus (JUCE asserts otherwise - an
    // off-screen knob, as in the tests, still opens and takes typed values).
    if (ed.isShowing()) ed.grabKeyboardFocus();
    ed.selectAll();
    repaint();
}

void KhensuKnob::hideValueEditor(bool commit)
{
    if (!isValueEditorShowing()) return;
    const auto text = valueEditor_->getText();
    // Hidden, never deleted, here: this runs inside the editor's own callbacks.
    valueEditor_->setVisible(false);
    if (commit && text.trim().isNotEmpty()) {
        const double typed = getValueFromText(text);
        if (std::isfinite(typed)) {
            const juce::Slider::ScopedDragNotification gesture(*this); // one host gesture
            setValue(typed, juce::sendNotificationSync);
        }
    }
    repaint();
}

void KhensuKnob::mouseDown(const juce::MouseEvent& e)
{
    if (isValueEditorShowing()) hideValueEditor(true);
    lastDragPos_ = e.position;
    dragProportion_ = valueToProportionOfLength(getValue());
    // Alt/Option-click resets (JUCE does it, as one gesture); no drag follows.
    resetClick_ = e.mods.withoutMouseButtons() == juce::ModifierKeys(juce::ModifierKeys::altModifier);
    // Rotary knobs always drag relatively; linear sliders keep JUCE's
    // absolute click-to-position unless Shift asks for fine control.
    customDrag_ = !resetClick_ && (isRotary() || e.mods.isShiftDown());
    juce::Slider::mouseDown(e); // starts the host gesture, shows the popup
}

void KhensuKnob::mouseUp(const juce::MouseEvent& e)
{
    resetClick_ = false;
    juce::Slider::mouseUp(e);
}

void KhensuKnob::mouseDoubleClick(const juce::MouseEvent&)
{
    showValueEditor();
}

bool KhensuKnob::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::returnKey
        && key.getModifiers().withoutMouseButtons() == juce::ModifierKeys()) {
        showValueEditor();
        return true;
    }
    return juce::Slider::keyPressed(key);
}

void KhensuKnob::applyDragDelta(float dx, float dy, bool fine)
{
    const bool vertical = getSliderStyle() == juce::Slider::LinearVertical;
    const double pixels = (isRotary() ? dragPixels_ : static_cast<double>(vertical ? getHeight() : getWidth()))
                          * (fine ? fineFactor_ : 1.0);
    const double delta = isRotary() ? static_cast<double>(dx - dy) : static_cast<double>(vertical ? -dy : dx);
    dragProportion_ = juce::jlimit(0.0, 1.0, dragProportion_ + delta / juce::jmax(1.0, pixels));
    setValue(proportionOfLengthToValue(dragProportion_), juce::sendNotificationSync);
}

void KhensuKnob::mouseDrag(const juce::MouseEvent& e)
{
    if (!isEnabled() || resetClick_) return;
    if (!customDrag_ && e.mods.isShiftDown()) {
        // Shift pressed during an absolute linear drag: continue relatively.
        customDrag_ = true;
        lastDragPos_ = e.position;
        dragProportion_ = valueToProportionOfLength(getValue());
    }
    if (!customDrag_) {
        juce::Slider::mouseDrag(e);
        return;
    }
    applyDragDelta(e.position.x - lastDragPos_.x, e.position.y - lastDragPos_.y, e.mods.isShiftDown());
    lastDragPos_ = e.position;
}

void KhensuKnob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!isEnabled() || !isScrollWheelEnabled()) {
        juce::Slider::mouseWheelMove(e, wheel);
        return;
    }
    const float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (std::abs(delta) < 1.0e-4f) return;
    const double step = (wheel.isReversed ? -1.0 : 1.0) * (delta > 0.0f ? 1.0 : -1.0)
                        * (e.mods.isShiftDown() ? 0.005 : 0.04);
    const double proportion = juce::jlimit(0.0, 1.0, valueToProportionOfLength(getValue()) + step);
    // One wheel notch is one host gesture.
    const juce::Slider::ScopedDragNotification gesture(*this);
    setValue(proportionOfLengthToValue(proportion), juce::sendNotificationSync);
}

double KhensuKnob::simulateDragForTest(float dx, float dy, bool fine)
{
    dragProportion_ = valueToProportionOfLength(getValue());
    applyDragDelta(dx, dy, fine);
    return getValue();
}

} // namespace khensu::ui
