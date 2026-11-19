// desktop-app/Source/ui/shared/CounterfactualView.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <optional>
#include <vector>

namespace khensu {

class IAppController;

/// CE-TRU-2 "What would fix this?": the call-out opened from a finding card.
/// Asks /explain/counterfactual for the smallest single-parameter change that
/// clears the finding, rendered through the service's DSP chain and
/// re-measured from the written preview. Audition plays that preview at the
/// mix's loudness (no A/B or history change); Apply re-renders the
/// response's effect chain through AppController::applyEffectChain (the
/// guarded /copilot/apply_chain job: guardrail, quarantine, provenance) and
/// only that render joins the render tree / undo / A/B "B" side.
/// THREAD_CONTEXT(UIThread)
class CounterfactualView final : public juce::Component {
  public:
    CounterfactualView(IAppController& controller, const Recommendation& rec);

    /// Issue code + optional frequency / threshold the service needs for a
    /// recommendation (exposed for tests). Threshold comes from the session
    /// genre on the service side when not derivable here.
    struct Query {
        juce::String code;
        std::optional<double> freqHz;
        std::optional<double> threshold;
    };
    static Query queryFor(const Recommendation& rec);

    /// The CounterfactualResponse `effects` array as a renderable chain
    /// (exposed for tests).
    static std::vector<ipc::HttpServiceBridge::EffectSpec> chainFrom(const juce::var& effects);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void run();
    void apply();

    IAppController& controller_;
    Query query_;
    juce::String sourcePath_;
    juce::String sessionId_;
    juce::String previewPath_;
    std::vector<ipc::HttpServiceBridge::EffectSpec> chain_;

    juce::Label titleLabel_;
    juce::Label changeLabel_;
    juce::TextEditor detail_;
    juce::TextButton auditionButton_{TRANS("Audition")};
    juce::TextButton applyButton_{TRANS("Apply")};

    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CounterfactualView)
};

} // namespace khensu
