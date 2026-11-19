// desktop-app/Source/ui/shared/MultiBandGoniometerComponent.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include <vector>

namespace khensu {

// Concentric multi-band vector scope.
// Visualizes phase correlation and width separately for Low, Mid, and High frequency bands.
// THREAD_CONTEXT(UIThread)
class MultiBandGoniometerComponent final : public juce::Component {
  public:
    MultiBandGoniometerComponent() = default;
    ~MultiBandGoniometerComponent() override = default;

    void setAnalysisData(const AnalysisResult& r);
    void updateRealTime(float globalWidth, float globalCorr);

    void paint(juce::Graphics& g) override;

  private:
    struct GoniometerPoint {
        float x, y;
        float alpha;
        int bandIndex; // 0 = Low, 1 = Mid, 2 = High
    };

    std::vector<GoniometerPoint> points_;

    float lowWidth_ = 0.1f, lowCorr_ = 0.95f;
    float midWidth_ = 0.5f, midCorr_ = 0.70f;
    float highWidth_ = 0.8f, highCorr_ = 0.40f;

    bool hasData_ = false;

    void generateScatter();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MultiBandGoniometerComponent)
};

} // namespace khensu
