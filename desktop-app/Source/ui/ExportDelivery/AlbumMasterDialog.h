// desktop-app/Source/ui/ExportDelivery/AlbumMasterDialog.h
// Album master editor: track order, gaps / crossfades / fades, ISRCs, the
// disc's UPC/EAN and CD-Text, album-mode trims, then one render to a
// 44.1 kHz/16-bit WAV + cue sheet + DDP 2.0 fileset (service:
// /album/master/*). Opened from AlbumAssemblyPanel with its track list.
//
// Background work goes through the runners the opener passes in (bound to
// AppController::runInBackground / runLongInBackground), never detached
// threads. The album is not session data - the user picked these files - so
// results are not dropped on a session switch; a SafePointer plus the shared
// `alive_` flag stops the poll loop once the dialog closes.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AlbumSequence.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <atomic>
#include <functional>
#include <memory>

namespace khensu {

class AlbumMasterDialog final : public juce::Component, private juce::TableListBoxModel {
  public:
    using Runner = std::function<void(std::function<void()>)>;

    AlbumMasterDialog(const juce::StringArray& trackPaths, Runner shortJobs, Runner longJobs);
    ~AlbumMasterDialog() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Opens the dialog in its own window.
    static void launch(const juce::StringArray& trackPaths, Runner shortJobs, Runner longJobs);

  private:
    enum Column { colNum = 1, colTitle, colIsrc, colGap, colXfade, colFadeOut, colTrim };

    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell(int row, int col, bool selected, juce::Component* existing) override;

    juce::String cellText(int row, int col) const;
    void setCellText(int row, int col, const juce::String& text);
    void moveSelected(int delta);
    void removeSelected();
    void suggestTrims();
    void checkLayout();
    void chooseOutputDir();
    void startExport();
    void cancelExport();
    void pollJob(const juce::String& jobId);
    ipc::HttpServiceBridge::AlbumMasterSpec buildSpec() const;
    void setBusy(bool busy, const juce::String& status);
    void showLines(const juce::StringArray& lines, juce::Colour colour);
    void applyThemeColours();

    album_seq::AlbumSequence sequence_;
    Runner shortJobs_, longJobs_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    juce::Label titleLabel_, performerLabel_, catalogLabel_;
    juce::TextEditor titleEdit_, performerEdit_, catalogEdit_;
    juce::TableListBox table_{"Album tracks", this};
    juce::TextButton upButton_{TRANS("Move up")}, downButton_{TRANS("Move down")}, removeButton_{TRANS("Remove")};
    juce::TextButton trimsButton_{TRANS("Suggest album trims")}, planButton_{TRANS("Check layout")};
    juce::ToggleButton wavToggle_{TRANS("WAV")}, cueToggle_{TRANS("Cue sheet")}, ddpToggle_{TRANS("DDP 2.0")},
        cdTextToggle_{TRANS("CD-Text")};
    juce::TextButton folderButton_{TRANS("Output folder...")};
    juce::TextButton exportButton_{TRANS("Render album")}, cancelButton_{TRANS("Cancel render")};
    double progress_ = 0.0;
    juce::ProgressBar progressBar_{progress_};
    juce::Label statusLabel_;
    juce::TextEditor detailText_;

    juce::String outputDir_;
    juce::String activeJobId_;
    std::unique_ptr<juce::FileChooser> chooser_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlbumMasterDialog)
};

} // namespace khensu
