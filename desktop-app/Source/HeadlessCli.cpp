#include "HeadlessCli.h"
#include "HeadlessCliProcess.h"
#include "PythonServiceManager.h"
#include <cstdio>

namespace khensu::headless {

namespace {

constexpr const char* kAnalyzeFlag = "--analyze";
constexpr const char* kTargetsFlag = "--targets";
constexpr int kNoInterpreter = 127;

void reportError(const juce::String& message)
{
#if JUCE_WINDOWS
    writeConsoleError(message.toStdString());
#else
    std::fprintf(stderr, "%s\n", message.toRawUTF8());
#endif
}

} // namespace

bool isHeadlessRequest(const juce::StringArray& appArgs)
{
    return appArgs.contains(kAnalyzeFlag) || appArgs.contains(kTargetsFlag);
}

juce::StringArray toCliArguments(const juce::StringArray& appArgs)
{
    const bool analyze = appArgs.contains(kAnalyzeFlag);
    juce::StringArray out;
    out.add(analyze ? "analyze" : "targets");
    bool modeFlagSkipped = false;
    for (const auto& arg : appArgs) {
        // Drop the first mode flag only; a later one is forwarded so the CLI
        // reports it rather than this layer silently swallowing it.
        if (!modeFlagSkipped && arg == (analyze ? kAnalyzeFlag : kTargetsFlag)) {
            modeFlagSkipped = true;
            continue;
        }
        out.add(arg);
    }
    return out;
}

juce::String quoteWindowsArgument(const juce::String& arg)
{
    if (arg.isNotEmpty() && !arg.containsAnyOf(" \t\n\v\""))
        return arg;

    juce::String quoted = "\"";
    int backslashes = 0;
    for (auto p = arg.getCharPointer(); !p.isEmpty(); ++p) {
        const juce::juce_wchar c = *p;
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 2n+1 backslashes, then the literal quote.
            quoted += juce::String::repeatedString("\\", backslashes * 2 + 1) + "\"";
        } else {
            quoted += juce::String::repeatedString("\\", backslashes);
            quoted += juce::String::charToString(c);
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled so they stay literal.
    quoted += juce::String::repeatedString("\\", backslashes * 2) + "\"";
    return quoted;
}

juce::String buildCommandLine(const juce::String& pythonExe, const juce::StringArray& cliArgs)
{
    juce::StringArray parts{quoteWindowsArgument(pythonExe), "-m", "khensu_services.cli"};
    for (const auto& a : cliArgs) parts.add(quoteWindowsArgument(a));
    return parts.joinIntoString(" ");
}

int run(const juce::StringArray& appArgs)
{
    juce::String pythonExe;
    juce::File servicesDir;
    {
        // Only used for its interpreter / launcher lookup - never launched,
        // so its destructor sends nothing to a running service.
        PythonServiceManager locator;
        pythonExe = locator.resolvedPythonExe();
        const juce::File launcher = locator.resolvedLauncherScript();
        if (launcher.existsAsFile()) servicesDir = launcher.getParentDirectory();
    }
    if (pythonExe.isEmpty() || servicesDir == juce::File()) {
        reportError("Khensu Mixing: headless mode needs the analysis service: "
                    + juce::String(pythonExe.isEmpty() ? "no Python interpreter found"
                                                       : "local-services/launch_service.py not found")
                    + ". Run the app once normally to set it up.");
        return kNoInterpreter;
    }

    const juce::StringArray cliArgs = toCliArguments(appArgs);
#if JUCE_WINDOWS
    const juce::String cmd = buildCommandLine(pythonExe, cliArgs);
    const int code = runWithConsole(cmd.toWideCharPointer(), servicesDir.getFullPathName().toWideCharPointer());
    if (code < 0) {
        reportError("Khensu Mixing: could not start " + pythonExe);
        return kNoInterpreter;
    }
    return code;
#else
    // Unix-like: no console attach problem - stream the child's output through.
    const juce::String existing = juce::SystemStats::getEnvironmentVariable("PYTHONPATH", {});
    setenv("PYTHONPATH",
           (servicesDir.getFullPathName() + (existing.isNotEmpty() ? ":" + existing : juce::String())).toRawUTF8(),
           1);
    juce::StringArray argv{pythonExe, "-m", "khensu_services.cli"};
    argv.addArray(cliArgs);
    juce::ChildProcess child;
    if (!child.start(argv, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
        reportError("Khensu Mixing: could not start " + pythonExe);
        return kNoInterpreter;
    }
    char buffer[4096];
    for (;;) {
        const int n = child.readProcessOutput(buffer, sizeof(buffer));
        if (n <= 0) break;
        std::fwrite(buffer, 1, static_cast<size_t>(n), stdout);
    }
    std::fflush(stdout);
    child.waitForProcessToFinish(-1);
    return static_cast<int>(child.getExitCode());
#endif
}

} // namespace khensu::headless
