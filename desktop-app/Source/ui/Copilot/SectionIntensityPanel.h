// desktop-app/Source/ui/Copilot/SectionIntensityPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../IAppController.h"
#include <atomic>
#include <string>
#include <vector>

namespace khensu {

/// Section-aware processing: applies a "punch" chain (Compressor + makeup
/// Gain) whose intensity varies per detected song section instead of one
/// fixed chain for the whole track - a chorus/drop can carry full intensity
/// while a verse is left mostly or fully dry. Backed by
/// /copilot/apply_section_chain/start|status|cancel.
// THREAD_CONTEXT(UIThread)
class SectionIntensityPanel final : public juce::Component {
  public:
    explicit SectionIntensityPanel(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~SectionIntensityPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    // Test-only entry points - see MasteringAgentPanel.h's identical comment.
    // refreshSections() is normally driven by visibilityChanged(), which
    // production always triggers once the panel is added to a visible
    // window - tests construct the panel standalone, so it's called directly.
    void refreshSections();
    void apply();
    void cancelJob();
    juce::String currentJobId() const noexcept { return currentJobId_; }
    juce::String statusText() const { return statusLabel_.getText(); }
    bool isApplyButtonEnabled() const noexcept { return applyButton_.isEnabled(); }

  private:
    /// Waits for `jobId` on the long pool through the shared JobPoller
    /// (bounded consecutive errors, 404 terminal - UIX-07).
    void pollJob(const juce::String& jobId);

    IAppController& controller_;
    std::string baseUrl_;

    juce::Label headerLabel_;
    juce::Label baseIntensityLabel_{{}, TRANS("Base Intensity (chorus/drop)")};
    juce::Slider baseIntensitySlider_; // 0..1, scales the compressor ratio + makeup gain
    juce::TextButton applyButton_{TRANS("Apply Section-Aware Chain")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::Label statusLabel_;
    juce::Label guardrailLabel_;

    struct SectionRow {
        juce::Label label;            // "Section 2  (0:32-1:04)"
        juce::Slider intensitySlider; // 0..1 relative to baseIntensitySlider_
    };
    std::vector<std::unique_ptr<SectionRow>> rows_;
    /// Session + section layout the rows were built for: re-showing the panel
    /// keeps the user's sliders unless the sections really changed (UIX-19).
    juce::String rowsKey_;
    juce::Viewport rowsViewport_;
    juce::Component rowsContent_;
    void layoutRows();

    juce::String currentJobId_;
    /// Session the running job was started for (AUDIT-002); see pollJob().
    juce::String jobSessionId_;
    juce::String jobInputPath_; ///< WF-1: what the job processed
    bool jobSessionChanged() const;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SectionIntensityPanel)
};

} // namespace khensu
