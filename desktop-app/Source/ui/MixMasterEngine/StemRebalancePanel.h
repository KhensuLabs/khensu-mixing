// desktop-app/Source/ui/MixMasterEngine/StemRebalancePanel.h
// FEAT-4: stem rebalance. A gain per stem (vocals, drums, bass, other: Demucs
// or the session's own stems), a suggestion from the genre's stem balance,
// and a render that re-sums the stems at those gains into a new version of
// the mix (registered on the render tree, A/B'd loudness-matched like any
// fix). With every gain at 0 it is the "commit to mix" check: the residual
// to the original says whether the stems still add up to it (WF-4).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include "../shared/PanelRunState.h"
#include <array>
#include <map>

namespace khensu {

class StemRebalancePanel final : public PanelCard {
  public:
    explicit StemRebalancePanel(IAppController& c);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void resetForSession() override;
    void visibilityChanged() override;
    /// Stem paths present for the active session (tests read it too).
    std::map<std::string, std::string> stemPaths() const;

    static constexpr std::array<const char*, 4> kStems{"vocals", "drums", "bass", "other"};

  private:
    void suggest();
    void render();
    void refreshAvailability();

    std::array<juce::Slider, 4> gains_;
    std::array<juce::Label, 4> names_;
    juce::TextButton suggestButton_{TRANS("Suggest")};
    juce::TextButton renderButton_{TRANS("Render rebalance")};
    juce::TextButton resetButton_{TRANS("Reset")};
    PanelRunState run_{this, &renderButton_};
    /// The current vocal changed (WF-4): the idle line says which vocal sums.
    ListenerHandle vocalListener_;
    /// Fires after every separation: Suggest and Render wait for the stems
    /// (RUN-15 - they stayed disabled after the stems were written).
    ListenerHandle separationListener_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemRebalancePanel)
};

} // namespace khensu
