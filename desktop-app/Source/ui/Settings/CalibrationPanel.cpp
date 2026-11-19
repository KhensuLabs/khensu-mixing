// desktop-app/Source/ui/Settings/CalibrationPanel.cpp
#include "CalibrationPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <cmath>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void CalibrationPanel::applyThemeColours()
{
    noiseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    noiseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    splLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    saveButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    saveButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

CalibrationPanel::CalibrationPanel(IAppController& c) : PanelCard(c, "Monitor Calibration")
{
    applyThemeColours();
    setExplainer(TRANS("Monitor Calibration"), TRANS("Calibrated monitoring is the root of consistent decisions: when a known "
                                        "signal level always produces the same SPL in the room, your ears judge "
                                        "every mix against the same reference instead of today's volume-knob mood.\n\n"
                                        "The K-System-style routine: 1) play the -20 dBFS RMS pink noise through "
                                        "your normal playback path at your normal monitor gain, one speaker at a "
                                        "time if you can; 2) read SPL at the mix position (any phone SPL app: "
                                        "C-weighted, slow); 3) enter the reading. The wizard tells you how far you "
                                        "are from the reference for your room size, and saves the profile so the "
                                        "fatigue coach and future sessions know your real monitoring level.\n\n"
                                        "References: large control room 83 dB SPL per speaker; mid room 79 dB; "
                                        "small/home room 76 dB."));

    noiseButton_.onClick = [this] { togglePinkNoise(); };
    addAndMakeVisible(noiseButton_);

    targetBox_.addItem(TRANS("Small / home room (76 dB SPL)"), 1);
    targetBox_.addItem(TRANS("Mid-size room (79 dB SPL)"), 2);
    targetBox_.addItem(TRANS("Large control room (83 dB SPL)"), 3);
    targetBox_.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(targetBox_);

    splLabel_.setText(TRANS("Measured SPL:"), juce::dontSendNotification);
    splLabel_.setFont(KhensuTheme::labelFont(10.5f));
    addAndMakeVisible(splLabel_);

    splInput_.setInputRestrictions(5, "0123456789.");
    splInput_.setTextToShowWhenEmpty(TRANS("e.g. 74"), KhensuTheme::textTertiary);
    splInput_.setFont(KhensuTheme::bodyFont(13.0f));
    addAndMakeVisible(splInput_);

    saveButton_.onClick = [this] { saveCalibration(); };
    addAndMakeVisible(saveButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(statusLabel_);

    loadSavedCalibration();
}

juce::File CalibrationPanel::generatePinkNoiseFile()
{
    // Voss-McCartney pink noise: 12 white generators updated at octave-spaced
    // intervals, summed - flat-per-octave energy, the calibration standard.
    const int sr = 44100, seconds = 30, rows = 12;
    const int n = sr * seconds;
    juce::AudioBuffer<float> buf(2, n);
    juce::Random rng;

    float rowsVal[rows] = {};
    for (int r = 0; r < rows; ++r)
        rowsVal[r] = rng.nextFloat() * 2.0f - 1.0f;
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int r = 0; r < rows; ++r)
            if ((i & ((1 << r) - 1)) == 0) rowsVal[r] = rng.nextFloat() * 2.0f - 1.0f;
        float s = 0.0f;
        for (int r = 0; r < rows; ++r)
            s += rowsVal[r];
        s /= (float)rows;
        buf.setSample(0, i, s);
        buf.setSample(1, i, s);
        sumSq += s * s;
    }
    // Normalize to -20 dBFS RMS.
    const float rms = (float)std::sqrt(sumSq / n);
    const float target = std::pow(10.0f, -20.0f / 20.0f);
    buf.applyGain(target / juce::jmax(1.0e-9f, rms));

    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_pink_noise_-20dBFS.wav");
    file.deleteFile();
    juce::WavAudioFormat fmt;
    if (std::unique_ptr<juce::OutputStream> stream = file.createOutputStream()) {
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(2).withBitsPerSample(24);
        if (auto writer = fmt.createWriterFor(stream, options)) writer->writeFromAudioSampleBuffer(buf, 0, n);
    }
    return file;
}

void CalibrationPanel::togglePinkNoise()
{
    if (noisePlaying_) {
        controller_.playback().stopAudio();
        if (restorePath_.isNotEmpty() && juce::File(restorePath_).existsAsFile())
            controller_.playback().loadAudioFileForPlayback(juce::File(restorePath_));
        noisePlaying_ = false;
        noiseButton_.setButtonText(TRANS("Play Pink Noise (-20 dBFS)"));
        return;
    }
    if (const auto* s = controller_.getActiveSession(); s != nullptr && !s->files.empty())
        restorePath_ = juce::String(s->files[0].path);

    if (!pinkFile_.existsAsFile()) {
        statusLabel_.setText(TRANS("Generating pink noise..."), juce::dontSendNotification);
        pinkFile_ = generatePinkNoiseFile();
    }
    if (!pinkFile_.existsAsFile()) {
        statusLabel_.setText(TRANS("Could not generate the noise file."), juce::dontSendNotification);
        return;
    }
    controller_.playback().loadAudioFileForPlayback(pinkFile_);
    controller_.playback().playAudio();
    noisePlaying_ = true;
    noiseButton_.setButtonText(TRANS("Stop Pink Noise"));
    statusLabel_.setText(TRANS("Noise playing - read SPL at the mix position "
                         "(C-weighted, slow), then enter it above."),
                         juce::dontSendNotification);
}

void CalibrationPanel::saveCalibration()
{
    const float measured = splInput_.getText().getFloatValue();
    if (measured < 40.0f || measured > 120.0f) {
        statusLabel_.setText(TRANS("Enter the measured SPL first (a plausible value, 40-120 dB)."),
                             juce::dontSendNotification);
        return;
    }
    const float targets[] = {76.0f, 79.0f, 83.0f};
    const float target = targets[juce::jlimit(0, 2, targetBox_.getSelectedId() - 1)];
    const float offset = measured - target;

    auto& sm = controller_.sessions().sessionManager();
    sm.setUserPreference("monitor_cal_spl", std::to_string(measured));
    sm.setUserPreference("monitor_cal_target", std::to_string(target));
    sm.setUserPreference("monitor_cal_date", juce::Time::getCurrentTime().formatted("%Y-%m-%d").toStdString());

    juce::String verdict =
        std::abs(offset) <= 1.0f
            ? "Calibrated + - you're at reference."
            : (offset > 0 ? "Trim monitor gain DOWN " + juce::String(offset, 1) + " dB to hit reference."
                          : "Bring monitor gain UP " + juce::String(-offset, 1) + " dB to hit reference.");
    statusLabel_.setColour(juce::Label::textColourId,
                           std::abs(offset) <= 1.0f ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
    statusLabel_.setText(verdict + "  Saved " + juce::String(measured, 1) + " dB SPL vs " + juce::String(juce::roundToInt(target)) +
                             " dB reference.",
                         juce::dontSendNotification);
}

void CalibrationPanel::loadSavedCalibration()
{
    auto spl = controller_.sessions().sessionManager().getUserPreference("monitor_cal_spl");
    auto date = controller_.sessions().sessionManager().getUserPreference("monitor_cal_date");
    if (spl.is_ok() && !spl.value().empty()) {
        statusLabel_.setText(TRANS("Last calibration: ") + juce::String(spl.value()) + " dB SPL" +
                                 (date.is_ok() ? " (" + juce::String(date.value()) + ")" : juce::String()) +
                                 ". Re-run after moving speakers or changing the room.",
                             juce::dontSendNotification);
    } else {
        statusLabel_.setText(TRANS("Not calibrated yet - 5 minutes now buys consistent "
                             "level judgments forever."),
                             juce::dontSendNotification);
    }
}

void CalibrationPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    noiseButton_.setBounds(b.removeFromTop(26));
    b.removeFromTop(6);
    targetBox_.setBounds(b.removeFromTop(24));
    b.removeFromTop(6);
    auto row = b.removeFromTop(24);
    splLabel_.setBounds(row.removeFromLeft(90));
    splInput_.setBounds(row.removeFromLeft(70));
    row.removeFromLeft(8);
    saveButton_.setBounds(row.removeFromLeft(130));
    b.removeFromTop(6);
    statusLabel_.setBounds(b);
}

} // namespace khensu
