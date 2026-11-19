// desktop-app/Source/ui/VocalWorkspace/StemEnergyPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include <array>

namespace khensu {

/// RMS energy share across the four separated stems (vocals/drums/bass/
/// other), read directly off the stem .wav files on a background thread
/// (juce::AudioFormatReader) -- no backend call needed, the files already
/// exist once separation completes. Shows the mix balance at a glance
/// instead of requiring the user to solo each stem and judge loudness by
/// ear.
///
/// THREAD_CONTEXT(UIThread)
class StemEnergyPanel final : public PanelCard {
  public:
    explicit StemEnergyPanel(IAppController& controller);

    void setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                      const juce::String& other);

    void resized() override;
    void paint(juce::Graphics& g) override;

  private:
    void computeEnergy();

    juce::String stemPaths_[4]; // vocals, drums, bass, other
    static constexpr const char* kLabels[4] = {"VOCALS", "DRUMS", "BASS", "OTHER"};
    static constexpr juce::uint32 kColours[4] = {0xffff5599, 0xffffaa55, 0xff55ccff, 0xff88cc55};

    std::array<float, 4> rmsShare_{0.0f, 0.0f, 0.0f, 0.0f}; // normalized 0..1, sums to 1
    bool haveData_ = false;
    juce::String lastComputedKey_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemEnergyPanel)
};

} // namespace khensu
