// desktop-app/Source/ui/VocalWorkspace/VocalMacroPanel.cpp
#include "VocalMacroPanel.h"
#include "../../IAppController.h"
#include <khensu/feedback/FeedbackManager.h>
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/common/EventHub.h>

namespace khensu {

const char* VocalMacroPanel::kMacroNames[kMacroCount] = {"Compression", "Presence", "Width", "De-essing",
                                                         "Saturation",  "Reverb",   "Delay"};

// Every cached palette colour on this component, in one place (UI-01).
void VocalMacroPanel::applyThemeColours()
{
    resetButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgSurface);
    resetButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    learnButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgSurface);
    learnButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

VocalMacroPanel::VocalMacroPanel(IAppController& c) : PanelCard(c, "Vocal Macros (Suggested Starting Points)")
{
    applyThemeColours();
    setExplainer(TRANS("Vocal Macros"), TRANS("Suggested starting points for your vocal chain, based on the selected strategy. "
                                 "Values are on a 0-100 scale (not absolute dB or ms). "
                                 "Drag to adjust for taste. Click 'Reset to Suggested' to restore strategy defaults."));

    for (int i = 0; i < kMacroCount; ++i) {
        sliders_[i].setSliderStyle(juce::Slider::LinearHorizontal);
        sliders_[i].setTextBoxStyle(juce::Slider::TextBoxRight, false, 36, 20);
        sliders_[i].setRange(0.0, 100.0, 1.0);
        sliders_[i].setColour(juce::Slider::thumbColourId, KhensuTheme::accentTeal);
        addAndMakeVisible(sliders_[i]);

        labels_[i].setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        labels_[i].setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        addAndMakeVisible(labels_[i]);
        updateLabelText(i);

        const int idx = i;
        const juce::String name = kMacroNames[i];
        sliders_[i].onDragStart = [this]() { controller_.vocalEdit().pushUndoState(); };

        sliders_[i].onValueChange = [this, idx, name]() {
            float actual = static_cast<float>(sliders_[idx].getValue());
            controller_.vocalEdit().setVocalMacroValue(name, actual);
            updateLabelText(idx);

            if (auto* fm = controller_.analysis().getFeedbackManager()) {
                MacroPreset preset = presetForStrategy(currentStrategyId_);
                float recommended = static_cast<float>(preset.values[idx]);
                if (std::abs(actual - recommended) > 0.01f) {
                    fm->recordOverride("vocal_strategy_" + currentStrategyId_.toStdString(), name.toStdString(),
                                       recommended, actual);
                }
            }
        };
    }

    resetButton_.onClick = [this] {
        controller_.vocalEdit().pushUndoState();
        setStrategy(currentStrategyId_);
    };
    addAndMakeVisible(resetButton_);
    learnButton_.setTooltip(TRANS("Map hardware MIDI controls or OSC to these macros (learn mode)."));
    learnButton_.onClick = [this] { controller_.shell().showMacroMappingWindow(); };
    addAndMakeVisible(learnButton_);

    EventHub::getInstance().addListener(this, EventType::VocalStrategyChanged,
                                        [this](const EventData& data) { setStrategy(data.stringVal); });

    EventHub::getInstance().addListener(this, EventType::VocalMacroChanged, [this](const EventData& data) {
        for (int i = 0; i < kMacroCount; ++i) {
            if (juce::String(kMacroNames[i]) == data.stringVal) {
                if (std::abs(sliders_[i].getValue() - data.floatVal) > 0.01) {
                    sliders_[i].setValue(data.floatVal, juce::dontSendNotification);
                    updateLabelText(i);
                }
                break;
            }
        }
    });

    setStrategy("clean");
}

VocalMacroPanel::~VocalMacroPanel() { EventHub::getInstance().removeListener(this); }

void VocalMacroPanel::setStrategy(const juce::String& strategyId, bool forcePresetDefault)
{
    if (currentStrategyId_ == strategyId && isInitialized_ && !forcePresetDefault) {
        // Just sync the sliders with the controller's current macro values
        for (int i = 0; i < kMacroCount; ++i) {
            sliders_[i].setValue(controller_.vocalEdit().getVocalMacroValue(kMacroNames[i]), juce::dontSendNotification);
            updateLabelText(i);
        }
        return;
    }

    currentStrategyId_ = strategyId;
    isInitialized_ = true;

    // If the controller already has user/restored macro values, use those instead of defaults
    if (controller_.vocalEdit().hasExplicitVocalStrategy() && !forcePresetDefault) {
        for (int i = 0; i < kMacroCount; ++i) {
            float val = controller_.vocalEdit().getVocalMacroValue(kMacroNames[i]);
            sliders_[i].setValue(val, juce::dontSendNotification);
            updateLabelText(i);
        }
        return;
    }

    MacroPreset preset = presetForStrategy(strategyId);

    // Adapt starting points based on actual vocal characteristics if available
    if (controller_.stems().hasVocalAnalysis()) {
        const auto& r = controller_.stems().currentVocalAnalysis();

        // 1. Compression (index 0): Increase compression if vocal is highly dynamic (high crest factor)
        float crest = r.dynamics.crest_factor_db;
        if (crest > 0.0f) {
            float compDiff = (crest - 10.0f) * 2.5f; // e.g. crest factor 14dB -> +10% compression
            preset.values[0] = juce::jlimit(10, 100, preset.values[0] + static_cast<int>(compDiff));
        }

        // 2. Presence (index 1): Increase presence if vocal mid-range energy is low
        float midEnergy = r.spectral.balance_mid;
        if (midEnergy > 0.0f) {
            float presDiff = (0.4f - midEnergy) * 100.0f; // e.g. midEnergy 0.25 -> +15% presence
            preset.values[1] = juce::jlimit(10, 100, preset.values[1] + static_cast<int>(presDiff));
        }

        // 3. Width (index 2): Reduce width if vocal is already wide
        float width = r.stereo.width;
        if (width > 0.5f) {
            preset.values[2] = juce::jlimit(0, 100, preset.values[2] - 15);
        }

        // 4. De-essing (index 3): Increase de-essing if vocal sibilance risk (high energy) is high
        float highEnergy = r.spectral.balance_high;
        if (highEnergy > 0.0f) {
            float deessDiff = (highEnergy - 0.20f) * 150.0f; // e.g. highEnergy 0.3 -> +15% de-essing
            preset.values[3] = juce::jlimit(10, 100, preset.values[3] + static_cast<int>(deessDiff));
        }
    }

    for (int i = 0; i < kMacroCount; ++i) {
        sliders_[i].setValue(preset.values[i], juce::dontSendNotification);
        controller_.vocalEdit().setVocalMacroValue(kMacroNames[i], static_cast<float>(preset.values[i]));
        updateLabelText(i);
    }
}
void VocalMacroPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    const int labelH = 11;
    const int sliderH = 16;
    const int gap = 5;

    for (int i = 0; i < kMacroCount; ++i) {
        labels_[i].setBounds(b.removeFromTop(labelH));
        sliders_[i].setBounds(b.removeFromTop(sliderH));
        b.removeFromTop(gap);
    }
    b.removeFromTop(2);
    auto buttons = b.removeFromTop(24).withSizeKeepingCentre(340, 22);
    resetButton_.setBounds(buttons.removeFromLeft(160));
    buttons.removeFromLeft(20);
    learnButton_.setBounds(buttons);
}
/*static*/ VocalMacroPanel::MacroPreset VocalMacroPanel::presetForStrategy(const juce::String& id) noexcept
{
    // {Compression, Presence, Width, De-essing, Saturation, Reverb, Delay}
    if (id == "lead_pop" || id == "clean") return {{75, 70, 40, 60, 20, 30, 25}};
    if (id == "warm_intimate" || id == "intimate") return {{50, 55, 30, 30, 35, 45, 20}};
    if (id == "aggressive_cut" || id == "aggressive") return {{85, 80, 25, 50, 60, 20, 15}};
    if (id == "ambient_wide" || id == "ethereal") return {{40, 40, 80, 20, 15, 75, 60}};
    if (id == "airy_ethereal" || id == "breathy") return {{35, 50, 70, 25, 10, 65, 50}};
    return {{75, 70, 40, 60, 20, 30, 25}}; // fallback: lead_pop
}

void VocalMacroPanel::updateLabelText(int idx)
{
    const double val = sliders_[idx].getValue();
    const double norm = val / 100.0;
    juce::String targetDesc;

    if (idx == 0) { // Compression
        double thresh = -10.0 - std::pow(norm, 1.5) * 20.0;
        double ratio = 2.0 + std::pow(norm, 1.2) * 6.0;
        targetDesc = "Compressor (Thresh: " + juce::String(thresh, 1) + " dB, Ratio: " + juce::String(ratio, 1) + ":1)";
    } else if (idx == 1) { // Presence
        double boost = 0.5 + std::pow(norm, 1.5) * 4.5;
        targetDesc = "EQ (HF Boost: +" + juce::String(boost, 1) + " dB)";
    } else if (idx == 2) { // Width
        double width = 100.0 + std::pow(norm, 1.5) * 100.0;
        targetDesc = "Stereo Imager (Width: " + juce::String(juce::roundToInt(width)) + "%)";
    } else if (idx == 3) { // De-essing
        double sens = 2.0 + std::pow(norm, 1.2) * 6.0;
        double range = std::pow(norm, 1.5) * 10.0;
        targetDesc = "De-esser (Sens: " + juce::String(sens, 1) + ", Range: -" + juce::String(range, 1) + " dB)";
    } else if (idx == 4) { // Saturation
        int drive = static_cast<int>(std::pow(norm, 1.5) * 50.0);
        targetDesc = "Saturation (Drive: " + juce::String(drive) + "%)";
    } else if (idx == 5) { // Reverb
        double decay = 1.0 + std::pow(norm, 2.0) * 3.5;
        int mix = 5 + static_cast<int>(std::pow(norm, 1.5) * 45.0);
        targetDesc = "Reverb (Decay: " + juce::String(decay, 1) + "s, Mix: " + juce::String(mix) + "%)";
    } else if (idx == 6) { // Delay
        int feedback = 10 + static_cast<int>(std::pow(norm, 1.2) * 50.0);
        int mix = 5 + static_cast<int>(std::pow(norm, 1.5) * 35.0);
        targetDesc = "Delay (Feedback: " + juce::String(feedback) + "%, Mix: " + juce::String(mix) + "%)";
    }

    labels_[idx].setText(juce::String(kMacroNames[idx]) + " -> " + targetDesc, juce::dontSendNotification);
}

} // namespace khensu
