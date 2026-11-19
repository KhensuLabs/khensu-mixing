// plugin-copilot/Source/ui/HookIndicatorPanel.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

// Compact hook intelligence display: strength bar + count + density.
// THREAD_CONTEXT(UIThread)
class HookIndicatorPanel final : public juce::Component {
  public:
    HookIndicatorPanel();
    void paint(juce::Graphics& g) override;
    void resized() override;

    void setData(float hookStrength, int hookCount, float hookDensity) noexcept;
    void clearData() noexcept;

  private:
    float hookStrength_ = -1.0f;
    int hookCount_ = 0;
    float hookDensity_ = 0.0f;
    bool hasData_ = false;

    static void drawBadge(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& label,
                          const juce::String& value, juce::Colour accent);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HookIndicatorPanel)
};

} // namespace khensu
