// desktop-app/Source/ui/SessionIntake/RecurringIssuesPanel.h
// AUTO-5: "Recurring issues in your mixes" on Intake. Lists the measured
// findings (/agent/findings) that come back across the user's recent tracks
// (/agent/sessions): how many tracks, when last seen and whether it is
// getting more or less frequent. A row opens the newest session with it.
// Each file's findings are kept for the life of the app (keyed on its size
// and modification time), so only new or changed tracks are measured again.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "RecurringIssues.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

class RecurringIssuesPanel final : public juce::Component, private juce::Timer {
  public:
    /// Runs a job on the app's long background pool (it measures files).
    using Runner = std::function<void(std::function<void()>)>;

    explicit RecurringIssuesPanel(Runner runLong, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~RecurringIssuesPanel() override;

    /// True when a scan may start now: the engine is up and no track is
    /// being analysed (this is a low-priority job). Unset: always.
    std::function<bool()> canRunNow;
    /// A row was chosen: open that session.
    std::function<void(const std::string& sessionId)> onOpenSession;

    /// Starts a scan unless one is running; `force` ignores the last result's age.
    void refresh(bool force = false);

    /// The tracks a scan looks at, at most.
    static constexpr int kMaxTracks = 10;
    /// Rows shown, at most.
    static constexpr int kMaxRows = 5;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    /// Tests: show these results as a finished scan would.
    void setResultsForTesting(std::vector<recurring_issues::Recurring> rows, int scanned);
    int rowCountForTesting() const { return static_cast<int>(rowButtons_.size()); }
    juce::String statusForTesting() const { return status_; }

  private:
    class RowButton;
    void timerCallback() override;
    void showResults(std::vector<recurring_issues::Recurring> rows, int scanned, int failed);

    Runner runLong_;
    std::string baseUrl_;
    juce::String status_;
    bool running_ = false;
    bool loadedOnce_ = false;
    juce::int64 lastRunMs_ = 0;
    std::vector<recurring_issues::Recurring> rows_;
    std::vector<std::unique_ptr<RowButton>> rowButtons_;
    juce::TextButton refreshButton_{TRANS("Refresh")};
    /// Bumped by each scan, so a late progress message of an older one is dropped.
    int generation_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecurringIssuesPanel)
};

} // namespace khensu
