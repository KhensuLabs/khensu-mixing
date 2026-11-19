// desktop-app/Source/ui/VocalWorkspace/LyricIntelligibilityPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

/// Lyric-aware mixing hints: word-level transcription (faster-whisper) +
/// per-word vocal-presence-band masking check (see
/// AnalysisResult::LyricIntelligibilityData, /analyze/lyrics). Auto-runs as
/// soon as a file path is set (setFilePath) - not part of the *main*
/// analysis pipeline since it needs an optional server-side dependency and
/// can be slow, but /analyze/lyrics fails soft (503 + a clear status
/// message) when that dependency is missing, so there's no silent-surprise
/// concern in triggering it automatically. "Analyze Lyrics" stays as a
/// manual re-run button.
///
/// THREAD_CONTEXT(UIThread)
class LyricIntelligibilityPanel final : public PanelCard {
  public:
    explicit LyricIntelligibilityPanel(IAppController& c);

    /// Called whenever the active file changes so a stale transcript from a
    /// previous file isn't shown against the new one.
    void setFilePath(const juce::String& path);

    void resized() override;

  private:
    void runAnalysis();

    struct WordRow final : public juce::Component {
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent&) override
        {
            if (onClicked) onClicked();
        }
        juce::String word;
        float startSec = 0.0f, maskingRatio = 0.0f;
        bool likelyMasked = false;
        std::function<void()> onClicked;
    };

    juce::String filePath_;
    juce::TextButton analyzeButton_{TRANS("Analyze Lyrics")};
    /// Status line, error state and the run button's busy state (UX-8).
    PanelRunState run_{this, &analyzeButton_};

    /// Lyric-aligned timeline heat strip: every word as a colored segment
    /// (teal = clear, amber/red = masked) along the song timeline, so buried
    /// passages are scannable at a glance instead of only via the word list.
    /// Click to seek.
    struct HeatStrip final : public juce::Component {
        struct Seg {
            float start = 0, end = 0, ratio = 0;
            bool masked = false;
        };
        std::vector<Seg> segs;
        float duration = 0.0f;
        std::function<void(float)> onSeek;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (onSeek && duration > 0.0f) onSeek(duration * (float)e.x / (float)juce::jmax(1, getWidth()));
        }
    };
    HeatStrip heatStrip_;

    juce::Viewport wordsViewport_;
    juce::Component wordsContent_;
    std::vector<std::unique_ptr<WordRow>> rows_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricIntelligibilityPanel)
};

} // namespace khensu
