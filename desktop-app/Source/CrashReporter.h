// desktop-app/Source/CrashReporter.h
#pragma once
#include <JuceHeader.h>

/// Local-only crash capture: minidumps + a short text summary written to
/// <config dir>/crashes/. Nothing is ever uploaded - the next launch just
/// tells the user a report was saved and offers to open the folder.
///
/// Windows: an unhandled-exception filter, plus pure-call, invalid-parameter
/// and SIGABRT handlers, hand the crash to a pre-started writer thread that
/// calls MiniDumpWriteDump (dbghelp, loaded at install). Writing from another
/// thread means a stack overflow on the crashing thread can still be dumped.
/// Everything the crash path needs (paths, header text, the dbghelp entry
/// point) is prepared at install; the crash path itself allocates nothing.
/// macOS/Linux: compile-safe no-ops, except the report-folder bookkeeping.
namespace khensu::crash {

/// Reports kept on disk; older .dmp/.txt pairs are deleted at install.
inline constexpr int kMaxReports = 10;

/// %LOCALAPPDATA%/KhensuMixing/crashes on Windows, <user app data>/KhensuMixing/
/// crashes elsewhere (the plugin's dumps and the Python service's faulthandler
/// and crash logs go to the same folder).
juce::File defaultCrashesDir();

/// Installs the crash handlers. Call once, first thing in initialise().
void install(const juce::File& crashesDir, const juce::String& appVersion, const juce::String& serviceVersion);

/// Name of the visible screen, recorded in the next report. Message thread;
/// copies into a fixed buffer, so it's cheap enough to call on every switch.
void setCurrentScreen(const char* name) noexcept;

/// Writes a report now (a dump of the whole process, with the calling
/// thread's context if `exceptionPointers` is null). Used by the crash
/// handlers, std::terminate and tests. Returns the .dmp, or {} on failure or
/// on a non-Windows build.
juce::File writeReportNow(const juce::File& crashesDir, void* exceptionPointers, const char* reason);

/// The std::terminate path: writes a report into the installed directory
/// (once per process). No-op before install().
void writeTerminateReport() noexcept;

/// .dmp files in `dir`, newest first.
juce::Array<juce::File> listReports(const juce::File& dir);

/// Deletes all but the newest `keep` reports (each .dmp with its .txt).
void pruneReports(const juce::File& dir, int keep = kMaxReports);

/// What the next launch should say. `show` is true when the newest report
/// has not been acknowledged yet.
struct PendingNotice {
    bool show = false;
    juce::File newestReport;
};
PendingNotice pendingNotice(const juce::File& dir);

/// Marks every current report as seen (the notice's "Dismiss" / "Open folder").
void acknowledge(const juce::File& dir);

} // namespace khensu::crash
