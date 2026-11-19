// desktop-app/Source/ui/ExportDelivery/FormatPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../../export/AudioExporter.h"
#include "../shared/PanelCard.h"

namespace khensu {
class IAppController;

struct FormatPanelState {
    juce::String outputPath;
    AudioExportOptions::Format format = AudioExportOptions::Format::WAV;
    AudioExportOptions::BitDepth bitDepth = AudioExportOptions::BitDepth::Bits24;
    AudioExportOptions::SampleRate sampleRate = AudioExportOptions::SampleRate::Source;
    AudioExportOptions::Mp3Bitrate mp3Bitrate = AudioExportOptions::Mp3Bitrate::kbps320;
};

class FormatPanel final : public PanelCard {
  public:
    explicit FormatPanel(IAppController& c);
    void resized() override;
    FormatPanelState getState() const noexcept;
    /// Clears the output path and remembers the session's name for the
    /// default file name. ExportScreen calls it whenever the session changes.
    void resetForSession(const juce::String& sessionName);

    enum class FormatType { WAV, AIFF, FLAC, MP3, AAC };

  private:
    // Delivery presets: one pick configures format/depth/rate for a
    // destination, with the dither/normalization implications spelled out -
    // delivery mistakes are silent quality killers, so the correct settings
    // per destination shouldn't require knowing them by heart.
    juce::Label presetLabel_;
    juce::ComboBox presetBox_;
    juce::Label presetNoteLabel_;
    void applyPreset(int presetId);

    juce::TextButton wavButton_{TRANS("WAV")};
    juce::TextButton aiffButton_{TRANS("AIFF")};
    juce::TextButton flacButton_{TRANS("FLAC")};
    juce::TextButton mp3Button_{TRANS("MP3")};
    juce::TextButton aacButton_{TRANS("AAC")};
    juce::Label bitDepthLabel_, sampleRateLabel_, bitrateLabel_;
    juce::ComboBox bitDepthBox_, sampleRateBox_, mp3BitrateBox_;
    juce::Label mp3WarningLabel_;
    juce::TextButton chooseOutputButton_{TRANS("Choose...")};
    juce::Label outputPathLabel_;

    juce::String outputPath_;
    juce::String sessionName_;
    static constexpr int kSourceRateId = 4;
    void followFormatExtension();
    FormatType selectedFormat_ = FormatType::WAV;
    std::unique_ptr<juce::FileChooser> chooser_;

    void onFormatToggle(FormatType format);
    void onChooseOutput();
    void updateFormatVisibility();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FormatPanel)
};

} // namespace khensu
