// desktop-app/Source/ui/Dashboard/RecreateSoundPanel.h
// Stem -> MIDI + timbre classification -> third-party plugin recreation recipes.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "AutoScanCompletion.h"
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <vector>

namespace khensu {
class IAppController;

class RecreateSoundPanel final : public PanelCard {
  public:
    explicit RecreateSoundPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;
    /// Dashboard auto-run (results gate): true if a scan started; `onDone`
    /// then fires once when it ends. See AutoScanCompletion.h.
    bool autoRun(std::function<void()> onDone)
    {
        // A scan already in flight queues one re-run instead of being
        // skipped (DSK-03) - see AutoScanCompletion::request().
        return autoScan_.request(std::move(onDone), [this] { runAnalysis(); },
                                 [this] { return run_.isBusy(); });
    }

    void resetForSession() override
    {
        autoScan_.reset();
        category_.clear();
        midiSummary_.clear();
        timbreSummary_.clear();
        recipes_.clear();
        run_.reset();
        resized();
        repaint();
    }

  private:
    AutoScanCompletion autoScan_;
    void runAnalysis();
    void runMidiVariation();

    struct Recipe {
        juce::String plugin, patch, chain, why;
    };

    juce::TextButton analyzeButton_{TRANS("Recreate Sound")};
    juce::TextButton midiVarButton_{TRANS("Vary / Humanize MIDI")};

    juce::Slider timingVarSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label timingVarLabel_;
    juce::Slider velocityVarSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label velocityVarLabel_;
    juce::Slider pitchVarSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::Label pitchVarLabel_;

    juce::String category_;
    juce::String midiSummary_;
    juce::String timbreSummary_;
    std::vector<Recipe> recipes_;
    /// Status line, error state and the Recreate button's busy state (UX-8).
    /// The MIDI variation run reports on the same status line.
    PanelRunState run_{this, &analyzeButton_};

    /// Re-applies every cached palette colour on this panel. Called at
    /// construction and on each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecreateSoundPanel)
};
} // namespace khensu
