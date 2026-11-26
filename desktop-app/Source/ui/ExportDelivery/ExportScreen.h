// desktop-app/Source/ui/ExportDelivery/ExportScreen.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ScreenHeader.h"
#include "../shared/ListenerHandle.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include <atomic>
#include <vector>
#include <khensu/analysis/AnalysisResult.h>
#include "FormatPanel.h"
#include "ReportPanel.h"
#include "AlbumAssemblyPanel.h"
#include "ProjectBoardPanel.h"
#include "MasterAssistantPanel.h"
#include "BatchRenderPanel.h"
#include "PlatformMastersPanel.h"
#include "DeliveryQaPanel.h"
#include "DeliveryPackagePanel.h"
#include "../shared/SegmentedControl.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {
class IAppController;

class ExportTargetsAdvisoryPanel final : public PanelCard, private juce::Timer {
  public:
    explicit ExportTargetsAdvisoryPanel(IAppController& c) : PanelCard(c, "ADVISORY TARGETS")
    {
        setExplainer(TRANS("Export Targets"), TRANS("Advisory loudness and true peak targets for standard platforms."));

        textBox_.setMultiLine(true);
        textBox_.setReadOnly(true);
        textBox_.setCaretVisible(false);
        textBox_.setScrollbarsShown(true);
        textBox_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        textBox_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        textBox_.setFont(
            KhensuTheme::monoFont(type::caption));
        addAndMakeVisible(textBox_);

        loadTargets();
    }

    void resized() override { textBox_.setBounds(bodyArea()); }

    void loadTargets(); // defined in ExportScreen.cpp (needs the complete AppController)
    /// BUG-2: true while the panel shows the "waiting for the engine"
    /// placeholder and watches for the engine to come up.
    bool waitingForEngine() const noexcept { return waitingForEngine_; }
    juce::String textForTesting() const { return textBox_.getText(); }

  private:
    /// BUG-2: the engine was down when the targets were asked for. Watch its
    /// status (the lifecycle's) and fetch again once it is Running, instead
    /// of keeping the placeholder until the screen is rebuilt.
    void timerCallback() override; // defined in ExportScreen.cpp
    bool waitingForEngine_ = false;
    bool loading_ = false;
    int runningRetries_ = 0; ///< failures with the engine up: retried a few times, then left
    juce::TextEditor textBox_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportTargetsAdvisoryPanel)
};

class ExportScreen final : public juce::Component, private juce::Timer {
  public:
    enum Step { kFinishStep = 0, kExportStep, kDeliveryStep, kAlbumStep, kStepCount };
    /// Shows one step's page: Finish (the Master Assistant, FEAT-2 / WF-3),
    /// Export, Delivery or Album (CON-2, FEAT-6).
    void showStep(int step);

    explicit ExportScreen(IAppController& c);
    ~ExportScreen() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

  private:
    IAppController& controller_;
    ScreenHeader titleLabel_; // UX-3: title = the sidebar label

    // ── Loudness platform preset ────────────────────────────────────────
    // Format / depth / rate / bitrate / path all live in FormatPanel - it is
    // the single source of truth (previously two conflicting format UIs
    // existed and this screen ignored the FormatPanel's choices).
    //
    // The preset list is fetched live from GET /analyze/export-targets (the
    // same platform table the advisory panel already shows) instead of a
    // hardcoded 4-bucket Streaming/Mastering/Broadcast/Custom guess - so
    // "Spotify", "Apple Music", "Club", EBU R128, ATSC A/85, podcast etc. are
    // all selectable with their real LUFS/true-peak/LRA targets, grouped by
    // category (analysis/delivery_targets.py is the one table).
    juce::ComboBox platformPresetBox_;
    bool platformChosenByUser_ = false; ///< WF-6: onboarding's platform applies until then
    std::vector<ipc::ExportTarget> loadedTargets_;
    int customPresetItemId_ = -1; // combo item id of the trailing "Custom" entry

    juce::Label gainCalloutLabel_;
    // Broadcast / podcast compliance for the selected target (POST
    // /analyze/delivery_compliance on the file being exported). Shown only
    // for targets with a pass/fail delivery window.
    juce::Label complianceLabel_;
    int complianceRequest_ = 0; // latest-wins: a slower, superseded reply is dropped
    void refreshCompliance();
    bool selectedTargetHasCompliance() const;
    juce::Slider customLufsSlider_; // visible when preset == Custom
    float targetLufsCustom_ = -14.0f;

    void loadExportTargets();
    /// Returns the currently-selected preset's LUFS target, true-peak ceiling,
    /// and (if the platform defines one) max LRA. Falls back to Custom values
    /// when no presets have loaded yet or "Custom" is selected.
    struct SelectedTarget {
        float lufs = -14.0f;
        float truePeakDbtp = -1.0f;
        juce::String name;
        bool normalize = true; ///< false = "As mastered": no gain at all (UIX-02)
        std::string id;        ///< the export-target id; empty for As mastered / Custom
        /// The true-peak ceiling the gain respects: the target's own, or a
        /// safe -1 dBTP for Custom.
        float ceiling() const noexcept { return id.empty() ? -1.0f : truePeakDbtp; }
    };
    SelectedTarget currentTarget() const;
    static constexpr int kAsMasteredId = 1000; // combo id of "As mastered (no normalization)"

    /// Everything one export carries from the pre-flight to the write.
    struct ExportJob {
        AudioExportOptions opts;
        SelectedTarget target;
        bool complianceTarget = false; ///< re-measure the written file afterwards
        bool gainKnown = false;        ///< false: normalising, but the loudness was unmeasurable
        /// The exported file's integrated loudness and true peak, when known
        /// (the extra formats' gains are planned from them).
        std::optional<std::pair<float, float>> levels;
        /// AUTO-2: the extra presets this press also writes, and their paths.
        std::vector<export_presets::Preset> extras;
        std::vector<juce::String> extraPaths;
    };
    /// The export target a preset's loudness key names (see ExportPresets.h).
    SelectedTarget targetForKey(const juce::String& key) const;
    juce::String currentLoudnessKey() const;
    void applyLoudnessKey(const juce::String& key);
    /// Writes job.extras[index...] one after the other, then reports.
    void writeExtras(ExportJob job, size_t index, juce::String status, int written);
    void startExport();
    void beginWrite(const ExportJob& job);
    void abortExport(const juce::String& message);
    /// Resets per-session export state (output path) when the session changed.
    void syncSession();
    juce::String lastSessionId_;

    // ── Steps (CON-2 / FEAT-6) ─────────────────────────────────────────
    // Export (target, format, masters, report), Delivery (QA, the package,
    // the platform table) and Album (the project board, album assembly):
    // one page each, so a step shows only its own cards.
    SegmentedControl stepStrip_;
    juce::Component finishPage_, exportPage_, deliveryPage_, albumPage_;
    int step_ = 0;

    // ── Output path panel ──────────────────────────────────────────────
    FormatPanel formatPanel_;

    // ── Report panel (right column) ────────────────────────────────────
    ReportPanel reportPanel_;
    ExportTargetsAdvisoryPanel advisoryPanel_;
    AlbumAssemblyPanel albumPanel_;
    ProjectBoardPanel projectBoard_; ///< FEAT-6
    MasterAssistantPanel masterAssistant_; ///< FEAT-2 / WF-3

    // ── Batch render (left column, under FormatPanel) ──────────────────
    BatchRenderPanel batchPanel_;
    // ── One-click platform-target master set ───────────────────────────
    PlatformMastersPanel platformMastersPanel_;
    // ── Delivery & QA toolset ──────────────────────────────────────────
    DeliveryQaPanel deliveryQaPanel_;
    // ── CE-DEL-4 one-click delivery package + content credentials ─────
    DeliveryPackagePanel deliveryPackagePanel_;

    // ── Export action ──────────────────────────────────────────────────
    juce::TextButton exportAudioButton_{TRANS("Export Audio")};

    // ── Inline progress / cancel ───────────────────────────────────────
    // shared_ptr: the export worker holds a reference, so the flag outlives
    // the screen if the app shuts down mid-export (no dangling atomic read).
    std::shared_ptr<std::atomic<bool>> cancelRequested_{std::make_shared<std::atomic<bool>>(false)};
    // Live 0..1 write progress reported by AudioExporter's block loop - the
    // Timer below polls this so the bar moves smoothly during the actual
    // (potentially many-second) export instead of jumping straight to 100%.
    std::shared_ptr<std::atomic<float>> exportProgressAtomic_{std::make_shared<std::atomic<float>>(0.0f)};
    float exportProgress_ = 0.0f;
    bool isExporting_ = false;
    juce::String exportStatusText_;
    juce::TextButton cancelButton_{TRANS("CANCEL")};

    void updateState();
    void pushDataToPanels(const AnalysisResult& r);
    void onExport();
    void updatePredictedOutput();
    void timerCallback() override;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // ENG-2: hub registrations; destroyed (deregistered) before the members
    // above them that the callbacks touch.
    ListenerHandle analysisListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportScreen)
};

} // namespace khensu
