// desktop-app/Source/ui/Dashboard/StructuralFlowPanel.cpp
#include "StructuralFlowPanel.h"
#include "../../IAppController.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

StructuralFlowPanel::StructuralFlowPanel(IAppController& c) : PanelCard(c, "Structural Flow"), timeline_(&c)
{
    setExplainer(TRANS("Structure"),
                 TRANS("The song's sections over time and its arc quality -- how well tension builds and "
                 "releases across the arrangement. Higher arc quality means a more satisfying journey."),
                 "structural_flow_panel");

    arcMeter_.setRange(0.0f, 1.0f);
    arcMeter_.setTargetZone(0.5f, 1.0f);
    addAndMakeVisible(arcMeter_);

    timeline_.onSeekRequest([this](float time_sec) { controller_.playback().setPlaybackPosition(time_sec); });
    addAndMakeVisible(timeline_);
}

void StructuralFlowPanel::setData(const AnalysisResult& r)
{
    arcMeter_.setValue(r.python_data.structural_flow.arc_quality,
                       "Arc quality: " + juce::String(r.python_data.structural_flow.arc_quality, 2));

    if (auto* s = controller_.getActiveSession()) {
        if (!s->files.empty()) timeline_.setAudioFile(juce::File(s->files[0].path));
    }

    // Build section list from quick/advanced analysis JSON (has start_sec and label).
    std::vector<TimelineSection> sections;
    float span = 0.0f;
    auto parsed = juce::JSON::parse(juce::String(r.python_data.sections_json));
    if (auto* arr = parsed.getArray()) {
        for (auto& item : *arr) {
            float start = (float)(double)item.getProperty(af::SectionMarker::start_sec, 0.0);
            auto label = item.getProperty(af::SectionMarker::label, juce::var("")).toString();
            sections.push_back({start, label, 0.0f});
            span = juce::jmax(span, start);
        }
    }

    // Merge per-section LUFS from deep section analysis when available.
    // The sections_json does NOT include LUFS - that only comes from /analyze/sections.
    // Match by label + approximate start time (≤ 0.5s tolerance).
    if (r.section_analysis.has_value()) {
        for (auto& ts : sections) {
            for (const auto& sd : r.section_analysis->sections) {
                if (sd.lufs_integrated != 0.0f && juce::String(sd.label).equalsIgnoreCase(ts.label) &&
                    std::abs(sd.start_sec - ts.start_sec) < 0.5f) {
                    ts.lufs_integrated = sd.lufs_integrated;
                    ts.crest_db = sd.crest_db;
                    break;
                }
            }
        }
    }

    // Wire beat/downbeat markers to the timeline so the waveform shows beat ticks.
    {
        std::vector<float> beats, downbeats;
        auto beatsParsed = juce::JSON::parse(juce::String(r.python_data.beat_times_json));
        if (auto* ba = beatsParsed.getArray())
            for (auto& v : *ba)
                beats.push_back((float)(double)v);

        auto dbParsed = juce::JSON::parse(juce::String(r.python_data.downbeat_times_json));
        if (auto* da = dbParsed.getArray())
            for (auto& v : *da)
                downbeats.push_back((float)(double)v);

        timeline_.setBeats(std::move(beats), std::move(downbeats));
    }

    timeline_.setSpan(juce::jmax(span + 8.0f, 1.0f));
    timeline_.setSections(std::move(sections));
    timeline_.setRecommendations(r.recommendations);

    setAnalysisState(r.has_python_analysis() ? PanelAnalysisState::Analyzed
                     : r.is_partial          ? PanelAnalysisState::Analyzing
                                             : PanelAnalysisState::NotAnalyzed);

    repaint();
}

void StructuralFlowPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    arcMeter_.setBounds(b.removeFromTop(32));
    b.removeFromTop(8);
    timeline_.setBounds(b);
}

} // namespace khensu
