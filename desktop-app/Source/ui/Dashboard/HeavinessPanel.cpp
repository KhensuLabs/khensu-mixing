// desktop-app/Source/ui/Dashboard/HeavinessPanel.cpp
#include "HeavinessPanel.h"
#include <khensu/session/Session.h>
#include "../shared/RecommendationExplainer.h"
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace {
const juce::String kHeavinessExplainerBase =
    "Four facets of perceptual heaviness: distortion, weight (low-end power), clarity, "
    "and extremity. The grey outline (when present) is the target genre's norm.";
}

HeavinessPanel::HeavinessPanel(IAppController& c) : PanelCard(c, "Heaviness")
{
    setExplainer(TRANS("Heaviness"), kHeavinessExplainerBase, "heaviness_panel");
    addAndMakeVisible(radar_);
}

void HeavinessPanel::setData(const AnalysisResult& r)
{
    const auto& h = r.python_data.heaviness;
    std::vector<RadarAxis> axes = {
        {"Distort", juce::jlimit(0.0f, 1.0f, h.distortion)},
        {"Weight", juce::jlimit(0.0f, 1.0f, h.weight)},
        {"Clarity", juce::jlimit(0.0f, 1.0f, h.clarity)},
        {"Extreme", juce::jlimit(0.0f, 1.0f, h.extremity)},
    };
    radar_.setAxes(std::move(axes));

    if (auto* session = controller_.getActiveSession()) {
        if (auto g = controller_.genreDatabase().findById(session->target.primary_genre)) {
            radar_.setReference({g->heaviness_distortion_norm, g->heaviness_weight_norm, g->heaviness_clarity_norm,
                                 g->heaviness_extremity_norm});
        }
    }

    const auto* rec = explainer::findTopRecommendation(
        r.recommendations, {"heaviness_extremity_mismatch", "heaviness_weight_low", "heaviness_distortion_mismatch"});
    setExplainer(TRANS("Heaviness"), explainer::appendReasoningTrace(kHeavinessExplainerBase, rec), "heaviness_panel");

    setAnalysisState(r.has_python_analysis() ? PanelAnalysisState::Analyzed
                     : r.is_partial          ? PanelAnalysisState::Analyzing
                                             : PanelAnalysisState::NotAnalyzed);

    repaint();
}

void HeavinessPanel::resized()
{
    PanelCard::resized();
    radar_.setBounds(bodyArea());
}

} // namespace khensu
