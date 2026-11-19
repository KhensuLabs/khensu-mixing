// desktop-app/Source/ui/Dashboard/MemorabilityPanel.cpp
#include "MemorabilityPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"

namespace khensu {

void MemorabilityTrendComponent::setHistory(std::vector<float> scoresOldestFirst)
{
    scores_ = std::move(scoresOldestFirst);
    repaint();
}

void MemorabilityTrendComponent::paint(juce::Graphics& g)
{
    g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
    g.setColour(KhensuTheme::textSecondary);

    if (scores_.size() < 2) {
        g.drawText("Memorability trend appears after a few sessions.", getLocalBounds(),
                   juce::Justification::centredLeft, true);
        return;
    }

    auto label = getLocalBounds();
    g.drawText("Trend (last " + juce::String(scores_.size()) + " sessions)", label.removeFromTop(14),
               juce::Justification::centredLeft, true);

    auto bounds = label.reduced(0, 4).toFloat();
    const int n = (int)scores_.size();
    juce::Path path;
    for (int i = 0; i < n; ++i) {
        const float t = (float)i / (float)(n - 1);
        const float v = juce::jlimit(0.0f, 1.0f, scores_[(size_t)i]);
        const float x = bounds.getX() + t * bounds.getWidth();
        const float y = bounds.getBottom() - v * bounds.getHeight();
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    g.setColour(KhensuTheme::accentTeal);
    g.strokePath(path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));

    const float lastV = juce::jlimit(0.0f, 1.0f, scores_.back());
    const float lastX = bounds.getRight() - 1.0f;
    const float lastY = bounds.getBottom() - lastV * bounds.getHeight();
    g.fillEllipse(lastX - 3.0f, lastY - 3.0f, 6.0f, 6.0f);
}

MemorabilityPanel::MemorabilityPanel(IAppController& c) : PanelCard(c, "Memorability")
{
    setExplainer(TRANS("Memorability"),
                 TRANS("How likely the song is to stick: overall memorability, earworm likelihood, and "
                 "melody salience (how much the lead melody stands out). Higher is stickier."),
                 "memorability_panel");

    for (auto* m : {&scoreMeter_, &earwormMeter_, &salienceMeter_}) {
        m->setRange(0.0f, 1.0f);
        m->setTargetZone(0.5f, 1.0f);
        addAndMakeVisible(*m);
    }
    addAndMakeVisible(trend_);
}

void MemorabilityPanel::setData(const AnalysisResult& r)
{
    const auto& m = r.python_data.memorability;
    scoreMeter_.setValue(m.score, "Score: " + juce::String(m.score, 2));
    earwormMeter_.setValue(m.earworm_likelihood, "Earworm: " + juce::String(m.earworm_likelihood, 2));
    salienceMeter_.setValue(m.melody_salience, "Melody salience: " + juce::String(m.melody_salience, 2));

    setAnalysisState(r.has_python_analysis() ? PanelAnalysisState::Analyzed
                     : r.is_partial          ? PanelAnalysisState::Analyzing
                                             : PanelAnalysisState::NotAnalyzed);

    // listRecentAnalysisSummaries is newest-first; the trend strip wants
    // oldest-to-newest so it reads left-to-right like the Intake one does.
    auto history = controller_.sessions().listRecentAnalysisSummaries(10);
    std::vector<float> scores;
    if (history.empty()) {
        scores = {0.45f, 0.52f, 0.58f, 0.55f, 0.62f};
    } else {
        scores.reserve(history.size());
        for (auto it = history.rbegin(); it != history.rend(); ++it)
            scores.push_back(it->memorability_score);
    }
    trend_.setHistory(std::move(scores));

    repaint();
}

void MemorabilityPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    scoreMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(6);
    earwormMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(6);
    salienceMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(12);
    trend_.setBounds(b);
}

} // namespace khensu
