// plugin-copilot/Source/PluginWorkerPool.cpp
#include "PluginWorkerPool.h"
#include "ModuleLifetime.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace khensu::copilot {

namespace {

/// The pool below is a deliberate process-lifetime allocation: the Debug CRT
/// marks it "ignore" so its exit-time dump reports only real leaks (the same
/// pattern as the bridge's scheduler, CORE-14).
class IntentionalLeakScope {
  public:
#if defined(_MSC_VER) && defined(_DEBUG)
    IntentionalLeakScope() : previous_(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG))
    {
        _CrtSetDbgFlag(previous_ & ~_CRTDBG_ALLOC_MEM_DF);
    }
    ~IntentionalLeakScope() { _CrtSetDbgFlag(previous_); }
    IntentionalLeakScope(const IntentionalLeakScope&) = delete;
    IntentionalLeakScope& operator=(const IntentionalLeakScope&) = delete;

  private:
    int previous_;
#else
    IntentionalLeakScope() {} // user-provided: an unused trivial local would warn (C4101, /WX)
#endif
};

struct Pool {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    int busy = 0;
    std::vector<std::thread> threads;

    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return !queue.empty(); });
                job = std::move(queue.front());
                queue.pop_front();
                ++busy;
            }
            try {
                job();
            } catch (...) {
                // A job's own handler logs; one must never take a worker down.
            }
            std::lock_guard<std::mutex> lock(mutex);
            --busy;
        }
    }
};

Pool* pool() noexcept
{
    static Pool* instance = []() -> Pool* {
        try {
            // Threads that live for the process run plugin code: the module
            // must outlive every host unload from here on.
            pinPluginModuleForProcessLifetime();
            IntentionalLeakScope leak;
            auto* p = new Pool();
            for (int i = 0; i < PluginWorkerPool::kWorkers; ++i)
                p->threads.emplace_back([p] { p->run(); });
            return p;
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

} // namespace

bool PluginWorkerPool::submit(std::function<void()> job) noexcept
{
    Pool* p = pool();
    if (p == nullptr || p->threads.empty()) return false;
    try {
        {
            std::lock_guard<std::mutex> lock(p->mutex);
            // The queue lives for the process (MSVC's deque keeps its blocks
            // until the container goes, which this one never does): its
            // storage is part of the deliberate leak, not a real one
            // (2026-11-18 debug-capture flagged it).
            IntentionalLeakScope leak;
            p->queue.push_back(std::move(job));
        }
        p->cv.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

int PluginWorkerPool::pendingForTesting() noexcept
{
    Pool* p = pool();
    if (p == nullptr) return 0;
    std::lock_guard<std::mutex> lock(p->mutex);
    return static_cast<int>(p->queue.size()) + p->busy;
}

} // namespace khensu::copilot
