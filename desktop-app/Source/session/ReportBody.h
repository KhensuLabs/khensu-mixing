// desktop-app/Source/session/ReportBody.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

struct AnalysisResult;
struct Session;

namespace report {

/// The JSON body of a /report or /report/pdf request: the session, the
/// analysis' headline numbers, findings and the current theme's palette
/// (so the report matches the app). Empty without a session. A pure function
/// of its inputs (REF-8: it was RenderService::buildReportJsonBody).
juce::String buildRequestBody(const AnalysisResult& analysis, const Session* session, const juce::String& outputPath,
                              const juce::String& reportMode, const juce::var& followUpScans = {});

} // namespace report
} // namespace khensu
