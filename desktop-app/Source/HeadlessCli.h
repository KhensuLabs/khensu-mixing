#pragma once
#include <JuceHeader.h>

/// Headless command-line mode for the desktop executable:
///
///   "Khensu Mixing.exe" --analyze mix.wav [--report out.html] [--json -] ...
///   "Khensu Mixing.exe" --targets
///
/// No window is created. The app finds the same Python interpreter and
/// local-services directory the service launch uses (PythonServiceManager),
/// runs `python -m khensu_services.cli analyze ...` with every other argument
/// passed through unchanged, and exits with the CLI's exit code (0 ok, 1
/// --fail-on reached, 2 usage error, 3 a file could not be analysed; 127 when
/// no interpreter or service directory is found). Output goes to the console
/// that started the app.
namespace khensu::headless {

/// True when the command line asks for headless mode (`--analyze` or `--targets`).
bool isHeadlessRequest(const juce::StringArray& appArgs);

/// App arguments -> CLI arguments: `--analyze a.wav --report r.html` becomes
/// `analyze a.wav --report r.html`; `--targets [--json]` becomes `targets [--json]`.
/// Every argument other than the mode flag is forwarded verbatim, in order.
juce::StringArray toCliArguments(const juce::StringArray& appArgs);

/// Quotes one argument so CommandLineToArgvW / the MSVC runtime parse it back
/// unchanged (backslash-before-quote rules included).
juce::String quoteWindowsArgument(const juce::String& arg);

/// `<python> -m khensu_services.cli <cliArgs...>` as one Windows command line.
juce::String buildCommandLine(const juce::String& pythonExe, const juce::StringArray& cliArgs);

/// Runs the headless request and returns the process exit code.
int run(const juce::StringArray& appArgs);

} // namespace khensu::headless
