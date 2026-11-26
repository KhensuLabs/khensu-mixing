// desktop-app/Source/ui/Dashboard/TranslationPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/ChartStyle.h"
#include <khensu/ui/KhensuPalette.h>
#include "../shared/MeterBarComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class TranslationPanel final : public PanelCard {
  public:
    explicit TranslationPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void resized() override;

  private:
    void updateView();

    MeterBarComponent studioMeter_, earbudsMeter_, phoneMeter_, carMeter_, clubMeter_;

    juce::TextButton reportToggle_{TRANS("Client Summary")};
    bool clientMode_ = false;

    // Audition-through-device: renders the mix through a device simulation
    // (server-side) and swaps playback to the result - translation is
    // something you hear, not just a predicted risk number.
    juce::TextButton hearPhoneButton_{TRANS("Hear: Phone")};
    juce::TextButton hearEarbudsButton_{TRANS("Hear: Buds")};
    juce::TextButton hearCarButton_{TRANS("Hear: Car")};
    juce::TextButton hearClubButton_{TRANS("Hear: Club")};
    juce::TextButton hearOriginalButton_{TRANS("Original")};
    juce::Label deviceNoteLabel_;
    void auditionDevice(const juce::String& device);
    void restoreOriginal();

    static constexpr int kNumEnvs = 5;
    static constexpr int kNumBands = 3;
    float riskMatrix_[kNumEnvs][kNumBands]{};
    juce::Rectangle<int> matrixArea() const;
    juce::Rectangle<int> cellRect(int env, int band) const;
    juce::String matrixSummary() const;
    juce::Point<int> hoverPos_{-1, -1};
    ui::chart::ChartSummary matrixSummary_; ///< VIS-8

    juce::Label summaryHeaderLabel_;
    juce::Label summaryBodyLabel_;

    AnalysisResult lastResult_;
    bool hasResult_ = false;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranslationPanel)
};

} // namespace khensu
