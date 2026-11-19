// desktop-app/Source/ui/Dashboard/VocalInfoPanel.h
#pragma once
#include "../shared/PanelCard.h"
#include "../shared/BadgeComponent.h"
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

// Pseudo-3D fingerprint strip for the isolated vocal stem: loudness /
// brightness / dynamics, normalized to 0..1 heat bars.
class VocalFingerprintStrip : public juce::Component {
  public:
    VocalFingerprintStrip() = default;
    void setValues(float loudness0to1, float brightness0to1, float dynamics0to1)
    {
        loudness_ = loudness0to1;
        brightness_ = brightness0to1;
        dynamics_ = dynamics0to1;
        hasData_ = true;
        repaint();
    }
    void clear()
    {
        hasData_ = false;
        repaint();
    }
    void setEmptyText(const juce::String& t)
    {
        emptyText_ = t;
        repaint();
    }
    void paint(juce::Graphics& g) override;

  private:
    float loudness_ = 0.0f, brightness_ = 0.0f, dynamics_ = 0.0f;
    bool hasData_ = false;
    juce::String emptyText_;
    JUCE_DECLARE_NON_COPYABLE(VocalFingerprintStrip)
};

// Vocal info surfaced on the Dashboard (previously only visible on the Vocal
// Workspace screen, and only after manually clicking "Isolate Stems"): stem
// separation status, hook strength/density (available without stems), and -
// once the automatic intake-time separation finishes - a read on the
// isolated vocal stem itself.
class VocalInfoPanel final : public PanelCard {
  public:
    explicit VocalInfoPanel(IAppController& c);
    void setData(const AnalysisResult& r);
    /// Called via AppController::addVocalAnalysisListener whenever the vocal
    /// stem analysis (or stem-separation status) changes, independent of
    /// the main analysis pass.
    void refreshStemStatus();
    void resized() override;

  private:
    BadgeComponent stemStatusBadge_;
    BadgeComponent hookBadge_, densityBadge_;
    VocalFingerprintStrip fingerprint_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalInfoPanel)
};

} // namespace khensu
