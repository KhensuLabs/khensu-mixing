// desktop-app/Source/ui/Copilot/MasteringAgentPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include <atomic>
#include <string>
#include <vector>

namespace khensu {

/// Iterative self-correcting mastering loop: render -> analyze -> critique ->
/// adjust, repeated toward a genre's numeric targets (backed by
/// /copilot/mastering_agent/start|status|cancel). Each iteration is shown as
/// its own card - what changed, why, and the resulting metric deltas - so
/// the user watches the agent converge instead of getting only a final file.
// THREAD_CONTEXT(UIThread)
class MasteringAgentPanel final : public juce::Component {
  public:
    explicit MasteringAgentPanel(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~MasteringAgentPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    /// WF-3: selects the session's genre (the combo started at the first
    /// genre in the list). The Copilot screen calls it each time it shows.
    void syncToSessionGenre();
    /// The genre id the combo selects (tests).
    std::string selectedGenreIdForTesting() const;

    // Test-only entry points (KhensuTests has no way to reach these buttons -
    // they're private members with no accessor - so the action methods
    // themselves are public instead of adding a friend/accessor seam per
    // widget). Production code still only calls these via the button
    // onClick lambdas wired in the constructor.
    void run();
    void cancelRun();
    void acceptCandidate();
    void rejectCandidate();
    juce::String currentJobId() const noexcept { return currentJobId_; }
    juce::String statusText() const { return statusLabel_.getText(); }
    bool isRunButtonEnabled() const noexcept { return runButton_.isEnabled(); }

  private:
    /// Waits for `jobId` on the long pool through the shared JobPoller
    /// (bounded consecutive errors, 404 terminal - UIX-07), then hands the
    /// terminal status to onJobFinished on the message thread.
    void pollJob(const juce::String& jobId);
    void onJobFinished(const ipc::HttpServiceBridge::MasteringJobStatus& status);
    void rebuildIterationCards();

    IAppController& controller_;
    std::string baseUrl_;

    juce::Label genreLabel_{{}, TRANS("Genre")};
    juce::ComboBox genreBox_;
    juce::Label iterLabel_{{}, TRANS("Max Iterations")};
    juce::Slider iterSlider_;
    juce::TextButton runButton_{TRANS("Run Mastering Agent")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton acceptCandidateButton_{TRANS("Accept A/B Candidate")};
    juce::TextButton rejectCandidateButton_{TRANS("Reject Candidate")};
    juce::Label statusLabel_;
    juce::Label guardrailLabel_;

    /// Line chart of measured LUFS/crest_db across iterations (from each
    /// MasteringIteration's own deltas.<key>.measured), so convergence is
    /// visible as a trend instead of only readable by comparing each card's
    /// text metrics one at a time.
    struct IterationChartComponent final : public juce::Component {
        std::vector<float> lufsSeries, crestSeries;
        void paint(juce::Graphics& g) override;
    };
    IterationChartComponent iterationChart_;

    /// Companion to the line chart: each iteration's (lufs_delta, crest_delta)
    /// from its own genre target, plotted as a connected trajectory
    /// converging toward the origin (on-target) -- a 2D stand-in for "moving
    /// toward the reference-library target across iterations" scoped to
    /// this panel's already-fetched deltas rather than wiring a cross-screen
    /// point (the 3D reference cloud it fed was removed, CUT-6).
    struct ConvergenceTrajectoryComponent final : public juce::Component {
        std::vector<juce::Point<float>> deltas; // (lufs_delta, crest_delta) per iteration
        void paint(juce::Graphics& g) override;
    };
    ConvergenceTrajectoryComponent convergenceChart_;

    juce::Viewport cardsViewport_;
    juce::Component cardsContent_;

    struct IterationCard : public juce::Component {
        juce::Label headerLabel_;   // "Iteration 2 - converged" etc.
        juce::Label critiqueLabel_; // "why" text
        juce::Label metricsLabel_;  // measured vs target deltas
        IterationCard()
        {
            addAndMakeVisible(headerLabel_);
            addAndMakeVisible(critiqueLabel_);
            addAndMakeVisible(metricsLabel_);
        }
        void paint(juce::Graphics& g) override;
        void resized() override;
        /// Cards are built at runtime, so each re-tints itself (UIX-22).
        void applyColours();
        bool converged_ = false;
        // Declared last - see ThemeListener's note on member ordering.
        ThemeListener themeListener_{[this] {
            applyColours();
            repaint();
        }};
    };
    std::vector<std::unique_ptr<IterationCard>> cards_;
    void layoutCards();

    juce::String currentJobId_;
    /// Session the running job was started for (captured in run(), AUDIT-002):
    /// a poll that finds another session active cancels the job instead of
    /// loading its candidate into the new session's A/B.
    juce::String jobSessionId_;
    /// What the running job processes (WF-1: the current version or the
    /// original); an accepted candidate is registered as its child.
    juce::String jobInputPath_;
    bool jobSessionChanged() const;
    juce::String pendingCandidatePath_;
    juce::String pendingCandidateSessionId_;
    juce::String pendingCandidateInputPath_;
    juce::String pendingCandidateDecisionHash_; // the agent run's decision_hash
    /// {output_path, decision_hash?} - the /events payload must be an object.
    juce::var candidateEventPayload() const;
    /// Stops the running JobPoller (Cancel / destruction); it then cancels
    /// the job server-side.
    std::shared_ptr<std::atomic<bool>> pollStop_;
    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasteringAgentPanel)
};

} // namespace khensu
