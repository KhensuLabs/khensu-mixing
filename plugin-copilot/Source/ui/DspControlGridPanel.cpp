#include "DspControlGridPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>
#include "../CopilotUndoHistory.h"
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
    addShape(P::eqType);
    addKnob(P::eqFreq, "Freq");
    addKnob(P::eqGainDb, "Gain");
    addKnob(P::eqQ, "Q");

    addToggle(P::eq2On, "EQ 2", stageToggles_);
    addShape(P::eq2Type);
    addKnob(P::eq2Freq, "Freq");
    addKnob(P::eq2GainDb, "Gain");
    addKnob(P::eq2Q, "Q");

    addToggle(P::eq3On, "EQ 3", stageToggles_);
    addShape(P::eq3Type);
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

    // OVH-8: the split-band de-esser.
    addToggle(P::deessOn, "De-ess", stageToggles_);
    addKnob(P::deessFreq, "Freq");
    addKnob(P::deessThreshDb, "Thresh");
    addKnob(P::deessRangeDb, "Range");

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
        startTimerHz(30); // VIZ-6b: the live reduction meters
    else
        stopTimer();
}

DspControlGridPanel::~DspControlGridPanel() { stopTimer(); }

ui::KhensuKnob* DspControlGridPanel::knobForTest(const juce::String& paramId) const
{
    for (const auto& row : knobs_)
        if (row.paramId == paramId) return row.knob.get();
    return nullptr;
}

void DspControlGridPanel::addKnob(const char* paramId, const juce::String& labelText)
{
    KnobRow row;
    row.paramId = paramId;
    row.knob = std::make_unique<ui::KhensuKnob>(labelText);
    addAndMakeVisible(*row.knob);

    auto* param = processor_.getAPVTS().getParameter(paramId);
    // The undo history (UX2-6b) records the gesture, not the APVTS.
    row.attachment = std::make_unique<juce::SliderParameterAttachment>(*param, *row.knob, nullptr);

    // After the attachment, which installs the parameter's own (raw float)
    // text functions: the unit-aware readout and typed-value parser.
    const auto unit = copilot::ParamUndoHistory::unitFor(paramId);
    row.knob->setUnit(unit);
    row.knob->setDefaultValue(static_cast<double>(param->convertFrom0to1(param->getDefaultValue())));
    const auto& range = param->getNormalisableRange();
    row.knob->setBipolar(unit == ui::KhensuKnob::Unit::Db && range.start < 0.0f && range.end > 0.0f);

    // Spoken name + tooltip from the full parameter name ("Thresh" alone is
    // ambiguous between compressor and limiter).
    const auto fullName = param->getName(64);
    row.knob->setTitle(fullName);
    row.knob->setTooltip(fullName + ". " + ui::KhensuKnob::usageHint());

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
    row.attachment = std::make_unique<juce::ButtonParameterAttachment>(*param, *row.button, nullptr);

    into.push_back(std::move(row));
}

void DspControlGridPanel::addShape(const char* paramId)
{
    ShapeRow row;
    row.box = std::make_unique<juce::ComboBox>();
    auto* param = processor_.getAPVTS().getParameter(paramId);
    row.box->addItemList(param->getAllValueStrings(), 1);
    row.box->setTitle(param->getName(64));
    row.box->setTooltip(param->getName(64) + ": bell, low shelf or high shelf.");
    row.box->setColour(juce::ComboBox::backgroundColourId, PluginLookAndFeel::bgDark);
    row.box->setColour(juce::ComboBox::textColourId, PluginLookAndFeel::textPrimary);
    row.box->setColour(juce::ComboBox::outlineColourId, PluginLookAndFeel::controlOutline);
    row.box->setColour(juce::ComboBox::arrowColourId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(*row.box);
    row.attachment = std::make_unique<juce::ComboBoxParameterAttachment>(*param, *row.box, nullptr);
    shapes_.push_back(std::move(row));
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
    // VIZ-6b: the live meters move every tick.
    auto push = [](std::vector<float>& h, float v) {
        h.push_back(v);
        if (h.size() > kHistory) h.erase(h.begin());
    };
    push(limiterHistory_, processor_.getLimiterReductionDb());
    push(deessHistory_, processor_.getDeessReductionDb());
    for (size_t i = 0; i < vizBounds_.size(); ++i)
        if (!vizBounds_[i].isEmpty()) repaint(vizBounds_[i]);
}

void DspControlGridPanel::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgPanel);
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRect(getLocalBounds(), 1);

    // A hairline between rows, so each stage reads as one line.
    g.setColour(ui::studio::hairline());
    for (size_t i = 0; i < rowBounds_.size(); ++i) {
        const auto& r = rowBounds_[i];
        if (i % 5 != 0) g.fillRect(r.getX(), r.getY() - 3, r.getWidth(), 1);
    }

    // AI-set dots at each control's top-right corner, in this component's own
    // paint pass (nothing about the controls' own drawing is touched).
    auto drawDot = [&](const juce::Rectangle<int>& controlBounds, const juce::String& paramId) {
        if (!processor_.isAiSet(paramId.toRawUTF8())) return;
        const int d = 7;
        juce::Rectangle<int> dot(controlBounds.getRight() - d - 2, controlBounds.getY() + 1, d, d);
        g.setColour(PluginLookAndFeel::accentTeal);
        g.fillEllipse(dot.toFloat());
    };

    for (const auto& row : knobs_)
        drawDot(row.knob->getBounds(), row.paramId);
    for (const auto& row : stageToggles_)
        drawDot(row.button->getBounds(), row.paramId);
    for (size_t i = 0; i < vizBounds_.size(); ++i)
        if (!vizBounds_[i].isEmpty()) paintViz(g, static_cast<int>(i), vizBounds_[i].toFloat());
}

float DspControlGridPanel::param(const char* id) const
{
    auto* p = processor_.getAPVTS().getRawParameterValue(id);
    return p != nullptr ? p->load() : 0.0f;
}

void DspControlGridPanel::paintViz(juce::Graphics& g, int row, juce::Rectangle<float> r)
{
    using P = KhensuCopilotProcessor::ParamIDs;
    const auto style = ui::module_viz::Style::fromPalette();
    const double sr = processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0;
    auto eqBand = [&](const char* type, const char* freq, const char* gain, const char* q) {
        EqBand b;
        const int shape = juce::roundToInt(param(type));
        b.type = shape == 1 ? EqBandType::LowShelf : shape == 2 ? EqBandType::HighShelf : EqBandType::Peak;
        b.hz = param(freq);
        b.gain_db = param(gain);
        b.q = param(q);
        return b;
    };
    switch (row) {
    case 0: {
        EqBand b;
        b.type = EqBandType::HighPass;
        b.hz = param(P::hpfFreq);
        ui::module_viz::paintEqResponse(g, r, {b}, sr, style);
        break;
    }
    case 1: {
        EqBand b;
        b.type = EqBandType::LowPass;
        b.hz = param(P::lpfFreq);
        ui::module_viz::paintEqResponse(g, r, {b}, sr, style);
        break;
    }
    case 2: ui::module_viz::paintEqResponse(g, r, {eqBand(P::eqType, P::eqFreq, P::eqGainDb, P::eqQ)}, sr, style); break;
    case 3: ui::module_viz::paintEqResponse(g, r, {eqBand(P::eq2Type, P::eq2Freq, P::eq2GainDb, P::eq2Q)}, sr, style); break;
    case 4: ui::module_viz::paintEqResponse(g, r, {eqBand(P::eq3Type, P::eq3Freq, P::eq3GainDb, P::eq3Q)}, sr, style); break;
    case 5: {
        ui::module_viz::CompressorState c;
        c.thresholdDb = param(P::compThreshDb);
        c.ratio = param(P::compRatio);
        const float in = processor_.getCompInputDb();
        if (in > -100.0f) c.inputDb = in;
        c.reductionDb = processor_.getCompReductionDb();
        ui::module_viz::paintCompressor(g, r, c, style);
        break;
    }
    case 7: ui::module_viz::paintReductionHistory(g, r, deessHistory_, style, 12.0f); break;
    case 9: ui::module_viz::paintReductionHistory(g, r, limiterHistory_, style, 6.0f); break;
    default: break;
    }
}

void DspControlGridPanel::resized()
{
    auto b = getLocalBounds().reduced(14, 10);

    // Two columns of five rows. Every knob gets the same cell - sized from
    // the widest row (five knobs) and the row height - so dials, values and
    // names sit on one grid across both columns, and scale with the window.
    struct RowSpec {
        int knobs;
        bool toggle;
    };
    // Construction order: HPF, LPF, EQ1, EQ2, EQ3 | Comp, Multiband, De-esser, Out/Mix, Limiter.
    static constexpr RowSpec kRows[] = {{1, true}, {1, true}, {3, true}, {3, true}, {3, true},
                                        {4, true}, {5, true}, {3, true}, {2, false}, {1, true}};
    constexpr int kRowsPerColumn = 5;
    constexpr int kMaxKnobsPerRow = 5;
    constexpr int kRowGap = 6;
    constexpr int kColGap = 28;

    const int colW = (b.getWidth() - kColGap) / 2;
    const int toggleW = juce::jlimit(70, 96, colW / 5);
    const int rowH = juce::jlimit(44, 124, (b.getHeight() - kRowGap * (kRowsPerColumn - 1)) / kRowsPerColumn);
    const int cellW = juce::jlimit(40, 92, (colW - toggleW - 8) / kMaxKnobsPerRow);
    // Gap between cells: the dial is at most rowH-30 wide, so a wide cell
    // keeps generous air between neighbours.

    rowBounds_.clear();
    vizBounds_.clear();
    size_t knobIdx = 0, toggleIdx = 0, shapeIdx = 0;
    for (int i = 0; i < static_cast<int>(std::size(kRows)); ++i) {
        const int col = i / kRowsPerColumn;
        const int rowInCol = i % kRowsPerColumn;
        juce::Rectangle<int> row(b.getX() + col * (colW + kColGap), b.getY() + rowInCol * (rowH + kRowGap), colW, rowH);
        rowBounds_.push_back(row);
        auto toggleArea = row.removeFromLeft(toggleW);
        row.removeFromLeft(8);
        if (kRows[i].toggle && toggleIdx < stageToggles_.size()) {
            const bool eqRow = i >= 2 && i <= 4 && shapeIdx < shapes_.size();
            if (eqRow && toggleArea.getHeight() >= 52) {
                // The band's switch over its shape.
                auto top = toggleArea.removeFromTop(toggleArea.getHeight() / 2);
                stageToggles_[toggleIdx].button->setBounds(top.withSizeKeepingCentre(toggleW, 24).withY(top.getBottom() - 26));
                shapes_[shapeIdx++].box->setBounds(toggleArea.withHeight(22).translated(0, 2).withWidth(toggleW - 6));
            } else if (eqRow) {
                // Too short for two lines: switch and shape side by side is
                // too wide, so the shape takes the switch's lower half.
                stageToggles_[toggleIdx].button->setBounds(toggleArea.removeFromTop(toggleArea.getHeight() / 2));
                shapes_[shapeIdx++].box->setBounds(toggleArea.withWidth(toggleW - 6));
            } else {
                stageToggles_[toggleIdx].button->setBounds(toggleArea.withSizeKeepingCentre(toggleW, 24));
            }
            ++toggleIdx;
        }
        for (int k = 0; k < kRows[i].knobs && knobIdx < knobs_.size(); ++k, ++knobIdx)
            knobs_[knobIdx].knob->setBounds(row.removeFromLeft(cellW).reduced(3, 0));
        // VIZ-6b: what the module does, in the room right of its knobs.
        row.removeFromLeft(6);
        vizBounds_.push_back(row.getWidth() >= 48 && row.getHeight() >= 28
                                 ? row.withSizeKeepingCentre(juce::jmin(row.getWidth(), 220), juce::jmin(row.getHeight() - 6, 90))
                                 : juce::Rectangle<int>());
    }
}

} // namespace khensu
