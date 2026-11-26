// desktop-app/Source/ui/SessionIntake/TargetOverrideEditor.cpp
#include "TargetOverrideEditor.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../../IAppController.h"
#include <khensu/feedback/FeedbackManager.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void TargetOverrideEditor::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

TargetOverrideEditor::TargetOverrideEditor(IAppController& c, const GenreEntry& genre) : controller_(c), genre_(genre)
{
    applyThemeColours();
    backdropAlpha_ = 0.65f;

    if (auto* fm = controller_.analysis().getFeedbackManager()) prefs_.loadFromDatabase(*fm);

    addAndMakeVisible(card_);

    // NOTE: UserPreferenceModel's offsets are stored under fixed, global
    // FeedbackManager keys (not namespaced per genre) - so despite showing
    // this genre's baseline values, the delta sliders below apply to every
    // genre, not just this one. The title says so explicitly rather than
    // implying per-genre scoping that doesn't exist. True per-genre scoping
    // would mean namespacing the preference keys the way ScoreWeightProfile
    // does for readiness/scorecard weights.
    titleLabel_.setText(TRANS("Target Overrides (applies to all genres) -- baseline shown for ") + juce::String(genre_.id),
                        juce::dontSendNotification);
    titleLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::subtitle, juce::Font::bold));
    card_.addAndMakeVisible(titleLabel_);

    closeButton_.onClick = [this]() {
        if (onClose) onClose();
    };
    card_.addAndMakeVisible(closeButton_);

    setupRow(lufsRow_, TRANS("Loudness (LUFS)"), -4.0, 4.0, prefs_.getLufsOffset(), [this](float v) {
        prefs_.setLufsOffset(v);
        scheduleSave();
        refreshLabels();
    });
    setupRow(lraRow_, TRANS("Loudness Range (LU)"), -6.0, 6.0, prefs_.getLraOffset(), [this](float v) {
        prefs_.setLraOffset(v);
        scheduleSave();
        refreshLabels();
    });
    setupRow(widthRow_, TRANS("Stereo Width"), -0.4, 0.4, prefs_.getStereoWidthOffset(), [this](float v) {
        prefs_.setStereoWidthOffset(v);
        scheduleSave();
        refreshLabels();
    });
    setupRow(lowEndRow_, TRANS("Low-End Weight"), -0.3, 0.3, prefs_.getLowEndWeightOffset(), [this](float v) {
        prefs_.setLowEndWeightOffset(v);
        scheduleSave();
        refreshLabels();
    });

    resetButton_.onClick = [this]() {
        prefs_ = UserPreferenceModel{};
        lufsRow_.slider.setValue(0.0, juce::dontSendNotification);
        lraRow_.slider.setValue(0.0, juce::dontSendNotification);
        widthRow_.slider.setValue(0.0, juce::dontSendNotification);
        lowEndRow_.slider.setValue(0.0, juce::dontSendNotification);
        saveNow();
        refreshLabels();
    };
    card_.addAndMakeVisible(resetButton_);

    refreshLabels();

    grabModalFocus();
}

void TargetOverrideEditor::setupRow(Row& row, const juce::String& name, double minV, double maxV, float initial,
                                    std::function<void(float)> onChange)
{
    row.label.setText(name, juce::dontSendNotification);
    row.label.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    row.label.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    card_.addAndMakeVisible(row.label);

    row.slider.setRange(minV, maxV, (maxV - minV) / 200.0);
    row.slider.setSliderStyle(juce::Slider::LinearHorizontal);
    row.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    row.slider.setValue(initial, juce::dontSendNotification);
    row.slider.onValueChange = [&row, onChange = std::move(onChange)]() {
        onChange(static_cast<float>(row.slider.getValue()));
    };
    row.slider.onDragEnd = [this] { saveNow(); }; // a finished drag saves at once
    card_.addAndMakeVisible(row.slider);

    row.baselineValue.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    row.baselineValue.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    card_.addAndMakeVisible(row.baselineValue);
}

TargetOverrideEditor::~TargetOverrideEditor()
{
    if (isTimerRunning()) saveNow(); // closed before the last edit settled
}

void TargetOverrideEditor::saveNow()
{
    stopTimer();
    ++saves_;
    if (auto* fm = controller_.analysis().getFeedbackManager()) prefs_.saveToDatabase(*fm);
}

void TargetOverrideEditor::setRowValueForTesting(int row, double value)
{
    Row* rows[] = {&lufsRow_, &lraRow_, &widthRow_, &lowEndRow_};
    if (row >= 0 && row < 4) rows[row]->slider.setValue(value, juce::sendNotificationSync);
}

void TargetOverrideEditor::refreshLabels()
{
    lufsRow_.baselineValue.setText(
        TRANS("Genre baseline: ") + juce::String(genre_.lufs_target, 1) +
            " LUFS  ->  your target: " + juce::String(genre_.lufs_target + prefs_.getLufsOffset(), 1) + " LUFS",
        juce::dontSendNotification);

    const float lraBaseline = (genre_.lra_range_min + genre_.lra_range_max) * 0.5f;
    lraRow_.baselineValue.setText(TRANS("Genre baseline: ") + juce::String(lraBaseline, 1) + " LU  ->  your target: " +
                                      juce::String(lraBaseline + prefs_.getLraOffset(), 1) + " LU",
                                  juce::dontSendNotification);

    const float widthBaseline = (genre_.stereo_width_min + genre_.stereo_width_max) * 0.5f;
    widthRow_.baselineValue.setText(TRANS("Genre baseline: ") + juce::String(widthBaseline, 2) + "  ->  your target: " +
                                        juce::String(widthBaseline + prefs_.getStereoWidthOffset(), 2),
                                    juce::dontSendNotification);

    lowEndRow_.baselineValue.setText(
        TRANS("Genre baseline: ") + juce::String(genre_.low_end_weight, 2) +
            "  ->  your target: " + juce::String(genre_.low_end_weight + prefs_.getLowEndWeightOffset(), 2),
        juce::dontSendNotification);
}

void TargetOverrideEditor::resized()
{
    auto bounds = getLocalBounds();
    auto cardW = juce::jmin(520, bounds.getWidth() - 40);
    auto cardH = juce::jmin(340, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);

    auto b = card_.getLocalBounds().reduced(16);
    auto header = b.removeFromTop(24);
    closeButton_.setBounds(header.removeFromRight(70));
    titleLabel_.setBounds(header);
    b.removeFromTop(10);

    for (auto* row : {&lufsRow_, &lraRow_, &widthRow_, &lowEndRow_}) {
        row->label.setBounds(b.removeFromTop(16));
        row->slider.setBounds(b.removeFromTop(22));
        row->baselineValue.setBounds(b.removeFromTop(14));
        b.removeFromTop(8);
    }

    b.removeFromTop(6);
    resetButton_.setBounds(b.removeFromTop(26).removeFromLeft(200));
}

} // namespace khensu
