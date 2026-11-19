#pragma once
#include <juce_core/juce_core.h>

namespace khensu::plugincrash {

/// CE-ENG-4: local crash records for the VST3 copilot.
///
/// A plugin shares its process with the host, so this is deliberately
/// narrower than the desktop app's handler (desktop-app/Source/CrashReporter):
///
/// * Windows only (guarded): an unhandled-exception filter is CHAINED in
///   front of whatever the host installed. It writes a minidump plus a short
///   text record to %LOCALAPPDATA%\KhensuMixing\crashes ONLY when the faulting
///   address lies inside this plugin's own module, and then always hands the
///   exception on to the previous filter, so the host's own crash reporting
///   is unchanged.
/// * The dump is MiniDumpNormal + thread / unloaded-module info: no heap
///   memory, so nothing of the host's (or other plugins') data is captured.
/// * Hosts that wrap plugin calls in their own structured exception handling
///   never let the exception reach an unhandled filter; then nothing is
///   written (the host's report is the record). A stack overflow may also
///   fail to write, since the dump is written on the faulting thread.
/// * Nothing is uploaded, ever. The desktop app lists these records with its
///   own on its next launch (same folder).
///
/// Reference-counted: the first processor instance installs, the last one
/// uninstalls (and only restores the previous filter if ours is still the
/// active one - a host that replaced it is left alone).
void acquire(const char* pluginVersion) noexcept;
void release() noexcept;

/// The crashes folder (exists or not). Shared with the desktop app.
juce::File crashesDir();

/// Writes a record now, without crashing (tests; the "is the plumbing
/// working" check). Returns the .dmp, or {} off Windows / on failure.
juce::File writeTestRecord(const juce::File& dir);

} // namespace khensu::plugincrash
