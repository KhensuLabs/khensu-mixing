#pragma once

#include <JuceHeader.h>
#include "../timeline/IssueTimeline.h"
#include <functional>
#include <vector>

namespace khensu {

class KhensuCopilotProcessor;

/// Findings timeline + "Issues" playlist (CE-DAW-3), shown on the AI
/// Suggestions tab. Sources: this instance's rolling live analysis and, in an
/// ARA host, the analysed audio sources mapped onto their regions (CE-DAW-1).
/// Clicking an issue selects it; "Jump" (or a double-click) seeks the host
/// through ARA when available, else via the service seek / MMC Locate.
/// "Write as automation" arms CE-DAW-4; "Export" writes .mid + REAPER
/// envelopes through the service.
///
/// THREAD_CONTEXT(UIThread). Polls the timelines' generation counters at
/// 5 Hz and rebuilds only when they move.
class IssueTimelinePanel final : public juce::Component,
                                 private juce::ListBoxModel,
                                 private juce::Timer {
  public:
    explicit IssueTimelinePanel(KhensuCopilotProcessor& processor);
    ~IssueTimelinePanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /// Rebuilds from the processor now (tests; the timer does it at 5 Hz).
    void refresh();
    const std::vector<timeline::TimedIssue>& issues() const noexcept { return issues_; }
    int selectedIndex() const { return list_.getSelectedRow(); }
    void selectIssue(int index);
    /// Seeks the host to the selected issue (a little pre-roll before it).
    void jumpToSelected();
    static constexpr double kPreRollSec = 1.0;

    /// Severity colour shared with the strip: red severe, amber moderate,
    /// blue minor (the project-wide severity system).
    static juce::Colour severityColour(float severity);

  private:
    /// The horizontal findings strip with a playhead marker.
    class Strip final : public juce::Component, public juce::SettableTooltipClient {
      public:
        explicit Strip(IssueTimelinePanel& owner) : owner_(owner) {}
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void setRange(double start, double end) noexcept { start_ = start; end_ = end; }
        double xToTime(float x) const noexcept;
        float timeToX(double t) const noexcept;

      private:
        IssueTimelinePanel& owner_;
        double start_ = 0.0, end_ = 60.0;
    };

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void selectedRowsChanged(int lastRowSelected) override;
    juce::String getTooltipForRow(int row) override;

    void timerCallback() override;
    void writeSelectedAsAutomation();
    void exportSelected();
    void updateButtons();
    void setStatus(const juce::String& text, juce::Colour colour);

    KhensuCopilotProcessor& processor_;
    std::vector<timeline::TimedIssue> issues_;
    uint64_t lastLiveGeneration_ = ~uint64_t{0};
    uint64_t lastAraGeneration_ = ~uint64_t{0};

    juce::Label titleLabel_;
    juce::ToggleButton liveToggle_{"Live analysis"};
    juce::Label sourceLabel_;
    Strip strip_{*this};
    juce::ListBox list_{"Issues", this};
    juce::TextButton jumpButton_{"Jump"};
    juce::TextButton writeButton_{"Write as automation"};
    juce::TextButton exportButton_{"Export .mid / REAPER"};
    juce::TextButton clearButton_{"Clear"};
    juce::Label statusLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IssueTimelinePanel)
};

} // namespace khensu
