// desktop-app/Source/ui/VocalWorkspace/StemMaskingPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include <khensu/ipc/ILocalServiceBridge.h>
#include <array>
#include <functional>
#include <vector>

namespace khensu {

/// Masking & Frequency Collision Detector UI: a stem-pair matrix showing
/// pairwise frequency-collision risk (0-100%) between the four separated
/// stems. Requires stems to already exist on disk (gated by the host screen
/// on StemState::Ready, same as the per-stem solo row).
///
/// THREAD_CONTEXT(UIThread)
class StemMaskingPanel final : public PanelCard {
  public:
    explicit StemMaskingPanel(IAppController& controller);

    void setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                      const juce::String& other);

    /// Surfaces which separation method produced the current stems and its
    /// quality score, so a low score can be flagged as "collision scores
    /// below may be contaminated by stem leakage" rather than presented with
    /// unwarranted confidence. Pass qualityScore < 0 if unknown (e.g. stems
    /// found on disk from a previous run, not this session).
    void setSeparationInfo(const juce::String& method, float qualityScore);

    /// Intake auto-run (spec §1.2): same as pressing the analyze button, but
    /// skipped while an analysis is already in flight (button disabled) or
    /// before stem paths have been set.
    void autoRun()
    {
        if (analyzeButton_.isEnabled() && stemPaths_[0].isNotEmpty()) runAnalysis();
    }

    void resized() override;

  private:
    void runAnalysis();
    static juce::Colour riskColour(double score01);

    juce::String stemPaths_[4]; // vocals, drums, bass, other
    juce::TextButton analyzeButton_{TRANS("Analyze Masking")};
    juce::Label statusLabel_;
    juce::Label separationInfoLabel_;

    /// CE-AI-5: the vocal's partial loudness inside the mix (Moore-Glasberg)
    /// - "buried" as a number: sones on its own vs sones left in the mix.
    void runVocalAudibility();
    juce::Label audibilityLabel_;

    struct Cell final : public juce::Component {
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent&) override
        {
            if (onClicked) onClicked();
        }
        juce::String stemA, stemB;
        double score01 = -1.0; // -1 = not yet computed
        std::vector<ipc::StemMaskingWindow> windows;
        std::function<void()> onClicked;
    };
    std::array<Cell, 6> cells_; // vocals-drums, vocals-bass, vocals-other, drums-bass, drums-other, bass-other

    /// Clicking a cell shows this pair's collision score localized to
    /// time windows instead of only the whole-track average in the cell
    /// itself - a hotspot in the chorus is invisible in a single number.
    struct CollisionTimeStrip final : public juce::Component {
        void paint(juce::Graphics& g) override;
        juce::String pairLabel;
        std::vector<ipc::StemMaskingWindow> windows;
        double totalDuration = 0.0;
    };
    CollisionTimeStrip timeStrip_;
    void selectPairForTimeStrip(const Cell& cell);

    /// Sub-second time x frequency masking heatmap for the worst-colliding
    /// pair (Gemini audit: transient-scale dynamic masking). Rows = the 10
    /// masking bands (low at bottom); columns = ~80 ms time slices; brighter/
    /// hotter = more collision at that instant and band.
    struct MaskingHeatmapView final : public juce::Component {
        void paint(juce::Graphics& g) override;
        std::vector<std::vector<float>> grid; // [frame][band]
        std::vector<double> bandsHz;
        double blockMs = 0.0;
        juce::String label;
    };
    MaskingHeatmapView heatmapView_;

    // ── Click-to-fix ─────────────────────────────────────────────────────────
    // Clicking a colliding pair doesn't just show more data - it proposes a
    // concrete DSP fix (heuristic, keyed on which two stem types collide) and
    // offers to turn that into an actual plugin-chain suggestion for the
    // higher-priority stem of the pair via the mixer chain generator.
    static juce::String suggestFix(const juce::String& stemA, const juce::String& stemB, double score01);
    void generateFixChain();

    juce::Label fixSuggestionLabel_;
    juce::TextButton generateFixButton_{TRANS("Generate Fix Chain")};
    juce::String selectedStemA_, selectedStemB_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemMaskingPanel)
};

} // namespace khensu
