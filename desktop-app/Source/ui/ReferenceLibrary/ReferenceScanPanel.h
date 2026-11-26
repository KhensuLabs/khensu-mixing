// desktop-app/Source/ui/ReferenceLibrary/ReferenceScanPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuEmptyState.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/ipc/HttpServiceBridge.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

class IAppController;
class GenreDatabase;

/// Display name of a catalog genre id or a family id ("hip-hop" -> "Hip-Hop").
juce::String referenceGenreName(const GenreDatabase& db, const juce::String& id);

/// REFS-1: the reference folder scan. "Reference folder: <path> [Choose...]
/// [Scan now]" with progress and "N songs, M samples, K packs found"; below,
/// the songs, samples or packs the scan sorted into the genre picked on the
/// screen - each with its artwork thumbnail, title, author, pack, genre /
/// subgenre chips and confidence. A song can be made the session's
/// reference; any item's genre can be corrected (a sample's for its whole
/// pack). The library is per machine, not per session.
/// THREAD_CONTEXT(UIThread)
class ReferenceScanPanel final : public juce::Component, private juce::ListBoxModel, private juce::Timer {
  public:
    explicit ReferenceScanPanel(IAppController& controller,
                                std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~ReferenceScanPanel() override;

    /// Which genre the list shows: a catalog genre, a whole family, the
    /// unsorted items, or (all empty / false) everything.
    struct GenreScope {
        juce::String genre;
        juce::String family;
        bool unsorted = false;
        bool operator==(const GenreScope& o) const
        {
            return genre == o.genre && family == o.family && unsorted == o.unsorted;
        }
    };
    void setGenreScope(const GenreScope& scope);

    /// Reads the folder, the counts and the list again, and follows a scan
    /// that is already running (a launch rescan). Called when the screen opens.
    void refresh();

    /// A scan finished (songs may have joined the reference library).
    std::function<void()> onScanFinished;
    /// The latest /references/scan/summary reply (per-genre counts for the picker).
    std::function<void(const juce::var& summary)> onSummary;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    enum class View { Songs, Samples, Packs };
    struct Row {
        juce::String id, path, title, author, album, pack, packPath, genre, family, source, kind, sampleType, key,
            artworkPath;
        float confidence = 0.0f;
        double bpm = 0.0, durationS = 0.0;
        int samples = 0, loops = 0; ///< packs view
        bool exists = true;
    };

    // juce::ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    juce::String getTooltipForRow(int row) override;
    // juce::Timer: polls a running scan
    void timerCallback() override;

    void setView(View v);
    void chooseFolder();
    void startScan();
    void cancelScan();
    void forgetFolder();
    void setRescanOnLaunch(bool on);
    void loadRootsAndSummary();
    void loadList();
    void followJob(const juce::String& jobId);
    void showProgress(const juce::var& status);
    void useSelectedAsReference();
    void correctSelectedGenre();
    void applyGenre(const Row& row, const juce::String& genre, bool wholePack);
    void updateButtons();
    void updateEmptyState();
    juce::Image thumbnailFor(const juce::String& path);
    ipc::HttpServiceBridge::ReferenceScanFilter currentFilter() const;
    static Row rowFromItem(const juce::var& item);
    static Row rowFromPack(const juce::var& pack);
    void drawChip(juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& text, juce::Colour colour) const;

    IAppController& controller_;
    std::string baseUrl_;
    View view_ = View::Songs;
    GenreScope scope_;
    juce::String rootPath_;
    juce::String jobId_;
    bool scanning_ = false;
    bool pollInFlight_ = false;
    bool listInFlight_ = false;
    bool listDirty_ = false;
    bool loadedOnce_ = false;
    int listRetries_ = 0; ///< re-asks while the engine did not answer
    int songs_ = 0, samples_ = 0, packs_ = 0;
    int listTotal_ = 0;
    double progress_ = 0.0;

    juce::Label folderCaption_;
    juce::Label folderPath_;
    juce::TextButton chooseButton_{TRANS("Choose...")};
    juce::TextButton scanButton_{TRANS("Scan now")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton forgetButton_{TRANS("Forget")};
    juce::ToggleButton rescanToggle_{TRANS("Rescan on launch")};
    juce::ProgressBar progressBar_{progress_};
    juce::Label summaryLabel_;
    juce::Label statusLabel_;
    juce::TextButton songsTab_{TRANS("Songs")};
    juce::TextButton samplesTab_{TRANS("Samples")};
    juce::TextButton packsTab_{TRANS("Packs")};
    juce::TextEditor searchEditor_;
    juce::ListBox list_;
    juce::TextButton useButton_{TRANS("Use as reference")};
    juce::TextButton genreButton_{TRANS("Correct genre...")};
    juce::TextButton revealButton_{TRANS("Show file")};
    KhensuEmptyState emptyState_;
    std::vector<Row> rows_;
    std::map<juce::String, juce::Image> thumbs_;
    std::unique_ptr<juce::FileChooser> chooser_;

    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceScanPanel)
};

} // namespace khensu
