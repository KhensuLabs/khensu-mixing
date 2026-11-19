// desktop-app/Source/ui/ExportDelivery/FormatPanel.cpp
#include "FormatPanel.h"
#include "../shared/KhensuTheme.h"
#include "ExportPlan.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void FormatPanel::applyThemeColours()
{
    presetLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    presetNoteLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    mp3WarningLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
    outputPathLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    bitDepthLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    sampleRateLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    bitrateLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
}

FormatPanel::FormatPanel(IAppController& c) : PanelCard(c, "FORMAT & OUTPUT")
{
    applyThemeColours();
    setExplainer(TRANS("Export"), TRANS("Choose the audio format, quality settings, and destination file for the export.\n\n"
                           "Delivery presets set the correct format/bit depth/sample rate per destination: "
                           "streaming services want 24-bit/44.1k WAV; CD replication requires 16-bit/44.1k "
                           "(TPDF dither is applied automatically on 16-bit exports); video post wants 48 kHz; "
                           "archives keep a hi-res FLAC. Wrong delivery settings are silent quality killers "
                           "- nobody hears them until it's pressed."));

    presetLabel_.setText(TRANS("Preset:"), juce::dontSendNotification);
    presetLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    addAndMakeVisible(presetLabel_);

    presetBox_.addItem(TRANS("Custom"), 1);
    presetBox_.addItem(TRANS("Streaming Master (WAV 24/44.1)"), 2);
    presetBox_.addItem(TRANS("CD Master (WAV 16/44.1 + dither)"), 3);
    presetBox_.addItem(TRANS("Video / Broadcast (WAV 24/48)"), 4);
    presetBox_.addItem(TRANS("Hi-Res Archive (FLAC 24/96)"), 5);
    presetBox_.addItem(TRANS("Client Preview (MP3 320)"), 6);
    presetBox_.setSelectedId(1, juce::dontSendNotification);
    presetBox_.onChange = [this] { applyPreset(presetBox_.getSelectedId()); };
    addAndMakeVisible(presetBox_);

    presetNoteLabel_.setFont(khensu::KhensuTheme::uiFont(10.5f, juce::Font::plain));
    addAndMakeVisible(presetNoteLabel_);

    wavButton_.setRadioGroupId(2001);
    wavButton_.setClickingTogglesState(true);
    wavButton_.setToggleState(true, juce::dontSendNotification);
    wavButton_.onClick = [this]() { onFormatToggle(FormatType::WAV); };
    addAndMakeVisible(wavButton_);

    aiffButton_.setRadioGroupId(2001);
    aiffButton_.setClickingTogglesState(true);
    aiffButton_.onClick = [this]() { onFormatToggle(FormatType::AIFF); };
    addAndMakeVisible(aiffButton_);

    flacButton_.setRadioGroupId(2001);
    flacButton_.setClickingTogglesState(true);
    flacButton_.onClick = [this]() { onFormatToggle(FormatType::FLAC); };
    addAndMakeVisible(flacButton_);

    mp3Button_.setRadioGroupId(2001);
    mp3Button_.setClickingTogglesState(true);
    mp3Button_.onClick = [this]() { onFormatToggle(FormatType::MP3); };
    addAndMakeVisible(mp3Button_);

    aacButton_.setRadioGroupId(2001);
    aacButton_.setClickingTogglesState(true);
    aacButton_.onClick = [this]() { onFormatToggle(FormatType::AAC); };
    addAndMakeVisible(aacButton_);

    auto setupLabel = [](juce::Label& lbl, const juce::String& text) {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    };

    setupLabel(bitDepthLabel_, TRANS("Bit Depth:"));
    addAndMakeVisible(bitDepthLabel_);

    bitDepthBox_.addItem(TRANS("16-bit"), 1);
    bitDepthBox_.addItem(TRANS("24-bit"), 2);
    bitDepthBox_.addItem(TRANS("32-bit"), 3);
    bitDepthBox_.setSelectedId(2, juce::dontSendNotification);
    addAndMakeVisible(bitDepthBox_);

    setupLabel(sampleRateLabel_, TRANS("Sample Rate:"));
    addAndMakeVisible(sampleRateLabel_);

    // "Source rate" is the default: no resampling unless a destination
    // needs a specific rate (UIX-21 - 44.1k used to resample every 48k
    // session through a non-mastering-grade ResamplingAudioSource).
    sampleRateBox_.addItem(TRANS("Source rate (no resampling)"), kSourceRateId);
    sampleRateBox_.addItem(TRANS("44100 Hz"), 1);
    sampleRateBox_.addItem(TRANS("48000 Hz"), 2);
    sampleRateBox_.addItem(TRANS("96000 Hz"), 3);
    sampleRateBox_.setSelectedId(kSourceRateId, juce::dontSendNotification);
    sampleRateBox_.setTitle(TRANS("Sample rate"));
    sampleRateBox_.setTooltip(TRANS("Keep the source rate, or resample for a destination that needs a fixed rate."));
    addAndMakeVisible(sampleRateBox_);

    setupLabel(bitrateLabel_, TRANS("Bitrate:"));
    addAndMakeVisible(bitrateLabel_);

    mp3BitrateBox_.addItem(TRANS("128 kbps"), 1);
    mp3BitrateBox_.addItem(TRANS("192 kbps"), 2);
    mp3BitrateBox_.addItem(TRANS("320 kbps"), 3);
    mp3BitrateBox_.setSelectedId(3, juce::dontSendNotification);
    addAndMakeVisible(mp3BitrateBox_);

    mp3WarningLabel_.setText(TRANS("LAME encoder not found. Place lame.exe next to app or in PATH."),
                             juce::dontSendNotification);
    mp3WarningLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    mp3WarningLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(mp3WarningLabel_);

    outputPathLabel_.setText(TRANS("(no output path chosen)"), juce::dontSendNotification);
    outputPathLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    addAndMakeVisible(outputPathLabel_);

    chooseOutputButton_.onClick = [this]() { onChooseOutput(); };
    addAndMakeVisible(chooseOutputButton_);

    updateFormatVisibility();
}

void FormatPanel::applyPreset(int presetId)
{
    auto selectFormat = [this](FormatType t, juce::TextButton& btn) {
        btn.setToggleState(true, juce::dontSendNotification);
        selectedFormat_ = t;
    };

    switch (presetId) {
    case 2: // Streaming Master
        selectFormat(FormatType::WAV, wavButton_);
        bitDepthBox_.setSelectedId(2, juce::dontSendNotification);   // 24-bit
        sampleRateBox_.setSelectedId(1, juce::dontSendNotification); // 44.1k
        presetNoteLabel_.setText(TRANS("24-bit/44.1k WAV - what every streaming "
                                 "distributor wants. Pair it with \"As mastered\" so the "
                                 "platform does its own loudness normalization."),
                                 juce::dontSendNotification);
        break;
    case 3: // CD Master
        selectFormat(FormatType::WAV, wavButton_);
        bitDepthBox_.setSelectedId(1, juce::dontSendNotification);   // 16-bit
        sampleRateBox_.setSelectedId(1, juce::dontSendNotification); // 44.1k
        presetNoteLabel_.setText(TRANS("16-bit/44.1k - TPDF dither is applied "
                                 "automatically on the bit-depth reduction (never "
                                 "truncate to 16-bit undithered)."),
                                 juce::dontSendNotification);
        break;
    case 4: // Video / Broadcast
        selectFormat(FormatType::WAV, wavButton_);
        bitDepthBox_.setSelectedId(2, juce::dontSendNotification);   // 24-bit
        sampleRateBox_.setSelectedId(2, juce::dontSendNotification); // 48k
        presetNoteLabel_.setText(TRANS("24-bit/48k - the video/broadcast standard "
                                 "rate; delivering 44.1k to post gets it resampled by "
                                 "someone who cares less than you."),
                                 juce::dontSendNotification);
        break;
    case 5: // Hi-Res Archive
        selectFormat(FormatType::FLAC, flacButton_);
        bitDepthBox_.setSelectedId(2, juce::dontSendNotification);   // 24-bit
        sampleRateBox_.setSelectedId(3, juce::dontSendNotification); // 96k
        presetNoteLabel_.setText(TRANS("24-bit/96k FLAC - lossless archive copy for "
                                 "future remasters and hi-res stores."),
                                 juce::dontSendNotification);
        break;
    case 6: // Client Preview
        selectFormat(FormatType::MP3, mp3Button_);
        mp3BitrateBox_.setSelectedId(3, juce::dontSendNotification); // 320
        sampleRateBox_.setSelectedId(kSourceRateId, juce::dontSendNotification); // capped at 48k for MP3
        presetNoteLabel_.setText(TRANS("320 kbps MP3 - small enough to text, good "
                                 "enough to judge. Never the delivery master."),
                                 juce::dontSendNotification);
        break;
    default:
        presetNoteLabel_.setText({}, juce::dontSendNotification);
        break;
    }
    followFormatExtension();
    updateFormatVisibility();
    resized();
}

void FormatPanel::followFormatExtension()
{
    // The output name follows the format, or FLAC/MP3 bytes went into the
    // "export.wav" chosen earlier (UIX-03).
    if (outputPath_.isEmpty()) return;
    outputPath_ = export_plan::withFormatExtension(outputPath_, getState().format);
    outputPathLabel_.setText(juce::File(outputPath_).getFileName(), juce::dontSendNotification);
}

void FormatPanel::resetForSession(const juce::String& sessionName)
{
    // A path chosen for session A must not silently receive session B's
    // export (UIX-04): every session starts with no path and its own name.
    sessionName_ = sessionName;
    outputPath_ = {};
    outputPathLabel_.setText(TRANS("(no output path chosen)"), juce::dontSendNotification);
    outputPathLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

void FormatPanel::updateFormatVisibility()
{
    const bool lossless = (selectedFormat_ == FormatType::WAV || selectedFormat_ == FormatType::AIFF ||
                           selectedFormat_ == FormatType::FLAC);
    const bool mp3 = (selectedFormat_ == FormatType::MP3);

    bitDepthLabel_.setVisible(lossless);
    bitDepthBox_.setVisible(lossless);
    // 32-bit (float) is WAV-only: JUCE's FLAC/AIFF writers reject it, so the
    // export always failed (AUDIT-041). Disable the item and step a 32-bit
    // pick down to 24-bit when switching away from WAV.
    const bool wav = selectedFormat_ == FormatType::WAV;
    bitDepthBox_.setItemEnabled(3, wav);
    if (!wav && bitDepthBox_.getSelectedId() == 3) bitDepthBox_.setSelectedId(2, juce::dontSendNotification);

    bitrateLabel_.setVisible(mp3);
    mp3BitrateBox_.setVisible(mp3);
    mp3WarningLabel_.setVisible(mp3 && !AudioExporter::isLameAvailable());
}

void FormatPanel::onFormatToggle(FormatType format)
{
    selectedFormat_ = format;
    followFormatExtension();
    updateFormatVisibility();
    resized();
}

void FormatPanel::onChooseOutput()
{
    const auto format = getState().format;
    const juce::String ext = export_plan::extensionFor(format);
    // Default name from the session (legal on every OS), next to the last
    // chosen file when there is one.
    const juce::File dir = outputPath_.isNotEmpty()
                               ? juce::File(outputPath_).getParentDirectory()
                               : juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save Export As..."), dir.getChildFile(export_plan::defaultFileName(sessionName_, format)), "*" + ext);

    const auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<FormatPanel> self(this);
    chooser_->launchAsync(chooserFlags, [self](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        auto result = fc.getResult();
        if (result != juce::File{}) {
            self->outputPath_ = export_plan::withFormatExtension(result.getFullPathName(), self->getState().format);
            self->outputPathLabel_.setText(juce::File(self->outputPath_).getFileName(), juce::dontSendNotification);
            self->outputPathLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        }
    });
}

FormatPanelState FormatPanel::getState() const noexcept
{
    FormatPanelState s;
    s.outputPath = outputPath_;
    switch (selectedFormat_) {
    case FormatType::MP3:
        s.format = AudioExportOptions::Format::MP3;
        break;
    case FormatType::AAC:
        s.format = AudioExportOptions::Format::AAC;
        break;
    case FormatType::AIFF:
        s.format = AudioExportOptions::Format::AIFF;
        break;
    case FormatType::FLAC:
        s.format = AudioExportOptions::Format::FLAC;
        break;
    default:
        s.format = AudioExportOptions::Format::WAV;
        break;
    }

    switch (bitDepthBox_.getSelectedId()) {
    case 1:
        s.bitDepth = AudioExportOptions::BitDepth::Bits16;
        break;
    case 3:
        s.bitDepth = AudioExportOptions::BitDepth::Bits32;
        break;
    default:
        s.bitDepth = AudioExportOptions::BitDepth::Bits24;
        break;
    }
    if (!AudioExportOptions::supportsBitDepth(s.format, s.bitDepth)) s.bitDepth = AudioExportOptions::BitDepth::Bits24;
    switch (sampleRateBox_.getSelectedId()) {
    case 1:
        s.sampleRate = AudioExportOptions::SampleRate::SR44100;
        break;
    case 2:
        s.sampleRate = AudioExportOptions::SampleRate::SR48000;
        break;
    case 3:
        s.sampleRate = AudioExportOptions::SampleRate::SR96000;
        break;
    default:
        s.sampleRate = AudioExportOptions::SampleRate::Source;
        break;
    }
    switch (mp3BitrateBox_.getSelectedId()) {
    case 1:
        s.mp3Bitrate = AudioExportOptions::Mp3Bitrate::kbps128;
        break;
    case 2:
        s.mp3Bitrate = AudioExportOptions::Mp3Bitrate::kbps192;
        break;
    default:
        s.mp3Bitrate = AudioExportOptions::Mp3Bitrate::kbps320;
        break;
    }
    return s;
}

void FormatPanel::resized()
{
    auto b = bodyArea();
    const int rowH = 34;
    const int labelW = 90;
    const int gap = 8;

    // Delivery preset row
    {
        auto presetRow = b.removeFromTop(rowH);
        presetLabel_.setBounds(presetRow.removeFromLeft(labelW));
        presetBox_.setBounds(presetRow);
        b.removeFromTop(2);
        presetNoteLabel_.setBounds(b.removeFromTop(26));
        b.removeFromTop(gap);
    }

    // Format toggle
    auto fmtRow = b.removeFromTop(rowH);
    for (auto* btn : {&wavButton_, &aiffButton_, &flacButton_, &mp3Button_, &aacButton_}) {
        btn->setBounds(fmtRow.removeFromLeft(56));
        fmtRow.removeFromLeft(5);
    }
    b.removeFromTop(gap);

    if (bitDepthBox_.isVisible()) {
        auto bdRow = b.removeFromTop(rowH);
        bitDepthLabel_.setBounds(bdRow.removeFromLeft(labelW));
        bitDepthBox_.setBounds(bdRow);
        b.removeFromTop(gap);
    }

    {
        auto srRow = b.removeFromTop(rowH);
        sampleRateLabel_.setBounds(srRow.removeFromLeft(labelW));
        sampleRateBox_.setBounds(srRow);
        b.removeFromTop(gap);
    }

    if (selectedFormat_ == FormatType::MP3) {
        auto brRow = b.removeFromTop(rowH);
        bitrateLabel_.setBounds(brRow.removeFromLeft(labelW));
        mp3BitrateBox_.setBounds(brRow);
        b.removeFromTop(gap);

        if (mp3WarningLabel_.isVisible()) {
            mp3WarningLabel_.setBounds(b.removeFromTop(rowH));
            b.removeFromTop(gap);
        }
    }

    auto pathRow = b.removeFromTop(rowH);
    chooseOutputButton_.setBounds(pathRow.removeFromRight(90));
    pathRow.removeFromRight(6);
    outputPathLabel_.setBounds(pathRow);
}

} // namespace khensu
