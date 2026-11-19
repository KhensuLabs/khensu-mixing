// desktop-app/Source/ui/SessionIntake/MultitrackSessionView.h
// "Import stem folder": scans a folder of bounced stems, analyzes them as one
// multitrack session (per-track metrics + advice, track-vs-track masking) and
// hands the track list back to IntakeScreen to create the session.
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "MaskingMatrixComponent.h"
#include "MultitrackModel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace khensu {

class IBackgroundWork;

// THREAD_CONTEXT(UIThread)
class MultitrackSessionView final : public juce::Component, private juce::TableListBoxModel {
  public:
    MultitrackSessionView(IBackgroundWork& controller, const juce::File& folder);
    ~MultitrackSessionView() override;

    std::function<void()> onClose;
    /// Called with the session name and its files (mixdown first, then the
    /// named stems) when the user creates the session.
    std::function<void(const juce::String& name, const std::vector<AudioFileRef>& files)> onCreateSession;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground(juce::Graphics& g, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics& g, int row, int columnId, int w, int h, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    void startScan();
    void startAnalysis();
    void cancelAnalysis();
    void pollJob(const juce::String& jobId);
    void showScan(const multitrack::Scan& scan);
    void showAnalysis(multitrack::Analysis analysis);
    void showAdviceFor(int row);
    void setStatus(const juce::String& text, bool isError = false);
    void updateButtons();

    IBackgroundWork& controller_;
    juce::File folder_;
    std::optional<multitrack::Scan> scan_;
    std::optional<multitrack::Analysis> analysis_;
    juce::String jobId_;
    bool running_ = false;
    /// Cleared on destruction so the poll loop stops and never touches `this`.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    juce::Label titleLabel_;
    juce::Label statusLabel_;
    juce::Label mismatchLabel_;
    juce::Label matrixLabel_;
    juce::TableListBox trackTable_;
    juce::TextEditor adviceEditor_;
    MaskingMatrixComponent matrix_;
    juce::TextButton analyzeButton_{TRANS("Analyze stems")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton createButton_{TRANS("Create multitrack session")};
    juce::TextButton closeButton_{TRANS("Close")};

    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultitrackSessionView)
};

} // namespace khensu
