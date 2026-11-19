// desktop-app/Source/services/IShell.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <string>
#include <vector>
#include "../ui/shared/MetricDiffTable.h"

namespace khensu {

struct Recommendation;
struct GuardrailRejection;
class Midi2DeviceManager;
class MacroControlRouter;

enum class ScreenType {
    Intake,
    Dashboard,
    VocalWorkspace,
    MixMaster,
    SystemBrain,
    Export,
    Copilot,
    Issues,        // appended at the end so existing ScreenType/sidebar
                      // index values used elsewhere (int casts, persisted
                      // state) don't shift.
    ReferenceLibrary, // appended after Assistant for the same reason. Its
                      // visual sidebar position (between MixMaster and
                      // SystemBrain) is decoupled from this storage order -
                      // see SidebarComponent::kVisualOrder.
    Settings,         // appended (opt-out online toggles). Visual position last.
    AiDetection       // appended; visual position right after Analysis.
};

/// App-wide detail level, independent of the per-export report-mode string
/// used by ReportPanel/generateReport(). Client Summary trims technical
/// detail (e.g. hides the Provenance Timeline); Engineer Detail embeds it.
enum class ReportMode { ClientSummary, EngineerDetail };

/// The window (ENG-1): navigation, overlays and app-wide UI state. Message
/// thread only.
class IShell {
  public:
    virtual ~IShell() = default;

    virtual void showScreen(ScreenType screen) = 0;
    /// Intake's file browser: Ctrl+O and the empty screens' "Load a track"
    /// (UX-5 / UX-11).
    virtual void browseForAudioFile() = 0;
    /// Fixes the most severe open issue in the issue queue (F, the Issues
    /// header, the Dashboard's verdict card; CON-1).
    virtual void fixTopIssue() = 0;

    virtual ReportMode getReportMode() const noexcept = 0;
    virtual void setReportMode(ReportMode mode) = 0;
    virtual void addReportModeListener(void* key, std::function<void(ReportMode)> cb) = 0;
    virtual void removeReportModeListener(void* key) = 0;

    virtual void showExplainer(const juce::String& title, const juce::String& category, const juce::String& description,
                               const juce::String& systemBrainFeatureId = {}) = 0;
    /// The "Chain Reasoning View" (Detected -> Cause -> Fix -> Trade-off ->
    /// Alternatives -> How to Verify) for one recommendation.
    virtual void showChainReasoning(const Recommendation& rec) = 0;
    virtual void showMixMasterCard(int tabIdx) = 0;
    /// A two-column metric diff modal for hosts without an overlay slot of
    /// their own (e.g. Snapshot History's compare-to-current).
    virtual void showMetricDiff(const juce::String& title, std::vector<MetricDiffTable::Row> rows,
                                const juce::String& labelA, const juce::String& labelB) = 0;
    /// The blind ABX + delta-solo overlay; needs an A/B comparison.
    /// `blindReferencePaths` makes it a hidden multi-reference trial (each
    /// trial picks one B source from the set, gain-matched as usual).
    virtual void showAbxTrial(const juce::StringArray& blindReferencePaths = {}) = 0;
    /// The reference-track modal (drop a reference, deltas, match-EQ, A/B).
    virtual void showReferenceTrackRitual() = 0;
    virtual void showOnboardingWizard() = 0;
    /// The MIDI/OSC macro-mapping window (created on first use).
    virtual void showMacroMappingWindow() = 0;
    virtual void selectSystemBrainFeature(const std::string& id) = 0;
    /// Keyboard "next issue": the Mix/Master screen, cycling unresolved
    /// recommendations by severity.
    virtual void focusNextIssue() = 0;
    /// FEAT-8: plays [start, end) seconds on a loop (A/B keeps looping it)
    /// until stopLoopRegion(), another region, or a stop.
    virtual void loopRegion(double startSec, double endSec) = 0;
    virtual void stopLoopRegion() = 0;
    virtual bool isLoopingRegion(double startSec, double endSec) const noexcept = 0;
    virtual void cycleTheme() = 0;
    /// SHIP-4: asks the release feed now (past the hourly throttle) and always
    /// reports the answer - newer release, up to date, or why it could not tell.
    virtual void checkForAppUpdates() = 0;

    /// Transient top-of-window error banner (de-duplicated).
    virtual void showServiceError(const juce::String& message) = 0;
    /// The quarantined-render prompt (AI-01): override with a typed reason.
    virtual void showGuardrailOverridePrompt(const GuardrailRejection& rejection) = 0;

    virtual Midi2DeviceManager& midi2DeviceManager() noexcept = 0;
    virtual MacroControlRouter& macroRouter() noexcept = 0;
};

} // namespace khensu
