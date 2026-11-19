#include <khensu/mixerchain/PluginScanner.h>
#include <JuceHeader.h>
#include <khensu/common/LogRetention.h>
#include <exception>
#include "MainWindow.h"
#include "HeadlessCli.h"
#include "CrashReporter.h"
#include "UpdateInstaller.h"
#include "PythonServiceManager.h"
#include <khensu/common/Version.h>

namespace khensu {

namespace {

// Terminate diagnostics: an uncaught C++ exception on ANY thread fast-fails
// the whole process (ucrtbase abort, 0xC0000409) before a window even shows,
// with nothing in the UI and only an opaque WER entry to go on. This handler
// runs at that moment and writes the exception's type and message to
// %APPDATA%/KhensuMixing/logs/terminate-*.log so an intermittent startup
// crash is diagnosable from the field. It must stay async-signal-lean:
// resolve the exception, write one small file, then die as before.
[[noreturn]] void logUncaughtAndTerminate()
{
    juce::String detail = "std::terminate() called";
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            detail = "Uncaught std::exception: " + juce::String(typeid(e).name()) + " - " + juce::String(e.what());
        } catch (...) {
            detail = "Uncaught non-std exception";
        }
    }

    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("KhensuMixing")
                   .getChildFile("logs");
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");
    dir.getChildFile("terminate-" + stamp + ".log")
        .replaceWithText(detail + "\nThread: " +
                         juce::String(juce::Thread::getCurrentThread() != nullptr
                                          ? juce::Thread::getCurrentThread()->getThreadName()
                                          : "message thread or non-JUCE thread") +
                         "\nStack:\n" + juce::SystemStats::getStackBacktrace() + "\n");
    crash::writeTerminateReport(); // minidump next to the log's summary
    std::abort();
}

} // namespace

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class KhensuApplication final : public juce::JUCEApplication {
  public:
    const juce::String getApplicationName() override { return "Khensu Mixing"; }
    // The shared product version (DSK-19), not a hard-coded copy that drifts.
    const juce::String getApplicationVersion() override { return KHENSU_VERSION_STRING; }
    // A headless run must not be forwarded to (and swallowed by) an open window.
    bool moreThanOneInstanceAllowed() override
    {
        // The plugin-scan worker (CORE-07) runs beside the open app too.
        return headless::isHeadlessRequest(getCommandLineParameterArray()) || isScanWorker();
    }

    bool isScanWorker()
    {
        std::vector<std::string> args;
        for (const auto& a : getCommandLineParameterArray())
            args.push_back(a.toStdString());
        return mixerchain::PluginScanner::isScanWorkerRequest(args);
    }

    void initialise(const juce::String& /*commandLine*/) override
    {
        // CORE-07: the plugin-scan worker probes one plugin file and exits.
        // Before the crash handler: a plugin crashing here is the point of the
        // worker, not an app crash to report at the next launch.
        if (isScanWorker()) {
            std::vector<std::string> args;
            for (const auto& a : getCommandLineParameterArray())
                args.push_back(a.toStdString());
            setApplicationReturnValue(mixerchain::PluginScanner::runScanWorker(args));
            quit();
            return;
        }
        // Local-only minidumps in <config dir>/crashes; never uploaded.
        crash::install(crash::defaultCrashesDir(), getApplicationVersion(),
                       PythonServiceManager::kExpectedServiceVersion);
        std::set_terminate(logUncaughtAndTerminate);
        // SHIP-7: the handler above writes one file per crash; keep the newest.
        khensu::logs::pruneOldLogs(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                       .getChildFile("KhensuMixing")
                                       .getChildFile("logs"),
                                   "terminate-*.log", khensu::logs::kKeepCrashLogs);
        if (headless::isHeadlessRequest(getCommandLineParameterArray())) { // --analyze / --targets: no GUI
            setApplicationReturnValue(headless::run(getCommandLineParameterArray()));
            quit();
            return;
        }
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        // v1.0.0: a verified update the service downloaded installs now, as
        // the app closes ("Restart now" already launched it, with a relaunch).
        update_installer::launchPending(/*relaunch=*/false);
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override {}

  private:
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace khensu

START_JUCE_APPLICATION(khensu::KhensuApplication)
