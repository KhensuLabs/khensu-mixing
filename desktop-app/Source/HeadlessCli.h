#pragma once
#include <JuceHeader.h>

/// Headless command-line mode for the desktop executable:
///
///   "Khensu Mixing.exe" --analyze mix.wav [--report out.html] [--json -] ...
///   "Khensu Mixing.exe" --targets
///   "Khensu Mixing.exe" --fix=mix.wav --out=mix_fix.wav [--top] [--include-denoise] [--genre=pop]
///   "Khensu Mixing.exe" --master=mix.wav --out=mix_master.wav [--style=warm] [--strength=0.6]
///   "Khensu Mixing.exe" --help
///
/// No window is created. --analyze / --targets find the same Python
/// interpreter and local-services directory the service launch uses
/// (PythonServiceManager), run `python -m khensu_services.cli ...` with every
/// other argument passed through unchanged, and exit with the CLI's exit code
/// (0 ok, 1 --fail-on reached, 2 usage error, 3 a file could not be analysed;
/// 127 when no interpreter or service directory is found).
///
/// --fix runs without the service: the native analysis pass, the
/// recommendation engine, the desktop's "Fix all safe" selection
/// (session/FixAllSafe.h: validated fixes only; advice-only findings and the
/// denoise left out unless --include-denoise), the native chain renderer and
/// the fix check, printing JSON on stdout. --master runs the mastering agent
/// through the Python CLI (`khensu master`; the analysis runtime, not a
/// running service); without that runtime, or with --native, it makes a
/// native loudness master (gain to the target, -1 dBTP true-peak limiter).
/// Output goes to the console that started the app.
namespace khensu::headless {

enum class Mode { None, Analyze, Targets, Fix, Master, Help };

/// The headless mode the command line asks for (None: start the GUI).
Mode modeOf(const juce::StringArray& appArgs);

/// True when the command line asks for headless mode.
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

/// A --fix / --master request. Options take `--name=value` or `--name value`.
struct NativeRequest {
    Mode mode = Mode::None;
    juce::String input;   ///< --fix / --master
    juce::String output;  ///< --out (default <name>_fix.wav / <name>_master.wav beside the input)
    juce::String genre;   ///< --genre: genre_targets.json id ("" = no genre, as the app)
    juce::String style;   ///< --master --style: warm | balanced | open | loud ("" = not given)
    double strength = -1.0;     ///< --strength 0..1 (< 0 = not given)
    bool hasTargetLufs = false; ///< --target-lufs given
    double targetLufs = -14.0;  ///< the native master's loudness target
    int maxIterations = 0;      ///< --max-iterations (0 = the agent's default)
    bool top = false;            ///< --top: only the most severe validated fix
    bool includeDenoise = false; ///< --include-denoise
    bool forceNative = false;    ///< --native: the native loudness master even with the runtime
    juce::String failOn;         ///< --fail-on moderate | severe ("" = never)
    juce::String error;          ///< a usage problem (exit 2), empty when the request is usable
};

/// Parses a --fix / --master command line (mode None for anything else).
NativeRequest parseNativeRequest(const juce::StringArray& appArgs);

/// The Python CLI arguments for a --master request (`master <in> --out <out> ... --json -`).
juce::StringArray toMasterCliArguments(const NativeRequest& req);

/// The --help text (no docs needed to use the headless modes).
juce::String usageText();

/// Where the native fix reads its reference data; defaults are the bundled
/// resources (resources/genre_norms/genre_targets.json and
/// resources/calibration/finding_evidence.json). Tests point them at the
/// source tree.
struct NativeData {
    juce::File genreTargets;
    juce::File findingEvidence;
};

/// --fix without the service. Returns the exit code (0 ok, 1 --fail-on
/// reached, 2 usage error, 3 analysis or render failed) and the JSON report.
int runNativeFix(const NativeRequest& req, juce::String& json, const NativeData& data = {});

/// The native loudness master (gain to the target, -1 dBTP limiter, a second
/// pass when the limiter cost loudness). Same exit codes and JSON as runNativeFix.
int runNativeMaster(const NativeRequest& req, juce::String& json, const NativeData& data = {});

/// Runs the headless request and returns the process exit code.
int run(const juce::StringArray& appArgs);

} // namespace khensu::headless
