// desktop-app/Source/ui/VocalWorkspace/VocalStrategyPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include "../shared/SegmentedControl.h"
#include <khensu/analysis/AnalysisResult.h>
#include <functional>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class VocalStrategyPanel final : public PanelCard {
  public:
    explicit VocalStrategyPanel(IAppController& c);
    ~VocalStrategyPanel() override;
    void setData(const AnalysisResult& r);
    juce::String selectedStrategyId() const;

    std::function<void(const juce::String& strategyId)> onStrategyChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    SegmentedControl strategyControl_;

    static int suggestSegmentIndex(const std::string& primaryGenre) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VocalStrategyPanel)
};

} // namespace khensu
