// desktop-app/Source/ui/Dashboard/HookDashboard.cpp
#include "HookDashboard.h"
#include "../../IAppController.h"

namespace khensu {

HookDashboard::HookDashboard(IAppController& c) : PanelCard(c, "Hooks"), timeline_(&c)
{
    setExplainer(TRANS("Hooks"),
                 TRANS("Hook strength and density measure how prominent and frequent the song's memorable "
                 "moments are. The timeline marks detected hook instances (taller bars = stronger hooks), "
                 "beat grid (grey ticks), and downbeats (teal ticks) when deep analysis is complete."),
                 "hook_dashboard");
    strengthMeter_.setRange(0.0f, 1.0f);
    strengthMeter_.setTargetZone(0.5f, 1.0f);
    densityMeter_.setRange(0.0f, 1.0f);
    densityMeter_.setTargetZone(0.4f, 1.0f);
    addAndMakeVisible(strengthMeter_);
    addAndMakeVisible(densityMeter_);

    timeline_.onSeekRequest([this](float time_sec) { controller_.playback().setPlaybackPosition(time_sec); });
    addAndMakeVisible(timeline_);
}

void HookDashboard::setData(const AnalysisResult& r)
{
    const auto& h = r.python_data.hooks;
    strengthMeter_.setValue(h.hook_strength, "Strength: " + juce::String(h.hook_strength, 2));
    densityMeter_.setValue(h.hook_density, "Density: " + juce::String(h.hook_density, 2) + "  (" +
                                               juce::String(h.hook_count) + " hooks)");

    if (auto* s = controller_.getActiveSession()) {
        if (!s->files.empty()) {
            timeline_.setAudioFile(juce::File(s->files[0].path));
        }
    }

    float span = 0.0f;
    std::vector<TimelineMarker> markers;
    for (const auto& inst : h.instances) {
        markers.push_back({inst.start_sec, inst.end_sec, inst.strength});
        span = juce::jmax(span, inst.end_sec);
    }
    timeline_.setSpan(juce::jmax(span, 1.0f));
    timeline_.setMarkers(std::move(markers));

    // Beat / downbeat grid - only populated after deep analysis; empty vectors are safe
    auto parseFloats = [](const std::string& json) -> std::vector<float> {
        std::vector<float> out;
        if (json.empty()) return out;
        auto v = juce::JSON::parse(juce::String(json));
        if (auto* arr = v.getArray())
            for (auto& el : *arr)
                out.push_back((float)(double)el);
        return out;
    };
    timeline_.setBeats(parseFloats(r.python_data.beat_times_json), parseFloats(r.python_data.downbeat_times_json));

    timeline_.setRecommendations(r.recommendations);

    setAnalysisState(r.has_python_analysis() ? PanelAnalysisState::Analyzed
                     : r.is_partial          ? PanelAnalysisState::Analyzing
                                             : PanelAnalysisState::NotAnalyzed);

    repaint();
}

void HookDashboard::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    strengthMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(4);
    densityMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(8);
    timeline_.setBounds(b);
}

} // namespace khensu
