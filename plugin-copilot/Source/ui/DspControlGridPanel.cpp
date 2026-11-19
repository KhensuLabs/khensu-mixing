#include "DspControlGridPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include "../PluginProcessor.h"

namespace khensu {

DspControlGridPanel::DspControlGridPanel(KhensuCopilotProcessor& proc) : processor_(proc)
{
    using P = KhensuCopilotProcessor::ParamIDs;

    addToggle(P::hpfOn, "HPF", stageToggles_);
    addKnob(P::hpfFreq, "Freq");

    addToggle(P::lpfOn, "LPF", stageToggles_);
    addKnob(P::lpfFreq, "Freq");

    addToggle(P::eqOn, "EQ", stageToggles_);
    addKnob(P::eqFreq, "Freq");
    addKnob(P::eqGainDb, "Gain");
    addKnob(P::eqQ, "Q");

    addToggle(P::eq2On, "EQ 2", stageToggles_);
    addKnob(P::eq2Freq, "Freq");
    addKnob(P::eq2GainDb, "Gain");
    addKnob(P::eq2Q, "Q");

    addToggle(P::eq3On, "EQ 3", stageToggles_);
    addKnob(P::eq3Freq, "Freq");
    addKnob(P::eq3GainDb, "Gain");
    addKnob(P::eq3Q, "Q");

    addToggle(P::compOn, "Comp", stageToggles_);
    addKnob(P::compThreshDb, "Thresh");
    addKnob(P::compRatio, "Ratio");
    addKnob(P::compAttackMs, "Attack");
    addKnob(P::compReleaseMs, "Release");

    // Multiband compressor (PLG-20): its switch, the three band thresholds
    // and the two crossover frequencies had no controls at all.
    addToggle(P::compMultiband, "Multiband", stageToggles_);
    addKnob(P::compLowThreshDb, "Low Thr");
    addKnob(P::compMidThreshDb, "Mid Thr");
    addKnob(P::compHighThreshDb, "High Thr");
    addKnob(P::crossoverLowFreq, "X-Low");
    addKnob(P::crossoverHighFreq, "X-High");

    addKnob(P::outGainDb, "Out Gain");
    addKnob(P::chainMix, "Mix");

    addToggle(P::limOn, "Limiter", stageToggles_);
    addKnob(P::limThreshDb, "Ceiling");

    visibilityChanged();
}

void DspControlGridPanel::visibilityChanged()
{
    // Only poll while the Chain tab is shown (hidden-tab timers paused).
    if (isVisible())
        startTimerHz(10);
    else
        stopTimer();
}

DspControlGridPanel::~DspControlGridPanel() { stopTimer(); }

void DspControlGridPanel::addKnob(const char* paramId, const juce::String& labelText)
{
    KnobRow row;
    row.paramId = paramId;

    row.slider = std::make_unique<NamedSlider>();
    row.slider->setLookAndFeel(&lookAndFeel_);
    addAndMakeVisible(*row.slider);

    row.label = std::make_unique<juce::Label>();
    row.label->setText(labelText, juce::dontSendNotification);
    row.label->setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    row.label->setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    row.label->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(*row.label);

    auto* param = processor_.getAPVTS().getParameter(paramId);
    row.sliderAttachment =
        std::make_unique<juce::SliderParameterAttachment>(*param, *row.slider, processor_.getAPVTS().undoManager);

    // Spoken name + tooltip from the full parameter name ("Thresh" alone is
    // ambiguous between compressor and limiter), and name the editable value
    // box too: it is a separate focus stop (audit AUD-UX-01).
    const auto fullName = param->getName(64);
    row.slider->setTitle(fullName);
    row.slider->setTooltip(fullName + " - drag or use arrow keys; type a value in the box below.");
    row.slider->nameValueBox();
    // Parameters carry no string formatter, so the attachment would print
    // raw float noise ("79.9999924"); show three significant figures-ish.
    row.slider->textFromValueFunction = [](double v) {
        const double a = std::abs(v);
        return juce::String(v, a >= 100.0 ? 0 : (a >= 10.0 ? 1 : 2));
    };
    row.slider->updateText();

    // The AI-set dot clears on ANY value change that is not an AI write
    // (drag, arrow keys, typed value, host automation) - the processor's
    // parameter listener does it (PLG-20), so no per-control hook is needed.
    knobs_.push_back(std::move(row));
}

void DspControlGridPanel::addToggle(const char* paramId, const juce::String& labelText, std::vector<ToggleRow>& into)
{
    ToggleRow row;
    row.paramId = paramId;
    row.button = std::make_unique<juce::ToggleButton>(labelText);
    addAndMakeVisible(*row.button);

    auto* param = processor_.getAPVTS().getParameter(paramId);
    row.button->setTitle(param->getName(64));
    row.button->setTooltip("Enable or disable " + param->getName(64) + ".");
    row.attachment =
        std::make_unique<juce::ButtonParameterAttachment>(*param, *row.button, processor_.getAPVTS().undoManager);

    into.push_back(std::move(row));
}

void DspControlGridPanel::timerCallback()
{
    // Repaint only when an AI-set dot actually changed (it used to repaint the
    // whole grid 33 times a second, shown or not).
    std::uint64_t mask = 0;
    for (size_t i = 0; i < knobs_.size(); ++i)
        if (processor_.isAiSet(knobs_[i].paramId.toRawUTF8())) mask |= (std::uint64_t{1} << i);
    for (size_t i = 0; i < stageToggles_.size(); ++i)
        if (processor_.isAiSet(stageToggles_[i].paramId.toRawUTF8())) mask |= (std::uint64_t{1} << (32 + i));
    if (mask != lastAiMask_) {
        lastAiMask_ = mask;
        repaint();
    }
}

void DspControlGridPanel::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgPanel);
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRect(getLocalBounds(), 1);

    // AI-set dots: drawn directly above each slider/toggle's bounds, in this
    // component's own paint pass (not the slider's), so nothing about
    // JUCE's internal Slider/ToggleButton redraw is touched.
    auto drawDot = [&](const juce::Rectangle<int>& controlBounds, const juce::String& paramId) {
        if (!processor_.isAiSet(paramId.toRawUTF8())) return;
        const int d = 8;
        juce::Rectangle<int> dot(controlBounds.getCentreX() - d / 2, controlBounds.getY() - d - 2, d, d);
        g.setColour(PluginLookAndFeel::accentTeal);
        g.fillEllipse(dot.toFloat());
    };

    for (const auto& row : knobs_)
        drawDot(row.slider->getBounds(), row.paramId);
    for (const auto& row : stageToggles_)
        drawDot(row.button->getBounds(), row.paramId);
}

void DspControlGridPanel::resized()
{
    auto b = getLocalBounds().reduced(10);

    // Two columns of five rows, the row height derived from the space
    // actually available, so every knob stays legible at the minimum editor
    // size and scales up when the host window grows.
    struct RowSpec {
        int knobs;
        bool toggle;
    };
    // Construction order: HPF, LPF, EQ1, EQ2, EQ3 | Comp, Multiband, Out/Mix, Limiter.
    static constexpr RowSpec kRows[] = {{1, true}, {1, true}, {3, true}, {3, true}, {3, true},
                                        {4, true}, {5, true}, {2, false}, {1, true}};
    constexpr int kRowsPerColumn = 5;
    constexpr int kGap = 4;
    constexpr int kToggleW = 78;

    const int colGap = 12;
    const int colW = (b.getWidth() - colGap) / 2;
    const int rowH = juce::jlimit(48, 110, (b.getHeight() - kGap * (kRowsPerColumn - 1)) / kRowsPerColumn);
    const int knobW = juce::jmax(36, juce::jmin(90, (colW - kToggleW) / 5));

    size_t knobIdx = 0, toggleIdx = 0;
    for (int i = 0; i < static_cast<int>(std::size(kRows)); ++i) {
        const int col = i / kRowsPerColumn;
        const int rowInCol = i % kRowsPerColumn;
        juce::Rectangle<int> row(b.getX() + col * (colW + colGap), b.getY() + rowInCol * (rowH + kGap), colW, rowH);
        auto toggleArea = row.removeFromLeft(kToggleW);
        if (kRows[i].toggle && toggleIdx < stageToggles_.size()) {
            stageToggles_[toggleIdx].button->setBounds(toggleArea.withSizeKeepingCentre(kToggleW, 24));
            ++toggleIdx;
        }
        for (int k = 0; k < kRows[i].knobs && knobIdx < knobs_.size(); ++k, ++knobIdx) {
            auto knobArea = row.removeFromLeft(knobW);
            knobs_[knobIdx].label->setBounds(knobArea.removeFromTop(14));
            knobs_[knobIdx].slider->setBounds(knobArea.reduced(2, 1));
        }
    }
}

} // namespace khensu
