#pragma once
// Deliberately free of JuceHeader.h (see HeadlessCliProcess.cpp).
#include <string>

namespace khensu::headless {

/// Runs `commandLine` (UTF-16, already quoted) with stdout/stderr bound to
/// this process's standard handles - or, when the GUI-subsystem app was
/// started without any (a plain console launch), to the parent's console,
/// which it attaches to. `pythonPath` is prepended to PYTHONPATH for the
/// child. Waits and returns the child's exit code, or -1 when it could not
/// be started. Windows only; returns -1 elsewhere.
int runWithConsole(const std::wstring& commandLine, const std::wstring& pythonPath);

/// Writes a UTF-8 line to the console stderr used by runWithConsole().
void writeConsoleError(const std::string& utf8Line);

} // namespace khensu::headless
