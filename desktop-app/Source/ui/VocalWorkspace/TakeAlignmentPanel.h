// desktop-app/Source/ui/VocalWorkspace/TakeAlignmentPanel.h
// Doubles & ad-lib timing: align double/ad-lib takes to the lead vocal, or
// quantize them to a tempo grid, via POST /analyze/align_takes. Shows each
// take's per-segment shift as a bar strip and the before/after timing error;
// rendered takes can be auditioned through the transport A/B.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "TakeAlignmentModel.h"

namespace khensu {

class IAppController;

class TakeAlignmentPanel final : public PanelCard {
  public:
    explicit TakeAlignmentPanel(IAppController& c);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    juce::String referencePath() const;
    void chooseTakes();
    /// Up to 8 of `chosen` become the takes to align.
    void useTakes(const juce::StringArray& chosen);
    void run();
    void auditionSelected();

    juce::StringArray takePaths_;
    TakeAlignmentResult result_;
    /// Session the aligned takes were rendered for; auditioning one registers
    /// it into that session only (AUDIT-002).
    juce::String resultSessionId_;
    std::unique_ptr<juce::FileChooser> chooser_;

    juce::TextButton chooseButton_{TRANS("Add Takes...")};
    juce::ComboBox modeBox_;
    juce::ComboBox gridBox_;
    juce::Slider strengthSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::TextButton runButton_{TRANS("Align & Render")};
    juce::ComboBox auditionBox_;
    juce::Label statusLabel_;

    void applyThemeColours();
    // After the controls its callback touches.
    ListenerHandle takesListener_; ///< the session's Takes bin (WF-5)

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeAlignmentPanel)
};

} // namespace khensu
