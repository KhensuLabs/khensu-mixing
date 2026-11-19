// desktop-app/Source/ui/SessionIntake/IntakeScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/common/ThreadContext.h>
#include <khensu/session/AudioFileRef.h>
#include <atomic>
#include <map>
#include <memory>
#include "DropZoneComponent.h"
#include "IntakeQueueList.h"
#include "IntakeBatchReport.h"
#include "TargetSetupPanel.h"
#include "RecentSessionsPanel.h"
#include "TrendLinePanel.h"
#include "TargetOverrideEditor.h"
#include "MultitrackSessionView.h"
#include "IntakeQueueState.h"
#include "NativeMeterTiles.h"

namespace khensu {

class IAppController;

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class IntakeScreen final : public juce::Component {
  public:
    explicit IntakeScreen(IAppController& controller);
    ~IntakeScreen() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override; // refresh recent sessions when shown

    /// Ctrl+O / the palette's "Open audio file" (UX-11).
    void browseForFiles() { dropZone.browse(); }

    /// Onboarding's "Match a reference" goal (P0-4): switches the genre
    /// source to "Match reference track" and asks for the reference file.
    /// The session started from here carries it, and AppController loads it
    /// as the comparison reference when the session opens.
    void startMatchReference();
    /// Preselects the genre the user chose during onboarding in the Target
    /// panel (P0-4: the onboarding profile used to be read only as a flag).
    void presetGenre(const juce::String& genreId);

    /// The screenshot run's drop (diagnostics only): as if `files` were dropped.
    void dropFilesForDiagnostics(const std::vector<juce::File>& files) { onFilesChosen(files); }
    /// Create Session is enabled (the first file is analysed) - diagnostics.
    bool createSessionReady() const noexcept { return createButton.isEnabled(); }

  private:
    void onFilesChosen(const std::vector<juce::File>& files);
    void onCreateClicked();
    void onQueueReorder(int oldIndex, int newIndex);
    void onQueueCancel(int index);
    int queueIndexForPath(const juce::String& path) const;
    /// The FullIntakeRequest settings the genre-source controls select; the
    /// start and the per-stage retry must send the same ones.
    struct IntakeSettings {
        juce::String selection, statedGenre, reference;
        bool useTargets = true;
        bool confirmDetected = false;
    };
    IntakeSettings currentIntakeSettings() const;
    /// The Target panel's target plus what the genre-context controls say
    /// (P0-1): whether genre targets are on, how the genre was confirmed,
    /// and a named reference track.
    CreativeTarget buildSessionTarget() const;
    void sendGenreExample(const CreativeTarget& target);
    void refreshFullIntakeContext(const juce::String& path);
    void retrySelectedIntakeStage();
    void jumpToSelectedIntakeStage();
    void showSelectedStageStatus();
    void cancelFullIntakeJob();
    /// "Import stem folder": choose a folder, then the multitrack overlay.
    void openStemFolder();


  private:
    IAppController& controller;
    ScreenHeader titleLabel; // UX-3: title = the sidebar label
    juce::TextButton openFileButton_{TRANS("Open audio file...")}; // the header's primary (UX-3)

    std::vector<AudioFileRef> probedFiles_;
    bool haveProbedFile = false;

    // DSK-01: the queue worker reads ONLY this mutex-guarded snapshot of
    // {path, duration, cancel flag} - never probedFiles_ (message-thread
    // owned) - and stops when a newer drop starts a new generation.
    std::shared_ptr<IntakeQueueState> queueState_ = std::make_shared<IntakeQueueState>();
    // DSK-18 / WIRE-02: the full-intake job of the current drop. A new drop
    // cancels the previous job; a late bundle for another path or an older
    // generation is ignored.
    int fullIntakeGeneration_ = 0;
    juce::String fullIntakePath_;

    DropZoneComponent dropZone;
    // CE-PLT-3: native LUFS / true peak / LRA / width tiles, shown on drop.
    std::unique_ptr<NativeMeterTiles> nativeTiles_;
    IntakeQueueList queueList_;
    std::unique_ptr<TargetSetupPanel> targetPanel;
    std::unique_ptr<IntakeBatchReport> batchReport_;
    RecentSessionsPanel recentPanel;
    TrendLinePanel trendPanel;
    juce::TextButton createButton{TRANS("Create Session")};
    juce::Label genreContextLabel_;
    juce::ComboBox genreSourceCombo_;
    juce::ToggleButton genreTargetsToggle_{TRANS("Allow genre-relative targets")};
    juce::ToggleButton confirmCandidateToggle_{TRANS("I confirm the top detected candidate")};
    juce::TextEditor referencePathEditor_;
    /// The reference path the genre context last ran with: typing alone does
    /// not start a full intake; Return, leaving the field or the chooser does.
    juce::String committedReferencePath_;
    void commitReferencePath();
    juce::TextButton chooseReferenceButton_{TRANS("Choose...")};
    std::unique_ptr<juce::FileChooser> referenceChooser_;
    void chooseReferenceFile();
    juce::ComboBox intakeStageCombo_;
    juce::StringArray intakeStageKeys_; ///< service keys, parallel to the combo items
    juce::String selectedIntakeStage() const;
    juce::TextButton retryStageButton_{TRANS("Retry stage")};
    juce::TextButton openStagePanelButton_{TRANS("Open panel")};
    juce::TextButton cancelIntakeButton_{TRANS("Cancel intake")};
    juce::String activeIntakeJobId_;
    juce::var intakeStageStatus_;
    juce::var intakeStageResults_;

    // Shown only while waiting on / failing to reach the local Python
    // service - an exact retry countdown plus a manual restart action,
    // instead of createButton silently re-enabling after 15s with no
    // explanation of what happened.
    juce::Label serviceStatusLabel_;
    juce::TextButton startServiceButton_{TRANS("Start Service")};

    // Target Overrides (genre baseline + custom delta) - doubles as the
    // "Advanced Preferences" entry point since there's no separate
    // Preferences screen yet.
    juce::TextButton targetOverridesButton_{TRANS("Target Overrides")};
    std::unique_ptr<TargetOverrideEditor> targetOverrideOverlay_;

    // Multitrack sessions from a folder of bounced stems.
    juce::TextButton importStemsButton_{TRANS("Import stem folder")};
    std::unique_ptr<juce::FileChooser> stemFolderChooser_;
    std::unique_ptr<MultitrackSessionView> multitrackOverlay_;

    // ---- UX-12: progressive disclosure ----
    // Before a file: the drop zone and recent sessions. After a drop: what
    // was detected (genre, key, BPM) with one Confirm, Create Session, and
    // the target form on demand. Pipeline controls (genre source, reference,
    // stage retry, cancel, target overrides) sit under Advanced.
    void updateDisclosure();
    void updateDetectedLabel();
    bool showAdvanced_ = false;
    bool showTargetForm_ = false;
    juce::String detectedSummary_; ///< "A minor - 140 BPM" from the first analysis
    /// The genre offered for confirmation (NEW-9): from the full intake's
    /// musical_context, never filled into the target until Confirm.
    juce::String suggestedGenreId_;
    juce::String suggestedGenreText_;
    juce::TextButton advancedButton_{TRANS("Advanced")};
    juce::TextButton confirmGenreButton_{TRANS("Confirm genre")};
    juce::TextButton changeTargetButton_{TRANS("Change target...")};
    juce::Label detectedLabel_;
    juce::Label createHintLabel_;

    void refreshRecentSessions();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IntakeScreen)
};

} // namespace khensu
