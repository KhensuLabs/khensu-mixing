// desktop-app/Source/ui/ExportDelivery/EvidenceReportBody.h
// CE-TRU-3: the POST /report/evidence body (EvidenceReportRequest) built from
// an analysis result - every recommendation with its issue code and the
// measurement evidence it was decided on. Pure, so it is unit-tested.
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

struct EvidenceReportMeta {
    juce::String sessionName;
    juce::String fileName;
    juce::String genre;
    juce::String outputPath; ///< .html
    juce::String jsonPath;   ///< .json beside it; empty = HTML only
};

juce::var buildEvidenceReportBody(const AnalysisResult& result, const EvidenceReportMeta& meta);

} // namespace khensu
