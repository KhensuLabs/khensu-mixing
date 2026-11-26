// desktop-app/Source/ui/ExportDelivery/ProjectBoardPanel.h
// FEAT-6: the project or album this track belongs to, as a readiness board -
// every track with its loudness, true peak, readiness score and which master
// the album uses (the accepted render, or the mix). Picks the track's project
// (or makes one), and opens another track's session with a click.
// ENH-4: bulk actions over the project's tracks, one track at a time with
// progress and Cancel - Analyse all (each session opened and analysed),
// Fix all safe + master all (the per-file smart batch, then each result
// registered as that track's new version) and Export all.
// THREAD_CONTEXT(UIThread)
#pragma once
#include "AlbumConsistencyView.h"
#include <JuceHeader.h>
#include "../../session/ProjectTracks.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "FormatPanel.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace khensu {

class IAppController;

class ProjectBoardPanel final : public PanelCard, private juce::Timer {
  public:
    explicit ProjectBoardPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void visibilityChanged() override;
    void resetForSession() override { refresh(); }

    /// Re-reads the projects and this track's project (off the message thread).
    void refresh();
    /// The rows shown (tests read it).
    const std::vector<project_tracks::Track>& tracks() const noexcept { return snapshot_.tracks; }

    // ---- ENH-4: bulk actions over the project's tracks ----
    /// How "Fix all safe + master all" masters (a Master Assistant set can
    /// pass its style, strength, loudness and ceiling).
    struct MasterOptions {
        bool fix = true;
        juce::String style{"balanced"};
        double strength = 0.5;
        double loudnessOffsetDb = 0.0;
        std::optional<double> ceilingDbtp;
        juce::String label; ///< e.g. the macro set's name, for the status line
    };
    void analyseAll();
    // Two overloads, not "= {}": clang rejects a default argument that needs
    // MasterOptions' member initializers inside the enclosing class.
    void fixAndMasterAll();
    void fixAndMasterAll(const MasterOptions& options);
    void exportAll();
    void cancelBulk();
    bool bulkRunning() const noexcept { return bulk_ != Bulk::None; }
    /// The Export step's format (FormatPanel); WAV 24-bit when unset.
    std::function<FormatPanelState()> exportFormat;
    juce::String bulkStatusText() const { return bulkLabel_.getText(); }

  private:
    void publish(project_tracks::Snapshot snap, const std::string& forSession);
    void rebuildProjectBox();
    void onProjectChosen();
    void promptNewProject();
    void promptRename();
    /// Runs a project change off the message thread, then tells every listener.
    void change(std::function<void(SessionManager&)> write);
    std::string activeSessionId() const;
    juce::Rectangle<int> listArea() const;
    int rowAt(juce::Point<int> p) const;

    static constexpr int kNoProjectId = 1;
    static constexpr int kFirstProjectId = 100;
    static constexpr int kRowH = 22;

    juce::Label projectLabel_;
    juce::ComboBox projectBox_;
    juce::TextButton newButton_{TRANS("New project...")};
    juce::TextButton renameButton_{TRANS("Rename...")};
    juce::Label statusLabel_;

    // ENH-4 bulk actions.
    enum class Bulk { None, Analyse, FixMaster, Register, Export };
    struct RegisterItem {
        std::string sessionId;
        juce::String inputPath, outputPath, summary;
    };
    juce::TextButton analyseAllButton_{TRANS("Analyse all")};
    juce::TextButton fixMasterAllButton_{TRANS("Fix all safe + master all")};
    juce::TextButton exportAllButton_{TRANS("Export all...")};
    juce::TextButton cancelBulkButton_{TRANS("Cancel")};
    juce::Label bulkLabel_;
    Bulk bulk_ = Bulk::None;
    std::vector<project_tracks::Track> bulkTracks_;
    size_t bulkIndex_ = 0;
    bool bulkLoaded_ = false;
    double bulkStepStartMs_ = 0.0;
    std::string bulkReturnSession_;
    std::vector<RegisterItem> registerQueue_;
    juce::String smartJobId_;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    std::map<std::string, juce::String> lastResult_; ///< session id -> the last bulk run's outcome
    std::unique_ptr<juce::FileChooser> exportChooser_;
    bool startBulk(Bulk kind);
    void finishBulk(const juce::String& text);
    void updateBulkButtons();
    void timerCallback() override;
    void tickAnalyse();
    void tickRegister();
    void pollSmartBatch(const juce::String& jobId);
    void onSmartBatchFinished(const ipc::HttpServiceBridge::SmartBatchStatus& st);
    void exportNext(juce::File dir, FormatPanelState format);
    static juce::String describeResult(const ipc::HttpServiceBridge::SmartBatchFileResult& r);

    project_tracks::Snapshot snapshot_;
    /// VIZ-N2: the project's tracks on one chart (loudness and tone).
    AlbumConsistencyView album_;
    int albumRequest_ = 0;
    void refreshAlbum();
    int request_ = 0; ///< latest-wins
    int hoverRow_ = -1;
    std::unique_ptr<juce::AlertWindow> dialog_;

    void applyThemeColours();

    // After everything the callbacks touch.
    ListenerHandle projectsListener_;
    ListenerHandle analysisListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectBoardPanel)
};

} // namespace khensu
