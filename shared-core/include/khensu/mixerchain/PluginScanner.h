#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace khensu::mixerchain {

struct ScannedPlugin {
    std::string id;
    std::string name;
    std::string manufacturer;
    std::string category;
    std::string format;
    std::string file_path;
};

/// Scans the machine's VST3 (and, on macOS, AudioUnit) library.
///
/// With a worker executable (setWorkerExecutable; the desktop app sets its
/// own), plugins are probed in child processes, so one that crashes or hangs
/// takes only the worker down and is quarantined (CORE-07). Without one (the
/// plugin, tests), scanning loads every plugin binary IN PROCESS, so a plugin
/// that crashes while being probed takes the host down with it. A
/// dead-man's-pedal file makes that a one-time event: the file
/// being probed is recorded in the pedal before it is loaded and removed once
/// it loaded cleanly, so after a crash the culprit is still listed. Every
/// later scan QUARANTINES the listed files - they are never loaded again (and
/// are reported through onProgress) until clearQuarantine() is called, e.g.
/// from a "retry skipped plugins" action.
class PluginScanner {
  public:
    /// Uses the per-user pedal file (defaultDeadMansPedalFile()).
    PluginScanner();
    /// Uses `deadMansPedalPath` instead (tests, or a host with its own data dir).
    explicit PluginScanner(std::string deadMansPedalPath);
    ~PluginScanner();

    /// Starts scanning in the background. Calls onComplete when finished.
    /// Progress callback receives the name of the plugin currently being
    /// scanned, and one "Skipped ..." line per quarantined plugin.
    ///
    /// THREADING: both callbacks are invoked ON A SCAN THREAD, never the
    /// message thread; with worker processes, onProgress may be called from
    /// several scan threads at once. Marshal to the UI with juce::MessageManager::callAsync
    /// (and guard the target's lifetime, e.g. a SafePointer). onComplete is
    /// not called when the scan is cancelled.
    void startScan(std::function<void(const std::vector<ScannedPlugin>&)> onComplete,
                   std::function<void(const std::string&)> onProgress = nullptr);

    /// Probes only the quarantined files again (e.g. a "Retry skipped plugins"
    /// action): clears the quarantine, then scans those files one at a time,
    /// allowing kRetryTimeoutMs each so the user can answer a licence or
    /// sign-in window. One that crashes or hangs again goes back into the
    /// quarantine. onComplete gets only the plugins found in those files.
    /// Returns false (and starts nothing) when nothing is quarantined.
    bool startRetryQuarantined(std::function<void(const std::vector<ScannedPlugin>&)> onComplete,
                               std::function<void(const std::string&)> onProgress = nullptr);
    /// Cancels any running scan (blocks up to 2 s for the scan thread).
    void cancelScan();

    /// Returns true if a scan is currently in progress.
    bool isScanning() const;

    /// Plugin files currently quarantined because a previous scan crashed
    /// while loading them.
    std::vector<std::string> quarantinedPlugins() const;

    /// Forgets the quarantine so the next scan probes those plugins again.
    /// Returns false when the pedal file could not be removed.
    bool clearQuarantine() const;

    /// `<data dir>/plugin_scan_deadmans_pedal.txt`.
    static std::string defaultDeadMansPedalFile();

    // ---- Out-of-process scanning (CORE-07) ----
    /// Probe plugin files in child processes running `exe` (the app's own
    /// executable, in its scan-worker mode): a plugin that crashes or hangs
    /// while it is probed takes only the child down, and is quarantined like
    /// a pedal entry; a new worker carries on after it. Empty (the default):
    /// scan in this process (tests, the plugin). Call before startScan().
    void setWorkerExecutable(std::string exe) { workerExe_ = std::move(exe); }
    /// How long one plugin file may take in a worker before it counts as hung.
    static constexpr int kWorkerTimeoutMs = 60000;
    /// The same for a retry of quarantined files, long enough to answer a window.
    static constexpr int kRetryTimeoutMs = 300000;
    /// Workers run side by side, the files dealt out between them.
    static constexpr int kWorkerProcesses = 3;

    /// The worker's switches: `--scan-plugin-list=<txt, one path per line>
    /// --scan-plugin-format=<name> --scan-plugin-out=<dir>`. It writes
    /// `current.txt` (the line it is probing) before each file, `<line>.xml`
    /// after it, and `done` at the end.
    static constexpr const char* kWorkerListArg = "--scan-plugin-list=";
    static constexpr const char* kWorkerFormatArg = "--scan-plugin-format=";
    static constexpr const char* kWorkerOutArg = "--scan-plugin-out=";
    /// True when `args` (the process's command-line parameters) ask for the worker.
    static bool isScanWorkerRequest(const std::vector<std::string>& args);
    /// The worker: probes the listed files with the named format in THIS process,
    /// in order. Returns the process exit code (0 = all written).
    /// Run on the message thread, before any UI.
    static int runScanWorker(const std::vector<std::string>& args);

  private:
    class ScanThread;
    std::string deadMansPedalPath_;
    std::string workerExe_; ///< CORE-07: empty = scan in process
    std::unique_ptr<ScanThread> scanThread_;
};

} // namespace khensu::mixerchain
