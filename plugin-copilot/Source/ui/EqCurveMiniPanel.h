#pragma once
#include <JuceHeader.h>
#include <khensu/ui/EqCurveComponent.h>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

class KhensuCopilotProcessor;

/// "Visuals" tab, phase 1 of VST3/Dashboard panel parity: hosts the shared
/// EqCurveComponent (also used by desktop-app's MixMasterScreen/EqCurvePanel/
/// ReferenceComparePanel) bound to the plugin's own 3-band corrective EQ
/// (ParamIDs::eq*/eq2*/eq3*). Bidirectional: a host automating those params
/// updates the curve here (polled at low rate, since APVTS parameters don't
/// push change notifications to arbitrary listeners without a dedicated
/// AudioProcessorParameter::Listener per param); dragging a band here writes
/// straight back into the APVTS, same as any other host-automatable control.
class EqCurveMiniPanel final : public juce::Component, private juce::Timer {
  public:
    explicit EqCurveMiniPanel(KhensuCopilotProcessor& proc);
    ~EqCurveMiniPanel() override;

    void resized() override;
    /// The 8 Hz APVTS poll only runs while this tab is shown (hidden-tab
    /// timers are paused, audit 12.3).
    void visibilityChanged() override;

    /// Pushes non-band context (spectrum underlay, resonance markers, genre
    /// reference curve) from a freshly-loaded analysis snapshot. Band values
    /// themselves always come from the APVTS (see class comment), not here.
    void pushAnalysis(const AnalysisResult& analysis);

  private:
    void timerCallback() override; // ~8 Hz - polls APVTS band params
    void syncBandsFromApvts();
    std::vector<EqBand> readBandsFromApvts() const;
    void persistOverride(int index, const EqBand& before, const EqBand& after);

    KhensuCopilotProcessor& processor_;
    EqCurveComponent eqCurve_;
    std::vector<EqBand> lastSyncedBands_;
    EqBand dragBaseline_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqCurveMiniPanel)
};

} // namespace khensu
