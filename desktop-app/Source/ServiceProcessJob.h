#pragma once

namespace khensu {

/// Ties the spawned Python service (and anything it execs/spawns) to the
/// desktop app's lifetime (AUDIT-040). On Windows this is a job object with
/// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the OS closes the handle when the app
/// exits - normally, by crash, or from Task Manager - and that kills every
/// process in the job, so an orphaned service can no longer squat on the port.
/// It also lets shutdown terminate the real server: launch_service.py
/// os.execv()s into the venv interpreter, which on Windows is a NEW process
/// that juce::ChildProcess::kill() never reaches.
///
/// Elsewhere every method is a no-op (POSIX children are reaped by the
/// existing kill path).
///
/// THREAD_CONTEXT(AnyThread) - callers serialise (PythonServiceManager's
/// process mutex).
class ServiceProcessJob {
  public:
    ServiceProcessJob();
    ~ServiceProcessJob(); ///< closing the handle kills whatever is still in the job

    ServiceProcessJob(const ServiceProcessJob&) = delete;
    ServiceProcessJob& operator=(const ServiceProcessJob&) = delete;

    /// Assigns every direct child process of this process whose image name
    /// starts with "py" (python.exe / pythonw.exe / py.exe) and that is not
    /// in the job yet. Call immediately after spawning the service, before it
    /// can exec. Returns how many processes were added.
    int adoptNewPythonChildren();

    /// Waits until no process in the job is alive (true) or `timeoutMs`
    /// elapses (false).
    bool waitUntilEmpty(int timeoutMs);

    /// Hard-kills every process in the job.
    void terminateAll();

  private:
    void* handle_ = nullptr; ///< HANDLE on Windows
};

} // namespace khensu
