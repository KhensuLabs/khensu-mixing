// desktop-app/Source/ui/Dashboard/WeightProfileOverlay.cpp
#include "WeightProfileOverlay.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void WeightProfileOverlay::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

WeightProfileOverlay::WeightProfileOverlay(const ScoreWeightProfile& initial, const juce::String& genreLabel)
    : current_(initial)
{
    applyThemeColours();
    addAndMakeVisible(card_);

    titleLabel_.setText(TRANS("Score Weight Profile -- ") + genreLabel, juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::subtitle, juce::Font::bold));
    card_.addAndMakeVisible(titleLabel_);

    viewport_.setViewedComponent(&rowsContainer_, false);
    viewport_.setScrollBarsShown(true, false);
    card_.addAndMakeVisible(viewport_);

    addRow("Headroom Penalty Cap", &ScoreWeightProfile::readiness_headroom_cap, 0.0, 40.0);
    addRow("Crest Factor Penalty Cap", &ScoreWeightProfile::readiness_crest_cap, 0.0, 40.0);
    addRow("Low-End Balance Penalty Cap", &ScoreWeightProfile::readiness_low_end_cap, 0.0, 40.0);
    addRow("Stereo Correlation Penalty Cap", &ScoreWeightProfile::readiness_stereo_cap, 0.0, 30.0);
    addRow("Critical Issues Penalty Cap", &ScoreWeightProfile::readiness_critical_recs_cap, 0.0, 40.0);
    addRow("Streaming Delta Penalty Cap", &ScoreWeightProfile::readiness_streaming_cap, 0.0, 30.0);
    addRow("FX & Processing Weight", &ScoreWeightProfile::scorecard_fx_weight, 0.0, 0.4);
    addRow("Mix Balance Weight", &ScoreWeightProfile::scorecard_mix_balance_weight, 0.0, 0.4);
    addRow("Stereo Width & Depth Weight", &ScoreWeightProfile::scorecard_stereo_weight, 0.0, 0.4);
    addRow("Tonal Balance & EQ Weight", &ScoreWeightProfile::scorecard_tonal_weight, 0.0, 0.4);
    addRow("Dynamics & Compression Weight", &ScoreWeightProfile::scorecard_dynamics_weight, 0.0, 0.4);
    addRow("Frequency Masking Weight", &ScoreWeightProfile::scorecard_masking_weight, 0.0, 0.4);
    addRow("Technical Integrity Weight", &ScoreWeightProfile::scorecard_technical_weight, 0.0, 0.4);

    resetButton_.onClick = [this]() {
        current_ = ScoreWeightProfile{};
        for (auto& row : rows_)
            row->slider.setValue(current_.*(row->field), juce::dontSendNotification);
    };
    card_.addAndMakeVisible(resetButton_);

    saveButton_.onClick = [this]() {
        if (onSave) onSave(current_);
    };
    card_.addAndMakeVisible(saveButton_);

    cancelButton_.onClick = [this]() {
        if (onCancel) onCancel();
    };
    card_.addAndMakeVisible(cancelButton_);

    grabModalFocus();
}

void WeightProfileOverlay::addRow(const juce::String& label, float ScoreWeightProfile::* field, double minV,
                                  double maxV)
{
    auto row = std::make_unique<Row>();
    row->field = field;
    row->label.setText(label, juce::dontSendNotification);
    row->label.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    row->label.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    rowsContainer_.addAndMakeVisible(row->label);

    row->slider.setRange(minV, maxV, maxV > 1.0 ? 0.5 : 0.01);
    row->slider.setSliderStyle(juce::Slider::LinearHorizontal);
    row->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    row->slider.setValue(current_.*field, juce::dontSendNotification);
    row->slider.onValueChange = [this, field, sliderPtr = &row->slider]() {
        current_.*field = static_cast<float>(sliderPtr->getValue());
    };
    rowsContainer_.addAndMakeVisible(row->slider);

    rows_.push_back(std::move(row));
}

void WeightProfileOverlay::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(560, bounds.getWidth() - 40);
    auto cardH = juce::jmin(560, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(16);
    titleLabel_.setBounds(b.removeFromTop(24));
    b.removeFromTop(8);

    auto footer = b.removeFromBottom(32);
    cancelButton_.setBounds(footer.removeFromRight(80));
    footer.removeFromRight(6);
    saveButton_.setBounds(footer.removeFromRight(80));
    footer.removeFromRight(6);
    resetButton_.setBounds(footer.removeFromLeft(150));
    b.removeFromBottom(8);

    viewport_.setBounds(b);
    const int rowH = 30;
    const int w = viewport_.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& row : rows_) {
        auto rowBounds = juce::Rectangle<int>(0, y, w, rowH);
        row->label.setBounds(rowBounds.removeFromLeft(w * 55 / 100));
        row->slider.setBounds(rowBounds);
        y += rowH;
    }
    rowsContainer_.setSize(w, y);
}

} // namespace khensu
