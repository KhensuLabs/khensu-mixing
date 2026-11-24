// desktop-app/Source/ui/ExportDelivery/VisualReport.h
// VIZ-N5: a shareable visual report - one self-contained HTML file (inline
// SVG, no scripts, no external files) with the key charts, for a client or a
// collaborator who does not have the app:
//   - the readiness verdict (and why it is capped, when it is);
//   - tonal balance against the genre's released tracks (the spread bands);
//   - the five measurements with their dot strips and percentiles;
//   - the readiness categories against released tracks;
//   - loudness over time with the sections and true-peak events;
//   - what the current version changed (measured), when one is current;
//   - the open findings, worst first, with what to do.
// Uses the light palette (it prints), the same data the app's charts draw.
// THREAD_CONTEXT(Any) - pure string building.
#pragma once
#include <JuceHeader.h>
#include <optional>
#include <vector>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/analysis/TrackTimeline.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/targets/GenreSpread.h>
#include "../shared/FixDifferenceView.h"
#include "../shared/HeroSpectrumModel.h"

namespace khensu::visual_report {

struct Finding {
    juce::String title, action;
    float severity = 0.0f;
    std::optional<std::pair<double, double>> moment;
};

struct Input {
    juce::String trackName, genreName, versionName;
    juce::String generatedAt;
    MixScorecard scorecard;
    std::map<std::string, GenreSpread::Percentiles> scorecardSpread;
    hero_spectrum::Model hero; ///< spectrum, spread, gauges (dots, captions)
    const TrackTimeline* timeline = nullptr;
    std::vector<SectionData> sections;
    std::optional<std::pair<float, float>> lufsWindow;
    const fix_difference::Result* difference = nullptr;
    std::vector<Finding> findings;
};

/// The whole page.
juce::String buildHtml(const Input& in);

// The pieces (tested on their own).
juce::String tonalBalanceSvg(const hero_spectrum::Model& m, int width, int height);
juce::String dotStripSvg(const hero_spectrum::Gauge& g, int width, int height);
juce::String loudnessSvg(const TrackTimeline& tl, const std::vector<SectionData>& sections,
                         std::optional<std::pair<float, float>> window, int width, int height);
juce::String differenceSvg(const fix_difference::Result& r, int width, int height);
juce::String escape(const juce::String& s);

} // namespace khensu::visual_report
