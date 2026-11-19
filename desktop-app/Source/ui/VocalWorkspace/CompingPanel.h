// desktop-app/Source/ui/VocalWorkspace/CompingPanel.h
// Comping Assistant: load the takes, get per-phrase scores (pitch / level
// / tone) and a proposed comp, render it with crossfades and A/B it.
// Modern vocal production comps per-phrase; this computes the best-of
// grid instead of eyeballing waveforms.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include <vector>

namespace khensu {

class IAppController;

class CompingPanel final : public PanelCard {
  public:
    explicit CompingPanel(IAppController& c);
    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void chooseTakes();
    void runAnalysis(bool render);
    /// The status line while no takes are loaded here: what the session's bin holds.
    void showBinStatus();

    struct Phrase {
        float start = 0.0f, end = 0.0f;
        std::vector<float> totals; // score per take
        int best = 0;
    };

    juce::TextButton chooseButton_{TRANS("Load Takes...")};
    juce::TextButton renderButton_{TRANS("Render Comp")};
    juce::Label statusLabel_;

    juce::StringArray takePaths_;
    std::vector<Phrase> phrases_;
    int takeCount_ = 0;

    std::unique_ptr<juce::FileChooser> chooser_;
    ListenerHandle takesListener_; ///< the session's Takes bin (WF-5)
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompingPanel)
};

} // namespace khensu
