// desktop-app/Source/diagnostics/AppDiagnostics.h
#pragma once
#include <JuceHeader.h>
#include "IDiagnosticsHost.h"

/// Whole-app checks that run inside the real app (ENG-1 step 8). Each takes
/// the window through IDiagnosticsHost. Message thread only.
namespace khensu::diagnostics {

/// `--a11y-audit=<file>`: visits every screen and writes controls lacking a
/// spoken name/tooltip to `report`; returns the gap count.
int writeAccessibilityAudit(IDiagnosticsHost& host, const juce::File& report);

/// `--ui-bench=<file>`: times every screen's first visit, layout and paint,
/// the Dashboard update pass and session-switch memory; writes JSON to
/// `report` and returns the over-budget count.
int writeUiBenchmark(IDiagnosticsHost& host, const juce::File& report);

/// `--screenshot=<dir>`: launch-state PNG + one per screen + component trees.
/// Returns the number of PNGs written.
int writeScreenshots(IDiagnosticsHost& host, const juce::File& dir);

/// `--screenshot-audio=<file>` (with --screenshot): analyse `audio` in a
/// throwaway session, capture the Dashboard while results are withheld and
/// once they are released, log the gate's states, delete the session, quit.
void runAnalysisScreenshots(IDiagnosticsHost& host, const juce::File& dir, const juce::File& audio);

/// Reads the launch flags above and schedules the matching run 3 s after
/// launch; the run sets the exit code and quits. False if none was given.
/// `--soak-audio=<file> [--soak-minutes=N] [--soak-log=<file>]`: repeated
/// analyse / fix / delete cycles in one process, logging memory, handles and
/// GUI objects per cycle; quits 1 on a failed cycle or growth over budget.
void runSoak(IDiagnosticsHost& host, const juce::File& audio, double minutes, const juce::File& log);

bool scheduleFromCommandLine(IDiagnosticsHost& host);

} // namespace khensu::diagnostics
