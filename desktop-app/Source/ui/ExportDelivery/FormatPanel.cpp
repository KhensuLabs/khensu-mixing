// desktop-app/Source/ui/ExportDelivery/FormatPanel.cpp
#include "FormatPanel.h"
#include "../shared/KhensuTheme.h"
#include "ExportPlan.h"
#include "../../IAppController.h"
#include <khensu/session/SessionManager.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void FormatPanel::applyThemeColours()
{
    presetLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    presetNoteLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    extrasLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
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
    presetLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::bold));
    addAndMakeVisible(presetLabel_);

    rebuildPresetBox(); // the five built-ins, then the user's presets (AUTO-2)
    presetBox_.setSelectedId(1, juce::dontSendNotification);
    presetBox_.onChange = [this] { applyPreset(presetBox_.getSelectedId()); };
    addAndMakeVisible(presetBox_);

    savePresetButton_.setTooltip(TRANS("Save the format, bit depth, sample rate, bitrate and the export loudness "
                                       "as a named preset (with a file-name suffix)."));
    savePresetButton_.onClick = [this] { promptSavePreset(); };
    addAndMakeVisible(savePresetButton_);
    deletePresetButton_.setTooltip(TRANS("Delete the selected preset of your own."));
    deletePresetButton_.onClick = [this] { deleteSelectedPreset(); };
    deletePresetButton_.setEnabled(false);
    addAndMakeVisible(deletePresetButton_);
    extrasButton_.setTooltip(TRANS("Pick more presets: one Export press then also writes a file for each, named "
                                   "after the original with the preset's suffix, beside the chosen file."));
    extrasButton_.onClick = [this] { showExtrasMenu(); };
    addAndMakeVisible(extrasButton_);
    extrasLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    extrasLabel_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(extrasLabel_);
    updateExtrasLabel();
    loadUserPresets();

    presetNoteLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
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
        lbl.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::bold));
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
    mp3WarningLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    mp3WarningLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(mp3WarningLabel_);

    outputPathLabel_.setText(TRANS("(no output path chosen)"), juce::dontSendNotification);
    outputPathLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
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
        if (presetId >= kFirstUserPresetId && presetId - kFirstUserPresetId < static_cast<int>(userPresets_.size())) {
            const auto& p = userPresets_[static_cast<size_t>(presetId - kFirstUserPresetId)];
            applyPresetSettings(p);
            presetNoteLabel_.setText(TRANS("Your preset - its extra files end in %s.").replace("%s", p.suffix),
                                     juce::dontSendNotification);
            deletePresetButton_.setEnabled(true);
            return;
        }
        presetNoteLabel_.setText({}, juce::dontSendNotification);
        break;
    }
    deletePresetButton_.setEnabled(false);
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
        deletePresetButton_.setBounds(presetRow.removeFromRight(64).reduced(0, 3));
        presetRow.removeFromRight(4);
        savePresetButton_.setBounds(presetRow.removeFromRight(104).reduced(0, 3));
        presetRow.removeFromRight(6);
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

    {
        auto extrasRow = b.removeFromTop(rowH);
        extrasButton_.setBounds(extrasRow.removeFromLeft(labelW + 30).reduced(0, 3));
        extrasRow.removeFromLeft(6);
        extrasLabel_.setBounds(extrasRow);
        b.removeFromTop(gap);
    }

    auto pathRow = b.removeFromTop(rowH);
    chooseOutputButton_.setBounds(pathRow.removeFromRight(90));
    pathRow.removeFromRight(6);
    outputPathLabel_.setBounds(pathRow);
}

// ---- AUTO-2: user presets and multi-format export ---------------------------

export_presets::Preset FormatPanel::currentAsPreset(const juce::String& name) const
{
    const auto st = getState();
    export_presets::Preset p;
    p.name = name.trim();
    p.format = st.format;
    p.bitDepth = st.bitDepth;
    p.sampleRate = st.sampleRate;
    p.mp3Bitrate = st.mp3Bitrate;
    p.loudness = loudnessKey ? loudnessKey() : juce::String();
    p.suffix = export_presets::suffixForName(p.name);
    return p;
}

void FormatPanel::applyPresetSettings(const export_presets::Preset& p)
{
    juce::TextButton* button = &wavButton_;
    FormatType type = FormatType::WAV;
    switch (p.format) {
    case AudioExportOptions::Format::AIFF: button = &aiffButton_; type = FormatType::AIFF; break;
    case AudioExportOptions::Format::FLAC: button = &flacButton_; type = FormatType::FLAC; break;
    case AudioExportOptions::Format::MP3: button = &mp3Button_; type = FormatType::MP3; break;
    case AudioExportOptions::Format::AAC: button = &aacButton_; type = FormatType::AAC; break;
    case AudioExportOptions::Format::WAV: break;
    }
    button->setToggleState(true, juce::dontSendNotification);
    selectedFormat_ = type;
    bitDepthBox_.setSelectedId(p.bitDepth == AudioExportOptions::BitDepth::Bits16   ? 1
                               : p.bitDepth == AudioExportOptions::BitDepth::Bits32 ? 3
                                                                                    : 2,
                               juce::dontSendNotification);
    sampleRateBox_.setSelectedId(p.sampleRate == AudioExportOptions::SampleRate::SR44100   ? 1
                                 : p.sampleRate == AudioExportOptions::SampleRate::SR48000 ? 2
                                 : p.sampleRate == AudioExportOptions::SampleRate::SR96000 ? 3
                                                                                           : kSourceRateId,
                                 juce::dontSendNotification);
    mp3BitrateBox_.setSelectedId(p.mp3Bitrate == AudioExportOptions::Mp3Bitrate::kbps128   ? 1
                                 : p.mp3Bitrate == AudioExportOptions::Mp3Bitrate::kbps192 ? 2
                                                                                           : 3,
                                 juce::dontSendNotification);
    if (!p.builtIn() && applyLoudnessKey) applyLoudnessKey(p.loudness);
    followFormatExtension();
    updateFormatVisibility();
    resized();
}

std::vector<export_presets::Preset> FormatPanel::allPresets() const
{
    auto all = export_presets::builtIns();
    all.insert(all.end(), userPresets_.begin(), userPresets_.end());
    return all;
}

std::vector<export_presets::Preset> FormatPanel::extraPresets() const
{
    std::vector<export_presets::Preset> out;
    for (const auto& p : allPresets())
        if (extraNames_.contains(p.name)) out.push_back(p);
    return out;
}

void FormatPanel::setUserPresetsForTesting(std::vector<export_presets::Preset> presets)
{
    userPresets_ = std::move(presets);
    rebuildPresetBox();
    updateExtrasLabel();
}

void FormatPanel::setExtraSelected(const juce::String& presetName, bool selected)
{
    if (selected)
        extraNames_.addIfNotAlreadyThere(presetName);
    else
        extraNames_.removeString(presetName);
    updateExtrasLabel();
}

void FormatPanel::rebuildPresetBox()
{
    const int selected = presetBox_.getSelectedId();
    const juce::String selectedName = selected >= kFirstUserPresetId ? presetBox_.getText() : juce::String();
    presetBox_.clear(juce::dontSendNotification);
    presetBox_.addItem(TRANS("Custom"), 1);
    for (const auto& p : export_presets::builtIns())
        presetBox_.addItem(p.name, p.builtInId);
    if (!userPresets_.empty()) presetBox_.addSectionHeading(TRANS("Your presets"));
    int restore = selected >= kFirstUserPresetId ? 1 : selected;
    for (size_t i = 0; i < userPresets_.size(); ++i) {
        const int id = kFirstUserPresetId + static_cast<int>(i);
        presetBox_.addItem(userPresets_[i].name, id);
        if (selectedName.isNotEmpty() && userPresets_[i].name == selectedName) restore = id;
    }
    if (restore > 0) presetBox_.setSelectedId(restore, juce::dontSendNotification);
    deletePresetButton_.setEnabled(restore >= kFirstUserPresetId);
    // Extras that no longer exist are dropped.
    const auto all = allPresets();
    for (int i = extraNames_.size(); --i >= 0;)
        if (std::none_of(all.begin(), all.end(), [&](const export_presets::Preset& p) { return p.name == extraNames_[i]; }))
            extraNames_.remove(i);
}

void FormatPanel::loadUserPresets()
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    juce::Component::SafePointer<FormatPanel> self(this);
    controller_.runInBackground([self, db] {
        auto r = db->getUserPreference(export_presets::kPreferenceKey);
        const juce::String json = r.is_ok() ? juce::String(r.value()) : juce::String();
        juce::MessageManager::callAsync([self, json] {
            if (self == nullptr) return; // the presets are the user's, not the session's
            self->userPresets_ = export_presets::parse(json);
            self->rebuildPresetBox();
            self->updateExtrasLabel();
        });
    });
}

void FormatPanel::storeUserPresets()
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    const std::string json = export_presets::serialize(userPresets_).toStdString();
    if (controller_.isShuttingDown()) {
        db->setUserPreference(export_presets::kPreferenceKey, json);
        return;
    }
    controller_.runInBackground([db, json] { db->setUserPreference(export_presets::kPreferenceKey, json); });
}

void FormatPanel::promptSavePreset()
{
    dialog_ = std::make_unique<juce::AlertWindow>(
        TRANS("Save export preset"),
        TRANS("Saves the format, bit depth, sample rate, MP3 bitrate and the export loudness. The suffix ends the "
              "file names a multi-format export writes with this preset."),
        juce::MessageBoxIconType::NoIcon, this);
    const juce::String current = presetBox_.getSelectedId() >= kFirstUserPresetId ? presetBox_.getText() : juce::String();
    dialog_->addTextEditor("name", current, TRANS("Name"));
    dialog_->addTextEditor("suffix", current.isNotEmpty() ? export_presets::suffixForName(current) : juce::String(),
                           TRANS("File suffix (optional)"));
    dialog_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<FormatPanel> self(this);
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self](int r) {
                                 if (self == nullptr || self->dialog_ == nullptr) return;
                                 const auto name = self->dialog_->getTextEditorContents("name").trim();
                                 const auto suffix = self->dialog_->getTextEditorContents("suffix").trim();
                                 self->dialog_.reset();
                                 if (r != 1 || name.isEmpty()) return;
                                 auto p = self->currentAsPreset(name);
                                 if (suffix.isNotEmpty()) p.suffix = export_presets::sanitizeSuffix(suffix);
                                 self->userPresets_ = export_presets::withPreset(self->userPresets_, p);
                                 self->storeUserPresets();
                                 self->rebuildPresetBox();
                                 for (size_t i = 0; i < self->userPresets_.size(); ++i)
                                     if (self->userPresets_[i].name == name)
                                         self->presetBox_.setSelectedId(kFirstUserPresetId + static_cast<int>(i),
                                                                        juce::dontSendNotification);
                                 self->deletePresetButton_.setEnabled(true);
                                 self->updateExtrasLabel();
                             }));
}

void FormatPanel::deleteSelectedPreset()
{
    if (presetBox_.getSelectedId() < kFirstUserPresetId) return;
    const auto name = presetBox_.getText();
    userPresets_ = export_presets::withoutPreset(userPresets_, name);
    extraNames_.removeString(name);
    presetBox_.setSelectedId(1, juce::dontSendNotification);
    storeUserPresets();
    rebuildPresetBox();
    updateExtrasLabel();
}

void FormatPanel::showExtrasMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Also write these with one Export press"));
    const auto all = allPresets();
    for (size_t i = 0; i < all.size(); ++i) {
        if (i == export_presets::builtIns().size()) menu.addSectionHeader(TRANS("Your presets"));
        menu.addItem(static_cast<int>(i) + 1, all[i].name + "  (" + all[i].suffix + ")", true,
                     extraNames_.contains(all[i].name));
    }
    menu.addSeparator();
    menu.addItem(1000, TRANS("None"), !extraNames_.isEmpty());
    menu.addItem(1001, TRANS("Done"));
    juce::Component::SafePointer<FormatPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&extrasButton_), [self, all](int result) {
        if (self == nullptr || result <= 0 || result == 1001) return;
        if (result == 1000) {
            self->extraNames_.clear();
        } else {
            const auto idx = static_cast<size_t>(result - 1);
            if (idx >= all.size()) return;
            self->setExtraSelected(all[idx].name, !self->extraNames_.contains(all[idx].name));
        }
        self->updateExtrasLabel();
        self->showExtrasMenu(); // stay open for the next tick
    });
}

void FormatPanel::updateExtrasLabel()
{
    const auto extras = extraPresets();
    if (extras.empty()) {
        extrasLabel_.setText(TRANS("One file. Pick more presets to write several formats at once."),
                             juce::dontSendNotification);
        return;
    }
    juce::StringArray names;
    for (const auto& p : extras)
        names.add(p.name);
    extrasLabel_.setText(TRANS("Also writes %n more: %l").replace("%n", juce::String((int)extras.size()))
                             .replace("%l", names.joinIntoString(", ")),
                         juce::dontSendNotification);
}

} // namespace khensu
