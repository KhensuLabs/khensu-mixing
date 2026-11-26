// desktop-app/Source/ui/ExportDelivery/FormatPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../../export/AudioExporter.h"
#include "../shared/PanelCard.h"
#include "ExportPresets.h"
#include <functional>
#include <vector>

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

    // ---- AUTO-2: user presets and multi-format export ----
    /// The Export screen's loudness choice as a preset key (see
    /// ExportPresets.h) - read when a preset is saved - and how to put a
    /// saved key back on the screen when a preset is picked.
    std::function<juce::String()> loudnessKey;
    std::function<void(const juce::String&)> applyLoudnessKey;
    /// The current controls as a preset named `name`.
    export_presets::Preset currentAsPreset(const juce::String& name) const;
    /// Puts a preset's format, depth, rate and bitrate on the controls (and
    /// its loudness on the screen through applyLoudnessKey).
    void applyPresetSettings(const export_presets::Preset& p);
    /// The user's saved presets (sorted by name; empty until read).
    const std::vector<export_presets::Preset>& userPresets() const noexcept { return userPresets_; }
    /// The extra presets one Export press also writes (multi-format).
    std::vector<export_presets::Preset> extraPresets() const;
    /// Every preset the extras can be picked from: the built-ins, then the user's.
    std::vector<export_presets::Preset> allPresets() const;
    /// Test seam: replaces the user presets without touching the store.
    void setUserPresetsForTesting(std::vector<export_presets::Preset> presets);
    /// Test seam: ticks / unticks an extra by preset name.
    void setExtraSelected(const juce::String& presetName, bool selected);

  private:
    // Delivery presets: one pick configures format/depth/rate for a
    // destination, with the dither/normalization implications spelled out -
    // delivery mistakes are silent quality killers, so the correct settings
    // per destination shouldn't require knowing them by heart.
    juce::Label presetLabel_;
    juce::ComboBox presetBox_;
    juce::Label presetNoteLabel_;
    void applyPreset(int presetId);

    // AUTO-2.
    static constexpr int kFirstUserPresetId = 100;
    juce::TextButton savePresetButton_{TRANS("Save preset...")};
    juce::TextButton deletePresetButton_{TRANS("Delete")};
    juce::TextButton extrasButton_{TRANS("Also export...")};
    juce::Label extrasLabel_;
    std::vector<export_presets::Preset> userPresets_;
    juce::StringArray extraNames_; ///< preset names ticked for multi-format
    std::unique_ptr<juce::AlertWindow> dialog_;
    void loadUserPresets();
    void storeUserPresets();
    void rebuildPresetBox();
    void promptSavePreset();
    void deleteSelectedPreset();
    void showExtrasMenu();
    void updateExtrasLabel();

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
