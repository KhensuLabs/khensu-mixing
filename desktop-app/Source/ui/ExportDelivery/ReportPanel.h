// desktop-app/Source/ui/ExportDelivery/ReportPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include "../shared/SegmentedControl.h"
#include "../shared/ListenerHandle.h"

namespace khensu {
class IAppController;

class ReportPanel final : public PanelCard {
  public:
    explicit ReportPanel(IAppController& c);
    void resized() override;

  private:
    juce::Label modeLabel_;
    SegmentedControl modeControl_;

    juce::Label outputDirLabel_;
    juce::Label outputPathDisplay_;
    juce::TextButton chooseOutputButton_{TRANS("Browse...")};
    juce::TextButton generateButton_{TRANS("Generate HTML")};
    /// CE-TRU-3: the evidence trail as a client-readable HTML page + JSON.
    juce::TextButton evidenceButton_{TRANS("Export evidence report")};
    juce::TextButton quickExportButton_{TRANS("Quick Export")};
    juce::Label statusLabel_;
    juce::TextButton openButton_{TRANS("Open in Browser")};
    juce::TextButton savePdfButton_{TRANS("Save as PDF")};
    /// VIZ-N5: one self-contained HTML page with the key charts.
    juce::TextButton visualButton_{TRANS("Save visual report...")};
    void onSaveVisual();
    void writeVisual(const juce::File& file);
    /// STU-4: the sections and timestamped findings as DAW markers (CSV,
    /// Reaper regions / markers, or a MIDI file with marker events).
    juce::TextButton markersButton_{TRANS("DAW markers...")};
    void onExportMarkers();
    void chooseMarkersFile(int format);
    void writeMarkers(const juce::File& file, int format);
    juce::Label pdfStatusLabel_;

    juce::String reportOutputPath_;
    std::unique_ptr<juce::FileChooser> chooser_;

    void onGenerate();
    void onChooseOutput();
    void onOpen();
    void onSavePdf();
    void onQuickExport();
    void onExportEvidence();

    /// Re-applies the panel's resting palette colours (not the transient
    /// success/failure status tints - see the .cpp note).
    void applyThemeColours();

    /// UX-9: the report mode follows the app-wide detail level (Engineer
    /// Detail in the top bar); changing it here changes that one setting.
    ListenerHandle reportModeListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReportPanel)
};

} // namespace khensu
